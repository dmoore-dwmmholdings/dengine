/* DENG-580: terrain.info, terrain.sample, terrain.exportHeightmap and the terrain.* edit ops
 * (stroke, import, bake, restore) of the DIDE world.terrain capability (protocol 1.11), through
 * the real handlers, the edit transactions and the generated protocol readers and writers. The
 * terrain is the DIDE mock engine's default (33 x 33 samples over 32 m, height 32 m, spacing 1),
 * so the expected numbers are the mock's (protocol/mock/src/terrain.rs). */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "de_test.h"
#if !defined(DE_SHIPPING)
#include "host/editor/editor_internal.h"
#include "host/host_internal.h"
#include "world/terrain/terrain.h"
#include <dengine/de_ecs.h>
#include <dengine/de_render.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif

#define STORAGE  (2u << 20)
#define RESPONSE (512u << 10)
#define OX       10.0
#define OZ       20.0

void de__ed_session_shutdown(void);

typedef struct terrain_fx {
    de_engine engine;
    de__host host;
    de__editor ed;
    unsigned char *storage;
    char *response;
    char *note;
    char params[16384];
    mpack_tree_t tree;
    bool parsed;
    de_component comp;
    de_entity t; /**< the terrain, at (OX, 0, OZ) */
} terrain_fx;

static void terrain_fx_setup(terrain_fx *f) {
    memset(f, 0, sizeof *f);
    ASSERT_EQ(de_log_init(&(de_log_desc){.no_console_sink = true}), DE_OK);
    ASSERT_EQ(de_type_registry_create(NULL, &f->engine.types), DE_OK);
    ASSERT_EQ(de_world_create(&(de_world_desc){.types = f->engine.types}, &f->engine.world), DE_OK);
    (void)de_builtin_components(f->engine.world);
    f->storage = (unsigned char *)malloc(STORAGE);
    f->response = (char *)malloc(RESPONSE);
    f->note = (char *)malloc(256u << 10);
    ASSERT_TRUE(f->storage != NULL && f->response != NULL && f->note != NULL);
    f->ed.e = &f->engine;
    f->ed.h = &f->host;
    f->ed.mode = DE_EP_WORLD_MODE_EDIT;
    f->ed.arena = f->storage;
    f->ed.arena_cap = STORAGE;
    f->ed.note = f->note;
    f->ed.note_cap = 256u << 10;
    f->ed.proto_major = 1;
    f->ed.proto_minor = 17;
    de__ed_edit_register(&f->ed);
    de__ed_terrain_register(&f->ed);
    f->comp = de_component_find(f->engine.world, DE_TERRAIN_COMPONENT);
    ASSERT_TRUE(DE_HANDLE_VALID(f->comp));
    f->t = de_entity_spawn(f->engine.world);
    ASSERT_TRUE(de_entity_add(f->engine.world, f->t, f->comp) != NULL);
    DeTransform *x = (DeTransform *)de_entity_add(
        f->engine.world, f->t, de_builtin_components(f->engine.world).transform);
    ASSERT_TRUE(x != NULL);
    *x = de_transform_identity();
    x->position = (de_dvec3){OX, 0, OZ};
}
static void terrain_fx_teardown(terrain_fx *f) {
    if (f->parsed)
        mpack_tree_destroy(&f->tree);
    for (u32 i = 0; i < f->ed.reset_count; ++i)
        f->ed.resets[i](&f->ed);
    de__ed_session_shutdown();
    de_world_destroy(f->engine.world);
    de_type_registry_destroy(f->engine.types);
    free(f->storage);
    free(f->response);
    free(f->note);
    de_log_shutdown();
}

static DeTerrain *terrain(terrain_fx *f) {
    DeTerrain *t = (DeTerrain *)de_entity_get(f->engine.world, f->t, f->comp);
    ASSERT_TRUE(t != NULL);
    return t;
}
static u32 idx(u32 x, u32 y) {
    return y * DE_TERRAIN_RES + x;
}
static f32 metres(const DeTerrain *t, u32 x, u32 y) {
    return (f32)t->heights[idx(x, y)] * t->height / 65535.0f;
}

