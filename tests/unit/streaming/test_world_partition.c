/*
 * test_world_partition.c: the world partition (DENG-271, src/world/streaming/world_partition.c)
 * over a real asset system in SOURCE mode: the cell scenes are imported in-process, so these
 * tests need the cooker (DE_HAS_COOKER: Debug and Dev with tools) and skip elsewhere.
 *
 *   stream_path   a synthetic 10 km world (3 rows of 40 cells), cooked by `de-cook world` into
 *                 the build tree; a camera flies along it. Loads start nearest first, never more
 *                 than max_loads_per_frame per frame, and memory returns to the baseline.
 *   hysteresis    world_small: a cell loads inside load_radius, stays loaded up to
 *                 unload_radius, and does not reload until it is inside load_radius again.
 *   far_origin    world_small: the entity at (100000, 100000.125, 3) lands within 1 mm.
 *   proxy_fade    world_small: the far proxy shows for unloaded cells and fades out on load.
 *   no_alloc      steady update frames allocate nothing.
 *   editor        world_small, the editor extensions (DENG-831, partition_editor.h): editor
 *                 sources, forces, the dry-run preview, per-cell bytes and entities.
 *   packed        world_small plus far proxies, packed by `de-cook cook` (DENG-856): PACKED mode
 *                 finds the same cells and proxies as SOURCE mode, with or without the manifest.
 *   terrain       a DeTerrain across a cell boundary (DENG-857): cells.toml lists its tiles per
 *                 cell, a cell load makes its tiles resident (a shared tile once), the last
 *                 unload releases them, and memory returns to the baseline after a flight.
 */
#include "de_test.h"
#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_ecs.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_reflect.h>
#include <dengine/de_task.h>
#include "world/streaming/partition_editor.h"
#include "world/streaming/streaming_internal.h"

#if defined(DE_STREAMING_HAS_WORLD_COOK)
#include "cook/cook_cmd.h"
#include "cook/world_cells.h"
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DE_HAS_COOKER
#define DE_HAS_COOKER 0
#endif

#define FIXTURE DE_STREAMING_FIXTURE_DIR
#define TMP DE_STREAMING_TMP_DIR
#define NO_COOKER_MSG "this build does not link the cooker (DE_HAS_COOKER: Debug and Dev with tools)"

/* ---- Fixture -------------------------------------------------------------------------------- */

#define MAX_EVENTS 512

typedef struct cell_event {
    i32 cx, cy;
    bool loaded;
    de_entity root;
} cell_event;

typedef struct wp_fx {
    de_tracking_allocator t_asset, t_world, t_part;
    de_allocator a_asset, a_world, a_part;
    de_type_registry *reg;
    de_task_system *tasks;
    de_asset_system *s;
    de_world *w;
    de_world_partition *p;
    cell_event ev[MAX_EVENTS];
    u32 ev_count;
} wp_fx;

static void on_cell(void *user, i32 cx, i32 cy, de_entity root, bool loaded) {
    wp_fx *f = (wp_fx *)user;
    if (f->ev_count < MAX_EVENTS)
        f->ev[f->ev_count++] = (cell_event){cx, cy, loaded, root};
}

static void wp_fx_setup(wp_fx *f) {
    memset(f, 0, sizeof *f);
    de_log_desc ld;
    memset(&ld, 0, sizeof ld);
    ld.no_console_sink = true;
    ASSERT_EQ(de_log_init(&ld), DE_OK);
    de_fp_env_init_thread();
    f->a_asset = de_tracking_allocator_init(&f->t_asset, NULL, DE_MEM_ASSET);
    f->a_world = de_tracking_allocator_init(&f->t_world, NULL, DE_MEM_ECS);
    f->a_part = de_tracking_allocator_init(&f->t_part, NULL, DE_MEM_ASSET);
    ASSERT_EQ(de_type_registry_create(NULL, &f->reg), DE_OK);
    ASSERT_EQ(de_task_system_create(&(de_task_system_desc){.worker_count = 2}, &f->tasks), DE_OK);
}

static void wp_fx_teardown(wp_fx *f) {
    de_world_partition_close(f->p);
    if (f->w != NULL)
        de_world_destroy(f->w);
    if (f->s != NULL)
        de_asset_system_destroy(f->s);
    de_task_system_destroy(f->tasks);
    de_type_registry_destroy(f->reg);
    EXPECT_EQ(de_tracking_allocator_stats(&f->t_asset).bytes_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->t_world).bytes_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->t_part).bytes_live, 0u);
    de_log_shutdown();
}

/* World first (it registers the ECS built-in types the scene importer reads), then the asset
 * system on `manifest`, then the partition on world `name`. */
static void wp_open(wp_fx *f, const char *manifest, const char *name, u32 max_resident) {
    ASSERT_EQ(de_world_create(&(de_world_desc){.allocator = &f->a_world, .types = f->reg}, &f->w),
              DE_OK);
    ASSERT_TRUE(DE_HANDLE_VALID(de_builtin_components(f->w).transform));
    de_asset_system_desc d = {.allocator = &f->a_asset,
                              .tasks = f->tasks,
                              .io_threads = 1,
                              .cache_dir = "nocache:/decache",
                              .types = f->reg};
    ASSERT_EQ(de_asset_system_create(&d, &f->s), DE_OK);
    ASSERT_EQ(de_asset_manifest_load(f->s, manifest), DE_OK);
    de_world_partition_desc pd = {.allocator = &f->a_part,
                                  .max_resident_cells = max_resident,
                                  .on_cell = on_cell,
                                  .user = f};
    ASSERT_EQ(de_world_partition_open(f->s, f->w, name, &pd, &f->p), DE_OK);
}

static void set_camera(wp_fx *f, f64 x, f64 y) {
    de_streaming_source src = {x, y, 0.0, 0.0f};
    de_asset_set_streaming_sources(f->s, &src, 1);
}

/* The asset update plus DE_SCHEDULE_FIRST, as the host runs them: the de.asset_events system
 * sends (and frees) the queued asset events there. */
static void asset_pump(wp_fx *f) {
    de_asset_system_update(f->s, f->w);
    (void)de_world_run_schedule(f->w, DE_SCHEDULE_FIRST, 0.0f, 0.0f);
}

/* One frame: the asset system applies finished loads, then the partition updates. */
static de_world_partition_stats frame(wp_fx *f, f32 dt) {
    asset_pump(f);
    EXPECT_EQ(de_world_partition_update(f->p, dt), DE_OK);
    return de_world_partition_get_stats(f->p);
}

/* Frames until nothing is loading (at most ~10 s). */
static de_world_partition_stats settle(wp_fx *f, f32 dt) {
    de_world_partition_stats st = frame(f, dt);
    for (u32 i = 0; i < 10000 && (st.cells_loading > 0 || st.loads_started > 0); ++i) {
        de_sleep_ns(1000000ull);
        st = frame(f, dt);
    }
    return st;
}

static void mount_once(const char *root, const char *dir, u32 flags) {
    if (!de_path_exists(root)) /* best effort: tests mount each root once per process */
        (void)de_fs_mount(root, dir, flags, 0);
}

/* ---- world_small ----------------------------------------------------------------------------- */

static bool g_small_mounted;
static void small_open(wp_fx *f, u32 max_resident) {
    if (!g_small_mounted) {
        ASSERT_EQ(de_fs_mount("wsmall", FIXTURE, DE_MOUNT_READ_ONLY, 0), DE_OK);
        g_small_mounted = true;
    }
    wp_open(f, "wsmall:/assets.toml", "small", max_resident);
}

