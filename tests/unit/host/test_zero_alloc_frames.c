/*
 * test_zero_alloc_frames.c: no heap traffic in 1000 headless frames of the whole host loop
 * (DENG-97; SPEC §5.3, §19.3 and the M1 exit criterion of §23).
 *
 * The fixture game (fixtures/alloc_game.c) runs headless through the host internals in the
 * order de_host_run uses (host_run.c): the profiler, the log and the VFS, the engine, headless
 * mode, hot reload, game attach, crash reporting, the game's callbacks, then de__loop_run with a
 * frame limit of 1060. A frame-end hook, added after the host's own hooks, starts the alloc
 * guard (tests/support/de_alloc_guard.h, DENG-155) after frame 60 and stops it after frame 1060,
 * so the guard spans 1000 whole frames: pump, every schedule, the host's frame-end hooks
 * (headless stop check, crash annotations and cvar snapshot), the frame arena swap and pacing.
 * DE_ALLOC_GUARD_ASSERT_ZERO then fails the test with the first offending call sites and their
 * memory tag. Nothing is allowed (no de_alloc_guard_allow).
 *
 * What is counted:
 *   - Engine allocations through the allocation hooks. Debug and Dev: the engine uses the default
 *     heap, as de_host_run does, and the heap reports every block with its module's tag. Release
 *     (the headless CI jobs): the heap does not report, so the engine gets a tracking allocator
 *     around it, and everything the engine creates is still counted.
 *   - Raw malloc-family calls: macOS (DYLD_INTERPOSE) and Linux (ld --wrap). Not on Windows, and
 *     not in macOS ASan/TSan builds (see de_alloc_guard_counts_raw()).
 *
 * Built twice:
 *   test_zero_alloc           zero_alloc.frames_1000: must pass.
 *   test_zero_alloc_selftest  the same with ALLOC_GUARD_SELFTEST: the fixture makes one DE_ALLOC
 *                             per frame with the tag "alloc_game_selftest", so
 *                             zero_alloc_selftest.detects must fail (CTest WILL_FAIL), and its
 *                             output must name the tag (zero_alloc_selftest_names_tag).
 *
 * The frames run on a step clock (one 60 Hz period per frame), so every frame runs exactly one
 * FixedUpdate and the run is the same on every machine. Frame pacing is real: capped at 500 Hz,
 * so the window spans about 2 s and the once-per-second work of the frame-end hooks runs in it.
 *
 * When host_run.c changes its start order or adds a per-frame service, mirror it in run_host()
 * below, so this test keeps covering what a real run does.
 */
#include "de_test.h"

#include "de_alloc_guard.h"

#include "host/host_crash.h"
#include "host/host_internal.h"

#include "alloc_game.h"

#include <dengine/de_debug.h>
#include <dengine/de_task.h>

#include "platform/de_atomic.h"
#include "services/crash/bundle_internal.h" /* de__crash_cvar_snapshot_count */

#include <stdio.h>
#include <string.h>

#ifndef DE_HOST_TEST_DATA_DIR
#error "DE_HOST_TEST_DATA_DIR must be defined by tests/unit/host/CMakeLists.txt"
#endif
#ifndef DE_HOST_TEST_TMP_DIR
#error "DE_HOST_TEST_TMP_DIR must be defined by tests/unit/host/CMakeLists.txt"
#endif

#define WARMUP_FRAMES   60u
#define MEASURED_FRAMES 1000u
#define TOTAL_FRAMES    (WARMUP_FRAMES + MEASURED_FRAMES)

#define FRAME_CAP_HZ 500u

#define MS 1000000ull

/* ---- The guard window (a frame-end hook) ---------------------------------------------------- */

static struct {
    de_alloc_guard guard;
    u64 frame; /* frame-end hook runs so far */
    bool began, ended;
    u64 begin_ns, end_ns;               /* wall clock of the guarded frames */
    u64 snapshots_begin, snapshots_end; /* crash bundle cvar snapshots formatted (DENG-96) */
} g_win;

/* Runs after every other frame-end hook of the frame. After frame WARMUP_FRAMES - 1 (that is,
 * after 60 frames) it starts the guard; after frame TOTAL_FRAMES - 1 it stops it. */
