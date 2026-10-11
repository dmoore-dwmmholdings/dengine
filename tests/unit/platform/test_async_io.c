/* Tests for the async I/O system (DENG-247): random reads, priority order, cancellation,
 * delivery threads, shutdown, and no allocation per request.
 *
 * de_test.h checks may only run on the test thread, so callbacks that run on I/O threads only
 * record what they see (atomics, per-request slots) and the test thread checks it afterwards.
 * Each test writes its data file into its own directory under DE_IO_TEST_TMP_DIR. */
#include "de_test.h"

#include "de_alloc_guard.h"

#include <dengine/de_platform.h>

#include "platform/de_atomic.h"
#include "platform/vfs_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DE_IO_TEST_TMP_DIR
#error "DE_IO_TEST_TMP_DIR must be defined by tests/unit/platform/CMakeLists.txt"
#endif

/* Test hook in src/platform/async_io.c: runs on the I/O thread after each chunk. */
void de__io_test_set_chunk_hook(void (*fn)(de_io_request *req, u64 done));

#define BIG_FILE_BYTES ((u64)64 << 20)
#define NS_PER_S       1000000000ull

/* ---------- helpers ---------- */

/* Byte i of the test file: a hash of i, so a read at a wrong offset is caught. */
static u8 pattern(u64 i) {
    return (u8)((i * 0x9E3779B97F4A7C15ull) >> 56);
}

static bool matches_pattern(const u8 *p, u64 offset, u64 n) {
    for (u64 k = 0; k < n; ++k) {
        if (p[k] != pattern(offset + k))
            return false;
    }
    return true;
}

/* Mounts DE_IO_TEST_TMP_DIR/<test> as user:/ and writes user:/big.bin (`bytes` of pattern). */
static void setup_file(const char *test, u64 bytes) {
    char dir[DE_FS_PATH_MAX];
    snprintf(dir, sizeof dir, "%s/%s", DE_IO_TEST_TMP_DIR, test);
    ASSERT_EQ(de__native_mkdirs(dir), DE_OK);
    ASSERT_EQ(de_fs_mount("user", dir, DE_MOUNT_WRITABLE, 0), DE_OK);
    de_file *f = NULL;
    ASSERT_EQ(de_file_open("user:/big.bin", DE_FILE_WRITE, &f), DE_OK);
    u8 *chunk = (u8 *)malloc((size_t)1 << 20);
    ASSERT_TRUE(chunk != NULL);
    for (u64 off = 0; off < bytes; off += (u64)1 << 20) {
        u64 n = bytes - off < ((u64)1 << 20) ? bytes - off : ((u64)1 << 20);
        for (u64 k = 0; k < n; ++k)
            chunk[k] = pattern(off + k);
        ASSERT_EQ(de_file_write(f, chunk, n), DE_OK);
    }
    free(chunk);
    de_file_close(f);
}

/* Deletes user:/big.bin (64 MiB files stay out of the build tree) and unmounts the directory. */
static void teardown_file(const char *test) {
    char dir[DE_FS_PATH_MAX];
    snprintf(dir, sizeof dir, "%s/%s", DE_IO_TEST_TMP_DIR, test);
    EXPECT_EQ(de_file_delete("user:/big.bin"), DE_OK);
    EXPECT_EQ(de_fs_unmount("user", dir), DE_OK);
}

static de_file *open_big(void) {
    de_file *f = NULL;
    ASSERT_EQ(de_file_open("user:/big.bin", DE_FILE_READ, &f), DE_OK);
    return f;
}

/* xorshift64*, for reproducible random requests. */
static u64 rng_next(u64 *s) {
    u64 x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 0x2545F4914F6CDD1Dull;
}

/* Per-request record, reached through req->user. */
typedef struct slot {
    de_atomic_u32 calls;
    de_atomic_u32 thread_id;
    de_atomic_u32 order; /* completion order, 1-based */
    de_atomic_u32 result;
    de_atomic_u64 bytes_read;
} slot;

static de_atomic_u32 g_done;     /* completions seen */
static de_atomic_u32 g_sequence; /* source of slot.order */

