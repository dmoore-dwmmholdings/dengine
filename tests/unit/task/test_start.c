/* Unit tests of the task system's start latch (src/task/scheduler.c, worker.c; DENG-416).
 *
 * de_task_system_create returns only once every thread it started runs and has touched its
 * thread-local storage. Before DENG-416 a thread could start after create returned, and on macOS
 * its first thread-local access made dyld allocate the thread's TLS block (4640 bytes) inside
 * the caller's no-heap region (DENG-415: 20 of 300 runs of a render test beside 20 CPU hogs).
 * start.create_under_load_then_no_heap creates task systems while busy threads keep every core
 * loaded, and runs a guarded region right after each create, long enough for a late thread to
 * start inside it (without the latch it fails in every run on the macOS host). */
#include "de_test.h"

#include <dengine/de_math.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_task.h>

#include <stdio.h>

#include "de_alloc_guard.h"
#include "platform/de_atomic.h"

#define LOAD_MAX 64u

/* Busy threads that load every core while `run` is set and sleep otherwise. The load is on only
 * around each create and its guarded region: on Windows, which schedules strictly by priority, a
 * permanent load would starve the below-normal background workers when the system is destroyed
 * (their join waits for the OS's anti-starvation boost), and it would slow other tests that run
 * in parallel (the test runs serially; see CMakeLists.txt). */
typedef struct load_state {
    de_atomic_u32 started;
    de_atomic_u32 run;
    de_atomic_u32 stop;
} load_state;

static i32 load_main(void *p) {
    load_state *l = (load_state *)p;
    de_atomic_fetch_add_u32(&l->started, 1u, DE_ATOMIC_RELEASE);
    u64 x = 0x9E3779B97F4A7C15ull;
    while (!de_atomic_load_u32(&l->stop, DE_ATOMIC_ACQUIRE)) {
        if (!de_atomic_load_u32(&l->run, DE_ATOMIC_ACQUIRE)) {
            de_sleep_ns(500000); /* 0.5 ms */
            continue;
        }
        for (u32 i = 0; i < 4096u; ++i) /* busy work: keep the core */
            x ^= x << 13, x ^= x >> 7, x ^= x << 17;
    }
    return (i32)(x & 1u);
}

static void bg_noop(void *p) {
    (void)p;
}

TEST(start, create_under_load_then_no_heap) {
    de_fp_env_init_thread();
    /* One busy thread per core, all started before the first create (their own start-up must not
     * land in a guarded region either). */
    load_state load;
    de_atomic_init_u32(&load.started, 0);
    de_atomic_init_u32(&load.run, 0);
    de_atomic_init_u32(&load.stop, 0);
    u32 n_load = de_cpu_logical_cores();
    if (n_load > LOAD_MAX)
        n_load = LOAD_MAX;
    de_thread *threads[LOAD_MAX];
    u32 made = 0;
    for (; made < n_load; ++made)
        if (de_thread_create(load_main, &load, "de-test-load", DE_THREAD_NORMAL, 0,
                             &threads[made]) != DE_OK)
            break;
    ASSERT_GT(made, 0u);
    while (de_atomic_load_u32(&load.started, DE_ATOMIC_ACQUIRE) < made)
        de_sleep_ns(100000);

    /* Five rounds: on the macOS host the first round already fails without the latch. Windows
     * CI runners hand busy threads long quanta, so each round takes several seconds there. */
    u32 failures = 0;
    for (u32 round = 0; round < 5u; ++round) {
        de_atomic_store_u32(&load.run, 1u, DE_ATOMIC_RELEASE);
        de_sleep_ns(2u * 1000u * 1000u); /* the busy threads have the cores */
        de_task_system *ts = NULL;
        const de_result r = de_task_system_create(
            &(de_task_system_desc){.worker_count = 4, .background_worker_count = 2}, &ts);
        de_alloc_guard g = {0};
        if (r == DE_OK) {
            de_alloc_guard_begin(&g);
            de_sleep_ns(20u * 1000u * 1000u); /* a thread that had not started would start here */
            de_alloc_guard_end(&g);
        }
        de_atomic_store_u32(&load.run, 0u, DE_ATOMIC_RELEASE);
        if (r != DE_OK) {
            failures = ~0u;
            break;
        }
        if (de_alloc_guard_offences(&g) != 0) {
            ++failures;
            printf("round %u: heap traffic after de_task_system_create returned "
                   "(a thread started late):\n",
                   round);
            de_alloc_guard_report(&g, __FILE__, __LINE__); /* the sites */
        }
        /* The system works: a background task runs (without the load). */
        de_task_counter c = {0};
        de_task_submit(ts, DE_TASK_POOL_BACKGROUND, bg_noop, NULL, &c);
        de_task_wait(ts, &c);
        de_task_system_destroy(ts);
    }
    de_atomic_store_u32(&load.stop, 1u, DE_ATOMIC_RELEASE);
    for (u32 i = 0; i < made; ++i)
        de_thread_join(threads[i]);
    EXPECT_EQ(failures, 0u);
}
