/* test_aabb_tree.c: the broadphase's dynamic AABB tree (DENG-133, SPEC §13.1): insert, remove,
 * move, validation after 10k random operations, the height bound of SAH insertion with rotations,
 * overlap and ray traversal against brute force, traversal order, and a deterministic layout. */
#include "de_test.h"

#include <dengine/de_physics.h>

#include "physics/broadphase/aabb_tree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- helpers ------------------------------------------------------------------------------ */

typedef struct tree_fx {
    de__tree_pool pool;
    de__tree tree;
} tree_fx;

static void tree_fx_init(tree_fx *f, u32 capacity) {
    ASSERT_EQ(de__tree_pool_init(&f->pool, de_heap_allocator(), capacity), DE_OK);
    de__tree_init(&f->tree, &f->pool);
}

static void tree_fx_free(tree_fx *f) {
    de__tree_pool_free(&f->pool, de_heap_allocator());
}

static de_aabb box_at(f32 x, f32 y, f32 z, f32 hx, f32 hy, f32 hz) {
    de_aabb b;
    b.min = (de_vec3){x - hx, y - hy, z - hz};
    b.max = (de_vec3){x + hx, y + hy, z + hz};
    return b;
}

static de_aabb random_box(de_rng *r, f32 extent, f32 max_half) {
    const f32 x = (de_rng_f32(r) * 2.0f - 1.0f) * extent;
    const f32 y = (de_rng_f32(r) * 2.0f - 1.0f) * extent;
    const f32 z = (de_rng_f32(r) * 2.0f - 1.0f) * extent;
    const f32 hx = 0.05f + de_rng_f32(r) * max_half;
    const f32 hy = 0.05f + de_rng_f32(r) * max_half;
    const f32 hz = 0.05f + de_rng_f32(r) * max_half;
    return box_at(x, y, z, hx, hy, hz);
}

/* Collects the payloads a traversal reports, in order. */
typedef struct collect {
    u32 values[8192];
    u32 count;
    u32 stop_after; /* 0 = never stop */
} collect;

static bool collect_cb(void *user, u32 leaf, u32 value) {
    collect *c = (collect *)user;
    (void)leaf;
    if (c->count < 8192u)
        c->values[c->count] = value;
    ++c->count;
    return c->stop_after == 0 || c->count < c->stop_after;
}

