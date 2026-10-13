/* test_broadphase.c: the physics broadphase (DENG-133, SPEC §13.1, §13.2): pairs against brute
 * force, order independence, filters, body-type rules, no allocation after warm-up, overflow,
 * queries, and the checked-in pair-list vector.
 *
 * physics_broadphase.pair_vector_golden hashes the sorted pair list of a 2,000-body de_rng scene
 * over 300 ticks of scripted motion and compares with tests/unit/physics/vectors/
 * broadphase_golden.txt. x86-64 and arm64 must reproduce it bit for bit. Regenerate (never by
 * hand, only for a deliberate change of the scene or of the world AABBs) with
 *     DE_REGEN_GOLDEN=1 build/dev/tests/unit/physics/test_physics_broadphase
 * --filter=physics_broadphase.pair_vector_golden */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* getenv, fopen */
#endif

#include "de_test.h"

#include <dengine/de_debug.h>
#include <dengine/de_physics.h>
#include <dengine/de_platform.h>

#include "physics/broadphase/broadphase.h"
#include "physics/physics_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DE_PHYSICS_BROADPHASE_GOLDEN
#error "DE_PHYSICS_BROADPHASE_GOLDEN (path of broadphase_golden.txt) must be defined by the build"
#endif

/* ---- helpers ------------------------------------------------------------------------------ */

static de_physics_world *world_new(const de_allocator *a, u32 max_bodies) {
    de_physics_desc d;
    memset(&d, 0, sizeof d);
    d.allocator = a;
    d.max_bodies = max_bodies;
    de_physics_world *p = NULL;
    ASSERT_EQ(de_physics_create(&d, &p), DE_OK);
    return p;
}

static de_shape sphere(de_physics_world *p, f32 r) {
    de_shape_desc d;
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_SPHERE;
    d.radius = r;
    const de_shape s = de_shape_create(p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(s));
    return s;
}

static de_shape box(de_physics_world *p, f32 x, f32 y, f32 z) {
    de_shape_desc d;
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_BOX;
    d.half_extents = (de_vec3){x, y, z};
    const de_shape s = de_shape_create(p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(s));
    return s;
}

static de_shape capsule(de_physics_world *p, f32 r, f32 hh) {
    de_shape_desc d;
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_CAPSULE;
    d.radius = r;
    d.half_height = hh;
    const de_shape s = de_shape_create(p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(s));
    return s;
}

static de_body_desc body_desc(de_body_type type, de_shape s, f64 x, f64 y, f64 z) {
    de_body_desc d;
    memset(&d, 0, sizeof d);
    d.type = type;
    d.shape = s;
    d.transform.position = (de_dvec3){x, y, z};
    d.transform.rotation = (de_quat){0, 0, 0, 1};
    d.transform.scale = (de_vec3){1, 1, 1};
    return d;
}

static de_body add(de_physics_world *p, const de_body_desc *d) {
    const de_body b = de_body_create(p, d);
    ASSERT_TRUE(DE_HANDLE_VALID(b));
    return b;
}

static u32 idx(de_body b) {
    return de__handle_index(b.id);
}

static void move_to(de_physics_world *p, de_body b, f64 x, f64 y, f64 z) {
    de_transform t;
    ASSERT_TRUE(de_body_get_transform(p, b, &t));
    t.position = (de_dvec3){x, y, z};
    ASSERT_TRUE(de_body_set_transform(p, b, &t));
}

/* Every pair the broadphase must report, by brute force over the world AABBs. */
static u32 brute_pairs(const de_physics_world *p, de__bp_pair *out, u32 max) {
    const de__body_pool *b = &p->bodies;
    u32 n = 0;
    for (u32 i = 0; i < b->high_water; ++i) {
        if (!b->alive[i] || b->world_aabb[i].min.x > b->world_aabb[i].max.x)
            continue;
        for (u32 j = i + 1; j < b->high_water; ++j) {
            if (!b->alive[j] || b->world_aabb[j].min.x > b->world_aabb[j].max.x)
                continue;
            if (!de__bp_should_collide(p, i, j) ||
                !de__aabb_overlap(&b->world_aabb[i], &b->world_aabb[j]))
                continue;
            if (n < max) {
                out[n].a = i;
                out[n].b = j;
            }
            ++n;
        }
    }
    return n;
}

#define MAX_TEST_PAIRS 65536u
static de__bp_pair g_brute[MAX_TEST_PAIRS];

/* Update, then compare the pair list with brute force; returns the pair count. */
static u32 update_and_check(de_physics_world *p) {
    ASSERT_EQ(de__broadphase_update(p), DE_OK);
    EXPECT_TRUE(de__broadphase_validate(p));
    const de__bp_pair *pairs;
    u32 count;
    de__broadphase_pairs(p, &pairs, &count);
    const u32 nb = brute_pairs(p, g_brute, MAX_TEST_PAIRS);
    ASSERT_LE(nb, MAX_TEST_PAIRS);
    ASSERT_EQ(count, nb);
    if (count > 0)
        ASSERT_MEMEQ(pairs, g_brute, sizeof(de__bp_pair) * count);
    return count;
}

static bool has_pair(const de_physics_world *p, u32 a, u32 b) {
    const de__bp_pair *pairs;
    u32 count;
    de__broadphase_pairs(p, &pairs, &count);
    for (u32 i = 0; i < count; ++i)
        if (pairs[i].a == a && pairs[i].b == b)
            return true;
    return false;
}

/* ---- Random scene with scripted motion (pairs vs brute force, golden, no-alloc, bench) ------- */

#define SCENE_MAX 10000u