/* Calls `m` with the `size` bytes in f->params and returns the parsed reply. */
static mpack_node_t call(terrain_fx *f, de_ep_method m, size_t size) {
    const de_ep_request req = {
        .id = 580, .method = de_ep_cstr(de_ep_method_name(m)), .params = {f->params, size}};
    ASSERT_TRUE(f->ed.handlers[m] != NULL);
    de_ep_arena a = {f->storage, STORAGE, 0};
    mpack_writer_t w;
    mpack_writer_init(&w, f->response, RESPONSE);
    f->ed.handlers[m](&f->ed, &req, &a, &w);
    const size_t used = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    if (f->parsed)
        mpack_tree_destroy(&f->tree);
    mpack_tree_init_data(&f->tree, f->response, used);
    mpack_tree_parse(&f->tree);
    f->parsed = true;
    ASSERT_EQ(mpack_tree_error(&f->tree), mpack_ok);
    mpack_node_t root = mpack_tree_root(&f->tree);
    ASSERT_EQ(mpack_node_u64(mpack_node_map_cstr(root, "id")), 580u);
    return root;
}
#define CALL(f, m, writer, p)                                                                      \
    do {                                                                                           \
        mpack_writer_t w_;                                                                         \
        mpack_writer_init(&w_, (f)->params, sizeof(f)->params);                                    \
        writer(&w_, p);                                                                            \
        const size_t n_ = mpack_writer_buffer_used(&w_);                                           \
        ASSERT_EQ(mpack_writer_destroy(&w_), mpack_ok);                                            \
        root = call(f, m, n_);                                                                     \
    } while (0)

/* The error code, or 0 for a result. */
static int code_of(mpack_node_t root) {
    if (!mpack_node_map_contains_cstr(root, "error")) {
        EXPECT_TRUE(mpack_node_map_contains_cstr(root, "result"));
        return 0;
    }
    return (int)mpack_node_i32(mpack_node_map_cstr(mpack_node_map_cstr(root, "error"), "code"));
}
static mpack_node_t result(mpack_node_t root) {
    ASSERT_EQ(code_of(root), 0);
    return mpack_node_map_cstr(root, "result");
}
static bool str_is(mpack_node_t n, const char *s) {
    return mpack_node_type(n) == mpack_type_str && mpack_node_strlen(n) == strlen(s) &&
           memcmp(mpack_node_str(n), s, strlen(s)) == 0;
}

static mpack_node_t info(terrain_fx *f, u64 e) {
    mpack_node_t root;
    CALL(f, DE_EP_METHOD_TERRAIN_INFO, de_ep_write_terrain_params, &(de_ep_terrain_params){e});
    return root;
}
/* terrain.sample at sample (x, y) of the terrain (world (OX + x, OZ + y)). */
static mpack_node_t sample_xy(terrain_fx *f, u64 e, f64 x, f64 z) {
    mpack_node_t root;
    CALL(f, DE_EP_METHOD_TERRAIN_SAMPLE, de_ep_write_terrain_sample_params,
         (&(de_ep_terrain_sample_params){e, OX + x, OZ + z}));
    return root;
}
static mpack_node_t export_hm(terrain_fx *f, u64 e, const char *path, const char *format,
                              const u32 *tile, u32 tile_count) {
    mpack_node_t root;
    de_ep_heightmap_export_params q = {.entity = e, .path = de_ep_cstr(path)};
    if (format != NULL)
        q.format = de_ep_cstr(format);
    if (tile != NULL) {
        q.has_tile = true;
        q.tile = (de_ep_list_u32){(uint32_t *)tile, tile_count};
    }
    CALL(f, DE_EP_METHOD_TERRAIN_EXPORT_HEIGHTMAP, de_ep_write_heightmap_export_params, &q);
    return root;
}

/* ---- Edit transactions ---- */

