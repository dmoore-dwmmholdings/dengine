/* test_world_bodies.c: physics world, body pool, handles, checksum, step and ECS attach
 * (DENG-132, SPEC §13.1, §13.2). */
#include "de_test.h"

#include <dengine/de_ecs.h>
#include <dengine/de_physics.h>

#include "physics/physics_internal.h"
#include "physics/shapes/shape_internal.h"

#include <math.h>
#include <string.h>

/* ---- Fixture: a world on a tracking allocator; teardown checks that nothing leaked --------- */

typedef struct pw_fx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_physics_world *p;
} pw_fx;

static void pw_fx_setup(pw_fx *f) {
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_PHYSICS);
    de_physics_desc d;
    memset(&d, 0, sizeof d);
    d.allocator = &f->alloc;
    d.max_bodies = 64;
    ASSERT_EQ(de_physics_create(&d, &f->p), DE_OK);
}

static void pw_fx_teardown(pw_fx *f) {
    de_physics_destroy(f->p);
    const de_tracking_stats s = de_tracking_allocator_stats(&f->track);
    EXPECT_EQ(s.bytes_live, 0u);
    EXPECT_EQ(s.blocks_live, 0u);
}

static de_shape make_box(de_physics_world *p, f32 x, f32 y, f32 z) {
    de_shape_desc d;
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_BOX;
    d.half_extents = (de_vec3){x, y, z};
    const de_shape s = de_shape_create(p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(s));
    return s;
}

static de_shape make_sphere(de_physics_world *p, f32 r) {
    de_shape_desc d;
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_SPHERE;
    d.radius = r;
    const de_shape s = de_shape_create(p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(s));
    return s;
}

static de_body make_body(de_physics_world *p, de_body_type type, de_shape s, f64 x) {
    de_body_desc d;
    memset(&d, 0, sizeof d);
    d.type = type;
    d.shape = s;
    d.transform.position = (de_dvec3){x, 0.0, 0.0};
    d.transform.rotation = (de_quat){0, 0, 0, 1};
    const de_body b = de_body_create(p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(b));
    return b;
}

static u32 body_index(de_body b) {
    return de__handle_index(b.id);
}

/* ---- create / destroy ------------------------------------------------------------------- */

