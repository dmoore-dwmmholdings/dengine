/* DENG-777: the spline.carve edit op (DIDE protocol 1.12, world.splines on a world.terrain
 * entity) through the real handlers, the edit transactions and the generated protocol readers and
 * writers. The terrain is the DIDE mock engine's default (33 x 33 samples over 32 m, height 32 m,
 * spacing 1) at (OX, 0, OZ), so the expected numbers are the mock's (mock-engine/src/splines.rs,
 * test carve_lowers_terrain_under_the_curve_and_its_inverse_restores_it). */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include "de_test.h"
#if !defined(DE_SHIPPING)
#include "host/editor/editor_internal.h"
#include "ecs/change_internal.h"
#include "host/host_internal.h"
#include "world/spline/spline.h"
#include "world/terrain/terrain.h"
#include <dengine/de_ecs.h>
#include <dengine/de_render.h>
#include <stdlib.h>
#include <string.h>

#define STORAGE  (2u << 20)
#define RESPONSE (512u << 10)
#define OX       10.0
#define OZ       20.0

void de__ed_session_shutdown(void);

typedef struct carve_fx {
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
    de_spline_ids ids;
    de_entity t; /**< the terrain, at (OX, 0, OZ) */
} carve_fx;

static void place(carve_fx *f, de_entity e, f64 x, f64 y, f64 z) {
    DeTransform *xf = (DeTransform *)de_entity_add(
        f->engine.world, e, de_builtin_components(f->engine.world).transform);
    ASSERT_TRUE(xf != NULL);
    *xf = de_transform_identity();
    xf->position = (de_dvec3){x, y, z};
}

