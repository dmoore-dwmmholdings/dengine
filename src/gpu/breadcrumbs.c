/* breadcrumbs.c — GPU crash diagnostics in the de_gpu front-end (DENG-328; SPEC §18.5, §12.3):
 * breadcrumb rings, the submit log, the device-lost report, the device-lost callback and the
 * crash handler that puts the report next to a crash report. See breadcrumbs_internal.h for the
 * design; the backends write the breadcrumbs and add their own fault data (gpu_backend.h).
 */
#include "gpu/breadcrumbs_internal.h"

#include "platform/crash_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---------- State ---------- */

/** What de_gpu_cmd_breadcrumb stored for one sequence number (slot = seq % DE__GPU_BC_SLOTS). */
typedef struct bc_slot {
    de_atomic_u32 seq; /**< the sequence number that owns the slot now (0: none yet) */
    u32 id;
    char path[DE__GPU_BC_PATH]; /**< open markers when it was recorded */
} bc_slot;

/** One used chunk of one submitted list. */
typedef struct bc_log {
    u64 value; /**< the submit's internal queue value */
    u32 first, count;
    char list[32]; /**< the list's name (cut) */
} bc_log;

typedef struct bc_queue {
    de_gpu_buffer ring;
    de__gpu_buffer *ring_rec;
    const volatile u32 *mapped; /**< the ring's CPU view (READBACK: persistently mapped) */
    de_atomic_u32 next_seq;     /**< next chunk's first sequence number */
    u32 log_count;              /**< entries ever logged; under the queue's submit_lock */
    bc_slot slots[DE__GPU_BC_SLOTS];
    bc_log log[DE__GPU_BC_LOG];
} bc_queue;

typedef struct bc_name {
    u32 id;
    u32 used;
    char name[DE__GPU_NAME_MAX];
} bc_name;

enum { CRASH_OK, CRASH_BUILDING, CRASH_READY };

struct de__gpu_crash {
    bool breadcrumbs;
    de_atomic_u32 state;   /**< CRASH_* */
    de_atomic_u32 dropped; /**< breadcrumbs past a recording's chunks */
    de_mutex lock;         /**< names and the callback */
    bc_name *names;        /**< open addressing, `name_cap` (a power of two) entries */
    u32 name_cap, name_count;
    de_gpu_device_lost_fn fn;
    void *fn_user;
    bool fn_called;
    bc_queue *queues[DE_QUEUE_COUNT_];
    u32 report_len;
    char report[DE_GPU_DEVICE_LOST_REPORT_MAX];
};

static const char *const k_queue_names[DE_QUEUE_COUNT_] = {"graphics", "compute", "copy"};

/* Process-wide: the report the crash handler writes (the first device lost in the process). */
static char g_copy[DE_GPU_DEVICE_LOST_REPORT_MAX];
static de_atomic_u32 g_copy_len;   /* 0 until a report was copied */
static de_atomic_u32 g_copy_owner; /* 1 once a device claimed g_copy */
static de_atomic_u32 g_handler;    /* 1 once the crash handler was added (or tried) */
static char g_path[DE__CRASH_PATH_CAP];

/* ---------- Crash handler (async-signal-safe) ---------- */

static void crash_put(const de_crash_context *ctx, const char *s) {
    de_crash_write(ctx, s, strlen(s));
}

void de__gpu_crash_handler(const de_crash_context *ctx, void *user) {
    (void)user;
    const u32 len = de_atomic_load_u32(&g_copy_len, DE_ATOMIC_ACQUIRE);
    if (len == 0 || ctx == NULL || ctx->report_stem == NULL)
        return;
    g_path[0] = '\0';
    bool exists = false;
    intptr_t fd = -1;
    if (de__crash_str_append(g_path, sizeof g_path, ctx->report_stem) &&
        de__crash_str_append(g_path, sizeof g_path, ".gpu_report.txt"))
        fd = de__crash_os_create(g_path, &exists);
    crash_put(ctx, "gpu_report: ");
    if (fd == -1) {
        crash_put(ctx, "(failed)\n");
        return;
    }
    de__crash_os_write(fd, g_copy, len);
    de__crash_os_close(fd);
    crash_put(ctx, g_path);
    crash_put(ctx, "\n");
}