static u32 begin(terrain_fx *f) {
    mpack_node_t root;
    CALL(f, DE_EP_METHOD_EDIT_BEGIN, de_ep_write_edit_begin_params,
         (&(de_ep_edit_begin_params){.label = de_ep_cstr("terrain"), .scene = 1}));
    return mpack_node_u32(mpack_node_map_cstr(result(root), "tx"));
}
static int tx_call(terrain_fx *f, de_ep_method m, u32 tx) {
    mpack_node_t root;
    CALL(f, m, de_ep_write_tx_params, (&(de_ep_tx_params){.tx = tx}));
    return code_of(root);
}
static int apply(terrain_fx *f, u32 tx, const de_ep_edit_op *op) {
    mpack_node_t root;
    CALL(f, DE_EP_METHOD_EDIT_APPLY, de_ep_write_edit_apply_params,
         (&(de_ep_edit_apply_params){.tx = tx, .ops = {(de_ep_edit_op *)op, 1}}));
    return code_of(root);
}
/* One op in its own transaction: the apply code; committed when it is 0, cancelled otherwise. */
static int edit(terrain_fx *f, const de_ep_edit_op *op, u32 *tx_out) {
    const u32 tx = begin(f);
    const int code = apply(f, tx, op);
    EXPECT_EQ(tx_call(f, code == 0 ? DE_EP_METHOD_EDIT_COMMIT : DE_EP_METHOD_EDIT_CANCEL, tx), 0);
    if (tx_out != NULL)
        *tx_out = tx;
    return code;
}

static char g_value[8192];
/* A terrain.stroke op on the terrain, its value `s` (a tool and one dab at sample (x, y)). */
static de_ep_edit_op stroke_op(terrain_fx *f, de_ep_terrain_stroke *s, const f32 dab[4]) {
    s->points = (de_ep_list_f32){(float *)dab, 4};
    mpack_writer_t w;
    mpack_writer_init(&w, g_value, sizeof g_value);
    de_ep_write_terrain_stroke(&w, s);
    const size_t n = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    return (de_ep_edit_op){.op = de_ep_cstr("terrain.stroke"),
                           .has_entity = true,
                           .entity = f->t.bits,
                           .has_value = true,
                           .value = {g_value, n}};
}
static int stroke(terrain_fx *f, const char *tool, f32 x, f32 y, f32 radius, f32 strength,
                  de_ep_terrain_stroke *extra) {
    de_ep_terrain_stroke s = extra != NULL ? *extra : (de_ep_terrain_stroke){0};
    s.tool = de_ep_cstr(tool);
    s.radius = radius;
    s.strength = strength;
    const f32 dab[4] = {(f32)OX + x, 0, (f32)OZ + y, 1};
    const de_ep_edit_op op = stroke_op(f, &s, dab);
    return edit(f, &op, NULL);
}

/* ---- terrain.info and terrain.sample ---- */

TEST_F(editor_terrain, capability_and_info_shape, terrain_fx) {
    bool offered = false;
    for (u32 i = 0; i < f->ed.cap_count; ++i)
        offered = offered || strcmp(f->ed.caps[i], "world.terrain") == 0;
    EXPECT_TRUE(offered);
    terrain(f)->order[0] = 2;
    terrain(f)->order[2] = 0;
    memcpy(terrain(f)->layers[2].name, "Rock", sizeof "Rock");
    const mpack_node_t r = result(info(f, f->t.bits));
    EXPECT_EQ(mpack_node_u64(mpack_node_map_cstr(r, "entity")), f->t.bits);
    EXPECT_TRUE(str_is(mpack_node_map_cstr(r, "component"), "DeTerrain"));
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(r, "resolution")), 33u);
    EXPECT_FLOAT_EQ(mpack_node_float(mpack_node_map_cstr(r, "size")), 32.0f, 1e-6f);
    EXPECT_FLOAT_EQ(mpack_node_float(mpack_node_map_cstr(r, "height")), 32.0f, 1e-6f);
    const mpack_node_t org = mpack_node_map_cstr(r, "origin");
    EXPECT_FLOAT_EQ((f32)mpack_node_double(mpack_node_array_at(org, 0)), (f32)OX, 1e-6f);
    EXPECT_FLOAT_EQ((f32)mpack_node_double(mpack_node_array_at(org, 2)), (f32)OZ, 1e-6f);
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(r, "tileSize")), 16u);
    const mpack_node_t tiles = mpack_node_map_cstr(r, "tiles");
    EXPECT_EQ(mpack_node_u32(mpack_node_array_at(tiles, 0)), 3u);
    EXPECT_EQ(mpack_node_u32(mpack_node_array_at(tiles, 1)), 3u);
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(r, "maxLayers")), 4u);
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(r, "maxRules")), 4u);
    const mpack_node_t layers = mpack_node_map_cstr(r, "layers");
    ASSERT_EQ(mpack_node_array_length(layers), 4u);
    const mpack_node_t first = mpack_node_array_at(layers, 0);
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(first, "slot")), 2u);
    EXPECT_TRUE(str_is(mpack_node_map_cstr(first, "name"), "Rock"));
    EXPECT_TRUE(str_is(mpack_node_map_cstr(first, "material"), ""));
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(mpack_node_array_at(layers, 2), "slot")), 0u);
    const mpack_node_t tools = mpack_node_map_cstr(r, "tools");
    ASSERT_EQ(mpack_node_array_length(tools), 12u);
    EXPECT_TRUE(str_is(mpack_node_array_at(tools, 0), "raise"));
    EXPECT_TRUE(str_is(mpack_node_array_at(tools, 11), "hole"));
    const mpack_node_t formats = mpack_node_map_cstr(r, "formats");
    ASSERT_EQ(mpack_node_array_length(formats), 3u); /* Dev builds read and write exr */
    EXPECT_TRUE(str_is(mpack_node_array_at(formats, 0), "png16"));
    EXPECT_TRUE(str_is(mpack_node_array_at(formats, 2), "exr"));
}