TEST_F(streaming, world_partition_open, wp_fx) {
    small_open(f, 0);
    de_world_partition_stats st = de_world_partition_get_stats(f->p);
    EXPECT_EQ(st.cell_count, 4u);
    EXPECT_EQ(st.cells_loaded, 0u);
    de_asset_world_info info;
    ASSERT_EQ(de_asset_world_info_get(f->s, "small", &info), DE_OK);
    EXPECT_EQ(info.cell_size, 256u);
    EXPECT_FLOAT_EQ(info.load_radius, 384.0f, 0.0f);
    EXPECT_FLOAT_EQ(info.unload_radius, 512.0f, 0.0f);
    EXPECT_EQ(info.max_loads_per_frame, 2u);
    EXPECT_EQ(info.scene_count, 2u);
    EXPECT_TRUE(info.proxy_enabled);
    EXPECT_FLOAT_EQ(info.proxy_fade_seconds, 0.25f, 1e-6f);
    EXPECT_EQ(de_asset_world_info_get(f->s, "nope", &info), DE_ERR_NOT_FOUND);
    de_world_partition *q = NULL;
    EXPECT_NE(de_world_partition_open(f->s, f->w, "nope", NULL, &q), DE_OK);
    EXPECT_TRUE(q == NULL);
    /* No sources: nothing loads. */
    st = frame(f, 0.016f);
    EXPECT_EQ(st.loads_started, 0u);
    EXPECT_EQ(de_world_partition_cell_state(f->p, 0, 0), DE_WORLD_CELL_UNLOADED);
    EXPECT_EQ(de_world_partition_cell_state(f->p, 7, 7), DE_WORLD_CELL_UNLOADED); /* no such cell */
}

/* ---- test_world_partition_hysteresis --------------------------------------------------------- */

TEST_F(streaming, world_partition_hysteresis, wp_fx) {
#if !DE_HAS_COOKER
    SKIP(NO_COOKER_MSG);
#endif
    small_open(f, 0);
    /* Middle of cell (0, 0): (0, 0) and (1, 0) (128 m away) load; (-1, -3) is 652 m away. */
    set_camera(f, 128.0, 128.0);
    de_world_partition_stats st = settle(f, 0.016f);
    EXPECT_EQ(de_world_partition_cell_state(f->p, 0, 0), DE_WORLD_CELL_LOADED);
    EXPECT_EQ(de_world_partition_cell_state(f->p, 1, 0), DE_WORLD_CELL_LOADED);
    EXPECT_EQ(de_world_partition_cell_state(f->p, -1, -3), DE_WORLD_CELL_UNLOADED);
    EXPECT_EQ(st.cells_loaded, 2u);
    /* x = -150: (1, 0) is 406 m away, between the radii: it stays. */
    set_camera(f, -150.0, 128.0);
    st = settle(f, 0.016f);
    EXPECT_EQ(de_world_partition_cell_state(f->p, 1, 0), DE_WORLD_CELL_LOADED);
    /* x = -300: 556 m, past unload_radius: it goes. */
    set_camera(f, -300.0, 128.0);
    st = frame(f, 0.016f);
    EXPECT_EQ(st.unloads, 1u);
    EXPECT_EQ(de_world_partition_cell_state(f->p, 1, 0), DE_WORLD_CELL_UNLOADED);
    EXPECT_EQ(de_world_partition_cell_state(f->p, 0, 0), DE_WORLD_CELL_LOADED);
    /* Back to x = -150: 406 m is outside load_radius, so it does not reload. */
    set_camera(f, -150.0, 128.0);
    for (u32 i = 0; i < 20; ++i)
        st = frame(f, 0.016f);
    EXPECT_EQ(de_world_partition_cell_state(f->p, 1, 0), DE_WORLD_CELL_UNLOADED);
    /* x = 0: 256 m, inside load_radius: it reloads. */
    set_camera(f, 0.0, 128.0);
    st = settle(f, 0.016f);
    EXPECT_EQ(de_world_partition_cell_state(f->p, 1, 0), DE_WORLD_CELL_LOADED);
    EXPECT_EQ(st.total_unloads, 1u);
    EXPECT_EQ(st.total_loads, 3u);
    /* An entity with DeStreamingSource is a source too: clear the asset-system ones, put an
     * entity at (-1, -3)'s centre. */
    de_asset_set_streaming_sources(f->s, NULL, 0);
    de_component c_src = de_component_find(f->w, "DeStreamingSource");
    ASSERT_TRUE(DE_HANDLE_VALID(c_src));
    de_entity cam = de_entity_spawn(f->w);
    de_transform *tf = (de_transform *)de_entity_add(f->w, cam, de_builtin_components(f->w).transform);
    ASSERT_TRUE(tf != NULL);
    tf->position = (de_dvec3){-128.0, -640.0, 0.0};
    tf->rotation.w = 1.0f;
    tf->scale.x = tf->scale.y = tf->scale.z = 1.0f;
    DeStreamingSource *ss = (DeStreamingSource *)de_entity_add(f->w, cam, c_src);
    ASSERT_TRUE(ss != NULL);
    ss->radius = 100.0f;
    st = settle(f, 0.016f);
    EXPECT_EQ(de_world_partition_cell_state(f->p, -1, -3), DE_WORLD_CELL_LOADED);
    EXPECT_EQ(de_world_partition_cell_state(f->p, 0, 0), DE_WORLD_CELL_UNLOADED);
    EXPECT_EQ(st.cells_loaded, 1u);
}

/* ---- test_world_partition_far_origin --------------------------------------------------------- */

/* Finds, below `e`, an entity whose global position is nearest `want`; returns its distance. */
static f64 nearest_global(de_world *w, de_entity e, de_dvec3 want, de_component gt) {
    f64 best = 1e300;
    for (de_entity c = de_entity_children_first(w, e); c.bits != 0;
         c = de_entity_children_next(w, c)) {
        const de_transform *g = (const de_transform *)de_entity_get_const(w, c, gt);
        if (g != NULL) {
            f64 dx = g->position.x - want.x, dy = g->position.y - want.y,
                dz = g->position.z - want.z;
            f64 d = sqrt(dx * dx + dy * dy + dz * dz);
            if (d < best)
                best = d;
        }
        f64 sub = nearest_global(w, c, want, gt);
        if (sub < best)
            best = sub;
    }
    return best;
}

