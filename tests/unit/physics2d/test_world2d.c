/* test_world2d.c: 2D physics world, body pool, handles, checksum and step (DENG-252, SPEC §13.2,
 * §13.3). */
#include "de_test.h"

#include <dengine/de_physics.h>

#include "physics/2d/p2d_internal.h"

#include <math.h>
#include <string.h>

/* ---- Fixture: a world on a tracking allocator; teardown checks that nothing leaked --------- */

typedef struct p2_fx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_physics2d_world *p;
} p2_fx;

static void p2_fx_setup(p2_fx *f) {
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_PHYSICS);
    de_physics2d_desc d;
    memset(&d, 0, sizeof d);
    d.allocator = &f->alloc;
    d.max_bodies = 64;
    ASSERT_EQ(de_physics2d_create(&d, &f->p), DE_OK);
}

static void p2_fx_teardown(p2_fx *f) {
    de_physics2d_destroy(f->p);
    const de_tracking_stats s = de_tracking_allocator_stats(&f->track);
    EXPECT_EQ(s.bytes_live, 0u);
    EXPECT_EQ(s.blocks_live, 0u);
}

static de_shape make_box(de_physics2d_world *p, f32 hx, f32 hy) {
    de_shape2d_desc d;
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE2D_BOX;
    d.half_extents = (de_vec2){hx, hy};
    const de_shape s = de_shape2d_create(p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(s));
    return s;
}

static de_shape make_circle(de_physics2d_world *p, f32 r) {
    de_shape2d_desc d;
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE2D_CIRCLE;
    d.radius = r;
    const de_shape s = de_shape2d_create(p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(s));
    return s;
}

static de_body make_body(de_physics2d_world *p, de_body_type type, de_shape s, f64 x) {
    de_body2d_desc d;
    memset(&d, 0, sizeof d);
    d.type = type;
    d.shape = s;
    d.transform.x = x;
    d.transform.rotation = (de_rot2d){1, 0};
    const de_body b = de_body2d_create(p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(b));
    return b;
}

static u32 body_index(de_body b) {
    return de__handle_index(b.id);
}

/* ---- create / destroy ------------------------------------------------------------------- */

