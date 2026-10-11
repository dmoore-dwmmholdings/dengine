/* log.c: categories, levels, per-thread log buffers and the log thread (SPEC §5.2, DENG-150).
 *
 * Producers. Each thread that logs owns one byte ring (single producer). de_log_ checks the
 * level, formats into a stack buffer, then copies one record (header + message + NUL) into the
 * ring and publishes it with a release store of `head`. A ring that is full drops the message
 * and counts it; the producer never waits. Rings are claimed lazily on a thread's first log call
 * (allocated once, or taken over from a thread that exited) and found again through a
 * thread-local pointer, so later calls allocate nothing.
 *
 *     ring bytes:  [ rec | rec | rec | PAD ]      head/tail are byte counts that only grow;
 *                     ^tail          ^head        offset = count & mask. A record never wraps:
 *                                                 when it does not fit before the end, a PAD
 *                                                 record fills the rest and it starts at 0.
 *
 * Consumer. Whoever holds the drain lock (the `de-log` thread, or a thread in de_log_flush) is
 * the single consumer of every ring. A drain batch snapshots each ring's head, then merges the
 * rings by timestamp (each ring is already in time order, because one thread's timestamps never
 * go backwards), calls the sinks for each record and releases the space with a store of `tail`.
 * Drop counts are reported after the batch as one Warn line per thread.
 *
 * Thread exit. A de_tls_key destructor marks the thread's ring DEAD. The next drain empties it
 * and marks it FREE, and the next new thread takes it over.
 *
 * Generations. Every de_log_init starts a new generation with a new TLS key. A thread whose
 * thread-local generation is stale claims a new ring instead of touching the old pointer, so
 * rings freed by de_log_shutdown are never used again.
 */
#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "core/de_compiler.h"
#include "core/log_internal.h"
#include "platform/de_atomic.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DE__LOG_DEFAULT_RING_BYTES (64u * 1024u)
#define DE__LOG_MIN_RING_BYTES     (4u * 1024u)
#define DE__LOG_MAX_RING_BYTES     (1u << 30)
#define DE__LOG_DEFAULT_INTERVAL   5u
#define DE__NS_PER_MS              1000000ull

/* ---------- Ring records ---------- */

enum { DE__REC_MSG = 1, DE__REC_PAD = 2 };

/* The first 8 bytes of every record. A PAD record is only this. */
typedef struct de__log_prefix {
    u32 size; /* bytes of the whole record, a multiple of 8 */
    u32 kind;
} de__log_prefix;

typedef struct de__log_hdr {
    u32 size;
    u32 kind;
    u32 level;
    u32 category;
    u64 timestamp_ns;
    const char *file;
    u32 line;
    u32 msg_len; /* without the NUL that follows the message */
} de__log_hdr;

#define DE__ALIGN8(n) (((n) + 7u) & ~(u32)7u)
_Static_assert(sizeof(de__log_hdr) % 8 == 0, "records stay 8-byte aligned");
_Static_assert(sizeof(de__log_hdr) + DE_LOG_MESSAGE_MAX + 8 <= DE__LOG_MIN_RING_BYTES / 2,
               "the largest record must fit in half of the smallest ring");

enum { DE__RING_LIVE = 1, DE__RING_DEAD = 2, DE__RING_FREE = 3 };

typedef struct de__log_ring {
    /* Written by the producer. */
    de_atomic_u64 head;         /* bytes published */
    de_atomic_u64 dropped;      /* messages dropped since the last drain */
    de_atomic_u32 wake_pending; /* the producer posted the wake semaphore */
    u8 pad0[64];
    /* Written by the consumer. */
    de_atomic_u64 tail; /* bytes released */
    u64 cursor, end;    /* drain batch state, guarded by the drain lock */
    u32 drain_state;
    struct de__log_ring *drain_next;
    u8 pad1[64];
    /* Rarely written. */
    de_atomic_u32 state; /* DE__RING_LIVE / DEAD / FREE */
    de_atomic_u32 tid;
    struct de__log_ring *next; /* registry list; fixed once published */
    de_allocator alloc;        /* the allocator that made this ring */
    size_t alloc_size;
    u32 capacity, mask;
    u8 *buf;
} de__log_ring;

