/* DENG-775: spline.info, spline.sample and spline.instances (DIDE protocol 1.12, world.splines),
 * through the real handlers and the generated protocol readers and writers. */
#include "de_test.h"
#if !defined(DE_SHIPPING)
#include "host/editor/editor_internal.h"
#include "host/host_internal.h"
#include "world/spline/spline.h"
#include <dengine/de_ecs.h>
#include <dengine/de_reflect.h>
#include <math.h>
#include <stdlib.h>

#define STORAGE  (2u << 20)
#define RESPONSE (512u << 10)

typedef struct spline_fx {
    de_engine engine;
    de__editor editor;
    unsigned char *storage;
    char *response;
    char params[256];
    mpack_tree_t tree;
    bool parsed;
} spline_fx;

static void spline_fx_setup(spline_fx *f) {
    memset(f, 0, sizeof *f);
    ASSERT_EQ(de_type_registry_create(NULL, &f->engine.types), DE_OK);
    ASSERT_EQ(de_world_create(&(de_world_desc){.types = f->engine.types}, &f->engine.world), DE_OK);
    f->editor.e = &f->engine;
    f->storage = (unsigned char *)malloc(STORAGE);
    f->response = (char *)malloc(RESPONSE);
    ASSERT_TRUE(f->storage != NULL && f->response != NULL);
    de__ed_world_register(&f->editor);
    de__ed_spline_register(&f->editor);
}
static void spline_fx_teardown(spline_fx *f) {
    if (f->parsed)
        mpack_tree_destroy(&f->tree);
    for (u32 i = 0; i < f->editor.reset_count; ++i)
        f->editor.resets[i](&f->editor);
    de_world_destroy(f->engine.world);
    de_type_registry_destroy(f->engine.types);
    free(f->storage);
    free(f->response);
}

/* Calls `m` with params {entity, step} (step < 0: spline_params) and returns the parsed reply. */
static mpack_node_t call(spline_fx *f, de_ep_method m, u64 entity, float step) {
    mpack_writer_t w;
    mpack_writer_init(&w, f->params, sizeof f->params);
    if (m == DE_EP_METHOD_SPLINE_SAMPLE)
        de_ep_write_spline_sample_params(&w, &(de_ep_spline_sample_params){entity, step});
    else
        de_ep_write_spline_params(&w, &(de_ep_spline_params){entity});
    const size_t psize = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    const de_ep_request req = {
        .id = 7, .method = de_ep_cstr("spline"), .params = {f->params, psize}};
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
    ASSERT_FALSE(mpack_node_map_contains_cstr(root, "error"));
    return mpack_node_map_cstr(root, "result");
}
static void expect_error(mpack_node_t root, de_ep_error code) {
    EXPECT_FALSE(mpack_node_map_contains_cstr(root, "result"));
    mpack_node_t error = mpack_node_map_cstr(root, "error");
    EXPECT_EQ(mpack_node_i32(mpack_node_map_cstr(error, "code")), (i32)code);
}
static float at(mpack_node_t list, u32 i) {
    return mpack_node_float(mpack_node_array_at(list, i));
}

static de_spline_ids ids(spline_fx *f) {
    de_spline_ids out;
    ASSERT_EQ(de__spline_register(f->engine.world, &out), DE_OK);
    return out;
}
/* A spline entity through `n` points, at the identity unless `t` is given. */
static de_entity spawn(spline_fx *f, const de_vec3 *p, u32 n, bool closed, const de_transform *t) {
    const de_entity e = de_entity_spawn(f->engine.world);
    DeSpline *s = (DeSpline *)de_entity_add(f->engine.world, e, ids(f).spline);
    ASSERT_TRUE(s != NULL);
    memset(s, 0, sizeof *s);
    s->kind = DE_SPLINE_CATMULL_ROM;
    s->closed = closed;
    s->count = n;
    for (u32 i = 0; i < n; ++i)
        s->points[i].position = p[i];
    if (t != NULL) {
        DeTransform *x = (DeTransform *)de_entity_add(
            f->engine.world, e, de_builtin_components(f->engine.world).transform);
        ASSERT_TRUE(x != NULL);
        *x = *t;
    }
    return e;
}
static de_entity line(spline_fx *f, float len, const de_transform *t) {
    const de_vec3 p[2] = {{0, 0, 0}, {len, 0, 0}};
    return spawn(f, p, 2, false, t);
}
static void meshes(spline_fx *f, de_entity e, float spacing, float length, bool deform,
                   float offset) {
    DeSplineMeshes *m = (DeSplineMeshes *)de_entity_add(f->engine.world, e, ids(f).meshes);
    ASSERT_TRUE(m != NULL);
    *m = (DeSplineMeshes){.spacing = spacing, .length = length, .deform = deform, .offset = offset};
}

