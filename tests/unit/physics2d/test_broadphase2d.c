/*
 * test_broadphase2d.c: the 2D AABB tree and broadphase (DENG-253, SPEC §13.2, §13.3): tree
 * invariants under random operations, queries and ray traversal against brute force, the pair list
 * against brute force (filters, body types, pair distance, predictive growth), independence from
 * insertion and move history, capacity overflow, and the chain segment bounds.
 */
#include "p2d_collide_test_util.h"

/* ---- Tree ----------------------------------------------------------------------------------- */

static de_aabb2d rand_box(de_rng *r, f32 world, f32 size) {
    const f32 x = world * de_rng_f32(r), y = world * de_rng_f32(r);
    de_aabb2d b = {{x, y}, {x + size * de_rng_f32(r), y + size * de_rng_f32(r)}};
    return b;
}

typedef struct leaf_list {
    u32 v[1024];
    u32 n;
} leaf_list;

static bool collect_cb(void *user, u32 leaf, u32 value) {
    leaf_list *l = (leaf_list *)user;
    (void)leaf;
    l->v[l->n++] = value;
    return true;
}

static f32 ray_collect_cb(void *user, const de__tree2d_ray *ray, u32 leaf, u32 value,
                          f32 max_fraction) {
    leaf_list *l = (leaf_list *)user;
    (void)ray;
    (void)leaf;
    l->v[l->n++] = value;
    return max_fraction;
}

