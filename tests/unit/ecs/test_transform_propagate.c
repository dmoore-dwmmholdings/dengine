/* test_transform_propagate.c: the built-in transform propagation system (DENG-351; SPEC §7.1,
 * §5.5, §10.2 step 4, §5.7).
 *
 * XFORM_FOREST_TRACE is the test vector of xform.bitwise_across_workers: a hash of the bits of
 * every global transform of a fixed 100 000-entity forest. Its inputs are exact binary values and
 * de_transform_mul is bit-exact across platforms (math.transform.golden_bitwise), so it must hold
 * on every machine and for every worker count; a change is a determinism bug, never a reason to
 * update it. */
#define _CRT_SECURE_NO_WARNINGS /* getenv (CI, DE_CI) */
#include "ecs_test_util.h"

#include "ecs/hierarchy_internal.h"

#include <dengine/de_platform.h>

#include <stdio.h>
#include <stdlib.h>

/** Hash of xform.bitwise_across_workers (see the file comment). */
#define XFORM_FOREST_TRACE 0x8F335C9DC283A891ull

static de_task_system *make_ts(u32 workers) {
    de_fp_env_init_thread();
    de_task_system_desc d;
    memset(&d, 0, sizeof d);
    d.worker_count = workers;
    d.background_worker_count = 1;
    de_task_system *ts = NULL;
    ASSERT_EQ(de_task_system_create(&d, &ts), DE_OK);
    return ts;
}

static void rebuild_world(ecs_fx *f, de_task_system *ts) {
    ecs_fx_destroy_world(f);
    de_world_desc d;
    memset(&d, 0, sizeof d);
    d.tasks = ts;
    ecs_fx_create_world(f, &d);
}

static de_builtin_component_ids builtins(ecs_fx *f) {
    const de_builtin_component_ids ids = de_builtin_components(f->w);
    ASSERT_TRUE(DE_HANDLE_VALID(ids.transform) && DE_HANDLE_VALID(ids.children));
    return ids;
}

static void run_post(ecs_fx *f) {
    ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_POST_UPDATE, 0.0f, 0.0f), DE_OK);
}

static de_quat quat(f32 x, f32 y, f32 z, f32 w) {
    de_quat q;
    q.x = x;
    q.y = y;
    q.z = z;
    q.w = w;
    return q;
}

static de_transform xf(f64 px, f64 py, f64 pz, de_quat r, f32 sx, f32 sy, f32 sz) {
    de_transform t;
    memset(&t, 0, sizeof t);
    t.position.x = px;
    t.position.y = py;
    t.position.z = pz;
    t.rotation = r;
    t.scale.x = sx;
    t.scale.y = sy;
    t.scale.z = sz;
    return t;
}

/* Bitwise equality of the fields (not the padding). */
static bool xf_bits_eq(const de_transform *a, const de_transform *b) {
    return memcmp(&a->position, &b->position, sizeof a->position) == 0 &&
           memcmp(&a->rotation, &b->rotation, sizeof a->rotation) == 0 &&
           memcmp(&a->scale, &b->scale, sizeof a->scale) == 0;
}

static de_entity spawn_xf(ecs_fx *f, const de_builtin_component_ids *ids, de_transform local,
                          bool with_global) {
    const de_component comps[2] = {ids->transform, ids->global_transform};
    de_entity e;
    ASSERT_EQ(de_entity_spawn_with(f->w, comps, with_global ? 2u : 1u, &e), DE_OK);
    *(DeTransform *)de_entity_get(f->w, e, ids->transform) = local;
    return e;
}

static const DeGlobalTransform *global_of(ecs_fx *f, const de_builtin_component_ids *ids,
                                          de_entity e) {
    return (const DeGlobalTransform *)de_entity_get_const(f->w, e, ids->global_transform);
}

/* The reference world transform of `e`: top-down de_transform_mul along its ancestors (mul is
 * not associative under non-uniform scale, so the order matters). */
static de_transform reference_global(ecs_fx *f, const de_builtin_component_ids *ids, de_entity e) {
    de_entity chain[64];
    u32 n = 0;
    for (de_entity a = e; a.bits != 0 && n < 64; a = de_entity_parent(f->w, a))
        chain[n++] = a;
    de_transform g = *(const DeTransform *)de_entity_get_const(f->w, chain[n - 1], ids->transform);
    for (u32 i = n - 1; i-- > 0;) {
        const DeTransform *l =
            (const DeTransform *)de_entity_get_const(f->w, chain[i], ids->transform);
        g = de_transform_mul(&g, l);
    }
    return g;
}