TEST_F(streaming, world_partition_far_origin, wp_fx) {
#if !DE_HAS_COOKER
    SKIP(NO_COOKER_MSG);
#endif
    small_open(f, 0);
    set_camera(f, 100000.0, 100000.0);
    de_world_partition_stats st = settle(f, 0.016f);
    EXPECT_EQ(st.cells_loaded, 1u);
    ASSERT_EQ(de_world_partition_cell_state(f->p, 390, 390), DE_WORLD_CELL_LOADED);
    ASSERT_TRUE(f->ev_count == 1u && f->ev[0].loaded);
    EXPECT_EQ(f->ev[0].cx, 390);
    EXPECT_EQ(f->ev[0].cy, 390);
    de_entity root = f->ev[0].root;
    /* The cell root sits at the cell origin and carries DeWorldCell. */
    de_builtin_component_ids b = de_builtin_components(f->w);
    const de_transform *rt = (const de_transform *)de_entity_get_const(f->w, root, b.transform);
    ASSERT_TRUE(rt != NULL);
    EXPECT_FLOAT_EQ(rt->position.x, 99840.0, 0.0);
    EXPECT_FLOAT_EQ(rt->position.y, 99840.0, 0.0);
    const DeWorldCell *wc =
        (const DeWorldCell *)de_entity_get_const(f->w, root, de_component_find(f->w, "DeWorldCell"));
    ASSERT_TRUE(wc != NULL);
    EXPECT_EQ(wc->cx, 390);
    EXPECT_EQ(wc->cy, 390);
    /* After the transform pass the entity is where the source scene put it, within 1 mm. */
    de_world_run_schedule(f->w, DE_SCHEDULE_POST_UPDATE, 0.0f, 0.0f);
    f64 d = nearest_global(f->w, root, (de_dvec3){100000.0, 100000.125, 3.0}, b.global_transform);
    EXPECT_TRUE(d < 0.001);
    if (!(d < 0.001))
        printf("  far entity off by %.9f m\n", d);
    /* Camera-relative: the cell origin relative to a camera at the entity, plus the local
     * offset, is ~0 in f32. */
    const i32 cell[2] = {390, 390};
    de_vec3 rel;
    de_world_cell_origins_relative(cell, 1, (de_dvec3){100000.0, 100000.125, 3.0}, &rel);
    EXPECT_TRUE(fabsf(rel.x + 160.0f) < 0.001f);
    EXPECT_TRUE(fabsf(rel.y + 160.125f) < 0.001f);
}

/* ---- far proxies ----------------------------------------------------------------------------- */

TEST_F(streaming, world_partition_proxy_fade, wp_fx) {
#if !DE_HAS_COOKER
    SKIP(NO_COOKER_MSG);
#endif
    small_open(f, 0);
    bool vis = false;
    f32 alpha = 0.0f;
    /* Before any update every cell shows its proxy. */
    EXPECT_TRUE(de_world_partition_proxy(f->p, 0, 0, &vis, &alpha));
    EXPECT_TRUE(vis);
    EXPECT_FLOAT_EQ(alpha, 1.0f, 0.0);
    EXPECT_FALSE(de_world_partition_proxy(f->p, 5, 5, &vis, &alpha)); /* no such cell */
    EXPECT_FALSE(vis);
    /* Load (0, 0) with frames of dt = 0 so the fade has not started when it lands. */
    set_camera(f, 128.0, 128.0);
    de_world_partition_stats st = settle(f, 0.0f);
    ASSERT_EQ(de_world_partition_cell_state(f->p, 0, 0), DE_WORLD_CELL_LOADED);
    (void)st;
    EXPECT_TRUE(de_world_partition_proxy(f->p, 0, 0, &vis, &alpha));
    EXPECT_TRUE(vis);
    EXPECT_FLOAT_EQ(alpha, 1.0f, 0.0);
    /* 0.25 s fade: 0.1 s steps go 0.6, 0.2, then off. */
    frame(f, 0.1f);
    de_world_partition_proxy(f->p, 0, 0, &vis, &alpha);
    EXPECT_TRUE(vis);
    EXPECT_FLOAT_EQ(alpha, 0.6f, 1e-5f);
    frame(f, 0.1f);
    de_world_partition_proxy(f->p, 0, 0, &vis, &alpha);
    EXPECT_FLOAT_EQ(alpha, 0.2f, 1e-5f);
    frame(f, 0.1f);
    de_world_partition_proxy(f->p, 0, 0, &vis, &alpha);
    EXPECT_FALSE(vis);
    /* The far cell still shows its proxy. */
    de_world_partition_proxy(f->p, 390, 390, &vis, &alpha);
    EXPECT_TRUE(vis);
    EXPECT_FLOAT_EQ(alpha, 1.0f, 0.0);
    /* Fly away: (0, 0) unloads and its proxy is back at once. */
    set_camera(f, 5000.0, 5000.0);
    frame(f, 0.1f);
    EXPECT_EQ(de_world_partition_cell_state(f->p, 0, 0), DE_WORLD_CELL_UNLOADED);
    de_world_partition_proxy(f->p, 0, 0, &vis, &alpha);
    EXPECT_TRUE(vis);
    EXPECT_FLOAT_EQ(alpha, 1.0f, 0.0);
}

/* ---- test_world_partition_stream_path -------------------------------------------------------- */

#define PATH_CELLS_X 40 /* 40 * 256 m = 10.24 km */
#define PATH_ROWS 3     /* cy = -1, 0, 1 */

#if defined(DE_STREAMING_HAS_WORLD_COOK)
static void path_cook_sink(void *user, de_cook_stream stream, const char *text, size_t len) {
    (void)user;
    if (stream == DE_COOK_STDERR)
        printf("%.*s", (int)len, text);
}
#endif

/* Writes the synthetic world under <TMP>/path10k and cooks it. False when it cannot. */
static bool path_world_make(void) {
#if defined(DE_STREAMING_HAS_WORLD_COOK)
    static bool made;
    if (made)
        return true;
    mount_once("wptmp", TMP, DE_MOUNT_WRITABLE);
    bool ok = de_dir_create("wptmp:/path10k/assets/scenes") == DE_OK;
    static const char manifest[] =
        "# DENG-271 test world: 3 rows of 40 cells along x (test_world_partition.c).\n"
        "[project]\nasset_root = \"assets\"\ninclude = [\"assets/worlds/path/cells.toml\"]\n"
        "[assets]\n\"levels/path\" = { type = \"scene\", source = \"scenes/path.scene.toml\" }\n"
        "[worlds.path]\nscenes = [\"levels/path\"]\nload_radius = 384.0\n"
        "unload_radius = 512.0\nmax_loads_per_frame = 2\n";
    ok = ok && de_file_write_all_atomic("wptmp:/path10k/assets.toml", manifest,
                                        sizeof manifest - 1) == DE_OK;
    /* Two entities per cell. */
    size_t cap = 256u * PATH_CELLS_X * PATH_ROWS * 2u, len = 0;
    char *scene = (char *)malloc(cap);
    ok = ok && scene != NULL;
    u32 id = 1;
    for (i32 cx = 0; ok && cx < PATH_CELLS_X; ++cx)
        for (i32 cy = -1; cy <= 1; ++cy)
            for (i32 k = 0; k < 2; ++k) {
                int n = snprintf(scene + len, cap - len,
                                 "[entity.0x%016x.DeTransform]\nposition = [%d.5, %d.25, 0.0]\n"
                                 "rotation = [0.0, 0.0, 0.0, 1.0]\nscale = [1.0, 1.0, 1.0]\n",
                                 id++, cx * 256 + 40 + k * 100, cy * 256 + 60 + k * 50);
                ok = n > 0 && (size_t)n < cap - len;
                len += ok ? (size_t)n : 0;
            }
    ok = ok && de_file_write_all_atomic("wptmp:/path10k/assets/scenes/path.scene.toml", scene,
                                        len) == DE_OK;
    free(scene);
    if (ok) {
        const char *argv[] = {TMP "/path10k/assets.toml", "--world", "path"};
        de_cook_io io = {path_cook_sink, NULL};
        ok = de_cook_world_main(3, argv, &io) == DE_COOK_EXIT_OK;
    }
    made = ok;
    return ok;
#else
    return false;
#endif
}