/* ---------- Global state ---------- */

/* Logging is a process-wide service (DE_LOG has no context argument), so its state is global.
 * Everything that needs an allocator takes the one from de_log_desc. */

typedef struct de__log_sink_slot {
    de_log_sink_fn fn;
    de__log_batch_end_fn batch_end;
    void *user;
} de__log_sink_slot;

typedef struct de__log_tl_state {
    de__log_ring *ring;
    u32 gen;
} de__log_tl_state;

static struct {
    /* The drain lock, created on first use and never destroyed. It guards draining, the sink
     * table and the init/shutdown transitions. */
    de_atomic_u32 lock_state; /* 0 none, 1 creating, 2 ready */
    de_mutex lock;
    de_atomic_u32 drain_owner; /* thread id holding the lock, 0 if none */

    de_atomic_u32 running; /* 1 between de_log_init and de_log_shutdown */
    de_atomic_u32 gen;     /* incremented by each de_log_init */
    de_atomic_u32 stop;    /* asks the log thread to exit */
    de_atomic_ptr rings;   /* de__log_ring list head */

    /* Set by de_log_init before `running` is published; read-only while running. */
    de_allocator alloc;
    u32 ring_bytes;
    u64 interval_ns;
    de_tls_key key;
    de_sem wake;
    de_thread *thread;

    de__log_sink_slot sinks[DE_LOG_MAX_SINKS];
    u32 sink_count;

    /* Categories. Slots below cat_count are published; names never change once published. */
    de_atomic_u32 cat_lock;
    de_atomic_u32 cat_count;
    de_atomic_u32 cat_level[DE_LOG_MAX_CATEGORIES];
    char cat_name[DE_LOG_MAX_CATEGORIES][DE_LOG_CATEGORY_NAME_MAX];

    de_atomic_u64 n_dropped, n_batches, n_rings, n_recycled;
} g = {
    .cat_count = DE_ATOMIC_INIT(3),
    .cat_level = {[0] = DE_ATOMIC_INIT(DE_LOG_INFO),
                  [1] = DE_ATOMIC_INIT(DE_LOG_INFO),
                  [2] = DE_ATOMIC_INIT(DE_LOG_INFO)},
    .cat_name = {"general", "core", "platform"},
};

static DE_THREAD_LOCAL de__log_tl_state de__log_tl;
/* Per thread, so counting costs no shared cache line on the hot path. */
static DE_THREAD_LOCAL u64 de__log_tl_formatted;

/* ---------- Default allocator ---------- */

/* NULL in de_log_desc, and the file sink before de_log_init: the engine heap. */
static de_allocator de__log_default_allocator(void) {
    return *de_heap_allocator();
}

/* ---------- Locks ---------- */

static de_mutex *de__log_mutex(void) {
    if (DE_UNLIKELY(de_atomic_load_u32(&g.lock_state, DE_ATOMIC_ACQUIRE) != 2)) {
        u32 expected = 0;
        if (de_atomic_cas_u32(&g.lock_state, &expected, 1, DE_ATOMIC_ACQ_REL, DE_ATOMIC_ACQUIRE)) {
            de_mutex_init(&g.lock);
            de_atomic_store_u32(&g.lock_state, 2, DE_ATOMIC_RELEASE);
        } else {
            while (de_atomic_load_u32(&g.lock_state, DE_ATOMIC_ACQUIRE) != 2)
                de_cpu_pause();
        }
    }
    return &g.lock;
}

static void de__log_lock(void) {
    de_mutex_lock(de__log_mutex());
    de_atomic_store_u32(&g.drain_owner, de_thread_current_id(), DE_ATOMIC_RELAXED);
}

static bool de__log_try_lock(void) {
    if (!de_mutex_try_lock(de__log_mutex()))
        return false;
    de_atomic_store_u32(&g.drain_owner, de_thread_current_id(), DE_ATOMIC_RELAXED);
    return true;
}

