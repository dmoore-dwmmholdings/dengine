/* DENG-831: world.streaming (DIDE protocol 1.26, DIDE-127, DIDE-R173) through the real handlers
 * and the generated protocol readers and writers:
 *   info_and_errors     stream.info defaults, the conformance error cases with no world open,
 *                       editor-source checks, stream.overlay round trip, a disconnect drops the
 *                       editor sources
 *   open_force_preview  (cooker builds) world_small: stream.open, stream.cells, stream.force
 *                       loads a far cell and the topic stream.cells reports it, stream.preview,
 *                       stream.assignments with and without DeWorldCellPin, regenerateProxy
 *                       answers unsupported
 */
#include "de_test.h"
#if !defined(DE_SHIPPING)
#include "host/editor/editor_internal.h"
#include "host/host_internal.h"
#include <dengine/de_asset.h>
#include <dengine/de_ecs.h>
#include <dengine/de_platform.h>
#include <dengine/de_task.h>
#include <math.h>
#include <stdlib.h>

#ifndef DE_HAS_COOKER
#define DE_HAS_COOKER 0
#endif

#define STORAGE  (4u << 20)
#define RESPONSE (1u << 20)
#define PARAMS   (64u << 10)

typedef struct st_fx {
    de_engine engine;
    de__editor editor;
    unsigned char *storage;
    char *response;
    char *params;
    char *out;
    mpack_tree_t tree;
    bool parsed;
    u32 notes; /* stream.cells notes seen */
    u32 note_loaded; /* cells with state "loaded" in the last note */
} st_fx;

static void st_fx_setup(st_fx *f) {
    memset(f, 0, sizeof *f);
    de_log_desc ld;
    memset(&ld, 0, sizeof ld);
    ld.no_console_sink = true;
    ASSERT_EQ(de_log_init(&ld), DE_OK);
    de_fp_env_init_thread();
    ASSERT_EQ(de_type_registry_create(NULL, &f->engine.types), DE_OK);
    ASSERT_EQ(de_world_create(&(de_world_desc){.types = f->engine.types}, &f->engine.world), DE_OK);
    f->editor.e = &f->engine;
    f->editor.mode = DE_EP_WORLD_MODE_EDIT;
    f->storage = (unsigned char *)malloc(STORAGE);
    f->response = (char *)malloc(RESPONSE);
    f->params = (char *)malloc(PARAMS);
    f->out = (char *)malloc(RESPONSE);
    ASSERT_TRUE(f->storage != NULL && f->response != NULL && f->params != NULL && f->out != NULL);
    f->editor.note = f->out;
    f->editor.note_cap = RESPONSE;
    f->editor.connected = true; /* no IPC: de__ed_send drops the frame, the buffer keeps it */
}

/* The asset system on world_small (after the world: it registers the types scenes use). */
static void st_fx_assets(st_fx *f) {
    static bool mounted;
    if (!mounted) {
        ASSERT_EQ(de_fs_mount("edwsmall", DE_EDSTREAM_FIXTURE_DIR, DE_MOUNT_READ_ONLY, 0), DE_OK);
        mounted = true;
    }
    ASSERT_EQ(de_task_system_create(&(de_task_system_desc){.worker_count = 2}, &f->engine.tasks),
              DE_OK);
    de_asset_system_desc d = {.tasks = f->engine.tasks,
                              .io_threads = 1,
                              .cache_dir = "nocache:/decache",
                              .types = f->engine.types};
    ASSERT_EQ(de_asset_system_create(&d, &f->engine.assets), DE_OK);
    ASSERT_EQ(de_asset_manifest_load(f->engine.assets, "edwsmall:/assets.toml"), DE_OK);
}

