/* Unit tests: the profiler front end (src/core/prof/prof.c, DENG-94) on a mock backend.
 *
 * This executable compiles its own copy of prof.c with DE_PROF_MOCK, which makes it call
 * de__prof_mock_backend below instead of Tracy; its prof.o comes first, so the linker never takes
 * de_core's copy. The mock records every call (from any thread: the log thread, worker threads),
 * and the tests check the calls of the thread they care about, in order.
 *
 * Without DE_PROFILE (Debug, Shipping, Release without DE_ENABLE_PROFILE) only the compile-out
 * checks in test_prof_off.c (in test_core) apply, and this file has one test: de_prof_init is
 * DE_ERR_UNSUPPORTED. */
#include "de_test.h"

#include <dengine/de_base.h>
#include <dengine/de_debug.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "core/prof/prof_internal.h"
#include "platform/de_atomic.h"

#include <string.h>

#ifdef DE_PROFILE

/* ---- Mock backend -------------------------------------------------------------------------- */

typedef enum ev_kind {
    EV_STARTUP,
    EV_SHUTDOWN,
    EV_ZONE_BEGIN,
    EV_ZONE_END,
    EV_FRAME,
    EV_PLOT,
    EV_ALLOC,
    EV_FREE,
    EV_MESSAGE,
    EV_THREAD_NAME,
    EV_LOCK_ANNOUNCE,
    EV_LOCK_TERMINATE,
    EV_LOCK_BEFORE,
    EV_LOCK_AFTER,
    EV_LOCK_AFTER_TRY,
    EV_LOCK_AFTER_UNLOCK,
} ev_kind;

typedef struct ev {
    ev_kind kind;
    u32 thread;
    const void *ptr;  /* zone loc, block, lock ctx */
    const char *name; /* zone/frame/plot/pool name pointer */
    u64 id;           /* zone id */
    size_t size;
    f64 value;
    u32 color;
    int sev;
    bool flag; /* try-lock result */
    char text[200];
} ev;

#define MOCK_MAX 4096

static struct {
    de_atomic_u32 lock;
    u32 count;
    bool overflow;
    u64 next_zone;
    u64 next_lock;
    bool connected;
    ev evs[MOCK_MAX];
} mock;

static void mock_lock(void) {
    u32 expected = 0;
    while (
        !de_atomic_cas_weak_u32(&mock.lock, &expected, 1, DE_ATOMIC_ACQUIRE, DE_ATOMIC_RELAXED)) {
        expected = 0;
        de_cpu_pause();
    }
}

static void mock_unlock(void) {
    de_atomic_store_u32(&mock.lock, 0, DE_ATOMIC_RELEASE);
}

/* Appends a copy of `e`, stamped with the calling thread. */
static void mock_push(const ev *e) {
    mock_lock();
    if (mock.count < MOCK_MAX) {
        mock.evs[mock.count] = *e;
        mock.evs[mock.count].thread = de_thread_current_id();
        mock.count++;
    } else {
        mock.overflow = true;
    }
    mock_unlock();
}

static ev ev_make(ev_kind kind) {
    ev e;
    memset(&e, 0, sizeof e);
    e.kind = kind;
    return e;
}

static void mock_reset(void) {
    mock_lock();
    mock.count = 0;
    mock.overflow = false;
    mock.connected = false;
    mock_unlock();
}