const char *de__gpu_crash_report_copy(size_t *len) {
    const u32 n = de_atomic_load_u32(&g_copy_len, DE_ATOMIC_ACQUIRE);
    if (len)
        *len = n;
    return n ? g_copy : "";
}

static void add_crash_handler(de_gpu_device *d) {
    u32 expected = 0;
    if (!de_atomic_cas_u32(&g_handler, &expected, 1u, DE_ATOMIC_ACQ_REL, DE_ATOMIC_ACQUIRE))
        return;
    if (de_crash_add_handler(de__gpu_crash_handler, NULL) != DE_OK) {
        /* All handler slots taken: reports still work, only gpu_report.txt is missing. */
        DE_WARN(d->log, "gpu: no crash handler slot left; crash reports will not include the GPU "
                        "device-lost report");
        de_error_clear();
    }
}

/* ---------- Setup ---------- */

de_result de__gpu_crash_init(de_gpu_device *d) {
    struct de__gpu_crash *cr =
        (struct de__gpu_crash *)DE_ALLOC(d->alloc, sizeof *cr, 16, DE_MEM_RENDER);
    if (!cr)
        return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "de_gpu_device_create: crash diagnostics");
    memset(cr, 0, sizeof *cr);
    de_atomic_init_u32(&cr->state, CRASH_OK);
    de_atomic_init_u32(&cr->dropped, 0);
    de_mutex_init(&cr->lock);
    d->crash = cr;
    add_crash_handler(d);
    if (!d->desc.crash_breadcrumbs)
        return DE_OK;
    cr->breadcrumbs = true;
    for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q) {
        bc_queue *bq = (bc_queue *)DE_ALLOC(d->alloc, sizeof *bq, 16, DE_MEM_RENDER);
        if (!bq)
            return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "de_gpu_device_create: breadcrumbs");
        memset(bq, 0, sizeof *bq);
        de_atomic_init_u32(&bq->next_seq, 1u);
        for (u32 i = 0; i < DE__GPU_BC_SLOTS; ++i)
            de_atomic_init_u32(&bq->slots[i].seq, 0);
        cr->queues[q] = bq;
        char name[40];
        snprintf(name, sizeof name, "de breadcrumbs (%s)", k_queue_names[q]);
        bq->ring = de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = DE__GPU_BC_SLOTS * 4u,
                                                                 .usage = DE_USAGE_COPY_DST,
                                                                 .memory = DE_MEM_READBACK,
                                                                 .name = name});
        if (!DE_HANDLE_VALID(bq->ring))
            return DE_FAIL_WRAP(de_error_last() ? de_error_last()->result : DE_ERR_INTERNAL,
                                "de_gpu_device_create: breadcrumb ring (%s)", k_queue_names[q]);
        bq->ring_rec = de__gpu_get_buffer(d, bq->ring);
        bq->mapped = (const volatile u32 *)bq->ring_rec->mapped;
        if (bq->ring_rec->mapped)
            memset(bq->ring_rec->mapped, 0, DE__GPU_BC_SLOTS * 4u);
    }
    return DE_OK;
}

void de__gpu_crash_release(de_gpu_device *d) {
    struct de__gpu_crash *cr = d->crash;
    if (!cr)
        return;
    for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q) {
        bc_queue *bq = cr->queues[q];
        if (!bq || !DE_HANDLE_VALID(bq->ring))
            continue;
        de_gpu_buffer_destroy(d, bq->ring);
        bq->ring.id = 0;
        bq->ring_rec = NULL;
        bq->mapped = NULL;
    }
}

void de__gpu_crash_free(de_gpu_device *d) {
    struct de__gpu_crash *cr = d->crash;
    if (!cr)
        return;
    for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q)
        if (cr->queues[q])
            DE_FREE(d->alloc, cr->queues[q], sizeof(bc_queue));
    if (cr->names)
        DE_FREE(d->alloc, cr->names, (size_t)cr->name_cap * sizeof(bc_name));
    de_mutex_destroy(&cr->lock);
    DE_FREE(d->alloc, cr, sizeof *cr);
    d->crash = NULL;
}