static de_result guard_hook(de__host *h, void *user) {
    (void)h;
    (void)user;
    const u64 f = g_win.frame++;
    if (f == TOTAL_FRAMES - 1u && g_win.began && !g_win.ended) {
        de_alloc_guard_end(&g_win.guard);
        g_win.end_ns = de_time_now_ns();
        g_win.snapshots_end = de__crash_cvar_snapshot_count();
        g_win.ended = true;
    }
    if (f == WARMUP_FRAMES - 1u) {
        g_win.begin_ns = de_time_now_ns();
        g_win.snapshots_begin = de__crash_cvar_snapshot_count();
        de_alloc_guard_begin(&g_win.guard);
        g_win.began = true;
    }
    return DE_OK;
}

/* One 60 Hz period per frame: the loop reads its clock once per frame, so every frame runs
 * exactly one FixedUpdate. */
static u64 g_step_clock_ns;
static u64 step_clock(void *user) {
    (void)user;
    g_step_clock_ns += 16666667u;
    return g_step_clock_ns;
}

/* ---- Warming up every engine thread --------------------------------------------------------- */

/* Per-thread first uses that allocate once, and that a worker which ran no task with a
 * successor, or no par_each range, in the warm-up frames would otherwise do inside the window:
 *   - its two scratch arenas (first de_scratch_begin), and on macOS dyld's block of the
 *     image's thread-local variables;
 *   - its task record pool (src/task/scheduler.c): each worker's slot gets its first block of
 *     DE__TASK_BLOCK_RECORDS records, 10 KiB from the heap (tag "task"), on its first
 *     de_task_submit. A frame worker submits when it finishes a graph node with successors, so
 *     which workers have a block after 60 frames depends on scheduling. Without this warm-up
 *     about one run in three on the 16-core build host caught one such block in the window.
 * So every frame and background worker does both here, before the loop. (Log buffers: workers
 * only log at Debug, which is filtered before anything is allocated; the main thread logs Info
 * in frame 0.) */
typedef struct warm_barrier {
    de_task_system *ts;
    de_atomic_u32 arrived;
    u32 n;
} warm_barrier;

static void warm_noop(void *user) {
    (void)user;
}

static void warm_task(void *user) {
    warm_barrier *b = (warm_barrier *)user;
    de_scratch s = de_scratch_begin(NULL, 0);
    (void)de_arena_push(s.arena, 64, 16);
    de_scratch_end(s);
    /* A submit from this worker: its slot takes its first record block. The child goes to the
     * frame pool, which the frame workers and the waiting main thread drain. */
    de_task_counter child;
    memset(&child, 0, sizeof child);
    de_task_submit(b->ts, DE_TASK_POOL_FRAME, warm_noop, NULL, &child);
    de_task_wait(b->ts, &child);
    /* Hold this thread until `n` tasks run at once, so `n` distinct threads ran one. */
    de_atomic_fetch_add_u32(&b->arrived, 1u, DE_ATOMIC_ACQ_REL);
    const u64 deadline = de_time_now_ns() + 5000u * MS;
    while (de_atomic_load_u32(&b->arrived, DE_ATOMIC_ACQUIRE) < b->n && de_time_now_ns() < deadline)
        de_cpu_pause();
}

static void warm_pool(de_task_system *ts, de_task_pool pool, u32 n) {
    static warm_barrier b;
    b.ts = ts;
    de_atomic_init_u32(&b.arrived, 0);
    b.n = n;
    de_task_counter c;
    memset(&c, 0, sizeof c);
    for (u32 i = 0; i < n; ++i)
        de_task_submit(ts, pool, warm_task, &b, &c);
    de_task_wait(ts, &c);
    EXPECT_GE(de_atomic_load_u32(&b.arrived, DE_ATOMIC_ACQUIRE), n);
}

static void warm_threads(de_task_system *ts) {
    /* Frame pool: the workers plus the main thread (worker 0) that waits and helps. */
    warm_pool(ts, DE_TASK_POOL_FRAME, de_task_worker_count(ts));
    const u32 cores = de_cpu_logical_cores();
    warm_pool(ts, DE_TASK_POOL_BACKGROUND, cores / 4u > 1u ? cores / 4u : 1u);
}

/* ---- The host, in de_host_run's order ------------------------------------------------------- */

typedef struct run_result {
    de_result loop_result;
    u64 loop_frames, loop_fixed_steps;
    u32 frame_workers;
    bool tracking_used;
    de_tracking_stats tracking;
} run_result;