typedef struct scene {
    de_physics_world *p;
    de_shape shapes[4];
    de_body body[SCENE_MAX];
    de_dvec3 vel[SCENE_MAX];
    u32 n;
    f64 extent;
    de_rng rng;
    bool all_dynamic;
} scene;

static de_body_desc scene_desc(scene *s, u32 k) {
    de_rng *r = &s->rng;
    const f32 t = de_rng_f32(r);
    de_body_type type = DE_BODY_DYNAMIC;
    if (!s->all_dynamic)
        type = t < 0.1f ? DE_BODY_STATIC : t < 0.2f ? DE_BODY_KINEMATIC : DE_BODY_DYNAMIC;
    const de_shape sh = s->shapes[de_rng_range(r, 0, 4)];
    const f64 e = s->extent;
    /* One draw per statement: the evaluation order of function arguments and of initializer
     * lists is unspecified in C (GCC and MSVC differ from Clang), and the golden vector must not
     * depend on the compiler. */
    const f64 px = (de_rng_f64(r) * 2.0 - 1.0) * e;
    const f64 py = (de_rng_f64(r) * 2.0 - 1.0) * e;
    const f64 pz = (de_rng_f64(r) * 2.0 - 1.0) * e;
    de_body_desc d = body_desc(type, sh, px, py, pz);
    de_quat q;
    q.x = de_rng_f32(r) - 0.5f;
    q.y = de_rng_f32(r) - 0.5f;
    q.z = de_rng_f32(r) - 0.5f;
    q.w = de_rng_f32(r) - 0.5f;
    d.transform.rotation = de_dnormalize_quat(q);
    d.filter.layer = 1u << de_rng_range(r, 0, 4);
    d.filter.mask = de_rng_f32(r) < 0.8f ? 0xFFFFFFFFu : de_rng_u32(r) | 1u;
    d.filter.group = de_rng_f32(r) < 0.1f ? 1u + de_rng_range(r, 0, 4) : 0u;
    s->vel[k] = (de_dvec3){0, 0, 0};
    if (type != DE_BODY_STATIC) {
        s->vel[k].x = (de_rng_f64(r) - 0.5) * 6.0;
        s->vel[k].y = (de_rng_f64(r) - 0.5) * 6.0;
        s->vel[k].z = (de_rng_f64(r) - 0.5) * 6.0;
    }
    return d;
}

static void scene_init(scene *s, de_physics_world *p, u32 n, f64 extent, u64 seed,
                       bool all_dynamic) {
    memset(s, 0, sizeof *s);
    s->all_dynamic = all_dynamic;
    s->p = p;
    s->n = n;
    s->extent = extent;
    s->rng = de_rng_seed(seed, 133);
    s->shapes[0] = sphere(p, 0.5f);
    s->shapes[1] = box(p, 0.4f, 0.6f, 0.3f);
    s->shapes[2] = capsule(p, 0.3f, 0.5f);
    s->shapes[3] = sphere(p, 1.1f);
    for (u32 k = 0; k < n; ++k) {
        const de_body_desc d = scene_desc(s, k);
        s->body[k] = add(p, &d);
    }
}

static f64 reflect(f64 x, f64 *v, f64 e) {
    if (x > e) {
        *v = -*v;
        return e - (x - e);
    }
    if (x < -e) {
        *v = -*v;
        return -e + (-e - x);
    }
    return x;
}

/* One tick of scripted motion (f64 positions, bounce at the walls) and, every `churn_every`
 * ticks, `churn` bodies destroyed and recreated (exercises index reuse). */
static void scene_tick(scene *s, u32 tick, u32 churn_every, u32 churn) {
    const f64 dt = 1.0 / 60.0;
    for (u32 k = 0; k < s->n; ++k) {
        de_transform t;
        if (!de_body_get_transform(s->p, s->body[k], &t))
            continue;
        de_dvec3 *v = &s->vel[k];
        if (v->x == 0.0 && v->y == 0.0 && v->z == 0.0)
            continue;
        t.position.x = reflect(t.position.x + v->x * dt, &v->x, s->extent);
        t.position.y = reflect(t.position.y + v->y * dt, &v->y, s->extent);
        t.position.z = reflect(t.position.z + v->z * dt, &v->z, s->extent);
        ASSERT_TRUE(de_body_set_transform(s->p, s->body[k], &t));
    }
    if (churn_every > 0 && tick % churn_every == 0) {
        for (u32 c = 0; c < churn; ++c) {
            const u32 k = de_rng_range(&s->rng, 0, s->n);
            de_body_destroy(s->p, s->body[k]);
        }
        for (u32 k = 0; k < s->n; ++k) {
            if (de_body_valid(s->p, s->body[k]))
                continue;
            const de_body_desc d = scene_desc(s, k);
            s->body[k] = add(s->p, &d);
        }
    }
}

/* ---- rules -------------------------------------------------------------------------------- */