/* ---------- Recording and submission ---------- */

static bc_queue *queue_of(de_gpu_device *d, de_gpu_queue q) {
    struct de__gpu_crash *cr = d->crash;
    bc_queue *bq = cr && (u32)q < DE_QUEUE_COUNT_ ? cr->queues[q] : NULL;
    return bq && bq->ring_rec ? bq : NULL;
}

void de__gpu_breadcrumb_record(de_gpu_cmd *c, u32 id) {
    de_gpu_device *d = c->device;
    bc_queue *bq = queue_of(d, c->queue);
    if (!bq)
        return;
    u32 seq;
    const u32 r = c->bc_ranges;
    if (r > 0 && c->bc_used[r - 1u] < DE__GPU_BC_CHUNK) {
        seq = c->bc_first[r - 1u] + c->bc_used[r - 1u];
        c->bc_used[r - 1u]++;
    } else {
        if (r == DE__GPU_BC_RANGES) {
            c->bc_dropped++;
            de_atomic_fetch_add_u32(&d->crash->dropped, 1u, DE_ATOMIC_RELAXED);
            return;
        }
        /* Sequence number 0 means "never written" (the ring starts zeroed): skip the chunk that
         * holds it when the counter wraps. */
        u32 first;
        do {
            first = de_atomic_fetch_add_u32(&bq->next_seq, DE__GPU_BC_CHUNK, DE_ATOMIC_RELAXED);
        } while (first == 0 || first + (DE__GPU_BC_CHUNK - 1u) < first);
        c->bc_first[r] = first;
        c->bc_used[r] = 1;
        c->bc_ranges = r + 1u;
        seq = first;
    }
    const u32 slot = seq % DE__GPU_BC_SLOTS;
    bc_slot *s = &bq->slots[slot];
    s->id = id;
    de__gpu_cmd_marker_path(c, s->path, sizeof s->path);
    de_atomic_store_u32(&s->seq, seq, DE_ATOMIC_RELEASE);
    if (d->vt->cmd_breadcrumb)
        d->vt->cmd_breadcrumb(c, bq->ring_rec, (u64)slot * 4u, seq);
}

void de__gpu_breadcrumb_submitted(de_gpu_device *d, de_gpu_queue q, const de_gpu_submit *s,
                                  u64 value) {
    bc_queue *bq = queue_of(d, q);
    if (!bq)
        return;
    for (u32 i = 0; i < s->cmd_count; ++i) {
        const de_gpu_cmd *c = s->cmds[i];
        for (u32 k = 0; k < c->bc_ranges; ++k) {
            if (c->bc_used[k] == 0)
                continue;
            bc_log *e = &bq->log[bq->log_count % DE__GPU_BC_LOG];
            e->value = value;
            e->first = c->bc_first[k];
            e->count = c->bc_used[k];
            size_t n = strlen(c->name);
            if (n > sizeof e->list - 1u)
                n = sizeof e->list - 1u;
            memcpy(e->list, c->name, n);
            e->list[n] = '\0';
            bq->log_count++;
        }
    }
}

/* ---------- Names ---------- */

static u32 name_hash(u32 id) {
    return id * 2654435761u;
}

/* Index of `id` in the table, or of the empty entry where it would go. Under cr->lock. */
static u32 name_find(const struct de__gpu_crash *cr, u32 id) {
    const u32 mask = cr->name_cap - 1u;
    u32 i = name_hash(id) & mask;
    while (cr->names[i].used && cr->names[i].id != id)
        i = (i + 1u) & mask;
    return i;
}

static de_result name_grow(de_gpu_device *d, struct de__gpu_crash *cr) {
    const u32 cap = cr->name_cap ? cr->name_cap * 2u : 64u;
    bc_name *t = (bc_name *)DE_ALLOC(d->alloc, (size_t)cap * sizeof *t, 8, DE_MEM_RENDER);
    if (!t)
        return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "de_gpu_breadcrumb_name: name table (%u)", cap);
    memset(t, 0, (size_t)cap * sizeof *t);
    bc_name *old = cr->names;
    const u32 old_cap = cr->name_cap;
    cr->names = t;
    cr->name_cap = cap;
    for (u32 i = 0; i < old_cap; ++i)
        if (old[i].used)
            t[name_find(cr, old[i].id)] = old[i];
    if (old)
        DE_FREE(d->alloc, old, (size_t)old_cap * sizeof *old);
    return DE_OK;
}

