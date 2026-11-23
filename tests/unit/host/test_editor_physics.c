/* DENG-821: physics.loadAsset and the mouse spring physics.drag.begin/update/end (DIDE protocol
 * 1.23, DIDE-397; capabilities physics.authoring and physics.mouseSpring), through the real
 * handlers and the generated protocol readers and writers. */
#include "de_test.h"
#if !defined(DE_SHIPPING)
#include "host/editor/editor_internal.h"
#include "ecs/ecs_internal.h"
#include "ecs/system_internal.h"
#include "host/host_internal.h"
#include <dengine/de_ecs.h>
#include <dengine/de_physics.h>
#include <math.h>
#include <stdlib.h>

#define STORAGE  (2u << 20)
#define RESPONSE (64u << 10)
#define DT       (1.0f / 60.0f)

typedef struct phys_fx {
    de_engine engine;
    de__editor editor;
    de_physics_world *p;
    unsigned char *storage;
    char *response;
    char *params;
    mpack_tree_t tree;
    bool parsed;
} phys_fx;

static void phys_fx_setup(phys_fx *f) {
    memset(f, 0, sizeof *f);
    de_fp_env_init_thread(); /* physics.step is a deterministic system */
    ASSERT_EQ(de_type_registry_create(NULL, &f->engine.types), DE_OK);
    ASSERT_EQ(de_world_create(&(de_world_desc){.types = f->engine.types}, &f->engine.world), DE_OK);
    ASSERT_EQ(de_physics_create(&(de_physics_desc){0}, &f->p), DE_OK);
    ASSERT_EQ(de_physics_attach(f->p, f->engine.world, f->engine.types), DE_OK);
    f->editor.e = &f->engine;
    f->editor.mode = DE_EP_WORLD_MODE_EDIT;
    f->storage = (unsigned char *)malloc(STORAGE);
    f->response = (char *)malloc(RESPONSE);
    f->params = (char *)malloc((2u << 20) + 4096u);
    ASSERT_TRUE(f->storage != NULL && f->response != NULL && f->params != NULL);
    de__ed_world_register(&f->editor);
    de__ed_physics_register(&f->editor);
}
static void phys_fx_teardown(phys_fx *f) {
    if (f->parsed)
        mpack_tree_destroy(&f->tree);
    for (u32 i = 0; i < f->editor.reset_count; ++i)
        f->editor.resets[i](&f->editor);
    de_world_destroy(f->engine.world); /* first: its systems point to the physics world */
    de_physics_destroy(f->p);
    de_type_registry_destroy(f->engine.types);
    free(f->storage);
    free(f->response);
    free(f->params);
}