static void run_host(run_result *out) {
    memset(out, 0, sizeof *out);

    /* Process services, as de_host_run starts them (profiler first, before any thread). */
    bool own_prof = false;
    if (!de_prof_running()) {
        own_prof = de_prof_init() == DE_OK; /* DE_ERR_UNSUPPORTED without a backend: fine */
        de_error_clear();
    }
    de_thread_set_name("main");
    const bool own_log = de_log_init(NULL) == DE_OK;
    de_error_clear();
    de_fs_desc fd;
    memset(&fd, 0, sizeof fd);
    fd.game_dir = DE_HOST_TEST_DATA_DIR;
    fd.user_dir = DE_HOST_TEST_TMP_DIR "/user";
    fd.cache_dir = DE_HOST_TEST_TMP_DIR "/cache";
    ASSERT_EQ(de_fs_init(&fd), DE_OK);

    static de__host h; /* static: de__host is large */
    memset(&h, 0, sizeof h);
    h.desc.static_api = alloc_game_api();
    h.desc.headless = true;
    h.log = de_log_register_category("host", DE_LOG_INFO);
    (void)de_log_register_category("game", DE_LOG_INFO);
    de__game_config_defaults(&h.config); /* no game.toml: the defaults */
    snprintf(h.config.name, sizeof h.config.name, "%s", "alloc_game");
    /* Capped at FRAME_CAP_HZ, so the 1000 guarded frames take about 2 s of wall time: the frame
     * pacer sleeps and spins, and the crash bundle's cvar snapshot (once per second, DENG-96)
     * refreshes inside the window. FixedUpdate still follows the step clock. */
    h.config.timing.frame_cap_hz = FRAME_CAP_HZ;
    de_cvar_lock_startup();

    /* The engine's allocator: the default heap where it reports (Debug, Dev), as in a real run;
     * else a tracking allocator around it, so engine allocations are still counted. */
    de__engine_desc ed;
    memset(&ed, 0, sizeof ed);
    ed.config = &h.config;
    ed.headless = true;
#if !DE_ALLOC_GUARD_SEES_HEAP
    static de_tracking_allocator tracking;
    static de_allocator tracking_alloc;
    tracking_alloc = de_tracking_allocator_init(&tracking, NULL, DE_MEM_GENERAL);
    ed.allocator = &tracking_alloc;
    out->tracking_used = true;
#endif
    ASSERT_EQ(de__engine_create(&ed, &h.engine), DE_OK);
    de_engine *e = h.engine;
    ASSERT_TRUE(de__engine_from(e)->headless);
    ASSERT_EQ(de__headless_begin(&h), DE_OK);
    ASSERT_EQ(de__hot_reload_init(&h), DE_OK); /* a static game: nothing to reload */
    ASSERT_EQ(de__game_attach(&h, &h.desc), DE_OK);
    (void)de__host_crash_attach(&h);

    de__world_set_system_owner(e->world, DE__HOST_GAME_SYSTEM_OWNER);
    ASSERT_EQ(h.api->init(e), DE_OK);
    h.api->register_types(e, e->types);
    h.api->register_systems(e, e->world);
    de__world_set_system_owner(e->world, DE__SYSTEM_OWNER_ENGINE);

    out->frame_workers = de_task_worker_count(e->tasks);
    warm_threads(e->tasks);

    memset(&g_win, 0, sizeof g_win);
    g_step_clock_ns = 0;
    ASSERT_EQ(de__loop_add_frame_end_hook(&h, guard_hook, NULL), DE_OK);
    h.loop_max_frames = TOTAL_FRAMES;
    h.loop_clock = step_clock;
    out->loop_result = de__loop_run(&h);
    out->loop_frames = h.loop_frames;
    out->loop_fixed_steps = h.loop_fixed_steps;
    if (out->loop_result != DE_OK)
        de__host_log_error("main loop failed");

    de__headless_end(&h);
    h.api->shutdown(e);
    de__game_detach(&h);
    de__hot_reload_shutdown(&h);
    de__engine_destroy(h.engine);
    h.engine = NULL;
#if !DE_ALLOC_GUARD_SEES_HEAP
    out->tracking = de_tracking_allocator_stats(&tracking);
#endif

    de_fs_shutdown();
    if (own_log)
        de_log_shutdown();
    if (own_prof)
        de_prof_shutdown();
}

/* ---- The test ------------------------------------------------------------------------------- */

#if defined(ALLOC_GUARD_SELFTEST)
#define ZERO_ALLOC_SUITE zero_alloc_selftest
#define ZERO_ALLOC_TEST  detects
#else
#define ZERO_ALLOC_SUITE zero_alloc
#define ZERO_ALLOC_TEST  frames_1000
#endif