de_result de_gpu_breadcrumb_name(de_gpu_device *d, u32 id, const char *name) {
    if (!d || !name)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_gpu_breadcrumb_name: %s is NULL",
                       d ? "name" : "device");
    struct de__gpu_crash *cr = d->crash;
    if (!cr)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_gpu_breadcrumb_name: no crash state");
    de_result r = DE_OK;
    de_mutex_lock(&cr->lock);
    if ((cr->name_count + 1u) * 2u > cr->name_cap)
        r = name_grow(d, cr);
    if (r == DE_OK) {
        bc_name *e = &cr->names[name_find(cr, id)];
        if (!e->used) {
            e->used = 1;
            e->id = id;
            cr->name_count++;
        }
        size_t n = strlen(name);
        if (n > sizeof e->name - 1u)
            n = sizeof e->name - 1u;
        memcpy(e->name, name, n);
        e->name[n] = '\0';
    }
    de_mutex_unlock(&cr->lock);
    return r;
}

/* Copies the registered name of `id` (or "") into `out`. */
static void name_of(struct de__gpu_crash *cr, u32 id, char out[DE__GPU_NAME_MAX]) {
    out[0] = '\0';
    de_mutex_lock(&cr->lock);
    if (cr->name_cap) {
        const bc_name *e = &cr->names[name_find(cr, id)];
        if (e->used)
            memcpy(out, e->name, DE__GPU_NAME_MAX);
    }
    de_mutex_unlock(&cr->lock);
}

/* ---------- The report ---------- */

typedef struct report_buf {
    char *out;
    size_t cap, len;
} report_buf;

