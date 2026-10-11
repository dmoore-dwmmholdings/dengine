/* Unit tests: log categories and levels, sinks, per-thread buffers, the log thread, flush, the
 * stderr fallback and the fatal path (DENG-150).
 *
 * de_test.h checks may only run on the test's thread. Sinks run on the log thread (or inside
 * de_log_flush), so they only record; the test checks the records after de_log_flush() or
 * de_log_shutdown(), which synchronize with the drain through the drain lock or the join. */
#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS /* fopen, sscanf */
#elif defined(__APPLE__)
#define _DARWIN_C_SOURCE
#else
#define _POSIX_C_SOURCE 200809L /* dup, fileno */
#endif

#include "de_test.h"

#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "core/log_internal.h"
#include "platform/de_atomic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#define de_dup    _dup
#define de_dup2   _dup2
#define de_close  _close
#define de_fileno _fileno
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#define de_dup    dup
#define de_dup2   dup2
#define de_close  close
#define de_fileno fileno
#endif

#define NS_PER_MS 1000000ull

/* ---- Capture sink ---------------------------------------------------------------------------- */

typedef struct cap_rec {
    de_log_level level;
    u32 category;
    u64 timestamp_ns;
    u32 thread_id;
    u32 line;
    u32 batch;
    char msg[120];
} cap_rec;

typedef struct capture {
    cap_rec *recs;
    u32 cap;
    u32 count;    /* records stored */
    u32 calls;    /* sink calls, including records that did not fit */
    u32 batch;    /* batch-end calls so far */
    u32 overflow; /* records that did not fit */
} capture;

static void capture_sink(const de_log_record *rec, void *user) {
    capture *c = (capture *)user;
    c->calls++;
    if (c->count >= c->cap) {
        c->overflow++;
        return;
    }
    cap_rec *r = &c->recs[c->count++];
    r->level = rec->level;
    r->category = rec->category.id;
    r->timestamp_ns = rec->timestamp_ns;
    r->thread_id = rec->thread_id;
    r->line = rec->line;
    r->batch = c->batch;
    size_t n = rec->message.len < sizeof r->msg - 1 ? rec->message.len : sizeof r->msg - 1;
    memcpy(r->msg, rec->message.ptr, n);
    r->msg[n] = '\0';
}

static void capture_batch_end(void *user) {
    ((capture *)user)->batch++;
}

static void capture_init(capture *c, u32 cap) {
    memset(c, 0, sizeof *c);
    c->recs = (cap_rec *)calloc(cap, sizeof(cap_rec));
    ASSERT_TRUE(c->recs != NULL);
    c->cap = cap;
}

static void capture_free(capture *c) {
    free(c->recs);
    c->recs = NULL;
}

/* Starts logging without the console sink and adds `c` (with batch borders). */
static void start_capture(capture *c, u32 ring_bytes, u32 interval_ms) {
    de_log_desc d = {0};
    d.no_console_sink = true;
    d.thread_buffer_bytes = ring_bytes;
    d.drain_interval_ms = interval_ms;
    ASSERT_EQ(de_log_init(&d), DE_OK);
    ASSERT_TRUE(de__log_add_sink_ex(capture_sink, capture_batch_end, c));
}

static u32 count_containing(const capture *c, const char *needle) {
    u32 n = 0;
    for (u32 i = 0; i < c->count; i++)
        if (strstr(c->recs[i].msg, needle) != NULL)
            n++;
    return n;
}

/* Logging is process-global: every test leaves it shut down, even after a failed check. */
typedef struct log_fx {
    capture cap;
} log_fx;
static void log_fx_setup(log_fx *f) {
    (void)f;
    de_log_shutdown();
    de_log_set_level(DE_LOG_CAT_GENERAL, DE_LOG_INFO);
    de_log_set_level(DE_LOG_CAT_CORE, DE_LOG_INFO);
    de_log_set_level(DE_LOG_CAT_PLATFORM, DE_LOG_INFO);
}
static void log_fx_teardown(log_fx *f) {
    de_log_shutdown();
    capture_free(&f->cap);
    de_log_set_level(DE_LOG_CAT_CORE, DE_LOG_INFO);
}

/* ---- Categories and levels ---------------------------------------------------------------- */

