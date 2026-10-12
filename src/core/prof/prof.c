/*
 * prof.c: the profiler front end (DENG-94; SPEC §18.1, §5.2, §5.3). de_debug.h's profiling
 * section, on top of one backend (prof_internal.h): Tracy in DE_TRACY builds, a test mock with
 * DE_PROF_MOCK.
 *
 * State. One atomic pointer to the running backend (NULL when stopped). Every event loads it
 * (acquire) and returns when it is NULL, so a stopped profiler costs a load and a branch.
 * de_prof_init stores it last and de_prof_shutdown clears it first; both run while no other
 * thread profiles (de_debug.h, lifetime rules), so a backend call never overlaps its shutdown.
 *
 * Spans. Each de_prof_init starts a new span with a new epoch (never 0). A de_prof_lock records
 * the epoch it was announced in, and lock events ignore a context from another span: after a
 * shutdown the backend has freed it.
 *
 * Memory. The hooks (de_mem_set_hooks) report each block of a reporting allocator (the heap in
 * DE_MEM_TRACKING builds, tracking allocators) with its tag name as the pool. Tag names live in a
 * static table, so their pointers are stable, as Tracy needs. The hooks chain to the ones that
 * were installed before (the alloc guard, DENG-155, chains the same way).
 *
 * Log. A sink sends Warn, Error and Fatal records as "<category>: <message>", coloured by level.
 * Sinks run on the log thread (or the thread in de_log_flush), so the messages show there.
 *
 * Without DE_PROFILE only de_prof_init (DE_ERR_UNSUPPORTED), de_prof_shutdown, de_prof_running and
 * de_prof_connected exist, and nothing refers to a backend.
 */
#include <dengine/de_base.h>
#include <dengine/de_debug.h>
#include <dengine/de_memory.h>

#include "core/prof/prof_internal.h"

#ifdef DE_PROFILE

#include "core/de_compiler.h"
#include "platform/de_atomic.h"

#include <string.h>

#if defined(DE_PROF_MOCK)
#define DE__PROF_BACKEND (&de__prof_mock_backend)
#elif defined(DE_TRACY)
#define DE__PROF_BACKEND (&de__prof_tracy_backend)
#else
#define DE__PROF_BACKEND NULL
#endif

static struct {
    de_atomic_ptr backend; /* const de__prof_backend *, NULL when stopped */
    de_atomic_u32 epoch;   /* span of the running backend; 0 before the first init */
    de_mem_hooks hooks;    /* ours, while installed */
    const de_mem_hooks *prev_hooks;
} de__prof = {DE_ATOMIC_INIT(NULL), DE_ATOMIC_INIT(0), {NULL, NULL, NULL}, NULL};

static inline const de__prof_backend *de__prof_be(void) {
    return (const de__prof_backend *)de_atomic_load_ptr(&de__prof.backend, DE_ATOMIC_ACQUIRE);
}

/* ---------- Memory hooks ---------- */

static void de__prof_on_alloc(void *user, void *ptr, size_t size, u32 tag) {
    (void)user;
    const de__prof_backend *be = de__prof_be();
    if (be != NULL)
        be->alloc(ptr, size, de_mem_tag_name(tag));
    const de_mem_hooks *prev = de__prof.prev_hooks;
    if (prev != NULL && prev->on_alloc != NULL)
        prev->on_alloc(prev->user, ptr, size, tag);
}

static void de__prof_on_free(void *user, void *ptr, size_t size, u32 tag) {
    (void)user;
    const de__prof_backend *be = de__prof_be();
    if (be != NULL)
        be->free(ptr, de_mem_tag_name(tag));
    const de_mem_hooks *prev = de__prof.prev_hooks;
    if (prev != NULL && prev->on_free != NULL)
        prev->on_free(prev->user, ptr, size, tag);
}

/* ---------- Log sink ---------- */