static void put(report_buf *b, const char *fmt, ...) DE_PRINTF_FMT(2, 3);
static void put(report_buf *b, const char *fmt, ...) {
    if (b->len + 1u >= b->cap)
        return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(b->out + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (w > 0)
        b->len += (size_t)w < b->cap - b->len ? (size_t)w : b->cap - b->len - 1u;
}

/** What one queue's scan found. */
typedef struct bc_scan {
    bool have_last, have_next;
    u32 last_seq, next_seq;
    u32 last_k, next_k; /**< their log entries */
    u32 logged;         /**< breadcrumbs in the kept log, still in the ring */
    u32 overwritten;    /**< logged, but their slot was reused since */
    u32 pending;        /**< logged after the last completed one and not written */
} bc_scan;

static bool slot_current(const bc_queue *bq, u32 seq) {
    return de_atomic_load_u32(&bq->slots[seq % DE__GPU_BC_SLOTS].seq, DE_ATOMIC_ACQUIRE) == seq;
}

static bool slot_written(const bc_queue *bq, u32 seq) {
    return bq->mapped && bq->mapped[seq % DE__GPU_BC_SLOTS] == seq;
}

/* Scans the kept log of `bq` in submission order. `each` (may be NULL) is called for each pending
 * breadcrumb after the last completed one. Under the queue's submit_lock. */
static void scan_queue(const bc_queue *bq, bc_scan *sc,
                       void (*each)(void *user, const bc_log *e, u32 seq), void *user) {
    memset(sc, 0, sizeof *sc);
    const u32 n = bq->log_count < DE__GPU_BC_LOG ? bq->log_count : DE__GPU_BC_LOG;
    const u32 start = bq->log_count - n;
    for (u32 k = start; k != bq->log_count; ++k) {
        const bc_log *e = &bq->log[k % DE__GPU_BC_LOG];
        for (u32 j = 0; j < e->count; ++j) {
            const u32 seq = e->first + j;
            if (!slot_current(bq, seq)) {
                sc->overwritten++;
                continue;
            }
            sc->logged++;
            if (slot_written(bq, seq)) {
                sc->have_last = true;
                sc->last_seq = seq;
                sc->last_k = k;
            }
        }
    }
    const u32 from = sc->have_last ? sc->last_k : start;
    for (u32 k = from; k != bq->log_count; ++k) {
        const bc_log *e = &bq->log[k % DE__GPU_BC_LOG];
        for (u32 j = 0; j < e->count; ++j) {
            const u32 seq = e->first + j;
            if (k == sc->last_k && sc->have_last && seq - e->first <= sc->last_seq - e->first)
                continue; /* at or before the last completed one */
            if (!slot_current(bq, seq) || slot_written(bq, seq))
                continue;
            if (!sc->have_next) {
                sc->have_next = true;
                sc->next_seq = seq;
                sc->next_k = k;
            }
            sc->pending++;
            if (each)
                each(user, e, seq);
        }
    }
}

typedef struct describe_ctx {
    struct de__gpu_crash *cr;
    const bc_queue *bq;
    report_buf *b;
    u32 listed;
} describe_ctx;

static void describe(describe_ctx *x, const bc_log *e, u32 seq) {
    const bc_slot *s = &x->bq->slots[seq % DE__GPU_BC_SLOTS];
    char name[DE__GPU_NAME_MAX];
    name_of(x->cr, s->id, name);
    put(x->b, "id 0x%08x", (unsigned)s->id);
    if (name[0])
        put(x->b, " \"%s\"", name);
    put(x->b, " (list \"%s\", queue value %llu", e->list, (unsigned long long)e->value);
    if (s->path[0])
        put(x->b, ", markers %s", s->path);
    put(x->b, ")\n");
}

static void describe_pending(void *user, const bc_log *e, u32 seq) {
    describe_ctx *x = (describe_ctx *)user;
    if (x->listed++ >= DE__GPU_BC_REPORT_PENDING)
        return;
    put(x->b, "    ");
    describe(x, e, seq);
}

/* The queue's section; the summary names of the last and next breadcrumbs go to `last`/`next`. */
static void report_queue(de_gpu_device *d, de_gpu_queue q, report_buf *b, char *summary,
                         size_t summary_cap) {
    struct de__gpu_crash *cr = d->crash;
    const bc_queue *bq = queue_of(d, q);
    put(b, "\n[%s queue] last submitted queue value %llu\n", k_queue_names[q],
        (unsigned long long)de_atomic_load_u64(&d->submitted[q], DE_ATOMIC_ACQUIRE));
    if (!bq)
        return;
    if (bq->log_count == 0) {
        put(b, "  no breadcrumbs submitted\n");
        return;
    }
    describe_ctx x = {cr, bq, b, 0};
    bc_scan sc;
    /* First pass for the header lines, second pass lists the pending ones under them. */
    scan_queue(bq, &sc, NULL, NULL);
    put(b, "  breadcrumbs in the log: %u", sc.logged);
    if (sc.overwritten)
        put(b, " (%u more were overwritten in the ring)", sc.overwritten);
    put(b, "\n  last completed: ");
    if (sc.have_last)
        describe(&x, &bq->log[sc.last_k % DE__GPU_BC_LOG], sc.last_seq);
    else
        put(b, "none\n");
    put(b, "  not reached: %u\n", sc.pending);
    scan_queue(bq, &sc, describe_pending, &x);
    if (sc.pending > DE__GPU_BC_REPORT_PENDING)
        put(b, "    ... and %u more\n", sc.pending - DE__GPU_BC_REPORT_PENDING);

    /* Summary: "<queue>: after \"a\", at \"b\"". */
    char last[DE__GPU_NAME_MAX], next[DE__GPU_NAME_MAX];
    last[0] = next[0] = '\0';
    if (sc.have_last)
        name_of(cr, bq->slots[sc.last_seq % DE__GPU_BC_SLOTS].id, last);
    if (sc.have_next)
        name_of(cr, bq->slots[sc.next_seq % DE__GPU_BC_SLOTS].id, next);
    if (sc.have_last || sc.have_next) {
        size_t used = strlen(summary);
        char ids[2][16];
        snprintf(ids[0], sizeof ids[0], "0x%08x",
                 (unsigned)(sc.have_last ? bq->slots[sc.last_seq % DE__GPU_BC_SLOTS].id : 0));
        snprintf(ids[1], sizeof ids[1], "0x%08x",
                 (unsigned)(sc.have_next ? bq->slots[sc.next_seq % DE__GPU_BC_SLOTS].id : 0));
        snprintf(summary + used, summary_cap - used, "; %s: last done %s, next %s",
                 k_queue_names[q], sc.have_last ? (last[0] ? last : ids[0]) : "none",
                 sc.have_next ? (next[0] ? next : ids[1]) : "none");
    }
}

/* Builds cr->report. */
static void build_report(de_gpu_device *d, const char *where, const char *error, char *summary,
                         size_t summary_cap) {
    struct de__gpu_crash *cr = d->crash;
    report_buf b = {cr->report, sizeof cr->report, 0};
    cr->report[0] = '\0';
    put(&b, "DEngine GPU device-lost report\n");
    put(&b, "backend: %s, device \"%s\"\n", de_gpu_backend_name(d->caps.backend),
        d->caps.device_name);
    put(&b, "found by: %s\n", where);
    if (error && error[0])
        put(&b, "error: %s\n", error);
    put(&b, "breadcrumbs: %s\n",
        cr->breadcrumbs ? "on" : "off (de_gpu_device_desc.crash_breadcrumbs)");
    snprintf(summary, summary_cap, "%s device lost (%s)", de_gpu_backend_name(d->caps.backend),
             where);
    for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q) {
        de_mutex_lock(&d->submit_lock[q]);
        report_queue(d, (de_gpu_queue)q, &b, summary, summary_cap);
        de_mutex_unlock(&d->submit_lock[q]);
    }
    const u32 dropped = de_atomic_load_u32(&cr->dropped, DE_ATOMIC_RELAXED);
    if (dropped)
        put(&b, "\nbreadcrumbs dropped (more than %u in one recording): %u\n",
            DE__GPU_BC_RANGES * DE__GPU_BC_CHUNK, dropped);
    put(&b, "\n[%s backend]\n", d->vt->name ? d->vt->name : "?");
    if (d->vt->device_lost_report && b.len + 1u < b.cap) {
        size_t n = d->vt->device_lost_report(d, b.out + b.len, b.cap - b.len);
        b.len += n < b.cap - b.len ? n : b.cap - b.len - 1u;
        b.out[b.len] = '\0';
    } else {
        put(&b, "no backend data\n");
    }
    cr->report_len = (u32)b.len;
}