static ecs_hier_stats stats_delta(ecs_fx *f, ecs_hier_stats *prev) {
    ecs_hier_stats now, d;
    ecs_hier_stats_get(f->w, &now);
    d.runs = now.runs - prev->runs;
    d.visited = now.visited - prev->visited;
    d.recomputed = now.recomputed - prev->recomputed;
    d.globals_added = now.globals_added - prev->globals_added;
    *prev = now;
    return d;
}

/* ---- xform.three_levels_reference ------------------------------------------------------------ */

/* Moves the root before propagation in the same PostUpdate run. */
static void mover_fn(de_system_ctx *ctx) {
    de_entity root;
    memcpy(&root, ctx->user, sizeof root);
    const de_builtin_component_ids ids = de_builtin_components(ctx->world);
    DeTransform *t = (DeTransform *)de_entity_get(ctx->world, root, ids.transform);
    t->position.x += 1.0;
}

TEST_F(xform, three_levels_reference, ecs_fx) {
    const de_builtin_component_ids ids = builtins(f);
    /* Exact unit quaternions (0.6^2 + 0.8^2 = 0.28^2 + 0.96^2 = 1) and non-uniform scales. */
    const de_entity r = spawn_xf(
        f, &ids, xf(10.0, -4.0, 2.5, quat(0.0f, 0.0f, 0.6f, 0.8f), 2.0f, 1.0f, 0.5f), true);
    const de_entity c =
        spawn_xf(f, &ids, xf(1.0, 2.0, 3.0, quat(0.6f, 0.0f, 0.0f, 0.8f), 1.0f, 3.0f, 1.0f), false);
    const de_entity g = spawn_xf(
        f, &ids, xf(-0.5, 0.25, 4.0, quat(0.0f, 0.28f, 0.0f, 0.96f), 0.5f, 0.5f, 2.0f), false);
    const de_entity c2 = spawn_xf(
        f, &ids, xf(0.0, 0.0, -1.0, quat(0.0f, 0.0f, 0.0f, 1.0f), 1.0f, 1.0f, 1.0f), false);
    ASSERT_EQ(de_entity_set_parent(f->w, c, r), DE_OK);
    ASSERT_EQ(de_entity_set_parent(f->w, g, c), DE_OK);
    ASSERT_EQ(de_entity_set_parent(f->w, c2, r), DE_OK);

    ecs_hier_stats prev;
    ecs_hier_stats_get(f->w, &prev);
    run_post(f);
    ecs_hier_stats d = stats_delta(f, &prev);
    EXPECT_EQ(d.runs, 1u);
    EXPECT_EQ(d.visited, 4u);
    EXPECT_EQ(d.recomputed, 4u);
    EXPECT_EQ(d.globals_added, 3u); /* c, g, c2 had none: added with the value, same run */

    const DeTransform *lr = (const DeTransform *)de_entity_get_const(f->w, r, ids.transform);
    const DeTransform *lc = (const DeTransform *)de_entity_get_const(f->w, c, ids.transform);
    const DeTransform *lg = (const DeTransform *)de_entity_get_const(f->w, g, ids.transform);
    const de_transform er = *lr;
    const de_transform ec = de_transform_mul(&er, lc);
    const de_transform eg = de_transform_mul(&ec, lg);
    ASSERT_TRUE(global_of(f, &ids, r) != NULL && global_of(f, &ids, c) != NULL &&
                global_of(f, &ids, g) != NULL && global_of(f, &ids, c2) != NULL);
    EXPECT_TRUE(xf_bits_eq(global_of(f, &ids, r), &er));
    EXPECT_TRUE(xf_bits_eq(global_of(f, &ids, c), &ec));
    EXPECT_TRUE(xf_bits_eq(global_of(f, &ids, g), &eg));
    const de_transform ec2 = reference_global(f, &ids, c2);
    EXPECT_TRUE(xf_bits_eq(global_of(f, &ids, c2), &ec2));
    /* Sanity of the reference: a child's origin is exact (only its axes ignore the shear of a
     * rotated child under a non-uniform scale, see de_transform_mul). */
    const de_dvec3 origin = de_transform_point(&er, lc->position);
    EXPECT_FLOAT_EQ(global_of(f, &ids, c)->position.x, origin.x, 1e-12);
    EXPECT_FLOAT_EQ(global_of(f, &ids, c)->position.y, origin.y, 1e-12);
    EXPECT_FLOAT_EQ(global_of(f, &ids, c)->position.z, origin.z, 1e-12);

    /* Nothing changed: nothing recomputed, values kept. */
    run_post(f);
    d = stats_delta(f, &prev);
    EXPECT_EQ(d.recomputed, 0u);
    EXPECT_EQ(d.visited, 4u);
    EXPECT_TRUE(xf_bits_eq(global_of(f, &ids, g), &eg));

    /* A system before propagation moves the root: seen in the same run, whole tree recomputed. */
    de_system_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.name = "mover";
    sd.schedule = DE_SCHEDULE_POST_UPDATE;
    sd.fn = mover_fn;
    de_entity mover_root = r;
    sd.user = &mover_root;
    const de_query_term mt[1] = {{ids.transform, DE_WRITE}};
    const de_query_desc mq = {mt, 1, NULL, 0};
    sd.queries = &mq;
    sd.query_count = 1;
    const char *before[1] = {DE_TRANSFORM_PROPAGATE_SYSTEM};
    sd.before = before;
    sd.before_count = 1;
    ASSERT_TRUE(DE_HANDLE_VALID(de_system_add(f->w, &sd)));
    run_post(f);
    d = stats_delta(f, &prev);
    EXPECT_EQ(d.recomputed, 4u);
    EXPECT_FLOAT_EQ(global_of(f, &ids, r)->position.x, 11.0, 0.0);
    const de_transform eg2 = reference_global(f, &ids, g);
    EXPECT_TRUE(xf_bits_eq(global_of(f, &ids, g), &eg2));
}

