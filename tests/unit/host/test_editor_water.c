/* DENG-776: water.info, water.surface, water.flowMap, water.probes and water.showProbes (DIDE
 * protocol 1.13, world.water), through the real handlers and the generated protocol readers and
 * writers. The expected numbers are the DIDE mock engine's (mock-engine/src/water.rs). */
#include "de_test.h"
#if !defined(DE_SHIPPING)
#include "host/editor/editor_internal.h"
#include "host/host_internal.h"
#include "world/spline/spline.h"
#include "world/water/water.h"
#include <dengine/de_ecs.h>
#include <dengine/de_reflect.h>
#include <dengine/de_render.h>
#include <math.h>
#include <stdlib.h>

#define STORAGE  (2u << 20)
#define RESPONSE (512u << 10)

typedef struct water_fx {
    de_engine engine;
    de__editor editor;
    unsigned char *storage;
    char *response;
    char params[4096];
    mpack_writer_t pw;
    mpack_tree_t tree;
    bool parsed;
} water_fx;

/* A system in FIXED_UPDATE, so each run takes a change tick and a mutable get marks a newer one. */
static void tick_noop(de_system_ctx *ctx) {
    (void)ctx;
}

static void water_fx_setup(water_fx *f) {
    memset(f, 0, sizeof *f);
    ASSERT_EQ(de_type_registry_create(NULL, &f->engine.types), DE_OK);
    ASSERT_EQ(de_world_create(&(de_world_desc){.types = f->engine.types}, &f->engine.world), DE_OK);
    f->editor.e = &f->engine;
    f->storage = (unsigned char *)malloc(STORAGE);
    f->response = (char *)malloc(RESPONSE);
    ASSERT_TRUE(f->storage != NULL && f->response != NULL);
    ASSERT_TRUE(DE_HANDLE_VALID(de_system_add(
        f->engine.world, &(de_system_desc){.name = "water_test_tick",
                                           .schedule = DE_SCHEDULE_FIXED_UPDATE,
                                           .fn = tick_noop,
                                           .flags = DE_SYSTEM_EXCLUSIVE})));
    de__ed_world_register(&f->editor);
    de__ed_water_register(&f->editor);
}
static void water_fx_teardown(water_fx *f) {
    if (f->parsed)
        mpack_tree_destroy(&f->tree);
    for (u32 i = 0; i < f->editor.reset_count; ++i)
        f->editor.resets[i](&f->editor);
    de_world_destroy(f->engine.world);
    de_type_registry_destroy(f->engine.types);
    free(f->storage);
    free(f->response);
}