static void m_startup(void) {
    ev e = ev_make(EV_STARTUP);
    mock_push(&e);
}
static void m_shutdown(void) {
    ev e = ev_make(EV_SHUTDOWN);
    mock_push(&e);
}
static bool m_connected(void) {
    return mock.connected;
}
static de_prof_zone m_zone_begin(const de_prof_srcloc *loc) {
    ev e = ev_make(EV_ZONE_BEGIN);
    mock_lock();
    e.id = ++mock.next_zone;
    mock_unlock();
    e.ptr = loc;
    e.name = loc->name;
    mock_push(&e);
    de_prof_zone z = {{e.id, 0xABCDu}};
    return z;
}
static void m_zone_end(de_prof_zone z) {
    ev e = ev_make(EV_ZONE_END);
    e.id = z.opaque[0];
    e.size = (size_t)z.opaque[1];
    mock_push(&e);
}
static void m_frame_mark(const char *name) {
    ev e = ev_make(EV_FRAME);
    e.name = name;
    mock_push(&e);
}
static void m_plot(const char *name, f64 value) {
    ev e = ev_make(EV_PLOT);
    e.name = name;
    e.value = value;
    mock_push(&e);
}
static void m_alloc(const void *ptr, size_t size, const char *pool) {
    ev e = ev_make(EV_ALLOC);
    e.ptr = ptr;
    e.size = size;
    e.name = pool;
    mock_push(&e);
}
static void m_free(const void *ptr, const char *pool) {
    ev e = ev_make(EV_FREE);
    e.ptr = ptr;
    e.name = pool;
    mock_push(&e);
}
static void m_message(const char *text, size_t len, de__prof_severity sev, u32 color) {
    ev e = ev_make(EV_MESSAGE);
    size_t n = len < sizeof e.text - 1 ? len : sizeof e.text - 1;
    memcpy(e.text, text, n);
    e.text[n] = '\0';
    e.size = len;
    e.sev = (int)sev;
    e.color = color;
    mock_push(&e);
}
static void m_thread_name(const char *name) {
    ev e = ev_make(EV_THREAD_NAME);
    size_t n = strlen(name);
    if (n > sizeof e.text - 1)
        n = sizeof e.text - 1;
    memcpy(e.text, name, n);
    e.text[n] = '\0';
    mock_push(&e);
}
static void *m_lock_announce(const de_prof_srcloc *loc) {
    ev e = ev_make(EV_LOCK_ANNOUNCE);
    mock_lock();
    void *ctx = (void *)(uintptr_t)(0x1000u + 16u * (++mock.next_lock));
    mock_unlock();
    e.ptr = ctx;
    e.name = loc->name;
    mock_push(&e);
    return ctx;
}
static void m_lock_terminate(void *ctx) {
    ev e = ev_make(EV_LOCK_TERMINATE);
    e.ptr = ctx;
    mock_push(&e);
}
static bool m_lock_before(void *ctx) {
    ev e = ev_make(EV_LOCK_BEFORE);
    e.ptr = ctx;
    mock_push(&e);
    return true;
}
static void m_lock_after(void *ctx) {
    ev e = ev_make(EV_LOCK_AFTER);
    e.ptr = ctx;
    mock_push(&e);
}
static void m_lock_after_try(void *ctx, bool acquired) {
    ev e = ev_make(EV_LOCK_AFTER_TRY);
    e.ptr = ctx;
    e.flag = acquired;
    mock_push(&e);
}
static void m_lock_after_unlock(void *ctx) {
    ev e = ev_make(EV_LOCK_AFTER_UNLOCK);
    e.ptr = ctx;
    mock_push(&e);
}

const de__prof_backend de__prof_mock_backend = {
    "mock",          m_startup,        m_shutdown,
    m_connected,     m_zone_begin,     m_zone_end,
    m_frame_mark,    m_plot,           m_alloc,
    m_free,          m_message,        m_thread_name,
    m_lock_announce, m_lock_terminate, m_lock_before,
    m_lock_after,    m_lock_after_try, m_lock_after_unlock,
};

/* Copies the events of `thread` (0: every thread) whose kind is in [first, last] into `out`. */
static u32 mock_events(u32 thread, ev_kind first, ev_kind last, ev *out, u32 cap) {
    u32 n = 0;
    mock_lock();
    for (u32 i = 0; i < mock.count && n < cap; ++i) {
        const ev *e = &mock.evs[i];
        if ((thread == 0 || e->thread == thread) && e->kind >= first && e->kind <= last)
            out[n++] = *e;
    }
    mock_unlock();
    return n;
}

static u32 mock_count(ev_kind kind) {
    u32 n = 0;
    mock_lock();
    for (u32 i = 0; i < mock.count; ++i)
        n += mock.evs[i].kind == kind;
    mock_unlock();
    return n;
}

static ev g_evs[MOCK_MAX];

/* Copies the lock events of context `ctx` into `out`, in order. Other mutexes (the heap's, the
 * log's) report on the same thread, so the tests look at one context. */
static u32 lock_events(const void *ctx, ev *out, u32 cap) {
    u32 n = 0;
    mock_lock();
    for (u32 i = 0; i < mock.count && n < cap; ++i) {
        const ev *e = &mock.evs[i];
        if (e->kind >= EV_LOCK_ANNOUNCE && e->kind <= EV_LOCK_AFTER_UNLOCK && e->ptr == ctx)
            out[n++] = *e;
    }
    mock_unlock();
    return n;
}