static void de__prof_log_sink(const de_log_record *rec, void *user) {
    (void)user;
    if (rec->level < DE_LOG_WARN)
        return;
    const de__prof_backend *be = de__prof_be();
    if (be == NULL)
        return;
    /* "<category>: <message>", cut to the buffer (messages are at most DE_LOG_MESSAGE_MAX). */
    char buf[DE_LOG_CATEGORY_NAME_MAX + 2 + DE_LOG_MESSAGE_MAX];
    size_t n = 0;
    const char *cat = de_log_category_name(rec->category);
    if (cat != NULL && cat[0] != '\0') {
        size_t cl = strlen(cat);
        if (cl > DE_LOG_CATEGORY_NAME_MAX)
            cl = DE_LOG_CATEGORY_NAME_MAX;
        memcpy(buf, cat, cl);
        buf[cl] = ':';
        buf[cl + 1] = ' ';
        n = cl + 2;
    }
    size_t ml = rec->message.ptr != NULL ? rec->message.len : 0;
    if (ml > sizeof buf - n)
        ml = sizeof buf - n;
    if (ml > 0)
        memcpy(buf + n, rec->message.ptr, ml);
    n += ml;

    de__prof_severity sev;
    u32 color;
    switch (rec->level) {
    case DE_LOG_WARN:
        sev = DE__PROF_SEV_WARN;
        color = DE__PROF_COLOR_WARN;
        break;
    case DE_LOG_ERROR:
        sev = DE__PROF_SEV_ERROR;
        color = DE__PROF_COLOR_ERROR;
        break;
    default:
        sev = DE__PROF_SEV_FATAL;
        color = DE__PROF_COLOR_FATAL;
        break;
    }
    be->message(buf, n, sev, color);
}

/* ---------- Lifetime ---------- */

de_result de_prof_init(void) {
    const de__prof_backend *be = DE__PROF_BACKEND;
    if (be == NULL) {
        DE_FAIL(DE_ERR_UNSUPPORTED, "de_prof_init: this build has no profiler backend (DE_TRACY)");
        return DE_ERR_UNSUPPORTED;
    }
    if (de__prof_be() != NULL)
        return DE_OK;

    be->startup();
    u32 epoch = de_atomic_load_u32(&de__prof.epoch, DE_ATOMIC_RELAXED) + 1u;
    if (epoch == 0)
        epoch = 1;
    de_atomic_store_u32(&de__prof.epoch, epoch, DE_ATOMIC_RELAXED);
    de_atomic_store_ptr(&de__prof.backend, (void *)(uintptr_t)be, DE_ATOMIC_RELEASE);

    de__prof.hooks.on_alloc = de__prof_on_alloc;
    de__prof.hooks.on_free = de__prof_on_free;
    de__prof.hooks.user = NULL;
    /* No other thread runs during init (lifetime rules), so nothing reports between the install
     * and the store of prev_hooks. */
    de__prof.prev_hooks = de_mem_set_hooks(&de__prof.hooks);
    de_log_add_sink(de__prof_log_sink, NULL);
    return DE_OK;
}

void de_prof_shutdown(void) {
    const de__prof_backend *be = de__prof_be();
    if (be == NULL)
        return;
    de_log_remove_sink(de__prof_log_sink, NULL);
    const de_mem_hooks *cur = de_mem_set_hooks(de__prof.prev_hooks);
    if (cur != &de__prof.hooks) {
        /* Someone installed hooks after ours and did not take them out (uninstall order is the
         * reverse of install): keep theirs, ours stop sending once the backend is gone. */
        de_mem_set_hooks(cur);
    }
    de_atomic_store_ptr(&de__prof.backend, NULL, DE_ATOMIC_RELEASE);
    be->shutdown();
}

bool de_prof_running(void) {
    return de__prof_be() != NULL;
}

bool de_prof_connected(void) {
    const de__prof_backend *be = de__prof_be();
    return be != NULL && be->connected();
}

/* ---------- Events ---------- */