static void record(de_io_request *req, de_result r, u64 bytes_read) {
    slot *s = (slot *)req->user;
    de_atomic_store_u32(&s->result, (u32)r, DE_ATOMIC_RELAXED);
    de_atomic_store_u64(&s->bytes_read, bytes_read, DE_ATOMIC_RELAXED);
    de_atomic_store_u32(&s->thread_id, de_thread_current_id(), DE_ATOMIC_RELAXED);
    de_atomic_store_u32(&s->order, de_atomic_fetch_add_u32(&g_sequence, 1, DE_ATOMIC_RELAXED) + 1,
                        DE_ATOMIC_RELAXED);
    de_atomic_fetch_add_u32(&s->calls, 1, DE_ATOMIC_RELAXED);
    de_atomic_fetch_add_u32(&g_done, 1, DE_ATOMIC_RELEASE);
}

/* Sleeps with a timed wait on a semaphore nobody posts. It is created once, before any guarded
 * region (creating a dispatch semaphore allocates). */
static de_sem g_nap;
static bool g_nap_ready;

static void nap_ns(u64 ns) {
    if (!g_nap_ready) {
        if (de_sem_init(&g_nap, 0) != DE_OK)
            return;
        g_nap_ready = true;
    }
    (void)de_sem_wait_timeout(&g_nap, ns);
}

/* Like record, without the thread id and the order: no thread-local storage, which macOS
 * allocates on a thread's first access. For the alloc-guarded region. */
static void record_quiet(de_io_request *req, de_result r, u64 bytes_read) {
    slot *s = (slot *)req->user;
    de_atomic_store_u32(&s->result, (u32)r, DE_ATOMIC_RELAXED);
    de_atomic_store_u64(&s->bytes_read, bytes_read, DE_ATOMIC_RELAXED);
    de_atomic_fetch_add_u32(&s->calls, 1, DE_ATOMIC_RELAXED);
    de_atomic_fetch_add_u32(&g_done, 1, DE_ATOMIC_RELEASE);
}

/* Pumps until `count` completions were seen in total, or the timeout. */
static bool pump_until(u32 count, u64 timeout_ns) {
    u64 end = de_time_now_ns() + timeout_ns;
    while (de_atomic_load_u32(&g_done, DE_ATOMIC_ACQUIRE) < count) {
        if (de_time_now_ns() > end)
            return false;
        if (de_io_pump(0) == 0)
            nap_ns(100000);
    }
    return true;
}

static void reset_counters(void) {
    de_atomic_store_u32(&g_done, 0, DE_ATOMIC_RELAXED);
    de_atomic_store_u32(&g_sequence, 0, DE_ATOMIC_RELAXED);
}

/* A request whose callback (on the I/O thread) parks that thread until `release` is posted,
 * so the test can fill the queue while nothing is taken from it. */
static de_sem g_blocked, g_release;

static void blocker_done(de_io_request *req, de_result r, u64 n) {
    (void)req;
    (void)r;
    (void)n;
    de_sem_post(&g_blocked, 1);
    de_sem_wait(&g_release);
}

static void start_blocker(de_io_request *req, de_file *f, u8 *dst) {
    ASSERT_EQ(de_sem_init(&g_blocked, 0), DE_OK);
    ASSERT_EQ(de_sem_init(&g_release, 0), DE_OK);
    memset(req, 0, sizeof *req);
    req->file = f;
    req->bytes = 16;
    req->dst = dst;
    req->on_complete = blocker_done;
    req->flags = DE_IO_CALLBACK_ON_IO_THREAD;
    ASSERT_EQ(de_io_submit(req), DE_OK);
    ASSERT_TRUE(de_sem_wait_timeout(&g_blocked, 10 * NS_PER_S));
}

/* ---------- tests ---------- */

/* Pump-delivered completions with data checks, counted on the test thread. */
typedef struct check_ctx {
    u32 bad;
} check_ctx;