/* The profiler lock context de_mutex_init gave `m` (de_mutex.prof_ is a de_prof_lock). */
static const void *mutex_ctx(const de_mutex *m) {
    de_prof_lock l;
    memcpy(&l, m->prof_, sizeof l);
    return l.ctx;
}

/* ---- Lifetime ------------------------------------------------------------------------------ */

TEST(prof, not_running_is_noop) {
    mock_reset();
    EXPECT_FALSE(de_prof_running());
    EXPECT_FALSE(de_prof_connected());
    DE_PROFILE_ZONE_BEGIN(z, "before.init");
    DE_PROFILE_PLOT("before.init", 1);
    DE_PROFILE_FRAME(NULL);
    DE_PROFILE_ALLOC(&z, 8, "pool");
    DE_PROFILE_FREE(&z, "pool");
    DE_PROFILE_ZONE_END(z);
    de_prof_thread_name_("nobody");
    de_prof_shutdown(); /* not running: nothing */
    EXPECT_EQ(mock.count, 0u);
    EXPECT_EQ(z.opaque[0], 0u);
    EXPECT_EQ(z.opaque[1], 0u);
}

TEST(prof, init_shutdown_restart) {
    mock_reset();
    ASSERT_EQ(de_prof_init(), DE_OK);
    EXPECT_TRUE(de_prof_running());
    EXPECT_EQ(de_prof_init(), DE_OK); /* already running: nothing more */
    EXPECT_EQ(mock_count(EV_STARTUP), 1u);
    EXPECT_FALSE(de_prof_connected());
    mock.connected = true;
    EXPECT_TRUE(de_prof_connected());
    de_prof_shutdown();
    EXPECT_FALSE(de_prof_running());
    EXPECT_FALSE(de_prof_connected());
    de_prof_shutdown();
    EXPECT_EQ(mock_count(EV_SHUTDOWN), 1u);

    /* A new span works the same, and nothing is sent after the last shutdown. */
    ASSERT_EQ(de_prof_init(), DE_OK);
    DE_PROFILE_FRAME(NULL);
    de_prof_shutdown();
    DE_PROFILE_FRAME(NULL);
    EXPECT_EQ(mock_count(EV_STARTUP), 2u);
    EXPECT_EQ(mock_count(EV_SHUTDOWN), 2u);
    EXPECT_EQ(mock_count(EV_FRAME), 1u);
    EXPECT_EQ(mock.evs[0].kind, EV_STARTUP);
    EXPECT_EQ(mock.evs[mock.count - 1].kind, EV_SHUTDOWN);
}

/* ---- Zones, frames, plots ------------------------------------------------------------------ */

static const char *g_outer_name = "prof.outer";

TEST(prof, zone_nesting_mock) {
    mock_reset();
    ASSERT_EQ(de_prof_init(), DE_OK);
    const u32 me = de_thread_current_id();

    DE_PROFILE_ZONE_BEGIN(outer, "prof.outer");
    DE_PROFILE_ZONE_BEGIN_C(inner, "prof.inner", 0x00FF00);
    DE_PROFILE_PLOT("prof.depth", 2);
    DE_PROFILE_ZONE_END(inner);
    DE_PROFILE_FRAME(NULL);
    DE_PROFILE_ZONE_END(outer);
    DE_PROFILE_FRAME("prof.secondary");
    DE_PROFILE_PLOT("prof.value", 2.5f);
    de_prof_shutdown();

    const u32 n = mock_events(me, EV_ZONE_BEGIN, EV_PLOT, g_evs, MOCK_MAX);
    ASSERT_EQ(n, 8u);
    EXPECT_EQ(g_evs[0].kind, EV_ZONE_BEGIN);
    EXPECT_STREQ(g_evs[0].name, g_outer_name);
    EXPECT_EQ(g_evs[1].kind, EV_ZONE_BEGIN);
    EXPECT_STREQ(g_evs[1].name, "prof.inner");
    EXPECT_EQ(g_evs[2].kind, EV_PLOT);
    EXPECT_STREQ(g_evs[2].name, "prof.depth");
    EXPECT_FLOAT_EQ(g_evs[2].value, 2.0, 0.0);
    EXPECT_EQ(g_evs[3].kind, EV_ZONE_END);
    EXPECT_EQ(g_evs[3].id, g_evs[1].id); /* inner closes first */
    EXPECT_EQ(g_evs[3].size, 0xABCDu);   /* the zone value comes back unchanged */
    EXPECT_EQ(g_evs[4].kind, EV_FRAME);
    EXPECT_TRUE(g_evs[4].name == NULL);
    EXPECT_EQ(g_evs[5].kind, EV_ZONE_END);
    EXPECT_EQ(g_evs[5].id, g_evs[0].id);
    EXPECT_EQ(g_evs[6].kind, EV_FRAME);
    EXPECT_STREQ(g_evs[6].name, "prof.secondary");
    EXPECT_EQ(g_evs[7].kind, EV_PLOT);
    EXPECT_FLOAT_EQ(g_evs[7].value, 2.5, 0.0);

    /* The macro's static srcloc carries the call site, and the colour. */
    const de_prof_srcloc *oloc = (const de_prof_srcloc *)g_evs[0].ptr;
    const de_prof_srcloc *iloc = (const de_prof_srcloc *)g_evs[1].ptr;
    EXPECT_TRUE(strstr(oloc->function, "zone_nesting_mock") != NULL); /* the TEST function */
    EXPECT_TRUE(strstr(oloc->file, "test_prof.c") != NULL);
    EXPECT_GT(oloc->line, 0u);
    EXPECT_EQ(iloc->line, oloc->line + 1u);
    EXPECT_EQ(oloc->color, 0u);
    EXPECT_EQ(iloc->color, 0x00FF00u);
}