TEST(physics_broadphase, filter_rule) {
    const de_collision_filter all = {1u, 0xFFFFFFFFu, 0u};
    de_collision_filter a = all, b = all;
    EXPECT_TRUE(de__collision_filter_test(a, b));
    b.mask = 0xFFFFFFFEu; /* b ignores layer 1 */
    EXPECT_FALSE(de__collision_filter_test(a, b));
    EXPECT_FALSE(de__collision_filter_test(b, a)); /* symmetric */
    b = all;
    b.layer = 2u;
    a.mask = 1u; /* a accepts only layer 1 */
    EXPECT_FALSE(de__collision_filter_test(a, b));
    a = all;
    b = all;
    a.group = b.group = 3u; /* same non-zero group */
    EXPECT_FALSE(de__collision_filter_test(a, b));
    b.group = 4u;
    EXPECT_TRUE(de__collision_filter_test(a, b));
    a.group = b.group = 0u; /* group 0 is no group */
    EXPECT_TRUE(de__collision_filter_test(a, b));

    EXPECT_FALSE(de__bp_types_collide(DE_BODY_STATIC, DE_BODY_STATIC));
    EXPECT_FALSE(de__bp_types_collide(DE_BODY_KINEMATIC, DE_BODY_KINEMATIC));
    EXPECT_FALSE(de__bp_types_collide(DE_BODY_KINEMATIC, DE_BODY_STATIC));
    EXPECT_FALSE(de__bp_types_collide(DE_BODY_STATIC, DE_BODY_KINEMATIC));
    EXPECT_TRUE(de__bp_types_collide(DE_BODY_DYNAMIC, DE_BODY_STATIC));
    EXPECT_TRUE(de__bp_types_collide(DE_BODY_KINEMATIC, DE_BODY_DYNAMIC));
    EXPECT_TRUE(de__bp_types_collide(DE_BODY_DYNAMIC, DE_BODY_DYNAMIC));
}

TEST(physics_broadphase, test_filter_layer_mask_group) {
    de_physics_world *p = world_new(NULL, 64);
    const de_shape s = sphere(p, 1.0f);
    const de_collision_filter f[6] = {
        {1u, 0xFFFFFFFFu, 0u}, /* 0 */
        {2u, 0xFFFFFFFEu, 0u}, /* 1: ignores layer 1 */
        {4u, 1u, 0u},          /* 2: accepts only layer 1 */
        {1u, 0xFFFFFFFFu, 7u}, /* 3: group 7 */
        {1u, 0xFFFFFFFFu, 7u}, /* 4: group 7 */
        {1u, 0xFFFFFFFFu, 8u}, /* 5: group 8 */
    };
    for (u32 i = 0; i < 6; ++i) {
        de_body_desc d = body_desc(DE_BODY_DYNAMIC, s, 0.1 * (f64)i, 0, 0);
        d.filter = f[i];
        (void)add(p, &d);
    }
    const u32 n = update_and_check(p);
    static const u32 expect[][2] = {{0, 2}, {0, 3}, {0, 4}, {0, 5}, {2, 3},
                                    {2, 4}, {2, 5}, {3, 5}, {4, 5}};
    ASSERT_EQ(n, 9u);
    const de__bp_pair *pairs;
    u32 count;
    de__broadphase_pairs(p, &pairs, &count);
    for (u32 i = 0; i < 9; ++i) {
        EXPECT_EQ(pairs[i].a, expect[i][0]);
        EXPECT_EQ(pairs[i].b, expect[i][1]);
    }
    de_physics_destroy(p);
}

TEST(physics_broadphase, test_static_static_skipped) {
    de_physics_world *p = world_new(NULL, 64);
    const de_shape s = sphere(p, 1.0f);
    const de_body_type types[6] = {DE_BODY_STATIC,    DE_BODY_STATIC,  DE_BODY_KINEMATIC,
                                   DE_BODY_KINEMATIC, DE_BODY_DYNAMIC, DE_BODY_DYNAMIC};
    de_body b[6];
    for (u32 i = 0; i < 6; ++i) {
        const de_body_desc d = body_desc(types[i], s, 0.2 * (f64)i, 0, 0);
        b[i] = add(p, &d);
    }
    ASSERT_EQ(update_and_check(p), 9u);
    for (u32 i = 0; i < 4; ++i)
        for (u32 j = i + 1; j < 4; ++j)
            EXPECT_FALSE(has_pair(p, i, j));
    for (u32 i = 0; i < 4; ++i) {
        EXPECT_TRUE(has_pair(p, i, 4));
        EXPECT_TRUE(has_pair(p, i, 5));
    }
    EXPECT_TRUE(has_pair(p, 4, 5));

    de__bp_stats st;
    de__broadphase_stats(p, &st);
    EXPECT_EQ(st.proxies[DE__BP_TREE_STATIC], 2u);
    EXPECT_EQ(st.proxies[DE__BP_TREE_DYNAMIC], 4u);

    /* Moving a static or kinematic body onto another never makes a pair either. */
    move_to(p, b[1], 0.0, 0.0, 0.0);
    move_to(p, b[3], 0.0, 0.0, 0.0);
    move_to(p, b[4], 50.0, 0.0, 0.0);
    move_to(p, b[5], 60.0, 0.0, 0.0);
    EXPECT_EQ(update_and_check(p), 0u);
    de_physics_destroy(p);
}

/* ---- pairs against brute force ------------------------------------------------------------ */

TEST(physics_broadphase, test_pairs_vs_bruteforce) {
    static scene s;
    for (u32 seed = 1; seed <= 3; ++seed) {
        de_physics_world *p = world_new(NULL, 1024);
        scene_init(&s, p, 600, 9.0 + 3.0 * (f64)seed, seed, false);
        u32 total = 0;
        for (u32 tick = 0; tick < 60; ++tick) {
            total += update_and_check(p);
            scene_tick(&s, tick, 7, 9);
        }
        total += update_and_check(p);
        de__bp_stats st;
        de__broadphase_stats(p, &st);
        printf("  seed %u: %u pairs over 61 updates, %u candidates now, heights %u/%u\n",
               (unsigned)seed, (unsigned)total, (unsigned)st.candidates, (unsigned)st.height[0],
               (unsigned)st.height[1]);
        EXPECT_GT(total, 600u);
        de_physics_destroy(p);
    }
}