static void st_fx_teardown(st_fx *f) {
    if (f->parsed)
        mpack_tree_destroy(&f->tree);
    for (u32 i = 0; i < f->editor.reset_count; ++i)
        f->editor.resets[i](&f->editor);
    de__ed_stream_shutdown(&f->editor);
    de_world_destroy(f->engine.world);
    if (f->engine.assets != NULL)
        de_asset_system_destroy(f->engine.assets);
    if (f->engine.tasks != NULL)
        de_task_system_destroy(f->engine.tasks);
    de_type_registry_destroy(f->engine.types);
    free(f->storage);
    free(f->response);
    free(f->params);
    free(f->out);
    de_log_shutdown();
}

static mpack_node_t call(st_fx *f, de_ep_method m, const void *params) {
    mpack_writer_t w;
    mpack_writer_init(&w, f->params, PARAMS);
    switch (m) {
    case DE_EP_METHOD_STREAM_OPEN:
        de_ep_write_stream_open_params(&w, (const de_ep_stream_open_params *)params);
        break;
    case DE_EP_METHOD_STREAM_CELLS:
        de_ep_write_stream_cells_params(&w, (const de_ep_stream_cells_params *)params);
        break;
    case DE_EP_METHOD_STREAM_FORCE:
        de_ep_write_stream_force_params(&w, (const de_ep_stream_force_params *)params);
        break;
    case DE_EP_METHOD_STREAM_SET_EDITOR_SOURCES:
        de_ep_write_stream_editor_sources_params(
            &w, (const de_ep_stream_editor_sources_params *)params);
        break;
    case DE_EP_METHOD_STREAM_PREVIEW:
        de_ep_write_stream_preview_params(&w, (const de_ep_stream_preview_params *)params);
        break;
    case DE_EP_METHOD_STREAM_ASSIGNMENTS:
        de_ep_write_stream_assignments_params(&w,
                                              (const de_ep_stream_assignments_params *)params);
        break;
    case DE_EP_METHOD_STREAM_REGENERATE_PROXY:
        de_ep_write_stream_regenerate_proxy_params(
            &w, (const de_ep_stream_regenerate_proxy_params *)params);
        break;
    case DE_EP_METHOD_STREAM_CANCEL_PROXY:
        de_ep_write_stream_cancel_proxy_params(&w,
                                               (const de_ep_stream_cancel_proxy_params *)params);
        break;
    case DE_EP_METHOD_STREAM_OVERLAY:
        de_ep_write_stream_overlay_params(&w, (const de_ep_stream_overlay_params *)params);
        break;
    default: /* stream.info */
        mpack_start_map(&w, 0);
        mpack_finish_map(&w);
        break;
    }
    const size_t psize = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    const de_ep_request req = {.id = 7, .method = de_ep_cstr("stream"),
                               .params = {f->params, psize}};
    ASSERT_TRUE(f->editor.handlers[m] != NULL);
    de_ep_arena a = {f->storage, STORAGE, 0};
    mpack_writer_init(&w, f->response, RESPONSE);
    f->editor.handlers[m](&f->editor, &req, &a, &w);
    const size_t size = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    if (f->parsed)
        mpack_tree_destroy(&f->tree);
    mpack_tree_init_data(&f->tree, f->response, size);
    mpack_tree_parse(&f->tree);
    f->parsed = true;
    ASSERT_EQ(mpack_tree_error(&f->tree), mpack_ok);
    mpack_node_t root = mpack_tree_root(&f->tree);
    ASSERT_EQ(mpack_node_u64(mpack_node_map_cstr(root, "id")), 7u);
    return root;
}
static mpack_node_t result_of(mpack_node_t root) {
    if (mpack_node_map_contains_cstr(root, "error")) {
        mpack_node_t m = mpack_node_map_cstr(mpack_node_map_cstr(root, "error"), "message");
        fprintf(stderr, "error: %.*s\n", (int)mpack_node_strlen(m), mpack_node_str(m));
    }
    ASSERT_FALSE(mpack_node_map_contains_cstr(root, "error"));
    return mpack_node_map_cstr(root, "result");
}
static void expect_error(mpack_node_t root, de_ep_error code) {
    EXPECT_FALSE(mpack_node_map_contains_cstr(root, "result"));
    mpack_node_t error = mpack_node_map_cstr(root, "error");
    EXPECT_EQ(mpack_node_i32(mpack_node_map_cstr(error, "code")), (i32)code);
}
static bool is_str(mpack_node_t n, const char *s) {
    return mpack_node_type(n) == mpack_type_str && mpack_node_strlen(n) == strlen(s) &&
           memcmp(mpack_node_str(n), s, strlen(s)) == 0;
}
static mpack_node_t info(st_fx *f) {
    return result_of(call(f, DE_EP_METHOD_STREAM_INFO, NULL));
}
static u64 revision(st_fx *f) {
    return mpack_node_u64(mpack_node_map_cstr(info(f), "revision"));
}

