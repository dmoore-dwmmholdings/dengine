/* async_io.c: the v1 async I/O system, a pool of I/O threads doing blocking reads
 * (SPEC §6, §11.4, DENG-247).
 *
 * One process-wide instance, behind one mutex. Requests are caller-owned and carry their own
 * links (de_io_request.internal_), so nothing is allocated per request:
 *   - The submit queue is 256 intrusive doubly-linked FIFO lists, one per priority, plus a
 *     256-bit occupancy mask; a thread takes the head of the lowest set bit. Doubly linked so a
 *     cancel can unlink a queued request in O(1).
 *   - Finished requests that de_io_pump delivers wait in one intrusive FIFO (the completion
 *     list). It cannot fill up, so neither an I/O thread nor a cancelling thread ever blocks on
 *     a slow pump. (A bounded de_mpmc queue would have to drop completions or block there.)
 *
 * Request states, all changed under the mutex:
 *   IDLE -> QUEUED (submit) -> RUNNING (taken by a thread) -> COMPLETED (in the completion list)
 *   -> IDLE (just before on_complete runs). Cancel of QUEUED finishes it at once; cancel of
 *   RUNNING sets `cancel`, which the thread checks between chunks of DE_IO_CHUNK_BYTES.
 * The mutex is taken once per chunk; at 1 MiB per chunk that is noise next to the read. */
#include <dengine/de_platform.h>

#include "platform/de_atomic.h"
#include "platform/vfs_internal.h"

#include <stdio.h>
#include <string.h>

enum {
    REQ_IDLE = 0,
    REQ_QUEUED = 1,
    REQ_RUNNING = 2,
    REQ_COMPLETED = 3,
    PRIORITY_LEVELS = 256,
    MAX_THREADS = 64,
};

typedef struct req_list {
    de_io_request *head, *tail;
} req_list;

typedef void (*chunk_hook_fn)(de_io_request *req, u64 done);

static struct {
    de_atomic_u32 state; /* 0 = never used, 1 = initialising, 2 = mutex ready */
    de_mutex lock;
    de_cond wake; /* I/O threads wait here for work */
    bool running;
    bool stopping;
    u32 thread_count;
    de_thread *threads[MAX_THREADS];
    u64 mask[PRIORITY_LEVELS / 64];
    req_list queue[PRIORITY_LEVELS];
    req_list completed;
    chunk_hook_fn chunk_hook; /* tests only */
} g_io;

/* Test hook (tests/unit/platform/test_async_io.c declares it): called on the I/O thread after
 * each chunk, outside the lock. Not part of the API. */
void de__io_test_set_chunk_hook(chunk_hook_fn fn);

static void io_ensure(void) {
    if (de_atomic_load_u32(&g_io.state, DE_ATOMIC_ACQUIRE) == 2)
        return;
    u32 expected = 0;
    if (de_atomic_cas_u32(&g_io.state, &expected, 1, DE_ATOMIC_ACQ_REL, DE_ATOMIC_ACQUIRE)) {
        de_mutex_init(&g_io.lock);
        de_cond_init(&g_io.wake);
        de_atomic_store_u32(&g_io.state, 2, DE_ATOMIC_RELEASE);
        return;
    }
    while (de_atomic_load_u32(&g_io.state, DE_ATOMIC_ACQUIRE) != 2)
        de_cpu_pause();
}

void de__io_test_set_chunk_hook(chunk_hook_fn fn) {
    io_ensure();
    de_mutex_lock(&g_io.lock);
    g_io.chunk_hook = fn;
    de_mutex_unlock(&g_io.lock);
}

/* ---- Intrusive lists (lock held) --------------------------------------------------------- */

static void list_push_back(req_list *l, de_io_request *r) {
    r->internal_.next = NULL;
    r->internal_.prev = l->tail;
    if (l->tail != NULL)
        l->tail->internal_.next = r;
    else
        l->head = r;
    l->tail = r;
}

static void list_unlink(req_list *l, de_io_request *r) {
    if (r->internal_.prev != NULL)
        r->internal_.prev->internal_.next = r->internal_.next;
    else
        l->head = r->internal_.next;
    if (r->internal_.next != NULL)
        r->internal_.next->internal_.prev = r->internal_.prev;
    else
        l->tail = r->internal_.prev;
    r->internal_.next = r->internal_.prev = NULL;
}

static u32 ctz64(u64 v) {
    u32 n = 0;
    while ((v & 1u) == 0) {
        v >>= 1;
        ++n;
    }
    return n;
}