TEST_F(editor_terrain, sample_reads_height_weights_and_hole, terrain_fx) {
    DeTerrain *t = terrain(f);
    t->heights[idx(3, 4)] = 32768;
    t->holes[idx(5, 5)] = 1;
    t->splat[idx(3, 4) * 4 + 0] = 155;
    t->splat[idx(3, 4) * 4 + 1] = 100;
    mpack_node_t r = result(sample_xy(f, f->t.bits, 3.2, 3.9)); /* nearest is (3, 4) */
    EXPECT_FLOAT_EQ(mpack_node_float(mpack_node_map_cstr(r, "height")), 32768.0f * 32.0f / 65535.0f,
                    1e-4f);
    /* Slope is the central difference: 0 on a lone spike, steep beside it. */
    EXPECT_FLOAT_EQ(mpack_node_float(mpack_node_map_cstr(r, "slope")), 0.0f, 1e-6f);
    const mpack_node_t wts = mpack_node_map_cstr(r, "weights");
    ASSERT_EQ(mpack_node_array_length(wts), 4u);
    EXPECT_FLOAT_EQ(mpack_node_float(mpack_node_array_at(wts, 0)), 155.0f / 255.0f, 1e-5f);
    EXPECT_FLOAT_EQ(mpack_node_float(mpack_node_array_at(wts, 1)), 100.0f / 255.0f, 1e-5f);
    EXPECT_FALSE(mpack_node_bool(mpack_node_map_cstr(r, "hole")));
    const f32 beside = mpack_node_float(mpack_node_map_cstr(result(sample_xy(f, f->t.bits, 4, 4)), "slope"));
    EXPECT_TRUE(beside > 80.0f && beside < 90.0f); /* atan(16 m / 2 m) = 82.9 degrees */
    r = result(sample_xy(f, f->t.bits, 5, 5));
    EXPECT_TRUE(mpack_node_bool(mpack_node_map_cstr(r, "hole")));
    r = result(sample_xy(f, f->t.bits, 20, 20)); /* flat ground */
    EXPECT_FLOAT_EQ(mpack_node_float(mpack_node_map_cstr(r, "height")), 0.0f, 1e-6f);
    EXPECT_FLOAT_EQ(mpack_node_float(mpack_node_map_cstr(r, "slope")), 0.0f, 1e-6f);
}