TEST_F(editor_spline, advertises_world_splines, spline_fx) {
    bool found = false;
    for (u32 i = 0; i < f->editor.cap_count; ++i)
        found |= strcmp(f->editor.caps[i], "world.splines") == 0;
    EXPECT_TRUE(found);
    /* The components are registered with the service, so reflection lists them. */
    EXPECT_TRUE(DE_HANDLE_VALID(de_component_find(f->engine.world, "DeSpline")));
    EXPECT_TRUE(DE_HANDLE_VALID(de_component_find(f->engine.world, "DeSplineMeshes")));
}

/* DIDE conformance: with world.splines listed, each method on entity u64::MAX - 1 is not_found. */
TEST_F(editor_spline, unknown_entity_is_not_found, spline_fx) {
    const u64 missing = UINT64_MAX - 1;
    expect_error(call(f, DE_EP_METHOD_SPLINE_INFO, missing, -1), DE_EP_ERR_NOT_FOUND);
    expect_error(call(f, DE_EP_METHOD_SPLINE_SAMPLE, missing, 1.0f), DE_EP_ERR_NOT_FOUND);
    expect_error(call(f, DE_EP_METHOD_SPLINE_INSTANCES, missing, -1), DE_EP_ERR_NOT_FOUND);
    const de_entity plain = de_entity_spawn(f->engine.world);
    expect_error(call(f, DE_EP_METHOD_SPLINE_INFO, plain.bits, -1), DE_EP_ERR_NOT_FOUND);
    expect_error(call(f, DE_EP_METHOD_SPLINE_SAMPLE, plain.bits, 1.0f), DE_EP_ERR_NOT_FOUND);
    expect_error(call(f, DE_EP_METHOD_SPLINE_INSTANCES, plain.bits, -1), DE_EP_ERR_NOT_FOUND);
}

TEST_F(editor_spline, sample_step_below_a_centimetre_is_invalid, spline_fx) {
    const de_entity e = line(f, 4, NULL);
    expect_error(call(f, DE_EP_METHOD_SPLINE_SAMPLE, e.bits, 0.005f), DE_EP_ERR_INVALID_PARAMS);
    expect_error(call(f, DE_EP_METHOD_SPLINE_SAMPLE, e.bits, NAN), DE_EP_ERR_INVALID_PARAMS);
    expect_error(call(f, DE_EP_METHOD_SPLINE_SAMPLE, e.bits, -1.0f), DE_EP_ERR_INVALID_PARAMS);
}

TEST_F(editor_spline, info_describes_the_curve, spline_fx) {
    const de_entity e = line(f, 4, NULL);
    mpack_node_t r = result_of(call(f, DE_EP_METHOD_SPLINE_INFO, e.bits, -1));
    EXPECT_EQ(mpack_node_u64(mpack_node_map_cstr(r, "entity")), e.bits);
    char buf[64];
    mpack_node_copy_cstr(mpack_node_map_cstr(r, "component"), buf, sizeof buf);
    EXPECT_STREQ(buf, "DeSpline");
    mpack_node_copy_cstr(mpack_node_map_cstr(r, "meshComponent"), buf, sizeof buf);
    EXPECT_STREQ(buf, "DeSplineMeshes");
    mpack_node_t kinds = mpack_node_map_cstr(r, "kinds");
    ASSERT_EQ(mpack_node_array_length(kinds), 2u);
    mpack_node_copy_cstr(mpack_node_array_at(kinds, 0), buf, sizeof buf);
    EXPECT_STREQ(buf, "catmullRom");
    mpack_node_copy_cstr(mpack_node_array_at(kinds, 1), buf, sizeof buf);
    EXPECT_STREQ(buf, "bezier");
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(r, "maxPoints")), 64u);
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(r, "count")), 2u);
    EXPECT_FLOAT_EQ(mpack_node_float(mpack_node_map_cstr(r, "length")), 4.0f, 1e-3f);
    const u64 rev = mpack_node_u64(mpack_node_map_cstr(r, "revision"));
    EXPECT_GT(rev, 0u);
    EXPECT_EQ(rev, de__ed_entity_rev(&f->editor, e));
}