/* Params: start writing with params(f), write them, then invoke(f, method). */
static mpack_writer_t *params(water_fx *f) {
    mpack_writer_init(&f->pw, f->params, sizeof f->params);
    return &f->pw;
}
static mpack_node_t invoke(water_fx *f, de_ep_method m) {
    const size_t psize = mpack_writer_buffer_used(&f->pw);
    ASSERT_EQ(mpack_writer_destroy(&f->pw), mpack_ok);
    const de_ep_request req = {
        .id = 7, .method = de_ep_cstr("water"), .params = {f->params, psize}};
    ASSERT_TRUE(f->editor.handlers[m] != NULL);
    de_ep_arena a = {f->storage, STORAGE, 0};
    mpack_writer_t w;
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
static mpack_node_t info(water_fx *f, u64 e) {
    de_ep_write_water_params(params(f), &(de_ep_water_params){e});
    return invoke(f, DE_EP_METHOD_WATER_INFO);
}
static mpack_node_t surface(water_fx *f, u64 e, float x, float z, float size, u32 res) {
    de_ep_write_water_surface_params(params(f), &(de_ep_water_surface_params){e, x, z, size, res});
    return invoke(f, DE_EP_METHOD_WATER_SURFACE);
}
static mpack_node_t flow_map(water_fx *f, u64 e, u32 res) {
    de_ep_write_water_flow_params(params(f), &(de_ep_water_flow_params){e, res});
    return invoke(f, DE_EP_METHOD_WATER_FLOW_MAP);
}
static mpack_node_t probes(water_fx *f, const u64 *ents, u32 n, bool has) {
    de_ep_water_probes_params p = {has, {(uint64_t *)ents, n}};
    de_ep_write_water_probes_params(params(f), &p);
    return invoke(f, DE_EP_METHOD_WATER_PROBES);
}
static mpack_node_t show(water_fx *f, const u64 *ents, u32 n) {
    de_ep_write_water_show_probes_params(params(f),
                                         &(de_ep_water_show_probes_params){{(uint64_t *)ents, n}});
    return invoke(f, DE_EP_METHOD_WATER_SHOW_PROBES);
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
static float num(mpack_node_t map, const char *key) {
    return mpack_node_float(mpack_node_map_cstr(map, key));
}

static de_component water_comp(water_fx *f) {
    const de_component c = de_component_find(f->engine.world, "DeWater");
    ASSERT_TRUE(DE_HANDLE_VALID(c));
    return c;
}
static void place(water_fx *f, de_entity e, double x, double y, double z) {
    DeTransform *t = (DeTransform *)de_entity_add(
        f->engine.world, e, de_builtin_components(f->engine.world).transform);
    ASSERT_TRUE(t != NULL);
    *t = de_transform_identity();
    t->position = (de_dvec3){x, y, z};
}
/* A water body of `kind` at (x, y, z): the mock's defaults, surface_height 0. */
static de_entity body(water_fx *f, de_water_kind kind, double x, double y, double z) {
    const de_entity e = de_entity_spawn(f->engine.world);
    DeWater *w = (DeWater *)de_entity_add(f->engine.world, e, water_comp(f));
    ASSERT_TRUE(w != NULL);
    memset(w, 0, sizeof *w);
    w->kind = kind;
    w->spectrum = DE_WAVE_PIERSON_MOSKOWITZ;
    w->wind = (de_vec2){5, 0};
    w->choppiness = 1;
    w->extent = (de_vec3){64, 8, 64};
    w->width = 4;
    w->flow_speed = 1;
    place(f, e, x, y, z);
    return e;
}
static DeWater *water(water_fx *f, de_entity e) {
    DeWater *w = (DeWater *)de_entity_get(f->engine.world, e, water_comp(f));
    ASSERT_TRUE(w != NULL);
    return w;
}
/* A straight spline at height y from (x0, y, 0) to (x1, y, 0). */
static de_entity line(water_fx *f, float x0, float x1, float y) {
    de_spline_ids ids;
    ASSERT_EQ(de__spline_register(f->engine.world, &ids), DE_OK);
    const de_entity e = de_entity_spawn(f->engine.world);
    DeSpline *s = (DeSpline *)de_entity_add(f->engine.world, e, ids.spline);
    ASSERT_TRUE(s != NULL);
    memset(s, 0, sizeof *s);
    s->kind = DE_SPLINE_CATMULL_ROM;
    s->count = 2;
    s->points[0].position = (de_vec3){x0, y, 0};
    s->points[1].position = (de_vec3){x1, y, 0};
    return e;
}
/* A buoyant body with the default probes (the bottom corners of a 1 m box, radius 0.25). */
static de_entity floater(water_fx *f, double x, double y, double z) {
    de_component c;
    ASSERT_EQ(de__water_register(f->engine.world, &c), DE_OK);
    const de_entity e = de_entity_spawn(f->engine.world);
    ASSERT_TRUE(de_entity_add(f->engine.world, e, c) != NULL);
    place(f, e, x, y, z);
    return e;
}
static void advance(water_fx *f, u32 ticks) {
    for (u32 i = 0; i < ticks; ++i)
        ASSERT_EQ(de_world_run_schedule(f->engine.world, DE_SCHEDULE_FIXED_UPDATE, 1.0f / 60, 0),
                  DE_OK);
}

/* The mock engine's numbers, in double. */
static double ref_hs(double wind, double k) {
    return k * 0.21 * wind * wind / 9.81;
}
static double ref_wave(double wx, double wz, double chop, double x, double z, double t) {
    const double hs = ref_hs(hypot(wx, wz), 1.0), speed = fmax(hypot(wx, wz), 0.5);
    const double dir = atan2(wz, wx), kp = 9.81 / ((1.14 * speed) * (1.14 * speed));
    const double waves[4][3] = {
        {1.0, 0.6, 0}, {1.5, 0.25, 0.3}, {2.2, 0.1, -0.4}, {3.1, 0.05, 0.6}};
    double h = 0;
    for (int i = 0; i < 4; ++i) {
        const double k = kp * waves[i][0], a = 0.5 * hs * waves[i][1];
        const double th = k * (cos(dir + waves[i][2]) * x + sin(dir + waves[i][2]) * z) -
                          sqrt(9.81 * k) * t;
        h += a * (cos(th) + 0.5 * chop * k * a * cos(2 * th));
    }
    return h;
}

TEST_F(editor_water, advertises_world_water_and_reflects_the_components, water_fx) {
    bool found = false;
    for (u32 i = 0; i < f->editor.cap_count; ++i)
        found |= strcmp(f->editor.caps[i], "world.water") == 0;
    EXPECT_TRUE(found);
    /* Registered with the service, so the IDE's reflection lists them from the start. */
    EXPECT_TRUE(DE_HANDLE_VALID(de_component_find(f->engine.world, "DeWater")));
    EXPECT_TRUE(DE_HANDLE_VALID(de_component_find(f->engine.world, "DeBuoyancy")));
    const de_type_registry *reg = de_world_types(f->engine.world);
    const de_type_desc *w = de_type_get(reg, de_type_find(reg, "DeWater"));
    ASSERT_TRUE(w != NULL);
    EXPECT_EQ(w->version, 2u);
    const char *want[] = {"kind",       "material",   "spectrum",  "wind",
                          "choppiness", "surface_height", "extent", "spline",
                          "width",      "flow_speed", "fog_color", "fog_density"};
    ASSERT_EQ(w->field_count, 12u);
    for (u32 i = 0; i < 12; ++i)
        EXPECT_STREQ(w->fields[i].name, want[i]);
    const de_type_desc *b = de_type_get(reg, de_type_find(reg, "DeBuoyancy"));
    ASSERT_TRUE(b != NULL);
    ASSERT_EQ(b->field_count, 3u);
    EXPECT_STREQ(b->fields[2].name, "radius");
    EXPECT_EQ(b->fields[1].attrs.array_count, 8u);
}

/* DIDE conformance: with world.water listed, each method on entity u64::MAX - 1 is not_found. */
TEST_F(editor_water, unknown_entity_is_not_found, water_fx) {
    const u64 missing = UINT64_MAX - 1;
    expect_error(info(f, missing), DE_EP_ERR_NOT_FOUND);
    expect_error(surface(f, missing, 0, 0, 10, 4), DE_EP_ERR_NOT_FOUND);
    expect_error(flow_map(f, missing, 8), DE_EP_ERR_NOT_FOUND);
    expect_error(probes(f, &missing, 1, true), DE_EP_ERR_NOT_FOUND);
    expect_error(show(f, &missing, 1), DE_EP_ERR_NOT_FOUND);
    /* An entity without the water component is not a body either. */
    const de_entity plain = de_entity_spawn(f->engine.world);
    expect_error(info(f, plain.bits), DE_EP_ERR_NOT_FOUND);
    expect_error(surface(f, plain.bits, 0, 0, 10, 4), DE_EP_ERR_NOT_FOUND);
    expect_error(flow_map(f, plain.bits, 8), DE_EP_ERR_NOT_FOUND);
    /* water.probes skips a live entity without the buoyancy component. */
    mpack_node_t r = result_of(probes(f, &plain.bits, 1, true));
    EXPECT_EQ(mpack_node_array_length(mpack_node_map_cstr(r, "entities")), 0u);
}

TEST_F(editor_water, info_reports_the_body, water_fx) {
    const de_entity e = body(f, DE_WATER_OCEAN, 0, 0, 0);
    mpack_node_t r = result_of(info(f, e.bits));
    char buf[64];
    EXPECT_EQ(mpack_node_u64(mpack_node_map_cstr(r, "entity")), e.bits);
    mpack_node_copy_cstr(mpack_node_map_cstr(r, "component"), buf, sizeof buf);
    EXPECT_STREQ(buf, "DeWater");
    mpack_node_copy_cstr(mpack_node_map_cstr(r, "buoyancyComponent"), buf, sizeof buf);
    EXPECT_STREQ(buf, "DeBuoyancy");
    mpack_node_copy_cstr(mpack_node_map_cstr(r, "kind"), buf, sizeof buf);
    EXPECT_STREQ(buf, "ocean");
    const char *kinds[] = {"ocean", "river", "lake"};
    const char *spectra[] = {"phillips", "jonswap", "pierson_moskowitz"};
    mpack_node_t k = mpack_node_map_cstr(r, "kinds"), s = mpack_node_map_cstr(r, "spectra");
    ASSERT_EQ(mpack_node_array_length(k), 3u);
    ASSERT_EQ(mpack_node_array_length(s), 3u);
    for (u32 i = 0; i < 3; ++i) {
        mpack_node_copy_cstr(mpack_node_array_at(k, i), buf, sizeof buf);
        EXPECT_STREQ(buf, kinds[i]);
        mpack_node_copy_cstr(mpack_node_array_at(s, i), buf, sizeof buf);
        EXPECT_STREQ(buf, spectra[i]);
    }
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(r, "maxProbes")), 8u);
    const u64 rev = mpack_node_u64(mpack_node_map_cstr(r, "revision"));
    EXPECT_GT(rev, 0u);
    EXPECT_EQ(rev, de__ed_entity_rev(&f->editor, e));
}