/* ---- xform.large_world_precision ------------------------------------------------------------- */

TEST_F(xform, large_world_precision, ecs_fx) {
    const de_builtin_component_ids ids = builtins(f);
    const de_quat id = quat(0.0f, 0.0f, 0.0f, 1.0f);
    const f32 h = 0.70710678f; /* 90 degrees about +Z (rounded to f32) */
    const de_entity r = spawn_xf(f, &ids, xf(100000.0, 100000.0, 0.0, id, 1, 1, 1), true);
    const de_entity c = spawn_xf(f, &ids, xf(0.001, 0.0, 0.0, id, 1, 1, 1), true);
    const de_entity g = spawn_xf(f, &ids, xf(0.0, 0.001, 0.0, id, 1, 1, 1), true);
    const de_entity rr =
        spawn_xf(f, &ids, xf(100000.0, 100000.0, 0.0, quat(0, 0, h, h), 1, 1, 1), true);
    const de_entity rc = spawn_xf(f, &ids, xf(0.001, 0.0, 0.0, id, 1, 1, 1), true);
    ASSERT_EQ(de_entity_set_parent(f->w, c, r), DE_OK);
    ASSERT_EQ(de_entity_set_parent(f->w, g, c), DE_OK);
    ASSERT_EQ(de_entity_set_parent(f->w, rc, rr), DE_OK);
    run_post(f);

    /* Sub-millimetre (in fact sub-micrometre): the 1 mm offsets survive 100 km out. */
    const de_dvec3 pc = global_of(f, &ids, c)->position;
    const de_dvec3 pg = global_of(f, &ids, g)->position;
    EXPECT_FLOAT_EQ(pc.x - 100000.0, 0.001, 1e-9);
    EXPECT_FLOAT_EQ(pc.y - 100000.0, 0.0, 1e-9);
    EXPECT_FLOAT_EQ(pg.x - 100000.0, 0.001, 1e-9);
    EXPECT_FLOAT_EQ(pg.y - 100000.0, 0.001, 1e-9);
    /* Camera-relative conversion near the root keeps the millimetre in f32 too. */
    const de_vec3 rel = de_dv3_relative(pc, global_of(f, &ids, r)->position);
    EXPECT_FLOAT_EQ(rel.x, 0.001, 1e-9);
    /* An f32 position would have lost it: the f32 spacing at 1e5 is about 8 mm. */
    const f32 as_f32 = (f32)pc.x;
    EXPECT_FLOAT_EQ(as_f32 - 100000.0f, 0.0, 0.0);

    /* Rotated root: the offset turns to +Y and keeps its length. */
    const de_dvec3 prc = global_of(f, &ids, rc)->position;
    EXPECT_FLOAT_EQ(prc.x - 100000.0, 0.0, 1e-9);
    EXPECT_FLOAT_EQ(prc.y - 100000.0, 0.001, 1e-9);
}