TEST(physics_broadphase, shapeless_bodies_have_no_proxy) {
    de_physics_world *p = world_new(NULL, 16);
    const de_shape s = sphere(p, 1.0f);
    de_body_desc d = body_desc(DE_BODY_STATIC, (de_shape){0}, 0, 0, 0);
    const de_body none = add(p, &d);
    d = body_desc(DE_BODY_KINEMATIC, (de_shape){0}, 0, 0, 0);
    (void)add(p, &d);
    d = body_desc(DE_BODY_DYNAMIC, s, 0, 0, 0);
    (void)add(p, &d);
    EXPECT_EQ(update_and_check(p), 0u);
    EXPECT_EQ(de__broadphase_proxy(p, idx(none)), DE__TREE_NULL);
    de__bp_stats st;
    de__broadphase_stats(p, &st);
    EXPECT_EQ(st.proxies[0] + st.proxies[1], 1u);
    de_physics_destroy(p);
}

TEST(physics_broadphase, destroy_and_index_reuse) {
    de_physics_world *p = world_new(NULL, 16);
    const de_shape s = sphere(p, 1.0f);
    de_body_desc d = body_desc(DE_BODY_DYNAMIC, s, 0, 0, 0);
    de_body a = add(p, &d);
    d = body_desc(DE_BODY_STATIC, s, 0.5, 0, 0);
    const de_body b = add(p, &d);
    ASSERT_EQ(update_and_check(p), 1u);
    EXPECT_TRUE(has_pair(p, idx(a), idx(b)));

    /* The same index comes back as a static body: the old candidate must not survive. */
    const u32 ia = idx(a);
    de_body_destroy(p, a);
    EXPECT_EQ(de__broadphase_proxy(p, ia), DE__TREE_NULL);
    EXPECT_TRUE(de__broadphase_validate(p));
    d = body_desc(DE_BODY_STATIC, s, 0, 0, 0);
    a = add(p, &d);
    ASSERT_EQ(idx(a), ia);
    EXPECT_EQ(update_and_check(p), 0u);
    de_body_destroy(p, a);
    d = body_desc(DE_BODY_KINEMATIC, s, 0, 0, 0);
    a = add(p, &d);
    EXPECT_EQ(update_and_check(p), 0u);
    de_body_destroy(p, a);
    d = body_desc(DE_BODY_DYNAMIC, s, 0, 0, 0);
    a = add(p, &d);
    EXPECT_EQ(update_and_check(p), 1u);
    /* Destroyed between updates without replacement. */
    de_body_destroy(p, a);
    EXPECT_EQ(update_and_check(p), 0u);
    de_physics_destroy(p);
}

/* ---- order independence ------------------------------------------------------------------- */

#define ORDER_N 400u

static void order_descs(de_physics_world *p, de_body_desc *out) {
    de_rng r = de_rng_seed(4242, 1);
    const de_shape sh[3] = {sphere(p, 0.6f), box(p, 0.5f, 0.3f, 0.8f), capsule(p, 0.25f, 0.6f)};
    for (u32 i = 0; i < ORDER_N; ++i) {
        const f32 t = de_rng_f32(&r);
        const de_body_type type = t < 0.15f  ? DE_BODY_STATIC
                                  : t < 0.3f ? DE_BODY_KINEMATIC
                                             : DE_BODY_DYNAMIC;
        /* One draw per statement (argument evaluation order is unspecified). */
        const de_shape shape = sh[de_rng_range(&r, 0, 3)];
        const f64 px = (de_rng_f64(&r) - 0.5) * 16.0;
        const f64 py = (de_rng_f64(&r) - 0.5) * 16.0;
        const f64 pz = (de_rng_f64(&r) - 0.5) * 16.0;
        out[i] = body_desc(type, shape, px, py, pz);
        de_quat q = {0.0f, 0.0f, 0.3f, 1.0f};
        q.x = de_rng_f32(&r);
        q.y = de_rng_f32(&r) - 0.5f;
        out[i].transform.rotation = de_dnormalize_quat(q);
        out[i].filter.layer = 1u << de_rng_range(&r, 0, 3);
        out[i].filter.mask = de_rng_f32(&r) < 0.7f ? 0xFFFFFFFFu : 0x5u;
        out[i].filter.group = de_rng_f32(&r) < 0.1f ? 2u : 0u;
    }
}