TEST(physics2d_world, create_destroy_defaults) {
    de_tracking_allocator t;
    de_allocator a = de_tracking_allocator_init(&t, NULL, DE_MEM_PHYSICS);

    /* NULL desc and (desc){0} both give the defaults. */
    de_physics2d_world *p = NULL;
    ASSERT_EQ(de_physics2d_create(NULL, &p), DE_OK);
    ASSERT_TRUE(p != NULL);
    EXPECT_EQ(p->max_bodies, DE_PHYSICS_DEFAULT_MAX_BODIES);
    EXPECT_EQ(p->bodies.capacity, 65536u);
    EXPECT_EQ(p->substeps, 4u);
    EXPECT_EQ(de_physics2d_get_plane(p), DE_PHYSICS2D_PLANE_SIDE);
    de_vec2 g = de_physics2d_get_gravity(p);
    EXPECT_TRUE(g.x == 0.0f && g.y == -9.81f);
    EXPECT_EQ(de_physics2d_tick(p), 0u);
    EXPECT_EQ(de_body2d_count(p), 0u);
    de_physics2d_destroy(p);

    de_physics2d_desc d;
    memset(&d, 0, sizeof d);
    d.allocator = &a;
    ASSERT_EQ(de_physics2d_create(&d, &p), DE_OK);
    EXPECT_EQ(p->max_bodies, 65536u);
    EXPECT_EQ(p->alloc, (const de_allocator *)&a);
    de_physics2d_destroy(p);
    EXPECT_EQ(de_tracking_allocator_stats(&t).bytes_live, 0u);

    /* Top-down: no default gravity. */
    d.plane = DE_PHYSICS2D_PLANE_TOP_DOWN;
    ASSERT_EQ(de_physics2d_create(&d, &p), DE_OK);
    g = de_physics2d_get_gravity(p);
    EXPECT_TRUE(g.x == 0.0f && g.y == 0.0f);
    EXPECT_EQ(de_physics2d_get_plane(p), DE_PHYSICS2D_PLANE_TOP_DOWN);
    de_physics2d_destroy(p);

    /* Explicit values are kept; zero gravity is reachable through the setter. */
    d.plane = DE_PHYSICS2D_PLANE_SIDE;
    d.gravity = (de_vec2){1, 2};
    d.substeps = 8;
    d.max_bodies = 10;
    ASSERT_EQ(de_physics2d_create(&d, &p), DE_OK);
    EXPECT_EQ(p->substeps, 8u);
    EXPECT_EQ(p->bodies.capacity, 10u);
    EXPECT_TRUE(de_physics2d_get_gravity(p).y == 2.0f);
    de_physics2d_set_gravity(p, (de_vec2){0, 0});
    EXPECT_TRUE(de_physics2d_get_gravity(p).y == 0.0f);
    de_physics2d_set_gravity(p, (de_vec2){NAN, 0});
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(de_physics2d_get_gravity(p).x == 0.0f);
    de_physics2d_destroy(p);

    /* Refusals. */
    EXPECT_EQ(de_physics2d_create(&d, NULL), DE_ERR_INVALID_ARGUMENT);
    d.max_bodies = DE_PHYSICS_MAX_BODIES_LIMIT + 1u;
    p = (de_physics2d_world *)&d;
    EXPECT_EQ(de_physics2d_create(&d, &p), DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(p == NULL);
    d.max_bodies = 4;
    d.gravity.x = INFINITY;
    EXPECT_EQ(de_physics2d_create(&d, &p), DE_ERR_INVALID_ARGUMENT);
    d.gravity.x = 0.0f;
    d.plane = (de_physics2d_plane)7;
    EXPECT_EQ(de_physics2d_create(&d, &p), DE_ERR_INVALID_ARGUMENT);
    d.plane = DE_PHYSICS2D_PLANE_SIDE;

    /* Out of memory: nothing leaks. */
    for (i64 n = 0; n < 2; ++n) {
        de_tracking_allocator_fail_after(&t, n);
        EXPECT_EQ(de_physics2d_create(&d, &p), DE_ERR_OUT_OF_MEMORY);
        EXPECT_TRUE(p == NULL);
        de_tracking_allocator_fail_after(&t, -1);
    }
    EXPECT_EQ(de_tracking_allocator_stats(&t).bytes_live, 0u);
    de_physics2d_destroy(NULL);
    EXPECT_EQ(de_physics2d_tick(NULL), 0u);
    EXPECT_EQ(de_body2d_count(NULL), 0u);
}

/* ---- handles ---------------------------------------------------------------------------- */

TEST_F(physics2d_world, handle_generations, p2_fx) {
    de_physics2d_world *p = f->p;
    const de_shape box = make_box(p, 0.5f, 0.5f);
    const de_body a = make_body(p, DE_BODY_DYNAMIC, box, 0.0);
    EXPECT_TRUE(de_body2d_valid(p, a));
    EXPECT_EQ(de__handle_gen(a.id), 1u);
    de_body2d_destroy(p, a);
    EXPECT_FALSE(de_body2d_valid(p, a));

    /* The slot is reused with the next generation; the old handle stays stale everywhere. */
    const de_body b = make_body(p, DE_BODY_DYNAMIC, box, 1.0);
    EXPECT_EQ(body_index(b), body_index(a));
    EXPECT_EQ(de__handle_gen(b.id), 2u);
    EXPECT_NE(a.id, b.id);
    de_transform2d t;
    memset(&t, 0, sizeof t);
    de_vec2 v = {0, 0};
    f32 w = 0.0f;
    de_mass_props2d m;
    EXPECT_FALSE(de_body2d_get_transform(p, a, &t));
    EXPECT_FALSE(de_body2d_set_transform(p, a, &t));
    EXPECT_FALSE(de_body2d_get_velocity(p, a, &v, &w));
    EXPECT_FALSE(de_body2d_set_velocity(p, a, v, 0.0f));
    EXPECT_FALSE(de_body2d_get_mass_props(p, a, &m));
    de_body2d_destroy(p, a); /* ignored */
    EXPECT_TRUE(de_body2d_valid(p, b));
    EXPECT_EQ(de_body2d_count(p), 1u);

    /* Invalid ids: zero, an index never used, another world's pool range. */
    EXPECT_FALSE(de_body2d_valid(p, (de_body){0}));
    EXPECT_FALSE(de_body2d_valid(p, (de_body){de__handle_make(40, 1)}));
    EXPECT_FALSE(de_body2d_valid(NULL, b));

    /* Generations wrap 4095 -> 1 (never 0). */
    p->bodies.generation[body_index(b)] = DE__HANDLE_GEN_MAX;
    const de_body top = de__body2d_handle(p, body_index(b));
    de_body2d_destroy(p, top);
    EXPECT_EQ(p->bodies.generation[body_index(b)], 1u);
    const de_body c = make_body(p, DE_BODY_STATIC, box, 2.0);
    EXPECT_EQ(de__handle_gen(c.id), 1u);
    EXPECT_FALSE(de_body2d_valid(p, top));
    de_body2d_destroy(p, c);

    /* Shapes have generations too. */
    const de_shape circ = make_circle(p, 1.0f);
    EXPECT_EQ(de_shape2d_destroy(p, circ), DE_OK);
    EXPECT_FALSE(de_shape2d_valid(p, circ));
    EXPECT_EQ(de_shape2d_destroy(p, circ), DE_ERR_INVALID_ARGUMENT);
    const de_shape circ2 = make_circle(p, 2.0f);
    EXPECT_EQ(de__handle_index(circ2.id), de__handle_index(circ.id));
    EXPECT_NE(circ2.id, circ.id);
    de_shape2d_kind k;
    EXPECT_FALSE(de_shape2d_get_kind(p, circ, &k));
    EXPECT_TRUE(de_shape2d_get_kind(p, circ2, &k));
    EXPECT_EQ(k, DE_SHAPE2D_CIRCLE);
    /* A stale shape is refused by body create. */
    de_body2d_desc bd;
    memset(&bd, 0, sizeof bd);
    bd.shape = circ;
    EXPECT_FALSE(DE_HANDLE_VALID(de_body2d_create(p, &bd)));
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
}

TEST_F(physics2d_world, lifo_free_list_and_iteration_order, p2_fx) {
    de_physics2d_world *p = f->p;
    const de_shape box = make_box(p, 0.5f, 0.5f);
    de_body b[10];
    for (u32 i = 0; i < 10; ++i) {
        b[i] = make_body(p, DE_BODY_DYNAMIC, box, (f64)i);
        EXPECT_EQ(body_index(b[i]), i);
    }
    de_body2d_destroy(p, b[3]);
    de_body2d_destroy(p, b[7]);
    /* LIFO: 7 first, then 3, then fresh slots. */
    EXPECT_EQ(body_index(make_body(p, DE_BODY_DYNAMIC, box, 70.0)), 7u);
    EXPECT_EQ(body_index(make_body(p, DE_BODY_DYNAMIC, box, 30.0)), 3u);
    EXPECT_EQ(body_index(make_body(p, DE_BODY_DYNAMIC, box, 100.0)), 10u);

    /* Iteration visits ascending indices; positions show which body sits where. */
    de_body2d_destroy(p, b[5]);
    static const f64 expect[] = {0, 1, 2, 30, 4, 6, 70, 8, 9, 100};
    u32 n = 0;
    for (u32 i = 0; i < p->bodies.high_water; ++i) {
        if (!p->bodies.alive[i])
            continue;
        ASSERT_LT(n, 10u);
        EXPECT_TRUE(p->bodies.position[i].x == expect[n]);
        ++n;
    }
    EXPECT_EQ(n, 10u);
    EXPECT_EQ(de_body2d_count(p), 10u);
}

TEST(physics2d_world, pool_full) {
    de_physics2d_desc d;
    memset(&d, 0, sizeof d);
    d.max_bodies = 4;
    de_physics2d_world *p;
    ASSERT_EQ(de_physics2d_create(&d, &p), DE_OK);
    de_body2d_desc bd;
    memset(&bd, 0, sizeof bd);
    de_body last = {0};
    for (u32 i = 0; i < 4; ++i) {
        last = de_body2d_create(p, &bd);
        ASSERT_TRUE(DE_HANDLE_VALID(last));
    }
    EXPECT_FALSE(DE_HANDLE_VALID(de_body2d_create(p, &bd)));
    EXPECT_EQ(de_error_last()->result, DE_ERR_OUT_OF_MEMORY);
    de_body2d_destroy(p, last);
    EXPECT_TRUE(DE_HANDLE_VALID(de_body2d_create(p, &bd)));
    de_physics2d_destroy(p);
}

/* ---- body API --------------------------------------------------------------------------- */

TEST_F(physics2d_world, body_api, p2_fx) {
    de_physics2d_world *p = f->p;

    /* (desc){0}: static at the origin, no shape, default filter and density. */
    de_body2d_desc d;
    memset(&d, 0, sizeof d);
    const de_body s0 = de_body2d_create(p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(s0));
    u32 i = body_index(s0);
    EXPECT_EQ(p->bodies.type[i], (u8)DE_BODY_STATIC);
    EXPECT_TRUE(p->bodies.rotation[i].c == 1.0f && p->bodies.rotation[i].s == 0.0f);
    EXPECT_EQ(p->bodies.filter[i].layer, DE_COLLISION_FILTER_DEFAULT_LAYER);
    EXPECT_EQ(p->bodies.filter[i].mask, DE_COLLISION_FILTER_DEFAULT_MASK);
    EXPECT_TRUE(p->bodies.material[i].density == DE_PHYSICS2D_DEFAULT_DENSITY);
    EXPECT_TRUE(de__aabb2d_is_empty(p->bodies.world_aabb[i]));
    de_mass_props2d m;
    ASSERT_TRUE(de_body2d_get_mass_props(p, s0, &m));
    EXPECT_TRUE(m.mass == 0.0f && m.area == 0.0f && m.inertia == 0.0f);

    /* A dynamic 2 x 1 box with density 3: mass 6, inertia m (w^2 + h^2) / 12. */
    const de_shape box = make_box(p, 1.0f, 0.5f);
    memset(&d, 0, sizeof d);
    d.type = DE_BODY_DYNAMIC;
    d.shape = box;
    d.material.density = 3.0f;
    d.transform.x = 1e5;
    d.transform.y = -2.5;
    d.transform.rotation = (de_rot2d){3.0f, 4.0f}; /* normalized to (0.6, 0.8) */
    d.linear_velocity = (de_vec2){1, 2};
    d.angular_velocity = 3.0f;
    d.filter.layer = 4;
    const de_body b = de_body2d_create(p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(b));
    i = body_index(b);
    de_transform2d t;
    ASSERT_TRUE(de_body2d_get_transform(p, b, &t));
    EXPECT_TRUE(t.x == 1e5 && t.y == -2.5);
    EXPECT_FLOAT_EQ(t.rotation.c, 0.6, 1e-7);
    EXPECT_FLOAT_EQ(t.rotation.s, 0.8, 1e-7);
    EXPECT_EQ(p->bodies.filter[i].layer, 4u);
    EXPECT_EQ(p->bodies.filter[i].mask, 0u); /* a non-zero filter is kept as is */
    ASSERT_TRUE(de_body2d_get_mass_props(p, b, &m));
    EXPECT_FLOAT_EQ(m.mass, 6.0, 1e-6);
    EXPECT_FLOAT_EQ(m.area, 2.0, 1e-7);
    EXPECT_FLOAT_EQ(m.inertia, 6.0 * (4.0 + 1.0) / 12.0, 1e-6);
    EXPECT_FLOAT_EQ(p->bodies.inv_mass[i], 1.0 / 6.0, 1e-7);
    EXPECT_FLOAT_EQ(p->bodies.inv_inertia[i], 12.0 / 30.0, 1e-7);
    de_vec2 v;
    f32 w;
    ASSERT_TRUE(de_body2d_get_velocity(p, b, &v, &w));
    EXPECT_TRUE(v.x == 1.0f && v.y == 2.0f && w == 3.0f);
    ASSERT_TRUE(de_body2d_get_velocity(p, b, NULL, NULL));
    /* The world box follows the rotated box (corners at +-1, +-0.5 rotated). */
    const de_aabb2d wb = p->bodies.world_aabb[i];
    EXPECT_FLOAT_EQ(wb.max.x - 1e5, 0.6 * 1.0 + 0.8 * 0.5, 1e-2);
    EXPECT_FLOAT_EQ(wb.max.y + 2.5, 0.8 * 1.0 + 0.6 * 0.5, 1e-5);

    /* Mass override rescales the inertia; fixed rotation zeroes the inverse inertia. */
    d.mass = 12.0f;
    d.flags = DE_BODY2D_FLAG_FIXED_ROTATION;
    const de_body b2 = de_body2d_create(p, &d);
    ASSERT_TRUE(de_body2d_get_mass_props(p, b2, &m));
    EXPECT_FLOAT_EQ(m.mass, 12.0, 1e-6);
    EXPECT_FLOAT_EQ(m.inertia, 12.0 * 5.0 / 12.0, 1e-5);
    EXPECT_TRUE(p->bodies.inv_inertia[body_index(b2)] == 0.0f);
    EXPECT_TRUE(p->bodies.inv_mass[body_index(b2)] > 0.0f);

    /* Kinematic: no mass, but velocities kept. Static: velocities zero, setter ignored. */
    d.mass = 0.0f;
    d.flags = 0;
    d.type = DE_BODY_KINEMATIC;
    const de_body k = de_body2d_create(p, &d);
    ASSERT_TRUE(de_body2d_get_mass_props(p, k, &m));
    EXPECT_TRUE(m.mass == 0.0f && m.inertia == 0.0f);
    EXPECT_FLOAT_EQ(m.area, 2.0, 1e-7);
    ASSERT_TRUE(de_body2d_get_velocity(p, k, &v, &w));
    EXPECT_TRUE(v.x == 1.0f && w == 3.0f);
    d.type = DE_BODY_STATIC;
    const de_body st = de_body2d_create(p, &d);
    ASSERT_TRUE(de_body2d_get_velocity(p, st, &v, &w));
    EXPECT_TRUE(v.x == 0.0f && v.y == 0.0f && w == 0.0f);
    EXPECT_TRUE(de_body2d_set_velocity(p, st, (de_vec2){5, 5}, 1.0f));
    ASSERT_TRUE(de_body2d_get_velocity(p, st, &v, &w));
    EXPECT_TRUE(v.x == 0.0f && w == 0.0f);
    EXPECT_TRUE(de_body2d_set_velocity(p, k, (de_vec2){-1, 0}, -2.0f));
    ASSERT_TRUE(de_body2d_get_velocity(p, k, &v, &w));
    EXPECT_TRUE(v.x == -1.0f && w == -2.0f);
    EXPECT_FALSE(de_body2d_set_velocity(p, k, (de_vec2){NAN, 0}, 0.0f));
    EXPECT_FALSE(de_body2d_set_velocity(p, k, (de_vec2){0, 0}, INFINITY));

    /* Teleport: normalized rotation, refreshed box; a zero rotation is the identity. */
    t.x = -3.0;
    t.y = 7.0;
    t.rotation = (de_rot2d){0.0f, 0.0f};
    ASSERT_TRUE(de_body2d_set_transform(p, b, &t));
    ASSERT_TRUE(de_body2d_get_transform(p, b, &t));
    EXPECT_TRUE(t.rotation.c == 1.0f && t.rotation.s == 0.0f);
    EXPECT_TRUE(p->bodies.world_aabb[i].min.x <= -4.0f && p->bodies.world_aabb[i].min.x > -4.001f);
    EXPECT_TRUE(p->bodies.world_aabb[i].max.y >= 7.5f && p->bodies.world_aabb[i].max.y < 7.501f);
    t.x = INFINITY;
    EXPECT_FALSE(de_body2d_set_transform(p, b, &t));
    EXPECT_FALSE(de_body2d_set_transform(p, b, NULL));

    /* Refusals. */
    memset(&d, 0, sizeof d);
    d.type = DE_BODY_DYNAMIC;
    EXPECT_FALSE(DE_HANDLE_VALID(de_body2d_create(p, &d))); /* dynamic without a shape */
    d.shape = box;
    d.mass = -1.0f;
    EXPECT_FALSE(DE_HANDLE_VALID(de_body2d_create(p, &d)));
    d.mass = 0.0f;
    d.transform.rotation.s = NAN;
    EXPECT_FALSE(DE_HANDLE_VALID(de_body2d_create(p, &d)));
    d.transform.rotation.s = 0.0f;
    d.angular_velocity = INFINITY;
    EXPECT_FALSE(DE_HANDLE_VALID(de_body2d_create(p, &d)));
    d.angular_velocity = 0.0f;
    d.type = (de_body_type)9;
    EXPECT_FALSE(DE_HANDLE_VALID(de_body2d_create(p, &d)));
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    EXPECT_FALSE(DE_HANDLE_VALID(de_body2d_create(p, NULL)));
    EXPECT_FALSE(DE_HANDLE_VALID(de_body2d_create(NULL, &d)));

    /* Segments and chains: static and kinematic only. */
    const de_vec2 pts[4] = {{0, 0}, {-1, 0}, {-2, 0.5f}, {-3, 0}};
    const de_shape chain = de_shape2d_chain_create(p, pts, 4, false);
    ASSERT_TRUE(DE_HANDLE_VALID(chain));
    de_shape2d_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.kind = DE_SHAPE2D_SEGMENT;
    sd.p1 = (de_vec2){0, 0};
    sd.p2 = (de_vec2){1, 1};
    const de_shape seg = de_shape2d_create(p, &sd);
    ASSERT_TRUE(DE_HANDLE_VALID(seg));
    memset(&d, 0, sizeof d);
    d.type = DE_BODY_DYNAMIC;
    d.shape = chain;
    EXPECT_FALSE(DE_HANDLE_VALID(de_body2d_create(p, &d)));
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    d.shape = seg;
    EXPECT_FALSE(DE_HANDLE_VALID(de_body2d_create(p, &d)));
    d.type = DE_BODY_STATIC;
    const de_body cs = de_body2d_create(p, &d);
    EXPECT_TRUE(DE_HANDLE_VALID(cs));
    d.type = DE_BODY_KINEMATIC;
    d.shape = chain;
    const de_body ck = de_body2d_create(p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(ck));
    ASSERT_TRUE(de_body2d_get_mass_props(p, ck, &m));
    EXPECT_TRUE(m.mass == 0.0f && m.area == 0.0f);
    const de_aabb2d cb = p->bodies.world_aabb[body_index(ck)];
    EXPECT_TRUE(cb.min.x <= -3.0f && cb.max.x >= 0.0f && cb.max.y >= 0.5f && cb.min.y <= 0.0f);
}

TEST(physics2d_world, rotation_helpers) {
    const de_rot2d r = de_rot2d_from_angle(0.5f);
    EXPECT_TRUE(r.c == de_dcos(0.5f) && r.s == de_dsin(0.5f));
    EXPECT_FLOAT_EQ(de_rot2d_angle(r), 0.5, 1e-6);
    EXPECT_FLOAT_EQ(de_rot2d_angle(de_rot2d_from_angle(-3.0f)), -3.0, 1e-6);
    const de_rot2d id = de_rot2d_from_angle(0.0f);
    EXPECT_TRUE(id.c == 1.0f && id.s == 0.0f);
    /* Rotating (1, 0) by a quarter turn gives (0, 1): counter-clockwise. */
    const de_vec2 q = de__rot2d_apply(de_rot2d_from_angle(1.57079632679f), (de_vec2){1, 0});
    EXPECT_FLOAT_EQ(q.x, 0.0, 1e-6);
    EXPECT_FLOAT_EQ(q.y, 1.0, 1e-6);
}

TEST_F(physics2d_world, shape_refcount, p2_fx) {
    de_physics2d_world *p = f->p;
    const de_shape box = make_box(p, 1, 1);
    const de_body a = make_body(p, DE_BODY_DYNAMIC, box, 0.0);
    const de_body b = make_body(p, DE_BODY_STATIC, box, 3.0);
    EXPECT_EQ(de__shape2d_get(p, box)->refcount, 2u);
    EXPECT_EQ(de_shape2d_destroy(p, box), DE_ERR_BUSY);
    EXPECT_TRUE(de_shape2d_valid(p, box));
    de_body2d_destroy(p, a);
    EXPECT_EQ(de_shape2d_destroy(p, box), DE_ERR_BUSY);
    de_body2d_destroy(p, b);
    EXPECT_EQ(de__shape2d_get(p, box)->refcount, 0u);
    EXPECT_EQ(de_shape2d_destroy(p, box), DE_OK);
    EXPECT_EQ(p->shape_live_count, 0u);
}

/* ---- checksum --------------------------------------------------------------------------- */

static void build_scene(de_physics2d_world *p) {
    const de_shape box = make_box(p, 0.5f, 0.25f);
    const de_shape circ = make_circle(p, 0.3f);
    for (u32 k = 0; k < 12; ++k) {
        de_body2d_desc d;
        memset(&d, 0, sizeof d);
        d.type = (de_body_type)(k % 3u);
        d.shape = (k & 1u) ? box : circ;
        d.transform.x = (f64)k * 1.25;
        d.transform.y = -(f64)k * 0.5;
        d.transform.rotation = de_rot2d_from_angle(0.3f * (f32)k);
        d.linear_velocity = (de_vec2){(f32)k, 1.0f};
        d.angular_velocity = 0.1f * (f32)k;
        d.linear_damping = 0.01f * (f32)k;
        d.angular_damping = 0.02f;
        d.gravity_scale = 1.0f;
        d.material.friction = 0.5f;
        d.material.restitution = 0.1f;
        d.flags = k & 3u;
        d.owner.bits = 1000u + k;
        ASSERT_TRUE(DE_HANDLE_VALID(de_body2d_create(p, &d)));
    }
    de_body2d_destroy(p, de__body2d_handle(p, 5));
}

TEST_F(physics2d_world, checksum_sensitivity, p2_fx) {
    de_physics2d_world *p = f->p;
    build_scene(p);
    const u64 base = de_physics2d_checksum(p);
    EXPECT_NE(base, 0u);
    EXPECT_EQ(de_physics2d_checksum(p), base);
    EXPECT_EQ(de_physics2d_checksum(NULL), 0u);

    /* An identical run gives the same value. */
    de_physics2d_world *q;
    de_physics2d_desc d;
    memset(&d, 0, sizeof d);
    d.max_bodies = 64;
    ASSERT_EQ(de_physics2d_create(&d, &q), DE_OK);
    build_scene(q);
    EXPECT_EQ(de_physics2d_checksum(q), base);
    for (u32 s = 0; s < 5; ++s) {
        de_physics2d_step(p, 1.0f / 60.0f);
        de_physics2d_step(q, 1.0f / 60.0f);
        EXPECT_EQ(de_physics2d_checksum(p), de_physics2d_checksum(q));
    }
    EXPECT_NE(de_physics2d_checksum(p), base); /* the tick is part of it */
    de_physics2d_destroy(q);

    /* Flipping one bit of any 4-byte word of any field of one live body changes the checksum;
     * restoring it gives the old value back. Every array of the pool is covered. */
    const u64 cur = de_physics2d_checksum(p);
    de__body2d_pool *b = &p->bodies;
    const u32 i = 7;
    ASSERT_TRUE(b->alive[i] != 0);
    struct {
        void *base;
        size_t elem;
    } fields[] = {
        {b->generation, sizeof *b->generation},
        {b->type, sizeof *b->type},
        {b->flags, sizeof *b->flags},
        {b->position, sizeof *b->position},
        {b->rotation, sizeof *b->rotation},
        {b->linear_velocity, sizeof *b->linear_velocity},
        {b->angular_velocity, sizeof *b->angular_velocity},
        {b->mass, sizeof *b->mass},
        {b->inv_mass, sizeof *b->inv_mass},
        {b->inertia, sizeof *b->inertia},
        {b->inv_inertia, sizeof *b->inv_inertia},
        {b->local_com, sizeof *b->local_com},
        {b->linear_damping, sizeof *b->linear_damping},
        {b->angular_damping, sizeof *b->angular_damping},
        {b->gravity_scale, sizeof *b->gravity_scale},
        {b->shape, sizeof *b->shape},
        {b->filter, sizeof *b->filter},
        {b->material, sizeof *b->material},
        {b->owner, sizeof *b->owner},
        {b->world_aabb, sizeof *b->world_aabb},
    };
    for (u32 k = 0; k < sizeof fields / sizeof fields[0]; ++k) {
        u8 *elem = (u8 *)fields[k].base + fields[k].elem * i;
        for (size_t off = 0; off < fields[k].elem; off += 4) {
            elem[off] ^= 0x01u;
            EXPECT_NE(de_physics2d_checksum(p), cur);
            elem[off] ^= 0x01u;
            EXPECT_EQ(de_physics2d_checksum(p), cur);
        }
    }
    /* The high word of an f64 position too (not only its low bits). */
    b->position[i].y = -b->position[i].y - 1.0;
    EXPECT_NE(de_physics2d_checksum(p), cur);
    b->position[i].y = -(b->position[i].y + 1.0);
    EXPECT_EQ(de_physics2d_checksum(p), cur);
    /* Gravity, substeps and plane are part of it too; a dead slot is not. */
    de_physics2d_set_gravity(p, (de_vec2){0, -9.8f});
    EXPECT_NE(de_physics2d_checksum(p), cur);
    de_physics2d_set_gravity(p, (de_vec2){0, -9.81f});
    EXPECT_EQ(de_physics2d_checksum(p), cur);
    p->substeps = 5;
    EXPECT_NE(de_physics2d_checksum(p), cur);
    p->substeps = 4;
    p->plane = DE_PHYSICS2D_PLANE_TOP_DOWN;
    EXPECT_NE(de_physics2d_checksum(p), cur);
    p->plane = DE_PHYSICS2D_PLANE_SIDE;
    EXPECT_EQ(de_physics2d_checksum(p), cur);
    ASSERT_TRUE(b->alive[5] == 0);
    b->position[5].x += 1.0;
    EXPECT_EQ(de_physics2d_checksum(p), cur);
}

/* ---- step ------------------------------------------------------------------------------- */

TEST_F(physics2d_world, step_validates_and_refreshes, p2_fx) {
    de_physics2d_world *p = f->p;
    const de_shape box = make_box(p, 1, 1);
    const de_body b = make_body(p, DE_BODY_DYNAMIC, box, 0.0);
    const u32 i = body_index(b);
    de_physics2d_step(p, 0.0f);
    de_physics2d_step(p, -1.0f);
    de_physics2d_step(p, NAN);
    de_physics2d_step(p, INFINITY);
    EXPECT_EQ(de_physics2d_tick(p), 0u);
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    de_physics2d_step(NULL, 1.0f);

    /* A later solver writes positions directly; the step refreshes the box. */
    p->bodies.position[i].y = 50.0;
    de_physics2d_step(p, 1.0f / 60.0f);
    EXPECT_EQ(de_physics2d_tick(p), 1u);
    EXPECT_TRUE(p->last_dt == 1.0f / 60.0f);
    EXPECT_TRUE(p->bodies.world_aabb[i].min.y <= 49.0f && p->bodies.world_aabb[i].max.y >= 51.0f);
    EXPECT_TRUE(p->bodies.world_aabb[i].min.y > 48.9f);
}

TEST(physics2d_world, step_no_alloc) {
    de_tracking_allocator t;
    de_allocator a = de_tracking_allocator_init(&t, NULL, DE_MEM_PHYSICS);
    de_physics2d_desc d;
    memset(&d, 0, sizeof d);
    d.allocator = &a;
    d.max_bodies = 1024;
    de_physics2d_world *p;
    ASSERT_EQ(de_physics2d_create(&d, &p), DE_OK);
    const de_shape box = make_box(p, 0.5f, 0.5f);
    const de_vec2 pts[6] = {{-1, -1}, {1, -1}, {1.5f, 0}, {1, 1}, {-1, 1}, {-1.5f, 0}};
    de_shape2d_desc hd;
    memset(&hd, 0, sizeof hd);
    hd.kind = DE_SHAPE2D_POLYGON;
    hd.points = pts;
    hd.point_count = 6;
    hd.radius = 0.05f;
    const de_shape hex = de_shape2d_create(p, &hd);
    ASSERT_TRUE(DE_HANDLE_VALID(hex));
    const de_vec2 cpts[5] = {{0, 0}, {-2, 0}, {-4, 1}, {-6, 1}, {-8, 0}};
    const de_shape chain = de_shape2d_chain_create(p, cpts, 5, false);
    ASSERT_TRUE(DE_HANDLE_VALID(chain));
    for (u32 k = 0; k < 1000; ++k)
        (void)make_body(p, (de_body_type)(k % 3u), (k & 1u) ? box : hex, (f64)k);
    (void)make_body(p, DE_BODY_STATIC, chain, 0.0);
    /* DENG-253: the first step creates the broadphase and the contacts state (warm-up). */
    de_physics2d_step(p, 1.0f / 60.0f);
    const de_tracking_stats before = de_tracking_allocator_stats(&t);
    for (u32 s = 0; s < 120; ++s) {
        for (u32 k = 0; k < 1000; k += 7)
            p->bodies.position[k].y += 0.01;
        de_physics2d_step(p, 1.0f / 60.0f);
        (void)de_physics2d_checksum(p);
    }
    const de_tracking_stats after = de_tracking_allocator_stats(&t);
    EXPECT_EQ(after.alloc_count, before.alloc_count);
    EXPECT_EQ(after.realloc_count, before.realloc_count);
    EXPECT_EQ(after.free_count, before.free_count);
    /* Body create and destroy do not allocate either (preallocated pool). */
    const de_body extra = make_body(p, DE_BODY_DYNAMIC, box, 0.0);
    de_body2d_destroy(p, extra);
    EXPECT_EQ(de_tracking_allocator_stats(&t).alloc_count, before.alloc_count);
    de_physics2d_destroy(p);
    EXPECT_EQ(de_tracking_allocator_stats(&t).bytes_live, 0u);
}

/* ---- extension slots -------------------------------------------------------------------- */

typedef struct ext_log {
    u32 added[8], removed[8], n_added, n_removed, destroyed;
    bool bodies_alive_at_destroy;
} ext_log;

static void ext_destroy(de_physics2d_world *p, void *state) {
    ext_log *l = (ext_log *)state;
    ++l->destroyed;
    /* Called before bodies and shapes are freed. */
    l->bodies_alive_at_destroy = p->bodies.live_count > 0 && p->shape_live_count > 0;
}
static void ext_added(de_physics2d_world *p, void *state, u32 i) {
    EXPECT_TRUE(de__body2d_alive(p, i)); /* called after the body is created */
    ext_log *l = (ext_log *)state;
    l->added[l->n_added++ & 7u] = i;
}
static void ext_removed(de_physics2d_world *p, void *state, u32 i) {
    ext_log *l = (ext_log *)state;
    EXPECT_TRUE(de__body2d_alive(p, i)); /* called before the body is freed */
    l->removed[l->n_removed++ & 7u] = i;
}

TEST(physics2d_world, extension_slots) {
    de_physics2d_world *p;
    ASSERT_EQ(de_physics2d_create(NULL, &p), DE_OK);
    static const de__p2d_ext_vtbl vt = {ext_destroy, ext_added, ext_removed};
    ext_log log;
    memset(&log, 0, sizeof log);
    de__p2d_ext_set(p, DE__P2D_EXT_CONTACTS, &log, &vt);
    EXPECT_TRUE(de__p2d_ext_get(p, DE__P2D_EXT_CONTACTS) == &log);
    EXPECT_TRUE(de__p2d_ext_get(p, DE__P2D_EXT_SOLVER) == NULL);
    const de_shape box = make_box(p, 1, 1);
    const de_body a = make_body(p, DE_BODY_STATIC, box, 0.0);
    const de_body b = make_body(p, DE_BODY_DYNAMIC, box, 1.0);
    de_body2d_destroy(p, a);
    EXPECT_EQ(log.n_added, 2u);
    EXPECT_EQ(log.added[0], 0u);
    EXPECT_EQ(log.added[1], 1u);
    EXPECT_EQ(log.n_removed, 1u);
    EXPECT_EQ(log.removed[0], 0u);
    (void)b;
    de_physics2d_destroy(p);
    EXPECT_EQ(log.destroyed, 1u);
    EXPECT_TRUE(log.bodies_alive_at_destroy);
}