TEST(aio, random_reads) {
    setup_file("random_reads", BIG_FILE_BYTES);
    de_file *f = open_big();
    enum { N = 1000 };
    de_io_request *reqs = (de_io_request *)calloc(N, sizeof *reqs);
    slot *slots = (slot *)calloc(N, sizeof *slots);
    u8 **bufs = (u8 **)calloc(N, sizeof *bufs);
    u64 *expect = (u64 *)calloc(N, sizeof *expect);
    ASSERT_TRUE(reqs != NULL && slots != NULL && bufs != NULL && expect != NULL);

    u64 seed = 0x1234567887654321ull;
    for (u32 i = 0; i < N; ++i) {
        u64 len = 1 + rng_next(&seed) % (64u << 10);
        if (i % 50 == 0)
            len = (3u << 20) + rng_next(&seed) % (1u << 20); /* several chunks */
        u64 off = rng_next(&seed) % BIG_FILE_BYTES;
        if (i % 97 == 0)
            off = BIG_FILE_BYTES - 100; /* runs past the end */
        bufs[i] = (u8 *)malloc((size_t)len);
        ASSERT_TRUE(bufs[i] != NULL);
        de_io_request *r = &reqs[i];
        r->file = f;
        r->offset = off;
        r->bytes = len;
        r->dst = bufs[i];
        r->on_complete = record_quiet;
        r->user = &slots[i];
        r->priority = (u8)(rng_next(&seed) % 4);
        r->flags = i % 2 == 0 ? DE_IO_CALLBACK_ON_IO_THREAD : 0;
        expect[i] = off + len <= BIG_FILE_BYTES ? len : BIG_FILE_BYTES - off;
    }

    ASSERT_EQ(de_io_init(&(de_io_desc){.thread_count = 4}), DE_OK);
    EXPECT_EQ(de_io_init(NULL), DE_ERR_ALREADY_EXISTS);
    /* Warm up the I/O threads and the pump path (first-use thread-local storage). */
    {
        de_io_request warm[8];
        slot warm_slots[8];
        u8 warm_buf[8][64];
        memset(warm, 0, sizeof warm);
        memset(warm_slots, 0, sizeof warm_slots);
        reset_counters();
        for (u32 i = 0; i < 8; ++i) {
            warm[i] = (de_io_request){.file = f,
                                      .bytes = 64,
                                      .dst = warm_buf[i],
                                      .on_complete = record_quiet,
                                      .user = &warm_slots[i]};
            ASSERT_EQ(de_io_submit(&warm[i]), DE_OK);
        }
        ASSERT_TRUE(pump_until(8, 10 * NS_PER_S));
        nap_ns(1000);
        reset_counters();
    }

    /* The whole submit/complete path allocates nothing: requests are caller-owned. */
    de_alloc_guard g = {0};
    de_alloc_guard_begin(&g);
    for (u32 i = 0; i < N; ++i) {
        if (de_io_submit(&reqs[i]) != DE_OK)
            break;
    }
    bool all = pump_until(N, 60 * NS_PER_S);
    de_alloc_guard_end(&g);
    ASSERT_TRUE(all);
    DE_ALLOC_GUARD_ASSERT_ZERO(&g);
    EXPECT_EQ(de_io_pump(0), 0u);

    u32 bad = 0;
    for (u32 i = 0; i < N; ++i) {
        EXPECT_EQ(de_atomic_load_u32(&slots[i].calls, DE_ATOMIC_ACQUIRE), 1u);
        EXPECT_EQ(de_atomic_load_u32(&slots[i].result, DE_ATOMIC_RELAXED), (u32)DE_OK);
        u64 got = de_atomic_load_u64(&slots[i].bytes_read, DE_ATOMIC_RELAXED);
        EXPECT_EQ(got, expect[i]);
        if (got != expect[i] || !matches_pattern(bufs[i], reqs[i].offset, got))
            ++bad;
        free(bufs[i]);
    }
    EXPECT_EQ(bad, 0u);
    de_io_shutdown();
    de_file_close(f);
    teardown_file("random_reads");
    free(reqs);
    free(slots);
    free(bufs);
    free(expect);
}