TEST_F(log, category_levels, log_fx) {
    /* Pre-registered categories. */
    EXPECT_STREQ(de_log_category_name(DE_LOG_CAT_GENERAL), "general");
    EXPECT_STREQ(de_log_category_name((de_log_category){0}), "general");
    EXPECT_STREQ(de_log_category_name(DE_LOG_CAT_CORE), "core");
    EXPECT_STREQ(de_log_category_name(DE_LOG_CAT_PLATFORM), "platform");
    EXPECT_EQ(de_log_get_level(DE_LOG_CAT_CORE), DE_LOG_INFO);
    EXPECT_EQ(de_log_register_category("core", DE_LOG_ERROR).id, 1u);
    EXPECT_EQ(de_log_get_level(DE_LOG_CAT_CORE), DE_LOG_INFO); /* existing: level untouched */

    /* New categories get new ids; registering again returns the same one. */
    de_log_category render = de_log_register_category("test_render", DE_LOG_WARN);
    de_log_category audio = de_log_register_category("test_audio", DE_LOG_TRACE);
    EXPECT_GE(render.id, 3u);
    EXPECT_NE(render.id, audio.id);
    EXPECT_EQ(de_log_register_category("test_render", DE_LOG_TRACE).id, render.id);
    EXPECT_STREQ(de_log_category_name(render), "test_render");
    EXPECT_EQ(de_log_get_level(render), DE_LOG_WARN);
    EXPECT_EQ(de_log_register_category(NULL, DE_LOG_INFO).id, 0u);
    EXPECT_EQ(de_log_register_category("", DE_LOG_INFO).id, 0u);

    /* Names are cut to DE_LOG_CATEGORY_NAME_MAX - 1 bytes, and the cut name matches itself. */
    char long_name[100];
    memset(long_name, 'x', sizeof long_name - 1);
    long_name[sizeof long_name - 1] = '\0';
    de_log_category lc = de_log_register_category(long_name, DE_LOG_INFO);
    EXPECT_EQ(strlen(de_log_category_name(lc)), (size_t)DE_LOG_CATEGORY_NAME_MAX - 1);
    EXPECT_EQ(de_log_register_category(long_name, DE_LOG_INFO).id, lc.id);

    /* Each category filters on its own level. */
    EXPECT_FALSE(de_log_enabled(DE_LOG_INFO, render));
    EXPECT_TRUE(de_log_enabled(DE_LOG_WARN, render));
    EXPECT_TRUE(de_log_enabled(DE_LOG_TRACE, audio));
    EXPECT_FALSE(de_log_enabled(DE_LOG_DEBUG, DE_LOG_CAT_GENERAL));
    de_log_set_level(render, DE_LOG_ERROR);
    EXPECT_EQ(de_log_get_level(render), DE_LOG_ERROR);
    EXPECT_FALSE(de_log_enabled(DE_LOG_WARN, render));
    EXPECT_TRUE(de_log_enabled(DE_LOG_FATAL, render));

    /* An unregistered id behaves like `general`. */
    de_log_category bogus = {DE_LOG_MAX_CATEGORIES + 5};
    EXPECT_STREQ(de_log_category_name(bogus), "general");
    EXPECT_EQ(de_log_get_level(bogus), DE_LOG_INFO);

    EXPECT_STREQ(de_log_level_name(DE_LOG_TRACE), "trace");
    EXPECT_STREQ(de_log_level_name(DE_LOG_FATAL), "fatal");
    EXPECT_STREQ(de_log_level_name((de_log_level)42), "?");

    /* Delivery honours the levels. */
    capture_init(&f->cap, 64);
    start_capture(&f->cap, 0, 0);
    DE_INFO(render, "render info");   /* below Error */
    DE_ERROR(render, "render error"); /* delivered */
    DE_LOG(DE_LOG_TRACE, audio, "audio trace");
    DE_INFO(bogus, "bogus info"); /* general, Info */
    de_log_flush();
    ASSERT_EQ(f->cap.count, 3u);
    EXPECT_STREQ(f->cap.recs[0].msg, "render error");
    EXPECT_EQ(f->cap.recs[0].category, render.id);
    EXPECT_EQ(f->cap.recs[0].level, DE_LOG_ERROR);
    EXPECT_STREQ(f->cap.recs[1].msg, "audio trace");
    EXPECT_STREQ(f->cap.recs[2].msg, "bogus info");
    EXPECT_EQ(f->cap.recs[2].category, 0u);
}

/* ---- Filter before format ----------------------------------------------------------------- */