/* ---- xform.bitwise_across_workers ------------------------------------------------------------ */

#define FOREST_ROOTS 1000u
#define FOREST_TREE  100u /* 1 root, 9 children, 10 grandchildren each */

static de_transform forest_local(u32 i) {
    static const f32 q[4][4] = {
        {0.0f, 0.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, 0.6f, 0.8f},
        {0.6f, 0.0f, 0.0f, 0.8f},
        {0.0f, 0.28f, 0.0f, 0.96f},
    };
    static const f32 s[4][3] = {{1, 1, 1}, {2, 2, 2}, {1, 2, 0.5f}, {0.5f, 1, 1}};
    const u32 k = i % 4u, m = (i / 4u) % 4u;
    /* Exact binary values: multiples of 1/4 and 1/2 within +-100 km. */
    return xf((f64)(i % 97u) * 0.5 + (f64)(i / FOREST_TREE) * 64.0, (f64)(i % 89u) * 0.25 - 10.0,
              (f64)(i % 13u) * 2.0, quat(q[k][0], q[k][1], q[k][2], q[k][3]), s[m][0], s[m][1],
              s[m][2]);
}

/* Builds the forest (entities in `es`, spawn order) in the fixture's current world. */
static void build_forest(ecs_fx *f, const de_builtin_component_ids *ids, de_entity *es) {
    for (u32 t = 0; t < FOREST_ROOTS; ++t) {
        const u32 base = t * FOREST_TREE;
        for (u32 j = 0; j < FOREST_TREE; ++j)
            es[base + j] = spawn_xf(f, ids, forest_local(base + j), true);
        for (u32 j = 1; j < 10u; ++j) /* children of the root */
            ASSERT_EQ(de_entity_set_parent(f->w, es[base + j], es[base]), DE_OK);
        for (u32 j = 10; j < FOREST_TREE; ++j) /* grandchildren: 10 per child */
            ASSERT_EQ(de_entity_set_parent(f->w, es[base + j], es[base + 1u + (j - 10u) / 10u]),
                      DE_OK);
    }
}