/* AC: the wave height follows the wind and the spectrum in the next reply after an edit. */
TEST_F(editor_water, wave_height_rises_with_the_wind, water_fx) {
    const de_entity e = body(f, DE_WATER_OCEAN, 0, 0, 0);
    const float h5 = num(result_of(info(f, e.bits)), "waveHeight");
    EXPECT_FLOAT_EQ(h5, (float)ref_hs(5, 1.0), 1e-4f);
    water(f, e)->wind = (de_vec2){0, 10};
    const float h10 = num(result_of(info(f, e.bits)), "waveHeight");
    EXPECT_FLOAT_EQ(h10, (float)ref_hs(10, 1.0), 1e-4f);
    EXPECT_TRUE(h10 > h5);
    water(f, e)->spectrum = DE_WAVE_JONSWAP;
    EXPECT_FLOAT_EQ(num(result_of(info(f, e.bits)), "waveHeight"), (float)ref_hs(10, 1.3), 1e-4f);
    water(f, e)->spectrum = DE_WAVE_PHILLIPS;
    EXPECT_FLOAT_EQ(num(result_of(info(f, e.bits)), "waveHeight"), (float)ref_hs(10, 0.9), 1e-4f);
    /* water.surface reports the same height. */
    EXPECT_FLOAT_EQ(num(result_of(surface(f, e.bits, 0, 0, 10, 2)), "waveHeight"),
                    (float)ref_hs(10, 0.9), 1e-4f);
    /* Rivers and lakes have no waves. */
    water(f, e)->kind = DE_WATER_LAKE;
    EXPECT_FLOAT_EQ(num(result_of(info(f, e.bits)), "waveHeight"), 0.0f, 0.0f);
}

