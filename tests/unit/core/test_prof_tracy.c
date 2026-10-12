/* Unit tests: the profiler with the backend this build really has (DENG-94). In a Tracy build
 * (DE_PROF_TEST_TRACY, set by CMake when de_tp_tracy is linked) it starts the Tracy client,
 * streams a few frames of nested zones, plots, memory, lock and log events, and stops it. Without
 * a backend, de_prof_init is DE_ERR_UNSUPPORTED.
 *
 * Manual check that a profiler connects (docs: Trama "core: Profiling"): start the Tracy 0.14
 * profiler GUI, then run
 *     DE_PROF_WAIT_S=30 build/dev/tests/unit/core/test_core --filter=prof_tracy.live_frames
 * The test waits up to that many seconds for a connection, then streams frames for 2 s: the
 * profiler shows thread "prof-tracy-main" with "prof_tracy.frame" > "prof_tracy.outer" >
 * "prof_tracy.inner" zones, the "prof_tracy.value" plot, the "prof_tracy.pool" memory pool, a
 * de_mutex lock and a "core: prof_tracy warning" message. TRACY_PORT changes the port. */
#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS /* getenv */
#endif

#include "de_test.h"

#include <dengine/de_base.h>
#include <dengine/de_debug.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include <stdlib.h>
#include <string.h>

#if defined(DE_PROFILE) && defined(DE_PROF_TEST_TRACY)

static void one_frame(const de_allocator *a, u32 tag, de_mutex *m, u32 i) {
    DE_PROFILE_ZONE_BEGIN(frame, "prof_tracy.frame");
    DE_PROFILE_ZONE_BEGIN(outer, "prof_tracy.outer");
    DE_PROFILE_ZONE_BEGIN_C(inner, "prof_tracy.inner", 0x2080FF);
    void *p = DE_ALLOC(a, 64u + 16u * (i % 8u), 16, tag);
    de_mutex_lock(m);
    de_mutex_unlock(m);
    DE_FREE(a, p, 64u + 16u * (i % 8u));
    DE_PROFILE_ZONE_END(inner);
    DE_PROFILE_PLOT("prof_tracy.value", (f64)(i % 16u));
    DE_PROFILE_ZONE_END(outer);
    DE_PROFILE_ZONE_END(frame);
    DE_PROFILE_FRAME(NULL);
}

TEST(prof_tracy, live_frames) {
    de_log_desc desc;
    memset(&desc, 0, sizeof desc);
    desc.no_console_sink = true;
    ASSERT_EQ(de_prof_init(), DE_OK);
    ASSERT_EQ(de_log_init(&desc), DE_OK);
    ASSERT_TRUE(de_prof_running());
    de_thread_set_name("prof-tracy-main");

    const u32 tag = de_mem_register_tag("prof_tracy.pool");
    de_tracking_allocator storage;
    const de_allocator a = de_tracking_allocator_init(&storage, NULL, tag);
    de_mutex m;
    de_mutex_init(&m);

    /* Manual check: wait for the profiler GUI. */
    const char *wait = getenv("DE_PROF_WAIT_S");
    const u64 wait_s = wait != NULL ? (u64)strtoull(wait, NULL, 10) : 0u;
    if (wait_s > 0) {
        const u64 until = de_time_now_ns() + wait_s * 1000000000ull;
        while (!de_prof_connected() && de_time_now_ns() < until)
            de_sleep_ns(10000000ull);
        EXPECT_TRUE(de_prof_connected());
    }

    DE_WARN(DE_LOG_CAT_CORE, "prof_tracy warning %d", 1);
    const u64 stream_ns = wait_s > 0 ? 2000000000ull : 0u;
    const u64 start = de_time_now_ns();
    u32 frames = 0;
    do {
        one_frame(&a, tag, &m, frames++);
        if (wait_s > 0)
            de_sleep_ns(1000000ull);
    } while (frames < 64u || de_time_now_ns() - start < stream_ns);

    de_mutex_destroy(&m);
    de_log_shutdown();
    de_prof_shutdown();
    EXPECT_FALSE(de_prof_running());
    EXPECT_FALSE(de_prof_connected());
    const de_tracking_stats st = de_tracking_allocator_stats(&storage);
    EXPECT_EQ(st.blocks_live, 0u);
}

#else

TEST(prof_tracy, init_without_backend) {
    EXPECT_EQ(de_prof_init(), DE_ERR_UNSUPPORTED);
    EXPECT_EQ(de_error_last()->result, DE_ERR_UNSUPPORTED);
    EXPECT_FALSE(de_prof_running());
    EXPECT_FALSE(de_prof_connected());
    de_prof_shutdown();
}

#endif