/* One editor frame (the asset update first, as the host runs it); records a stream.cells note. */
static void frame(st_fx *f) {
    if (f->engine.assets != NULL) {
        de_asset_system_update(f->engine.assets, f->engine.world);
        (void)de_world_run_schedule(f->engine.world, DE_SCHEDULE_FIRST, 0.0f, 0.0f);
    }
    const u64 seq = f->editor.seq;
    for (u32 i = 0; i < f->editor.tick_count; ++i)
        f->editor.ticks[i](&f->editor);
    if (f->editor.seq == seq)
        return;
    mpack_tree_t t;
    mpack_tree_init_data(&t, f->out, RESPONSE);
    mpack_tree_parse(&t);
    ASSERT_EQ(mpack_tree_error(&t), mpack_ok);
    mpack_node_t root = mpack_tree_root(&t);
    EXPECT_TRUE(is_str(mpack_node_map_cstr(root, "topic"), "stream.cells"));
    mpack_node_t cells = mpack_node_map_cstr(mpack_node_map_cstr(root, "data"), "cells");
    f->notes++;
    f->note_loaded = 0;
    for (size_t i = 0; i < mpack_node_array_length(cells); ++i)
        if (is_str(mpack_node_map_cstr(mpack_node_array_at(cells, i), "state"), "loaded"))
            f->note_loaded++;
    mpack_tree_destroy(&t);
}

static de_ep_stream_source_spec spec(f64 *pos, f64 x, f64 y, f64 z, f32 r) {
    pos[0] = x, pos[1] = y, pos[2] = z;
    de_ep_stream_source_spec s;
    memset(&s, 0, sizeof s);
    s.position.items = pos;
    s.position.count = 3;
    s.radius = r;
    return s;
}

