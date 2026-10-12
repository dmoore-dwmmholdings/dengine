/* test_structural.c: immediate structural changes: add, remove, despawn, spawn_with, and the
 * archetype edge cache (DENG-345). */
#include "ecs_test_util.h"

#include <stdlib.h>

/* Every row of every table points back at itself through its entity's record, and the rows add
 * up to `alive` entities. */
static void check_records(de_world *w, u32 alive) {
    u32 rows = 0;
    for (u32 ai = 0; ai < ecs_archetype_count(w); ++ai) {
        const ecs_archetype *a = ecs_archetype_at(w, ai);
        u32 in_table = 0;
        for (u32 c = 0; c < a->chunks.count; ++c) {
            const ecs_chunk *ch = ecs_table_chunk(a, c);
            if (c + 1u < a->chunks.count)
                ASSERT_EQ(ch->count, a->rows_per_chunk);
            ASSERT_GT(ch->count, 0u);
            for (u32 r = 0; r < ch->count; ++r) {
                const ecs_record *rec = ecs_entity_record(w, ecs_chunk_entities(ch)[r]);
                ASSERT_TRUE(rec != NULL);
                ASSERT_EQ(rec->archetype, ai);
                ASSERT_EQ(rec->chunk, c);
                ASSERT_EQ(rec->row, r);
            }
            in_table += ch->count;
        }
        ASSERT_EQ(in_table, a->count);
        rows += in_table;
    }
    ASSERT_EQ(rows, alive);
}

/* true if archetype `a` is exactly the sorted set `ids[0..n)`. */
static bool archetype_is(const ecs_archetype *a, const u32 *ids, u32 n) {
    if (a->component_count != n)
        return false;
    for (u32 i = 0; i < n; ++i)
        if (a->components[i] != ids[i])
            return false;
    return true;
}

static f32 fval(u32 i, u32 k) {
    return (f32)i * 8.0f + (f32)k * 0.5f;
}

/* ---- A component with counting hooks --------------------------------------------------------- */

typedef struct TCounted {
    u32 value;
} TCounted;

static int g_construct;
static int g_destruct;
static void counted_construct(void *o) {
    ((TCounted *)o)->value = 0x5EEDu;
    ++g_construct;
}
static void counted_destruct(void *o) {
    (void)o;
    ++g_destruct;
}
static void counted_copy(void *dst, const void *src) {
    memcpy(dst, src, sizeof(TCounted));
}

static de_component register_counted(ecs_fx *f) {
    static const de_field_info fields[] = {DE_FIELD(TCounted, value, DE_FK_U32, DE_FIELD_DEFAULT)};
    de_type_desc d = DE_TYPE_DESC(TCounted, 1, fields);
    d.is_pod = false;
    d.hooks.construct = counted_construct;
    d.hooks.destruct = counted_destruct;
    d.hooks.copy = counted_copy;
    const de_component c = de_component_register(f->w, de_type_register(f->types, &d), 0);
    g_construct = 0;
    g_destruct = 0;
    return c;
}

/* ---- structural.add_remove_data_intact --------------------------------------------------------
 */

#define DATA_N 300u