static void carve_fx_setup(carve_fx *f) {
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
    de__ed_spline_register(&f->ed);
    de__ed_terrain_register(&f->ed);
    f->comp = de_component_find(f->engine.world, DE_TERRAIN_COMPONENT);
    ASSERT_TRUE(DE_HANDLE_VALID(f->comp));
    ASSERT_EQ(de__spline_register(f->engine.world, &f->ids), DE_OK);
    f->t = de_entity_spawn(f->engine.world);
    ASSERT_TRUE(de_entity_add(f->engine.world, f->t, f->comp) != NULL);
    place(f, f->t, OX, 0, OZ);
}
static void carve_fx_teardown(carve_fx *f) {
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

static DeTerrain *terrain(carve_fx *f) {
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
/* The whole terrain at `m` metres. */
static void level(carve_fx *f, f32 m) {
    DeTerrain *t = terrain(f);
    for (u32 i = 0; i < DE_TERRAIN_SAMPLES; ++i)
        t->heights[i] = (u16)(m / t->height * 65535.0f + 0.5f);
}

/* A spline entity at (x, y, z) through `n` local points (xyz each). */
static de_entity spline(carve_fx *f, de_spline_kind kind, const f32 *pts, u32 n, f64 x, f64 y,
                        f64 z) {
    const de_entity e = de_entity_spawn(f->engine.world);
    DeSpline *s = (DeSpline *)de_entity_add(f->engine.world, e, f->ids.spline);
    ASSERT_TRUE(s != NULL);
    s->kind = kind;
    s->count = n;
    for (u32 i = 0; i < n; ++i)
        s->points[i].position = (de_vec3){pts[3 * i], pts[3 * i + 1], pts[3 * i + 2]};
    place(f, e, x, y, z);
    return e;
}

/* Calls `m` with the `size` bytes in f->params and returns the parsed reply. */
static mpack_node_t call(carve_fx *f, de_ep_method m, size_t size) {
    const de_ep_request req = {
        .id = 777, .method = de_ep_cstr(de_ep_method_name(m)), .params = {f->params, size}};
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
    return mpack_tree_root(&f->tree);
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

static int code_of(mpack_node_t root) {
    if (!mpack_node_map_contains_cstr(root, "error"))
        return 0;
    return (int)mpack_node_i32(mpack_node_map_cstr(mpack_node_map_cstr(root, "error"), "code"));
}

static u32 begin(carve_fx *f) {
    mpack_node_t root;
    CALL(f, DE_EP_METHOD_EDIT_BEGIN, de_ep_write_edit_begin_params,
         (&(de_ep_edit_begin_params){.label = de_ep_cstr("carve"), .scene = 1}));
    EXPECT_EQ(code_of(root), 0);
    return mpack_node_u32(mpack_node_map_cstr(mpack_node_map_cstr(root, "result"), "tx"));
}
static int tx_call(carve_fx *f, de_ep_method m, u32 tx) {
    mpack_node_t root;
    CALL(f, m, de_ep_write_tx_params, (&(de_ep_tx_params){.tx = tx}));
    return code_of(root);
}
/* One op in its own transaction: the apply code; committed when it is 0, cancelled otherwise. */
static int edit(carve_fx *f, const de_ep_edit_op *op, u32 *tx_out) {
    const u32 tx = begin(f);
    mpack_node_t root;
    CALL(f, DE_EP_METHOD_EDIT_APPLY, de_ep_write_edit_apply_params,
         (&(de_ep_edit_apply_params){.tx = tx, .ops = {(de_ep_edit_op *)op, 1}}));
    const int code = code_of(root);
    EXPECT_EQ(tx_call(f, code == 0 ? DE_EP_METHOD_EDIT_COMMIT : DE_EP_METHOD_EDIT_CANCEL, tx), 0);
    if (tx_out != NULL)
        *tx_out = tx;
    return code;
}

static char g_value[256];
/* A spline.carve op on terrain `terrain_bits`. */
static de_ep_edit_op carve_op(u64 terrain_bits, de_entity s, f32 width, f32 falloff, f32 depth) {
    const de_ep_spline_carve v = {s.bits, width, falloff, depth};
    mpack_writer_t w;
    mpack_writer_init(&w, g_value, sizeof g_value);
    de_ep_write_spline_carve(&w, &v);
    const size_t n = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    return (de_ep_edit_op){.op = de_ep_cstr("spline.carve"),
                           .has_entity = true,
                           .entity = terrain_bits,
                           .has_value = true,
                           .value = {g_value, n}};
}

/* A straight spline across the terrain at z = OZ + 16, curve height 3 m (the mock's test). */
static de_entity cross_line(carve_fx *f) {
    const f32 pts[6] = {(f32)OX + 0, 3, (f32)OZ + 16, (f32)OX + 32, 3, (f32)OZ + 16};
    return spline(f, DE_SPLINE_CATMULL_ROM, pts, 2, 0, 0, 0);
}

TEST_F(editor_spline_carve, lowers_the_terrain_under_the_curve, carve_fx) {
    level(f, 4.0f);
    const de_entity s = cross_line(f);
    /* A 2 m wide ditch 1 m below the 3 m curve, blending back over 2 m. */
    const de_ep_edit_op op = carve_op(f->t.bits, s, 2, 2, 1);
    ASSERT_EQ(edit(f, &op, NULL), 0);
    const DeTerrain *t = terrain(f);
    EXPECT_FLOAT_EQ(metres(t, 16, 16), 2.0f, 0.01f); /* on the curve */
    EXPECT_FLOAT_EQ(metres(t, 16, 17), 2.0f, 0.01f); /* within width / 2 */
    /* 2 m off: (d - half) / falloff = 0.5, smoothstep 0.5: halfway from 4 back to 2. */
    EXPECT_FLOAT_EQ(metres(t, 16, 18), 3.0f, 0.01f);
    EXPECT_FLOAT_EQ(metres(t, 16, 19), 4.0f, 0.01f); /* past the falloff */
    EXPECT_FLOAT_EQ(metres(t, 16, 2), 4.0f, 0.01f);
}

TEST_F(editor_spline_carve, undo_and_redo_restore_the_exact_bytes, carve_fx) {
    level(f, 4.0f);
    static DeTerrain before, carved;
    before = *terrain(f);
    const de_entity s = cross_line(f);
    u32 tx = 0;
    const de_ep_edit_op op = carve_op(f->t.bits, s, 2, 2, 1);
    ASSERT_EQ(edit(f, &op, &tx), 0);
    carved = *terrain(f);
    EXPECT_NE(memcmp(&carved, &before, sizeof before), 0);
    ASSERT_EQ(tx_call(f, DE_EP_METHOD_EDIT_UNDO, tx), 0);
    EXPECT_EQ(memcmp(terrain(f), &before, sizeof before), 0);
    ASSERT_EQ(tx_call(f, DE_EP_METHOD_EDIT_REDO, tx), 0);
    EXPECT_EQ(memcmp(terrain(f), &carved, sizeof carved), 0);
}

/* DENG-793: a set on a fixed-array field (DeSpline.points, an array of structs) has an inverse
 * that carries the old array, so undo restores it exactly. */
TEST_F(editor_spline_carve, set_on_a_fixed_array_field_undoes_to_the_exact_old_array, carve_fx) {
    const f32 pts[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    const de_entity s = spline(f, DE_SPLINE_CATMULL_ROM, pts, 3, 0, 0, 0);
    DeSpline *sp = (DeSpline *)de_entity_get(f->engine.world, s, f->ids.spline);
    ASSERT_TRUE(sp != NULL);
    sp->points[1].out_tangent = (de_vec3){0.5f, 0.25f, 0.125f};
    static de_spline_point before[DE_SPLINE_MAX_POINTS];
    memcpy(before, sp->points, sizeof before);

    /* Two new points, each {position: [x, y, z]}. */
    static char value[512];
    mpack_writer_t w;
    mpack_writer_init(&w, value, sizeof value);
    mpack_start_array(&w, 2);
    for (u32 i = 0; i < 2; ++i) {
        mpack_start_map(&w, 1);
        mpack_write_cstr(&w, "position");
        mpack_start_array(&w, 3);
        for (u32 c = 0; c < 3; ++c)
            mpack_write_double(&w, 100.0 + (double)(i * 3 + c));
        mpack_finish_array(&w);
        mpack_finish_map(&w);
    }
    mpack_finish_array(&w);
    const size_t vn = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    const de_ep_edit_op op = {.op = de_ep_cstr("set"),
                              .has_entity = true,
                              .entity = s.bits,
                              .has_path = true,
                              .path = de_ep_cstr("DeSpline.points"),
                              .has_value = true,
                              .value = {value, vn}};
    u32 tx = 0;
    ASSERT_EQ(edit(f, &op, &tx), 0);
    sp = (DeSpline *)de_entity_get(f->engine.world, s, f->ids.spline);
    EXPECT_FLOAT_EQ(sp->points[0].position.x, 100.0f, 0.0f);
    EXPECT_FLOAT_EQ(sp->points[1].position.z, 105.0f, 0.0f);
    EXPECT_NE(memcmp(sp->points, before, sizeof before), 0);

    ASSERT_EQ(tx_call(f, DE_EP_METHOD_EDIT_UNDO, tx), 0);
    sp = (DeSpline *)de_entity_get(f->engine.world, s, f->ids.spline);
    EXPECT_EQ(memcmp(sp->points, before, sizeof before), 0);
    ASSERT_EQ(tx_call(f, DE_EP_METHOD_EDIT_REDO, tx), 0);
    sp = (DeSpline *)de_entity_get(f->engine.world, s, f->ids.spline);
    EXPECT_FLOAT_EQ(sp->points[0].position.x, 100.0f, 0.0f);
}

TEST_F(editor_spline_carve, inverse_is_a_restore_of_only_the_changed_tiles, carve_fx) {
    level(f, 4.0f);
    /* A short line inside tile (0, 0) (samples 0..15 on each side), no falloff. */
    const f32 pts[6] = {(f32)OX + 2, 3, (f32)OZ + 4, (f32)OX + 8, 3, (f32)OZ + 4};
    const de_entity s = spline(f, DE_SPLINE_CATMULL_ROM, pts, 2, 0, 0, 0);
    const de_ep_edit_op op = carve_op(f->t.bits, s, 1, 0, 1);
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
    EXPECT_FALSE(mpack_node_map_contains_cstr(v, "rules"));
    EXPECT_FLOAT_EQ(metres(terrain(f), 5, 4), 2.0f, 0.01f);
    EXPECT_FLOAT_EQ(metres(terrain(f), 5, 5), 4.0f, 0.01f);
    mpack_tree_destroy(&tree);
}

TEST_F(editor_spline_carve, follows_the_spline_world_transform, carve_fx) {
    level(f, 4.0f);
    /* Local points at z = 0; the entity at z = OZ + 10, y = 2 puts the curve on row 10, 2 m up. */
    const f32 pts[6] = {(f32)OX + 0, 0, 0, (f32)OX + 32, 0, 0};
    const de_entity s = spline(f, DE_SPLINE_CATMULL_ROM, pts, 2, 0, 2, OZ + 10);
    const de_ep_edit_op op = carve_op(f->t.bits, s, 1, 0, 0.5f);
    ASSERT_EQ(edit(f, &op, NULL), 0);
    const DeTerrain *t = terrain(f);
    EXPECT_FLOAT_EQ(metres(t, 7, 10), 1.5f, 0.01f);
    EXPECT_FLOAT_EQ(metres(t, 7, 11), 4.0f, 0.01f);
    EXPECT_FLOAT_EQ(metres(t, 7, 0), 4.0f, 0.01f);
}

TEST_F(editor_spline_carve, bumps_the_terrain_revision, carve_fx) {
    level(f, 4.0f);
    const de_entity s = cross_line(f);
    const u64 rev = de__ed_entity_rev(&f->ed, f->t);
    (void)ecs_change_tick_advance(f->engine.world); /* a frame between the edits */
    const de_ep_edit_op op = carve_op(f->t.bits, s, 2, 2, 1);
    ASSERT_EQ(edit(f, &op, NULL), 0);
    EXPECT_TRUE(de__ed_entity_rev(&f->ed, f->t) > rev);
}

TEST_F(editor_spline_carve, bad_values_are_invalid_and_missing_entities_not_found, carve_fx) {
    level(f, 4.0f);
    static DeTerrain before;
    before = *terrain(f);
    const de_entity s = cross_line(f);
    de_ep_edit_op op = carve_op(f->t.bits, s, 0, 1, 0); /* width must be > 0 */
    EXPECT_EQ(edit(f, &op, NULL), DE_EP_ERR_INVALID_PARAMS);
    op = carve_op(f->t.bits, s, 2, -1, 0); /* falloff must be >= 0 */
    EXPECT_EQ(edit(f, &op, NULL), DE_EP_ERR_INVALID_PARAMS);
    op = carve_op(f->t.bits, s, 2, 1, 0);
    op.has_value = false;
    EXPECT_EQ(edit(f, &op, NULL), DE_EP_ERR_INVALID_PARAMS);
    op = carve_op(f->t.bits, s, 2, 1, 0);
    op.has_entity = false; /* entity 0: every entity op answers not_found */
    EXPECT_EQ(edit(f, &op, NULL), DE_EP_ERR_NOT_FOUND);
    const f32 one[3] = {(f32)OX + 4, 3, (f32)OZ + 4};
    const de_entity dot = spline(f, DE_SPLINE_CATMULL_ROM, one, 1, 0, 0, 0);
    op = carve_op(f->t.bits, dot, 2, 1, 0); /* a curve needs two points */
    EXPECT_EQ(edit(f, &op, NULL), DE_EP_ERR_INVALID_PARAMS);
    const de_entity plain = de_entity_spawn(f->engine.world);
    op = carve_op(f->t.bits, plain, 2, 1, 0); /* no DeSpline */
    EXPECT_EQ(edit(f, &op, NULL), DE_EP_ERR_NOT_FOUND);
    op = carve_op(plain.bits, s, 2, 1, 0); /* no DeTerrain */
    EXPECT_EQ(edit(f, &op, NULL), DE_EP_ERR_NOT_FOUND);
    EXPECT_EQ(memcmp(terrain(f), &before, sizeof before), 0);
}

#else
TEST(editor_spline_carve, shipping_has_no_editor) {
    SKIP_TEST("no editor in shipping builds");
}
#endif