/* Calls `m` with `params` (the method's params struct) and returns the parsed reply. */
static mpack_node_t call(phys_fx *f, de_ep_method m, const void *params) {
    mpack_writer_t w;
    mpack_writer_init(&w, f->params, (2u << 20) + 4096u);
    switch (m) {
    case DE_EP_METHOD_PHYSICS_LOAD_ASSET:
        de_ep_write_physics_load_asset_params(&w, (const de_ep_physics_load_asset_params *)params);
        break;
    case DE_EP_METHOD_PHYSICS_DRAG_BEGIN:
        de_ep_write_physics_drag_begin_params(&w, (const de_ep_physics_drag_begin_params *)params);
        break;
    case DE_EP_METHOD_PHYSICS_DRAG_UPDATE:
        de_ep_write_physics_drag_update_params(&w,
                                               (const de_ep_physics_drag_update_params *)params);
        break;
    default:
        de_ep_write_physics_drag_params(&w, (const de_ep_physics_drag_params *)params);
        break;
    }
    const size_t psize = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    const de_ep_request req = {
        .id = 7, .method = de_ep_cstr("physics"), .params = {f->params, psize}};
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

static bool is_str(mpack_node_t n, const char *s) {
    return mpack_node_strlen(n) == strlen(s) && memcmp(mpack_node_str(n), s, strlen(s)) == 0;
}

static mpack_node_t load(phys_fx *f, const char *path, const char *text) {
    de_ep_physics_load_asset_params q = {.path = de_ep_cstr(path)};
    if (text != NULL) {
        q.has_text = true;
        q.text = de_ep_cstr(text);
    }
    return call(f, DE_EP_METHOD_PHYSICS_LOAD_ASSET, &q);
}

/* A ray from `o` along `d`. */
typedef struct ray {
    double o[3];
    float d[3];
} ray;
static de_ep_physics_drag_begin_params begin_params(ray *r) {
    de_ep_physics_drag_begin_params q;
    memset(&q, 0, sizeof q);
    q.origin = (de_ep_list_f64){r->o, 3};
    q.direction = (de_ep_list_f32){r->d, 3};
    return q;
}
static mpack_node_t update(phys_fx *f, u32 drag, double x, double y, double z) {
    double t[3] = {x, y, z};
    const de_ep_physics_drag_update_params q = {drag, {t, 3}};
    return call(f, DE_EP_METHOD_PHYSICS_DRAG_UPDATE, &q);
}
static mpack_node_t end(phys_fx *f, u32 drag) {
    const de_ep_physics_drag_params q = {drag};
    return call(f, DE_EP_METHOD_PHYSICS_DRAG_END, &q);
}

/* A dynamic 1 kg sphere (radius 0.5, no gravity) at (x, y, z); its body exists after a tick. */
static de_entity ball(phys_fx *f, de_body_type type, double x, double y, double z) {
    de_world *w = f->engine.world;
    const de_builtin_component_ids bi = de_builtin_components(w);
    const de_component comps[2] = {bi.transform, bi.global_transform};
    de_entity e;
    ASSERT_EQ(de_entity_spawn_with(w, comps, 2, &e), DE_OK);
    de_transform *t = (de_transform *)de_entity_get(w, e, bi.transform);
    t->position = (de_dvec3){x, y, z};
    DeRigidBody *rb = (DeRigidBody *)de_entity_add(w, e, de_component_find(w, "DeRigidBody"));
    ASSERT_TRUE(rb != NULL);
    rb->type = type;
    rb->mass = 1.0f;
    rb->gravity_scale = 0.0f;
    DeCollider *c = (DeCollider *)de_entity_add(w, e, de_component_find(w, "DeCollider"));
    ASSERT_TRUE(c != NULL);
    c->shape_kind = DE_COLLIDER_SHAPE_SPHERE;
    c->radius = 0.5f;
    c->offset.rotation.w = 1.0f;
    c->offset.scale = (de_vec3){1, 1, 1};
    c->material.friction = 0.5f;
    c->material.density = 1000.0f;
    return e;
}
static void tick(phys_fx *f, u32 n) {
    for (u32 i = 0; i < n; ++i) {
        de_world_update_events(f->engine.world);
        ASSERT_EQ(de_world_run_schedule(f->engine.world, DE_SCHEDULE_FIXED_UPDATE, DT, 0.0f),
                  DE_OK);
        for (u32 k = 0; k < f->editor.tick_count; ++k)
            f->editor.ticks[k](&f->editor);
    }
}
static de_dvec3 position(phys_fx *f, de_entity e) {
    const de_transform *t = (const de_transform *)de_entity_get_const(
        f->engine.world, e, de_builtin_components(f->engine.world).transform);
    return t->position;
}

/* Grabs the ball at the origin from (0, -5, 0) along +Y. Returns the drag id. */
static u32 grab(phys_fx *f, de_entity e) {
    ray r = {{0, -5, 0}, {0, 2, 0}}; /* the direction need not be unit length */
    const de_ep_physics_drag_begin_params q = begin_params(&r);
    mpack_node_t res = result_of(call(f, DE_EP_METHOD_PHYSICS_DRAG_BEGIN, &q));
    EXPECT_EQ(mpack_node_u64(mpack_node_map_cstr(res, "entity")), e.bits);
    EXPECT_FALSE(mpack_node_map_contains_cstr(res, "bone"));
    mpack_node_t pt = mpack_node_map_cstr(res, "point");
    EXPECT_FLOAT_EQ(mpack_node_double(mpack_node_array_at(pt, 1)), -0.5, 1e-3);
    EXPECT_FLOAT_EQ(mpack_node_float(mpack_node_map_cstr(res, "distance")), 4.5f, 1e-3f);
    return mpack_node_u32(mpack_node_map_cstr(res, "drag"));
}

TEST_F(editor_physics, advertises_the_capabilities, phys_fx) {
    bool authoring = false, spring = false;
    for (u32 i = 0; i < f->editor.cap_count; ++i) {
        authoring |= strcmp(f->editor.caps[i], "physics.authoring") == 0;
        spring |= strcmp(f->editor.caps[i], "physics.mouseSpring") == 0;
    }
    EXPECT_TRUE(authoring);
    EXPECT_TRUE(spring);
    EXPECT_STREQ(DE_EP_PROTOCOL_VERSION, "1.24"); /* pinned dide 406153b1 (DENG-828) */
}

/* The DIDE conformance probes at 1.23. */
TEST_F(editor_physics, load_asset_rejects_bad_requests, phys_fx) {
    expect_error(load(f, "dide-conformance.toml", ""), DE_EP_ERR_INVALID_PARAMS);
    expect_error(load(f, "dide-conformance.ragdoll.toml", "bones = []"),
                 DE_EP_ERR_INVALID_PARAMS);
    expect_error(load(f, "dide-conformance/no-such.physmat.toml", NULL), DE_EP_ERR_NOT_FOUND);
    expect_error(load(f, "/abs/a.physmat.toml", "version = 1\n"), DE_EP_ERR_INVALID_PARAMS);
    expect_error(load(f, "a/../b.physmat.toml", "version = 1\n"), DE_EP_ERR_INVALID_PARAMS);
    expect_error(load(f, "bad.physmat.toml", "friction_combine = \"loudest\"\n"),
                 DE_EP_ERR_INVALID_PARAMS);
    /* Text over 1 MiB. */
    const size_t n = (1u << 20) + 1u;
    char *big = (char *)malloc(n + 1);
    ASSERT_TRUE(big != NULL);
    memset(big, ' ', n);
    big[n] = '\0';
    expect_error(load(f, "big.physmat.toml", big), DE_EP_ERR_INVALID_PARAMS);
    free(big);
}

TEST_F(editor_physics, load_asset_registers_material_and_ragdoll, phys_fx) {
    f->editor.mode = DE_EP_WORLD_MODE_EDIT; /* allowed in every mode */
    mpack_node_t r = result_of(load(f, "physics/bouncy.physmat.toml", "restitution = 0.9\n"));
    EXPECT_TRUE(is_str(mpack_node_map_cstr(r, "kind"), "material"));
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(r, "respawned")), 0u);
    de_material_phys m;
    ASSERT_TRUE(de_physics_material_find(f->p, de_name_from("physics/bouncy.physmat.toml"), &m));
    EXPECT_FLOAT_EQ(m.restitution, 0.9f, 1e-6f);

    static const char k_ragdoll[] = "version = 1\n"
                                    "[[bones]]\n"
                                    "name = \"pelvis\"\n"
                                    "bind = { position = [0.0, 0.0, 1.0] }\n"
                                    "shape = \"box\"\n"
                                    "half_extents = [0.15, 0.1, 0.1]\n"
                                    "mass = 10.0\n";
    f->editor.mode = DE_EP_WORLD_MODE_PLAY;
    r = result_of(load(f, "physics/hero.ragdoll.toml", k_ragdoll));
    EXPECT_TRUE(is_str(mpack_node_map_cstr(r, "kind"), "ragdoll"));
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(r, "respawned")), 0u);
}

