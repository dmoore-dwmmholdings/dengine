/*
 * test_asset_host.c: real asset loads through the app host (DENG-143).
 *
 * A headless engine is created through the host internals (de__engine_create, as de_host_run
 * does), so it has the asset system the host creates, attached to the world. The test registers
 * the `test_bytes` fixture type (with a loader), loads the two-file bundle of data/host/
 * (mounted as dev:/) and runs the real main loop (de__loop_run) until the bundle is READY. A
 * main-thread Update system reads the asset events: one LOADED per asset and one REQUEST_DONE.
 */
#include "de_test.h"
#include "test_bytes_type.h"

#include "host/host_internal.h"
#include "platform/de_atomic.h"

#include <dengine/de_asset.h>
#include <dengine/de_ecs.h>
#include <dengine/de_game.h>
#include <dengine/de_platform.h>

#include <stdio.h>
#include <string.h>

#ifndef DE_ASSET_TEST_DATA_DIR
#error "DE_ASSET_TEST_DATA_DIR must be defined by tests/unit/asset/CMakeLists.txt"
#endif
#ifndef DE_ASSET_HOST_TMP_DIR
#error "DE_ASSET_HOST_TMP_DIR must be defined by tests/unit/asset/CMakeLists.txt"
#endif

#define HOST_EV_MAX     16u
#define HOST_TIMEOUT_NS (20ull * 1000000000ull)
/* Two small files load in milliseconds. On the Windows CI runner an uncapped loop took 1-19 s
 * while the opens ran on the below-normal background workers (DENG-386). */
#define HOST_READY_NS (5ull * 1000000000ull)

typedef struct host_obj {
    u64 size;
    char first;
} host_obj;

static host_obj g_objs[2];
/* Loaders run on frame workers, two at a time (DENG-386: a plain ++ lost a count under load). */
static de_atomic_u32 g_loads;

/* test_bytes loader: the object records the size and first byte of the source. */
static de_result host_bytes_load(de_asset_load_ctx *ctx) {
    u64 size = 0;
    const void *data = de_asset_load_ctx_data(ctx, &size);
    u32 i = de_asset_load_ctx_id(ctx).hash == de_asset_id_from("host/one").hash ? 0u : 1u;
    g_objs[i].size = size;
    g_objs[i].first = size != 0 ? ((const char *)data)[0] : '\0';
    (void)de_atomic_fetch_add_u32(&g_loads, 1, DE_ATOMIC_RELAXED);
    de_asset_load_ctx_set_object(ctx, &g_objs[i], size);
    return DE_OK;
}

static struct {
    de_engine *e;
    de_load_handle h;
    de_event_reader reader;
    de_asset_event got[HOST_EV_MAX];
    u32 got_count;
    u32 frames, ready_frame;
    bool ready;
    u64 start_ns, ready_ns;
} g_host;

/* Update, main thread: reads the asset events every frame; quits two frames after READY (so a
 * late event would still be seen), or at the timeout. */
static void host_watch(de_system_ctx *ctx) {
    const de_asset_event *ev;
    while ((ev = (const de_asset_event *)de_event_next(ctx->world, &g_host.reader)) != NULL) {
        if (g_host.got_count < HOST_EV_MAX)
            g_host.got[g_host.got_count] = *ev;
        g_host.got_count++;
    }
    if (!g_host.ready && de_asset_status(g_host.e->assets, g_host.h, NULL) >= DE_LOAD_READY) {
        g_host.ready = true;
        g_host.ready_frame = g_host.frames;
        g_host.ready_ns = de_time_now_ns();
    }
    if ((g_host.ready && g_host.frames >= g_host.ready_frame + 2u) ||
        de_time_now_ns() - g_host.start_ns > HOST_TIMEOUT_NS)
        de_engine_request_quit(g_host.e);
    g_host.frames++;
}