TEST_F(streaming, world_partition_stream_path, wp_fx) {
#if !DE_HAS_COOKER || !defined(DE_STREAMING_HAS_WORLD_COOK)
    SKIP(NO_COOKER_MSG);
#endif
    if (!path_world_make())
        SKIP("cannot write the 10 km test world in the build tree");
    mount_once("wpath", TMP "/path10k", DE_MOUNT_READ_ONLY);
    wp_open(f, "wpath:/assets.toml", "path", 0);
    de_world_partition_stats st = de_world_partition_get_stats(f->p);
    ASSERT_EQ(st.cell_count, (u32)(PATH_CELLS_X * PATH_ROWS));
    const u32 cap = 2; /* max_loads_per_frame of the world */

    /* Memory: a first flight sizes the asset system's tables (they grow and are kept), so the
     * baseline is taken after it, back out of range with every cell unloaded. The measured
     * flight below must return to the same bytes with nothing resident. */
    for (f64 x = 0.0; x <= 10000.0; x += 100.0) {
        set_camera(f, x, 128.0);
        settle(f, 0.016f);
    }
    set_camera(f, -5000.0, 128.0);
    st = settle(f, 0.016f);
    EXPECT_EQ(st.cells_loaded, 0u);
    asset_pump(f);
    de_asset_flush_unused(f->s);
    asset_pump(f);
    EXPECT_EQ(de_asset_get_stats(f->s).assets_resident, 0u);
    de_tracking_stats base_asset = de_tracking_allocator_stats(&f->t_asset);
    u64 base_loads = de_world_partition_get_stats(f->p).total_loads;
    f->ev_count = 0;

    i32 prev[PATH_CELLS_X][PATH_ROWS];
    memset(prev, 0, sizeof prev);
    u32 max_started = 0, max_resident = 0, order_violations = 0, far_unloads = 0;
    /* Fly x = 0 .. 10000 m along y = 128, 25 m per frame (400 frames, 90 km/h at 60 fps). */
    for (u32 fi = 0; fi <= 400; ++fi) {
        f64 cx = (f64)fi * 25.0, cy = 128.0;
        set_camera(f, cx, cy);
        if (fi % 4 == 0)
            de_sleep_ns(500000ull); /* let some loads land between frames */
        st = frame(f, 0.016f);
        if (st.loads_started > max_started)
            max_started = st.loads_started;
        if (st.cells_loaded + st.cells_loading > max_resident)
            max_resident = st.cells_loaded + st.cells_loading;
        /* Cells that started loading this frame, and unloaded candidates still waiting. */
        f64 newest_far = -1.0, waiting_near = 1e300;
        for (i32 x = 0; x < PATH_CELLS_X; ++x)
            for (i32 r = 0; r < PATH_ROWS; ++r) {
                de_world_cell_state s = de_world_partition_cell_state(f->p, x, r - 1);
                f64 d2 = de__cell_dist2(x, r - 1, cx, cy);
                if (s == DE_WORLD_CELL_LOADING && prev[x][r] == DE_WORLD_CELL_UNLOADED &&
                    d2 > newest_far)
                    newest_far = d2;
                if (s == DE_WORLD_CELL_UNLOADED && d2 <= 384.0 * 384.0 && d2 < waiting_near)
                    waiting_near = d2;
                if (s == DE_WORLD_CELL_UNLOADED && prev[x][r] != DE_WORLD_CELL_UNLOADED &&
                    d2 <= 512.0 * 512.0)
                    ++far_unloads; /* unloaded while inside unload_radius */
                prev[x][r] = (i32)s;
            }
        /* Distance order: no cell left waiting is nearer than one started this frame. */
        if (newest_far >= 0.0 && waiting_near < newest_far)
            ++order_violations;
    }
    EXPECT_LE(max_started, cap);
    EXPECT_GT(max_started, 0u);
    EXPECT_EQ(order_violations, 0u);
    EXPECT_EQ(far_unloads, 0u);
    /* Within 512 m of a point on y = 128 there are at most 5 columns x 3 rows. */
    EXPECT_LE(max_resident, 15u);
    st = settle(f, 0.016f);
    EXPECT_GT(st.total_loads - base_loads, (u64)(PATH_CELLS_X * PATH_ROWS) / 2u);
    EXPECT_GT(st.total_unloads, 0u);
    /* Every load_started reported is a load or a pending load: none lost. */
    EXPECT_EQ(st.cells_loading, 0u);

    /* Fly far away: everything unloads and the asset memory returns to the baseline. */
    set_camera(f, -5000.0, 128.0);
    st = settle(f, 0.016f);
    EXPECT_EQ(st.cells_loaded, 0u);
    EXPECT_EQ(st.total_loads, st.total_unloads);
    asset_pump(f);
    de_asset_flush_unused(f->s);
    asset_pump(f);
    EXPECT_EQ(de_asset_get_stats(f->s).assets_resident, 0u);
    /* Every block allocated during the flight is freed. The bytes may differ by a table's
     * capacity, which grows to the high-water mark of overlapping loads (timing dependent). */
    de_tracking_stats end_asset = de_tracking_allocator_stats(&f->t_asset);
    EXPECT_EQ(end_asset.blocks_live, base_asset.blocks_live);
    EXPECT_TRUE(end_asset.bytes_live <= base_asset.bytes_live + 4096u);
    if (end_asset.blocks_live != base_asset.blocks_live ||
        end_asset.bytes_live > base_asset.bytes_live + 4096u)
        printf("  asset bytes live %llu (baseline %llu), blocks %llu (baseline %llu)\n",
               (unsigned long long)end_asset.bytes_live,
               (unsigned long long)base_asset.bytes_live,
               (unsigned long long)end_asset.blocks_live,
               (unsigned long long)base_asset.blocks_live);
    /* The loaded/unloaded events pair up. */
    u32 loads = 0, unloads = 0;
    for (u32 i = 0; i < f->ev_count; ++i)
        f->ev[i].loaded ? ++loads : ++unloads;
    EXPECT_EQ(loads, unloads);
}

/* ---- test_world_partition_terrain (DENG-857) ------------------------------------------------- */

/* A 128 m terrain at (200, 100, 5): its x = 0 tiles (200..264 m) overlap cells (0, 0) and (1, 0),
 * the others (264..328 m) only (1, 0), which holds no entity. One more entity in cell (7, 0). */
#define TERRA_ID 0x100u