de_prof_zone de_prof_zone_begin_(const de_prof_srcloc *loc) {
    const de__prof_backend *be = de__prof_be();
    if (DE_LIKELY(be == NULL)) {
        de_prof_zone z = {{0, 0}};
        return z;
    }
    return be->zone_begin(loc);
}

void de_prof_zone_end_(de_prof_zone z) {
    const de__prof_backend *be = de__prof_be();
    if (be != NULL)
        be->zone_end(z);
}

void de_prof_frame_mark_(const char *name) {
    const de__prof_backend *be = de__prof_be();
    if (be != NULL)
        be->frame_mark(name);
}

void de_prof_plot_(const char *name, f64 value) {
    const de__prof_backend *be = de__prof_be();
    if (be != NULL && name != NULL)
        be->plot(name, value);
}

void de_prof_alloc_(const void *ptr, size_t size, const char *pool) {
    const de__prof_backend *be = de__prof_be();
    if (be != NULL)
        be->alloc(ptr, size, pool);
}

void de_prof_free_(const void *ptr, const char *pool) {
    const de__prof_backend *be = de__prof_be();
    if (be != NULL)
        be->free(ptr, pool);
}

void de_prof_thread_name_(const char *name) {
    const de__prof_backend *be = de__prof_be();
    if (be != NULL && name != NULL && name[0] != '\0')
        be->thread_name(name);
}

/* ---------- Locks ---------- */

/* The context of `l` if it belongs to the running span, else NULL. */
static inline void *de__prof_lock_ctx(const de_prof_lock *l, const de__prof_backend *be) {
    if (be == NULL || l->ctx == NULL)
        return NULL;
    return l->epoch == de_atomic_load_u32(&de__prof.epoch, DE_ATOMIC_RELAXED) ? l->ctx : NULL;
}

void de_prof_lock_init_(de_prof_lock *l, const de_prof_srcloc *loc) {
    memset(l, 0, sizeof *l);
    const de__prof_backend *be = de__prof_be();
    if (be == NULL)
        return;
    l->ctx = be->lock_announce(loc);
    l->epoch = de_atomic_load_u32(&de__prof.epoch, DE_ATOMIC_RELAXED);
}

void de_prof_lock_destroy_(de_prof_lock *l) {
    const de__prof_backend *be = de__prof_be();
    void *ctx = de__prof_lock_ctx(l, be);
    if (ctx != NULL)
        be->lock_terminate(ctx);
    memset(l, 0, sizeof *l);
}

bool de_prof_lock_before_(de_prof_lock *l) {
    const de__prof_backend *be = de__prof_be();
    void *ctx = de__prof_lock_ctx(l, be);
    return ctx != NULL && be->lock_before(ctx);
}

void de_prof_lock_after_(de_prof_lock *l) {
    const de__prof_backend *be = de__prof_be();
    void *ctx = de__prof_lock_ctx(l, be);
    if (ctx != NULL)
        be->lock_after(ctx);
}

void de_prof_lock_after_try_(de_prof_lock *l, bool acquired) {
    const de__prof_backend *be = de__prof_be();
    void *ctx = de__prof_lock_ctx(l, be);
    if (ctx != NULL)
        be->lock_after_try(ctx, acquired);
}

void de_prof_lock_after_unlock_(de_prof_lock *l) {
    const de__prof_backend *be = de__prof_be();
    void *ctx = de__prof_lock_ctx(l, be);
    if (ctx != NULL)
        be->lock_after_unlock(ctx);
}

#else /* !DE_PROFILE */

de_result de_prof_init(void) {
    DE_FAIL(DE_ERR_UNSUPPORTED, "de_prof_init: profiling is compiled out (no DE_PROFILE)");
    return DE_ERR_UNSUPPORTED;
}

void de_prof_shutdown(void) {}

bool de_prof_running(void) {
    return false;
}

bool de_prof_connected(void) {
    return false;
}

#endif /* DE_PROFILE */