/* Loads the bundle through the host with the loop capped at `frame_cap_hz` (0: uncapped). */
static void run_bundle_ready(u32 frame_cap_hz) {
    de_fs_desc fd;
    memset(&fd, 0, sizeof fd);
    fd.game_dir = DE_ASSET_HOST_TMP_DIR;
    fd.user_dir = DE_ASSET_HOST_TMP_DIR "/user";
    fd.cache_dir = DE_ASSET_HOST_TMP_DIR "/cache";
    ASSERT_EQ(de_fs_init(&fd), DE_OK);
    ASSERT_EQ(de_fs_mount("dev", DE_ASSET_TEST_DATA_DIR "/host", DE_MOUNT_READ_ONLY, 0), DE_OK);

    de__game_config cfg;
    de__game_config_defaults(&cfg);
    cfg.task_workers = 2;
    cfg.timing.frame_cap_hz = frame_cap_hz;
    de__engine_desc ed;
    memset(&ed, 0, sizeof ed);
    ed.config = &cfg;
    ed.headless = true;
    de__host h;
    memset(&h, 0, sizeof h);
    ASSERT_EQ(de__engine_create(&ed, &h.engine), DE_OK);
    de_engine *e = h.engine;
    memset(&g_host, 0, sizeof g_host);
    memset(g_objs, 0, sizeof g_objs);
    de_atomic_init_u32(&g_loads, 0);
    g_host.e = e;

    /* The host created the asset system and attached it before any game code. */
    ASSERT_TRUE(e->assets != NULL);
    de_event_type t = de_asset_event_type(e->assets);
    ASSERT_TRUE(DE_HANDLE_VALID(t));
    const DeAssetSystemRef *ref =
        (const DeAssetSystemRef *)de_resource_get(e->world, de_asset_resource(e->assets));
    ASSERT_TRUE(ref != NULL);
    EXPECT_TRUE(ref->ptr == e->assets);

    test_bytes_counters counters;
    memset(&counters, 0, sizeof counters);
    de_asset_type_desc td = test_bytes_desc(&counters);
    td.load = host_bytes_load;
    ASSERT_TRUE(DE_HANDLE_VALID(de_asset_type_register(e->assets, &td)));
    ASSERT_EQ(de_asset_manifest_load(e->assets, "dev:/assets.toml"), DE_OK);

    g_host.reader = de_event_reader_create(e->world, t);
    de_system_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.name = "test.asset_watch";
    sd.schedule = DE_SCHEDULE_UPDATE;
    sd.fn = host_watch;
    sd.flags = DE_SYSTEM_MAIN_THREAD;
    sd.reads_events = &t;
    sd.reads_event_count = 1;
    ASSERT_TRUE(DE_HANDLE_VALID(de_system_add(e->world, &sd)));

    g_host.h = de_asset_load_bundle(e->assets, "pair", DE_LOAD_DEFAULT);
    ASSERT_TRUE(DE_HANDLE_VALID(g_host.h));
    g_host.start_ns = de_time_now_ns();
    h.loop_max_frames = 0; /* host_watch quits: two frames after READY, or at the timeout */
    EXPECT_EQ(de__loop_run(&h), DE_OK);

    de_load_info info;
    EXPECT_EQ(de_asset_status(e->assets, g_host.h, &info), DE_LOAD_READY);
    EXPECT_EQ(info.assets_total, 2u);
    EXPECT_EQ(info.assets_ready, 2u);
    EXPECT_EQ(de_atomic_load_u32(&g_loads, DE_ATOMIC_ACQUIRE), 2u);
    EXPECT_EQ(g_objs[0].first, 'o');
    EXPECT_EQ(g_objs[1].first, 't');
    EXPECT_GT(g_objs[0].size, 0u);
    EXPECT_GT(g_objs[1].size, g_objs[0].size);

    /* One LOADED per asset (completion order) and then one REQUEST_DONE, nothing else. */
    ASSERT_EQ(g_host.got_count, 3u);
    u64 one = de_asset_id_from("host/one").hash, two = de_asset_id_from("host/two").hash;
    EXPECT_EQ(g_host.got[0].kind, DE_ASSET_EV_LOADED);
    EXPECT_EQ(g_host.got[1].kind, DE_ASSET_EV_LOADED);
    EXPECT_TRUE((g_host.got[0].id.hash == one && g_host.got[1].id.hash == two) ||
                (g_host.got[0].id.hash == two && g_host.got[1].id.hash == one));
    EXPECT_EQ(g_host.got[0].state, DE_LOAD_READY);
    EXPECT_EQ(g_host.got[2].kind, DE_ASSET_EV_REQUEST_DONE);
    EXPECT_EQ(g_host.got[2].handle.id, g_host.h.id);
    EXPECT_EQ(g_host.got[2].state, DE_LOAD_READY);
    EXPECT_EQ(g_host.got[2].error, DE_OK);
    printf("  bundle READY after %u frames (%.1f ms, frame cap %u Hz)\n", g_host.ready_frame + 1u,
           (double)(g_host.ready_ns - g_host.start_ns) / 1e6, frame_cap_hz);
    EXPECT_TRUE(g_host.ready);
    EXPECT_LT(g_host.ready_ns - g_host.start_ns, HOST_READY_NS);

    de_asset_release(e->assets, g_host.h);
    de__engine_destroy(h.engine);
    /* The asset system unloaded both objects through test_bytes. */
    EXPECT_EQ(counters.unloads, 2u);
    de_fs_shutdown();
}

TEST(asset_host, bundle_ready) {
    run_bundle_ready(500);
}

/* The loop a headless server or a benchmark runs: no pacing at all (DENG-386). */
TEST(asset_host, bundle_ready_uncapped) {
    run_bundle_ready(0);
}