static bool terra_world_make(void) {
#if defined(DE_STREAMING_HAS_WORLD_COOK)
    static bool made;
    if (made)
        return true;
    mount_once("wptmp", TMP, DE_MOUNT_WRITABLE);
    bool ok = de_dir_create("wptmp:/terra/assets/scenes") == DE_OK;
    static const char manifest[] =
        "# DENG-857 test world: a terrain across a cell boundary (test_world_partition.c).\n"
        "[project]\nasset_root = \"assets\"\ninclude = [\"assets/worlds/terra/cells.toml\"]\n"
        "[assets]\n\"levels/terra\" = { type = \"scene\", source = \"scenes/terra.scene.toml\" }\n"
        "[worlds.terra]\nscenes = [\"levels/terra\"]\nload_radius = 100.0\n"
        "unload_radius = 150.0\nmax_loads_per_frame = 2\n[worlds.terra.proxy]\nenabled = false\n";
    ok = ok && de_file_write_all_atomic("wptmp:/terra/assets.toml", manifest,
                                        sizeof manifest - 1) == DE_OK;
    size_t cap = 64u * 1024u, len = 0;
    char *scene = (char *)malloc(cap);
    ok = ok && scene != NULL;
    int n = ok ? snprintf(scene, cap,
                          "[entity.0x%016x.DeTransform]\nposition = [200.0, 100.0, 5.0]\n"
                          "[entity.0x%016x.DeTerrain]\nsize = 128.0\nheight = 10.0\nheights = [",
                          TERRA_ID, TERRA_ID)
               : 0;
    len = n > 0 ? (size_t)n : 0;
    for (u32 i = 0; ok && i < 33u * 33u; ++i) {
        n = snprintf(scene + len, cap - len, "%s%u", i ? ", " : "", (unsigned)(i * 60u));
        ok = n > 0 && (size_t)n < cap - len;
        len += ok ? (size_t)n : 0;
    }
    n = ok ? snprintf(scene + len, cap - len, "]\nholes = [1") : 0;
    ok = ok && n > 0;
    len += ok ? (size_t)n : 0;
    for (u32 i = 1; ok && i < 33u * 33u; ++i) {
        n = snprintf(scene + len, cap - len, ", 0");
        ok = n > 0 && (size_t)n < cap - len;
        len += ok ? (size_t)n : 0;
    }
    n = ok ? snprintf(scene + len, cap - len,
                      "]\n[entity.0x0000000000000101.DeTransform]\n"
                      "position = [2000.5, 100.25, 0.0]\n")
           : 0;
    ok = ok && n > 0 && (size_t)n < cap - len;
    len += ok ? (size_t)n : 0;
    ok = ok && de_file_write_all_atomic("wptmp:/terra/assets/scenes/terra.scene.toml", scene,
                                        len) == DE_OK;
    free(scene);
    if (ok) {
        const char *argv[] = {TMP "/terra/assets.toml", "--world", "terra"};
        de_cook_io io = {path_cook_sink, NULL};
        ok = de_cook_world_main(3, argv, &io) == DE_COOK_EXIT_OK;
    }
    made = ok;
    return ok;
#else
    return false;
#endif
}

static u32 terra_refs(wp_fx *f, u32 tx, u32 ty) {
    u32 refs = UINT32_MAX;
    EXPECT_TRUE(de_world_partition_terrain_tile(f->p, TERRA_ID, tx, ty, &refs, NULL));
    return refs;
}

static de_asset_id terra_tile(u32 tx, u32 ty) {
    char name[128];
    (void)de__ttile_name(name, sizeof name, "terra", TERRA_ID, tx, ty);
    return de_asset_id_from(name);
}

TEST_F(streaming, world_partition_terrain, wp_fx) {
#if !DE_HAS_COOKER || !defined(DE_STREAMING_HAS_WORLD_COOK)
    SKIP(NO_COOKER_MSG);
#endif
    if (!terra_world_make())
        SKIP("cannot write the terrain test world in the build tree");

    /* AC1: cells.toml lists each cell's tiles; the terrain-only cell (1, 0) is a cell. */
    void *data = NULL;
    u64 size = 0;
    ASSERT_EQ(de_file_read_all("wptmp:/terra/assets/worlds/terra/cells.toml", de_heap_allocator(),
                               &data, &size),
              DE_OK);
    const char *toml = (const char *)data;
#define TT(x, y) "\"world/terra/terrain_0000000000000100_" #x "_" #y "\""
    EXPECT_TRUE(strstr(toml, "[bundles.\"world/terra/cell_0_0\"]\nassets = "
                             "[\"world/terra/cell_0_0\"]\ndepends = [" TT(0, 0) ", " TT(0, 1)
                             ", " TT(0, 2) "]\n") != NULL);
    EXPECT_TRUE(strstr(toml, "[bundles.\"world/terra/cell_1_0\"]\nassets = "
                             "[\"world/terra/cell_1_0\"]\ndepends = [" TT(0, 0) ", " TT(1, 0)
                             ", " TT(2, 0) ", " TT(0, 1) ", " TT(1, 1) ", " TT(2, 1) ", " TT(0, 2)
                             ", " TT(1, 2) ", " TT(2, 2) "]\n") != NULL);
    EXPECT_TRUE(strstr(toml, "[bundles.\"world/terra/cell_7_0\"]\nassets = "
                             "[\"world/terra/cell_7_0\"]\n\n") != NULL);
    EXPECT_TRUE(strstr(toml, TT(2, 2) " = { type = \"blob\", source = "
                                      "\"worlds/terra/terrain_0000000000000100_2_2.tile\" }") !=
                NULL);
#undef TT
    DE_FREE(de_heap_allocator(), data, size + 1);

    mount_once("wterra", TMP "/terra", DE_MOUNT_READ_ONLY);
    wp_open(f, "wterra:/assets.toml", "terra", 0);
    de_world_partition_stats st = de_world_partition_get_stats(f->p);
    ASSERT_EQ(st.cell_count, 3u);
    EXPECT_EQ(st.terrain_tiles, 9u);
    EXPECT_FALSE(de_world_partition_terrain_tile(f->p, TERRA_ID, 3, 0, NULL, NULL));
    EXPECT_FALSE(de_world_partition_terrain_tile(f->p, 0x101u, 0, 0, NULL, NULL));

    /* Memory: a warm-up flight sizes the asset system's tables; the baseline comes after it. */
    for (f64 x = -400.0; x <= 2400.0; x += 50.0) {
        set_camera(f, x, 128.0);
        settle(f, 0.016f);
    }
    set_camera(f, -1000.0, 128.0);
    st = settle(f, 0.016f);
    EXPECT_EQ(st.cells_loaded, 0u);
    EXPECT_EQ(st.terrain_tiles_resident, 0u);
    asset_pump(f);
    de_asset_flush_unused(f->s);
    asset_pump(f);
    EXPECT_EQ(de_asset_get_stats(f->s).assets_resident, 0u);
    de_tracking_stats base_asset = de_tracking_allocator_stats(&f->t_asset);

    /* AC2: cell (0, 0) alone holds the x = 0 tiles. */
    set_camera(f, 64.0, 128.0);
    st = settle(f, 0.016f);
    EXPECT_EQ(de_world_partition_cell_state(f->p, 0, 0), DE_WORLD_CELL_LOADED);
    EXPECT_EQ(de_world_partition_cell_state(f->p, 1, 0), DE_WORLD_CELL_UNLOADED);
    EXPECT_EQ(st.terrain_tiles_resident, 3u);
    for (u32 ty = 0; ty < 3; ++ty) {
        EXPECT_EQ(terra_refs(f, 0, ty), 1u);
        EXPECT_EQ(terra_refs(f, 1, ty), 0u);
        EXPECT_EQ(de_asset_status_of(f->s, terra_tile(0, ty)), DE_LOAD_READY);
    }
    /* The tile blob as a renderer reads it. */
    u64 bytes = 0;
    const void *blob = de_blob_data(f->s, de_asset_blob(f->s, terra_tile(0, 0)), &bytes);
    de__ttile_view v;
    ASSERT_TRUE(de__ttile_parse(blob, bytes, &v));
    EXPECT_EQ(v.terrain, (u64)TERRA_ID);
    EXPECT_EQ(v.tx, 0u);
    EXPECT_EQ(v.ty, 0u);
    EXPECT_EQ(v.w, 17u);
    EXPECT_EQ(v.h, 17u);
    EXPECT_FLOAT_EQ(v.origin[0], 200.0, 0.0);
    EXPECT_FLOAT_EQ(v.origin[1], 100.0, 0.0);
    EXPECT_FLOAT_EQ(v.origin[2], 5.0, 0.0);
    EXPECT_FLOAT_EQ(v.spacing, 4.0, 0.0);
    EXPECT_FLOAT_EQ(v.height, 10.0, 0.0);
    EXPECT_EQ(de__ttile_height(&v, 2u * 17u + 1u), (u16)((2u * 33u + 1u) * 60u));
    EXPECT_EQ(v.holes[0], 1u);
    EXPECT_EQ(v.holes[1], 0u);
    EXPECT_EQ(v.splat[0], 255u);

    /* Both cells: the shared tiles load once and count two cells. */
    set_camera(f, 300.0, 128.0);
    st = settle(f, 0.016f);
    EXPECT_EQ(st.cells_loaded, 2u);
    EXPECT_EQ(st.terrain_tiles_resident, 9u);
    EXPECT_EQ(terra_refs(f, 0, 1), 2u);
    EXPECT_EQ(terra_refs(f, 2, 2), 1u);
    blob = de_blob_data(f->s, de_asset_blob(f->s, terra_tile(2, 2)), &bytes);
    ASSERT_TRUE(de__ttile_parse(blob, bytes, &v));
    EXPECT_EQ(v.x0, 32u);
    EXPECT_EQ(v.w, 1u);

    /* Cell (0, 0) leaves the unload radius: (1, 0) still holds every tile. */
    set_camera(f, 450.0, 128.0);
    st = settle(f, 0.016f);
    EXPECT_EQ(de_world_partition_cell_state(f->p, 0, 0), DE_WORLD_CELL_UNLOADED);
    EXPECT_EQ(de_world_partition_cell_state(f->p, 1, 0), DE_WORLD_CELL_LOADED);
    EXPECT_EQ(st.terrain_tiles_resident, 9u);
    EXPECT_EQ(terra_refs(f, 0, 1), 1u);
    asset_pump(f);
    de_asset_flush_unused(f->s);
    EXPECT_EQ(de_asset_status_of(f->s, terra_tile(0, 1)), DE_LOAD_READY);

    /* Hysteresis: 120 m from (0, 0) is inside unload_radius but outside load_radius. */
    set_camera(f, 376.0, 128.0);
    st = settle(f, 0.016f);
    EXPECT_EQ(de_world_partition_cell_state(f->p, 0, 0), DE_WORLD_CELL_UNLOADED);

    /* Away: the last cell releases the tiles, and memory is back at the baseline. */
    set_camera(f, -1000.0, 128.0);
    st = settle(f, 0.016f);
    EXPECT_EQ(st.cells_loaded, 0u);
    EXPECT_EQ(st.terrain_tiles_resident, 0u);
    for (u32 ty = 0; ty < 3; ++ty)
        for (u32 tx = 0; tx < 3; ++tx)
            EXPECT_EQ(terra_refs(f, tx, ty), 0u);
    asset_pump(f);
    de_asset_flush_unused(f->s);
    asset_pump(f);
    EXPECT_EQ(de_asset_get_stats(f->s).assets_resident, 0u);
    EXPECT_NE(de_asset_status_of(f->s, terra_tile(0, 0)), DE_LOAD_READY);
    de_tracking_stats end_asset = de_tracking_allocator_stats(&f->t_asset);
    EXPECT_EQ(end_asset.blocks_live, base_asset.blocks_live);
    EXPECT_TRUE(end_asset.bytes_live <= base_asset.bytes_live + 4096u);
}