static int cmp_u32(const void *a, const void *b) {
    const u32 x = *(const u32 *)a, y = *(const u32 *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* Brute-force log2 ceiling for the height bound. */
static u32 log2_ceil(u32 n) {
    u32 k = 0;
    while ((1u << k) < n)
        ++k;
    return k;
}

/* ---- basic operations --------------------------------------------------------------------- */

TEST(physics_aabb_tree, insert_remove_move_basic) {
    tree_fx f;
    tree_fx_init(&f, 16);
    EXPECT_TRUE(de__tree_validate(&f.tree));
    EXPECT_EQ(de__tree_height(&f.tree), 0u);

    const u32 a = de__tree_insert(&f.tree, box_at(0, 0, 0, 1, 1, 1), 10);
    ASSERT_NE(a, DE__TREE_NULL);
    EXPECT_EQ(f.tree.root, a);
    EXPECT_EQ(f.tree.leaf_count, 1u);
    EXPECT_EQ(f.tree.node_count, 1u);
    EXPECT_EQ(de__tree_value(&f.tree, a), 10u);
    EXPECT_TRUE(de__tree_validate(&f.tree));

    const u32 b = de__tree_insert(&f.tree, box_at(5, 0, 0, 1, 1, 1), 11);
    const u32 c = de__tree_insert(&f.tree, box_at(10, 0, 0, 1, 1, 1), 12);
    ASSERT_NE(b, DE__TREE_NULL);
    ASSERT_NE(c, DE__TREE_NULL);
    EXPECT_EQ(f.tree.leaf_count, 3u);
    EXPECT_EQ(f.tree.node_count, 5u);
    EXPECT_EQ(f.pool.used, 5u);
    EXPECT_TRUE(de__tree_validate(&f.tree));
    EXPECT_EQ(de__tree_height(&f.tree), 2u);

    collect col;
    memset(&col, 0, sizeof col);
    de__tree_query(&f.tree, box_at(4.5f, 0, 0, 0.1f, 0.1f, 0.1f), collect_cb, &col);
    ASSERT_EQ(col.count, 1u);
    EXPECT_EQ(col.values[0], 11u);

    /* Move keeps the leaf index. */
    de__tree_move(&f.tree, b, box_at(10.5f, 0, 0, 1, 1, 1));
    EXPECT_TRUE(de__tree_validate(&f.tree));
    EXPECT_EQ(de__tree_value(&f.tree, b), 11u);
    memset(&col, 0, sizeof col);
    de__tree_query(&f.tree, box_at(4.5f, 0, 0, 0.1f, 0.1f, 0.1f), collect_cb, &col);
    EXPECT_EQ(col.count, 0u);
    memset(&col, 0, sizeof col);
    de__tree_query(&f.tree, box_at(10.2f, 0, 0, 0.1f, 0.1f, 0.1f), collect_cb, &col);
    ASSERT_EQ(col.count, 2u);
    qsort(col.values, col.count, sizeof(u32), cmp_u32);
    EXPECT_EQ(col.values[0], 11u);
    EXPECT_EQ(col.values[1], 12u);

    /* Touching boxes overlap (closed boxes). */
    memset(&col, 0, sizeof col);
    de__tree_query(&f.tree, box_at(-2, 0, 0, 1, 1, 1), collect_cb, &col);
    EXPECT_EQ(col.count, 1u);

    de__tree_remove(&f.tree, a);
    EXPECT_TRUE(de__tree_validate(&f.tree));
    de__tree_remove(&f.tree, c);
    EXPECT_TRUE(de__tree_validate(&f.tree));
    EXPECT_EQ(f.tree.root, b);
    de__tree_remove(&f.tree, b);
    EXPECT_TRUE(de__tree_validate(&f.tree));
    EXPECT_EQ(f.tree.root, DE__TREE_NULL);
    EXPECT_EQ(f.pool.used, 0u);

    /* Freed nodes are reused LIFO: the pool never grows past its high water. */
    const u32 hw = f.pool.high_water;
    for (u32 i = 0; i < 3; ++i)
        (void)de__tree_insert(&f.tree, box_at((f32)i, 0, 0, 1, 1, 1), i);
    EXPECT_EQ(f.pool.high_water, hw);
    tree_fx_free(&f);
}

TEST(physics_aabb_tree, pool_full) {
    tree_fx f;
    tree_fx_init(&f, 4);
    EXPECT_NE(de__tree_insert(&f.tree, box_at(0, 0, 0, 1, 1, 1), 0), DE__TREE_NULL);
    EXPECT_NE(de__tree_insert(&f.tree, box_at(3, 0, 0, 1, 1, 1), 1), DE__TREE_NULL);
    /* 3 nodes used, 1 free: a third leaf needs 2. */
    EXPECT_EQ(de__tree_insert(&f.tree, box_at(6, 0, 0, 1, 1, 1), 2), DE__TREE_NULL);
    EXPECT_EQ(f.tree.leaf_count, 2u);
    EXPECT_TRUE(de__tree_validate(&f.tree));
    tree_fx_free(&f);
}

TEST(physics_aabb_tree, shared_pool_two_trees) {
    de__tree_pool pool;
    ASSERT_EQ(de__tree_pool_init(&pool, de_heap_allocator(), 64), DE_OK);
    de__tree t1, t2;
    de__tree_init(&t1, &pool);
    de__tree_init(&t2, &pool);
    de_rng r = de_rng_seed(3, 1);
    for (u32 i = 0; i < 16; ++i) {
        ASSERT_NE(de__tree_insert((i & 1u) ? &t2 : &t1, random_box(&r, 10, 1), i), DE__TREE_NULL);
    }
    EXPECT_TRUE(de__tree_validate(&t1));
    EXPECT_TRUE(de__tree_validate(&t2));
    EXPECT_EQ(pool.used, t1.node_count + t2.node_count);
    EXPECT_EQ(t1.leaf_count, 8u);
    EXPECT_EQ(t2.leaf_count, 8u);
    de__tree_pool_free(&pool, de_heap_allocator());
}

TEST(physics_aabb_tree, validate_detects_corruption) {
    tree_fx f;
    tree_fx_init(&f, 32);
    de_rng r = de_rng_seed(5, 5);
    for (u32 i = 0; i < 8; ++i)
        (void)de__tree_insert(&f.tree, random_box(&r, 10, 1), i);
    ASSERT_TRUE(de__tree_validate(&f.tree));
    de__tree_node *root = &f.pool.nodes[f.tree.root];
    const de_aabb saved = root->box;
    root->box.max.x += 1.0f;
    EXPECT_FALSE(de__tree_validate(&f.tree));
    root->box = saved;
    const u32 h = root->height;
    root->height = h + 1u;
    EXPECT_FALSE(de__tree_validate(&f.tree));
    root->height = h;
    ++f.tree.leaf_count;
    EXPECT_FALSE(de__tree_validate(&f.tree));
    --f.tree.leaf_count;
    EXPECT_TRUE(de__tree_validate(&f.tree));
    tree_fx_free(&f);
}

/* ---- 10k random operations ---------------------------------------------------------------- */

TEST(physics_aabb_tree, random_ops_validate) {
    enum { MAXL = 1024, OPS = 10000 };
    tree_fx f;
    tree_fx_init(&f, 2 * MAXL);
    static u32 leaf_of[MAXL];
    static de_aabb box_of[MAXL];
    for (u32 i = 0; i < MAXL; ++i)
        leaf_of[i] = DE__TREE_NULL;
    de_rng r = de_rng_seed(1234, 7);
    u32 live = 0;
    static collect col;
    for (u32 op = 0; op < OPS; ++op) {
        const u32 id = de_rng_range(&r, 0, MAXL);
        const u32 kind = de_rng_range(&r, 0, 10);
        if (leaf_of[id] == DE__TREE_NULL) {
            box_of[id] = random_box(&r, 50, 2);
            leaf_of[id] = de__tree_insert(&f.tree, box_of[id], id);
            ASSERT_NE(leaf_of[id], DE__TREE_NULL);
            ++live;
        } else if (kind < 3) {
            de__tree_remove(&f.tree, leaf_of[id]);
            leaf_of[id] = DE__TREE_NULL;
            --live;
        } else {
            /* Small moves mostly, sometimes a teleport. */
            de_aabb b = box_of[id];
            const f32 dx = kind == 9 ? 60.0f * (de_rng_f32(&r) - 0.5f) : de_rng_f32(&r) - 0.5f;
            b.min.x += dx;
            b.max.x += dx;
            box_of[id] = b;
            de__tree_move(&f.tree, leaf_of[id], b);
        }
        ASSERT_EQ(f.tree.leaf_count, live);
        if (op % 97u == 0u || op == OPS - 1u) {
            ASSERT_TRUE(de__tree_validate(&f.tree));
            /* Query a random box: same set as brute force. */
            const de_aabb q = random_box(&r, 50, 8);
            memset(&col, 0, sizeof col);
            de__tree_query(&f.tree, q, collect_cb, &col);
            qsort(col.values, col.count, sizeof(u32), cmp_u32);
            u32 expect = 0;
            for (u32 i = 0; i < MAXL; ++i) {
                if (leaf_of[i] == DE__TREE_NULL || !de__aabb_overlap(&box_of[i], &q))
                    continue;
                ASSERT_LT(expect, col.count);
                EXPECT_EQ(col.values[expect], i);
                ++expect;
            }
            EXPECT_EQ(col.count, expect);
        }
    }
    EXPECT_EQ(f.pool.used, f.tree.node_count);
    tree_fx_free(&f);
}

/* ---- balance ------------------------------------------------------------------------------ */

TEST(physics_aabb_tree, height_bound) {
    enum { N = 4096 };
    tree_fx f;
    tree_fx_init(&f, 2 * N);
    /* Worst case for a naive tree: boxes inserted in sorted order along a line. */
    for (u32 i = 0; i < N; ++i)
        ASSERT_NE(de__tree_insert(&f.tree, box_at((f32)i, 0, 0, 0.4f, 0.4f, 0.4f), i),
                  DE__TREE_NULL);
    ASSERT_TRUE(de__tree_validate(&f.tree));
    const u32 h_line = de__tree_height(&f.tree);
    /* A grid in sorted order. */
    tree_fx g;
    tree_fx_init(&g, 2 * N);
    for (u32 i = 0; i < N; ++i)
        (void)de__tree_insert(
            &g.tree,
            box_at((f32)(i % 16u), (f32)((i / 16u) % 16u), (f32)(i / 256u), 0.4f, 0.4f, 0.4f), i);
    ASSERT_TRUE(de__tree_validate(&g.tree));
    const u32 h_grid = de__tree_height(&g.tree);
    /* Random boxes. */
    tree_fx q;
    tree_fx_init(&q, 2 * N);
    de_rng r = de_rng_seed(77, 1);
    static u32 leaves[N];
    for (u32 i = 0; i < N; ++i)
        leaves[i] = de__tree_insert(&q.tree, random_box(&r, 100, 1), i);
    ASSERT_TRUE(de__tree_validate(&q.tree));
    const u32 h_rand = de__tree_height(&q.tree);
    printf("  heights for %u leaves: line %u, grid %u, random %u (log2 = %u)\n", (unsigned)N,
           (unsigned)h_line, (unsigned)h_grid, (unsigned)h_rand, (unsigned)log2_ceil(N));
    /* Rotations keep the tree within a small factor of a balanced one (12 for 4096 leaves);
     * without them, sorted insertion along a line degenerates towards a list. */
    const u32 bound = 3u * log2_ceil(N);
    EXPECT_LE(h_line, bound);
    EXPECT_LE(h_grid, bound);
    EXPECT_LE(h_rand, bound);

    /* Removing half the leaves and moving the rest (twice) keeps the bound. */
    for (u32 i = 0; i < N; i += 2)
        de__tree_remove(&q.tree, leaves[i]);
    for (u32 pass = 0; pass < 2; ++pass)
        for (u32 i = 1; i < N; i += 2)
            de__tree_move(&q.tree, leaves[i], random_box(&r, 100, 1));
    ASSERT_TRUE(de__tree_validate(&q.tree));
    EXPECT_EQ(q.tree.leaf_count, (u32)N / 2u);
    EXPECT_LE(de__tree_height(&q.tree), bound);
    tree_fx_free(&f);
    tree_fx_free(&g);
    tree_fx_free(&q);
}

/* ---- traversal ---------------------------------------------------------------------------- */

TEST(physics_aabb_tree, query_order_is_depth_first_child1_first) {
    tree_fx f;
    tree_fx_init(&f, 512);
    de_rng r = de_rng_seed(8, 8);
    for (u32 i = 0; i < 200; ++i)
        (void)de__tree_insert(&f.tree, random_box(&r, 20, 3), i);
    /* Reference order: a recursive left-first walk. */
    static u32 expect[256];
    u32 ne = 0;
    const de_aabb q = box_at(0, 0, 0, 12, 12, 12);
    u32 stack[128], sp = 0;
    stack[sp++] = f.tree.root;
    while (sp > 0) {
        const u32 n = stack[--sp];
        const de__tree_node *x = &f.pool.nodes[n];
        if (!de__aabb_overlap(&x->box, &q))
            continue;
        if (x->height == 0) {
            expect[ne++] = x->value;
        } else {
            stack[sp++] = x->child2;
            stack[sp++] = x->child1;
        }
    }
    static collect col;
    memset(&col, 0, sizeof col);
    EXPECT_TRUE(de__tree_query(&f.tree, q, collect_cb, &col));
    ASSERT_EQ(col.count, ne);
    ASSERT_GT(ne, 3u);
    EXPECT_MEMEQ(col.values, expect, sizeof(u32) * ne);

    /* Stopping early. */
    memset(&col, 0, sizeof col);
    col.stop_after = 3;
    EXPECT_FALSE(de__tree_query(&f.tree, q, collect_cb, &col));
    EXPECT_EQ(col.count, 3u);
    EXPECT_MEMEQ(col.values, expect, sizeof(u32) * 3u);
    tree_fx_free(&f);
}

typedef struct ray_collect {
    u32 values[4096];
    f64 enter[4096];
    u32 count;
    bool clip; /* return the entry fraction (closest-hit search) */
    const de__tree *tree;
} ray_collect;

static f32 ray_collect_cb(void *user, const de__tree_ray *ray, u32 leaf, u32 value,
                          f32 max_fraction) {
    ray_collect *c = (ray_collect *)user;
    f64 t;
    const de_aabb b = de__tree_box(c->tree, leaf);
    if (!de__ray_aabb(ray, max_fraction, &b, &t))
        return max_fraction; /* never happens: the traversal tested it */
    if (c->count < 4096u) {
        c->values[c->count] = value;
        c->enter[c->count] = t;
    }
    ++c->count;
    if (c->clip)
        return t > 0.0 ? (f32)t : max_fraction;
    return max_fraction;
}

TEST(physics_aabb_tree, ray_vs_bruteforce) {
    enum { N = 1500, RAYS = 400 };
    tree_fx f;
    tree_fx_init(&f, 2 * N);
    static de_aabb boxes[N];
    de_rng r = de_rng_seed(99, 3);
    for (u32 i = 0; i < N; ++i) {
        boxes[i] = random_box(&r, 30, 3.0f);
        (void)de__tree_insert(&f.tree, boxes[i], i);
    }
    static ray_collect rc;
    u32 total_hits = 0;
    for (u32 k = 0; k < RAYS; ++k) {
        de__tree_ray ray;
        /* One draw per statement (initializer evaluation order is unspecified). */
        ray.origin.x = (de_rng_f64(&r) - 0.5) * 100.0;
        ray.origin.y = (de_rng_f64(&r) - 0.5) * 100.0;
        ray.origin.z = (de_rng_f64(&r) - 0.5) * 100.0;
        ray.delta.x = (de_rng_f32(&r) - 0.5f) * 120.0f;
        ray.delta.y = (de_rng_f32(&r) - 0.5f) * 120.0f;
        ray.delta.z = (de_rng_f32(&r) - 0.5f) * 120.0f;
        if (k % 10u == 0u) /* axis-aligned rays exercise the zero-delta slabs */
            ray.delta.y = ray.delta.z = 0.0f;
        ray.max_fraction = 1.0f;

        /* All hits. */
        memset(&rc, 0, sizeof rc);
        rc.tree = &f.tree;
        EXPECT_TRUE(de__tree_raycast(&f.tree, &ray, ray_collect_cb, &rc) == 1.0f);
        qsort(rc.values, rc.count, sizeof(u32), cmp_u32);
        u32 expect = 0;
        f64 best = 2.0;
        u32 best_i = DE__TREE_NULL;
        for (u32 i = 0; i < N; ++i) {
            f64 t;
            if (!de__ray_aabb(&ray, 1.0f, &boxes[i], &t))
                continue;
            ASSERT_LT(expect, rc.count);
            EXPECT_EQ(rc.values[expect], i);
            ++expect;
            if (t < best) {
                best = t;
                best_i = i;
            }
        }
        EXPECT_EQ(rc.count, expect);
        total_hits += expect;

        /* Closest hit with clipping: the last reported hit is the nearest box. */
        memset(&rc, 0, sizeof rc);
        rc.tree = &f.tree;
        rc.clip = true;
        const f32 fr = de__tree_raycast(&f.tree, &ray, ray_collect_cb, &rc);
        if (best_i == DE__TREE_NULL) {
            EXPECT_EQ(rc.count, 0u);
            EXPECT_TRUE(fr == 1.0f);
        } else if (best > 0.0) {
            ASSERT_GT(rc.count, 0u);
            EXPECT_TRUE(rc.enter[rc.count - 1u] == best);
            EXPECT_TRUE(fr == (f32)best);
            EXPECT_LE(rc.count, expect);
        }
    }
    printf("  %u rays, %u box hits\n", (unsigned)RAYS, (unsigned)total_hits);
    EXPECT_GT(total_hits, 100u);
    tree_fx_free(&f);
}

static f32 ray_stop_cb(void *user, const de__tree_ray *ray, u32 leaf, u32 value, f32 max_fraction) {
    (void)ray;
    (void)leaf;
    (void)value;
    (void)max_fraction;
    ++*(u32 *)user;
    return 0.0f;
}

TEST(physics_aabb_tree, ray_stop_and_far_origin) {
    tree_fx f;
    tree_fx_init(&f, 16);
    /* Boxes at 100 km: the f64 origin keeps the slab test exact. */
    const f32 big = 100000.0f;
    (void)de__tree_insert(&f.tree, box_at(big + 5.0f, big, 0, 0.5f, 0.5f, 0.5f), 1);
    (void)de__tree_insert(&f.tree, box_at(big + 10.0f, big, 0, 0.5f, 0.5f, 0.5f), 2);
    de__tree_ray ray;
    ray.origin = (de_dvec3){(f64)big, (f64)big, 0.0};
    ray.delta = (de_vec3){20.0f, 0.0f, 0.0f};
    ray.max_fraction = 1.0f;
    u32 calls = 0;
    EXPECT_TRUE(de__tree_raycast(&f.tree, &ray, ray_stop_cb, &calls) == 0.0f);
    EXPECT_EQ(calls, 1u);
    f64 t;
    const de_aabb b = de__tree_box(&f.tree, f.pool.nodes[f.tree.root].child1);
    ASSERT_TRUE(de__ray_aabb(&ray, 1.0f, &b, &t));
    EXPECT_TRUE(t == 4.5 / 20.0 || t == 9.5 / 20.0);
    /* A segment that ends before the first box. */
    ray.max_fraction = 0.2f;
    calls = 0;
    EXPECT_TRUE(de__tree_raycast(&f.tree, &ray, ray_stop_cb, &calls) == 0.2f);
    EXPECT_EQ(calls, 0u);
    tree_fx_free(&f);
}

/* ---- determinism -------------------------------------------------------------------------- */

static void run_ops(tree_fx *f, u32 seed) {
    enum { MAXL = 256 };
    u32 leaf_of[MAXL];
    for (u32 i = 0; i < MAXL; ++i)
        leaf_of[i] = DE__TREE_NULL;
    de_rng r = de_rng_seed(seed, 2);
    for (u32 op = 0; op < 3000; ++op) {
        const u32 id = de_rng_range(&r, 0, MAXL);
        if (leaf_of[id] == DE__TREE_NULL)
            leaf_of[id] = de__tree_insert(&f->tree, random_box(&r, 30, 2), id);
        else if (de_rng_range(&r, 0, 4) == 0) {
            de__tree_remove(&f->tree, leaf_of[id]);
            leaf_of[id] = DE__TREE_NULL;
        } else
            de__tree_move(&f->tree, leaf_of[id], random_box(&r, 30, 2));
    }
}

TEST(physics_aabb_tree, deterministic_layout) {
    tree_fx a, b;
    tree_fx_init(&a, 512);
    tree_fx_init(&b, 512);
    run_ops(&a, 31);
    run_ops(&b, 31);
    ASSERT_TRUE(de__tree_validate(&a.tree));
    EXPECT_EQ(a.tree.root, b.tree.root);
    EXPECT_EQ(a.pool.high_water, b.pool.high_water);
    EXPECT_MEMEQ(a.pool.nodes, b.pool.nodes, sizeof(de__tree_node) * a.pool.high_water);
    /* Same operations, same tree: the SAH cost is identical too. */
    const f64 cost = de__tree_area_cost(&a.tree);
    EXPECT_TRUE(cost > 0.0 && cost == de__tree_area_cost(&b.tree));
    tree_fx_free(&a);
    tree_fx_free(&b);
}