TEST(aio, priority_order) {
    setup_file("priority_order", (u64)1 << 20);
    de_file *f = open_big();
    ASSERT_EQ(de_io_init(&(de_io_desc){.thread_count = 1}), DE_OK);
    reset_counters();
    de_io_request blocker;
    u8 blocker_buf[16];
    start_blocker(&blocker, f, blocker_buf);

    /* The single I/O thread is parked: queue 500 low-priority, then 20 priority-0 reads. */
    enum { LOW = 500, HIGH = 20, N = LOW + HIGH };
    de_io_request *reqs = (de_io_request *)calloc(N, sizeof *reqs);
    slot *slots = (slot *)calloc(N, sizeof *slots);
    u8(*bufs)[256] = (u8(*)[256])malloc((size_t)N * 256);
    ASSERT_TRUE(reqs != NULL && slots != NULL && bufs != NULL);
    for (u32 i = 0; i < N; ++i) {
        reqs[i].file = f;
        reqs[i].offset = (u64)i * 256;
        reqs[i].bytes = 256;
        reqs[i].dst = bufs[i];
        reqs[i].on_complete = record;
        reqs[i].user = &slots[i];
        reqs[i].priority = i < LOW ? (u8)(100 + i % 100) : 0;
        reqs[i].flags = DE_IO_CALLBACK_ON_IO_THREAD;
        ASSERT_EQ(de_io_submit(&reqs[i]), DE_OK);
    }
    de_sem_post(&g_release, 1);
    ASSERT_TRUE(pump_until(N, 30 * NS_PER_S));

    /* Every priority-0 read completed before every low-priority one (one thread, so the order
     * is exact), and the low ones in priority order, FIFO within one priority. */
    u32 max_high = 0, min_low = 0xFFFFFFFFu;
    for (u32 i = 0; i < N; ++i) {
        u32 o = de_atomic_load_u32(&slots[i].order, DE_ATOMIC_RELAXED);
        EXPECT_EQ(de_atomic_load_u32(&slots[i].calls, DE_ATOMIC_RELAXED), 1u);
        if (i < LOW)
            min_low = o < min_low ? o : min_low;
        else
            max_high = o > max_high ? o : max_high;
    }
    EXPECT_LT(max_high, min_low);
    EXPECT_EQ(max_high, (u32)HIGH);
    u32 inversions = 0;
    for (u32 i = 0; i < LOW; ++i) {
        for (u32 j = i + 1; j < LOW; ++j) {
            u32 oi = de_atomic_load_u32(&slots[i].order, DE_ATOMIC_RELAXED);
            u32 oj = de_atomic_load_u32(&slots[j].order, DE_ATOMIC_RELAXED);
            bool i_first = reqs[i].priority < reqs[j].priority ||
                           (reqs[i].priority == reqs[j].priority); /* FIFO: i was first */
            if (i_first != (oi < oj))
                ++inversions;
        }
    }
    EXPECT_EQ(inversions, 0u);
    for (u32 i = 0; i < N; ++i)
        EXPECT_TRUE(matches_pattern(bufs[i], (u64)i * 256, 256));

    de_io_shutdown();
    de_sem_destroy(&g_blocked);
    de_sem_destroy(&g_release);
    de_file_close(f);
    teardown_file("priority_order");
    free(reqs);
    free(slots);
    free(bufs);
}