static void de__log_unlock(void) {
    de_atomic_store_u32(&g.drain_owner, 0, DE_ATOMIC_RELAXED);
    de_mutex_unlock(&g.lock);
}

/* True when the calling thread holds the drain lock (it is inside a sink). Only this thread can
 * have stored its own id, so a relaxed load is enough. */
static bool de__log_in_drain(void) {
    return de_atomic_load_u32(&g.drain_owner, DE_ATOMIC_RELAXED) == de_thread_current_id();
}

static void de__cat_lock(void) {
    for (;;) {
        u32 expected = 0;
        if (de_atomic_cas_weak_u32(&g.cat_lock, &expected, 1, DE_ATOMIC_ACQUIRE, DE_ATOMIC_RELAXED))
            return;
        de_cpu_pause();
    }
}

static void de__cat_unlock(void) {
    de_atomic_store_u32(&g.cat_lock, 0, DE_ATOMIC_RELEASE);
}

/* ---------- Categories and levels ---------- */

static u32 de__cat_index(de_log_category cat) {
    return cat.id < de_atomic_load_u32(&g.cat_count, DE_ATOMIC_ACQUIRE) ? cat.id : 0;
}

bool de_log_enabled(de_log_level lvl, de_log_category cat) {
    u32 min = de_atomic_load_u32(&g.cat_level[de__cat_index(cat)], DE_ATOMIC_RELAXED);
    return (u32)lvl >= min;
}

de_log_level de_log_get_level(de_log_category cat) {
    return (de_log_level)de_atomic_load_u32(&g.cat_level[de__cat_index(cat)], DE_ATOMIC_RELAXED);
}

void de_log_set_level(de_log_category cat, de_log_level level) {
    if (cat.id >= de_atomic_load_u32(&g.cat_count, DE_ATOMIC_ACQUIRE))
        return;
    if ((u32)level > (u32)DE_LOG_FATAL)
        level = DE_LOG_FATAL;
    de_atomic_store_u32(&g.cat_level[cat.id], (u32)level, DE_ATOMIC_RELAXED);
}

const char *de_log_category_name(de_log_category cat) {
    return g.cat_name[de__cat_index(cat)];
}

const char *de_log_level_name(de_log_level lvl) {
    static const char *const names[] = {"trace", "debug", "info", "warn", "error", "fatal"};
    return (u32)lvl < sizeof names / sizeof names[0] ? names[lvl] : "?";
}

de_log_category de_log_register_category(const char *name, de_log_level default_level) {
    if (name == NULL || name[0] == '\0')
        return DE_LOG_CAT_GENERAL;
    if ((u32)default_level > (u32)DE_LOG_FATAL)
        default_level = DE_LOG_FATAL;

    /* Compare on the cut name, so a long name registered twice still finds itself. */
    char cut[DE_LOG_CATEGORY_NAME_MAX];
    size_t len = strlen(name);
    if (len > DE_LOG_CATEGORY_NAME_MAX - 1)
        len = DE_LOG_CATEGORY_NAME_MAX - 1;
    memcpy(cut, name, len);
    cut[len] = '\0';

    de__cat_lock();
    u32 count = de_atomic_load_u32(&g.cat_count, DE_ATOMIC_RELAXED);
    for (u32 i = 0; i < count; i++) {
        if (strcmp(g.cat_name[i], cut) == 0) {
            de__cat_unlock();
            return (de_log_category){i};
        }
    }
    if (count >= DE_LOG_MAX_CATEGORIES) {
        de__cat_unlock();
        fprintf(stderr, "[warn][core] log: category table full (%d); '%s' logs as 'general'\n",
                DE_LOG_MAX_CATEGORIES, cut);
        return DE_LOG_CAT_GENERAL;
    }
    memcpy(g.cat_name[count], cut, len + 1);
    de_atomic_store_u32(&g.cat_level[count], (u32)default_level, DE_ATOMIC_RELAXED);
    de_atomic_store_u32(&g.cat_count, count + 1, DE_ATOMIC_RELEASE);
    de__cat_unlock();
    return (de_log_category){count};
}