TEST_F(editor_spline, sample_is_even_by_arc_length_and_ends_at_the_end, spline_fx) {
    const de_entity e = line(f, 4, NULL);
    mpack_node_t r = result_of(call(f, DE_EP_METHOD_SPLINE_SAMPLE, e.bits, 1.0f));
    EXPECT_FLOAT_EQ(mpack_node_float(mpack_node_map_cstr(r, "length")), 4.0f, 1e-3f);
    mpack_node_t pos = mpack_node_map_cstr(r, "positions"),
                 tan = mpack_node_map_cstr(r, "tangents"),
                 dist = mpack_node_map_cstr(r, "distances");
    ASSERT_EQ(mpack_node_array_length(dist), 5u);
    ASSERT_EQ(mpack_node_array_length(pos), 15u);
    ASSERT_EQ(mpack_node_array_length(tan), 15u);
    for (u32 i = 0; i < 5; ++i) {
        EXPECT_FLOAT_EQ(at(dist, i), (float)i, 1e-3f);
        EXPECT_FLOAT_EQ(at(pos, 3 * i), (float)i, 1e-3f);
        EXPECT_FLOAT_EQ(at(pos, 3 * i + 1), 0.0f, 1e-4f);
        EXPECT_FLOAT_EQ(at(tan, 3 * i), 1.0f, 1e-4f);
    }
    EXPECT_EQ(mpack_node_u64(mpack_node_map_cstr(r, "revision")), de__ed_entity_rev(&f->editor, e));
}

TEST_F(editor_spline, closed_sample_returns_to_the_start, spline_fx) {
    const de_vec3 sq[4] = {{0, 0, 0}, {4, 0, 0}, {4, 4, 0}, {0, 4, 0}};
    const de_entity e = spawn(f, sq, 4, true, NULL);
    mpack_node_t r = result_of(call(f, DE_EP_METHOD_SPLINE_SAMPLE, e.bits, 0.5f));
    mpack_node_t pos = mpack_node_map_cstr(r, "positions"),
                 dist = mpack_node_map_cstr(r, "distances");
    const u32 n = (u32)mpack_node_array_length(dist);
    ASSERT_GT(n, 2u);
    for (u32 k = 0; k < 3; ++k)
        EXPECT_FLOAT_EQ(at(pos, 3 * (n - 1) + k), at(pos, k), 1e-3f);
    EXPECT_FLOAT_EQ(at(dist, n - 1), mpack_node_float(mpack_node_map_cstr(r, "length")), 1e-3f);
    /* A closed uniform Catmull-Rom through a 4 m square passes the corners and bulges out a little.
     */
    EXPECT_TRUE(at(dist, n - 1) > 12.0f);
    EXPECT_TRUE(at(dist, n - 1) < 20.0f);
}

TEST_F(editor_spline, sample_count_is_capped, spline_fx) {
    const de_entity e = line(f, 100, NULL);
    mpack_node_t r = result_of(call(f, DE_EP_METHOD_SPLINE_SAMPLE, e.bits, 0.01f));
    mpack_node_t dist = mpack_node_map_cstr(r, "distances");
    const u32 n = (u32)mpack_node_array_length(dist);
    EXPECT_LE(n, 4096u);
    EXPECT_GT(n, 4000u);
    EXPECT_FLOAT_EQ(at(dist, n - 1), 100.0f, 1e-2f);
}

TEST_F(editor_spline, instances_without_meshes_are_empty, spline_fx) {
    const de_entity e = line(f, 4, NULL);
    mpack_node_t r = result_of(call(f, DE_EP_METHOD_SPLINE_INSTANCES, e.bits, -1));
    EXPECT_EQ(mpack_node_strlen(mpack_node_map_cstr(r, "mesh")), 0u);
    EXPECT_FALSE(mpack_node_bool(mpack_node_map_cstr(r, "deform")));
    EXPECT_EQ(mpack_node_array_length(mpack_node_map_cstr(r, "transforms")), 0u);
}