TEST_F(editor_stream, info_and_errors, st_fx) {
    de__ed_stream_register(&f->editor);
    ASSERT_TRUE(DE_HANDLE_VALID(de_component_find(f->engine.world, "DeWorldCellPin")));
    ASSERT_TRUE(DE_HANDLE_VALID(de_component_find(f->engine.world, "DeStreamingSource")));
    mpack_node_t r = info(f);
    EXPECT_FALSE(mpack_node_map_contains_cstr(r, "open"));
    EXPECT_EQ(mpack_node_array_length(mpack_node_map_cstr(r, "worlds")), 0u);
    mpack_node_t axes = mpack_node_map_cstr(r, "axes");
    ASSERT_EQ(mpack_node_array_length(axes), 2u);
    EXPECT_EQ(mpack_node_u32(mpack_node_array_at(axes, 0)), 0u);
    EXPECT_EQ(mpack_node_u32(mpack_node_array_at(axes, 1)), 1u);
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(r, "maxEditorSources")), 8u);
    EXPECT_FALSE(mpack_node_bool(mpack_node_map_cstr(r, "proxyBake")));
    EXPECT_TRUE(is_str(mpack_node_map_cstr(r, "pinComponent"), "DeWorldCellPin"));
    EXPECT_TRUE(is_str(mpack_node_map_cstr(r, "sourceComponent"), "DeStreamingSource"));
    EXPECT_TRUE(is_str(mpack_node_map_cstr(r, "cellComponent"), "DeWorldCell"));

    /* The conformance error cases with nothing open. */
    de_ep_stream_open_params op = {.has_world = true, .world = de_ep_cstr("no-such-world")};
    expect_error(call(f, DE_EP_METHOD_STREAM_OPEN, &op), DE_EP_ERR_NOT_FOUND);
    de_ep_stream_cells_params cp;
    memset(&cp, 0, sizeof cp);
    expect_error(call(f, DE_EP_METHOD_STREAM_CELLS, &cp), DE_EP_ERR_NOT_FOUND);
    de_ep_stream_cell_key far = {INT32_MAX, INT32_MAX};
    de_ep_stream_force_params fp = {.cells = {&far, 1}, .force = DE_EP_STREAM_FORCE_LOAD};
    expect_error(call(f, DE_EP_METHOD_STREAM_FORCE, &fp), DE_EP_ERR_NOT_FOUND);
    f64 pos[3 * 9];
    de_ep_stream_source_spec bad = spec(pos, 0.0, 0.0, 0.0, -1.0f);
    de_ep_stream_editor_sources_params sp = {.sources = {&bad, 1}};
    expect_error(call(f, DE_EP_METHOD_STREAM_SET_EDITOR_SOURCES, &sp), DE_EP_ERR_INVALID_PARAMS);
    de_ep_stream_preview_params pp;
    memset(&pp, 0, sizeof pp);
    pp.sources.items = &bad;
    pp.sources.count = 1;
    expect_error(call(f, DE_EP_METHOD_STREAM_PREVIEW, &pp), DE_EP_ERR_NOT_FOUND);
    u64 ghost = UINT64_MAX - 1;
    de_ep_stream_assignments_params ap = {.has_entities = true, .entities = {&ghost, 1}};
    expect_error(call(f, DE_EP_METHOD_STREAM_ASSIGNMENTS, &ap), DE_EP_ERR_NOT_FOUND);
    de_ep_stream_regenerate_proxy_params rp;
    memset(&rp, 0, sizeof rp);
    expect_error(call(f, DE_EP_METHOD_STREAM_REGENERATE_PROXY, &rp), DE_EP_ERR_NOT_FOUND);
    de_ep_stream_cancel_proxy_params kp = {.job = UINT32_MAX};
    expect_error(call(f, DE_EP_METHOD_STREAM_CANCEL_PROXY, &kp), DE_EP_ERR_NOT_FOUND);

    /* Editor sources: kept with no world open, at most 8, Play refuses them. */
    de_ep_stream_source_spec many[9];
    for (u32 i = 0; i < 9; ++i)
        many[i] = spec(&pos[3 * i], (f64)i, 0.0, 0.0, 10.0f);
    sp.sources.items = many;
    sp.sources.count = 9;
    expect_error(call(f, DE_EP_METHOD_STREAM_SET_EDITOR_SOURCES, &sp), DE_EP_ERR_INVALID_PARAMS);
    pos[0] = NAN;
    sp.sources.count = 1;
    expect_error(call(f, DE_EP_METHOD_STREAM_SET_EDITOR_SOURCES, &sp), DE_EP_ERR_INVALID_PARAMS);
    pos[0] = 5.0;
    const u64 r0 = revision(f);
    sp.sources.count = 2;
    (void)result_of(call(f, DE_EP_METHOD_STREAM_SET_EDITOR_SOURCES, &sp));
    const u64 r1 = revision(f);
    EXPECT_GT(r1, r0);
    (void)result_of(call(f, DE_EP_METHOD_STREAM_SET_EDITOR_SOURCES, &sp));
    EXPECT_EQ(revision(f), r1); /* unchanged: no bump */
    mpack_node_t es = mpack_node_map_cstr(info(f), "editorSources");
    ASSERT_EQ(mpack_node_array_length(es), 2u);
    EXPECT_FLOAT_EQ(
        mpack_node_double(mpack_node_array_at(
            mpack_node_map_cstr(mpack_node_array_at(es, 0), "position"), 0)),
        5.0, 0.0);
    f->editor.mode = DE_EP_WORLD_MODE_PLAY;
    expect_error(call(f, DE_EP_METHOD_STREAM_SET_EDITOR_SOURCES, &sp), DE_EP_ERR_WRONG_MODE);
    op.world = de_ep_cstr("small");
    expect_error(call(f, DE_EP_METHOD_STREAM_OPEN, &op), DE_EP_ERR_WRONG_MODE);
    f->editor.mode = DE_EP_WORLD_MODE_EDIT;

    /* stream.overlay round-trips through stream.info. */
    de_ep_stream_cell_key order[2] = {{1, 2}, {3, 4}};
    de_ep_stream_overlay_params ov = {.grid = true, .memory = false, .sources = true,
                                      .proxies = false, .has_load_order = true,
                                      .load_order = {order, 2}};
    (void)result_of(call(f, DE_EP_METHOD_STREAM_OVERLAY, &ov));
    mpack_node_t o = mpack_node_map_cstr(info(f), "overlay");
    EXPECT_TRUE(mpack_node_bool(mpack_node_map_cstr(o, "grid")));
    EXPECT_FALSE(mpack_node_bool(mpack_node_map_cstr(o, "memory")));
    EXPECT_TRUE(mpack_node_bool(mpack_node_map_cstr(o, "sources")));
    mpack_node_t lo = mpack_node_map_cstr(o, "loadOrder");
    ASSERT_EQ(mpack_node_array_length(lo), 2u);
    EXPECT_EQ(mpack_node_i32(mpack_node_map_cstr(mpack_node_array_at(lo, 1), "cy")), 4);

    /* A disconnect drops the editor sources and bumps the revision. */
    const u64 r2 = revision(f);
    for (u32 i = 0; i < f->editor.reset_count; ++i)
        f->editor.resets[i](&f->editor);
    r = info(f);
    EXPECT_EQ(mpack_node_array_length(mpack_node_map_cstr(r, "editorSources")), 0u);
    EXPECT_GT(mpack_node_u64(mpack_node_map_cstr(r, "revision")), r2);
}