/* ---------- Sinks ---------- */

static bool de__sink_add_locked(de_log_sink_fn fn, de__log_batch_end_fn batch_end, void *user) {
    for (u32 i = 0; i < g.sink_count; i++)
        if (g.sinks[i].fn == fn && g.sinks[i].user == user)
            return false;
    if (g.sink_count >= DE_LOG_MAX_SINKS)
        return false;
    g.sinks[g.sink_count++] = (de__log_sink_slot){fn, batch_end, user};
    return true;
}

bool de__log_add_sink_ex(de_log_sink_fn fn, de__log_batch_end_fn batch_end, void *user) {
    if (fn == NULL || de__log_in_drain())
        return false;
    de__log_lock();
    bool ok = de__sink_add_locked(fn, batch_end, user);
    de__log_unlock();
    return ok;
}

void de_log_add_sink(de_log_sink_fn fn, void *user) {
    (void)de__log_add_sink_ex(fn, NULL, user);
}

void de_log_remove_sink(de_log_sink_fn fn, void *user) {
    if (fn == NULL || de__log_in_drain())
        return;
    de__log_lock();
    for (u32 i = 0; i < g.sink_count; i++) {
        if (g.sinks[i].fn == fn && g.sinks[i].user == user) {
            memmove(&g.sinks[i], &g.sinks[i + 1], (g.sink_count - i - 1) * sizeof g.sinks[0]);
            g.sink_count--;
            break;
        }
    }
    de__log_unlock();
}

static void de__emit(const de_log_record *rec) {
    for (u32 i = 0; i < g.sink_count; i++)
        g.sinks[i].fn(rec, g.sinks[i].user);
}

/* ---------- Rings: producer side ---------- */

static void de__ring_dtor(void *value) {
    de__log_ring *r = (de__log_ring *)value;
    de_atomic_store_u32(&r->state, DE__RING_DEAD, DE_ATOMIC_RELEASE);
    /* A destructor that runs later on this thread may still log: it then claims a new ring. */
    de__log_tl.ring = NULL;
    de__log_tl.gen = 0;
}

/* Slow path of a thread's first log call in this generation. Called only while running. */
static de__log_ring *de__ring_claim(u32 gen) {
    de__log_ring *r = NULL;
    for (de__log_ring *it = (de__log_ring *)de_atomic_load_ptr(&g.rings, DE_ATOMIC_ACQUIRE);
         it != NULL; it = it->next) {
        u32 expected = DE__RING_FREE;
        if (de_atomic_cas_u32(&it->state, &expected, DE__RING_LIVE, DE_ATOMIC_ACQ_REL,
                              DE_ATOMIC_RELAXED)) {
            r = it;
            de_atomic_fetch_add_u64(&g.n_recycled, 1, DE_ATOMIC_RELAXED);
            break;
        }
    }
    if (r == NULL) {
        size_t size = sizeof(de__log_ring) + g.ring_bytes;
        r = (de__log_ring *)g.alloc.alloc(g.alloc.user, size, DE_ALIGNOF(de__log_ring),
                                          DE_MEM_GENERAL);
        if (r == NULL)
            return NULL;
        memset(r, 0, sizeof *r);
        de_atomic_init_u64(&r->head, 0);
        de_atomic_init_u64(&r->dropped, 0);
        de_atomic_init_u32(&r->wake_pending, 0);
        de_atomic_init_u64(&r->tail, 0);
        de_atomic_init_u32(&r->state, DE__RING_LIVE);
        de_atomic_init_u32(&r->tid, 0);
        r->alloc = g.alloc;
        r->alloc_size = size;
        r->capacity = g.ring_bytes;
        r->mask = g.ring_bytes - 1;
        r->buf = (u8 *)(r + 1);
        void *head = de_atomic_load_ptr(&g.rings, DE_ATOMIC_RELAXED);
        do {
            r->next = (de__log_ring *)head;
        } while (!de_atomic_cas_weak_ptr(&g.rings, &head, r, DE_ATOMIC_RELEASE, DE_ATOMIC_RELAXED));
        de_atomic_fetch_add_u64(&g.n_rings, 1, DE_ATOMIC_RELAXED);
    }
    de_atomic_store_u32(&r->tid, de_thread_current_id(), DE_ATOMIC_RELAXED);
    /* Without the TLS value the ring is never recycled after this thread exits; it still works
     * and is freed at shutdown. */
    (void)de_tls_set(g.key, r);
    de__log_tl.ring = r;
    de__log_tl.gen = gen;
    return r;
}