/* A zone that began before init is ended harmlessly after it (and not sent). */
TEST(prof, zone_across_init_is_inactive) {
    mock_reset();
    DE_PROFILE_ZONE_BEGIN(early, "prof.early");
    ASSERT_EQ(de_prof_init(), DE_OK);
    const de_prof_zone copy = early;
    DE_PROFILE_ZONE_END(early);
    de_prof_shutdown();
    EXPECT_EQ(mock_count(EV_ZONE_BEGIN), 0u);
    /* The mock is called with the inactive (zero) zone; Tracy ignores an inactive context. */
    EXPECT_EQ(copy.opaque[0], 0u);
}

/* ---- Memory hooks -------------------------------------------------------------------------- */

static de_atomic_u32 g_prev_allocs, g_prev_frees;

static void prev_on_alloc(void *user, void *ptr, size_t size, u32 tag) {
    (void)user;
    (void)ptr;
    (void)size;
    (void)tag;
    de_atomic_fetch_add_u32(&g_prev_allocs, 1, DE_ATOMIC_RELAXED);
}

static void prev_on_free(void *user, void *ptr, size_t size, u32 tag) {
    (void)user;
    (void)ptr;
    (void)size;
    (void)tag;
    de_atomic_fetch_add_u32(&g_prev_frees, 1, DE_ATOMIC_RELAXED);
}

/* Finds the alloc/free events of block `p` on this thread. */
static u32 block_events(const void *p, ev *out, u32 cap) {
    const u32 n = mock_events(de_thread_current_id(), EV_ALLOC, EV_FREE, g_evs, MOCK_MAX);
    u32 k = 0;
    for (u32 i = 0; i < n && k < cap; ++i)
        if (g_evs[i].ptr == p)
            out[k++] = g_evs[i];
    return k;
}