static int side_effects;
static int side_effect(void) {
    return ++side_effects;
}

TEST_F(log, filter_before_format, log_fx) {
    capture_init(&f->cap, 64);
    start_capture(&f->cap, 0, 0);
    de_log_set_level(DE_LOG_CAT_CORE, DE_LOG_WARN);

    side_effects = 0;
    u64 formatted0 = de__log_get_counters().formatted;
    for (int i = 0; i < 100; i++) {
        DE_INFO(DE_LOG_CAT_CORE, "value %d", side_effect());
        DE_LOG(DE_LOG_DEBUG, DE_LOG_CAT_CORE, "value %d", side_effect());
        /* Called directly, de_log_ still checks the level before formatting. */
        de_log_(DE_LOG_INFO, DE_LOG_CAT_CORE, __FILE__, __LINE__, "direct %d", i);
    }
    EXPECT_EQ(side_effects, 0);
    EXPECT_EQ(de__log_get_counters().formatted, formatted0);
    de_log_flush();
    EXPECT_EQ(f->cap.calls, 0u);

    DE_WARN(DE_LOG_CAT_CORE, "value %d", side_effect());
    EXPECT_EQ(side_effects, 1);
    EXPECT_EQ(de__log_get_counters().formatted, formatted0 + 1);
    de_log_flush();
    ASSERT_EQ(f->cap.calls, 1u);
    EXPECT_STREQ(f->cap.recs[0].msg, "value 1");
}

/* ---- Multithreaded delivery --------------------------------------------------------------- */

#define MT_THREADS  8u
#define MT_MESSAGES 10000u

typedef struct mt_arg {
    u32 index;
    de_atomic_u32 *go;
} mt_arg;

static i32 mt_producer(void *user) {
    mt_arg *a = (mt_arg *)user;
    while (de_atomic_load_u32(a->go, DE_ATOMIC_ACQUIRE) == 0)
        de_cpu_pause();
    for (u32 i = 0; i < MT_MESSAGES; i++)
        DE_INFO(DE_LOG_CAT_CORE, "mt %u %u", (unsigned)a->index, (unsigned)i);
    return 0;
}

TEST_F(log, multithread_delivery, log_fx) {
    /* 4 MiB per thread holds all 10k records, so nothing may drop even if the log thread is
     * descheduled for the whole run. */
    capture_init(&f->cap, MT_THREADS * MT_MESSAGES + 64);
    start_capture(&f->cap, 4u << 20, 0);
    u64 dropped0 = de__log_get_counters().dropped;

    de_atomic_u32 go;
    de_atomic_init_u32(&go, 0);
    mt_arg args[MT_THREADS];
    de_thread *threads[MT_THREADS];
    for (u32 t = 0; t < MT_THREADS; t++) {
        args[t] = (mt_arg){t, &go};
        ASSERT_EQ(
            de_thread_create(mt_producer, &args[t], "log-mt", DE_THREAD_NORMAL, 0, &threads[t]),
            DE_OK);
    }
    de_atomic_store_u32(&go, 1, DE_ATOMIC_RELEASE);
    for (u32 t = 0; t < MT_THREADS; t++)
        (void)de_thread_join(threads[t]);
    de_log_shutdown(); /* final drain; joins the log thread */

    EXPECT_EQ(de__log_get_counters().dropped, dropped0);
    EXPECT_EQ(f->cap.overflow, 0u);
    ASSERT_EQ(f->cap.count, MT_THREADS * MT_MESSAGES);

    /* Exactly once, per-thread order kept, non-decreasing timestamps within a batch. */
    u32 next[MT_THREADS] = {0};
    u32 tid_of[MT_THREADS] = {0};
    u32 order_errors = 0, ts_errors = 0, parse_errors = 0, tid_errors = 0;
    for (u32 i = 0; i < f->cap.count; i++) {
        const cap_rec *r = &f->cap.recs[i];
        unsigned t = 0, s = 0;
        if (sscanf(r->msg, "mt %u %u", &t, &s) != 2 || t >= MT_THREADS) {
            parse_errors++;
            continue;
        }
        if (s != next[t])
            order_errors++;
        next[t] = s + 1;
        if (tid_of[t] == 0)
            tid_of[t] = r->thread_id;
        else if (tid_of[t] != r->thread_id)
            tid_errors++;
        if (i > 0 && f->cap.recs[i - 1].batch == r->batch &&
            f->cap.recs[i - 1].timestamp_ns > r->timestamp_ns)
            ts_errors++;
    }
    EXPECT_EQ(parse_errors, 0u);
    EXPECT_EQ(order_errors, 0u);
    EXPECT_EQ(ts_errors, 0u);
    EXPECT_EQ(tid_errors, 0u);
    for (u32 t = 0; t < MT_THREADS; t++)
        EXPECT_EQ(next[t], MT_MESSAGES);
}