static void run_callback(struct de__gpu_crash *cr) {
    de_mutex_lock(&cr->lock);
    const bool call = cr->fn && !cr->fn_called &&
                      de_atomic_load_u32(&cr->state, DE_ATOMIC_ACQUIRE) == CRASH_READY;
    de_gpu_device_lost_fn fn = cr->fn;
    void *user = cr->fn_user;
    if (call)
        cr->fn_called = true;
    de_mutex_unlock(&cr->lock);
    if (call)
        fn(cr->report, user);
}

/* The error context's chain as one line ("" when none). */
static void error_line(char *out, size_t cap) {
    size_t used = 0;
    out[0] = '\0';
    for (const de_error *e = de_error_last(); e && used + 1u < cap; e = e->cause) {
        int w = snprintf(out + used, cap - used, "%s%s", used ? " <- " : "", e->message);
        if (w < 0)
            break;
        used += (size_t)w < cap - used ? (size_t)w : cap - used - 1u;
    }
}

void de__gpu_device_lost(de_gpu_device *d, const char *where) {
    u32 expected = 0;
    if (!de_atomic_cas_u32(&d->lost, &expected, 1u, DE_ATOMIC_ACQ_REL, DE_ATOMIC_ACQUIRE))
        return; /* another call handles (or handled) it */
    struct de__gpu_crash *cr = d->crash;
    if (!cr)
        return;
    de_atomic_store_u32(&cr->state, CRASH_BUILDING, DE_ATOMIC_RELEASE);
    char error[768];
    error_line(error, sizeof error);
    char summary[DE_CRASH_ANNOTATION_VALUE_MAX];
    build_report(d, where, error, summary, sizeof summary);
    de_atomic_store_u32(&cr->state, CRASH_READY, DE_ATOMIC_RELEASE);
    DE_ERROR(d->log, "gpu: %s\n%s", summary, cr->report);

    u32 none = 0;
    if (de_atomic_cas_u32(&g_copy_owner, &none, 1u, DE_ATOMIC_ACQ_REL, DE_ATOMIC_ACQUIRE)) {
        memcpy(g_copy, cr->report, cr->report_len);
        de_atomic_store_u32(&g_copy_len, cr->report_len, DE_ATOMIC_RELEASE);
    }
    de_crash_set_annotation("gpu_fault", summary);
    run_callback(cr);
}