static bool de__ring_push(de__log_ring *r, de_log_level lvl, de_log_category cat, const char *file,
                          u32 line, u64 ts, const char *msg, u32 len) {
    u32 need = DE__ALIGN8((u32)sizeof(de__log_hdr) + len + 1u);
    u64 head = de_atomic_load_u64(&r->head, DE_ATOMIC_RELAXED);
    u64 tail = de_atomic_load_u64(&r->tail, DE_ATOMIC_ACQUIRE);
    u32 off = (u32)(head & r->mask);
    u32 contig = r->capacity - off;
    u64 total = need + (contig < need ? contig : 0u);
    if ((u64)r->capacity - (head - tail) < total) {
        de_atomic_fetch_add_u64(&r->dropped, 1, DE_ATOMIC_RELAXED);
        de_atomic_fetch_add_u64(&g.n_dropped, 1, DE_ATOMIC_RELAXED);
        return false;
    }
    if (contig < need) {
        /* contig is a non-zero multiple of 8, so the PAD prefix fits. */
        de__log_prefix pad = {contig, DE__REC_PAD};
        memcpy(r->buf + off, &pad, sizeof pad);
        off = 0;
    }
    de__log_hdr h = {need, DE__REC_MSG, (u32)lvl, cat.id, ts, file, line, len};
    memcpy(r->buf + off, &h, sizeof h);
    memcpy(r->buf + off + sizeof h, msg, len);
    r->buf[off + sizeof h + len] = '\0';
    u64 new_head = head + total;
    de_atomic_store_u64(&r->head, new_head, DE_ATOMIC_RELEASE);

    /* Wake the log thread early once the ring is half full; otherwise it drains on its timer. */
    if (new_head - tail > r->capacity / 2 &&
        de_atomic_exchange_u32(&r->wake_pending, 1, DE_ATOMIC_RELAXED) == 0)
        de_sem_post(&g.wake, 1);
    return true;
}

/* ---------- Rings: consumer side (drain lock held) ---------- */

static const de__log_hdr *de__ring_peek(de__log_ring *r) {
    while (r->cursor < r->end) {
        const u8 *p = r->buf + (r->cursor & r->mask);
        de__log_prefix pre;
        memcpy(&pre, p, sizeof pre);
        if (pre.kind == DE__REC_PAD) {
            r->cursor += pre.size;
            continue;
        }
        return (const de__log_hdr *)(const void *)p;
    }
    return NULL;
}