TEST(physics_broadphase, test_order_independent_of_insertion) {
    static de_body_desc desc[ORDER_N];
    static de__bp_pair first[MAX_TEST_PAIRS];

    /* A: broadphase first, bodies inserted in index order at their final place. */
    de_physics_world *pa = world_new(NULL, 1024);
    ASSERT_EQ(de__broadphase_create(pa, NULL), DE_OK);
    order_descs(pa, desc);
    for (u32 i = 0; i < ORDER_N; ++i)
        ASSERT_EQ(idx(add(pa, &desc[i])), i);
    const u32 na = update_and_check(pa);
    const de__bp_pair *pairs;
    u32 count;
    de__broadphase_pairs(pa, &pairs, &count);
    memcpy(first, pairs, sizeof(de__bp_pair) * count);
    EXPECT_GT(na, 50u);

    /* B: another margin; the same indices handed out in a shuffled order through the LIFO free
     * list; every body starts elsewhere and wanders through 20 updates before reaching the same
     * final place. */
    de_physics_world *pb = world_new(NULL, 1024);
    de__broadphase_desc bd = {0.5f, 0u};
    ASSERT_EQ(de__broadphase_create(pb, &bd), DE_OK);
    order_descs(pb, desc);
    static de_body tmp[ORDER_N];
    static u32 perm[ORDER_N];
    for (u32 i = 0; i < ORDER_N; ++i) {
        const de_body_desc d = body_desc(DE_BODY_DYNAMIC, desc[0].shape, 3.0 * (f64)i, 0, 0);
        tmp[i] = add(pb, &d);
        perm[i] = i;
    }
    de_rng r = de_rng_seed(77, 77);
    for (u32 i = ORDER_N - 1u; i > 0; --i) {
        const u32 j = de_rng_range(&r, 0, i + 1u);
        const u32 t = perm[i];
        perm[i] = perm[j];
        perm[j] = t;
    }
    ASSERT_EQ(de__broadphase_update(pb), DE_OK);
    for (u32 i = 0; i < ORDER_N; ++i)
        de_body_destroy(pb, tmp[perm[i]]);
    static de_body bb[ORDER_N];
    for (u32 k = 0; k < ORDER_N; ++k) {
        const u32 want = perm[ORDER_N - 1u - k]; /* LIFO: the last destroyed comes back first */
        de_body_desc d = desc[want];
        d.transform.position.x += 30.0;
        bb[want] = add(pb, &d);
        ASSERT_EQ(idx(bb[want]), want);
    }
    for (u32 step = 0; step < 20; ++step) {
        for (u32 i = 0; i < ORDER_N; ++i) {
            const f64 s = (f64)(20u - step) / 20.0;
            move_to(pb, bb[i], desc[i].transform.position.x + 30.0 * s,
                    desc[i].transform.position.y + (de_rng_f64(&r) - 0.5) * 4.0 * s,
                    desc[i].transform.position.z);
        }
        (void)update_and_check(pb);
    }
    for (u32 i = 0; i < ORDER_N; ++i)
        move_to(pb, bb[i], desc[i].transform.position.x, desc[i].transform.position.y,
                desc[i].transform.position.z);
    ASSERT_EQ(update_and_check(pb), na);
    de__broadphase_pairs(pb, &pairs, &count);
    EXPECT_MEMEQ(pairs, first, sizeof(de__bp_pair) * na);

    /* C: bodies first, broadphase created lazily by the first update (ascending insert). */
    de_physics_world *pc = world_new(NULL, 1024);
    order_descs(pc, desc);
    for (u32 i = 0; i < ORDER_N; ++i)
        (void)add(pc, &desc[i]);
    EXPECT_TRUE(de__broadphase_get(pc) == NULL);
    ASSERT_EQ(update_and_check(pc), na);
    de__broadphase_pairs(pc, &pairs, &count);
    EXPECT_MEMEQ(pairs, first, sizeof(de__bp_pair) * na);

    de_physics_destroy(pa);
    de_physics_destroy(pb);
    de_physics_destroy(pc);
}

/* ---- lifetime, settings, errors ---------------------------------------------------------- */