/* ---------- Loss checks (gpu.c) ---------- */

bool de__gpu_poll_lost(de_gpu_device *d, const char *where) {
    if (de__gpu_is_lost(d))
        return true;
    if (d->vt->device_lost && d->vt->device_lost(d)) {
        de__gpu_device_lost(d, where);
        return true;
    }
    return false;
}

de_result de__gpu_check_lost(de_gpu_device *d, de_result r, const char *where) {
    if (r == DE_ERR_DEVICE_LOST)
        de__gpu_device_lost(d, where);
    return r;
}

de_result de__gpu_lost_error(const char *where) {
    return DE_FAIL(DE_ERR_DEVICE_LOST, "%s: the GPU device is lost", where);
}

/* ---------- Public API ---------- */

void de_gpu_set_device_lost_callback(de_gpu_device *d, de_gpu_device_lost_fn fn, void *user) {
    if (!d || !d->crash)
        return;
    struct de__gpu_crash *cr = d->crash;
    de_mutex_lock(&cr->lock);
    cr->fn = fn;
    cr->fn_user = user;
    de_mutex_unlock(&cr->lock);
    run_callback(cr); /* the device may be lost already */
}

bool de_gpu_device_is_lost(de_gpu_device *d) {
    return d && de__gpu_poll_lost(d, "de_gpu_device_is_lost");
}

de_result de_gpu_device_lost_report(de_gpu_device *d, char *buf, u32 cap) {
    static const char F[] = "de_gpu_device_lost_report";
    if (!d || !buf || cap == 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s: %s", F,
                       !d     ? "device is NULL"
                       : !buf ? "buf is NULL"
                              : "cap is 0");
    buf[0] = '\0';
    if (!de__gpu_poll_lost(d, F) || !d->crash)
        return DE_FAIL(DE_ERR_NOT_FOUND, "%s: the device is not lost", F);
    struct de__gpu_crash *cr = d->crash;
    /* Another thread may still be writing it (it found the loss first). */
    while (de_atomic_load_u32(&cr->state, DE_ATOMIC_ACQUIRE) != CRASH_READY)
        de_sleep_ns(100000);
    if (cr->report_len < cap) {
        memcpy(buf, cr->report, (size_t)cr->report_len + 1u);
        return DE_OK;
    }
    memcpy(buf, cr->report, cap - 1u);
    buf[cap - 1u] = '\0';
    return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "%s: the report has %u bytes; cut to %u", F,
                   cr->report_len, cap - 1u);
}

/* ---------- Test hooks ---------- */

bool de__gpu_breadcrumb_last(de_gpu_device *d, de_gpu_queue q, u32 *seq, u32 *id) {
    bc_queue *bq = d ? queue_of(d, q) : NULL;
    if (!bq)
        return false;
    bc_scan sc;
    de_mutex_lock(&d->submit_lock[q]);
    scan_queue(bq, &sc, NULL, NULL);
    de_mutex_unlock(&d->submit_lock[q]);
    if (!sc.have_last)
        return false;
    if (seq)
        *seq = sc.last_seq;
    if (id)
        *id = bq->slots[sc.last_seq % DE__GPU_BC_SLOTS].id;
    return true;
}

de__gpu_buffer *de__gpu_breadcrumb_ring(de_gpu_device *d, de_gpu_queue q) {
    bc_queue *bq = d ? queue_of(d, q) : NULL;
    return bq ? bq->ring_rec : NULL;
}

u32 de__gpu_breadcrumb_dropped(de_gpu_device *d) {
    return d && d->crash ? de_atomic_load_u32(&d->crash->dropped, DE_ATOMIC_RELAXED) : 0;
}
