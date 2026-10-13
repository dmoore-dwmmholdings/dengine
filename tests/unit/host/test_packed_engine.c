/*
 * test_packed_engine.c: an engine running on a real cooked pack in game:/ (DENG-393).
 *
 * `game:/` is tests/data/packed_game/<host platform>/, which holds game.dpak: the fixture project
 * tests/data/cook_project/engine.toml cooked by the real de-cook (format version 2, names
 * stripped). The packs are checked in, because a Release or Shipping de-cook refuses to cook;
 * packed_game.packs_current (check_packed_game.cmake) keeps them equal to a fresh cook in Debug
 * and Dev builds.
 *
 * Shipping: engine create runs the asset system in PACKED mode and mounts every .dpak of game:/
 * itself (DENG-213, src/host/host_packs.c). Other builds run in SOURCE mode, which mounts nothing:
 * the test calls the same de__host_mount_packs on game:/ after create, so the load path below is
 * checked in every build type. With no manifest loaded, the bundles come from the pack.
 *
 * The real main loop (de__loop_run) loads the bundle level_01, which depends on core: the two
 * blobs, a scene and a prefab. The blob bytes are compared with the source files (mounted as
 * pgsrc:/), and the scene and prefab are spawned into the engine's world.
 */
#include "de_test.h"

#include "asset/dpak/dpak_reader.h"
#include "asset/mount_internal.h"
#include "host/host_internal.h"
#include "host/host_packs.h"

#include <dengine/de_asset.h>
#include <dengine/de_ecs.h>
#include <dengine/de_game.h>
#include <dengine/de_platform.h>

#include <stdio.h>
#include <string.h>

#ifndef DE_PACKED_GAME_DIR
#error "DE_PACKED_GAME_DIR must be defined by tests/unit/host/CMakeLists.txt"
#endif
#ifndef DE_PACKED_GAME_OTHER_DIR
#error "DE_PACKED_GAME_OTHER_DIR must be defined by tests/unit/host/CMakeLists.txt"
#endif
#ifndef DE_COOK_PROJECT_ASSETS_DIR
#error "DE_COOK_PROJECT_ASSETS_DIR must be defined by tests/unit/host/CMakeLists.txt"
#endif
#ifndef DE_HOST_TEST_TMP_DIR
#error "DE_HOST_TEST_TMP_DIR must be defined by tests/unit/host/CMakeLists.txt"
#endif

#define PG_EV_MAX     16u
#define PG_TIMEOUT_NS (30ull * 1000000000ull)

static struct {
    de_engine *e;
    de_load_handle h;
    de_event_reader reader;
    de_asset_event got[PG_EV_MAX];
    u32 got_count;
    u32 frames, final_frame;
    bool final;
    u64 start_ns;
} g_pg;

/* Update, main thread: collects the asset events; quits two frames after the request reaches a
 * final state (so a late event would still be seen), or at the timeout. */
static void pg_watch(de_system_ctx *ctx) {
    const de_asset_event *ev;
    while ((ev = (const de_asset_event *)de_event_next(ctx->world, &g_pg.reader)) != NULL) {
        if (g_pg.got_count < PG_EV_MAX)
            g_pg.got[g_pg.got_count] = *ev;
        g_pg.got_count++;
    }
    if (!g_pg.final && de_asset_status(g_pg.e->assets, g_pg.h, NULL) >= DE_LOAD_READY) {
        g_pg.final = true;
        g_pg.final_frame = g_pg.frames;
    }
    if ((g_pg.final && g_pg.frames >= g_pg.final_frame + 2u) ||
        de_time_now_ns() - g_pg.start_ns > PG_TIMEOUT_NS)
        de_engine_request_quit(g_pg.e);
    g_pg.frames++;
}

/* game:/ = `game_dir`; user:/ and cache:/ in the build tree. */
static void pg_fs_init(const char *game_dir) {
    de_fs_desc fd;
    memset(&fd, 0, sizeof fd);
    fd.game_dir = game_dir;
    fd.user_dir = DE_HOST_TEST_TMP_DIR "/user";
    fd.cache_dir = DE_HOST_TEST_TMP_DIR "/cache";
    ASSERT_EQ(de_fs_init(&fd), DE_OK);
}

static de__game_config pg_config(void) {
    de__game_config c;
    de__game_config_defaults(&c);
    c.task_workers = 2;
    c.timing.frame_cap_hz = 500;
    return c;
}