TEST(aio, cancel_queued) {
    setup_file("cancel_queued", (u64)1 << 20);
    de_file *f = open_big();
    de_io_request a, b;
    slot sa, sb;
    u8 buf_a[4096], buf_b[4096];
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    memset(&sa, 0, sizeof sa);
    memset(&sb, 0, sizeof sb);
    a = (de_io_request){
        .file = f, .bytes = sizeof buf_a, .dst = buf_a, .on_complete = record, .user = &sa};
    b = (de_io_request){.file = f,
                        .bytes = sizeof buf_b,
                        .dst = buf_b,
                        .on_complete = record,
                        .user = &sb,
                        .flags = DE_IO_CALLBACK_ON_IO_THREAD};

    /* Not running yet: refused, no callback. */
    EXPECT_EQ(de_io_submit(&a), DE_ERR_UNSUPPORTED);
    ASSERT_EQ(de_io_init(&(de_io_desc){.thread_count = 1}), DE_OK);
    reset_counters();
    de_io_request blocker;
    u8 blocker_buf[16];
    start_blocker(&blocker, f, blocker_buf);

    /* Bad requests. */
    de_io_request bad = a;
    bad.on_complete = NULL;
    EXPECT_EQ(de_io_submit(&bad), DE_ERR_INVALID_ARGUMENT);
    bad = a;
    bad.dst = NULL;
    EXPECT_EQ(de_io_submit(&bad), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_io_submit(NULL), DE_ERR_INVALID_ARGUMENT);
    de_file *wf = NULL;
    ASSERT_EQ(de_file_open("user:/w.bin", DE_FILE_WRITE, &wf), DE_OK);
    bad = a;
    bad.file = wf;
    EXPECT_EQ(de_io_submit(&bad), DE_ERR_INVALID_ARGUMENT);
    de_file_close(wf);
    EXPECT_EQ(de_file_delete("user:/w.bin"), DE_OK);

    ASSERT_EQ(de_io_submit(&a), DE_OK);
    ASSERT_EQ(de_io_submit(&b), DE_OK);
    EXPECT_EQ(de_io_submit(&a), DE_ERR_BUSY);

    /* b (I/O-thread delivery) completes inside de_io_cancel, on this thread. */
    de_io_cancel(&b);
    EXPECT_EQ(de_atomic_load_u32(&sb.calls, DE_ATOMIC_RELAXED), 1u);
    EXPECT_EQ(de_atomic_load_u32(&sb.result, DE_ATOMIC_RELAXED), (u32)DE_ERR_CANCELLED);
    EXPECT_EQ(de_atomic_load_u64(&sb.bytes_read, DE_ATOMIC_RELAXED), 0u);
    EXPECT_EQ(de_atomic_load_u32(&sb.thread_id, DE_ATOMIC_RELAXED), de_thread_current_id());

    /* a (pump delivery) waits for the pump. */
    de_io_cancel(&a);
    EXPECT_EQ(de_atomic_load_u32(&sa.calls, DE_ATOMIC_RELAXED), 0u);
    de_io_cancel(&a); /* already finished: nothing */
    EXPECT_EQ(de_io_pump(0), 1u);
    EXPECT_EQ(de_atomic_load_u32(&sa.calls, DE_ATOMIC_RELAXED), 1u);
    EXPECT_EQ(de_atomic_load_u32(&sa.result, DE_ATOMIC_RELAXED), (u32)DE_ERR_CANCELLED);
    EXPECT_EQ(de_atomic_load_u64(&sa.bytes_read, DE_ATOMIC_RELAXED), 0u);
    de_io_cancel(&a); /* idle: nothing */
    de_io_cancel(NULL);

    /* Release the I/O thread: nothing more is delivered, and the requests can be reused. */
    de_sem_post(&g_release, 1);
    EXPECT_EQ(de_io_pump(0), 0u);
    ASSERT_EQ(de_io_submit(&a), DE_OK);
    ASSERT_TRUE(pump_until(3, 10 * NS_PER_S)); /* b, a (cancelled) and a again */
    EXPECT_EQ(de_atomic_load_u32(&sa.calls, DE_ATOMIC_RELAXED), 2u);
    EXPECT_EQ(de_atomic_load_u32(&sa.result, DE_ATOMIC_RELAXED), (u32)DE_OK);
    EXPECT_EQ(de_atomic_load_u32(&sb.calls, DE_ATOMIC_RELAXED), 1u);
    EXPECT_TRUE(matches_pattern(buf_a, 0, sizeof buf_a));

    de_io_shutdown();
    de_io_shutdown(); /* not running: nothing */
    de_sem_destroy(&g_blocked);
    de_sem_destroy(&g_release);
    de_file_close(f);
    teardown_file("cancel_queued");
}

/* Cancel during a read: the hook parks the I/O thread after its first chunk of the target
 * request until the test thread has cancelled it. */
static de_sem g_hook_started, g_hook_go;
static de_io_request *g_hook_target; /* set before the submit (ordered by its lock) */

static void cancel_hook(de_io_request *req, u64 done) {
    if (req == g_hook_target && done == DE_IO_CHUNK_BYTES) {
        de_sem_post(&g_hook_started, 1);
        de_sem_wait(&g_hook_go);
    }
}