TEST_F(editor_terrain, missing_terrain_and_off_grid_are_not_found, terrain_fx) {
    const de_entity plain = de_entity_spawn(f->engine.world);
    EXPECT_EQ(code_of(info(f, plain.bits)), DE_EP_ERR_NOT_FOUND);
    EXPECT_EQ(code_of(sample_xy(f, plain.bits, 1, 1)), DE_EP_ERR_NOT_FOUND);
    EXPECT_EQ(code_of(export_hm(f, plain.bits, "unused.png", NULL, NULL, 0)), DE_EP_ERR_NOT_FOUND);
    EXPECT_EQ(code_of(sample_xy(f, f->t.bits, -5, 1)), DE_EP_ERR_NOT_FOUND);
    EXPECT_EQ(code_of(sample_xy(f, f->t.bits, 1, 40)), DE_EP_ERR_NOT_FOUND);
    const f32 dab[4] = {0, 0, 0, 1};
    de_ep_terrain_stroke s = {.tool = de_ep_cstr("raise"), .radius = 2, .strength = 1};
    de_ep_edit_op op = stroke_op(f, &s, dab);
    op.entity = plain.bits;
    EXPECT_EQ(edit(f, &op, NULL), DE_EP_ERR_NOT_FOUND);
}

/* ---- Strokes, undo and redo ---- */

TEST_F(editor_terrain, raise_changes_the_footprint_and_undo_restores_bytes, terrain_fx) {
    static DeTerrain before, raised;
    before = *terrain(f);
    u32 tx = 0;
    de_ep_terrain_stroke s = {.tool = de_ep_cstr("raise"), .radius = 2, .strength = 1};
    const f32 dab[4] = {(f32)OX + 4, 0, (f32)OZ + 4, 1};
    const de_ep_edit_op op = stroke_op(f, &s, dab);
    ASSERT_EQ(edit(f, &op, &tx), 0);
    const DeTerrain *t = terrain(f);
    /* Centre weight 1 * (1 - 0) * 1, step max(2 / 10, 0.1) = 0.2 m. */
    EXPECT_FLOAT_EQ(metres(t, 4, 4), 0.2f, 1e-3f);
    EXPECT_TRUE(metres(t, 5, 4) > 0.0f && metres(t, 5, 4) < metres(t, 4, 4));
    EXPECT_EQ(t->heights[idx(7, 4)], 0u); /* outside the radius */
    EXPECT_EQ(t->heights[idx(30, 30)], 0u);
    raised = *t;
    ASSERT_EQ(tx_call(f, DE_EP_METHOD_EDIT_UNDO, tx), 0);
    EXPECT_EQ(memcmp(terrain(f), &before, sizeof before), 0);
    ASSERT_EQ(tx_call(f, DE_EP_METHOD_EDIT_REDO, tx), 0);
    EXPECT_EQ(memcmp(terrain(f), &raised, sizeof raised), 0);
}

TEST_F(editor_terrain, inverse_holds_only_the_changed_tiles, terrain_fx) {
    de_ep_terrain_stroke s = {.tool = de_ep_cstr("raise"), .radius = 2, .strength = 1};
    const f32 dab[4] = {(f32)OX + 4, 0, (f32)OZ + 4, 1}; /* inside tile (0, 0) */
    const de_ep_edit_op op = stroke_op(f, &s, dab);
    ASSERT_TRUE(f->ed.terrain_op != NULL);
    de_ep_arena a = {f->storage, STORAGE, 0};
    de_ep_edit_op inv;
    de_ep_error code = 0;
    char msg[256] = "";
    ASSERT_TRUE(f->ed.terrain_op(&f->ed, &a, &op, &inv, &code, msg, sizeof msg));
    EXPECT_TRUE(de_ep_str_eq(inv.op, "terrain.restore"));
    EXPECT_EQ(inv.entity, f->t.bits);
    mpack_tree_t tree;
    mpack_tree_init_data(&tree, inv.value.ptr, inv.value.len);
    mpack_tree_parse(&tree);
    ASSERT_EQ(mpack_tree_error(&tree), mpack_ok);
    const mpack_node_t v = mpack_tree_root(&tree);
    const mpack_node_t tiles = mpack_node_map_cstr(v, "tiles");
    ASSERT_EQ(mpack_node_array_length(tiles), 1u);
    EXPECT_EQ(mpack_node_u32(mpack_node_array_at(mpack_node_array_at(tiles, 0), 0)), 0u);
    EXPECT_EQ(mpack_node_u32(mpack_node_array_at(mpack_node_array_at(tiles, 0), 1)), 0u);
    EXPECT_EQ(mpack_node_array_length(mpack_node_map_cstr(v, "heights")), 256u);
    EXPECT_EQ(mpack_node_array_length(mpack_node_map_cstr(v, "splat")), 1024u);
    EXPECT_FALSE(mpack_node_map_contains_cstr(v, "rules"));
    /* The inverse applied directly restores the flat ground. */
    de_ep_edit_op back;
    ASSERT_TRUE(f->ed.terrain_op(&f->ed, &a, &inv, &back, &code, msg, sizeof msg));
    mpack_tree_destroy(&tree);
    for (u32 i = 0; i < DE_TERRAIN_SAMPLES; ++i)
        ASSERT_EQ(terrain(f)->heights[i], 0u);
}