/* ---- Ring full ---------------------------------------------------------------------------- */

typedef struct blocking_sink {
    de_atomic_u32 entered;
    de_sem release;
    capture *cap;
} blocking_sink;

/* Blocks the drain on the record "block" until the test posts `release`. */
static void blocking_sink_fn(const de_log_record *rec, void *user) {
    blocking_sink *b = (blocking_sink *)user;
    capture_sink(rec, b->cap);
    if (rec->message.len == 5 && memcmp(rec->message.ptr, "block", 5) == 0) {
        de_atomic_store_u32(&b->entered, 1, DE_ATOMIC_RELEASE);
        de_sem_wait(&b->release);
    }
}

TEST_F(log, ring_full_drops, log_fx) {
    enum { N = 2000 };
    capture_init(&f->cap, N + 64);
    blocking_sink b;
    de_atomic_init_u32(&b.entered, 0);
    ASSERT_EQ(de_sem_init(&b.release, 0), DE_OK);
    b.cap = &f->cap;

    de_log_desc d = {0};
    d.no_console_sink = true;
    d.thread_buffer_bytes = 4096; /* the smallest ring */
    d.drain_interval_ms = 1;
    ASSERT_EQ(de_log_init(&d), DE_OK);
    de_log_add_sink(blocking_sink_fn, &b);
    u64 dropped0 = de__log_get_counters().dropped;

    /* Park the log thread inside the sink, so nothing drains while the ring fills. */
    DE_INFO(DE_LOG_CAT_GENERAL, "block");
    u64 deadline = de_time_now_ns() + 10000ull * NS_PER_MS;
    while (de_atomic_load_u32(&b.entered, DE_ATOMIC_ACQUIRE) == 0 && de_time_now_ns() < deadline)
        de_cpu_pause();
    if (de_atomic_load_u32(&b.entered, DE_ATOMIC_ACQUIRE) == 0) {
        de_sem_post(&b.release, 1);
        FAIL("log thread never reached the sink");
    }

    /* The producer must not block: if it did, this loop would never end (the sink waits). */
    u64 t0 = de_time_now_ns();
    for (int i = 0; i < N; i++)
        DE_INFO(DE_LOG_CAT_GENERAL, "fill %d", i);
    u64 elapsed = de_time_now_ns() - t0;
    u64 dropped = de__log_get_counters().dropped - dropped0;

    de_sem_post(&b.release, 1);
    de_log_flush();
    de_log_shutdown();
    de_sem_destroy(&b.release);

    EXPECT_LT(elapsed, 2000ull * NS_PER_MS);
    EXPECT_GT(dropped, 0u);
    EXPECT_LT(dropped, (u64)N);

    /* Delivered + reported drops account for every message; the report is one Warn line. */
    u32 delivered = count_containing(&f->cap, "fill ");
    EXPECT_EQ((u64)delivered + dropped, (u64)N);
    u32 reports = 0;
    u64 reported = 0;
    for (u32 i = 0; i < f->cap.count; i++) {
        const cap_rec *r = &f->cap.recs[i];
        unsigned long long n = 0;
        if (sscanf(r->msg, "log: dropped %llu messages", &n) == 1) {
            reports++;
            reported += n;
            EXPECT_EQ(r->level, DE_LOG_WARN);
            EXPECT_EQ(r->category, DE_LOG_CAT_CORE.id);
        }
    }
    EXPECT_EQ(reports, 1u);
    EXPECT_EQ(reported, dropped);

    /* What was delivered kept its order. */
    int prev = -1, order_errors = 0;
    for (u32 i = 0; i < f->cap.count; i++) {
        int v = 0;
        if (sscanf(f->cap.recs[i].msg, "fill %d", &v) == 1) {
            if (v <= prev)
                order_errors++;
            prev = v;
        }
    }
    EXPECT_EQ(order_errors, 0);
}

/* ---- Flush -------------------------------------------------------------------------------- */