TEST_F(editor_water, surface_params_are_checked, water_fx) {
    const de_entity e = body(f, DE_WATER_OCEAN, 0, 0, 0);
    expect_error(surface(f, e.bits, 0, 0, 0, 4), DE_EP_ERR_INVALID_PARAMS);
    expect_error(surface(f, e.bits, 0, 0, -1, 4), DE_EP_ERR_INVALID_PARAMS);
    expect_error(surface(f, e.bits, NAN, 0, 10, 4), DE_EP_ERR_INVALID_PARAMS);
    expect_error(surface(f, e.bits, 0, INFINITY, 10, 4), DE_EP_ERR_INVALID_PARAMS);
    expect_error(surface(f, e.bits, 0, 0, 10, 1), DE_EP_ERR_INVALID_PARAMS);
    expect_error(surface(f, e.bits, 0, 0, 10, 65), DE_EP_ERR_INVALID_PARAMS);
    mpack_node_t r = result_of(surface(f, e.bits, 0, 0, 10, 64));
    EXPECT_EQ(mpack_node_array_length(mpack_node_map_cstr(r, "heights")), 64u * 64u);
}

/* AC: the ocean surface is the mock's waves, and it moves with game time. */
TEST_F(editor_water, ocean_surface_moves_with_game_time, water_fx) {
    const de_entity e = body(f, DE_WATER_OCEAN, 0, 1, 0);
    water(f, e)->surface_height = 0.5f;
    mpack_node_t r = result_of(surface(f, e.bits, 3, -2, 20, 5));
    EXPECT_FLOAT_EQ(num(r, "time"), 0.0f, 0.0f);
    mpack_node_t h = mpack_node_map_cstr(r, "heights");
    ASSERT_EQ(mpack_node_array_length(h), 25u);
    float before[25];
    for (u32 j = 0; j < 5; ++j)
        for (u32 i = 0; i < 5; ++i) {
            const double x = 3 - 10 + 5.0 * i, z = -2 - 10 + 5.0 * j;
            before[j * 5 + i] = at(h, j * 5 + i);
            EXPECT_FLOAT_EQ(before[j * 5 + i], (float)(1.5 + ref_wave(5, 0, 1, x, z, 0)), 1e-3f);
        }
    advance(f, 30);
    r = result_of(surface(f, e.bits, 3, -2, 20, 5));
    EXPECT_FLOAT_EQ(num(r, "time"), 0.5f, 1e-6f);
    h = mpack_node_map_cstr(r, "heights");
    bool moved = false;
    for (u32 j = 0; j < 5; ++j)
        for (u32 i = 0; i < 5; ++i) {
            const double x = 3 - 10 + 5.0 * i, z = -2 - 10 + 5.0 * j;
            EXPECT_FLOAT_EQ(at(h, j * 5 + i), (float)(1.5 + ref_wave(5, 0, 1, x, z, 0.5)), 1e-3f);
            moved |= fabsf(at(h, j * 5 + i) - before[j * 5 + i]) > 1e-3f;
        }
    EXPECT_TRUE(moved);
}