TEST(physics_broadphase, create_errors_and_settings) {
    de_physics_world *p = world_new(NULL, 100);
    de__broadphase_desc d = {-1.0f, 0u};
    EXPECT_EQ(de__broadphase_create(p, &d), DE_ERR_INVALID_ARGUMENT);
    const u32 nan_bits = 0x7FC00000u;
    memcpy(&d.margin, &nan_bits, sizeof d.margin);
    EXPECT_EQ(de__broadphase_create(p, &d), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de__broadphase_create(NULL, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de__broadphase_update(NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(de__broadphase_get(p) == NULL);

    /* Before creation everything is empty and safe. */
    const de__bp_pair *pairs = (const de__bp_pair *)1;
    u32 count = 7;
    de__broadphase_pairs(p, &pairs, &count);
    EXPECT_TRUE(pairs == NULL);
    EXPECT_EQ(count, 0u);
    de__broadphase_sync(p);
    EXPECT_TRUE(de__broadphase_tree(p, 0) == NULL);

    ASSERT_EQ(de__broadphase_create(p, NULL), DE_OK);
    EXPECT_EQ(de__broadphase_create(p, NULL), DE_ERR_ALREADY_EXISTS);
    de__bp_stats st;
    de__broadphase_stats(p, &st);
    EXPECT_FLOAT_EQ(st.margin, DE__BP_DEFAULT_MARGIN, 0.0);
    EXPECT_EQ(st.max_pairs, DE__BP_MIN_PAIRS); /* 4 * 100 < 1024 */
    de_physics_destroy(p);

    /* The cvars are read at create. */
    const de_cvar mc = de_cvar_find(DE__BP_MARGIN_CVAR);
    const de_cvar pc = de_cvar_find(DE__BP_MAX_PAIRS_CVAR);
    ASSERT_TRUE(DE_HANDLE_VALID(mc));
    ASSERT_TRUE(DE_HANDLE_VALID(pc));
    ASSERT_EQ(de_cvar_set(mc, "0.25"), DE_OK);
    ASSERT_EQ(de_cvar_set(pc, "2000"), DE_OK);
    p = world_new(NULL, 1000);
    ASSERT_EQ(de__broadphase_create(p, NULL), DE_OK);
    de__broadphase_stats(p, &st);
    EXPECT_FLOAT_EQ(st.margin, 0.25, 0.0);
    EXPECT_EQ(st.max_pairs, 2000u);
    de_physics_destroy(p);
    ASSERT_EQ(de_cvar_set(mc, "0.05"), DE_OK);
    ASSERT_EQ(de_cvar_set(pc, "0"), DE_OK);
    p = world_new(NULL, 1000);
    ASSERT_EQ(de__broadphase_create(p, NULL), DE_OK);
    de__broadphase_stats(p, &st);
    EXPECT_EQ(st.max_pairs, 4000u);
    de_physics_destroy(p);
}

TEST(physics_broadphase, create_oom_no_leak) {
    de_tracking_allocator t;
    de_allocator a = de_tracking_allocator_init(&t, NULL, DE_MEM_PHYSICS);
    de_physics_world *p = world_new(&a, 256);
    const de_shape s = sphere(p, 1.0f);
    const de_body_desc d = body_desc(DE_BODY_DYNAMIC, s, 0, 0, 0);
    (void)add(p, &d);
    for (i64 k = 0; k < 3; ++k) {
        de_tracking_allocator_fail_after(&t, k);
        EXPECT_EQ(de__broadphase_create(p, NULL), DE_ERR_OUT_OF_MEMORY);
        EXPECT_TRUE(de__broadphase_get(p) == NULL);
    }
    de_tracking_allocator_fail_after(&t, -1);
    ASSERT_EQ(de__broadphase_create(p, NULL), DE_OK);
    de_physics_destroy(p);
    const de_tracking_stats st = de_tracking_allocator_stats(&t);
    EXPECT_EQ(st.bytes_live, 0u);
    EXPECT_EQ(st.blocks_live, 0u);
}

TEST(physics_broadphase, no_alloc_after_warmup) {
    de_tracking_allocator t;
    de_allocator a = de_tracking_allocator_init(&t, NULL, DE_MEM_PHYSICS);
    de_physics_world *p = world_new(&a, 2048);
    static scene s;
    scene_init(&s, p, 1500, 14.0, 9, false);
    ASSERT_EQ(de__broadphase_update(p), DE_OK); /* warm-up: creates the broadphase */
    de_physics_step(p, 1.0f / 60.0f);           /* and the contacts state (DENG-135) */
    const de_tracking_stats before = de_tracking_allocator_stats(&t);
    for (u32 tick = 0; tick < 120; ++tick) {
        scene_tick(&s, tick, 5, 20); /* includes body destroy + create */
        ASSERT_EQ(de__broadphase_update(p), DE_OK);
        de_physics_step(p, 1.0f / 60.0f);
    }
    const de_tracking_stats after = de_tracking_allocator_stats(&t);
    EXPECT_EQ(after.alloc_count, before.alloc_count);
    EXPECT_EQ(after.realloc_count, before.realloc_count);
    EXPECT_EQ(after.bytes_live, before.bytes_live);
    de_physics_destroy(p);
    EXPECT_EQ(de_tracking_allocator_stats(&t).bytes_live, 0u);
}

TEST(physics_broadphase, overflow_logs_and_recovers) {
    de_physics_world *p = world_new(NULL, 128);
    de__broadphase_desc d = {0.0f, 64u};
    ASSERT_EQ(de__broadphase_create(p, &d), DE_OK);
    const de_shape s = sphere(p, 1.0f);
    de_body b[40];
    for (u32 i = 0; i < 40; ++i) {
        const de_body_desc bd = body_desc(DE_BODY_DYNAMIC, s, 0.01 * (f64)i, 0, 0);
        b[i] = add(p, &bd);
    }
    /* 780 pairs do not fit in 64. */
    EXPECT_EQ(de__broadphase_update(p), DE_ERR_OUT_OF_MEMORY);
    const de__bp_pair *pairs;
    u32 count;
    de__broadphase_pairs(p, &pairs, &count);
    EXPECT_LE(count, 64u);
    for (u32 i = 1; i < count; ++i) /* still sorted, unique */
        EXPECT_TRUE(pairs[i - 1].a < pairs[i].a ||
                    (pairs[i - 1].a == pairs[i].a && pairs[i - 1].b < pairs[i].b));
    EXPECT_EQ(de__broadphase_update(p), DE_ERR_OUT_OF_MEMORY);
    de__bp_stats st;
    de__broadphase_stats(p, &st);
    EXPECT_EQ(st.overflows, 2u);

    /* Spread into 20 touching couples, 10 m apart: 20 pairs fit again. */
    for (u32 i = 0; i < 40; ++i)
        move_to(p, b[i], 10.0 * (f64)(i / 2u), 0.5 * (f64)(i & 1u), 0.0);
    EXPECT_EQ(update_and_check(p), 20u);
    de_physics_destroy(p);
}

/* ---- teleport, sync and queries ----------------------------------------------------------- */

typedef struct body_list {
    u32 v[4096];
    u32 n;
} body_list;

static bool list_cb(void *user, u32 body) {
    body_list *l = (body_list *)user;
    if (l->n < 4096u)
        l->v[l->n] = body;
    ++l->n;
    return true;
}

static f32 list_ray_cb(void *user, const de__tree_ray *ray, u32 body, f32 max_fraction) {
    (void)ray;
    (void)list_cb(user, body);
    return max_fraction;
}

static int cmp_u32(const void *a, const void *b) {
    const u32 x = *(const u32 *)a, y = *(const u32 *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

TEST(physics_broadphase, teleport_and_sync) {
    de_physics_world *p = world_new(NULL, 16);
    const de_shape s = sphere(p, 1.0f);
    de_body_desc d = body_desc(DE_BODY_DYNAMIC, s, 0, 0, 0);
    const de_body a = add(p, &d);
    d = body_desc(DE_BODY_DYNAMIC, s, 100, 0, 0);
    const de_body b = add(p, &d);
    EXPECT_EQ(update_and_check(p), 0u);

    move_to(p, b, 0.5, 0.0, 0.0);
    body_list l;
    memset(&l, 0, sizeof l);
    const de_aabb at0 = {{-0.1f, -0.1f, -0.1f}, {0.1f, 0.1f, 0.1f}};
    (void)de__broadphase_query_aabb(p, at0, NULL, list_cb, &l);
    EXPECT_EQ(l.n, 1u); /* b is still at its old place in the tree */
    de__broadphase_sync(p);
    memset(&l, 0, sizeof l);
    (void)de__broadphase_query_aabb(p, at0, NULL, list_cb, &l);
    EXPECT_EQ(l.n, 2u);
    EXPECT_EQ(update_and_check(p), 1u);
    EXPECT_TRUE(has_pair(p, idx(a), idx(b)));
    de_physics_destroy(p);
}

TEST(physics_broadphase, queries_vs_bruteforce) {
    de_physics_world *p = world_new(NULL, 1024);
    static scene s;
    scene_init(&s, p, 800, 15.0, 21, false);
    ASSERT_EQ(de__broadphase_update(p), DE_OK);
    for (u32 tick = 0; tick < 10; ++tick) {
        scene_tick(&s, tick, 0, 0);
        ASSERT_EQ(de__broadphase_update(p), DE_OK);
    }
    const de__body_pool *bp = &p->bodies;
    de_rng r = de_rng_seed(5, 6);
    static body_list got;
    for (u32 q = 0; q < 200; ++q) {
        de_collision_filter f = {1u << de_rng_range(&r, 0, 4), 0xFFFFFFFFu, 0u};
        if (q & 1u)
            f.mask = 0x3u;
        const de_collision_filter *fp = (q % 3u) == 0u ? NULL : &f;
        /* AABB query. */
        const f32 cx = (de_rng_f32(&r) - 0.5f) * 30.0f, cy = (de_rng_f32(&r) - 0.5f) * 30.0f;
        const de_aabb qb = {{cx - 3.0f, cy - 3.0f, -20.0f}, {cx + 3.0f, cy + 3.0f, 20.0f}};
        memset(&got, 0, sizeof got);
        EXPECT_TRUE(de__broadphase_query_aabb(p, qb, fp, list_cb, &got));
        qsort(got.v, got.n, sizeof(u32), cmp_u32);
        u32 expect = 0;
        for (u32 i = 0; i < bp->high_water; ++i) {
            const u32 leaf = de__broadphase_proxy(p, i);
            if (leaf == DE__TREE_NULL)
                continue;
            const de__tree *t = de__broadphase_tree(
                p, bp->type[i] == DE_BODY_STATIC ? DE__BP_TREE_STATIC : DE__BP_TREE_DYNAMIC);
            const de_aabb fat = de__tree_box(t, leaf);
            if (!de__aabb_overlap(&fat, &qb))
                continue;
            if (fp != NULL && !de__collision_filter_test(*fp, bp->filter[i]))
                continue;
            ASSERT_LT(expect, got.n);
            EXPECT_EQ(got.v[expect], i);
            ++expect;
        }
        EXPECT_EQ(got.n, expect);

        /* Ray query (all hits). */
        de__tree_ray ray;
        ray.origin = (de_dvec3){-25.0, 0.0, 0.0};
        ray.origin.y = (de_rng_f64(&r) - 0.5) * 30.0;
        ray.origin.z = (de_rng_f64(&r) - 0.5) * 30.0;
        ray.delta = (de_vec3){50.0f, (de_rng_f32(&r) - 0.5f) * 10.0f, 0.0f};
        ray.max_fraction = 1.0f;
        memset(&got, 0, sizeof got);
        EXPECT_TRUE(de__broadphase_raycast(p, &ray, fp, list_ray_cb, &got) == 1.0f);
        qsort(got.v, got.n, sizeof(u32), cmp_u32);
        expect = 0;
        for (u32 i = 0; i < bp->high_water; ++i) {
            const u32 leaf = de__broadphase_proxy(p, i);
            if (leaf == DE__TREE_NULL)
                continue;
            const de__tree *t = de__broadphase_tree(
                p, bp->type[i] == DE_BODY_STATIC ? DE__BP_TREE_STATIC : DE__BP_TREE_DYNAMIC);
            const de_aabb fat = de__tree_box(t, leaf);
            f64 te;
            if (!de__ray_aabb(&ray, 1.0f, &fat, &te))
                continue;
            if (fp != NULL && !de__collision_filter_test(*fp, bp->filter[i]))
                continue;
            ASSERT_LT(expect, got.n);
            EXPECT_EQ(got.v[expect], i);
            ++expect;
        }
        EXPECT_EQ(got.n, expect);
    }
    de_physics_destroy(p);
}

/* ---- checked-in vector -------------------------------------------------------------------- */

typedef struct hasher {
    u8 buf[8 + 8 * MAX_TEST_PAIRS];
    u32 len;
} hasher;

static void put_u32(hasher *h, u32 v) {
    h->buf[h->len++] = (u8)v;
    h->buf[h->len++] = (u8)(v >> 8);
    h->buf[h->len++] = (u8)(v >> 16);
    h->buf[h->len++] = (u8)(v >> 24);
}

/* The hash of one tick's pair list (little-endian tick, count, then every (a, b)). */
static u64 hash_pairs(u32 tick, const de__bp_pair *pairs, u32 count, u64 seed) {
    static hasher h;
    h.len = 0;
    put_u32(&h, tick);
    put_u32(&h, count);
    for (u32 i = 0; i < count && i < MAX_TEST_PAIRS; ++i) {
        put_u32(&h, pairs[i].a);
        put_u32(&h, pairs[i].b);
    }
    return de_hash64(h.buf, h.len, seed);
}

typedef struct golden_entry {
    char name[32];
    u64 value;
} golden_entry;

static u32 compute_golden(golden_entry *out) {
    de_physics_world *p = world_new(NULL, 2048);
    static scene s;
    scene_init(&s, p, 2000, 16.0, 2026, false);
    u64 chain = 0;
    u64 total = 0;
    u32 n = 0;
    for (u32 tick = 1; tick <= 300; ++tick) {
        scene_tick(&s, tick, 10, 8);
        u32 count;
        if (tick % 50u == 0u) {
            count = update_and_check(p); /* brute force every 50 ticks */
        } else {
            ASSERT_EQ(de__broadphase_update(p), DE_OK);
        }
        const de__bp_pair *pairs;
        de__broadphase_pairs(p, &pairs, &count);
        const u64 h = hash_pairs(tick, pairs, count, 0);
        chain = hash_pairs(tick, pairs, count, chain);
        total += count;
        if (tick == 1u || tick % 100u == 0u) {
            snprintf(out[n].name, sizeof out[n].name, "tick%03u", (unsigned)tick);
            out[n++].value = h;
        }
    }
    snprintf(out[n].name, sizeof out[n].name, "chain");
    out[n++].value = chain;
    snprintf(out[n].name, sizeof out[n].name, "pairs_total");
    out[n++].value = total;
    printf("  2000 bodies, 300 ticks: %llu pairs in total\n", (unsigned long long)total);
    de_physics_destroy(p);
    return n;
}

TEST(physics_broadphase, test_pair_vector_golden) {
    golden_entry got[16];
    const u32 n = compute_golden(got);
    const char *regen = getenv("DE_REGEN_GOLDEN");
    if (regen != NULL && strcmp(regen, "1") == 0) {
        FILE *out = fopen(DE_PHYSICS_BROADPHASE_GOLDEN, "wb");
        ASSERT_TRUE(out != NULL);
        fprintf(out,
                "# broadphase_golden.txt: hashes (XXH3-64) of the sorted broadphase pair list\n"
                "# of a 2,000-body de_rng scene over 300 ticks of scripted motion with body\n"
                "# churn (DENG-133). Generated by test_physics_broadphase with\n"
                "# DE_REGEN_GOLDEN=1; never edit by hand. x86-64 and arm64 must reproduce\n"
                "# every line.\n");
        for (u32 i = 0; i < n; ++i)
            fprintf(out, "%s %016llx\n", got[i].name, (unsigned long long)got[i].value);
        fclose(out);
        SKIP("DE_REGEN_GOLDEN=1: rewrote " DE_PHYSICS_BROADPHASE_GOLDEN);
    }
    FILE *in = fopen(DE_PHYSICS_BROADPHASE_GOLDEN, "rb");
    ASSERT_TRUE(in != NULL);
    char line[256];
    u32 lines = 0, bad = 0;
    while (fgets(line, sizeof line, in)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;
        char name[64];
        unsigned long long v = 0;
        ASSERT_EQ(sscanf(line, "%63s %llx", name, &v), 2);
        ASSERT_LT(lines, n);
        EXPECT_STREQ(name, got[lines].name);
        if ((u64)v != got[lines].value) {
            printf("  %s: expected %016llx got %016llx\n", name, v,
                   (unsigned long long)got[lines].value);
            ++bad;
        }
        ++lines;
    }
    fclose(in);
    EXPECT_EQ(lines, n);
    EXPECT_EQ(bad, 0u);
}

/* ---- informational benchmark -------------------------------------------------------------- */

/* 10k moving proxies: update + pairs per tick. Never fails on speed; the numbers that matter
 * come from a Release build. */
TEST(physics_broadphase, bench_10k_moving) {
    de_physics_world *p = world_new(NULL, SCENE_MAX);
    static scene s;
    scene_init(&s, p, SCENE_MAX, 30.0, 10000, true);
    const u64 c0 = de_time_now_ns();
    ASSERT_EQ(de__broadphase_update(p), DE_OK);
    const u64 c1 = de_time_now_ns();
    enum { TICKS = 60 };
    u64 total = 0, worst = 0;
    u32 pairs = 0;
    for (u32 tick = 0; tick < TICKS; ++tick) {
        scene_tick(&s, tick, 0, 0);
        const u64 t0 = de_time_now_ns();
        ASSERT_EQ(de__broadphase_update(p), DE_OK);
        const u64 dt = de_time_now_ns() - t0;
        total += dt;
        worst = dt > worst ? dt : worst;
        de__bp_stats st;
        de__broadphase_stats(p, &st);
        pairs += st.pairs;
    }
    de__bp_stats st;
    de__broadphase_stats(p, &st);
#if defined(DE_DEBUG)
    const char *cfg = "Debug (validates every update)";
#else
    const char *cfg = "optimized";
#endif
    printf("  [bench] broadphase %s: 10k proxies, first update %.3f ms, update + pairs mean "
           "%.3f ms, worst %.3f ms; %u pairs/tick, %u proxies queried last tick, %u candidates, "
           "heights %u/%u\n",
           cfg, (double)(c1 - c0) * 1e-6, (double)total * 1e-6 / (double)TICKS,
           (double)worst * 1e-6, (unsigned)(pairs / TICKS), (unsigned)st.queried,
           (unsigned)st.candidates, (unsigned)st.height[0], (unsigned)st.height[1]);
    de_physics_destroy(p);
}