/* ---- no per-frame allocation ----------------------------------------------------------------- */

TEST_F(streaming, world_partition_no_alloc, wp_fx) {
#if !DE_HAS_COOKER
    SKIP(NO_COOKER_MSG);
#endif
    small_open(f, 0);
    set_camera(f, 128.0, 128.0);
    settle(f, 0.016f);
    for (u32 i = 0; i < 30; ++i) /* finish the proxy fades */
        frame(f, 0.016f);
    /* Steady frames, then frames with the camera moving inside one cell (no load or unload). */
    de_tracking_stats a0 = de_tracking_allocator_stats(&f->t_asset);
    de_tracking_stats w0 = de_tracking_allocator_stats(&f->t_world);
    de_tracking_stats p0 = de_tracking_allocator_stats(&f->t_part);
    for (u32 i = 0; i < 200; ++i) {
        set_camera(f, 100.0 + (f64)(i % 50), 128.0);
        EXPECT_EQ(de_world_partition_update(f->p, 0.016f), DE_OK);
    }
    de_tracking_stats a1 = de_tracking_allocator_stats(&f->t_asset);
    de_tracking_stats w1 = de_tracking_allocator_stats(&f->t_world);
    de_tracking_stats p1 = de_tracking_allocator_stats(&f->t_part);
    EXPECT_EQ(a1.alloc_count + a1.realloc_count, a0.alloc_count + a0.realloc_count);
    EXPECT_EQ(w1.alloc_count + w1.realloc_count, w0.alloc_count + w0.realloc_count);
    EXPECT_EQ(p1.alloc_count + p1.realloc_count, p0.alloc_count + p0.realloc_count);
}

/* ---- test_world_partition_editor (DENG-831) -------------------------------------------------- */

static de__wp_cell_info cell_at(wp_fx *f, i32 cx, i32 cy) {
    u32 i = 0;
    de__wp_cell_info c;
    memset(&c, 0, sizeof c);
    EXPECT_TRUE(de__wp_cell_index(f->p, cx, cy, &i));
    EXPECT_TRUE(de__wp_cell_info_at(f->p, i, &c));
    return c;
}