static i32 flush_producer(void *user) {
    (void)user;
    for (int i = 0; i < 50; i++)
        DE_INFO(DE_LOG_CAT_PLATFORM, "other %d", i);
    return 0;
}

TEST_F(log, flush_sync, log_fx) {
    de_log_flush(); /* before init: no-op */

    capture_init(&f->cap, 256);
    /* The log thread sleeps 60 s and 100 short records never half-fill 64 KiB, so only the
     * flush can deliver them. */
    start_capture(&f->cap, 0, 60000);
    for (int i = 0; i < 100; i++)
        DE_INFO(DE_LOG_CAT_CORE, "msg %d", i);
    de_log_flush();
    ASSERT_EQ(f->cap.count, 100u);
    EXPECT_STREQ(f->cap.recs[99].msg, "msg 99");

    /* Messages of another thread that finished before the call are flushed too. */
    de_thread *t = NULL;
    ASSERT_EQ(de_thread_create(flush_producer, NULL, "log-flush", DE_THREAD_NORMAL, 0, &t), DE_OK);
    (void)de_thread_join(t);
    de_log_flush();
    EXPECT_EQ(f->cap.count, 150u);
    EXPECT_EQ(count_containing(&f->cap, "other "), 50u);

    /* A second init is refused; shutdown then init again works on the same threads. */
    EXPECT_EQ(de_log_init(NULL), DE_ERR_BUSY);
    de_log_shutdown();
    capture c2;
    capture_init(&c2, 16);
    de_log_desc d = {0};
    d.no_console_sink = true;
    ASSERT_EQ(de_log_init(&d), DE_OK);
    de_log_add_sink(capture_sink, &c2);
    DE_INFO(DE_LOG_CAT_CORE, "second generation");
    de_log_flush();
    de_log_shutdown();
    EXPECT_EQ(c2.count, 1u);
    EXPECT_STREQ(c2.recs[0].msg, "second generation");
    capture_free(&c2);
}

/* ---- Allocation ----------------------------------------------------------------------------- */

typedef struct counting_alloc {
    de_atomic_u64 allocs, frees, live_bytes;
} counting_alloc;

static void *counting_alloc_fn(void *user, size_t size, size_t align, u32 tag) {
    (void)align;
    (void)tag;
    counting_alloc *c = (counting_alloc *)user;
    de_atomic_fetch_add_u64(&c->allocs, 1, DE_ATOMIC_RELAXED);
    de_atomic_fetch_add_u64(&c->live_bytes, size, DE_ATOMIC_RELAXED);
    return malloc(size);
}
static void *counting_realloc_fn(void *user, void *ptr, size_t old_size, size_t new_size,
                                 size_t align, u32 tag) {
    (void)user, (void)ptr, (void)old_size, (void)new_size, (void)align, (void)tag;
    return NULL; /* the log never reallocates */
}
static void counting_free_fn(void *user, void *ptr, size_t size) {
    counting_alloc *c = (counting_alloc *)user;
    de_atomic_fetch_add_u64(&c->frees, 1, DE_ATOMIC_RELAXED);
    de_atomic_fetch_sub_u64(&c->live_bytes, size, DE_ATOMIC_RELAXED);
    free(ptr);
}

typedef struct alloc_arg {
    counting_alloc *ca;
    u64 after_first, after_many;
} alloc_arg;

static i32 alloc_producer(void *user) {
    alloc_arg *a = (alloc_arg *)user;
    DE_INFO(DE_LOG_CAT_CORE, "first");
    a->after_first = de_atomic_load_u64(&a->ca->allocs, DE_ATOMIC_RELAXED);
    for (int i = 0; i < 1000; i++)
        DE_INFO(DE_LOG_CAT_CORE, "hot %d %s", i, "path");
    a->after_many = de_atomic_load_u64(&a->ca->allocs, DE_ATOMIC_RELAXED);
    return 0;
}