TEST(physics_world, create_destroy_defaults) {
    de_tracking_allocator t;
    de_allocator a = de_tracking_allocator_init(&t, NULL, DE_MEM_PHYSICS);

    /* NULL desc and (desc){0} both give the defaults. */
    de_physics_world *p = NULL;
    ASSERT_EQ(de_physics_create(NULL, &p), DE_OK);
    ASSERT_TRUE(p != NULL);
    EXPECT_EQ(p->max_bodies, DE_PHYSICS_DEFAULT_MAX_BODIES);
    EXPECT_EQ(p->bodies.capacity, 65536u);
    EXPECT_EQ(p->substeps, 4u);
    const de_vec3 g = de_physics_get_gravity(p);
    EXPECT_TRUE(g.x == 0.0f && g.y == 0.0f && g.z == -9.81f);
    EXPECT_EQ(de_physics_tick(p), 0u);
    EXPECT_EQ(de_body_count(p), 0u);
    de_physics_destroy(p);

    de_physics_desc d;
    memset(&d, 0, sizeof d);
    d.allocator = &a;
    ASSERT_EQ(de_physics_create(&d, &p), DE_OK);
    EXPECT_EQ(p->max_bodies, 65536u);
    EXPECT_EQ(p->alloc, (const de_allocator *)&a);
    de_physics_destroy(p);
    EXPECT_EQ(de_tracking_allocator_stats(&t).bytes_live, 0u);

    /* Explicit values are kept; zero gravity is reachable through the setter. */
    d.gravity = (de_vec3){1, 2, 3};
    d.substeps = 8;
    d.max_bodies = 10;
    ASSERT_EQ(de_physics_create(&d, &p), DE_OK);
    EXPECT_EQ(p->substeps, 8u);
    EXPECT_EQ(p->bodies.capacity, 10u);
    EXPECT_TRUE(de_physics_get_gravity(p).y == 2.0f);
    de_physics_set_gravity(p, (de_vec3){0, 0, 0});
    EXPECT_TRUE(de_physics_get_gravity(p).z == 0.0f);
    de_physics_destroy(p);

    /* Refusals. */
    EXPECT_EQ(de_physics_create(&d, NULL), DE_ERR_INVALID_ARGUMENT);
    d.max_bodies = DE_PHYSICS_MAX_BODIES_LIMIT + 1u;
    p = (de_physics_world *)&d;
    EXPECT_EQ(de_physics_create(&d, &p), DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(p == NULL);
    d.max_bodies = 4;
    d.gravity.x = INFINITY;
    EXPECT_EQ(de_physics_create(&d, &p), DE_ERR_INVALID_ARGUMENT);
    d.gravity.x = 0.0f;

    /* Out of memory: nothing leaks. */
    for (i64 n = 0; n < 2; ++n) {
        de_tracking_allocator_fail_after(&t, n);
        EXPECT_EQ(de_physics_create(&d, &p), DE_ERR_OUT_OF_MEMORY);
        EXPECT_TRUE(p == NULL);
        de_tracking_allocator_fail_after(&t, -1);
    }
    EXPECT_EQ(de_tracking_allocator_stats(&t).bytes_live, 0u);
    de_physics_destroy(NULL);
}

/* ---- handles ---------------------------------------------------------------------------- */

TEST_F(physics_world, handle_generations, pw_fx) {
    de_physics_world *p = f->p;
    const de_shape box = make_box(p, 0.5f, 0.5f, 0.5f);
    const de_body b0 = make_body(p, DE_BODY_DYNAMIC, box, 0.0);
    EXPECT_TRUE(de_body_valid(p, b0));
    EXPECT_EQ(de__handle_gen(b0.id), 1u);
    EXPECT_EQ(body_index(b0), 0u);

    de_body_destroy(p, b0);
    EXPECT_FALSE(de_body_valid(p, b0));
    de_transform t;
    de_vec3 v;
    de_mass_props mp;
    EXPECT_FALSE(de_body_get_transform(p, b0, &t));
    EXPECT_FALSE(de_body_set_transform(p, b0, &t));
    EXPECT_FALSE(de_body_get_velocity(p, b0, &v, NULL));
    EXPECT_FALSE(de_body_set_velocity(p, b0, v, v));
    EXPECT_FALSE(de_body_get_mass_props(p, b0, &mp));
    de_body_destroy(p, b0); /* stale: ignored */
    EXPECT_EQ(de_body_count(p), 0u);

    /* The index comes back with the next generation; the old handle stays stale. */
    const de_body b1 = make_body(p, DE_BODY_DYNAMIC, box, 0.0);
    EXPECT_EQ(body_index(b1), 0u);
    EXPECT_EQ(de__handle_gen(b1.id), 2u);
    EXPECT_FALSE(de_body_valid(p, b0));
    EXPECT_TRUE(de_body_valid(p, b1));

    /* Invalid and out-of-range handles. */
    EXPECT_FALSE(de_body_valid(p, (de_body){0}));
    EXPECT_FALSE(de_body_valid(p, (de_body){de__handle_make(63, 1)}));
    EXPECT_FALSE(de_body_valid(p, (de_body){de__handle_make(1000, 1)}));
    EXPECT_FALSE(de_body_valid(NULL, b1));

    /* Generations wrap 4095 -> 1 (never 0). */
    de_body cur = b1;
    for (u32 i = 0; i < 4093; ++i) { /* generation 2 -> 4095 */
        de_body_destroy(p, cur);
        cur = make_body(p, DE_BODY_STATIC, box, 0.0);
        EXPECT_EQ(body_index(cur), 0u);
    }
    EXPECT_EQ(de__handle_gen(cur.id), 4095u);
    de_body_destroy(p, cur);
    cur = make_body(p, DE_BODY_STATIC, box, 0.0);
    EXPECT_EQ(de__handle_gen(cur.id), 1u);
    de_body_destroy(p, cur);

    /* Shapes: the same rules. */
    const de_shape s2 = make_sphere(p, 1.0f);
    ASSERT_EQ(de_shape_destroy(p, s2), DE_OK);
    EXPECT_FALSE(de_shape_valid(p, s2));
    EXPECT_EQ(de_shape_destroy(p, s2), DE_ERR_INVALID_ARGUMENT);
    de_aabb bb;
    de_shape_kind k;
    EXPECT_FALSE(de_shape_get_local_aabb(p, s2, &bb));
    EXPECT_FALSE(de_shape_get_kind(p, s2, &k));
    EXPECT_EQ(de_shape_get_mass_props(p, s2, 0.0f, &mp), DE_ERR_INVALID_ARGUMENT);
    const de_shape s3 = make_sphere(p, 1.0f);
    EXPECT_EQ(de__handle_index(s3.id), de__handle_index(s2.id));
    EXPECT_EQ(de__handle_gen(s3.id), de__handle_gen(s2.id) + 1u);
    EXPECT_FALSE(de_shape_valid(p, s2));
    EXPECT_TRUE(de_shape_valid(p, s3));
    /* A body cannot use a stale shape. */
    de_body_desc bd;
    memset(&bd, 0, sizeof bd);
    bd.shape = s2;
    EXPECT_FALSE(DE_HANDLE_VALID(de_body_create(p, &bd)));
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
}

TEST_F(physics_world, lifo_free_list_and_iteration_order, pw_fx) {
    de_physics_world *p = f->p;
    const de_shape s = make_sphere(p, 0.25f);
    de_body b[6];
    for (u32 i = 0; i < 6; ++i) {
        b[i] = make_body(p, DE_BODY_KINEMATIC, s, (f64)i);
        EXPECT_EQ(body_index(b[i]), i);
    }
    de_body_destroy(p, b[1]);
    de_body_destroy(p, b[4]);
    de_body_destroy(p, b[2]);
    /* Last freed, first reused: 2, then 4, then 1, then a new index 6. */
    const u32 expect[4] = {2, 4, 1, 6};
    for (u32 i = 0; i < 4; ++i)
        EXPECT_EQ(body_index(make_body(p, DE_BODY_DYNAMIC, s, 10.0 + i)), expect[i]);
    EXPECT_EQ(de_body_count(p), 7u);
    EXPECT_EQ(p->bodies.high_water, 7u);

    /* Iteration visits live bodies in ascending index. */
    de_body_destroy(p, b[3]);
    u32 seen[8], n = 0, prev = 0;
    for (u32 i = 0; i < p->bodies.high_water; ++i) {
        if (!p->bodies.alive[i])
            continue;
        if (n > 0)
            EXPECT_GT(i, prev);
        prev = i;
        seen[n++] = i;
    }
    EXPECT_EQ(n, 6u);
    const u32 want[6] = {0, 1, 2, 4, 5, 6};
    EXPECT_MEMEQ(seen, want, sizeof want);
}

TEST(physics_world, pool_full) {
    de_physics_desc d;
    memset(&d, 0, sizeof d);
    d.max_bodies = 3;
    de_physics_world *p;
    ASSERT_EQ(de_physics_create(&d, &p), DE_OK);
    de_body b[3];
    for (u32 i = 0; i < 3; ++i)
        b[i] = make_body(p, DE_BODY_STATIC, (de_shape){0}, 0.0);
    de_body_desc bd;
    memset(&bd, 0, sizeof bd);
    EXPECT_FALSE(DE_HANDLE_VALID(de_body_create(p, &bd)));
    EXPECT_EQ(de_error_last()->result, DE_ERR_OUT_OF_MEMORY);
    de_body_destroy(p, b[1]);
    EXPECT_TRUE(DE_HANDLE_VALID(de_body_create(p, &bd)));
    de_physics_destroy(p);
}

/* ---- body API --------------------------------------------------------------------------- */

TEST_F(physics_world, body_api, pw_fx) {
    de_physics_world *p = f->p;
    const de_shape box = make_box(p, 1.0f, 2.0f, 3.0f);

    /* (desc){0}: a static body at the origin without a shape. */
    de_body_desc d;
    memset(&d, 0, sizeof d);
    const de_body s0 = de_body_create(p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(s0));
    de_transform t;
    ASSERT_TRUE(de_body_get_transform(p, s0, &t));
    EXPECT_TRUE(t.rotation.w == 1.0f && t.rotation.x == 0.0f); /* zero quaternion -> identity */
    EXPECT_TRUE(t.scale.x == 1.0f);
    const u32 i0 = body_index(s0);
    EXPECT_EQ(p->bodies.filter[i0].layer, DE_COLLISION_FILTER_DEFAULT_LAYER);
    EXPECT_EQ(p->bodies.filter[i0].mask, DE_COLLISION_FILTER_DEFAULT_MASK);
    EXPECT_TRUE(p->bodies.material[i0].density == DE_PHYSICS_DEFAULT_DENSITY);
    EXPECT_TRUE(p->bodies.world_aabb[i0].min.x > p->bodies.world_aabb[i0].max.x); /* empty */

    /* A dynamic body: rotation normalized, mass from density, explicit filter kept. */
    d.type = DE_BODY_DYNAMIC;
    d.shape = box;
    d.transform.position = (de_dvec3){100000.5, -3.0, 2.0};
    d.transform.rotation = (de_quat){0.0f, 0.0f, 2.0f, 0.0f}; /* 180 deg about Z, unnormalized */
    d.linear_velocity = (de_vec3){1, 2, 3};
    d.material.density = 500.0f;
    d.filter = (de_collision_filter){4, 5, 6};
    d.flags = DE_BODY_FLAG_CCD | DE_BODY_FLAG_LOCK_ROT_X;
    const de_body b = de_body_create(p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(b));
    ASSERT_TRUE(de_body_get_transform(p, b, &t));
    EXPECT_TRUE(t.position.x == 100000.5);
    EXPECT_FLOAT_EQ(t.rotation.z, 1.0f, 0.0f);
    de_mass_props mp;
    ASSERT_TRUE(de_body_get_mass_props(p, b, &mp));
    EXPECT_FLOAT_EQ(mp.mass, 500.0f * 48.0f, 1e-3f);
    const u32 ib = body_index(b);
    EXPECT_EQ(p->bodies.filter[ib].group, 6u);
    EXPECT_EQ(p->bodies.flags[ib], (u32)(DE_BODY_FLAG_CCD | DE_BODY_FLAG_LOCK_ROT_X));
    EXPECT_FLOAT_EQ(p->bodies.inv_mass[ib], 1.0f / (500.0f * 48.0f), 1e-9f);
    /* World AABB of the rotated box at 100 km: x half extent 1, around 100000.5. */
    EXPECT_TRUE(p->bodies.world_aabb[ib].min.x <= 100000.5f - 1.0f);
    EXPECT_TRUE(p->bodies.world_aabb[ib].max.x >= 100000.5f + 1.0f);
    EXPECT_TRUE(p->bodies.world_aabb[ib].max.x <= 100000.5f + 1.02f);

    /* Mass override rescales the inertia. */
    d.mass = 10.0f;
    const de_body b2 = de_body_create(p, &d);
    de_mass_props m2;
    ASSERT_TRUE(de_body_get_mass_props(p, b2, &m2));
    EXPECT_FLOAT_EQ(m2.mass, 10.0f, 0.0f);
    EXPECT_FLOAT_EQ(m2.inertia.m[0], mp.inertia.m[0] * (10.0f / mp.mass), 1e-4f);
    d.mass = 0.0f;

    /* Velocities. */
    de_vec3 lin, ang;
    ASSERT_TRUE(de_body_get_velocity(p, b, &lin, &ang));
    EXPECT_TRUE(lin.y == 2.0f && ang.x == 0.0f);
    EXPECT_TRUE(de_body_set_velocity(p, b, (de_vec3){4, 5, 6}, (de_vec3){0, 0, 1}));
    ASSERT_TRUE(de_body_get_velocity(p, b, NULL, &ang));
    EXPECT_TRUE(ang.z == 1.0f);
    EXPECT_FALSE(de_body_set_velocity(p, b, (de_vec3){INFINITY, 0, 0}, ang));
    /* A static body keeps zero velocity. */
    EXPECT_TRUE(de_body_set_velocity(p, s0, (de_vec3){1, 1, 1}, (de_vec3){1, 1, 1}));
    ASSERT_TRUE(de_body_get_velocity(p, s0, &lin, &ang));
    EXPECT_TRUE(lin.x == 0.0f && ang.x == 0.0f);

    /* Teleport refreshes the AABB at once. */
    t.position = (de_dvec3){-5.0, 0.0, 0.0};
    t.rotation = (de_quat){0, 0, 0, 1};
    ASSERT_TRUE(de_body_set_transform(p, b, &t));
    EXPECT_TRUE(p->bodies.world_aabb[ib].min.x <= -6.0f && p->bodies.world_aabb[ib].max.x >= -4.0f);
    t.position.y = (f64)NAN;
    EXPECT_FALSE(de_body_set_transform(p, b, &t));

    /* Refusals. */
    de_body_desc bad = d;
    bad.type = (de_body_type)7;
    EXPECT_FALSE(DE_HANDLE_VALID(de_body_create(p, &bad)));
    bad = d;
    bad.shape = (de_shape){0};
    EXPECT_FALSE(DE_HANDLE_VALID(de_body_create(p, &bad))); /* dynamic without a shape */
    bad = d;
    bad.mass = -1.0f;
    EXPECT_FALSE(DE_HANDLE_VALID(de_body_create(p, &bad)));
    bad = d;
    bad.transform.position.z = (f64)INFINITY;
    EXPECT_FALSE(DE_HANDLE_VALID(de_body_create(p, &bad)));
    EXPECT_FALSE(DE_HANDLE_VALID(de_body_create(p, NULL)));
    EXPECT_FALSE(DE_HANDLE_VALID(de_body_create(NULL, &d)));

    /* Meshes are static-only: refused for a dynamic body, accepted for a kinematic one. */
    const de_vec3 verts[3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    const u32 idx[3] = {0, 1, 2};
    de_shape_desc md;
    memset(&md, 0, sizeof md);
    md.kind = DE_SHAPE_MESH;
    md.vertices = verts;
    md.vertex_count = 3;
    md.indices = idx;
    md.index_count = 3;
    const de_shape mesh = de_shape_create(p, &md);
    ASSERT_TRUE(DE_HANDLE_VALID(mesh));
    bad = d;
    bad.shape = mesh;
    EXPECT_FALSE(DE_HANDLE_VALID(de_body_create(p, &bad)));
    bad.type = DE_BODY_KINEMATIC;
    const de_body kin = de_body_create(p, &bad);
    ASSERT_TRUE(DE_HANDLE_VALID(kin));
    ASSERT_TRUE(de_body_get_mass_props(p, kin, &mp));
    EXPECT_TRUE(mp.mass == 0.0f);
}

TEST_F(physics_world, shape_refcount, pw_fx) {
    de_physics_world *p = f->p;
    const de_shape box = make_box(p, 1, 1, 1);
    const de_shape sph = make_sphere(p, 1);
    const de_body b = make_body(p, DE_BODY_DYNAMIC, box, 0.0);
    const de_body b2 = make_body(p, DE_BODY_STATIC, box, 3.0);
    EXPECT_EQ(de_shape_destroy(p, box), DE_ERR_BUSY);
    EXPECT_TRUE(de_shape_valid(p, box));
    de_body_destroy(p, b);
    EXPECT_EQ(de_shape_destroy(p, box), DE_ERR_BUSY);
    de_body_destroy(p, b2);

    /* A compound holds its children. */
    de_shape_desc cd;
    memset(&cd, 0, sizeof cd);
    cd.kind = DE_SHAPE_COMPOUND;
    const de_shape kids[2] = {box, sph};
    cd.children = kids;
    cd.child_count = 2;
    const de_shape comp = de_shape_create(p, &cd);
    ASSERT_TRUE(DE_HANDLE_VALID(comp));
    EXPECT_EQ(de_shape_destroy(p, box), DE_ERR_BUSY);
    EXPECT_EQ(de_shape_destroy(p, sph), DE_ERR_BUSY);
    ASSERT_EQ(de_shape_destroy(p, comp), DE_OK);
    EXPECT_EQ(de_shape_destroy(p, box), DE_OK);
    EXPECT_EQ(de_shape_destroy(p, sph), DE_OK);
    EXPECT_EQ(p->shape_live_count, 0u);
    /* Bodies and shapes left alive are freed by de_physics_destroy (teardown checks it). */
    const de_shape left = make_box(p, 2, 2, 2);
    (void)make_body(p, DE_BODY_DYNAMIC, left, 0.0);
    cd.children = &left;
    cd.child_count = 1;
    EXPECT_TRUE(DE_HANDLE_VALID(de_shape_create(p, &cd)));
}

/* ---- checksum --------------------------------------------------------------------------- */

static void build_scene(de_physics_world *p) {
    const de_shape box = make_box(p, 0.5f, 1.0f, 1.5f);
    const de_shape sph = make_sphere(p, 0.75f);
    for (u32 i = 0; i < 12; ++i) {
        de_body_desc d;
        memset(&d, 0, sizeof d);
        d.type = (de_body_type)(i % 3u);
        d.shape = (i & 1u) ? box : sph;
        d.transform.position = (de_dvec3){(f64)i * 1.5, 2.0 - (f64)i, 0.25 * (f64)i};
        d.transform.rotation = (de_quat){0.1f * (f32)i, 0.0f, 0.3f, 1.0f};
        d.linear_velocity = (de_vec3){(f32)i, 0.0f, -1.0f};
        d.owner.bits = 0x100000000ull + i;
        (void)make_body(p, DE_BODY_STATIC, (de_shape){0}, 0.0); /* interleave plain bodies */
        ASSERT_TRUE(DE_HANDLE_VALID(de_body_create(p, &d)));
    }
    for (u32 i = 0; i < 24; i += 5)
        de_body_destroy(p, de__body_handle(p, i));
}

TEST_F(physics_world, checksum_sensitivity, pw_fx) {
    de_physics_world *p = f->p;
    build_scene(p);
    const u64 base = de_physics_checksum(p);
    EXPECT_NE(base, 0u);
    EXPECT_EQ(de_physics_checksum(p), base);
    EXPECT_EQ(de_physics_checksum(NULL), 0u);

    /* An identical run gives the same value. */
    de_physics_world *q;
    de_physics_desc d;
    memset(&d, 0, sizeof d);
    d.max_bodies = 64;
    ASSERT_EQ(de_physics_create(&d, &q), DE_OK);
    build_scene(q);
    EXPECT_EQ(de_physics_checksum(q), base);
    for (u32 s = 0; s < 5; ++s) {
        de_physics_step(p, 1.0f / 60.0f);
        de_physics_step(q, 1.0f / 60.0f);
        EXPECT_EQ(de_physics_checksum(p), de_physics_checksum(q));
    }
    EXPECT_NE(de_physics_checksum(p), base); /* the tick is part of it */
    de_physics_destroy(q);

    /* Flipping one bit of any field of one live body changes the checksum; restoring it gives
     * the old value back. Every array of the pool is covered. */
    const u64 cur = de_physics_checksum(p);
    de__body_pool *b = &p->bodies;
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
        {b->local_com, sizeof *b->local_com},
        {b->local_inertia, sizeof *b->local_inertia},
        {b->local_inv_inertia, sizeof *b->local_inv_inertia},
        {b->linear_damping, sizeof *b->linear_damping},
        {b->angular_damping, sizeof *b->angular_damping},
        {b->gravity_scale, sizeof *b->gravity_scale},
        {b->shape, sizeof *b->shape},
        {b->filter, sizeof *b->filter},
        {b->material, sizeof *b->material},
        {b->owner, sizeof *b->owner},
        {b->world_aabb, sizeof *b->world_aabb},
        {b->friction_axis, sizeof *b->friction_axis}, /* DENG-217 */
        {b->friction_secondary, sizeof *b->friction_secondary},
    };
    for (u32 k = 0; k < sizeof fields / sizeof fields[0]; ++k) {
        u8 *elem = (u8 *)fields[k].base + fields[k].elem * i;
        /* Every 4-byte word of the element (de_quat has no padding; others are packed). */
        for (size_t off = 0; off < fields[k].elem; off += 4) {
            const size_t byte = off < fields[k].elem ? off : 0;
            elem[byte] ^= 0x01u;
            EXPECT_NE(de_physics_checksum(p), cur);
            elem[byte] ^= 0x01u;
            EXPECT_EQ(de_physics_checksum(p), cur);
        }
    }
    /* Gravity and substeps are part of it too; a dead slot is not. */
    de_physics_set_gravity(p, (de_vec3){0, 0, -9.8f});
    EXPECT_NE(de_physics_checksum(p), cur);
    de_physics_set_gravity(p, (de_vec3){0, 0, -9.81f});
    EXPECT_EQ(de_physics_checksum(p), cur);
    ASSERT_TRUE(b->alive[5] == 0);
    b->position[5].x += 1.0;
    EXPECT_EQ(de_physics_checksum(p), cur);
}

/* ---- step ------------------------------------------------------------------------------- */

TEST_F(physics_world, step_validates_and_refreshes, pw_fx) {
    de_physics_world *p = f->p;
    const de_shape box = make_box(p, 1, 1, 1);
    const de_body b = make_body(p, DE_BODY_DYNAMIC, box, 0.0);
    const u32 i = body_index(b);
    de_physics_step(p, 0.0f);
    de_physics_step(p, -1.0f);
    de_physics_step(p, NAN);
    de_physics_step(p, INFINITY);
    EXPECT_EQ(de_physics_tick(p), 0u);
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    de_physics_step(NULL, 1.0f);

    /* A later solver writes positions directly; the step refreshes the box. */
    p->bodies.position[i].z = 50.0;
    de_physics_step(p, 1.0f / 60.0f);
    EXPECT_EQ(de_physics_tick(p), 1u);
    EXPECT_TRUE(p->last_dt == 1.0f / 60.0f);
    EXPECT_TRUE(p->bodies.world_aabb[i].min.z <= 49.0f && p->bodies.world_aabb[i].max.z >= 51.0f);
    EXPECT_TRUE(p->bodies.world_aabb[i].min.z > 48.9f);
}

TEST(physics_world, step_no_alloc) {
    de_tracking_allocator t;
    de_allocator a = de_tracking_allocator_init(&t, NULL, DE_MEM_PHYSICS);
    de_physics_desc d;
    memset(&d, 0, sizeof d);
    d.allocator = &a;
    d.max_bodies = 1024;
    de_physics_world *p;
    ASSERT_EQ(de_physics_create(&d, &p), DE_OK);
    const de_shape box = make_box(p, 0.5f, 0.5f, 0.5f);
    const de_vec3 pts[8] = {{-1, -1, -1}, {1, -1, -1}, {-1, 1, -1}, {1, 1, -1},
                            {-1, -1, 1},  {1, -1, 1},  {-1, 1, 1},  {1, 1, 2}};
    de_shape_desc hd;
    memset(&hd, 0, sizeof hd);
    hd.kind = DE_SHAPE_CONVEX;
    hd.points = pts;
    hd.point_count = 8;
    const de_shape hull = de_shape_create(p, &hd);
    ASSERT_TRUE(DE_HANDLE_VALID(hull));
    for (u32 k = 0; k < 1000; ++k)
        (void)make_body(p, (de_body_type)(k % 3u), (k & 1u) ? box : hull, (f64)k);
    /* Warm-up: the first step creates the broadphase and the contacts state (DENG-135). */
    de_physics_step(p, 1.0f / 60.0f);
    const de_tracking_stats before = de_tracking_allocator_stats(&t);
    /* 30 steps: since DENG-135 every step collides the ~2000 overlapping pairs of this chain,
     * which is slow in Debug sanitizer builds (TSan); the allocation check needs no more. */
    for (u32 s = 0; s < 30; ++s) {
        for (u32 k = 0; k < 1000; k += 7)
            p->bodies.position[k].y += 0.01;
        de_physics_step(p, 1.0f / 60.0f);
        (void)de_physics_checksum(p);
    }
    const de_tracking_stats after = de_tracking_allocator_stats(&t);
    EXPECT_EQ(after.alloc_count, before.alloc_count);
    EXPECT_EQ(after.realloc_count, before.realloc_count);
    EXPECT_EQ(after.free_count, before.free_count);
    /* Body create and destroy do not allocate either (preallocated pool). */
    const de_body extra = make_body(p, DE_BODY_DYNAMIC, box, 0.0);
    de_body_destroy(p, extra);
    EXPECT_EQ(de_tracking_allocator_stats(&t).alloc_count, before.alloc_count);
    de_physics_destroy(p);
    EXPECT_EQ(de_tracking_allocator_stats(&t).bytes_live, 0u);
}

/* ---- extension slots -------------------------------------------------------------------- */

typedef struct ext_log {
    u32 added[8], removed[8], n_added, n_removed, destroyed;
} ext_log;

static void ext_destroy(de_physics_world *p, void *state) {
    (void)p;
    ++((ext_log *)state)->destroyed;
}
static void ext_added(de_physics_world *p, void *state, u32 i) {
    (void)p;
    ext_log *l = (ext_log *)state;
    l->added[l->n_added++ & 7u] = i;
}
static void ext_removed(de_physics_world *p, void *state, u32 i) {
    ext_log *l = (ext_log *)state;
    EXPECT_TRUE(de__body_alive(p, i)); /* called before the body is freed */
    l->removed[l->n_removed++ & 7u] = i;
}

TEST(physics_world, extension_slots) {
    de_physics_world *p;
    ASSERT_EQ(de_physics_create(NULL, &p), DE_OK);
    static const de__phys_ext_vtbl vt = {ext_destroy, ext_added, ext_removed};
    ext_log log;
    memset(&log, 0, sizeof log);
    de__phys_ext_set(p, DE__PHYS_EXT_BROADPHASE, &log, &vt);
    EXPECT_TRUE(de__phys_ext_get(p, DE__PHYS_EXT_BROADPHASE) == &log);
    const de_body a = make_body(p, DE_BODY_STATIC, (de_shape){0}, 0.0);
    const de_body b = make_body(p, DE_BODY_STATIC, (de_shape){0}, 0.0);
    de_body_destroy(p, a);
    EXPECT_EQ(log.n_added, 2u);
    EXPECT_EQ(log.added[1], body_index(b));
    EXPECT_EQ(log.n_removed, 1u);
    EXPECT_EQ(log.removed[0], body_index(a));
    de_physics_destroy(p);
    EXPECT_EQ(log.destroyed, 1u);
}

/* ---- ECS attach ------------------------------------------------------------------------- */

TEST(physics_world, attach_registers_components_and_resource) {
    de_tracking_allocator t;
    de_allocator a = de_tracking_allocator_init(&t, NULL, DE_MEM_ECS);
    de_type_registry *types;
    ASSERT_EQ(de_type_registry_create(NULL, &types), DE_OK);
    de_world_desc wd;
    memset(&wd, 0, sizeof wd);
    wd.allocator = &a;
    wd.types = types;
    de_world *w;
    ASSERT_EQ(de_world_create(&wd, &w), DE_OK);
    de_physics_world *p;
    ASSERT_EQ(de_physics_create(NULL, &p), DE_OK);

    EXPECT_EQ(de_physics_attach(NULL, w, types), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_physics_attach(p, NULL, types), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_physics_attach(p, w, NULL), DE_ERR_INVALID_ARGUMENT);
    ASSERT_EQ(de_physics_attach(p, w, types), DE_OK);

    const de_component rb = de_component_find(w, "DeRigidBody");
    const de_component co = de_component_find(w, "DeCollider");
    const de_component ch = de_component_find(w, "DeCharacter");
    ASSERT_TRUE(DE_HANDLE_VALID(rb) && DE_HANDLE_VALID(co) && DE_HANDLE_VALID(ch));
    EXPECT_EQ(rb.id, p->comp_rigid_body.id);
    EXPECT_EQ(co.id, p->comp_collider.id);
    EXPECT_EQ(ch.id, p->comp_character.id);
    /* Registered in a fixed order: dense ids 1, 2, 3 in a fresh world. */
    EXPECT_EQ(rb.id, 1u);
    EXPECT_EQ(co.id, 2u);
    EXPECT_EQ(ch.id, 3u);

    /* Reflection: field types are there, with the expected fields. */
    u32 nf = 0;
    const de_type_id t_rb = de_type_find(types, "DeRigidBody");
    const de_field_info *fi = de_type_fields(types, t_rb, &nf);
    ASSERT_TRUE(fi != NULL);
    EXPECT_EQ(nf, 13u);
    EXPECT_STREQ(fi[0].name, "type");
    EXPECT_EQ((u32)fi[0].kind, (u32)DE_FK_ENUM);
    EXPECT_EQ(fi[12].flags & DE_FIELD_SERIALIZE, 0u); /* the body handle is not serialized */
    EXPECT_TRUE((fi[12].flags & DE_FIELD_SNAPSHOT) != 0);
    EXPECT_TRUE(DE_HANDLE_VALID(de_type_find(types, "de_transform")));
    EXPECT_TRUE(DE_HANDLE_VALID(de_type_find(types, "de_material_phys")));
    EXPECT_TRUE(DE_HANDLE_VALID(de_type_find(types, "de_collision_filter")));
    EXPECT_TRUE(DE_HANDLE_VALID(de_type_find(types, "de_body_type")));
    i64 v = -1;
    EXPECT_TRUE(
        de_enum_value_of(types, de_type_find(types, "de_body_type"), "DE_BODY_DYNAMIC", &v));
    EXPECT_EQ(v, (i64)DE_BODY_DYNAMIC);

    /* The resource points to the physics world. */
    const de_resource_id res = de_resource_find(w, de_type_find(types, "DePhysicsWorld"));
    ASSERT_TRUE(DE_HANDLE_VALID(res));
    const DePhysicsWorld *pw = (const DePhysicsWorld *)de_resource_get(w, res);
    ASSERT_TRUE(pw != NULL);
    EXPECT_TRUE(pw->world == p);

    /* The components work as components. */
    const de_component comps[3] = {rb, co, ch};
    de_entity e;
    ASSERT_EQ(de_entity_spawn_with(w, comps, 3, &e), DE_OK);
    DeRigidBody *r = (DeRigidBody *)de_entity_get(w, e, rb);
    ASSERT_TRUE(r != NULL);
    EXPECT_EQ((u32)r->type, (u32)DE_BODY_STATIC);
    r->mass = 3.0f;
    EXPECT_TRUE(((const DeRigidBody *)de_entity_get_const(w, e, rb))->mass == 3.0f);

    /* Attaching again is harmless: same ids, same resource. */
    ASSERT_EQ(de_physics_attach(p, w, types), DE_OK);
    EXPECT_EQ(de_component_find(w, "DeRigidBody").id, rb.id);
    EXPECT_EQ(p->res_physics_world.id, res.id);

    de_world_destroy(w);
    de_physics_destroy(p);
    de_type_registry_destroy(types);
    EXPECT_EQ(de_tracking_allocator_stats(&t).bytes_live, 0u);
}