TEST_F(editor_spline, instances_every_spacing_from_offset, spline_fx) {
    const de_entity e = line(f, 4, NULL);
    meshes(f, e, 1.0f, 2.0f, false, 0.5f);
    mpack_node_t r = result_of(call(f, DE_EP_METHOD_SPLINE_INSTANCES, e.bits, -1));
    mpack_node_t xf = mpack_node_map_cstr(r, "transforms");
    ASSERT_EQ(mpack_node_array_length(xf), 40u); /* 0.5, 1.5, 2.5, 3.5 */
    for (u32 i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(at(xf, 10 * i), 0.5f + (float)i, 1e-3f);
        EXPECT_FLOAT_EQ(at(xf, 10 * i + 6), 1.0f, 1e-4f); /* +X along +X: identity rotation */
        EXPECT_FLOAT_EQ(at(xf, 10 * i + 7), 1.0f, 1e-4f);
    }
}

TEST_F(editor_spline, deform_fills_the_curve_with_stretched_pieces, spline_fx) {
    const de_entity e = line(f, 4, NULL);
    meshes(f, e, 1.0f, 1.5f, true, 0.0f);
    mpack_node_t r = result_of(call(f, DE_EP_METHOD_SPLINE_INSTANCES, e.bits, -1));
    EXPECT_TRUE(mpack_node_bool(mpack_node_map_cstr(r, "deform")));
    mpack_node_t xf = mpack_node_map_cstr(r, "transforms");
    ASSERT_EQ(mpack_node_array_length(xf), 30u); /* round(4 / 1.5) = 3 pieces of 4/3 m */
    const float piece = 4.0f / 3.0f;
    for (u32 i = 0; i < 3; ++i) {
        EXPECT_FLOAT_EQ(at(xf, 10 * i), ((float)i + 0.5f) * piece, 1e-3f);
        EXPECT_FLOAT_EQ(at(xf, 10 * i + 7), piece / 1.5f, 1e-4f);
        EXPECT_FLOAT_EQ(at(xf, 10 * i + 8), 1.0f, 1e-6f);
    }
}

TEST_F(editor_spline, replies_are_in_world_space, spline_fx) {
    de_transform t = de_transform_identity();
    t.position = (de_dvec3){10, 0, 0};
    t.rotation = de_quat_axis_angle((de_vec3){0, 0, 1}, DE_PI / 2);
    const de_entity e = line(f, 4, &t);
    meshes(f, e, 2.0f, 2.0f, false, 0.0f);
    const de_vec3 dir = de_quat_rotate(t.rotation, (de_vec3){1, 0, 0});
    mpack_node_t r = result_of(call(f, DE_EP_METHOD_SPLINE_INSTANCES, e.bits, -1));
    mpack_node_t xf = mpack_node_map_cstr(r, "transforms");
    ASSERT_EQ(mpack_node_array_length(xf), 30u); /* 0, 2, 4 */
    for (u32 i = 0; i < 3; ++i) {
        EXPECT_FLOAT_EQ(at(xf, 10 * i), 10.0f + dir.x * 2.0f * (float)i, 1e-3f);
        EXPECT_FLOAT_EQ(at(xf, 10 * i + 1), dir.y * 2.0f * (float)i, 1e-3f);
        const de_quat q = {at(xf, 10 * i + 3), at(xf, 10 * i + 4), at(xf, 10 * i + 5),
                           at(xf, 10 * i + 6)};
        const de_vec3 x = de_quat_rotate(q, (de_vec3){1, 0, 0});
        EXPECT_FLOAT_EQ(x.x, dir.x, 1e-4f);
        EXPECT_FLOAT_EQ(x.y, dir.y, 1e-4f);
        EXPECT_FLOAT_EQ(x.z, dir.z, 1e-4f);
    }
    r = result_of(call(f, DE_EP_METHOD_SPLINE_INFO, e.bits, -1));
    EXPECT_FLOAT_EQ(mpack_node_float(mpack_node_map_cstr(r, "length")), 4.0f, 1e-3f);
}
#endif