TEST(prof, alloc_hook_mock) {
    mock_reset();
    static const de_mem_hooks prev = {prev_on_alloc, prev_on_free, NULL};
    ASSERT_TRUE(de_mem_set_hooks(&prev) == NULL);
    ASSERT_EQ(de_prof_init(), DE_OK);

    const u32 tag = de_mem_register_tag("prof.test");
    ASSERT_NE(tag, (u32)DE_MEM_GENERAL);
    const char *tag_name = de_mem_tag_name(tag);
    de_tracking_allocator storage;
    const de_allocator a = de_tracking_allocator_init(&storage, NULL, tag);

    void *p = DE_ALLOC(&a, 100, 16, tag);
    ASSERT_TRUE(p != NULL);
    void *q = DE_REALLOC(&a, p, 100, 4000, 16, tag);
    ASSERT_TRUE(q != NULL);
    DE_FREE(&a, q, 4000);

    ev evs[8];
    u32 n = block_events(p, evs, 8);
    ASSERT_GE(n, 2u);
    EXPECT_EQ(evs[0].kind, EV_ALLOC);
    EXPECT_EQ(evs[0].size, 100u);
    EXPECT_TRUE(evs[0].name == tag_name); /* the tag name is the pool, by pointer */
    EXPECT_STREQ(evs[0].name, "prof.test");
    EXPECT_EQ(evs[1].kind, EV_FREE); /* realloc: the old block is freed first */
    EXPECT_TRUE(evs[1].name == tag_name);
    if (q != p) {
        u32 m = block_events(q, evs, 8);
        ASSERT_EQ(m, 2u);
        EXPECT_EQ(evs[0].kind, EV_ALLOC);
        EXPECT_EQ(evs[0].size, 4000u);
        EXPECT_EQ(evs[1].kind, EV_FREE);
    } else {
        ASSERT_EQ(n, 4u);
        EXPECT_EQ(evs[2].kind, EV_ALLOC);
        EXPECT_EQ(evs[2].size, 4000u);
        EXPECT_EQ(evs[3].kind, EV_FREE);
    }
    EXPECT_TRUE(evs[0].name == tag_name);

    /* The hooks installed before the profiler still see everything (chained). */
    EXPECT_GE(de_atomic_load_u32(&g_prev_allocs, DE_ATOMIC_RELAXED), 2u);
    EXPECT_GE(de_atomic_load_u32(&g_prev_frees, DE_ATOMIC_RELAXED), 2u);

    /* Shutdown puts the earlier hooks back, and nothing more reaches the backend. */
    de_prof_shutdown();
    const u32 sent = mock_count(EV_ALLOC);
    void *r = DE_ALLOC(&a, 64, 16, tag);
    DE_FREE(&a, r, 64);
    EXPECT_EQ(mock_count(EV_ALLOC), sent);
    EXPECT_TRUE(de_mem_set_hooks(NULL) == &prev);

    /* Memory outside the engine allocators, through the macros. */
    ASSERT_EQ(de_prof_init(), DE_OK);
    static int gpu_block;
    DE_PROFILE_ALLOC(&gpu_block, 256, "prof.gpu");
    DE_PROFILE_FREE(&gpu_block, "prof.gpu");
    de_prof_shutdown();
    n = block_events(&gpu_block, evs, 8);
    ASSERT_EQ(n, 2u);
    EXPECT_EQ(evs[0].size, 256u);
    EXPECT_STREQ(evs[0].name, "prof.gpu");
    EXPECT_STREQ(evs[1].name, "prof.gpu");
    EXPECT_TRUE(de_mem_set_hooks(NULL) == NULL);
}

/* ---- Log sink ------------------------------------------------------------------------------ */

TEST(prof, log_sink_mock) {
    mock_reset();
    de_log_desc desc;
    memset(&desc, 0, sizeof desc);
    desc.no_console_sink = true;
    ASSERT_EQ(de_log_init(&desc), DE_OK);
    ASSERT_EQ(de_prof_init(), DE_OK);

    DE_INFO(DE_LOG_CAT_CORE, "info is not sent %d", 1);
    DE_WARN(DE_LOG_CAT_CORE, "warn %d", 2);
    DE_ERROR(DE_LOG_CAT_PLATFORM, "error %s", "three");
    de_log_flush();

    const u32 n = mock_events(0, EV_MESSAGE, EV_MESSAGE, g_evs, MOCK_MAX);
    ASSERT_EQ(n, 2u);
    EXPECT_STREQ(g_evs[0].text, "core: warn 2");
    EXPECT_EQ(g_evs[0].sev, (int)DE__PROF_SEV_WARN);
    EXPECT_EQ(g_evs[0].color, DE__PROF_COLOR_WARN);
    EXPECT_EQ(g_evs[0].size, strlen("core: warn 2")); /* the length excludes any NUL */
    EXPECT_STREQ(g_evs[1].text, "platform: error three");
    EXPECT_EQ(g_evs[1].sev, (int)DE__PROF_SEV_ERROR);
    EXPECT_EQ(g_evs[1].color, DE__PROF_COLOR_ERROR);

    /* After shutdown the sink is gone. */
    de_prof_shutdown();
    DE_WARN(DE_LOG_CAT_CORE, "not sent");
    de_log_flush();
    EXPECT_EQ(mock_count(EV_MESSAGE), 2u);
    de_log_shutdown();
}

/* ---- Locks and thread names ---------------------------------------------------------------- */