static u64 fnv(u64 h, const void *p, size_t n) {
    const u8 *b = (const u8 *)p;
    for (size_t i = 0; i < n; ++i) {
        h ^= b[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

static u64 hash_globals(ecs_fx *f, const de_builtin_component_ids *ids, const de_entity *es,
                        u32 n) {
    u64 h = 0xCBF29CE484222325ull;
    for (u32 i = 0; i < n; ++i) {
        const DeGlobalTransform *g = global_of(f, ids, es[i]);
        if (g == NULL)
            return 0;
        h = fnv(h, &es[i].bits, sizeof es[i].bits);
        h = fnv(h, &g->position, sizeof g->position);
        h = fnv(h, &g->rotation, sizeof g->rotation);
        h = fnv(h, &g->scale, sizeof g->scale);
    }
    return h;
}

static bool env_set(const char *name) {
    const char *v = getenv(name);
    return v != NULL && v[0] != '\0' && strcmp(v, "0") != 0 && strcmp(v, "false") != 0;
}
static bool timing_is_loose(void) {
    bool loose = env_set("CI") || env_set("DE_CI");
#if defined(DE_DEBUG) || defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    loose = true;
#endif
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) ||                         \
    __has_feature(undefined_behavior_sanitizer)
    loose = true;
#endif
#endif
    return loose;
}

TEST_F(xform, bitwise_across_workers, ecs_fx) {
    enum { N = FOREST_ROOTS * FOREST_TREE, CONFIGS = 4 };
    const u32 workers[CONFIGS] = {0, 1, 2, 8}; /* 0: no task system (serial) */
    de_entity *es = (de_entity *)malloc((size_t)N * sizeof(de_entity));
    ASSERT_TRUE(es != NULL);
    u64 hashes[CONFIGS];
    u64 full_ns[CONFIGS], idle_ns[CONFIGS];
    for (u32 k = 0; k < CONFIGS; ++k) {
        de_task_system *ts = workers[k] ? make_ts(workers[k]) : NULL;
        if (ts == NULL)
            de_fp_env_init_thread();
        rebuild_world(f, ts);
        const de_builtin_component_ids ids = builtins(f);
        build_forest(f, &ids, es);
        ecs_hier_stats prev;
        ecs_hier_stats_get(f->w, &prev);
        u64 t0 = de_time_now_ns();
        run_post(f); /* everything new: every global computed */
        full_ns[k] = de_time_now_ns() - t0;
        const ecs_hier_stats d1 = stats_delta(f, &prev);
        t0 = de_time_now_ns();
        run_post(f); /* nothing changed: walked, not recomputed */
        idle_ns[k] = de_time_now_ns() - t0;
        const ecs_hier_stats d2 = stats_delta(f, &prev);
        EXPECT_EQ(d1.recomputed, (u64)N);
        EXPECT_EQ(d2.recomputed, 0u);
        EXPECT_EQ(d2.visited, (u64)N);
        hashes[k] = hash_globals(f, &ids, es, N);
        /* Spot check against the reference. */
        for (u32 i = 0; i < N; i += 997u) {
            const de_transform ref = reference_global(f, &ids, es[i]);
            EXPECT_TRUE(xf_bits_eq(global_of(f, &ids, es[i]), &ref));
        }
        ecs_fx_destroy_world(f);
        if (ts != NULL)
            de_task_system_destroy(ts);
    }
    for (u32 k = 0; k < CONFIGS; ++k) {
        printf("[BENCH] xform.bitwise_across_workers: %u entities (%u roots), workers %u: full "
               "propagation %.3f ms, unchanged %.3f ms; hash 0x%016llX\n",
               (unsigned)N, (unsigned)FOREST_ROOTS, (unsigned)workers[k], (double)full_ns[k] / 1e6,
               (double)idle_ns[k] / 1e6, (unsigned long long)hashes[k]);
        EXPECT_EQ(hashes[k], hashes[0]);
    }
    EXPECT_EQ(hashes[0], XFORM_FOREST_TRACE);
    /* Budget: a full propagation of the 100k forest on the serial path and with 8 workers. */
    const bool loose = timing_is_loose();
    const u64 bound = loose ? 1000000000ull : 10000000ull;
    printf("[BENCH] xform budget: full propagation < %s\n", loose ? "loose 1000 ms" : "10 ms");
    EXPECT_LT(full_ns[0], bound);
    EXPECT_LT(full_ns[3], bound);
    free(es);
    /* The fixture's teardown destroys a world: give it an empty one. */
    rebuild_world(f, NULL);
}

/* ---- xform.skip_unchanged_subtrees ----------------------------------------------------------- */

#define SKIP_ROOTS 50u
#define SKIP_TREE  13u /* 1 root, 3 children, 3 grandchildren each */

/* Every global equals the top-down reference, bit for bit. */
static bool all_match_reference(ecs_fx *f, const de_builtin_component_ids *ids, const de_entity *es,
                                u32 n) {
    for (u32 i = 0; i < n; ++i) {
        if (!de_entity_alive(f->w, es[i]))
            continue;
        const DeGlobalTransform *g = global_of(f, ids, es[i]);
        const de_transform ref = reference_global(f, ids, es[i]);
        if (g == NULL || !xf_bits_eq(g, &ref)) {
            printf("xform: entity %u does not match its reference\n", i);
            return false;
        }
    }
    return true;
}

TEST_F(xform, skip_unchanged_subtrees, ecs_fx) {
    de_task_system *ts = make_ts(4);
    rebuild_world(f, ts);
    const de_builtin_component_ids ids = builtins(f);
    enum { N = SKIP_ROOTS * SKIP_TREE };
    de_entity es[N];
    for (u32 t = 0; t < SKIP_ROOTS; ++t) {
        const u32 b = t * SKIP_TREE;
        for (u32 j = 0; j < SKIP_TREE; ++j)
            es[b + j] = spawn_xf(f, &ids, forest_local(b + j), (j % 2u) == 0);
        for (u32 j = 1; j < 4u; ++j)
            ASSERT_EQ(de_entity_set_parent(f->w, es[b + j], es[b]), DE_OK);
        for (u32 j = 4; j < SKIP_TREE; ++j)
            ASSERT_EQ(de_entity_set_parent(f->w, es[b + j], es[b + 1u + (j - 4u) / 3u]), DE_OK);
    }
    ecs_hier_stats prev;
    ecs_hier_stats_get(f->w, &prev);
    run_post(f);
    ecs_hier_stats d = stats_delta(f, &prev);
    EXPECT_EQ(d.recomputed, (u64)N);
    EXPECT_EQ(d.globals_added, (u64)(SKIP_ROOTS * 6u)); /* odd j had no DeGlobalTransform */
    EXPECT_TRUE(all_match_reference(f, &ids, es, N));

    run_post(f);
    d = stats_delta(f, &prev);
    EXPECT_EQ(d.recomputed, 0u);
    EXPECT_EQ(d.visited, (u64)N);

    /* One leaf (tree 7, grandchild 0): only it. */
    const u32 t7 = 7u * SKIP_TREE;
    ((DeTransform *)de_entity_get(f->w, es[t7 + 4u], ids.transform))->position.z += 1.0;
    run_post(f);
    d = stats_delta(f, &prev);
    EXPECT_EQ(d.recomputed, 1u);
    EXPECT_TRUE(all_match_reference(f, &ids, es, N));

    /* A middle node: it and its 3 children. */
    ((DeTransform *)de_entity_get(f->w, es[t7 + 2u], ids.transform))->scale.x = 3.0f;
    run_post(f);
    d = stats_delta(f, &prev);
    EXPECT_EQ(d.recomputed, 4u);
    EXPECT_TRUE(all_match_reference(f, &ids, es, N));

    /* A root: its whole tree. */
    de_entity_mark_changed(f->w, es[0], ids.transform);
    run_post(f);
    d = stats_delta(f, &prev);
    EXPECT_EQ(d.recomputed, (u64)SKIP_TREE);

    /* Reparent a middle node (with 3 children) under a leaf of another tree: 4 recomputed. */
    ASSERT_EQ(de_entity_set_parent(f->w, es[t7 + 3u], es[SKIP_TREE + 5u]), DE_OK);
    run_post(f);
    d = stats_delta(f, &prev);
    EXPECT_EQ(d.recomputed, 4u);
    EXPECT_TRUE(all_match_reference(f, &ids, es, N));

    /* Detach a leaf: it becomes a root, global = local. */
    ASSERT_EQ(de_entity_set_parent(f->w, es[t7 + 5u], DE_ENTITY_NULL), DE_OK);
    run_post(f);
    d = stats_delta(f, &prev);
    EXPECT_EQ(d.recomputed, 1u);
    EXPECT_TRUE(
        xf_bits_eq(global_of(f, &ids, es[t7 + 5u]),
                   (const DeTransform *)de_entity_get_const(f->w, es[t7 + 5u], ids.transform)));

    /* Despawn a middle node: its 3 children become roots. */
    de_entity_despawn(f->w, es[t7 + 1u]);
    run_post(f);
    d = stats_delta(f, &prev);
    EXPECT_EQ(d.recomputed, 2u); /* es[t7+4..6] minus the detached es[t7+5] */
    EXPECT_TRUE(all_match_reference(f, &ids, es, N));

    /* A child that loses its DeTransform stops the walk; getting one back recomputes it. */
    de_entity_remove(f->w, es[2u * SKIP_TREE + 1u], ids.transform);
    run_post(f);
    d = stats_delta(f, &prev);
    EXPECT_EQ(d.recomputed, 0u);
    EXPECT_EQ(d.visited, (u64)(N - 1u /* despawned */ - 1u /* no transform */ - 3u /* below */));
    *(DeTransform *)de_entity_add(f->w, es[2u * SKIP_TREE + 1u], ids.transform) =
        forest_local(2u * SKIP_TREE + 1u);
    run_post(f);
    d = stats_delta(f, &prev);
    EXPECT_EQ(d.recomputed, 4u);
    EXPECT_TRUE(all_match_reference(f, &ids, es, N));

    printf("[xform] skip: unchanged runs visit every entity and recompute none\n");
    ecs_fx_destroy_world(f);
    de_task_system_destroy(ts);
    rebuild_world(f, NULL);
}