TEST(aio, cancel_inflight) {
    setup_file("cancel_inflight", BIG_FILE_BYTES);
    de_file *f = open_big();
    u8 *dst = (u8 *)malloc((size_t)BIG_FILE_BYTES);
    ASSERT_TRUE(dst != NULL);
    ASSERT_EQ(de_sem_init(&g_hook_started, 0), DE_OK);
    ASSERT_EQ(de_sem_init(&g_hook_go, 0), DE_OK);
    ASSERT_EQ(de_io_init(&(de_io_desc){.thread_count = 1}), DE_OK);
    reset_counters();

    slot s;
    memset(&s, 0, sizeof s);
    de_io_request req = {
        .file = f, .bytes = BIG_FILE_BYTES, .dst = dst, .on_complete = record, .user = &s};
    g_hook_target = &req;
    de__io_test_set_chunk_hook(cancel_hook);
    ASSERT_EQ(de_io_submit(&req), DE_OK);
    ASSERT_TRUE(de_sem_wait_timeout(&g_hook_started, 10 * NS_PER_S));
    de_io_cancel(&req); /* in flight: flags it */
    EXPECT_EQ(de_io_submit(&req), DE_ERR_BUSY);
    de_sem_post(&g_hook_go, 1);
    ASSERT_TRUE(pump_until(1, 10 * NS_PER_S));
    de__io_test_set_chunk_hook(NULL);

    EXPECT_EQ(de_atomic_load_u32(&s.calls, DE_ATOMIC_RELAXED), 1u);
    EXPECT_EQ(de_atomic_load_u32(&s.result, DE_ATOMIC_RELAXED), (u32)DE_ERR_CANCELLED);
    u64 got = de_atomic_load_u64(&s.bytes_read, DE_ATOMIC_RELAXED);
    EXPECT_EQ(got, DE_IO_CHUNK_BYTES); /* stopped at the next chunk boundary */
    EXPECT_LT(got, BIG_FILE_BYTES);
    EXPECT_TRUE(matches_pattern(dst, 0, got));

    /* The same request again, uncancelled, reads all 64 MiB. */
    memset(&s, 0, sizeof s);
    reset_counters();
    ASSERT_EQ(de_io_submit(&req), DE_OK);
    ASSERT_TRUE(pump_until(1, 30 * NS_PER_S));
    EXPECT_EQ(de_atomic_load_u32(&s.result, DE_ATOMIC_RELAXED), (u32)DE_OK);
    EXPECT_EQ(de_atomic_load_u64(&s.bytes_read, DE_ATOMIC_RELAXED), BIG_FILE_BYTES);
    EXPECT_TRUE(matches_pattern(dst, 0, BIG_FILE_BYTES));

    de_io_shutdown();
    de_sem_destroy(&g_hook_started);
    de_sem_destroy(&g_hook_go);
    de_file_close(f);
    teardown_file("cancel_inflight");
    free(dst);
}

static de_sem g_io_thread_done;

static void record_and_post(de_io_request *req, de_result r, u64 n) {
    record(req, r, n);
    de_sem_post(&g_io_thread_done, 1);
}

TEST(aio, pump_vs_io_thread_callbacks) {
    setup_file("pump_vs_io_thread_callbacks", (u64)1 << 20);
    de_file *f = open_big();
    ASSERT_EQ(de_sem_init(&g_io_thread_done, 0), DE_OK);
    ASSERT_EQ(de_io_init(NULL), DE_OK); /* default: DE_IO_DEFAULT_THREADS */
    reset_counters();
    u32 me = de_thread_current_id();

    slot sp, st;
    memset(&sp, 0, sizeof sp);
    memset(&st, 0, sizeof st);
    u8 bp[1000], bt[1000];
    de_io_request p = {
        .file = f, .offset = 10, .bytes = sizeof bp, .dst = bp, .on_complete = record, .user = &sp};
    de_io_request t = {.file = f,
                       .offset = 20,
                       .bytes = sizeof bt,
                       .dst = bt,
                       .on_complete = record_and_post,
                       .user = &st,
                       .flags = DE_IO_CALLBACK_ON_IO_THREAD};
    ASSERT_EQ(de_io_submit(&p), DE_OK);
    ASSERT_EQ(de_io_submit(&t), DE_OK);

    /* t is delivered without any pump, on an I/O thread. */
    ASSERT_TRUE(de_sem_wait_timeout(&g_io_thread_done, 10 * NS_PER_S));
    EXPECT_EQ(de_atomic_load_u32(&st.calls, DE_ATOMIC_RELAXED), 1u);
    EXPECT_NE(de_atomic_load_u32(&st.thread_id, DE_ATOMIC_RELAXED), me);
    EXPECT_TRUE(matches_pattern(bt, 20, sizeof bt));

    /* p never runs its callback until this thread pumps. */
    nap_ns(20000000);
    EXPECT_EQ(de_atomic_load_u32(&sp.calls, DE_ATOMIC_RELAXED), 0u);
    ASSERT_TRUE(pump_until(2, 10 * NS_PER_S));
    EXPECT_EQ(de_atomic_load_u32(&sp.calls, DE_ATOMIC_RELAXED), 1u);
    EXPECT_EQ(de_atomic_load_u32(&sp.thread_id, DE_ATOMIC_RELAXED), me);
    EXPECT_EQ(de_atomic_load_u64(&sp.bytes_read, DE_ATOMIC_RELAXED), (u64)sizeof bp);
    EXPECT_TRUE(matches_pattern(bp, 10, sizeof bp));

    /* de_io_pump(max) delivers at most max. */
    slot s3[3];
    memset(s3, 0, sizeof s3);
    u8 b3[3][16];
    de_io_request r3[3];
    for (int i = 0; i < 3; ++i) {
        r3[i] = (de_io_request){
            .file = f, .bytes = 16, .dst = b3[i], .on_complete = record, .user = &s3[i]};
        ASSERT_EQ(de_io_submit(&r3[i]), DE_OK);
    }
    /* Never more than one per call. */
    u64 end = de_time_now_ns() + 10 * NS_PER_S;
    u32 delivered = 0, max_per_call = 0;
    while (delivered < 3 && de_time_now_ns() < end) {
        u32 n = de_io_pump(1);
        max_per_call = n > max_per_call ? n : max_per_call;
        delivered += n;
        if (n == 0)
            nap_ns(1000000);
    }
    EXPECT_EQ(delivered, 3u);
    EXPECT_EQ(max_per_call, 1u);
    for (int i = 0; i < 3; ++i)
        EXPECT_EQ(de_atomic_load_u32(&s3[i].calls, DE_ATOMIC_RELAXED), 1u);

    de_io_shutdown();
    de_sem_destroy(&g_io_thread_done);
    de_file_close(f);
    teardown_file("pump_vs_io_thread_callbacks");
}