/* The bytes of blob `id` equal the source file `src` (a pgsrc:/ path). */
static void pg_expect_blob(de_asset_system *s, const char *id, const char *src) {
    void *want = NULL;
    u64 want_size = 0;
    ASSERT_EQ(de_file_read_all(src, NULL, &want, &want_size), DE_OK);
    u64 size = 0;
    const void *got = de_blob_data(s, de_asset_blob(s, de_asset_id_from(id)), &size);
    EXPECT_TRUE(got != NULL);
    EXPECT_EQ(size, want_size);
    if (got != NULL && size == want_size)
        EXPECT_MEMEQ(got, want, (size_t)size);
    printf("  %s: %llu bytes, equal to %s\n", id, (unsigned long long)size, src);
    DE_FREE(de_heap_allocator(), want, want_size + 1);
}

/* True if `err` or an error it wraps names `what`. */
static bool pg_error_names(const de_error *err, const char *what) {
    for (; err != NULL; err = err->cause) {
        printf("  error: %s (%s)\n", err->message, de_result_str(err->result));
        if (strstr(err->message, what) != NULL)
            return true;
    }
    return false;
}

TEST(packed_engine, bundle_from_game_pack) {
    pg_fs_init(DE_PACKED_GAME_DIR);
    ASSERT_EQ(de_fs_mount("pgsrc", DE_COOK_PROJECT_ASSETS_DIR, DE_MOUNT_READ_ONLY, 0), DE_OK);

    /* The checked-in pack is a current-format pack for this platform, without names. */
    dpak_reader *r = NULL;
    ASSERT_EQ(dpak_open("game:/game.dpak", &(dpak_open_desc){.verify = true}, &r), DE_OK);
    EXPECT_EQ(dpak_get_header(r)->version, DPAK_VERSION);
    EXPECT_EQ(dpak_get_header(r)->platform, dpak_platform_host());
    EXPECT_TRUE(dpak_has_hashes(r));
    EXPECT_EQ(dpak_entry_count(r), 4u);
    EXPECT_EQ(dpak_bundle_count(r), 2u);
    dpak_close(r);

    de__game_config cfg = pg_config();
    de__engine_desc ed;
    memset(&ed, 0, sizeof ed);
    ed.config = &cfg;
    ed.headless = true;
    de__host h;
    memset(&h, 0, sizeof h);
    ASSERT_EQ(de__engine_create(&ed, &h.engine), DE_OK);
    de_engine *e = h.engine;
    ASSERT_TRUE(e->assets != NULL);

    de__mount_stats ms;
#if defined(DE_SHIPPING)
    /* PACKED: engine create mounted game:/game.dpak. A second mount of it is refused. */
    de__mount_get_stats(e->assets, &ms);
    EXPECT_EQ(ms.mounted, 1u);
    EXPECT_EQ(de_asset_pack_mount(e->assets, "game:/game.dpak", 0), DE_ERR_ALREADY_EXISTS);
    de_error_clear();
#else
    /* SOURCE: nothing mounted by create; the test mounts game:/ the way a packed engine does. */
    de__mount_get_stats(e->assets, &ms);
    EXPECT_EQ(ms.mounted, 0u);
    u32 count = 0;
    ASSERT_EQ(de__host_mount_packs(e->assets, DE__HOST_PACK_DIR, NULL, &count), DE_OK);
    EXPECT_EQ(count, 1u);
#endif

    memset(&g_pg, 0, sizeof g_pg);
    g_pg.e = e;
    de_event_type t = de_asset_event_type(e->assets);
    ASSERT_TRUE(DE_HANDLE_VALID(t));
    g_pg.reader = de_event_reader_create(e->world, t);
    de_system_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.name = "test.packed_watch";
    sd.schedule = DE_SCHEDULE_UPDATE;
    sd.fn = pg_watch;
    sd.flags = DE_SYSTEM_MAIN_THREAD;
    sd.reads_events = &t;
    sd.reads_event_count = 1;
    ASSERT_TRUE(DE_HANDLE_VALID(de_system_add(e->world, &sd)));

    /* level_01 depends on core: data/hello, data/big (core), levels/demo, props/barrel. */
    g_pg.h = de_asset_load_bundle(e->assets, "level_01", DE_LOAD_DEFAULT);
    ASSERT_TRUE(DE_HANDLE_VALID(g_pg.h));
    g_pg.start_ns = de_time_now_ns();
    h.loop_max_frames = 0; /* pg_watch quits */
    EXPECT_EQ(de__loop_run(&h), DE_OK);

    de_load_info info;
    EXPECT_EQ(de_asset_status(e->assets, g_pg.h, &info), DE_LOAD_READY);
    EXPECT_EQ(info.assets_total, 4u);
    EXPECT_EQ(info.assets_ready, 4u);
    EXPECT_EQ(info.assets_failed, 0u);
    if (info.first_error != DE_OK)
        printf("  first error: %s (%s)\n", de_result_str(info.first_error), info.first_error_msg);
    printf("  level_01 READY after %u frames\n", g_pg.final_frame + 1u);

    /* One LOADED per asset, then the REQUEST_DONE. */
    ASSERT_EQ(g_pg.got_count, 5u);
    u32 loaded = 0;
    for (u32 i = 0; i < 4u; ++i)
        loaded += g_pg.got[i].kind == DE_ASSET_EV_LOADED ? 1u : 0u;
    EXPECT_EQ(loaded, 4u);
    EXPECT_EQ(g_pg.got[4].kind, DE_ASSET_EV_REQUEST_DONE);
    EXPECT_EQ(g_pg.got[4].state, DE_LOAD_READY);

    /* Every asset came from the pack: data/big is two chunks, the others one each. */
    de__mount_get_stats(e->assets, &ms);
    EXPECT_EQ(ms.mounted, 1u);
    EXPECT_EQ(ms.jobs, 4u);
    EXPECT_EQ(ms.chunks, 5u);

    /* The blobs hold the source bytes. */
    pg_expect_blob(e->assets, "data/hello", "pgsrc:/data/hello.txt");
    pg_expect_blob(e->assets, "data/big", "pgsrc:/data/big.txt");

    /* The scene and the prefab spawn into the engine's world, with their hierarchy. */
    de_builtin_component_ids bc = de_builtin_components(e->world);
    de_entity root = DE_ENTITY_NULL;
    ASSERT_EQ(de_prefab_instantiate(e->assets, e->world,
                                    de_asset_scene(e->assets, de_asset_id_from("levels/demo")),
                                    DE_ENTITY_NULL, &root),
              DE_OK);
    const DeTransform *tr = (const DeTransform *)de_entity_get_const(e->world, root, bc.transform);
    ASSERT_TRUE(tr != NULL);
    EXPECT_TRUE(tr->position.x == 0.0);
    const DeChildren *ch = (const DeChildren *)de_entity_get_const(e->world, root, bc.children);
    ASSERT_TRUE(ch != NULL);
    EXPECT_EQ(ch->count, 1u);
    tr = (const DeTransform *)de_entity_get_const(e->world, ch->first, bc.transform);
    ASSERT_TRUE(tr != NULL);
    EXPECT_TRUE(tr->position.x == 2.0); /* entity 0xa2 of demo.scene.toml */
    EXPECT_TRUE(tr->position.z == 1.5);

    ASSERT_EQ(de_prefab_instantiate(e->assets, e->world,
                                    de_asset_prefab(e->assets, de_asset_id_from("props/barrel")),
                                    DE_ENTITY_NULL, &root),
              DE_OK);
    ch = (const DeChildren *)de_entity_get_const(e->world, root, bc.children);
    ASSERT_TRUE(ch != NULL);
    EXPECT_EQ(ch->count, 1u);
    tr = (const DeTransform *)de_entity_get_const(e->world, ch->first, bc.transform);
    ASSERT_TRUE(tr != NULL);
    EXPECT_TRUE(tr->position.z == 1.0); /* entity 0x2 of barrel.prefab.toml */

    de_asset_release(e->assets, g_pg.h);
    de__engine_destroy(h.engine);
    de_fs_shutdown();
}