TEST_F(editor_terrain, flatten_and_smooth, terrain_fx) {
    de_ep_terrain_stroke p = {.has_target = true, .target = 5};
    ASSERT_EQ(stroke(f, "flatten", 16, 16, 3, 1, &p), 0);
    const f32 c = metres(terrain(f), 16, 16);
    EXPECT_TRUE(c > 0.0f && c <= 5.0f + 1e-3f);
    for (int i = 0; i < 40; ++i)
        ASSERT_EQ(stroke(f, "flatten", 16, 16, 3, 1, &p), 0);
    EXPECT_FLOAT_EQ(metres(terrain(f), 16, 16), 5.0f, 1e-3f);

    terrain(f)->heights[idx(8, 24)] = 20000; /* a spike */
    ASSERT_EQ(stroke(f, "smooth", 8, 24, 2, 1, NULL), 0);
    const DeTerrain *t = terrain(f);
    EXPECT_TRUE(t->heights[idx(8, 24)] < 20000u);
    EXPECT_TRUE(t->heights[idx(9, 24)] > 0u);
}

TEST_F(editor_terrain, paint_keeps_the_weights_summing_to_255, terrain_fx) {
    de_ep_terrain_stroke p = {.has_layer = true, .layer = 1};
    ASSERT_EQ(stroke(f, "paint", 10, 10, 3, 0.5f, &p), 0);
    const DeTerrain *t = terrain(f);
    EXPECT_TRUE(t->splat[idx(10, 10) * 4 + 1] > 0u);
    EXPECT_EQ(t->splat[idx(20, 20) * 4 + 1], 0u);
    for (u32 i = 0; i < DE_TERRAIN_SAMPLES; ++i) {
        const u32 sum =
            t->splat[i * 4] + t->splat[i * 4 + 1] + t->splat[i * 4 + 2] + t->splat[i * 4 + 3];
        ASSERT_EQ(sum, 255u);
    }
    p.has_erase = true;
    p.erase = true;
    for (int i = 0; i < 20; ++i)
        ASSERT_EQ(stroke(f, "paint", 10, 10, 3, 1, &p), 0);
    EXPECT_EQ(terrain(f)->splat[idx(10, 10) * 4 + 1], 0u);
    EXPECT_EQ(terrain(f)->splat[idx(10, 10) * 4 + 0], 255u);
}

TEST_F(editor_terrain, hole_cuts_and_erase_fills, terrain_fx) {
    ASSERT_EQ(stroke(f, "hole", 12, 12, 1.5f, 1, NULL), 0);
    EXPECT_EQ(terrain(f)->holes[idx(12, 12)], 1u);
    EXPECT_EQ(terrain(f)->holes[idx(20, 12)], 0u);
    EXPECT_TRUE(mpack_node_bool(mpack_node_map_cstr(result(sample_xy(f, f->t.bits, 12, 12)), "hole")));
    de_ep_terrain_stroke p = {.has_erase = true, .erase = true};
    ASSERT_EQ(stroke(f, "hole", 12, 12, 1.5f, 1, &p), 0);
    EXPECT_EQ(terrain(f)->holes[idx(12, 12)], 0u);
}