TEST_F(editor_water, lakes_and_rivers_are_flat_at_their_level, water_fx) {
    const de_entity lake = body(f, DE_WATER_LAKE, 0, 2, 0);
    water(f, lake)->surface_height = 0.5f;
    mpack_node_t h = mpack_node_map_cstr(result_of(surface(f, lake.bits, 0, 0, 4, 3)), "heights");
    for (u32 i = 0; i < 9; ++i)
        EXPECT_FLOAT_EQ(at(h, i), 2.5f, 1e-5f);
    /* A river: the curve's height + surface_height. */
    const de_entity sp = line(f, 0, 10, 1);
    const de_entity river = body(f, DE_WATER_RIVER, 0, 0, 0);
    water(f, river)->spline = sp;
    water(f, river)->surface_height = 0.3f;
    h = mpack_node_map_cstr(result_of(surface(f, river.bits, 5, 0, 1, 2)), "heights");
    for (u32 i = 0; i < 4; ++i)
        EXPECT_FLOAT_EQ(at(h, i), 1.3f, 1e-4f);
}

/* AC: a river's flow map follows its spline, fastest at the centre, and an edit to the spline
 * shows in the next reply with a newer revision. */
TEST_F(editor_water, river_flow_map_follows_the_spline, water_fx) {
    const de_entity sp = line(f, 0, 10, 0);
    const de_entity river = body(f, DE_WATER_RIVER, 0, 0, 0);
    water(f, river)->spline = sp;
    water(f, river)->width = 4;
    water(f, river)->flow_speed = 2;
    mpack_node_t r = result_of(flow_map(f, river.bits, 14));
    /* Bounds: the curve +- width / 2 -> origin (-2, -2), size 14 x 4; a texel is 1 m. */
    mpack_node_t o = mpack_node_map_cstr(r, "origin"), s = mpack_node_map_cstr(r, "size");
    EXPECT_FLOAT_EQ(at(o, 0), -2.0f, 1e-4f);
    EXPECT_FLOAT_EQ(at(o, 1), -2.0f, 1e-4f);
    EXPECT_FLOAT_EQ(at(s, 0), 14.0f, 1e-4f);
    EXPECT_FLOAT_EQ(at(s, 1), 4.0f, 1e-4f);
    ASSERT_EQ(mpack_node_u32(mpack_node_map_cstr(r, "width")), 14u);
    ASSERT_EQ(mpack_node_u32(mpack_node_map_cstr(r, "height")), 4u);
    mpack_node_t flow = mpack_node_map_cstr(r, "flow");
    ASSERT_EQ(mpack_node_array_length(flow), 2u * 14u * 4u);
    /* Texel (5, 1) is at (3.5, -0.5): 0.5 m off the centre line, speed 2 (1 - 0.25^2). */
    EXPECT_FLOAT_EQ(at(flow, 2 * (1 * 14 + 5)), 2.0f * (1.0f - 0.0625f), 1e-4f);
    EXPECT_FLOAT_EQ(at(flow, 2 * (1 * 14 + 5) + 1), 0.0f, 1e-5f);
    /* Texel (5, 0) at (3.5, -1.5): slower, along +x. */
    EXPECT_FLOAT_EQ(at(flow, 2 * 5), 2.0f * (1.0f - 0.5625f), 1e-4f);
    /* Texel (0, 0) at (-1.5, -1.5): beyond the curve's end, 2.1 m away, outside the river. */
    EXPECT_FLOAT_EQ(at(flow, 0), 0.0f, 0.0f);
    EXPECT_FLOAT_EQ(num(r, "maxSpeed"), 2.0f * (1.0f - 0.0625f), 1e-4f);
    const u64 rev = mpack_node_u64(mpack_node_map_cstr(r, "revision"));
    EXPECT_EQ(rev, de__ed_entity_rev(&f->editor, river) > de__ed_entity_rev(&f->editor, sp)
                       ? de__ed_entity_rev(&f->editor, river)
                       : de__ed_entity_rev(&f->editor, sp));

    /* Turn the spline along +z: the flow turns with it, and the revision moves on. */
    advance(f, 1);
    de_spline_ids ids;
    ASSERT_EQ(de__spline_register(f->engine.world, &ids), DE_OK);
    DeSpline *spl = (DeSpline *)de_entity_get(f->engine.world, sp, ids.spline);
    ASSERT_TRUE(spl != NULL);
    spl->points[1].position = (de_vec3){0, 0, 10};
    r = result_of(flow_map(f, river.bits, 14));
    EXPECT_GT(mpack_node_u64(mpack_node_map_cstr(r, "revision")), rev);
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(r, "width")), 4u);
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(r, "height")), 14u);
    flow = mpack_node_map_cstr(r, "flow");
    /* Texel (1, 5) at (-0.5, 3.5): along +z. */
    EXPECT_FLOAT_EQ(at(flow, 2 * (5 * 4 + 1)), 0.0f, 1e-5f);
    EXPECT_FLOAT_EQ(at(flow, 2 * (5 * 4 + 1) + 1), 2.0f * (1.0f - 0.0625f), 1e-4f);
}