TEST_F(log, hot_path_no_alloc, log_fx) {
    counting_alloc ca;
    de_atomic_init_u64(&ca.allocs, 0);
    de_atomic_init_u64(&ca.frees, 0);
    de_atomic_init_u64(&ca.live_bytes, 0);
    de_allocator a = {counting_alloc_fn, counting_realloc_fn, counting_free_fn, &ca};

    capture_init(&f->cap, 4096);
    de_log_desc d = {0};
    d.allocator = &a;
    d.no_console_sink = true;
    d.thread_buffer_bytes = 256u << 10;
    ASSERT_EQ(de_log_init(&d), DE_OK);
    de_log_add_sink(capture_sink, &f->cap);

    /* Thread A: one allocation (its ring) on the first call, none after. */
    alloc_arg arg = {&ca, 0, 0};
    de_thread *t = NULL;
    ASSERT_EQ(de_thread_create(alloc_producer, &arg, "log-alloc", DE_THREAD_NORMAL, 0, &t), DE_OK);
    (void)de_thread_join(t);
    EXPECT_EQ(arg.after_first, 1u);
    EXPECT_EQ(arg.after_many, 1u);

    /* Thread A exited: after a drain its ring is free, and thread B reuses it. */
    de_log_flush();
    u64 recycled0 = de__log_get_counters().recycled;
    alloc_arg arg2 = {&ca, 0, 0};
    ASSERT_EQ(de_thread_create(alloc_producer, &arg2, "log-alloc", DE_THREAD_NORMAL, 0, &t), DE_OK);
    (void)de_thread_join(t);
    EXPECT_EQ(arg2.after_first, 1u);
    EXPECT_EQ(arg2.after_many, 1u);
    EXPECT_EQ(de__log_get_counters().recycled, recycled0 + 1);

    de_log_shutdown();
    EXPECT_EQ(f->cap.count, 2002u);
    /* Shutdown returned every buffer to the allocator that made it. */
    EXPECT_EQ(de_atomic_load_u64(&ca.frees, DE_ATOMIC_RELAXED),
              de_atomic_load_u64(&ca.allocs, DE_ATOMIC_RELAXED));
    EXPECT_EQ(de_atomic_load_u64(&ca.live_bytes, DE_ATOMIC_RELAXED), 0u);
}

/* A NULL de_log_desc.allocator means de_heap_allocator(): a new thread's buffer is a heap block,
 * which the heap reports to the hooks in DE_MEM_TRACKING builds. */
#if DE_MEM_TRACKING
typedef struct big_alloc_seen {
    de_atomic_u64 count;
    size_t min_size;
} big_alloc_seen;

static void big_alloc_hook(void *user, void *ptr, size_t size, u32 tag) {
    (void)ptr;
    (void)tag;
    big_alloc_seen *s = (big_alloc_seen *)user;
    if (size >= s->min_size)
        de_atomic_fetch_add_u64(&s->count, 1, DE_ATOMIC_RELAXED);
}

static i32 log_once(void *user) {
    (void)user;
    DE_INFO(DE_LOG_CAT_CORE, "from a new thread");
    return 0;
}
#endif

TEST_F(log, null_allocator_uses_heap, log_fx) {
#if !DE_MEM_TRACKING
    SKIP("the heap reports its blocks only in Debug and Dev builds");
#else
    capture_init(&f->cap, 16);
    de_log_desc d = {0};
    d.no_console_sink = true;
    d.thread_buffer_bytes = 64u << 10;
    ASSERT_EQ(de_log_init(&d), DE_OK);
    de_log_add_sink(capture_sink, &f->cap);

    big_alloc_seen seen;
    de_atomic_init_u64(&seen.count, 0);
    seen.min_size = 64u << 10;
    const de_mem_hooks hooks = {big_alloc_hook, NULL, &seen};
    const de_mem_hooks *prev = de_mem_set_hooks(&hooks);
    de_thread *t = NULL;
    de_result r = de_thread_create(log_once, NULL, "log-heap", DE_THREAD_NORMAL, 0, &t);
    if (r == DE_OK)
        (void)de_thread_join(t);
    de_mem_set_hooks(prev);
    ASSERT_EQ(r, DE_OK);
    EXPECT_EQ(de_atomic_load_u64(&seen.count, DE_ATOMIC_RELAXED), 1u);
    de_log_shutdown();
    EXPECT_EQ(f->cap.count, 1u);
#endif
}

/* ---- stderr before init ------------------------------------------------------------------ */

/* Runs fn with stderr (fd 2) redirected to a file, and returns what was written. */
static void capture_stderr(void (*fn)(void), char *out, size_t cap) {
    const char *path = "test_log_stderr.txt";
    FILE *tmp = fopen(path, "w+");
    ASSERT_TRUE(tmp != NULL);
    fflush(stderr);
    int saved = de_dup(2);
    ASSERT_TRUE(saved >= 0);
    ASSERT_EQ(de_dup2(de_fileno(tmp), 2), 2);
    fn();
    fflush(stderr);
    de_dup2(saved, 2);
    de_close(saved);
    rewind(tmp);
    size_t n = fread(out, 1, cap - 1, tmp);
    out[n] = '\0';
    fclose(tmp);
    remove(path);
}