static void de__drain_locked(void) {
    de_atomic_fetch_add_u64(&g.n_batches, 1, DE_ATOMIC_RELAXED);

    /* Snapshot: everything published before this point belongs to the batch. */
    de__log_ring *active = NULL;
    for (de__log_ring *r = (de__log_ring *)de_atomic_load_ptr(&g.rings, DE_ATOMIC_ACQUIRE);
         r != NULL; r = r->next) {
        r->drain_state = de_atomic_load_u32(&r->state, DE_ATOMIC_ACQUIRE);
        if (r->drain_state == DE__RING_FREE)
            continue;
        de_atomic_store_u32(&r->wake_pending, 0, DE_ATOMIC_RELAXED);
        r->cursor = de_atomic_load_u64(&r->tail, DE_ATOMIC_RELAXED);
        r->end = de_atomic_load_u64(&r->head, DE_ATOMIC_ACQUIRE);
        if (r->cursor != r->end) {
            r->drain_next = active;
            active = r;
        }
    }

    /* Merge by timestamp. Linear in the number of active rings per record, which is small. */
    for (;;) {
        de__log_ring *best = NULL;
        const de__log_hdr *best_h = NULL;
        for (de__log_ring *r = active; r != NULL; r = r->drain_next) {
            const de__log_hdr *h = de__ring_peek(r);
            if (h != NULL && (best_h == NULL || h->timestamp_ns < best_h->timestamp_ns)) {
                best = r;
                best_h = h;
            }
        }
        if (best == NULL)
            break;
        de_log_record rec = {
            .level = (de_log_level)best_h->level,
            .category = {best_h->category},
            .timestamp_ns = best_h->timestamp_ns,
            .thread_id = de_atomic_load_u32(&best->tid, DE_ATOMIC_RELAXED),
            .file = best_h->file,
            .line = best_h->line,
            .message = {(const char *)(best_h + 1), best_h->msg_len},
        };
        de__emit(&rec);
        best->cursor += best_h->size;
        de_atomic_store_u64(&best->tail, best->cursor, DE_ATOMIC_RELEASE);
    }
    /* Release trailing PAD records that the merge skipped. */
    for (de__log_ring *r = active; r != NULL; r = r->drain_next)
        de_atomic_store_u64(&r->tail, r->cursor, DE_ATOMIC_RELEASE);

    /* Drop reports and recycling. The reports come last, so their timestamps (now) keep the
     * batch in order. */
    for (de__log_ring *r = (de__log_ring *)de_atomic_load_ptr(&g.rings, DE_ATOMIC_ACQUIRE);
         r != NULL; r = r->next) {
        if (r->drain_state == DE__RING_FREE)
            continue;
        u64 dropped = de_atomic_exchange_u64(&r->dropped, 0, DE_ATOMIC_RELAXED);
        if (dropped != 0) {
            char msg[128];
            int n = snprintf(msg, sizeof msg,
                             "log: dropped %llu messages from thread %u (thread buffer full)",
                             (unsigned long long)dropped,
                             (unsigned)de_atomic_load_u32(&r->tid, DE_ATOMIC_RELAXED));
            de_log_record rec = {
                .level = DE_LOG_WARN,
                .category = DE_LOG_CAT_CORE,
                .timestamp_ns = de_time_now_ns(),
                .thread_id = de_atomic_load_u32(&r->tid, DE_ATOMIC_RELAXED),
                .file = __FILE__,
                .line = __LINE__,
                .message = {msg, n > 0 ? (size_t)n : 0},
            };
            de__emit(&rec);
        }
        /* DEAD was stored after the thread's last publish, and seen before the head snapshot,
         * so the ring is empty for good once the batch has drained it. */
        if (r->drain_state == DE__RING_DEAD && r->cursor == r->end) {
            de_atomic_store_u64(&r->dropped, 0, DE_ATOMIC_RELAXED);
            de_atomic_store_u32(&r->state, DE__RING_FREE, DE_ATOMIC_RELEASE);
        }
    }

    for (u32 i = 0; i < g.sink_count; i++)
        if (g.sinks[i].batch_end != NULL)
            g.sinks[i].batch_end(g.sinks[i].user);
}

/* ---------- Log thread ---------- */

static i32 de__log_thread_main(void *user) {
    (void)user;
    while (!de_atomic_load_u32(&g.stop, DE_ATOMIC_ACQUIRE)) {
        (void)de_sem_wait_timeout(&g.wake, g.interval_ns);
        de__log_lock();
        de__drain_locked();
        de__log_unlock();
    }
    return 0;
}

/* ---------- Init, shutdown, flush ---------- */