TEST_F(editor_water, flow_map_is_for_rivers_with_a_spline, water_fx) {
    const de_entity lake = body(f, DE_WATER_LAKE, 0, 0, 0);
    expect_error(flow_map(f, lake.bits, 8), DE_EP_ERR_INVALID_PARAMS);
    const de_entity river = body(f, DE_WATER_RIVER, 0, 0, 0);
    expect_error(flow_map(f, river.bits, 8), DE_EP_ERR_INVALID_PARAMS); /* no spline */
    water(f, river)->spline = line(f, 0, 10, 0);
    expect_error(flow_map(f, river.bits, 1), DE_EP_ERR_INVALID_PARAMS);
    expect_error(flow_map(f, river.bits, 129), DE_EP_ERR_INVALID_PARAMS);
    result_of(flow_map(f, river.bits, 128));
    /* A spline entity that is not a spline. */
    water(f, river)->spline = de_entity_spawn(f->engine.world);
    expect_error(flow_map(f, river.bits, 8), DE_EP_ERR_INVALID_PARAMS);
}

/* AC: a body in a lake floats: its probes are wet, at their depth, with the force rho g V s. */
TEST_F(editor_water, probes_float_in_a_lake, water_fx) {
    body(f, DE_WATER_LAKE, 0, 0, 0);
    const de_entity in = floater(f, 1, 0, 1), out = floater(f, 100, 0, 0);
    const u64 ask[2] = {in.bits, out.bits};
    mpack_node_t r = result_of(probes(f, ask, 2, true));
    mpack_node_t ents = mpack_node_map_cstr(r, "entities");
    mpack_node_t pos = mpack_node_map_cstr(r, "positions"), wet = mpack_node_map_cstr(r, "wet");
    mpack_node_t depth = mpack_node_map_cstr(r, "depths"), force = mpack_node_map_cstr(r, "forces");
    ASSERT_EQ(mpack_node_array_length(ents), 8u);
    ASSERT_EQ(mpack_node_array_length(pos), 24u);
    const float full = 1000.0f * 9.81f * (4.0f / 3.0f * DE_PI * 0.25f * 0.25f * 0.25f);
    for (u32 k = 0; k < 8; ++k) {
        const bool first = k < 4;
        EXPECT_EQ(mpack_node_u64(mpack_node_array_at(ents, k)), first ? in.bits : out.bits);
        EXPECT_FLOAT_EQ(at(pos, 3 * k + 1), -0.5f, 1e-5f);
        EXPECT_EQ(mpack_node_bool(mpack_node_array_at(wet, k)), first);
        EXPECT_FLOAT_EQ(at(depth, k), first ? 0.5f : 0.0f, 1e-5f);
        EXPECT_FLOAT_EQ(at(force, k), first ? full : 0.0f, 0.01f);
    }
    /* The probes are in world space: the first is at the body + (-0.5, -0.5, -0.5). */
    EXPECT_FLOAT_EQ(at(pos, 0), 0.5f, 1e-5f);
    EXPECT_FLOAT_EQ(at(pos, 2), 0.5f, 1e-5f);
    /* Half out of the water, half the force; a body raised clear of it, none. */
    DeTransform *t = (DeTransform *)de_entity_get(f->engine.world, in,
                                                  de_builtin_components(f->engine.world).transform);
    t->position.y = 0.5;
    r = result_of(probes(f, &in.bits, 1, true));
    EXPECT_FLOAT_EQ(at(mpack_node_map_cstr(r, "depths"), 0), 0.0f, 1e-5f);
    EXPECT_FLOAT_EQ(at(mpack_node_map_cstr(r, "forces"), 0), full * 0.5f, 0.01f);
    t->position.y = 2;
    r = result_of(probes(f, &in.bits, 1, true));
    EXPECT_TRUE(mpack_node_bool(mpack_node_array_at(mpack_node_map_cstr(r, "wet"), 0)));
    EXPECT_FLOAT_EQ(at(mpack_node_map_cstr(r, "depths"), 0), -1.5f, 1e-5f);
    EXPECT_FLOAT_EQ(at(mpack_node_map_cstr(r, "forces"), 0), 0.0f, 0.0f);
}