static u32 pre_init_line;
static void pre_init_logging(void) {
    DE_LOG(DE_LOG_DEBUG, DE_LOG_CAT_CORE, "hidden"); /* below Info */
    pre_init_line = __LINE__ + 1;
    DE_WARN(DE_LOG_CAT_CORE, "hello %d", 42);
}

TEST_F(log, pre_init_stderr, log_fx) {
    static char out[4096];
    capture_stderr(pre_init_logging, out, sizeof out);
    char where[64];
    snprintf(where, sizeof where, "test_log.c:%u hello 42\n", (unsigned)pre_init_line);
    EXPECT_TRUE(strstr(out, "warn") != NULL);
    EXPECT_TRUE(strstr(out, "[core] ") != NULL);
    EXPECT_TRUE(strstr(out, where) != NULL);
    EXPECT_TRUE(strstr(out, "hidden") == NULL);

    /* Sinks are not called before init. */
    capture_init(&f->cap, 16);
    de_log_add_sink(capture_sink, &f->cap);
    capture_stderr(pre_init_logging, out, sizeof out);
    EXPECT_EQ(f->cap.calls, 0u);
    EXPECT_TRUE(strstr(out, where) != NULL);

    /* After shutdown it is stderr again (and shutdown removed the sink). */
    de_log_desc d = {0};
    d.no_console_sink = true;
    ASSERT_EQ(de_log_init(&d), DE_OK);
    de_log_shutdown();
    capture_stderr(pre_init_logging, out, sizeof out);
    EXPECT_TRUE(strstr(out, where) != NULL);
    EXPECT_EQ(f->cap.calls, 0u);
}

/* ---- File sink ---------------------------------------------------------------------------- */