static u32 de__round_ring_bytes(u32 bytes) {
    if (bytes == 0)
        bytes = DE__LOG_DEFAULT_RING_BYTES;
    if (bytes < DE__LOG_MIN_RING_BYTES)
        bytes = DE__LOG_MIN_RING_BYTES;
    if (bytes > DE__LOG_MAX_RING_BYTES)
        bytes = DE__LOG_MAX_RING_BYTES;
    u32 p = DE__LOG_MIN_RING_BYTES;
    while (p < bytes)
        p <<= 1;
    return p;
}

de_result de_log_init(const de_log_desc *desc) {
    de_log_desc d = desc != NULL ? *desc : (de_log_desc){0};
    if (d.allocator != NULL && (d.allocator->alloc == NULL || d.allocator->free == NULL))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_log_init: allocator without alloc/free");

    if (de__log_in_drain())
        return DE_FAIL(DE_ERR_BUSY, "de_log_init: called from a log sink");
    de__log_lock();
    if (de_atomic_load_u32(&g.running, DE_ATOMIC_RELAXED)) {
        de__log_unlock();
        return DE_FAIL(DE_ERR_BUSY, "de_log_init: logging is already initialized");
    }
    g.alloc = d.allocator != NULL ? *d.allocator : de__log_default_allocator();
    g.ring_bytes = de__round_ring_bytes(d.thread_buffer_bytes);
    g.interval_ns =
        (u64)(d.drain_interval_ms != 0 ? d.drain_interval_ms : DE__LOG_DEFAULT_INTERVAL) *
        DE__NS_PER_MS;

    de_result r = de_sem_init(&g.wake, 0);
    if (r != DE_OK) {
        de__log_unlock();
        return DE_FAIL_WRAP(r, "de_log_init: wake semaphore");
    }
    r = de_tls_key_create(&g.key, de__ring_dtor);
    if (r != DE_OK) {
        de_sem_destroy(&g.wake);
        de__log_unlock();
        return DE_FAIL_WRAP(r, "de_log_init: TLS key");
    }
    bool added_console = !d.no_console_sink && de__sink_add_locked(de_log_console_sink, NULL, NULL);

    de_atomic_fetch_add_u32(&g.gen, 1, DE_ATOMIC_RELAXED);
    de_atomic_store_u32(&g.stop, 0, DE_ATOMIC_RELAXED);
    de_atomic_store_u32(&g.running, 1, DE_ATOMIC_RELEASE);

    r = de_thread_create(de__log_thread_main, NULL, "de-log", DE_THREAD_NORMAL, 0, &g.thread);
    if (r != DE_OK) {
        de_atomic_store_u32(&g.running, 0, DE_ATOMIC_SEQ_CST);
        if (added_console)
            g.sink_count--; /* it was appended last */
        de_tls_key_destroy(g.key);
        de_sem_destroy(&g.wake);
        de__log_unlock();
        return DE_FAIL_WRAP(r, "de_log_init: log thread");
    }
    de__log_unlock();
    return DE_OK;
}

void de_log_shutdown(void) {
    if (de__log_in_drain())
        return;
    de__log_lock();
    if (!de_atomic_load_u32(&g.running, DE_ATOMIC_RELAXED)) {
        de__log_unlock();
        return;
    }
    de__log_unlock();

    de_atomic_store_u32(&g.stop, 1, DE_ATOMIC_RELEASE);
    de_sem_post(&g.wake, 1);
    (void)de_thread_join(g.thread);
    g.thread = NULL;

    de__log_lock();
    /* From here on de_log_ writes to stderr; drain what is already buffered. */
    de_atomic_store_u32(&g.running, 0, DE_ATOMIC_SEQ_CST);
    de__drain_locked();
    /* On Windows, FlsFree runs the destructors of values still set; they only mark rings. */
    de_tls_key_destroy(g.key);
    de__log_ring *r = (de__log_ring *)de_atomic_exchange_ptr(&g.rings, NULL, DE_ATOMIC_ACQ_REL);
    while (r != NULL) {
        de__log_ring *next = r->next;
        de_allocator a = r->alloc;
        a.free(a.user, r, r->alloc_size);
        r = next;
    }
    g.sink_count = 0;
    de_sem_destroy(&g.wake);
    de__log_unlock();
}