TEST_F(streaming, world_partition_editor, wp_fx) {
#if !DE_HAS_COOKER
    SKIP(NO_COOKER_MSG);
#endif
    small_open(f, 0);
    ASSERT_EQ(de__wp_cell_count(f->p), 4u);
    u32 i = 0;
    EXPECT_FALSE(de__wp_cell_index(f->p, 5, 5, &i));
    ASSERT_TRUE(de__wp_cell_index(f->p, 390, 390, &i));
    EXPECT_EQ(i, 3u); /* sorted by cx, then cy */

    /* Preview with one source in cell (0, 0): (0, 0) then (1, 0) (128 m), nothing else. */
    de__wp_preview_cell buf[12];
    de__wp_preview pv = {.loads = &buf[0], .keep = &buf[4], .unloads = &buf[8]};
    const de_streaming_source at00 = {128.0, 128.0, 0.0, 0.0f};
    de__wp_preview_run(f->p, &at00, 1, false, &pv);
    ASSERT_EQ(pv.load_count, 2u);
    EXPECT_EQ(pv.loads[0].cx, 0);
    EXPECT_EQ(pv.loads[0].cy, 0);
    EXPECT_FLOAT_EQ(pv.loads[0].distance, 0.0, 1e-9);
    EXPECT_EQ(pv.loads[1].cx, 1);
    EXPECT_FLOAT_EQ(pv.loads[1].distance, 128.0, 1e-9);
    EXPECT_EQ(pv.keep_count, 0u);
    EXPECT_EQ(pv.unload_count, 0u);
    EXPECT_FALSE(pv.capped);
    EXPECT_EQ(de_world_partition_get_stats(f->p).loads_started, 0u); /* a dry run loads nothing */

    /* An editor source streams like any other, and is reported as one. */
    de__wp_set_editor_sources(f->p, &at00, 1);
    de_world_partition_stats st = settle(f, 0.016f);
    EXPECT_EQ(st.cells_loaded, 2u);
    const de__wp_source *src = NULL;
    ASSERT_EQ(de__wp_sources(f->p, &src), 1u);
    EXPECT_EQ(src[0].kind, DE__WP_SOURCE_EDITOR);
    EXPECT_FLOAT_EQ(src[0].s.radius, 384.0f, 0.0f); /* radius 0: the world's load radius */
    de__wp_cell_info c = cell_at(f, 0, 0);
    EXPECT_EQ(c.state, DE_WORLD_CELL_LOADED);
    EXPECT_GT(c.bytes, 0u);
    EXPECT_GT(c.entities, 0u);
    EXPECT_TRUE(c.error == NULL);
    f64 d = -1.0;
    ASSERT_TRUE(de__wp_cell_distance(f->p, 2, &d)); /* (1, 0) */
    EXPECT_FLOAT_EQ(d, 128.0, 1e-9);
    u32 failed = 9;
    u64 bytes = 0;
    de__wp_extra_stats(f->p, &failed, &bytes);
    EXPECT_EQ(failed, 0u);
    EXPECT_EQ(bytes, c.bytes + cell_at(f, 1, 0).bytes);

    /* Preview from the current state: both cells are kept. */
    de__wp_preview_run(f->p, &at00, 1, false, &pv);
    EXPECT_EQ(pv.load_count, 0u);
    EXPECT_EQ(pv.keep_count, 2u);

    /* Force (0, 0) out and (390, 390) in. */
    de__wp_set_force(f->p, 1, DE__WP_FORCE_UNLOAD);
    de__wp_set_force(f->p, 3, DE__WP_FORCE_LOAD);
    EXPECT_EQ(de__wp_forced_loads(f->p), 1u);
    de__wp_preview_run(f->p, &at00, 1, false, &pv);
    EXPECT_EQ(pv.unload_count, 1u);
    EXPECT_EQ(pv.load_count, 1u);
    EXPECT_EQ(pv.loads[0].cx, 390);
    de__wp_preview_run(f->p, &at00, 1, true, &pv); /* ignoring forces: as before */
    EXPECT_EQ(pv.load_count, 0u);
    EXPECT_EQ(pv.unload_count, 0u);
    st = settle(f, 0.016f);
    EXPECT_EQ(de_world_partition_cell_state(f->p, 0, 0), DE_WORLD_CELL_UNLOADED);
    EXPECT_EQ(de_world_partition_cell_state(f->p, 390, 390), DE_WORLD_CELL_LOADED);
    EXPECT_EQ(cell_at(f, 0, 0).bytes, 0u);
    EXPECT_EQ(cell_at(f, 0, 0).force, DE__WP_FORCE_UNLOAD);
    for (u32 k = 0; k < 10; ++k) /* forces hold */
        st = frame(f, 0.016f);
    EXPECT_EQ(de_world_partition_cell_state(f->p, 0, 0), DE_WORLD_CELL_UNLOADED);

    /* Clear the forces and the sources: the far cell goes, (0, 0) comes back, then all go. */
    de__wp_clear_forces(f->p);
    EXPECT_EQ(de__wp_forced_loads(f->p), 0u);
    st = settle(f, 0.016f);
    EXPECT_EQ(de_world_partition_cell_state(f->p, 390, 390), DE_WORLD_CELL_UNLOADED);
    EXPECT_EQ(de_world_partition_cell_state(f->p, 0, 0), DE_WORLD_CELL_LOADED);
    de__wp_set_editor_sources(f->p, NULL, 0);
    st = settle(f, 0.016f);
    EXPECT_EQ(st.cells_loaded, 0u);
    de__wp_extra_stats(f->p, &failed, &bytes);
    EXPECT_EQ(bytes, 0u);
}

/* ---- test_world_partition_packed (DENG-856) -------------------------------------------------- */

/* world_small copied under <TMP>/packw, plus proxies.toml: a far proxy for cell (0, 0) and one for
 * (7, 7), which has no cell. `de-cook cook` packs it into <TMP>/packw/out/small.dpak. */
#define PACKW TMP "/packw"

#if defined(DE_STREAMING_HAS_WORLD_COOK)
static bool packw_copy(const char *rel) {
    char src[256], dst[256];
    snprintf(src, sizeof src, "wsmall:/%s", rel);
    snprintf(dst, sizeof dst, "wpkw:/packw/%s", rel);
    const de_allocator *a = de_heap_allocator();
    void *data = NULL;
    u64 size = 0;
    if (de_file_read_all(src, a, &data, &size) != DE_OK)
        return false;
    bool ok = de_file_write_all_atomic(dst, data, size) == DE_OK;
    DE_FREE(a, data, size + 1);
    return ok;
}
#endif

/* Writes and packs the project once. False when it cannot. */
static bool packw_make(void) {
#if defined(DE_STREAMING_HAS_WORLD_COOK)
    static int made; /* 0 not tried, 1 made, -1 failed */
    if (made != 0)
        return made > 0;
    made = -1;
    if (!g_small_mounted) {
        if (de_fs_mount("wsmall", FIXTURE, DE_MOUNT_READ_ONLY, 0) != DE_OK)
            return false;
        g_small_mounted = true;
    }
    if (de_fs_mount("wpkw", TMP, DE_MOUNT_WRITABLE, 0) != DE_OK)
        return false;
    static const char *const files[] = {
        "assets/scenes/props.scene.toml",           "assets/scenes/terrain.scene.toml",
        "assets/worlds/small/cells.toml",           "assets/worlds/small/cell_0_0.scene.toml",
        "assets/worlds/small/cell_-1_-3.scene.toml", "assets/worlds/small/cell_1_0.scene.toml",
        "assets/worlds/small/cell_390_390.scene.toml"};
    bool ok = de_dir_create("wpkw:/packw/assets/scenes") == DE_OK &&
              de_dir_create("wpkw:/packw/assets/worlds/small") == DE_OK;
    for (u32 i = 0; ok && i < (u32)(sizeof files / sizeof files[0]); ++i)
        ok = packw_copy(files[i]);
    static const char manifest[] =
        "# DENG-856 test project: world_small plus far proxies (test_world_partition.c).\n"
        "[project]\nasset_root = \"assets\"\n"
        "include = [\"assets/worlds/small/cells.toml\", \"assets/worlds/small/proxies.toml\"]\n"
        "[assets]\n"
        "\"levels/small_terrain\" = { type = \"scene\", source = \"scenes/terrain.scene.toml\" }\n"
        "\"levels/small_props\" = { type = \"scene\", source = \"scenes/props.scene.toml\" }\n"
        "[worlds.small]\nscenes = [\"levels/small_terrain\", \"levels/small_props\"]\n"
        "load_radius = 384.0\nunload_radius = 512.0\nmax_loads_per_frame = 2\n";
    /* Scene assets stand in for the proxy models: the partition only requests them here. */
    static const char proxies[] =
        "[assets]\n"
        "\"world/small/proxy_0_0\" = { type = \"scene\", source = "
        "\"worlds/small/cell_0_0.scene.toml\" }\n"
        "\"world/small/proxy_7_7\" = { type = \"scene\", source = "
        "\"worlds/small/cell_1_0.scene.toml\" }\n"
        "[bundles.\"world/small/proxy_0_0\"]\nassets = [\"world/small/proxy_0_0\"]\n"
        "[bundles.\"world/small/proxy_7_7\"]\nassets = [\"world/small/proxy_7_7\"]\n";
    ok = ok &&
         de_file_write_all_atomic("wpkw:/packw/assets.toml", manifest, sizeof manifest - 1) ==
             DE_OK &&
         de_file_write_all_atomic("wpkw:/packw/assets/worlds/small/proxies.toml", proxies,
                                  sizeof proxies - 1) == DE_OK;
    if (ok) {
#if defined(_WIN32)
        const char *plat = "windows";
#elif defined(__APPLE__)
        const char *plat = "macos";
#else
        const char *plat = "linux";
#endif
        const char *argv[] = {PACKW "/assets.toml", "--platform", plat, "--out", PACKW "/out",
                              "--pack-name", "small", "--quiet", "--no-cache", "--jobs", "1"};
        de_cook_io io = {path_cook_sink, NULL};
        ok = de_cook_cook_run((int)(sizeof argv / sizeof argv[0]), argv, &io, NULL) == DE_COOK_EXIT_OK;
    }
    if (ok)
        made = 1;
    return ok;
#else
    return false;
#endif
}