TEST_F(log, file_sink, log_fx) {
    const char *path = "test_log_file_sink.log";
    de_log_file_sink *s = NULL;
    EXPECT_EQ(de_log_file_sink_open(NULL, &s), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_log_file_sink_open("no_such_dir_deng150/x.log", &s), DE_ERR_IO);
    EXPECT_TRUE(s == NULL);

    de_log_desc d = {0};
    d.no_console_sink = true;
    ASSERT_EQ(de_log_init(&d), DE_OK);
    ASSERT_EQ(de_log_file_sink_open(path, &s), DE_OK);
    u32 line1 = __LINE__ + 1;
    DE_INFO(DE_LOG_CAT_CORE, "file one %d", 1);
    DE_LOG(DE_LOG_DEBUG, DE_LOG_CAT_CORE, "not written");
    DE_ERROR(DE_LOG_CAT_PLATFORM, "file two");
    de_log_category cat = de_log_register_category("test_file", DE_LOG_TRACE);
    DE_LOG(DE_LOG_TRACE, cat, "file three");
    de_log_file_sink_close(s); /* flushes first */
    DE_INFO(DE_LOG_CAT_CORE, "after close");
    de_log_shutdown();

    FILE *fp = fopen(path, "r");
    ASSERT_TRUE(fp != NULL);
    static char buf[4096];
    size_t n = fread(buf, 1, sizeof buf - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    remove(path);

    u32 lines = 0;
    for (size_t i = 0; i < n; i++)
        lines += buf[i] == '\n';
    EXPECT_EQ(lines, 3u);
    char want[128];
    snprintf(want, sizeof want, "] test_log.c:%u file one 1\n", (unsigned)line1);
    EXPECT_TRUE(strstr(buf, want) != NULL);
    EXPECT_TRUE(strstr(buf, " [info][core][") != NULL);
    EXPECT_TRUE(strstr(buf, " [error][platform][") != NULL);
    EXPECT_TRUE(strstr(buf, " [trace][test_file][") != NULL);
    EXPECT_TRUE(strstr(buf, "not written") == NULL);
    EXPECT_TRUE(strstr(buf, "after close") == NULL);
    const char *one = strstr(buf, "file one");
    const char *two = strstr(buf, "file two");
    const char *three = strstr(buf, "file three");
    EXPECT_TRUE(one != NULL && two != NULL && three != NULL && one < two && two < three);
}

/* ---- Fatal and panic ------------------------------------------------------------------------ */

#if !defined(_WIN32)

#define FATAL_ABORT_EXIT 42

static void abort_to_exit(int sig) {
    (void)sig;
    _exit(FATAL_ABORT_EXIT);
}

static void read_all(int fd, char *out, size_t cap) {
    size_t len = 0;
    for (;;) {
        ssize_t n = read(fd, out + len, cap - 1 - len);
        if (n <= 0)
            break;
        len += (size_t)n;
        if (len == cap - 1)
            break;
    }
    out[len] = '\0';
}

/* Forks a child that starts logging with the console sink and a log thread that sleeps 60 s,
 * logs one Info line, then dies through DE_FATAL (fatal != 0) or DE_PANIC. Returns the child's
 * stderr; *exited tells whether it ended in abort(). */
static void run_fatal_child(int fatal, char *out, size_t cap, int *exited) {
    int err_pipe[2];
    ASSERT_EQ(pipe(err_pipe), 0);
    fflush(NULL);
    pid_t pid = fork();
    ASSERT_TRUE(pid >= 0);
    if (pid == 0) {
        close(err_pipe[0]);
        dup2(err_pipe[1], 2);
        signal(SIGABRT, abort_to_exit);
        de_log_desc d = {0};
        d.drain_interval_ms = 60000;
        if (de_log_init(&d) != DE_OK)
            _exit(3);
        DE_INFO(DE_LOG_CAT_CORE, "pending before the end");
        if (fatal)
            DE_FATAL(DE_LOG_CAT_CORE, "fatal %d", 7);
        else
            DE_PANIC("boom %d", 8);
        _exit(4); /* not reached */
    }
    close(err_pipe[1]);
    read_all(err_pipe[0], out, cap);
    close(err_pipe[0]);
    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    *exited = WIFEXITED(status) && WEXITSTATUS(status) == FATAL_ABORT_EXIT;
}

TEST_F(log, fatal_flushes_then_panics, log_fx) {
    static char out[8192];
    int exited = 0;
    run_fatal_child(1, out, sizeof out, &exited);
    EXPECT_TRUE(exited);
    const char *pending = strstr(out, "[core] ");
    const char *before = strstr(out, "pending before the end");
    const char *fatal = strstr(out, "fatal 7\n");
    const char *panic = strstr(out, "DE_PANIC at ");
    EXPECT_TRUE(pending != NULL);
    EXPECT_TRUE(strstr(out, "fatal") != NULL);
    ASSERT_TRUE(before != NULL && fatal != NULL && panic != NULL);
    EXPECT_TRUE(before < fatal && fatal < panic);
    EXPECT_TRUE(strstr(panic, "fatal 7") != NULL);
}

TEST_F(log, panic_flushes_log, log_fx) {
    static char out[8192];
    int exited = 0;
    run_fatal_child(0, out, sizeof out, &exited);
    EXPECT_TRUE(exited);
    const char *before = strstr(out, "pending before the end");
    const char *panic = strstr(out, "DE_PANIC at ");
    ASSERT_TRUE(before != NULL && panic != NULL);
    EXPECT_TRUE(before < panic);
    EXPECT_TRUE(strstr(panic, "boom 8") != NULL);
}

#else /* _WIN32: no fork */

TEST_F(log, fatal_flushes_then_panics, log_fx) {
    SKIP("needs fork(); covered on POSIX hosts");
}

TEST_F(log, panic_flushes_log, log_fx) {
    SKIP("needs fork(); covered on POSIX hosts");
}

#endif

/* ---- Category table full (last in the file: it fills the process-wide table) ------------- */

TEST(log, category_table_full) {
    de_log_category render = de_log_register_category("test_render", DE_LOG_WARN);
    char name[32];
    u32 last = 0;
    for (u32 i = 0; i < DE_LOG_MAX_CATEGORIES; i++) {
        snprintf(name, sizeof name, "test_fill_%u", (unsigned)i);
        last = de_log_register_category(name, DE_LOG_INFO).id;
    }
    EXPECT_EQ(last, 0u); /* full: falls back to `general` */
    EXPECT_EQ(de_log_register_category("test_render", DE_LOG_INFO).id, render.id);
    EXPECT_EQ(strncmp(de_log_category_name((de_log_category){DE_LOG_MAX_CATEGORIES - 1}),
                      "test_fill_", 10),
              0);
}