TEST_F(editor_stream, open_force_preview, st_fx) {
#if !DE_HAS_COOKER
    SKIP("this build does not link the cooker (DE_HAS_COOKER: Debug and Dev with tools)");
#endif
    st_fx_assets(f);
    de__ed_stream_register(&f->editor);
    mpack_node_t r = info(f);
    mpack_node_t worlds = mpack_node_map_cstr(r, "worlds");
    ASSERT_EQ(mpack_node_array_length(worlds), 1u);
    EXPECT_TRUE(is_str(mpack_node_map_cstr(mpack_node_array_at(worlds, 0), "name"), "small"));
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(mpack_node_array_at(worlds, 0), "cellSize")),
              256u);

    de_ep_stream_open_params op = {.has_world = true, .world = de_ep_cstr("small")};
    r = result_of(call(f, DE_EP_METHOD_STREAM_OPEN, &op));
    EXPECT_TRUE(is_str(mpack_node_map_cstr(r, "open"), "small"));
    EXPECT_TRUE(mpack_node_map_contains_cstr(r, "stats"));

    /* No sources: every cell is unloaded, with its square for bounds. */
    de_ep_stream_cells_params cp;
    memset(&cp, 0, sizeof cp);
    r = result_of(call(f, DE_EP_METHOD_STREAM_CELLS, &cp));
    mpack_node_t cells = mpack_node_map_cstr(r, "cells");
    ASSERT_EQ(mpack_node_array_length(cells), 4u);
    mpack_node_t c3 = mpack_node_array_at(cells, 3);
    EXPECT_EQ(mpack_node_i32(mpack_node_map_cstr(c3, "cx")), 390);
    EXPECT_TRUE(is_str(mpack_node_map_cstr(c3, "state"), "unloaded"));
    mpack_node_t mx = mpack_node_map_cstr(mpack_node_map_cstr(c3, "bounds"), "max");
    EXPECT_FLOAT_EQ(mpack_node_double(mpack_node_array_at(mx, 0)), 391.0 * 256.0, 0.0);
    cp.has_region = true;
    cp.region.min_x = 0, cp.region.min_y = 0, cp.region.max_x = 1, cp.region.max_y = 0;
    r = result_of(call(f, DE_EP_METHOD_STREAM_CELLS, &cp));
    EXPECT_EQ(mpack_node_array_length(mpack_node_map_cstr(r, "cells")), 2u);

    /* Force errors: an unknown cell, and not in Simulate. */
    de_ep_stream_cell_key keys[2] = {{390, 390}, {7, 7}};
    de_ep_stream_force_params fp = {.cells = {keys, 2}, .force = DE_EP_STREAM_FORCE_LOAD};
    expect_error(call(f, DE_EP_METHOD_STREAM_FORCE, &fp), DE_EP_ERR_NOT_FOUND);
    fp.cells.count = 1;
    f->editor.mode = DE_EP_WORLD_MODE_SIMULATE;
    expect_error(call(f, DE_EP_METHOD_STREAM_FORCE, &fp), DE_EP_ERR_WRONG_MODE);
    f->editor.mode = DE_EP_WORLD_MODE_EDIT;

    /* Force (390, 390) in: it loads, and stream.cells notes say so. */
    const u64 r0 = revision(f);
    r = result_of(call(f, DE_EP_METHOD_STREAM_FORCE, &fp));
    EXPECT_EQ(mpack_node_array_length(mpack_node_map_cstr(r, "cells")), 4u);
    EXPECT_TRUE(is_str(mpack_node_map_cstr(mpack_node_array_at(mpack_node_map_cstr(r, "cells"), 3),
                                           "force"),
                       "load"));
    EXPECT_GT(revision(f), r0);
    for (u32 i = 0; i < 2000 && f->note_loaded == 0; ++i) {
        frame(f);
        de_sleep_ns(1000000ull);
    }
    EXPECT_GT(f->notes, 0u);
    EXPECT_EQ(f->note_loaded, 1u);
    r = result_of(call(f, DE_EP_METHOD_STREAM_CELLS, &(de_ep_stream_cells_params){0}));
    c3 = mpack_node_array_at(mpack_node_map_cstr(r, "cells"), 3);
    EXPECT_TRUE(is_str(mpack_node_map_cstr(c3, "state"), "loaded"));
    EXPECT_GT(mpack_node_u64(mpack_node_map_cstr(c3, "bytes")), 0u);
    EXPECT_GT(mpack_node_u32(mpack_node_map_cstr(c3, "entities")), 0u);
    mpack_node_t stats = mpack_node_map_cstr(r, "stats");
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(stats, "cellsLoaded")), 1u);
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(stats, "cellsFailed")), 0u);
    EXPECT_GT(mpack_node_u64(mpack_node_map_cstr(stats, "bytesResident")), 0u);

    /* Preview with one source in cell (0, 0): (0, 0) then (1, 0); (390, 390) is kept (forced). */
    f64 pos[3];
    de_ep_stream_source_spec s = spec(pos, 128.0, 128.0, 0.0, 0.0f);
    de_ep_stream_preview_params pp;
    memset(&pp, 0, sizeof pp);
    pp.sources.items = &s;
    pp.sources.count = 1;
    r = result_of(call(f, DE_EP_METHOD_STREAM_PREVIEW, &pp));
    mpack_node_t loads = mpack_node_map_cstr(r, "loads");
    ASSERT_EQ(mpack_node_array_length(loads), 2u);
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(mpack_node_array_at(loads, 1), "order")), 2u);
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(mpack_node_array_at(loads, 1), "frame")), 1u);
    EXPECT_EQ(mpack_node_i32(mpack_node_map_cstr(mpack_node_array_at(loads, 1), "cx")), 1);
    EXPECT_EQ(mpack_node_array_length(mpack_node_map_cstr(r, "keep")), 1u);
    EXPECT_FALSE(mpack_node_bool(mpack_node_map_cstr(r, "capped")));

    /* Assignments: a root at (300, 10) is in (1, 0); a DeWorldCellPin on it wins. */
    de_world *w = f->engine.world;
    const de_entity e = de_entity_spawn(w);
    de_transform *t = (de_transform *)de_entity_add(w, e, de_builtin_components(w).transform);
    ASSERT_TRUE(t != NULL);
    *t = de_transform_identity();
    t->position.x = 300.0, t->position.y = 10.0;
    u64 bits = e.bits;
    de_ep_stream_assignments_params ap = {.has_entities = true, .entities = {&bits, 1}};
    r = result_of(call(f, DE_EP_METHOD_STREAM_ASSIGNMENTS, &ap));
    mpack_node_t it = mpack_node_array_at(mpack_node_map_cstr(r, "items"), 0);
    EXPECT_EQ(mpack_node_i32(mpack_node_map_cstr(it, "cx")), 1);
    EXPECT_FALSE(mpack_node_bool(mpack_node_map_cstr(it, "pinned")));
    DeWorldCellPin *pin =
        (DeWorldCellPin *)de_entity_add(w, e, de_component_find(w, "DeWorldCellPin"));
    ASSERT_TRUE(pin != NULL);
    pin->cx = 5, pin->cy = -6;
    r = result_of(call(f, DE_EP_METHOD_STREAM_ASSIGNMENTS, &ap));
    it = mpack_node_array_at(mpack_node_map_cstr(r, "items"), 0);
    EXPECT_EQ(mpack_node_i32(mpack_node_map_cstr(it, "cx")), 5);
    EXPECT_EQ(mpack_node_i32(mpack_node_map_cstr(it, "cy")), -6);
    EXPECT_TRUE(mpack_node_bool(mpack_node_map_cstr(it, "pinned")));
    EXPECT_EQ(mpack_node_i32(mpack_node_map_cstr(mpack_node_map_cstr(it, "auto"), "cx")), 1);

    /* Proxies are on in world_small, but the engine does not bake them yet. */
    de_ep_stream_regenerate_proxy_params rp;
    memset(&rp, 0, sizeof rp);
    expect_error(call(f, DE_EP_METHOD_STREAM_REGENERATE_PROXY, &rp), DE_EP_ERR_UNSUPPORTED);
    rp.has_cells = true;
    rp.cells.items = &keys[1];
    rp.cells.count = 1;
    expect_error(call(f, DE_EP_METHOD_STREAM_REGENERATE_PROXY, &rp), DE_EP_ERR_NOT_FOUND);

    /* A disconnect drops the force: the far cell unloads. */
    for (u32 i = 0; i < f->editor.reset_count; ++i)
        f->editor.resets[i](&f->editor);
    frame(f);
    r = result_of(call(f, DE_EP_METHOD_STREAM_CELLS, &(de_ep_stream_cells_params){0}));
    c3 = mpack_node_array_at(mpack_node_map_cstr(r, "cells"), 3);
    EXPECT_TRUE(is_str(mpack_node_map_cstr(c3, "state"), "unloaded"));
    EXPECT_TRUE(is_str(mpack_node_map_cstr(c3, "force"), "none"));

    /* stream.open with no world closes it. */
    de_ep_stream_open_params close = {0};
    r = result_of(call(f, DE_EP_METHOD_STREAM_OPEN, &close));
    EXPECT_FALSE(mpack_node_map_contains_cstr(r, "open"));
}

#endif /* !DE_SHIPPING */