TEST_F(editor_terrain, rules_preview_and_bake_writes_the_splat, terrain_fx) {
    DeTerrain *t = terrain(f);
    t->rules[0] = (de_terrain_rule){.enabled = true, .layer = 3}; /* unbounded: everywhere */
    mpack_node_t r = result(sample_xy(f, f->t.bits, 6, 6));
    EXPECT_FLOAT_EQ(mpack_node_float(mpack_node_array_at(mpack_node_map_cstr(r, "weights"), 3)),
                    1.0f, 1e-6f);
    EXPECT_FLOAT_EQ(mpack_node_float(mpack_node_array_at(mpack_node_map_cstr(r, "baked"), 0)),
                    1.0f, 1e-6f);
    u32 tx = 0;
    const de_ep_edit_op bake = {
        .op = de_ep_cstr("terrain.bake"), .has_entity = true, .entity = f->t.bits};
    ASSERT_EQ(edit(f, &bake, &tx), 0);
    t = terrain(f);
    EXPECT_FALSE(t->rules[0].enabled);
    EXPECT_EQ(t->splat[idx(6, 6) * 4 + 3], 255u);
    EXPECT_EQ(t->splat[idx(6, 6) * 4 + 0], 0u);
    ASSERT_EQ(tx_call(f, DE_EP_METHOD_EDIT_UNDO, tx), 0);
    t = terrain(f);
    EXPECT_TRUE(t->rules[0].enabled);
    EXPECT_EQ(t->rules[0].layer, 3u);
    EXPECT_EQ(t->splat[idx(6, 6) * 4 + 0], 255u);
}

TEST_F(editor_terrain, grid_fields_are_read_only, terrain_fx) {
    char v[8];
    mpack_writer_t vw;
    mpack_writer_init(&vw, v, sizeof v);
    mpack_write_float(&vw, 64.0f);
    const size_t vn = mpack_writer_buffer_used(&vw);
    ASSERT_EQ(mpack_writer_destroy(&vw), mpack_ok);
    const de_ep_edit_op op = {.op = de_ep_cstr("set"),
                              .has_entity = true,
                              .entity = f->t.bits,
                              .has_path = true,
                              .path = de_ep_cstr("DeTerrain.size"),
                              .has_value = true,
                              .value = {v, vn}};
    EXPECT_EQ(edit(f, &op, NULL), DE_EP_ERR_INVALID_PARAMS);
    EXPECT_FLOAT_EQ(terrain(f)->size, 32.0f, 0.0f);
}

TEST_F(editor_terrain, bad_strokes_and_tiles_are_invalid, terrain_fx) {
    EXPECT_EQ(stroke(f, "dig", 4, 4, 2, 1, NULL), DE_EP_ERR_INVALID_PARAMS);
    de_ep_terrain_stroke p = {.has_layer = true, .layer = 4};
    EXPECT_EQ(stroke(f, "paint", 4, 4, 2, 1, &p), DE_EP_ERR_INVALID_PARAMS);
    const u32 far[2] = {3, 0}, one[1] = {1};
    EXPECT_EQ(code_of(export_hm(f, f->t.bits, "unused.png", NULL, far, 2)),
              DE_EP_ERR_INVALID_PARAMS);
    EXPECT_EQ(code_of(export_hm(f, f->t.bits, "unused.png", NULL, one, 1)),
              DE_EP_ERR_INVALID_PARAMS);
    EXPECT_EQ(code_of(export_hm(f, f->t.bits, "unused.bmp", "bmp", NULL, 0)),
              DE_EP_ERR_INVALID_PARAMS);
    const de_ep_edit_op unknown = {
        .op = de_ep_cstr("terrain.dig"), .has_entity = true, .entity = f->t.bits};
    EXPECT_EQ(edit(f, &unknown, NULL), DE_EP_ERR_INVALID_PARAMS);
    const de_ep_edit_op restore = {
        .op = de_ep_cstr("terrain.restore"), .has_entity = true, .entity = f->t.bits};
    EXPECT_EQ(edit(f, &restore, NULL), DE_EP_ERR_INVALID_PARAMS);
    for (u32 i = 0; i < DE_TERRAIN_SAMPLES; ++i)
        ASSERT_EQ(terrain(f)->heights[i], 0u);
}

/* ---- Heightmap import and export ---- */