/* A pack cooked for another platform in game:/. Shipping: engine create fails, and the error
 * names the pack. Other builds: the same mount, done by the test, fails. */
TEST(packed_engine, other_platform_pack) {
    pg_fs_init(DE_PACKED_GAME_OTHER_DIR);
    de__game_config cfg = pg_config();
    de__engine_desc ed;
    memset(&ed, 0, sizeof ed);
    ed.config = &cfg;
    ed.headless = true;
    de_engine *e = NULL;
#if defined(DE_SHIPPING)
    de_error_clear();
    EXPECT_EQ(de__engine_create(&ed, &e), DE_ERR_UNSUPPORTED);
    EXPECT_TRUE(e == NULL);
    EXPECT_TRUE(pg_error_names(de_error_last(), "game.dpak"));
    de_error_clear();
#else
    ASSERT_EQ(de__engine_create(&ed, &e), DE_OK);
    u32 count = 0;
    de_error_clear();
    EXPECT_EQ(de__host_mount_packs(e->assets, DE__HOST_PACK_DIR, NULL, &count), DE_ERR_UNSUPPORTED);
    EXPECT_EQ(count, 0u);
    EXPECT_TRUE(pg_error_names(de_error_last(), "game.dpak"));
    de_error_clear();
    de__mount_stats ms;
    de__mount_get_stats(e->assets, &ms);
    EXPECT_EQ(ms.mounted, 0u);
    de__engine_destroy(e);
#endif
    de_fs_shutdown();
}