void de_log_flush(void) {
    if (!de_atomic_load_u32(&g.running, DE_ATOMIC_ACQUIRE) || de__log_in_drain())
        return;
    de__log_lock();
    if (de_atomic_load_u32(&g.running, DE_ATOMIC_RELAXED))
        de__drain_locked();
    de__log_unlock();
}

void de__log_flush_for_panic(void) {
    if (!de_atomic_load_u32(&g.running, DE_ATOMIC_ACQUIRE) || de__log_in_drain())
        return;
    /* The lock holder may be stuck in a sink; give up after about 100 ms rather than hang the
     * panic. A panic is rare enough that spinning does not matter. */
    u64 deadline = de_time_now_ns() + 100ull * DE__NS_PER_MS;
    while (!de__log_try_lock()) {
        if (de_time_now_ns() > deadline)
            return;
        de_cpu_pause();
    }
    if (de_atomic_load_u32(&g.running, DE_ATOMIC_RELAXED))
        de__drain_locked();
    de__log_unlock();
}

void de__log_allocator(de_allocator *out) {
    bool locked = !de__log_in_drain(); /* a sink already holds the lock */
    if (locked)
        de__log_lock();
    *out =
        de_atomic_load_u32(&g.running, DE_ATOMIC_RELAXED) ? g.alloc : de__log_default_allocator();
    if (locked)
        de__log_unlock();
}

de__log_counters de__log_get_counters(void) {
    de__log_counters c = {
        de__log_tl_formatted,
        de_atomic_load_u64(&g.n_dropped, DE_ATOMIC_RELAXED),
        de_atomic_load_u64(&g.n_batches, DE_ATOMIC_RELAXED),
        de_atomic_load_u64(&g.n_rings, DE_ATOMIC_RELAXED),
        de_atomic_load_u64(&g.n_recycled, DE_ATOMIC_RELAXED),
    };
    return c;
}

/* ---------- de_log_ ---------- */

void de_log_(de_log_level lvl, de_log_category cat, const char *file, u32 line, const char *fmt,
             ...) {
    if (!de_log_enabled(lvl, cat))
        return;
    if ((u32)lvl > (u32)DE_LOG_FATAL)
        lvl = DE_LOG_FATAL;
    cat.id = de__cat_index(cat);
    u64 ts = de_time_now_ns();

    char msg[DE_LOG_MESSAGE_MAX];
    u32 len = 0;
    if (fmt != NULL) {
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(msg, sizeof msg, fmt, ap);
        va_end(ap);
        if (n > 0)
            len = (u32)n < sizeof msg ? (u32)n : (u32)(sizeof msg - 1);
    }
    msg[len] = '\0';
    de__log_tl_formatted++;

    /* Synchronous stderr when logging is not initialized, when no buffer could be allocated,
     * and for a fatal message that does not fit even after a flush. A full buffer otherwise
     * drops the message (counted and reported by the drain). */
    bool sync = true;
    if (de_atomic_load_u32(&g.running, DE_ATOMIC_ACQUIRE)) {
        u32 gen = de_atomic_load_u32(&g.gen, DE_ATOMIC_RELAXED);
        de__log_ring *r = de__log_tl.ring;
        if (DE_UNLIKELY(r == NULL || de__log_tl.gen != gen))
            r = de__ring_claim(gen);
        if (r != NULL) {
            bool queued = de__ring_push(r, lvl, cat, file, line, ts, msg, len);
            if (!queued && lvl == DE_LOG_FATAL) {
                de_log_flush();
                queued = de__ring_push(r, lvl, cat, file, line, ts, msg, len);
            }
            sync = !queued && lvl == DE_LOG_FATAL;
        }
    }
    if (sync) {
        de_log_record rec = {lvl, cat, ts, de_thread_current_id(), file, line, {msg, len}};
        de__log_write_console(&rec);
    }

    if (lvl == DE_LOG_FATAL) {
        de_log_flush();
        de_panic_(file, line, NULL, "%s", msg);
    }
}