static int cmp_u32(const void *a, const void *b) {
    const u32 x = *(const u32 *)a, y = *(const u32 *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

TEST(physics2d_broadphase, tree_random_ops) {
    de__tree2d_pool pool;
    ASSERT_EQ(de__tree2d_pool_init(&pool, de_heap_allocator(), 1024), DE_OK);
    de__tree2d t;
    de__tree2d_init(&t, &pool);
    static u32 leaf_of[400];
    static de_aabb2d box_of[400];
    static bool live[400];
    memset(live, 0, sizeof live);
    de_rng r = de_rng_seed(253, 10);
    u32 live_count = 0;
    for (u32 op = 0; op < 4000; ++op) {
        const u32 v = de_rng_range(&r, 0, 400);
        if (!live[v]) {
            box_of[v] = rand_box(&r, 100.0f, 6.0f);
            leaf_of[v] = de__tree2d_insert(&t, box_of[v], v);
            ASSERT_NE(leaf_of[v], DE__TREE2D_NULL);
            live[v] = true;
            ++live_count;
        } else if (de_rng_range(&r, 0, 3) == 0) {
            de__tree2d_remove(&t, leaf_of[v]);
            live[v] = false;
            --live_count;
        } else {
            const u32 old = leaf_of[v];
            box_of[v] = rand_box(&r, 100.0f, 6.0f);
            de__tree2d_move(&t, leaf_of[v], box_of[v]);
            EXPECT_EQ(leaf_of[v], old);
        }
        ASSERT_EQ(t.leaf_count, live_count);
        if (op % 97u == 0u) {
            ASSERT_TRUE(de__tree2d_validate(&t));
            /* Query against brute force. */
            const de_aabb2d q = rand_box(&r, 100.0f, 30.0f);
            static leaf_list got;
            got.n = 0;
            ASSERT_TRUE(de__tree2d_query(&t, q, collect_cb, &got));
            u32 want = 0;
            for (u32 i = 0; i < 400; ++i)
                if (live[i] && de__aabb2d_overlap(&box_of[i], &q))
                    ++want;
            ASSERT_EQ(got.n, want);
            qsort(got.v, got.n, sizeof(u32), cmp_u32);
            for (u32 i = 1; i < got.n; ++i)
                ASSERT_LT(got.v[i - 1], got.v[i]);
            for (u32 i = 0; i < got.n; ++i)
                EXPECT_TRUE(de__aabb2d_overlap(&box_of[got.v[i]], &q));
            /* Ray against brute force (all entered leaves). */
            de__tree2d_ray ray;
            ray.origin = de__d2_make(100.0 * de_rng_f64(&r), -5.0);
            ray.delta = (de_vec2){60.0f * de_rng_f32(&r) - 30.0f, 110.0f};
            ray.max_fraction = 1.0f;
            got.n = 0;
            TU_EXPECT_FEQ(de__tree2d_raycast(&t, &ray, ray_collect_cb, &got), 1.0f);
            u32 wr = 0;
            for (u32 i = 0; i < 400; ++i) {
                f64 te;
                if (live[i] && de__ray2d_aabb(&ray, 1.0f, &box_of[i], &te))
                    ++wr;
            }
            EXPECT_EQ(got.n, wr);
        }
    }
    ASSERT_TRUE(de__tree2d_validate(&t));
    /* SAH insertion with rotations keeps the tree shallow. */
    EXPECT_LT(de__tree2d_height(&t), 24u);
    de__tree2d_pool_free(&pool, de_heap_allocator());
}

/* ---- Pair list against brute force -------------------------------------------------------------
 */

#define BW_BODIES 300u

typedef struct bp_world {
    de_physics2d_world *p;
    de_shape shapes[8];
    de_body bodies[BW_BODIES];
} bp_world;

static de_collision_filter rand_filter(de_rng *r) {
    de_collision_filter f;
    const u32 k = de_rng_range(r, 0, 6);
    f.layer = 1u << de_rng_range(r, 0, 3);
    f.mask = k == 0 ? 0x5u : 0xFFFFFFFFu;
    f.group = k == 1 ? 7u : 0u;
    return f;
}

static void bw_build(bp_world *w, de_rng *r, f32 margin) {
    de_physics2d_desc d;
    memset(&d, 0, sizeof d);
    d.max_bodies = BW_BODIES + 8u;
    ASSERT_EQ(de_physics2d_create(&d, &w->p), DE_OK);
    de__bp2d_desc bd = {margin, 0};
    ASSERT_EQ(de__bp2d_create(w->p, &bd), DE_OK);
    de_physics2d_world *p = w->p;
    w->shapes[0] = tu_box(p, 0.5f, 0.3f, 0.0f);
    w->shapes[1] = tu_circle(p, 0.4f, (de_vec2){0, 0});
    w->shapes[2] = tu_capsule(p, (de_vec2){-0.5f, 0}, (de_vec2){0.5f, 0}, 0.2f);
    w->shapes[3] = tu_box(p, 2.0f, 0.2f, 0.05f);
    w->shapes[4] = tu_segment(p, (de_vec2){-1, 0}, (de_vec2){1, 0.5f});
    de_vec2 pts[20];
    for (u32 i = 0; i < 20; ++i)
        pts[i] = (de_vec2){5.0f - 0.5f * (f32)i, 0.3f * de_rng_f32(r)};
    w->shapes[5] = de_shape2d_chain_create(p, pts, 20, false);
    w->shapes[6] = tu_circle(p, 0.1f, (de_vec2){0, 0});
    w->shapes[7] = tu_box(p, 0.1f, 0.1f, 0.0f);
    for (u32 i = 0; i < BW_BODIES; ++i) {
        const u32 k = de_rng_range(r, 0, 8);
        de_body_type type = (de_body_type)de_rng_range(r, 0, 3);
        if (k == 4 || k == 5)
            type = de_rng_range(r, 0, 2) == 0 ? DE_BODY_STATIC : DE_BODY_KINEMATIC;
        de_body2d_desc bd2;
        memset(&bd2, 0, sizeof bd2);
        bd2.type = type;
        bd2.shape = w->shapes[k];
        bd2.transform = tu_xf(40.0 * de_rng_f64(r), 40.0 * de_rng_f64(r), 6.28f * de_rng_f32(r));
        bd2.filter = rand_filter(r);
        w->bodies[i] = de_body2d_create(p, &bd2);
        ASSERT_TRUE(DE_HANDLE_VALID(w->bodies[i]));
    }
}

/* The broadphase contract (broadphase2d.h), by brute force. */
static u32 brute_pairs(const de_physics2d_world *p, f32 dist, de__bp2d_pair *out, u32 cap) {
    u32 n = 0;
    const de__body2d_pool *b = &p->bodies;
    for (u32 i = 0; i < b->high_water; ++i) {
        if (!b->alive[i] || de__aabb2d_is_empty(b->world_aabb[i]))
            continue;
        const de_aabb2d bi = de__bp2d_predict_box(p, i);
        for (u32 j = i + 1u; j < b->high_water; ++j) {
            if (!b->alive[j] || de__aabb2d_is_empty(b->world_aabb[j]))
                continue;
            if (!de__bp2d_should_collide(p, i, j))
                continue;
            const de_aabb2d bj = de__bp2d_predict_box(p, j);
            if (!de__aabb2d_within(&bi, &bj, dist))
                continue;
            ASSERT_LT(n, cap);
            out[n].a = i;
            out[n].b = j;
            ++n;
        }
    }
    return n;
}

static void expect_pairs_exact(de_physics2d_world *p, f32 dist) {
    static de__bp2d_pair want[20000];
    const u32 nw = brute_pairs(p, dist, want, 20000);
    const de__bp2d_pair *got = NULL;
    u32 ng = 0;
    de__bp2d_pairs(p, &got, &ng);
    ASSERT_EQ(ng, nw);
    for (u32 i = 0; i < ng; ++i) {
        ASSERT_EQ(got[i].a, want[i].a);
        ASSERT_EQ(got[i].b, want[i].b);
    }
}

static void bw_move(bp_world *w, de_rng *r, u32 tick) {
    de_physics2d_world *p = w->p;
    for (u32 i = 0; i < BW_BODIES; ++i) {
        const u32 k = de_rng_range(r, 0, 10);
        de_transform2d xf;
        if (!de_body2d_get_transform(p, w->bodies[i], &xf))
            continue;
        if (k < 6) { /* small drift */
            xf.x += 0.3 * de_rng_f64(r) - 0.15;
            xf.y += 0.3 * de_rng_f64(r) - 0.15;
        } else if (k == 6) { /* teleport */
            xf.x = 40.0 * de_rng_f64(r);
            xf.y = 40.0 * de_rng_f64(r);
        }
        ASSERT_TRUE(de_body2d_set_transform(p, w->bodies[i], &xf));
    }
    /* Destroy and recreate a few (indices reused). */
    if (tick % 5u == 0u) {
        for (u32 c = 0; c < 4; ++c) {
            const u32 i = de_rng_range(r, 0, BW_BODIES);
            de_body2d_destroy(p, w->bodies[i]);
            de_body2d_desc bd;
            memset(&bd, 0, sizeof bd);
            bd.type = DE_BODY_DYNAMIC;
            bd.shape = w->shapes[de_rng_range(r, 0, 4)];
            bd.transform = tu_xf(40.0 * de_rng_f64(r), 40.0 * de_rng_f64(r), 0.0f);
            bd.filter = rand_filter(r);
            w->bodies[i] = de_body2d_create(p, &bd);
        }
    }
}

TEST(physics2d_broadphase, pairs_match_brute_force) {
    de_rng r = de_rng_seed(253, 11);
    static bp_world w;
    bw_build(&w, &r, 0.0f);
    const f32 dists[3] = {0.0f, 0.02f, 0.05f};
    for (u32 tick = 0; tick < 60; ++tick) {
        const f32 dist = dists[tick % 3u];
        de__bp2d_set_pair_distance(w.p, dist);
        if (tick % 4u == 1u) {
            /* Predictive growth on a few bodies (DENG-254 hook). */
            for (u32 k = 0; k < 10; ++k)
                de__bp2d_set_predict(w.p, de_rng_range(&r, 0, BW_BODIES),
                                     (de_vec2){de_rng_f32(&r), 0.0f},
                                     (de_vec2){0.0f, 2.0f * de_rng_f32(&r)});
        }
        ASSERT_EQ(de__bp2d_update(w.p), DE_OK);
        ASSERT_TRUE(de__bp2d_validate(w.p));
        expect_pairs_exact(w.p, dist);
        bw_move(&w, &r, tick);
    }
    de__bp2d_stats st;
    de__bp2d_stats_get(w.p, &st);
    EXPECT_GT(st.pairs, 50u);
    EXPECT_EQ(st.overflows, 0u);
    de_physics2d_destroy(w.p);
}

TEST(physics2d_broadphase, order_and_history_independence) {
    /* Same final bodies (same indices and poses), different margins, insertion boxes and move
     * histories: the same sorted pair list. */
    static bp_world a, b;
    de_rng ra = de_rng_seed(253, 12), rb = de_rng_seed(253, 12);
    bw_build(&a, &ra, 0.05f);
    bw_build(&b, &rb, 0.5f);
    de_rng hist = de_rng_seed(99, 1);
    for (u32 t = 0; t < 8; ++t) {
        bw_move(&b, &hist, 1u); /* no destroys (tick 1) */
        ASSERT_EQ(de__bp2d_update(b.p), DE_OK);
    }
    /* Put b's bodies back where a's are. */
    for (u32 i = 0; i < BW_BODIES; ++i) {
        de_transform2d xf;
        ASSERT_TRUE(de_body2d_get_transform(a.p, a.bodies[i], &xf));
        ASSERT_TRUE(de_body2d_set_transform(b.p, b.bodies[i], &xf));
    }
    ASSERT_EQ(de__bp2d_update(a.p), DE_OK);
    ASSERT_EQ(de__bp2d_update(b.p), DE_OK);
    const de__bp2d_pair *pa, *pb;
    u32 na, nb;
    de__bp2d_pairs(a.p, &pa, &na);
    de__bp2d_pairs(b.p, &pb, &nb);
    ASSERT_EQ(na, nb);
    ASSERT_GT(na, 10u);
    EXPECT_MEMEQ(pa, pb, sizeof(de__bp2d_pair) * na);
    /* The trees differ (different histories); only the output is equal. */
    TU_EXPECT_FNE(de__tree2d_area_cost(de__bp2d_tree(a.p, DE__BP2D_TREE_DYNAMIC)),
                  de__tree2d_area_cost(de__bp2d_tree(b.p, DE__BP2D_TREE_DYNAMIC)));
    de_physics2d_destroy(a.p);
    de_physics2d_destroy(b.p);
}

TEST(physics2d_broadphase, filters_and_body_types) {
    de_physics2d_world *p;
    de_physics2d_desc d;
    memset(&d, 0, sizeof d);
    d.max_bodies = 32;
    ASSERT_EQ(de_physics2d_create(&d, &p), DE_OK);
    const de_shape box = tu_box(p, 1.0f, 1.0f, 0.0f);
    /* Nine overlapping bodies at the origin. */
    const de_body s0 = tu_body(p, DE_BODY_STATIC, box, tu_xf(0, 0, 0), 0);
    const de_body s1 = tu_body(p, DE_BODY_STATIC, box, tu_xf(0.5, 0, 0), 0);
    const de_body k0 = tu_body(p, DE_BODY_KINEMATIC, box, tu_xf(0, 0.5, 0), 0);
    const de_body k1 = tu_body(p, DE_BODY_KINEMATIC, box, tu_xf(0.5, 0.5, 0), 0);
    const de_body d0 = tu_body(p, DE_BODY_DYNAMIC, box, tu_xf(0, 0.2, 0), 0);
    de_body2d_desc bd;
    memset(&bd, 0, sizeof bd);
    bd.type = DE_BODY_DYNAMIC;
    bd.shape = box;
    bd.filter.layer = 2;
    bd.filter.mask = 0xFFFFFFFEu; /* not layer 1 (the default layer of the others) */
    const de_body d1 = de_body2d_create(p, &bd);
    bd.filter.layer = 1;
    bd.filter.mask = 0xFFFFFFFFu;
    bd.filter.group = 5;
    const de_body g0 = de_body2d_create(p, &bd);
    const de_body g1 = de_body2d_create(p, &bd);
    const de_body none = tu_body(p, DE_BODY_DYNAMIC, (de_shape){0}, tu_xf(0, 0, 0), 0);
    (void)none;
    ASSERT_EQ(de__bp2d_update(p), DE_OK);
    const de__bp2d_pair *pairs;
    u32 n;
    de__bp2d_pairs(p, &pairs, &n);
    bool seen[16][16];
    memset(seen, 0, sizeof seen);
    for (u32 i = 0; i < n; ++i)
        seen[pairs[i].a][pairs[i].b] = true;
#define IDX(b)     tu_index(b)
#define SEEN(x, y) (IDX(x) < IDX(y) ? seen[IDX(x)][IDX(y)] : seen[IDX(y)][IDX(x)])
    EXPECT_FALSE(SEEN(s0, s1));
    EXPECT_FALSE(SEEN(k0, k1));
    EXPECT_FALSE(SEEN(s0, k0));
    EXPECT_TRUE(SEEN(d0, s0));
    EXPECT_TRUE(SEEN(d0, k1));
    EXPECT_TRUE(SEEN(d0, g0));
    EXPECT_FALSE(SEEN(d1, s0)); /* its mask excludes layer 1 */
    EXPECT_FALSE(SEEN(d1, d0));
    EXPECT_FALSE(SEEN(g0, g1)); /* same non-zero group */
    EXPECT_TRUE(SEEN(g0, s0));
#undef SEEN
#undef IDX
    /* Exactly the brute-force set. */
    expect_pairs_exact(p, 0.0f);
    de_physics2d_destroy(p);
}

TEST(physics2d_broadphase, overflow_is_deterministic_and_recovers) {
    de_physics2d_world *p;
    de_physics2d_desc d;
    memset(&d, 0, sizeof d);
    d.max_bodies = 64;
    ASSERT_EQ(de_physics2d_create(&d, &p), DE_OK);
    de__bp2d_desc bd = {0.0f, 1024u};
    ASSERT_EQ(de__bp2d_create(p, &bd), DE_OK);
    const de_shape box = tu_box(p, 1.0f, 1.0f, 0.0f);
    de_body bodies[60];
    /* 60 dynamic bodies on one spot: 1770 pairs > 1024. */
    for (u32 i = 0; i < 60; ++i)
        bodies[i] = tu_body(p, DE_BODY_DYNAMIC, box, tu_xf(0.01 * i, 0, 0), 0);
    EXPECT_EQ(de__bp2d_update(p), DE_ERR_OUT_OF_MEMORY);
    const de__bp2d_pair *pairs;
    u32 n;
    de__bp2d_pairs(p, &pairs, &n);
    EXPECT_LE(n, 1024u);
    ASSERT_GT(n, 0u);
    /* The kept pairs are a sorted subset of the full set, and the same subset every time. */
    static de__bp2d_pair want[2000];
    const u32 nw = brute_pairs(p, 0.0f, want, 2000);
    ASSERT_EQ(nw, 1770u);
    u32 w = 0;
    for (u32 i = 0; i < n; ++i) {
        if (i > 0)
            EXPECT_TRUE(pairs[i - 1].a < pairs[i].a ||
                        (pairs[i - 1].a == pairs[i].a && pairs[i - 1].b < pairs[i].b));
        while (w < nw && (want[w].a != pairs[i].a || want[w].b != pairs[i].b))
            ++w;
        ASSERT_LT(w, nw);
    }
    {
        de_physics2d_world *q;
        ASSERT_EQ(de_physics2d_create(&d, &q), DE_OK);
        ASSERT_EQ(de__bp2d_create(q, &bd), DE_OK);
        const de_shape qbox = tu_box(q, 1.0f, 1.0f, 0.0f);
        for (u32 i = 0; i < 60; ++i)
            (void)tu_body(q, DE_BODY_DYNAMIC, qbox, tu_xf(0.01 * i, 0, 0), 0);
        EXPECT_EQ(de__bp2d_update(q), DE_ERR_OUT_OF_MEMORY);
        const de__bp2d_pair *qp;
        u32 qn;
        de__bp2d_pairs(q, &qp, &qn);
        ASSERT_EQ(qn, n);
        EXPECT_MEMEQ(qp, pairs, sizeof(de__bp2d_pair) * n);
        de_physics2d_destroy(q);
    }
    /* Spread them out: the count drops below capacity and the next update recovers. */
    for (u32 i = 0; i < 60; ++i) {
        const de_transform2d xf = tu_xf(3.0 * (f64)i, 0, 0);
        ASSERT_TRUE(de_body2d_set_transform(p, bodies[i], &xf));
    }
    EXPECT_EQ(de__bp2d_update(p), DE_OK);
    expect_pairs_exact(p, 0.0f);
    de_physics2d_destroy(p);
}

/* ---- Queries -------------------------------------------------------------------------------- */

typedef struct body_list {
    u32 v[BW_BODIES + 8u];
    u32 n;
} body_list;

static bool body_cb(void *user, u32 body) {
    body_list *l = (body_list *)user;
    l->v[l->n++] = body;
    return true;
}

TEST(physics2d_broadphase, queries_find_fat_boxes) {
    de_rng r = de_rng_seed(253, 13);
    static bp_world w;
    bw_build(&w, &r, 0.0f);
    ASSERT_EQ(de__bp2d_update(w.p), DE_OK);
    for (u32 q = 0; q < 100; ++q) {
        const de_aabb2d box = rand_box(&r, 40.0f, 8.0f);
        de_collision_filter f = rand_filter(&r);
        static body_list got;
        got.n = 0;
        ASSERT_TRUE(de__bp2d_query_aabb(w.p, box, (q & 1u) ? &f : NULL, body_cb, &got));
        /* Every body whose world box overlaps (and passes the filter) is found. */
        const de__body2d_pool *b = &w.p->bodies;
        for (u32 i = 0; i < b->high_water; ++i) {
            if (!b->alive[i] || !de__aabb2d_overlap(&b->world_aabb[i], &box))
                continue;
            if ((q & 1u) && !de__p2d_filter_test(f, b->filter[i]))
                continue;
            bool found = false;
            for (u32 k = 0; k < got.n; ++k)
                found = found || got.v[k] == i;
            EXPECT_TRUE(found);
        }
    }
    de_physics2d_destroy(w.p);
}

/* ---- Chain segment bounds ------------------------------------------------------------------- */

typedef struct seg_list {
    u32 v[1100];
    u32 n;
} seg_list;

static bool seg_cb(void *user, u32 segment) {
    seg_list *l = (seg_list *)user;
    l->v[l->n++] = segment;
    return true;
}

static f32 seg_ray_cb(void *user, const de__tree2d_ray *ray, u32 segment, f32 max_fraction) {
    seg_list *l = (seg_list *)user;
    (void)ray;
    l->v[l->n++] = segment;
    return max_fraction;
}

TEST(physics2d_broadphase, chain_segment_bounds) {
    de_physics2d_world *p;
    ASSERT_EQ(de_physics2d_create(NULL, &p), DE_OK);
    de_rng r = de_rng_seed(253, 14);
    const u32 counts[6] = {2, 3, 4, 17, 64, 1001};
    static de_vec2 pts[1001];
    for (u32 t = 0; t < 6; ++t) {
        const u32 cnt = counts[t];
        for (u32 i = 0; i < cnt; ++i)
            pts[i] = (de_vec2){(f32)i * 0.5f, 3.0f * de_rng_f32(&r)};
        const bool loop = t == 3;
        const de_shape s = de_shape2d_chain_create(p, pts, cnt, loop);
        ASSERT_TRUE(DE_HANDLE_VALID(s));
        const de__chain2d *c = &de__shape2d_get(p, s)->u.chain;
        ASSERT_EQ(c->level_offset[c->level_count], de__chain2d_bounds_count(c->segment_count));
        /* The top box holds every segment. */
        const de_aabb2d top = c->bounds[c->level_offset[c->level_count - 1u]];
        for (u32 i = 0; i < c->segment_count; ++i) {
            const de__chain_segment *g = &c->segments[i];
            de_aabb2d sb = {de__v2_min(g->p1, g->p2), de__v2_max(g->p1, g->p2)};
            EXPECT_TRUE(de__aabb2d_contains(&top, &sb));
        }
        for (u32 q = 0; q < 200; ++q) {
            const f32 x0 = (f32)cnt * 0.5f * de_rng_f32(&r) - 1.0f;
            const f32 y0 = 4.0f * de_rng_f32(&r) - 0.5f;
            const de_aabb2d box = {{x0, y0}, {x0 + 2.0f * de_rng_f32(&r), y0 + de_rng_f32(&r)}};
            static seg_list want, got;
            want.n = got.n = 0;
            for (u32 i = 0; i < c->segment_count; ++i) {
                const de__chain_segment *g = &c->segments[i];
                const de_aabb2d sb = {de__v2_min(g->p1, g->p2), de__v2_max(g->p1, g->p2)};
                if (de__aabb2d_overlap(&sb, &box))
                    want.v[want.n++] = i;
            }
            ASSERT_TRUE(de__chain2d_query(c, box, seg_cb, &got));
            ASSERT_EQ(got.n, want.n);
            EXPECT_MEMEQ(got.v, want.v, sizeof(u32) * got.n); /* ascending, like brute force */
            /* Ray: every segment box the segment enters, in ascending order. */
            de__tree2d_ray ray;
            ray.origin = de__d2_make((f64)x0, -1.0);
            ray.delta = (de_vec2){4.0f * de_rng_f32(&r) - 2.0f, 6.0f};
            ray.max_fraction = 1.0f;
            want.n = got.n = 0;
            for (u32 i = 0; i < c->segment_count; ++i) {
                const de__chain_segment *g = &c->segments[i];
                const de_aabb2d sb = {de__v2_min(g->p1, g->p2), de__v2_max(g->p1, g->p2)};
                f64 te;
                if (de__ray2d_aabb(&ray, 1.0f, &sb, &te))
                    want.v[want.n++] = i;
            }
            TU_EXPECT_FEQ(de__chain2d_raycast(c, &ray, seg_ray_cb, &got), 1.0f);
            ASSERT_EQ(got.n, want.n);
            EXPECT_MEMEQ(got.v, want.v, sizeof(u32) * got.n);
        }
        EXPECT_EQ(de_shape2d_destroy(p, s), DE_OK);
    }
    de_physics2d_destroy(p);
}