TEST_F(editor_physics, edit_mode_has_no_drags, phys_fx) {
    ray r = {{0, -5, 0}, {0, 1, 0}};
    const de_ep_physics_drag_begin_params q = begin_params(&r);
    expect_error(call(f, DE_EP_METHOD_PHYSICS_DRAG_BEGIN, &q), DE_EP_ERR_WRONG_MODE);
    expect_error(update(f, UINT32_MAX - 1, 0, 0, 0), DE_EP_ERR_WRONG_MODE);
    expect_error(end(f, UINT32_MAX - 1), DE_EP_ERR_NOT_FOUND);
}

TEST_F(editor_physics, a_miss_is_an_empty_result, phys_fx) {
    f->editor.mode = DE_EP_WORLD_MODE_PAUSED;
    ray r = {{1e6, 1e6, 1e6}, {0, 0, 1}};
    de_ep_physics_drag_begin_params q = begin_params(&r);
    q.has_max_distance = true;
    q.max_distance = 1.0f;
    mpack_node_t res = result_of(call(f, DE_EP_METHOD_PHYSICS_DRAG_BEGIN, &q));
    EXPECT_EQ(mpack_node_map_count(res), 0u);
    expect_error(update(f, UINT32_MAX - 1, 0, 0, 0), DE_EP_ERR_NOT_FOUND);
}