TEST(prof, mutex_lock_events_mock) {
    mock_reset();
    de_mutex before; /* initialized before the profiler: never profiled */
    de_mutex_init(&before);
    ASSERT_EQ(de_prof_init(), DE_OK);
    const u32 me = de_thread_current_id();

    de_mutex m;
    de_mutex_init(&m);
    const void *ctx = mutex_ctx(&m);
    ASSERT_TRUE(ctx != NULL);
    EXPECT_TRUE(mutex_ctx(&before) == NULL);
    de_mutex_lock(&m);
    de_mutex_unlock(&m);
    EXPECT_TRUE(de_mutex_try_lock(&m));
    de_mutex_unlock(&m);
    de_cond c;
    de_cond_init(&c);
    de_mutex_lock(&m);
    EXPECT_FALSE(de_cond_wait_timeout(&c, &m, 1000000ull)); /* 1 ms, nobody signals */
    de_mutex_unlock(&m);
    de_cond_destroy(&c);
    de_mutex_lock(&before);
    de_mutex_unlock(&before);
    de_mutex_destroy(&before);
    de_mutex_destroy(&m);
    de_prof_shutdown();

    const u32 n = lock_events(ctx, g_evs, MOCK_MAX);
    static const ev_kind want[] = {
        EV_LOCK_ANNOUNCE,                                    /* init */
        EV_LOCK_BEFORE,       EV_LOCK_AFTER,                 /* lock */
        EV_LOCK_AFTER_UNLOCK,                                /* unlock */
        EV_LOCK_AFTER_TRY,                                   /* try_lock */
        EV_LOCK_AFTER_UNLOCK,                                /* unlock */
        EV_LOCK_BEFORE,       EV_LOCK_AFTER,                 /* lock */
        EV_LOCK_AFTER_UNLOCK, EV_LOCK_BEFORE, EV_LOCK_AFTER, /* cond wait: release, retake */
        EV_LOCK_AFTER_UNLOCK,                                /* unlock */
        EV_LOCK_TERMINATE,                                   /* destroy */
    };
    const u32 want_n = (u32)(sizeof want / sizeof want[0]);
    ASSERT_EQ(n, want_n);
    for (u32 i = 0; i < n; ++i) {
        EXPECT_EQ(g_evs[i].kind, want[i]);
        EXPECT_EQ(g_evs[i].thread, me);
    }
    EXPECT_STREQ(g_evs[0].name, "de_mutex");
    EXPECT_TRUE(g_evs[4].flag);
}

/* A mutex announced in one span is ignored in the next: its context is gone. */
TEST(prof, mutex_from_earlier_span_is_ignored) {
    mock_reset();
    ASSERT_EQ(de_prof_init(), DE_OK);
    de_mutex m;
    de_mutex_init(&m);
    const void *ctx = mutex_ctx(&m);
    ASSERT_TRUE(ctx != NULL);
    de_prof_shutdown();
    ASSERT_EQ(de_prof_init(), DE_OK);
    de_mutex_lock(&m);
    de_mutex_unlock(&m);
    de_mutex_destroy(&m);
    de_prof_shutdown();
    const u32 n = lock_events(ctx, g_evs, MOCK_MAX);
    ASSERT_EQ(n, 1u); /* the announce only: no lock events, no terminate */
    EXPECT_EQ(g_evs[0].kind, EV_LOCK_ANNOUNCE);
}

static i32 named_thread(void *user) {
    *(u32 *)user = de_thread_current_id();
    return 0;
}

TEST(prof, thread_names_mock) {
    mock_reset();
    ASSERT_EQ(de_prof_init(), DE_OK);
    de_thread_set_name("prof-main");
    u32 tid = 0;
    de_thread *t = NULL;
    ASSERT_EQ(de_thread_create(named_thread, &tid, "prof-worker", DE_THREAD_NORMAL, 0, &t), DE_OK);
    EXPECT_EQ(de_thread_join(t), 0);
    de_prof_shutdown();

    u32 n = mock_events(de_thread_current_id(), EV_THREAD_NAME, EV_THREAD_NAME, g_evs, MOCK_MAX);
    ASSERT_EQ(n, 1u);
    EXPECT_STREQ(g_evs[0].text, "prof-main");
    ASSERT_NE(tid, 0u);
    n = mock_events(tid, EV_THREAD_NAME, EV_THREAD_NAME, g_evs, MOCK_MAX);
    ASSERT_EQ(n, 1u); /* named from inside the new thread, before its entry point */
    EXPECT_STREQ(g_evs[0].text, "prof-worker");
}

#else /* !DE_PROFILE */

TEST(prof, init_unsupported_without_profile) {
    EXPECT_EQ(de_prof_init(), DE_ERR_UNSUPPORTED);
    EXPECT_FALSE(de_prof_running());
    de_prof_shutdown();
}

#endif /* DE_PROFILE */