/* AC: water.showProbes picks the bodies the viewports draw, and water.probes without entities
 * reports them; an empty list hides them. */
TEST_F(editor_water, show_probes_round_trip, water_fx) {
    body(f, DE_WATER_LAKE, 0, 0, 0);
    const de_entity a = floater(f, 0, 0, 0), b = floater(f, 3, 0, 0);
    result_of(show(f, &b.bits, 1));
    mpack_node_t r = result_of(probes(f, NULL, 0, false));
    mpack_node_t shown = mpack_node_map_cstr(r, "shown");
    ASSERT_EQ(mpack_node_array_length(shown), 1u);
    EXPECT_EQ(mpack_node_u64(mpack_node_array_at(shown, 0)), b.bits);
    mpack_node_t ents = mpack_node_map_cstr(r, "entities");
    ASSERT_EQ(mpack_node_array_length(ents), 4u);
    EXPECT_EQ(mpack_node_u64(mpack_node_array_at(ents, 0)), b.bits);
    /* The viewport overlay draws the same probes. */
    de__ed_probe_mark m[16];
    ASSERT_EQ(de__ed_water_marks(&f->editor, m, 16), 4u);
    EXPECT_TRUE(m[0].wet);
    EXPECT_FLOAT_EQ(m[0].depth, 0.5f, 1e-5f);
    EXPECT_FLOAT_EQ(m[0].radius, 0.25f, 1e-6f);
    /* Asking for others leaves the shown list alone. */
    r = result_of(probes(f, &a.bits, 1, true));
    EXPECT_EQ(mpack_node_array_length(mpack_node_map_cstr(r, "shown")), 1u);
    /* Hide. */
    result_of(show(f, NULL, 0));
    r = result_of(probes(f, NULL, 0, false));
    EXPECT_EQ(mpack_node_array_length(mpack_node_map_cstr(r, "shown")), 0u);
    EXPECT_EQ(mpack_node_array_length(mpack_node_map_cstr(r, "entities")), 0u);
    EXPECT_EQ(de__ed_water_marks(&f->editor, m, 16), 0u);
    /* A new connection starts with none shown. */
    result_of(show(f, &a.bits, 1));
    for (u32 i = 0; i < f->editor.reset_count; ++i)
        f->editor.resets[i](&f->editor);
    EXPECT_EQ(de__ed_water_marks(&f->editor, m, 16), 0u);
}
#endif