static void temp_path(char *out, size_t cap, const char *ext) {
#if defined(_WIN32)
    snprintf(out, cap, "de_terrain_test_%s.%s", ext, ext);
#else
    snprintf(out, cap, "de_terrain_test_%d.%s", (int)getpid(), ext);
#endif
}
static void pattern(DeTerrain *t) {
    for (u32 y = 0; y < DE_TERRAIN_RES; ++y)
        for (u32 x = 0; x < DE_TERRAIN_RES; ++x)
            t->heights[idx(x, y)] = (u16)((x * 1931u + y * 977u) % 60000u);
}
static int import_hm(terrain_fx *f, const char *path, const char *value_tile_format, const u32 *tile) {
    char v[64];
    mpack_writer_t vw;
    mpack_writer_init(&vw, v, sizeof v);
    mpack_start_map(&vw, (value_tile_format != NULL) + (tile != NULL));
    if (value_tile_format != NULL) {
        mpack_write_cstr(&vw, "format");
        mpack_write_cstr(&vw, value_tile_format);
    }
    if (tile != NULL) {
        mpack_write_cstr(&vw, "tile");
        mpack_start_array(&vw, 2);
        mpack_write_u32(&vw, tile[0]);
        mpack_write_u32(&vw, tile[1]);
        mpack_finish_array(&vw);
    }
    mpack_finish_map(&vw);
    const size_t vn = mpack_writer_buffer_used(&vw);
    ASSERT_EQ(mpack_writer_destroy(&vw), mpack_ok);
    const de_ep_edit_op op = {.op = de_ep_cstr("terrain.import"),
                              .has_entity = true,
                              .entity = f->t.bits,
                              .has_path = true,
                              .path = de_ep_cstr(path),
                              .has_value = true,
                              .value = {v, vn}};
    return edit(f, &op, NULL);
}
/* Exports the pattern whole in `ext`, clears the heights, imports the file: the pattern again,
 * to one height step (exr is metres in f32). */
static void round_trip_whole(terrain_fx *f, const char *ext) {
    char path[128];
    temp_path(path, sizeof path, ext);
    pattern(terrain(f));
    static DeTerrain want;
    want = *terrain(f);
    const mpack_node_t r = result(export_hm(f, f->t.bits, path, NULL, NULL, 0));
    EXPECT_TRUE(str_is(mpack_node_map_cstr(r, "path"), path));
    memset(terrain(f)->heights, 0, sizeof want.heights);
    ASSERT_EQ(import_hm(f, path, NULL, NULL), 0);
    for (u32 i = 0; i < DE_TERRAIN_SAMPLES; ++i) {
        const int d = (int)terrain(f)->heights[i] - (int)want.heights[i];
        ASSERT_TRUE(d >= -1 && d <= 1);
    }
    remove(path);
}

TEST_F(editor_terrain, png16_round_trip, terrain_fx) {
    round_trip_whole(f, "png");
}
TEST_F(editor_terrain, raw16_round_trip, terrain_fx) {
    round_trip_whole(f, "r16");
}
TEST_F(editor_terrain, exr_round_trip, terrain_fx) {
    round_trip_whole(f, "exr");
}

TEST_F(editor_terrain, tile_export_and_import_touch_only_the_tile, terrain_fx) {
    char path[128];
    temp_path(path, sizeof path, "raw");
    pattern(terrain(f));
    static DeTerrain want;
    want = *terrain(f);
    const u32 tile[2] = {1, 1};
    ASSERT_EQ(code_of(export_hm(f, f->t.bits, path, "raw16", tile, 2)), 0);
    memset(terrain(f)->heights, 0, sizeof want.heights);
    ASSERT_EQ(import_hm(f, path, "raw16", tile), 0);
    const DeTerrain *t = terrain(f);
    for (u32 y = 0; y < DE_TERRAIN_RES; ++y)
        for (u32 x = 0; x < DE_TERRAIN_RES; ++x) {
            const bool in = x >= 16 && y >= 16; /* tile (1, 1) and its far edge */
            const int d = (int)t->heights[idx(x, y)] - (int)(in ? want.heights[idx(x, y)] : 0);
            ASSERT_TRUE(d >= -1 && d <= 1);
        }
    const u32 bad[2] = {0, 3};
    EXPECT_EQ(import_hm(f, path, NULL, bad), DE_EP_ERR_INVALID_PARAMS);
    EXPECT_EQ(import_hm(f, path, "bmp", NULL), DE_EP_ERR_INVALID_PARAMS);
    remove(path);
    EXPECT_EQ(import_hm(f, path, NULL, NULL), DE_EP_ERR_INVALID_PARAMS); /* no file */
}

#endif