TEST_F(editor_physics, begin_validates_its_params, phys_fx) {
    f->editor.mode = DE_EP_WORLD_MODE_PLAY;
    ray r = {{0, -5, 0}, {0, 1, 0}};
    de_ep_physics_drag_begin_params q = begin_params(&r);
    q.has_hertz = true;
    q.hertz = 0.0f;
    expect_error(call(f, DE_EP_METHOD_PHYSICS_DRAG_BEGIN, &q), DE_EP_ERR_INVALID_PARAMS);
    q = begin_params(&r);
    q.has_damping_ratio = true;
    q.damping_ratio = -0.1f;
    expect_error(call(f, DE_EP_METHOD_PHYSICS_DRAG_BEGIN, &q), DE_EP_ERR_INVALID_PARAMS);
    q.damping_ratio = 0.0f; /* 0 is allowed */
    result_of(call(f, DE_EP_METHOD_PHYSICS_DRAG_BEGIN, &q));
    q = begin_params(&r);
    q.has_max_force = true;
    q.max_force = 0.0f;
    expect_error(call(f, DE_EP_METHOD_PHYSICS_DRAG_BEGIN, &q), DE_EP_ERR_INVALID_PARAMS);
    ray zero = {{0, -5, 0}, {0, 0, 0}};
    q = begin_params(&zero);
    expect_error(call(f, DE_EP_METHOD_PHYSICS_DRAG_BEGIN, &q), DE_EP_ERR_INVALID_PARAMS);
    q = begin_params(&r);
    q.origin.count = 2;
    expect_error(call(f, DE_EP_METHOD_PHYSICS_DRAG_BEGIN, &q), DE_EP_ERR_INVALID_PARAMS);
    q = begin_params(&r);
    q.has_entity = true;
    q.entity = UINT64_MAX - 1;
    expect_error(call(f, DE_EP_METHOD_PHYSICS_DRAG_BEGIN, &q), DE_EP_ERR_NOT_FOUND);
}

TEST_F(editor_physics, static_and_kinematic_bodies_are_not_grabbed, phys_fx) {
    ball(f, DE_BODY_KINEMATIC, 0, 0, 0);
    tick(f, 1);
    f->editor.mode = DE_EP_WORLD_MODE_SIMULATE;
    ray r = {{0, -5, 0}, {0, 1, 0}};
    const de_ep_physics_drag_begin_params q = begin_params(&r);
    EXPECT_EQ(mpack_node_map_count(result_of(call(f, DE_EP_METHOD_PHYSICS_DRAG_BEGIN, &q))), 0u);
}

TEST_F(editor_physics, the_spring_pulls_the_body_to_the_target, phys_fx) {
    const de_entity e = ball(f, DE_BODY_DYNAMIC, 0, 0, 0);
    tick(f, 1);
    f->editor.mode = DE_EP_WORLD_MODE_SIMULATE;
    const u32 drag = grab(f, e);
    EXPECT_NE(drag, 0u);
    result_of(update(f, drag, 0, -0.5, 2.0)); /* lift the grab point 2 m */
    expect_error(update(f, drag, 0, NAN, 0), DE_EP_ERR_INVALID_PARAMS);
    expect_error(update(f, drag + 1, 0, 0, 0), DE_EP_ERR_NOT_FOUND);
    tick(f, 600); /* 10 s at 5 Hz, damping 0.7: settled, spin included */
    /* The grab point (on the surface) is at the target, so the center is a radius from it; the
     * ball may have turned about the grab point. */
    const de_dvec3 p = position(f, e);
    const double dist = sqrt(p.x * p.x + (p.y + 0.5) * (p.y + 0.5) + (p.z - 2.0) * (p.z - 2.0));
    EXPECT_FLOAT_EQ(dist, 0.5, 0.03);
    EXPECT_TRUE(p.z > 1.4);
    result_of(end(f, drag));
    expect_error(end(f, drag), DE_EP_ERR_NOT_FOUND);
}