TEST(ZERO_ALLOC_SUITE, ZERO_ALLOC_TEST) {
    const u64 t0 = de_time_now_ns();
    run_result r;
    run_host(&r);
    const u64 t1 = de_time_now_ns();

    /* The run itself: every frame, every path of the fixture. */
    ASSERT_EQ(r.loop_result, DE_OK);
    ASSERT_TRUE(g_win.began && g_win.ended);
    EXPECT_EQ(r.loop_frames, (u64)TOTAL_FRAMES);
    EXPECT_EQ(r.loop_fixed_steps, (u64)TOTAL_FRAMES);
    const alloc_game_stats *s = alloc_game_last_stats();
    EXPECT_EQ(s->entities, ALLOC_GAME_ENTITIES);
    EXPECT_EQ(s->frames, (u64)TOTAL_FRAMES);
    EXPECT_EQ(s->fixed_steps, (u64)TOTAL_FRAMES);
    EXPECT_EQ(s->fixed_rows, (u64)TOTAL_FRAMES * ALLOC_GAME_ENTITIES);
    EXPECT_GE(s->scratch_uses, (u64)TOTAL_FRAMES * (ALLOC_GAME_ENTITIES / ALLOC_GAME_GRAIN));
    EXPECT_EQ(s->events_sent, (u64)TOTAL_FRAMES * ALLOC_GAME_EVENTS_PER_FRAME);
    EXPECT_EQ(s->events_read, s->events_sent);
    EXPECT_EQ(s->events_missed, 0u);
    EXPECT_EQ(s->tag_commands, (u64)TOTAL_FRAMES * ALLOC_GAME_MARKED);
    /* The tag is on after even frames and off after odd ones. */
    EXPECT_EQ(s->marked_rows, (u64)(TOTAL_FRAMES + 1u) / 2u * ALLOC_GAME_MARKED);
    EXPECT_EQ(s->arena_bytes, (u64)TOTAL_FRAMES * ALLOC_GAME_ARENA_BYTES);
    EXPECT_EQ(s->info_lines,
              (u64)(TOTAL_FRAMES + ALLOC_GAME_INFO_EVERY - 1u) / ALLOC_GAME_INFO_EVERY);
    EXPECT_EQ(s->cvar_sets,
              (u64)(TOTAL_FRAMES + ALLOC_GAME_CVAR_EVERY - 1u) / ALLOC_GAME_CVAR_EVERY);
    if (r.tracking_used)
        EXPECT_EQ(r.tracking.blocks_live, 0u);
    /* The crash frame-end hook formatted the changed cvars at least once inside the window. */
    if (de__host_crash_active())
        EXPECT_GT(g_win.snapshots_end, g_win.snapshots_begin);

    const de_alloc_guard *g = &g_win.guard;
    printf("zero_alloc: %u frames guarded after %u warm-up frames in %.1f ms (whole run %.1f ms), "
           "%u frame workers, %llu cvar snapshots\n",
           MEASURED_FRAMES, WARMUP_FRAMES, (double)(g_win.end_ns - g_win.begin_ns) / 1e6,
           (double)(t1 - t0) / 1e6, r.frame_workers,
           (unsigned long long)(g_win.snapshots_end - g_win.snapshots_begin));
    printf("zero_alloc: engine %llu allocs / %llu frees (heap counted: %s), raw %llu mallocs / "
           "%llu frees (raw counted: %s), checksum %llu\n",
           (unsigned long long)g->engine_allocs, (unsigned long long)g->engine_frees,
           DE_ALLOC_GUARD_SEES_HEAP ? "yes" : "no, tracking allocator",
           (unsigned long long)g->raw_mallocs, (unsigned long long)g->raw_frees,
           de_alloc_guard_counts_raw() ? "yes" : "no", (unsigned long long)s->checksum);
#if defined(ALLOC_GUARD_SELFTEST)
    EXPECT_EQ(s->selftest_allocs, (u64)TOTAL_FRAMES);
#endif
    DE_ALLOC_GUARD_ASSERT_ZERO(g);
    /* Frees are not offences for the guard, but a frame that frees heap memory allocated it
     * somewhere: none either. */
    EXPECT_EQ(g->engine_frees, 0u);
    EXPECT_EQ(g->raw_frees, 0u);
}