TEST_F(structural, add_remove_data_intact, ecs_fx) {
    /* A = pos, B = vel, C = health. Many entities, so the swap-removes of each move shuffle the
     * rows of the others. */
    static de_entity e[DATA_N];
    for (u32 i = 0; i < DATA_N; ++i) {
        e[i] = de_entity_spawn(f->w);
        TPos *p = (TPos *)de_entity_add(f->w, e[i], f->pos);
        ASSERT_TRUE(p != NULL);
        EXPECT_FLOAT_EQ(p->v.x, 0.0, 0.0); /* zeroed */
        p->v = (de_vec3){fval(i, 0), fval(i, 1), fval(i, 2)};
    }
    for (u32 k = 0; k < DATA_N; ++k) {
        const u32 i = (k * 7u) % DATA_N;
        TVel *v = (TVel *)de_entity_add(f->w, e[i], f->vel);
        ASSERT_TRUE(v != NULL);
        EXPECT_FLOAT_EQ(v->v.y, 0.0, 0.0);
        v->v = (de_vec3){fval(i, 3), fval(i, 4), fval(i, 5)};
    }
    for (u32 k = 0; k < DATA_N; ++k) {
        const u32 i = DATA_N - 1u - k;
        THealth *h = (THealth *)de_entity_add(f->w, e[i], f->health);
        ASSERT_TRUE(h != NULL);
        /* The pointer is valid until the next structural change: write through it now. */
        *h = (THealth){fval(i, 6), fval(i, 7)};
    }
    check_records(f->w, DATA_N);
    for (u32 k = 0; k < DATA_N; ++k)
        de_entity_remove(f->w, e[(k * 11u) % DATA_N], f->vel);
    check_records(f->w, DATA_N);

    const u32 ac[] = {f->pos.id, f->health.id};
    for (u32 i = 0; i < DATA_N; ++i) {
        const ecs_archetype *a = ecs_test_archetype_of(f->w, e[i]);
        EXPECT_TRUE(archetype_is(a, ac, 2));
        EXPECT_FALSE(de_entity_has(f->w, e[i], f->vel));
        const TPos *p = (const TPos *)de_entity_get_const(f->w, e[i], f->pos);
        const THealth *h = (const THealth *)de_entity_get_const(f->w, e[i], f->health);
        ASSERT_TRUE(p != NULL && h != NULL);
        EXPECT_FLOAT_EQ(p->v.x, fval(i, 0), 0.0);
        EXPECT_FLOAT_EQ(p->v.y, fval(i, 1), 0.0);
        EXPECT_FLOAT_EQ(p->v.z, fval(i, 2), 0.0);
        EXPECT_FLOAT_EQ(h->current, fval(i, 6), 0.0);
        EXPECT_FLOAT_EQ(h->max, fval(i, 7), 0.0);
    }
    /* {}, {A}, {A,B}, {A,B,C}, {A,C}: five archetypes, in operation order. */
    EXPECT_EQ(ecs_archetype_count(f->w), 5u);
    const u32 s1[] = {f->pos.id};
    const u32 s2[] = {f->pos.id, f->vel.id};
    const u32 s3[] = {f->pos.id, f->vel.id, f->health.id};
    EXPECT_TRUE(archetype_is(ecs_archetype_at(f->w, 1), s1, 1));
    EXPECT_TRUE(archetype_is(ecs_archetype_at(f->w, 2), s2, 2));
    EXPECT_TRUE(archetype_is(ecs_archetype_at(f->w, 3), s3, 3));
    EXPECT_TRUE(archetype_is(ecs_archetype_at(f->w, 4), ac, 2));
    EXPECT_EQ(ecs_archetype_at(f->w, 3)->count, 0u);

    /* Tags: a non-NULL placeholder, no column. Removing an absent component does nothing. */
    void *tag = de_entity_add(f->w, e[0], f->enemy);
    EXPECT_TRUE(tag != NULL);
    EXPECT_TRUE(de_entity_has(f->w, e[0], f->enemy));
    const u32 before = ecs_archetype_count(f->w);
    const ecs_record rec0 = *ecs_entity_record(f->w, e[1]);
    de_entity_remove(f->w, e[1], f->enemy);
    de_entity_remove(f->w, e[1], f->rot);
    const ecs_record rec1 = *ecs_entity_record(f->w, e[1]);
    EXPECT_EQ(rec0.archetype, rec1.archetype);
    EXPECT_EQ(rec0.chunk, rec1.chunk);
    EXPECT_EQ(rec0.row, rec1.row);
    EXPECT_EQ(ecs_archetype_count(f->w), before);
    de_entity_remove(f->w, e[0], f->enemy);
    EXPECT_FALSE(de_entity_has(f->w, e[0], f->enemy));

    /* Refusals: a dead entity, an unregistered component, a NULL world. Nothing changes. */
    const de_entity dead = de_entity_spawn(f->w);
    de_entity_despawn(f->w, dead);
    EXPECT_TRUE(de_entity_add(f->w, dead, f->pos) == NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    const de_component bogus = {77};
    EXPECT_TRUE(de_entity_add(f->w, e[0], bogus) == NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    const de_component zero = {0};
    EXPECT_TRUE(de_entity_add(f->w, e[0], zero) == NULL);
    EXPECT_TRUE(de_entity_add(NULL, e[0], f->pos) == NULL);
    de_entity_remove(f->w, dead, f->pos);
    de_entity_remove(f->w, e[0], bogus);
    de_entity_remove(NULL, e[0], f->pos);
    EXPECT_EQ(ecs_archetype_count(f->w), before);
    check_records(f->w, DATA_N);
}

/* ---- structural.add_existing_no_move ----------------------------------------------------------
 */

TEST_F(structural, add_existing_no_move, ecs_fx) {
    const de_component counted = register_counted(f);
    const de_entity a = de_entity_spawn(f->w);
    const de_entity b = de_entity_spawn(f->w);
    TPos *pa = (TPos *)de_entity_add(f->w, a, f->pos);
    ASSERT_TRUE(pa != NULL);
    pa->v.x = 42.0f;
    ASSERT_TRUE(de_entity_add(f->w, b, f->pos) != NULL);
    TCounted *ca = (TCounted *)de_entity_add(f->w, a, counted);
    ASSERT_TRUE(ca != NULL);
    ca->value = 7u;
    EXPECT_EQ(g_construct, 1);

    const ecs_record before = *ecs_entity_record(f->w, a);
    const u32 archetypes = ecs_archetype_count(f->w);
    ecs_graph_stats s0;
    ecs_graph_stats_get(f->w, &s0);

    pa = (TPos *)de_entity_get(f->w, a, f->pos);
    TPos *again = (TPos *)de_entity_add(f->w, a, f->pos);
    EXPECT_TRUE(again == pa);
    EXPECT_FLOAT_EQ(again->v.x, 42.0, 0.0);
    TCounted *cagain = (TCounted *)de_entity_add(f->w, a, counted);
    EXPECT_EQ(cagain->value, 7u); /* not re-constructed */
    EXPECT_EQ(g_construct, 1);
    EXPECT_EQ(g_destruct, 0);

    const ecs_record after = *ecs_entity_record(f->w, a);
    EXPECT_EQ(after.archetype, before.archetype);
    EXPECT_EQ(after.chunk, before.chunk);
    EXPECT_EQ(after.row, before.row);
    EXPECT_EQ(ecs_archetype_count(f->w), archetypes);
    ecs_graph_stats s1;
    ecs_graph_stats_get(f->w, &s1);
    EXPECT_EQ(s1.edge_hits, s0.edge_hits); /* no graph lookup at all */
    EXPECT_EQ(s1.edge_misses, s0.edge_misses);
    check_records(f->w, 2);
}

/* ---- structural.edge_cache_hit ----------------------------------------------------------------
 */

TEST_F(structural, edge_cache_hit, ecs_fx) {
    ecs_graph_stats s;
    const de_entity e1 = de_entity_spawn(f->w);
    ASSERT_TRUE(de_entity_add(f->w, e1, f->pos) != NULL);
    ASSERT_TRUE(de_entity_add(f->w, e1, f->vel) != NULL);
    ecs_graph_stats_get(f->w, &s);
    EXPECT_EQ(s.edge_misses, 2u);
    EXPECT_EQ(s.edge_hits, 0u);
    /* Each new edge also cached its reverse. */
    EXPECT_EQ(s.add_edges, 2u);
    EXPECT_EQ(s.remove_edges, 2u);
    const u32 archetypes = ecs_archetype_count(f->w);

    /* The same adds on another entity: answered by the cache, no set lookup. */
    const de_entity e2 = de_entity_spawn(f->w);
    ASSERT_TRUE(de_entity_add(f->w, e2, f->pos) != NULL);
    ASSERT_TRUE(de_entity_add(f->w, e2, f->vel) != NULL);
    ecs_graph_stats_get(f->w, &s);
    EXPECT_EQ(s.edge_misses, 2u);
    EXPECT_EQ(s.edge_hits, 2u);

    /* Removing back along the path uses the reverse edges. */
    de_entity_remove(f->w, e2, f->vel);
    de_entity_remove(f->w, e2, f->pos);
    ecs_graph_stats_get(f->w, &s);
    EXPECT_EQ(s.edge_misses, 2u);
    EXPECT_EQ(s.edge_hits, 4u);
    EXPECT_EQ(ecs_archetype_count(f->w), archetypes);
    EXPECT_EQ(ecs_test_archetype_of(f->w, e2)->index, ECS_EMPTY_ARCHETYPE);

    /* Another path to {pos, vel} (vel first) creates {vel} and finds the existing {pos, vel} by
     * its set: two misses, one new archetype. */
    ASSERT_TRUE(de_entity_add(f->w, e2, f->vel) != NULL);
    ASSERT_TRUE(de_entity_add(f->w, e2, f->pos) != NULL);
    ecs_graph_stats_get(f->w, &s);
    EXPECT_EQ(s.edge_misses, 4u);
    EXPECT_EQ(ecs_archetype_count(f->w), archetypes + 1u);
    EXPECT_TRUE(ecs_test_archetype_of(f->w, e2) == ecs_test_archetype_of(f->w, e1));

    /* Edges out of one archetype stay sorted by component id whatever the insertion order. */
    const de_component order[] = {f->rot, f->enemy, f->health};
    for (u32 i = 0; i < 3; ++i) {
        const de_entity x = de_entity_spawn(f->w);
        ASSERT_TRUE(de_entity_add(f->w, x, order[i]) != NULL);
    }
    const ecs_archetype *empty = ecs_archetype_at(f->w, ECS_EMPTY_ARCHETYPE);
    ASSERT_EQ(empty->add_edge_count, 5u);
    for (u32 i = 0; i + 1u < empty->add_edge_count; ++i)
        EXPECT_LT(empty->add_edges[i].component, empty->add_edges[i + 1u].component);
    for (u32 i = 0; i < empty->add_edge_count; ++i) {
        const ecs_archetype *t = ecs_archetype_at(f->w, empty->add_edges[i].target);
        EXPECT_EQ(t->component_count, 1u);
        EXPECT_EQ(t->components[0], empty->add_edges[i].component);
    }

    /* The internal lookups: a component already present (add) or absent (remove) is the
     * archetype itself and counts no lookup; an unregistered one is refused. */
    ecs_archetype *a12 = ecs_test_archetype_of(f->w, e1);
    ecs_archetype *out = NULL;
    ecs_graph_stats_get(f->w, &s);
    const u64 hits = s.edge_hits, misses = s.edge_misses;
    EXPECT_EQ(ecs_archetype_add_target(f->w, a12, f->pos.id, &out), DE_OK);
    EXPECT_TRUE(out == a12);
    EXPECT_EQ(ecs_archetype_remove_target(f->w, a12, f->rot.id, &out), DE_OK);
    EXPECT_TRUE(out == a12);
    EXPECT_EQ(ecs_archetype_add_target(f->w, a12, 99u, &out), DE_ERR_INVALID_ARGUMENT);
    ecs_graph_stats_get(f->w, &s);
    EXPECT_EQ(s.edge_hits, hits);
    EXPECT_EQ(s.edge_misses, misses);
}

/* ---- structural.despawn_reuse_generation ------------------------------------------------------
 */

TEST_F(structural, despawn_reuse_generation, ecs_fx) {
    const de_entity e = de_entity_spawn(f->w);
    ASSERT_TRUE(de_entity_add(f->w, e, f->pos) != NULL);
    de_entity_despawn(f->w, e);
    EXPECT_FALSE(de_entity_alive(f->w, e));
    EXPECT_TRUE(de_entity_get(f->w, e, f->pos) == NULL);
    EXPECT_FALSE(de_entity_has(f->w, e, f->pos));

    /* The index comes back with the next generation; the old handle stays dead. */
    const de_entity r = de_entity_spawn(f->w);
    EXPECT_EQ(de_entity_index(r), de_entity_index(e));
    EXPECT_EQ(de_entity_generation(r), de_entity_generation(e) + 1u);
    EXPECT_TRUE(de_entity_alive(f->w, r));
    EXPECT_FALSE(de_entity_alive(f->w, e));
    EXPECT_FALSE(de_entity_has(f->w, r, f->pos)); /* a fresh entity, in the empty archetype */

    /* Despawning a dead, stale or null handle is a no-op: r is untouched, nothing is freed. */
    const u32 free_before = f->w->free_indices.count;
    de_entity_despawn(f->w, e);
    de_entity_despawn(f->w, DE_ENTITY_NULL);
    de_entity_despawn(f->w, ecs_entity_make(12345u, 1u));
    de_entity_despawn(NULL, r);
    EXPECT_TRUE(de_entity_alive(f->w, r));
    EXPECT_EQ(f->w->free_indices.count, free_before);
    EXPECT_EQ(ecs_entity_despawn(f->w, e), DE_ERR_INVALID_ARGUMENT);

    /* Swap-remove: despawning the first row moves the last row in and updates its record. */
    de_entity row[5];
    for (u32 i = 0; i < 5; ++i) {
        row[i] = de_entity_spawn(f->w);
        THealth *h = (THealth *)de_entity_add(f->w, row[i], f->health);
        ASSERT_TRUE(h != NULL);
        h->current = (f32)i;
    }
    de_entity_despawn(f->w, row[0]);
    const ecs_record *moved = ecs_entity_record(f->w, row[4]);
    ASSERT_TRUE(moved != NULL);
    EXPECT_EQ(moved->row, 0u);
    de_entity_despawn(f->w, row[2]);
    check_records(f->w, 4);
    for (u32 i = 1; i < 5; ++i) {
        if (i == 2)
            continue;
        const THealth *h = (const THealth *)de_entity_get_const(f->w, row[i], f->health);
        ASSERT_TRUE(h != NULL);
        EXPECT_FLOAT_EQ(h->current, (f64)i, 0.0);
    }

    /* LIFO reuse: last freed first. */
    const de_entity n1 = de_entity_spawn(f->w);
    const de_entity n2 = de_entity_spawn(f->w);
    EXPECT_EQ(de_entity_index(n1), de_entity_index(row[2]));
    EXPECT_EQ(de_entity_index(n2), de_entity_index(row[0]));
    EXPECT_EQ(de_entity_generation(n2), de_entity_generation(row[0]) + 1u);
}

/* ---- structural.hooks_counts ------------------------------------------------------------------
 */

TEST_F(structural, hooks_counts, ecs_fx) {
    const de_component counted = register_counted(f);

    const de_entity e = de_entity_spawn(f->w);
    TCounted *c = (TCounted *)de_entity_add(f->w, e, counted);
    ASSERT_TRUE(c != NULL);
    EXPECT_EQ(c->value, 0x5EEDu); /* construct hook, not zero */
    EXPECT_EQ(g_construct, 1);
    EXPECT_EQ(g_destruct, 0);

    /* Moves of other components relocate the data: no hook runs. */
    ASSERT_TRUE(de_entity_add(f->w, e, f->pos) != NULL);
    ASSERT_TRUE(de_entity_add(f->w, e, f->enemy) != NULL);
    de_entity_remove(f->w, e, f->pos);
    EXPECT_EQ(g_construct, 1);
    EXPECT_EQ(g_destruct, 0);
    EXPECT_EQ(((const TCounted *)de_entity_get_const(f->w, e, counted))->value, 0x5EEDu);

    de_entity_remove(f->w, e, counted);
    EXPECT_EQ(g_destruct, 1);
    de_entity_remove(f->w, e, counted); /* absent: nothing */
    EXPECT_EQ(g_destruct, 1);

    ASSERT_TRUE(de_entity_add(f->w, e, counted) != NULL);
    EXPECT_EQ(g_construct, 2);
    de_entity_despawn(f->w, e);
    EXPECT_EQ(g_destruct, 2);
    de_entity_despawn(f->w, e); /* dead: nothing */
    EXPECT_EQ(g_destruct, 2);

    /* spawn_with constructs once per component; destroy destructs what is left. */
    const de_component set[] = {f->pos, counted, counted};
    de_entity s = DE_ENTITY_NULL;
    ASSERT_EQ(de_entity_spawn_with(f->w, set, 3, &s), DE_OK);
    EXPECT_EQ(g_construct, 3);
    for (u32 i = 0; i < 10; ++i) {
        const de_entity x = de_entity_spawn(f->w);
        ASSERT_TRUE(de_entity_add(f->w, x, counted) != NULL);
        if (i % 2u == 0)
            de_entity_despawn(f->w, x);
    }
    EXPECT_EQ(g_construct, 13);
    EXPECT_EQ(g_destruct, 7);
    ecs_fx_destroy_world(f);
    EXPECT_EQ(g_destruct, 13); /* every construct matched by exactly one destruct */
}

/* ---- structural.churn_1m ----------------------------------------------------------------------
 */

#define CHURN_CYCLES 1000000u
#define CHURN_LIVE   256u

TEST_F(structural, churn_1m, ecs_fx) {
    const de_component counted = register_counted(f);
    static de_entity live[CHURN_LIVE];
    /* Warm up: fill the ring and touch every archetype and edge the loop uses. */
    for (u32 i = 0; i < CHURN_LIVE; ++i) {
        live[i] = de_entity_spawn(f->w);
        ASSERT_TRUE(de_entity_add(f->w, live[i], f->pos) != NULL);
        ASSERT_TRUE(de_entity_add(f->w, live[i], counted) != NULL);
        ASSERT_TRUE(de_entity_add(f->w, live[i], f->enemy) != NULL);
        de_entity_remove(f->w, live[i], f->enemy);
    }
    const de_tracking_stats warm = de_tracking_allocator_stats(&f->track);
    const u32 records = f->w->records.count;
    const u32 archetypes = ecs_archetype_count(f->w);

    u32 bad = 0;
    for (u32 k = 0; k < CHURN_CYCLES; ++k) {
        const u32 slot = k % CHURN_LIVE;
        const de_entity old = live[slot];
        de_entity_despawn(f->w, old);
        bad += de_entity_alive(f->w, old);
        const de_entity e = de_entity_spawn(f->w);
        bad += de_entity_index(e) != de_entity_index(old); /* LIFO: the slot just freed */
        bad += de_entity_generation(e) != de_entity_generation(old) + 1u;
        bad += de_entity_add(f->w, e, f->pos) == NULL;
        bad += de_entity_add(f->w, e, counted) == NULL;
        if (k % 3u == 0) {
            bad += de_entity_add(f->w, e, f->enemy) == NULL;
            de_entity_remove(f->w, e, f->enemy);
        }
        live[slot] = e;
    }
    EXPECT_EQ(bad, 0u);
    check_records(f->w, CHURN_LIVE);
    EXPECT_EQ(f->w->records.count, records); /* indices are reused, never leaked */
    EXPECT_EQ(ecs_archetype_count(f->w), archetypes);
    EXPECT_EQ(g_construct - g_destruct, (int)CHURN_LIVE);

    /* Steady state allocates nothing: chunks come from the pool, edges are cached. */
    const de_tracking_stats st = de_tracking_allocator_stats(&f->track);
    EXPECT_EQ(st.alloc_count, warm.alloc_count);
    EXPECT_EQ(st.realloc_count, warm.realloc_count);
    EXPECT_EQ(st.bytes_live, warm.bytes_live);
    EXPECT_EQ(st.blocks_live, warm.blocks_live);
}

/* ---- structural.spawn_with --------------------------------------------------------------------
 */

TEST_F(structural, spawn_with, ecs_fx) {
    const de_component counted = register_counted(f);
    const u32 archetypes = ecs_archetype_count(f->w);

    const de_component set[] = {f->vel, counted, f->pos, f->enemy, f->pos};
    de_entity e = DE_ENTITY_NULL;
    ASSERT_EQ(de_entity_spawn_with(f->w, set, 5, &e), DE_OK);
    EXPECT_TRUE(de_entity_alive(f->w, e));
    /* Created directly in the target archetype: one new archetype, no intermediates. */
    EXPECT_EQ(ecs_archetype_count(f->w), archetypes + 1u);
    const u32 want[] = {f->pos.id, f->vel.id, f->enemy.id, counted.id};
    EXPECT_TRUE(archetype_is(ecs_test_archetype_of(f->w, e), want, 4));
    const TPos *p = (const TPos *)de_entity_get_const(f->w, e, f->pos);
    ASSERT_TRUE(p != NULL);
    EXPECT_FLOAT_EQ(p->v.x, 0.0, 0.0);
    EXPECT_EQ(((const TCounted *)de_entity_get_const(f->w, e, counted))->value, 0x5EEDu);
    EXPECT_EQ(g_construct, 1);
    EXPECT_TRUE(de_entity_has(f->w, e, f->enemy));

    /* The same set again, in another order: same archetype. */
    const de_component set2[] = {f->enemy, f->pos, counted, f->vel};
    de_entity e2 = DE_ENTITY_NULL;
    ASSERT_EQ(de_entity_spawn_with(f->w, set2, 4, &e2), DE_OK);
    EXPECT_TRUE(ecs_test_archetype_of(f->w, e2) == ecs_test_archetype_of(f->w, e));
    EXPECT_EQ(ecs_archetype_count(f->w), archetypes + 1u);

    /* n = 0: a plain spawn. */
    de_entity z = DE_ENTITY_NULL;
    ASSERT_EQ(de_entity_spawn_with(f->w, NULL, 0, &z), DE_OK);
    EXPECT_EQ(ecs_test_archetype_of(f->w, z)->index, ECS_EMPTY_ARCHETYPE);

    /* A long list (more ids than the stack buffer), duplicates collapse. */
    static de_component many[100];
    for (u32 i = 0; i < 100; ++i)
        many[i] = (i % 2u) ? f->rot : f->health;
    de_entity m = DE_ENTITY_NULL;
    ASSERT_EQ(de_entity_spawn_with(f->w, many, 100, &m), DE_OK);
    const u32 want_m[] = {f->health.id, f->rot.id};
    EXPECT_TRUE(archetype_is(ecs_test_archetype_of(f->w, m), want_m, 2));

    /* Refusals leave *out NULL and the world unchanged. */
    const u32 before = ecs_archetype_count(f->w);
    const u32 rows = f->w->records.count;
    const de_component bad[] = {f->pos, {77}};
    de_entity x = ecs_entity_make(1, 1);
    EXPECT_EQ(de_entity_spawn_with(f->w, bad, 2, &x), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(x.bits, 0u);
    x = ecs_entity_make(1, 1);
    EXPECT_EQ(de_entity_spawn_with(f->w, NULL, 1, &x), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(x.bits, 0u);
    EXPECT_EQ(de_entity_spawn_with(f->w, set, 1, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_entity_spawn_with(NULL, set, 1, &x), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecs_archetype_count(f->w), before);
    EXPECT_EQ(f->w->records.count, rows);
    check_records(f->w, 4);
}

/* ---- structural.oom_no_change -----------------------------------------------------------------
 */

TEST_F(structural, oom_no_change, ecs_fx) {
    /* Every allocation of a sequence of adds, removes and spawn_withs fails in turn. A failed
     * operation leaves the entity where it was, with its data; nothing leaks. */
    bool done = false;
    for (i64 n = 0; n < 200 && !done; ++n) {
        ecs_fx_destroy_world(f);
        ecs_fx_create_world(f, NULL);
        const de_entity e = de_entity_spawn(f->w);
        ASSERT_TRUE(de_entity_alive(f->w, e));
        de_tracking_allocator_fail_after(&f->track, n);
        bool ok = true;
        const de_component steps[] = {f->pos, f->vel, f->enemy, f->rot};
        for (u32 i = 0; ok && i < 4; ++i) {
            const ecs_record before = *ecs_entity_record(f->w, e);
            void *d = de_entity_add(f->w, e, steps[i]);
            if (d == NULL) {
                ok = false;
                EXPECT_EQ(de_error_last()->result, DE_ERR_OUT_OF_MEMORY);
                const ecs_record *now = ecs_entity_record(f->w, e);
                ASSERT_TRUE(now != NULL);
                EXPECT_EQ(now->archetype, before.archetype);
                EXPECT_FALSE(de_entity_has(f->w, e, steps[i]));
            } else if (steps[i].id == f->pos.id) {
                ((TPos *)d)->v.x = 3.0f;
            }
        }
        if (ok) {
            de_entity_remove(f->w, e, f->vel);
            ok = !de_entity_has(f->w, e, f->vel);
        }
        if (ok) {
            const de_component set[] = {f->health, f->vel};
            de_entity s = DE_ENTITY_NULL;
            ok = de_entity_spawn_with(f->w, set, 2, &s) == DE_OK;
            if (!ok)
                EXPECT_EQ(s.bits, 0u);
        }
        if (de_entity_has(f->w, e, f->pos)) {
            const TPos *p = (const TPos *)de_entity_get_const(f->w, e, f->pos);
            EXPECT_FLOAT_EQ(p->v.x, 3.0, 0.0);
        }
        EXPECT_TRUE(de_entity_alive(f->w, e));
        de_tracking_allocator_fail_after(&f->track, -1);
        /* Whatever failed, the world still works and the graph is consistent. */
        ASSERT_TRUE(de_entity_add(f->w, e, f->health) != NULL);
        done = ok;
    }
    EXPECT_TRUE(done);
}

/* ---- structural.deterministic_ids -------------------------------------------------------------
 */

/* FNV-1a, 64-bit: a fixed, portable checksum for the test vector. */
static u64 fnv1a(u64 h, u64 v) {
    for (u32 i = 0; i < 8; ++i) {
        h ^= (v >> (i * 8u)) & 0xFFu;
        h *= 0x100000001B3ull;
    }
    return h;
}

#define DET_STEPS 20000u
#define DET_SLOTS 512u

/* A fixed pseudo-random sequence of spawns, spawn_withs, adds, removes and despawns. Returns a
 * checksum of every entity id handed out and of the final archetype array (sets in creation
 * order). */
static u64 run_scenario(ecs_fx *f) {
    const de_component comps[] = {f->pos, f->vel, f->health, f->enemy, f->rot};
    static de_entity slots[DET_SLOTS];
    memset(slots, 0, sizeof slots);
    u64 h = 0xCBF29CE484222325ull;
    u32 rng = 0x2545F491u;
    for (u32 k = 0; k < DET_STEPS; ++k) {
        rng = rng * 1664525u + 1013904223u;
        const u32 slot = (rng >> 8) % DET_SLOTS;
        const u32 op = (rng >> 20) % 8u;
        const de_component c = comps[(rng >> 24) % 5u];
        de_entity *e = &slots[slot];
        if (e->bits == 0 || op == 0) {
            if (e->bits != 0)
                de_entity_despawn(f->w, *e);
            if (op % 2u) {
                const de_component set[] = {c, comps[(rng >> 4) % 5u]};
                if (de_entity_spawn_with(f->w, set, 2, e) != DE_OK)
                    return 0;
            } else {
                *e = de_entity_spawn(f->w);
            }
            h = fnv1a(h, e->bits);
        } else if (op < 5u) {
            if (de_entity_add(f->w, *e, c) == NULL)
                return 0;
        } else {
            de_entity_remove(f->w, *e, c);
        }
    }
    for (u32 i = 0; i < ecs_archetype_count(f->w); ++i) {
        const ecs_archetype *a = ecs_archetype_at(f->w, i);
        h = fnv1a(h, ((u64)i << 32) | a->count);
        for (u32 j = 0; j < a->component_count; ++j)
            h = fnv1a(h, a->components[j]);
    }
    for (u32 i = 0; i < DET_SLOTS; ++i)
        h = fnv1a(h, slots[i].bits);
    return h;
}

TEST_F(structural, deterministic_ids, ecs_fx) {
    /* A small explicit vector: ids depend only on the sequence of spawns and despawns. */
    const de_entity a = de_entity_spawn(f->w); /* index 0, gen 1 */
    const de_entity b = de_entity_spawn(f->w); /* index 1, gen 1 */
    ASSERT_TRUE(de_entity_add(f->w, a, f->pos) != NULL);
    const de_component set[] = {f->vel, f->pos};
    de_entity c = DE_ENTITY_NULL; /* index 2, gen 1 */
    ASSERT_EQ(de_entity_spawn_with(f->w, set, 2, &c), DE_OK);
    de_entity_despawn(f->w, b);
    de_entity_despawn(f->w, a);
    const de_entity d = de_entity_spawn(f->w); /* index 0, gen 2 (last freed) */
    const de_entity e = de_entity_spawn(f->w); /* index 1, gen 2 */
    const de_entity g = de_entity_spawn(f->w); /* index 3, gen 1 */
    de_entity_despawn(f->w, d);
    const de_entity i = de_entity_spawn(f->w); /* index 0, gen 3 */
    EXPECT_EQ(a.bits, 0x0000000100000000ull);
    EXPECT_EQ(b.bits, 0x0000000100000001ull);
    EXPECT_EQ(c.bits, 0x0000000100000002ull);
    EXPECT_EQ(d.bits, 0x0000000200000000ull);
    EXPECT_EQ(e.bits, 0x0000000200000001ull);
    EXPECT_EQ(g.bits, 0x0000000100000003ull);
    EXPECT_EQ(i.bits, 0x0000000300000000ull);

    /* A long scenario: the same checksum on every run, in fresh worlds, and the golden value
     * (the same on every platform: integer-only operations). */
    ecs_fx_destroy_world(f);
    ecs_fx_create_world(f, NULL);
    const u64 first = run_scenario(f);
    EXPECT_NE(first, 0u);
    check_records(f->w, DET_SLOTS);
    for (u32 run = 0; run < 2; ++run) {
        ecs_fx_destroy_world(f);
        ecs_fx_create_world(f, NULL);
        EXPECT_EQ(run_scenario(f), first);
    }
    EXPECT_EQ(first, 0xDF773AB606B3E829ull); /* golden (DENG-345) */
}