/* Posts g_release after a delay, from its own thread. */
static i32 delayed_release(void *user) {
    (void)user;
    nap_ns(50000000);
    de_sem_post(&g_release, 1);
    return 0;
}

TEST(aio, shutdown_completes_everything) {
    setup_file("shutdown_completes_everything", (u64)8 << 20);
    de_file *f = open_big();
    ASSERT_EQ(de_io_init(&(de_io_desc){.thread_count = 1}), DE_OK);
    reset_counters();
    de_io_request blocker;
    u8 blocker_buf[16];
    start_blocker(&blocker, f, blocker_buf);
    enum { N = 40 };
    de_io_request reqs[N];
    slot slots[N];
    memset(slots, 0, sizeof slots);
    u8 *dst = (u8 *)malloc((size_t)N << 20);
    ASSERT_TRUE(dst != NULL);
    for (u32 i = 0; i < N; ++i) {
        reqs[i] = (de_io_request){.file = f,
                                  .offset = (u64)(i % 8) << 20,
                                  .bytes = (u64)1 << 20,
                                  .dst = dst + ((size_t)i << 20),
                                  .on_complete = record,
                                  .user = &slots[i],
                                  .flags = i % 3 == 0 ? DE_IO_CALLBACK_ON_IO_THREAD : 0u};
        ASSERT_EQ(de_io_submit(&reqs[i]), DE_OK);
    }
    /* The I/O thread is parked in the blocker until 50 ms from now; shutdown cancels the queue
     * at once and then waits for the thread. */
    de_thread *helper = NULL;
    ASSERT_EQ(de_thread_create(delayed_release, NULL, "release", DE_THREAD_NORMAL, 0, &helper),
              DE_OK);
    de_io_shutdown();
    (void)de_thread_join(helper);

    /* Every request completed exactly once, with DE_ERR_CANCELLED, before shutdown returned. */
    EXPECT_EQ(de_atomic_load_u32(&g_done, DE_ATOMIC_ACQUIRE), (u32)N);
    for (u32 i = 0; i < N; ++i) {
        EXPECT_EQ(de_atomic_load_u32(&slots[i].calls, DE_ATOMIC_RELAXED), 1u);
        EXPECT_EQ(de_atomic_load_u32(&slots[i].result, DE_ATOMIC_RELAXED), (u32)DE_ERR_CANCELLED);
    }
    EXPECT_EQ(de_io_pump(0), 0u);
    EXPECT_EQ(de_io_submit(&reqs[0]), DE_ERR_UNSUPPORTED);

    /* It starts again after a shutdown. */
    ASSERT_EQ(de_io_init(NULL), DE_OK);
    reset_counters();
    ASSERT_EQ(de_io_submit(&reqs[1]), DE_OK);
    ASSERT_TRUE(pump_until(1, 10 * NS_PER_S));
    EXPECT_EQ(de_atomic_load_u32(&slots[1].result, DE_ATOMIC_RELAXED), (u32)DE_OK);
    EXPECT_TRUE(matches_pattern(dst + ((size_t)1 << 20), (u64)1 << 20, (u64)1 << 20));
    de_io_shutdown();

    de_sem_destroy(&g_blocked);
    de_sem_destroy(&g_release);
    free(dst);
    de_file_close(f);
    teardown_file("shutdown_completes_everything");
}