#define PACKW_MAX 8
typedef struct packw_cells {
    u32 count;
    i32 cx[PACKW_MAX], cy[PACKW_MAX];
    bool proxy[PACKW_MAX];
    f32 load_radius; /* de_asset_world_info_get */
} packw_cells;

/* Opens world "small" in `mode`, on the manifest (NULL: none) and the pack (NULL: none), and
 * returns the cells the partition found. Closes everything again. */
static packw_cells packw_discover(wp_fx *f, de_asset_mode mode, const char *manifest,
                                  const char *pack) {
    packw_cells out;
    memset(&out, 0, sizeof out);
    ASSERT_EQ(de_world_create(&(de_world_desc){.allocator = &f->a_world, .types = f->reg},
                                  &f->w),
              DE_OK);
    de_asset_system_desc d = {.allocator = &f->a_asset,
                              .tasks = f->tasks,
                              .io_threads = 1,
                              .cache_dir = "nocache:/decache",
                              .types = f->reg,
                              .mode = mode};
    ASSERT_EQ(de_asset_system_create(&d, &f->s), DE_OK);
    if (manifest != NULL)
        ASSERT_EQ(de_asset_manifest_load(f->s, manifest), DE_OK);
    if (pack != NULL)
        ASSERT_EQ(de_asset_pack_mount(f->s, pack, 0), DE_OK);
    de_asset_world_info wi;
    ASSERT_EQ(de_asset_world_info_get(f->s, "small", &wi), DE_OK);
    out.load_radius = wi.load_radius;
    de_world_partition_desc pd = {.allocator = &f->a_part, .on_cell = on_cell, .user = f};
    ASSERT_EQ(de_world_partition_open(f->s, f->w, "small", &pd, &f->p), DE_OK);
    out.count = de__wp_cell_count(f->p);
    for (u32 i = 0; i < out.count && i < PACKW_MAX; ++i) {
        de__wp_cell_info c;
        EXPECT_TRUE(de__wp_cell_info_at(f->p, i, &c));
        out.cx[i] = c.cx;
        out.cy[i] = c.cy;
        out.proxy[i] = c.has_proxy;
    }
    de_world_partition_close(f->p);
    f->p = NULL;
    de_asset_system_destroy(f->s);
    f->s = NULL;
    de_world_destroy(f->w);
    f->w = NULL;
    return out;
}

static void packw_expect_same(const packw_cells *want, const packw_cells *got) {
    ASSERT_EQ(got->count, want->count);
    for (u32 i = 0; i < want->count && i < PACKW_MAX; ++i) {
        EXPECT_EQ(got->cx[i], want->cx[i]);
        EXPECT_EQ(got->cy[i], want->cy[i]);
        EXPECT_EQ(got->proxy[i], want->proxy[i]);
    }
}

TEST_F(streaming, world_partition_packed, wp_fx) {
#if !DE_HAS_COOKER || !defined(DE_STREAMING_HAS_WORLD_COOK)
    SKIP(NO_COOKER_MSG);
#endif
    if (!packw_make())
        SKIP("cannot write or pack the test project in the build tree");
    mount_once("wpk", PACKW, DE_MOUNT_READ_ONLY);

    /* SOURCE mode on the manifest: the reference. */
    packw_cells src = packw_discover(f, DE_ASSET_MODE_SOURCE, "wpk:/assets.toml", NULL);
    ASSERT_EQ(src.count, 4u);
    EXPECT_FLOAT_EQ(src.load_radius, 384.0f, 0.0);
    static const i32 want_cx[] = {-1, 0, 1, 390}, want_cy[] = {-3, 0, 0, 390};
    for (u32 i = 0; i < 4; ++i) {
        EXPECT_EQ(src.cx[i], want_cx[i]);
        EXPECT_EQ(src.cy[i], want_cy[i]);
    }
#if DE_HAS_RENDER
    EXPECT_TRUE(src.proxy[1]); /* (0, 0); the (7, 7) proxy has no cell */
    EXPECT_FALSE(src.proxy[0] || src.proxy[2] || src.proxy[3]);
#endif

    /* PACKED mode on the pack alone: the same cells and proxies, the default world settings. */
    packw_cells pk = packw_discover(f, DE_ASSET_MODE_PACKED, NULL, "wpk:/out/small.dpak");
    packw_expect_same(&src, &pk);
    EXPECT_FLOAT_EQ(pk.load_radius, 512.0f, 0.0);

    /* PACKED mode on the pack and the manifest: the same cells, the manifest's settings. */
    pk = packw_discover(f, DE_ASSET_MODE_PACKED, "wpk:/assets.toml", "wpk:/out/small.dpak");
    packw_expect_same(&src, &pk);
    EXPECT_FLOAT_EQ(pk.load_radius, 384.0f, 0.0);

    /* SOURCE mode with the pack mounted too: each bundle name is listed once. */
    ASSERT_EQ(de_world_create(&(de_world_desc){.allocator = &f->a_world, .types = f->reg}, &f->w),
              DE_OK);
    de_asset_system_desc d = {.allocator = &f->a_asset,
                              .tasks = f->tasks,
                              .io_threads = 1,
                              .cache_dir = "nocache:/decache",
                              .types = f->reg};
    ASSERT_EQ(de_asset_system_create(&d, &f->s), DE_OK);
    ASSERT_EQ(de_asset_manifest_load(f->s, "wpk:/assets.toml"), DE_OK);
    u32 n_manifest = de_asset_bundle_count(f->s);
    ASSERT_EQ(de_asset_pack_mount(f->s, "wpk:/out/small.dpak", 0), DE_OK);
    u32 n = de_asset_bundle_count(f->s);
    EXPECT_EQ(n, n_manifest);
    for (u32 i = 0; i < n; ++i)
        for (u32 j = i + 1; j < n; ++j)
            EXPECT_NE(strcmp(de_asset_bundle_name(f->s, i), de_asset_bundle_name(f->s, j)), 0);
    EXPECT_TRUE(de_asset_bundle_name(f->s, n) == NULL);
    /* A world no pack or manifest has stays NOT_FOUND. */
    de_asset_world_info wi;
    EXPECT_EQ(de_asset_world_info_get(f->s, "nope", &wi), DE_ERR_NOT_FOUND);
    de_asset_pack_unmount(f->s, "wpk:/out/small.dpak");
    EXPECT_EQ(de_asset_bundle_count(f->s), n_manifest);
}