TEST_F(editor_physics, paused_holds_the_body, phys_fx) {
    const de_entity e = ball(f, DE_BODY_DYNAMIC, 0, 0, 0);
    tick(f, 1);
    f->editor.mode = DE_EP_WORLD_MODE_PAUSED;
    const u32 drag = grab(f, e);
    result_of(update(f, drag, 0, -0.5, 2.0));
    /* Paused disables physics.step (a game system), so the spring does not act either. */
    de_world *w = f->engine.world;
    const ecs_system_registry *reg = w->systems;
    for (u32 i = 0; reg != NULL && i < reg->slots.count; ++i) {
        const ecs_system_slot *slot = (const ecs_system_slot *)de_array_at(&reg->slots, i);
        if (slot->sys != NULL && strcmp(slot->sys->name, DE_PHYSICS_STEP_SYSTEM) == 0)
            de_system_set_enabled(w, slot->sys->handle, false);
    }
    tick(f, 30);
    EXPECT_FLOAT_EQ(position(f, e).z, 0.0, 1e-9);
    result_of(update(f, drag, 0, -0.5, 1.0)); /* the drag is still live */
}

TEST_F(editor_physics, the_drag_ends_on_stop_despawn_reset_and_new_begin, phys_fx) {
    const de_entity e = ball(f, DE_BODY_DYNAMIC, 0, 0, 0);
    tick(f, 1);
    f->editor.mode = DE_EP_WORLD_MODE_PLAY;
    /* A new begin ends the previous drag; ids are not reused. */
    const u32 first = grab(f, e);
    const u32 second = grab(f, e);
    EXPECT_NE(first, second);
    expect_error(update(f, first, 0, 0, 0), DE_EP_ERR_NOT_FOUND);
    /* Stop: back to Edit ends it. */
    f->editor.mode = DE_EP_WORLD_MODE_EDIT;
    tick(f, 1);
    f->editor.mode = DE_EP_WORLD_MODE_PLAY;
    expect_error(update(f, second, 0, 0, 0), DE_EP_ERR_NOT_FOUND);
    /* Disconnect (the editor's reset hooks). */
    u32 d = grab(f, e);
    for (u32 i = 0; i < f->editor.reset_count; ++i)
        f->editor.resets[i](&f->editor);
    expect_error(end(f, d), DE_EP_ERR_NOT_FOUND);
    /* The entity goes. */
    d = grab(f, e);
    de_entity_despawn(f->engine.world, e);
    tick(f, 1);
    expect_error(update(f, d, 0, 0, 0), DE_EP_ERR_NOT_FOUND);
}

TEST_F(editor_physics, entity_filter_limits_the_hit, phys_fx) {
    const de_entity near = ball(f, DE_BODY_DYNAMIC, 0, 0, 0);
    const de_entity far = ball(f, DE_BODY_DYNAMIC, 0, 3, 0);
    tick(f, 1);
    f->editor.mode = DE_EP_WORLD_MODE_PLAY;
    ray r = {{0, -5, 0}, {0, 1, 0}};
    de_ep_physics_drag_begin_params q = begin_params(&r);
    q.has_entity = true;
    q.entity = far.bits;
    mpack_node_t res = result_of(call(f, DE_EP_METHOD_PHYSICS_DRAG_BEGIN, &q));
    EXPECT_EQ(mpack_node_u64(mpack_node_map_cstr(res, "entity")), far.bits);
    EXPECT_FLOAT_EQ(mpack_node_float(mpack_node_map_cstr(res, "distance")), 7.5f, 1e-3f);
    (void)near;
}
#endif