static void queue_push(de_io_request *r) {
    u32 p = r->priority;
    list_push_back(&g_io.queue[p], r);
    g_io.mask[p >> 6] |= (u64)1 << (p & 63);
}

static void queue_remove(de_io_request *r) {
    u32 p = r->priority;
    list_unlink(&g_io.queue[p], r);
    if (g_io.queue[p].head == NULL)
        g_io.mask[p >> 6] &= ~((u64)1 << (p & 63));
}

/* Head of the highest-priority (lowest value) non-empty list, or NULL. */
static de_io_request *queue_peek(void) {
    for (u32 w = 0; w < PRIORITY_LEVELS / 64; ++w) {
        if (g_io.mask[w] != 0)
            return g_io.queue[w * 64 + ctz64(g_io.mask[w])].head;
    }
    return NULL;
}

/* ---- Completion -------------------------------------------------------------------------- */

/*
 * Finishes `r` with the lock held. Pump-delivered requests go to the completion list and false
 * is returned. For DE_IO_CALLBACK_ON_IO_THREAD the request goes back to IDLE and true is
 * returned: the caller must run on_complete after unlocking.
 */
static bool finish_locked(de_io_request *r, de_result result, u64 done) {
    r->internal_.result = (i32)result;
    r->internal_.done = done;
    if ((r->flags & DE_IO_CALLBACK_ON_IO_THREAD) != 0) {
        r->internal_.state = REQ_IDLE;
        return true;
    }
    r->internal_.state = REQ_COMPLETED;
    list_push_back(&g_io.completed, r);
    return false;
}

/* ---- I/O threads ------------------------------------------------------------------------- */

static i32 io_thread_main(void *user) {
    (void)user;
    de_mutex_lock(&g_io.lock);
    for (;;) {
        de_io_request *r = queue_peek();
        while (r == NULL && !g_io.stopping) {
            de_cond_wait(&g_io.wake, &g_io.lock);
            r = queue_peek();
        }
        if (r == NULL)
            break; /* stopping, and shutdown already cancelled the queue */
        queue_remove(r);
        r->internal_.state = REQ_RUNNING;
        r->internal_.cancel = 0;
        /* Copy what the reads need: the caller does not change a submitted request. */
        de_file *file = r->file;
        u8 *dst = (u8 *)r->dst;
        u64 offset = r->offset, bytes = r->bytes;
        de_mutex_unlock(&g_io.lock);

        u64 done = 0;
        de_result result = DE_OK;
        for (;;) {
            de_mutex_lock(&g_io.lock);
            bool cancel = r->internal_.cancel != 0 || g_io.stopping;
            chunk_hook_fn hook = g_io.chunk_hook;
            de_mutex_unlock(&g_io.lock);
            if (cancel) {
                result = DE_ERR_CANCELLED;
                break;
            }
            if (done == bytes)
                break;
            u64 want = bytes - done;
            if (want > DE_IO_CHUNK_BYTES)
                want = DE_IO_CHUNK_BYTES;
            u64 got = 0;
            result = de__file_pread(file, dst + done, offset + done, want, &got);
            if (result != DE_OK)
                break;
            done += got;
            if (got == 0)
                break; /* end of file */
            if (hook != NULL)
                hook(r, done);
        }

        de_mutex_lock(&g_io.lock);
        if (finish_locked(r, result, done)) {
            void (*cb)(de_io_request *, de_result, u64) = r->on_complete;
            de_mutex_unlock(&g_io.lock);
            cb(r, result, done);
            de_mutex_lock(&g_io.lock);
        }
    }
    de_mutex_unlock(&g_io.lock);
    return 0;
}

/* ---- API --------------------------------------------------------------------------------- */

de_result de_io_init(const de_io_desc *desc) {
    u32 n = desc != NULL && desc->thread_count != 0 ? desc->thread_count : DE_IO_DEFAULT_THREADS;
    if (n > MAX_THREADS)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_io_init: %u threads (at most %d)", n,
                       MAX_THREADS);
    io_ensure();
    de_mutex_lock(&g_io.lock);
    if (g_io.running || g_io.stopping) {
        de_mutex_unlock(&g_io.lock);
        return DE_FAIL(DE_ERR_ALREADY_EXISTS, "de_io_init: the I/O system is already running");
    }
    g_io.running = true;
    g_io.stopping = false;
    g_io.thread_count = 0;
    de_mutex_unlock(&g_io.lock);

    for (u32 i = 0; i < n; ++i) {
        char name[16];
        snprintf(name, sizeof name, "de-io-%u", (unsigned)i);
        de_thread *t = NULL;
        de_result r = de_thread_create(io_thread_main, NULL, name, DE_THREAD_NORMAL, 0, &t);
        if (r != DE_OK) {
            de_io_shutdown();
            return DE_FAIL_WRAP(r, "de_io_init: cannot start I/O thread %u", (unsigned)i);
        }
        de_mutex_lock(&g_io.lock);
        g_io.threads[g_io.thread_count++] = t;
        de_mutex_unlock(&g_io.lock);
    }
    return DE_OK;
}

de_result de_io_submit(de_io_request *req) {
    if (req == NULL || req->file == NULL || req->on_complete == NULL ||
        (req->dst == NULL && req->bytes > 0))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_io_submit: request, file, on_complete or dst is NULL");
    if (!de__file_readable(req->file))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_io_submit: file not opened for reading");
    io_ensure();
    de_mutex_lock(&g_io.lock);
    if (!g_io.running || g_io.stopping) {
        de_mutex_unlock(&g_io.lock);
        return DE_FAIL(DE_ERR_UNSUPPORTED, "de_io_submit: the I/O system is not running");
    }
    if (req->internal_.state != REQ_IDLE) {
        de_mutex_unlock(&g_io.lock);
        return DE_FAIL(DE_ERR_BUSY, "de_io_submit: the request is already submitted");
    }
    req->internal_.state = REQ_QUEUED;
    req->internal_.cancel = 0;
    req->internal_.done = 0;
    req->internal_.result = DE_OK;
    queue_push(req);
    de_cond_signal(&g_io.wake);
    de_mutex_unlock(&g_io.lock);
    return DE_OK;
}

void de_io_cancel(de_io_request *req) {
    if (req == NULL)
        return;
    io_ensure();
    de_mutex_lock(&g_io.lock);
    if (req->internal_.state == REQ_QUEUED) {
        queue_remove(req);
        if (finish_locked(req, DE_ERR_CANCELLED, 0)) {
            void (*cb)(de_io_request *, de_result, u64) = req->on_complete;
            de_mutex_unlock(&g_io.lock);
            cb(req, DE_ERR_CANCELLED, 0);
            return;
        }
    } else if (req->internal_.state == REQ_RUNNING) {
        req->internal_.cancel = 1;
    }
    de_mutex_unlock(&g_io.lock);
}

u32 de_io_pump(u32 max_completions) {
    io_ensure();
    u32 delivered = 0;
    while (max_completions == 0 || delivered < max_completions) {
        de_mutex_lock(&g_io.lock);
        de_io_request *r = g_io.completed.head;
        if (r == NULL) {
            de_mutex_unlock(&g_io.lock);
            break;
        }
        list_unlink(&g_io.completed, r);
        r->internal_.state = REQ_IDLE;
        de_result result = (de_result)r->internal_.result;
        u64 done = r->internal_.done;
        void (*cb)(de_io_request *, de_result, u64) = r->on_complete;
        de_mutex_unlock(&g_io.lock);
        /* From here the request belongs to the caller again; it may be resubmitted or freed. */
        cb(r, result, done);
        ++delivered;
    }
    return delivered;
}

void de_io_shutdown(void) {
    io_ensure();
    de_mutex_lock(&g_io.lock);
    if (!g_io.running || g_io.stopping) {
        de_mutex_unlock(&g_io.lock);
        return;
    }
    g_io.stopping = true;
    de_cond_broadcast(&g_io.wake);
    /* Cancel what never started. Callbacks of DE_IO_CALLBACK_ON_IO_THREAD requests run here,
     * one at a time without the lock. */
    for (;;) {
        de_io_request *r = queue_peek();
        if (r == NULL)
            break;
        queue_remove(r);
        if (finish_locked(r, DE_ERR_CANCELLED, 0)) {
            void (*cb)(de_io_request *, de_result, u64) = r->on_complete;
            de_mutex_unlock(&g_io.lock);
            cb(r, DE_ERR_CANCELLED, 0);
            de_mutex_lock(&g_io.lock);
        }
    }
    u32 n = g_io.thread_count;
    de_thread *threads[MAX_THREADS];
    memcpy(threads, g_io.threads, n * sizeof threads[0]);
    de_mutex_unlock(&g_io.lock);

    /* Running requests stop at their next chunk boundary; the completion list never fills, so
     * the threads cannot block on it. */
    for (u32 i = 0; i < n; ++i)
        (void)de_thread_join(threads[i]);
    (void)de_io_pump(0);

    de_mutex_lock(&g_io.lock);
    g_io.thread_count = 0;
    g_io.running = false;
    g_io.stopping = false;
    de_mutex_unlock(&g_io.lock);
}
