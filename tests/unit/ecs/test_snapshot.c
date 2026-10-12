/* test_snapshot.c: world snapshots: take, restore, size (DENG-352, SPEC §7.4). */
#define _CRT_SECURE_NO_WARNINGS /* getenv (CI, DE_CI) */
#include "ecs_test_util.h"

#include <dengine/de_platform.h>

#include <stdio.h>
#include <stdlib.h>

/* ---- Helpers --------------------------------------------------------------------------------- */

/* A non-snapshot data component (TNote) registered after the fixture's five (id 6). */
typedef struct TNote {
    u32 value;
} TNote;

static de_component register_note(ecs_fx *f, u32 flags) {
    static const de_field_info k_fields[] = {DE_FIELD(TNote, value, DE_FK_U32, DE_FIELD_DEFAULT)};
    de_type_id t = de_type_find(f->types, "TNote");
    if (!DE_HANDLE_VALID(t))
        t = de_type_register(f->types, &DE_TYPE_DESC(TNote, 1, k_fields));
    ASSERT_TRUE(DE_HANDLE_VALID(t));
    const de_component c = de_component_register(f->w, t, flags);
    ASSERT_TRUE(DE_HANDLE_VALID(c));
    return c;
}

static void set_pos(ecs_fx *f, de_entity e, f32 x, f32 y, f32 z) {
    TPos *p = (TPos *)de_entity_get(f->w, e, f->pos);
    ASSERT_TRUE(p != NULL);
    p->v.x = x;
    p->v.y = y;
    p->v.z = z;
}

/* Every alive record points at a row that holds its entity; tables hold only alive entities;
 * the free list holds free, non-retired indices once each. */
static void check_world_consistent(de_world *w) {
    u64 alive = 0;
    for (u32 i = 0; i < w->records.count; ++i) {
        const ecs_record *rec = (const ecs_record *)de_array_at(&w->records, i);
        if (rec->archetype == ECS_INVALID_INDEX)
            continue;
        ++alive;
        ASSERT_LT(rec->archetype, ecs_archetype_count(w));
        const ecs_archetype *a = ecs_archetype_at(w, rec->archetype);
        ASSERT_LT(rec->chunk, a->chunks.count);
        const ecs_chunk *ch = ecs_table_chunk(a, rec->chunk);
        ASSERT_LT(rec->row, ch->count);
        ASSERT_EQ(ecs_chunk_entities(ch)[rec->row].bits, ecs_entity_make(i, rec->generation).bits);
    }
    u64 rows = 0;
    for (u32 ai = 0; ai < ecs_archetype_count(w); ++ai) {
        const ecs_archetype *a = ecs_archetype_at(w, ai);
        u32 n = 0;
        for (u32 c = 0; c < a->chunks.count; ++c) {
            const u32 cnt = ecs_table_chunk(a, c)->count;
            ASSERT_TRUE(cnt > 0);
            if (c + 1u < a->chunks.count)
                ASSERT_EQ(cnt, a->rows_per_chunk); /* dense */
            n += cnt;
        }
        ASSERT_EQ(n, a->count);
        rows += a->count;
    }
    ASSERT_EQ(rows, alive);
    ASSERT_GE(w->free_indices.capacity, w->records.capacity);
    const u32 *fl = (const u32 *)w->free_indices.data;
    for (u32 k = 0; k < w->free_indices.count; ++k) {
        ASSERT_LT(fl[k], w->records.count);
        const ecs_record *rec = (const ecs_record *)de_array_at(&w->records, fl[k]);
        ASSERT_EQ(rec->archetype, ECS_INVALID_INDEX);
        ASSERT_NE(rec->generation, 0u);
        for (u32 j = 0; j < k; ++j)
            ASSERT_NE(fl[j], fl[k]);
    }
}

/* Table order of every archetype: per archetype its count, then, for archetypes with a
 * DE_COMP_SNAPSHOT component, its entity ids in row order (restore puts those rows back in
 * take-time order; tables without snapshot components keep their own order). */
typedef struct world_dump {
    u32 archetypes;
    u32 n;
    u64 v[8192];
} world_dump;

static void dump_tables(de_world *w, world_dump *d) {
    d->archetypes = ecs_archetype_count(w);
    d->n = 0;
    for (u32 ai = 0; ai < d->archetypes; ++ai) {
        const ecs_archetype *a = ecs_archetype_at(w, ai);
        ASSERT_LT(d->n + 1u + a->count, (u32)(sizeof d->v / sizeof d->v[0]));
        d->v[d->n++] = a->count;
        bool has_snap = false;
        for (u32 i = 0; i < a->component_count; ++i)
            has_snap |= (ecs_component_get(w, a->components[i])->flags & DE_COMP_SNAPSHOT) != 0;
        for (u32 c = 0; has_snap && c < a->chunks.count; ++c) {
            const ecs_chunk *ch = ecs_table_chunk(a, c);
            for (u32 r = 0; r < ch->count; ++r)
                d->v[d->n++] = ecs_chunk_entities(ch)[r].bits;
        }
    }
}

/* The id the next spawn will get (LIFO free list, else a new index). */
static de_entity next_spawn_id(const de_world *w) {
    if (w->free_indices.count > 0) {
        const u32 i = ((const u32 *)w->free_indices.data)[w->free_indices.count - 1u];
        return ecs_entity_make(i, ((const ecs_record *)de_array_at(&w->records, i))->generation);
    }
    return ecs_entity_make(w->records.count, 1u);
}

#define RT_N 600u

/* The roundtrip world: five layouts ({pos}, {pos, vel}, {pos, vel, health}, {pos, rot}, {}),
 * the non-snapshot tag on every 7th entity, every 11th entity despawned. `out` gets every id. */
static void build_rt_world(ecs_fx *f, de_entity *out) {
    for (u32 i = 0; i < RT_N; ++i) {
        de_component comps[4];
        u32 n = 0;
        switch (i % 5u) {
        case 0:
            comps[n++] = f->pos;
            break;
        case 1:
            comps[n++] = f->pos;
            comps[n++] = f->vel;
            break;
        case 2:
            comps[n++] = f->pos;
            comps[n++] = f->vel;
            comps[n++] = f->health;
            break;
        case 3:
            comps[n++] = f->pos;
            comps[n++] = f->rot;
            break;
        default:
            break;
        }
        if (i % 7u == 0)
            comps[n++] = f->enemy;
        ASSERT_EQ(de_entity_spawn_with(f->w, comps, n, &out[i]), DE_OK);
        if (i % 5u != 4u)
            set_pos(f, out[i], (f32)i, (f32)(2u * i), (f32)(3u * i) * 0.5f);
        TVel *v = (TVel *)de_entity_get(f->w, out[i], f->vel);
        if (v != NULL)
            v->v.y = (f32)i * 0.25f;
        THealth *h = (THealth *)de_entity_get(f->w, out[i], f->health);
        if (h != NULL) {
            h->current = (f32)(i % 100u);
            h->max = 100.0f;
        }
        TRot *r = (TRot *)de_entity_get(f->w, out[i], f->rot);
        if (r != NULL)
            r->q.w = 1.0f - (f32)i * 0.001f;
    }
    for (u32 i = 3; i < RT_N; i += 11u)
        de_entity_despawn(f->w, out[i]);
}

/* ---- Tests ----------------------------------------------------------------------------------- */

TEST_F(snap, roundtrip_after_mutation, ecs_fx) {
    static de_entity ids[RT_N];
    static world_dump before, after;
    build_rt_world(f, ids);
    f->w->tick = 41;
    check_world_consistent(f->w);

    const u64 ck_snap = de_world_checksum(f->w, DE_COMP_SNAPSHOT);
    const u64 ck_all = de_world_checksum(f->w, 0xFFFFFFFFu);
    const u64 ck_ent = de_world_checksum(f->w, 0);
    dump_tables(f->w, &before);
    const de_entity next = next_spawn_id(f->w);

    de_world_snapshot snap;
    ASSERT_EQ(de_world_snapshot_take(f->w, NULL, &snap), DE_OK);
    ASSERT_TRUE(snap.data != NULL);
    ASSERT_EQ(snap.size, de_world_snapshot_size(f->w));
    ASSERT_EQ(snap.tick, 41u);

    /* Mutate: data, moves (add and remove snapshot components), spawns, despawns, index reuse,
     * entities without snapshot components gaining some. Tagged entities keep their structure:
     * a re-created entity would lose the non-snapshot tag (snap.nonsnapshot_components_kept). */
    f->w->tick = 77;
    for (u32 i = 0; i < RT_N; ++i) {
        TPos *p = (TPos *)de_entity_get(f->w, ids[i], f->pos);
        if (p != NULL)
            p->v.x += 1.0f;
    }
    u32 removed = 0, added = 0, despawned = 0;
    for (u32 i = 1; i < RT_N; ++i) {
        if (!de_entity_alive(f->w, ids[i]) || i % 7u == 0)
            continue;
        if (i % 5u == 1u && removed < 50u && (i / 5u) % 2u == 0) {
            de_entity_remove(f->w, ids[i], f->vel);
            ++removed;
        } else if (i % 5u == 0u && added < 30u) {
            ASSERT_TRUE(de_entity_add(f->w, ids[i], f->health) != NULL);
            ASSERT_TRUE(de_entity_add(f->w, ids[i], f->rot) != NULL);
            ++added;
        } else if (i % 5u == 4u) {
            ASSERT_TRUE(de_entity_add(f->w, ids[i], f->pos) != NULL);
        } else if (i % 5u == 2u && despawned < 80u) {
            de_entity_despawn(f->w, ids[i]);
            ++despawned;
        }
    }
    for (u32 k = 0; k < 100u; ++k) {
        const de_component c2[2] = {f->pos, f->vel};
        de_entity e;
        ASSERT_EQ(de_entity_spawn_with(f->w, c2, 2, &e), DE_OK);
        set_pos(f, e, -1.0f, -2.0f, -3.0f);
        if (k % 3u == 0)
            de_entity_despawn(f->w, e);
    }
    ASSERT_NE(de_world_checksum(f->w, DE_COMP_SNAPSHOT), ck_snap);
    ASSERT_NE(de_world_checksum(f->w, 0), ck_ent);

    ASSERT_EQ(de_world_snapshot_restore(f->w, &snap), DE_OK);
    check_world_consistent(f->w);
    EXPECT_EQ(de_world_checksum(f->w, DE_COMP_SNAPSHOT), ck_snap);
    EXPECT_EQ(de_world_checksum(f->w, 0xFFFFFFFFu), ck_all);
    EXPECT_EQ(de_world_checksum(f->w, 0), ck_ent);
    EXPECT_EQ(de_world_tick(f->w), 41u);

    /* Same table contents and row order in every archetype that existed at take; the ones made
     * since are empty. */
    dump_tables(f->w, &after);
    ASSERT_GE(after.archetypes, before.archetypes);
    ASSERT_MEMEQ(after.v, before.v, (size_t)before.n * sizeof(u64));
    for (u32 n = before.n; n < after.n; ++n)
        ASSERT_EQ(after.v[n], 0u);

    ecs_snapshot_stats st;
    ecs_snapshot_stats_get(f->w, &st);
    EXPECT_EQ(st.restores, 1u);
    EXPECT_EQ(st.loose_sections, 0u);
    EXPECT_GT(st.ordered_sections, 0u);
    EXPECT_GT(st.recreated, 0u);
    EXPECT_GT(st.despawned, 0u);
    EXPECT_GT(st.moved, 0u);

    /* The allocator state is the take-time one: the next spawn gets the same id. */
    const de_entity e = de_entity_spawn(f->w);
    EXPECT_EQ(e.bits, next.bits);
    de_entity_despawn(f->w, e);

    /* Restoring again changes nothing structurally: every section is fast. */
    const u32 sections = ((const u32 *)snap.data)[9]; /* snap_header.section_count */
    ASSERT_EQ(de_world_snapshot_restore(f->w, &snap), DE_OK);
    ecs_snapshot_stats st2;
    ecs_snapshot_stats_get(f->w, &st2);
    EXPECT_EQ(st2.fast_sections - st.fast_sections, sections);
    EXPECT_EQ(st2.row_copies, st.row_copies);
    EXPECT_EQ(st2.recreated, st.recreated);
    EXPECT_EQ(st2.moved, st.moved);
    EXPECT_EQ(de_world_checksum(f->w, 0xFFFFFFFFu), ck_all);

    /* Into another world with the same component registration. */
    de_world *w2 = NULL;
    de_world_desc d;
    memset(&d, 0, sizeof d);
    d.types = f->types;
    d.chunk_bytes = 2048; /* other chunk placement */
    ASSERT_EQ(de_world_create(&d, &w2), DE_OK);
    EXPECT_EQ(de_component_register(w2, f->t_pos, DE_COMP_SNAPSHOT).id, 1u);
    EXPECT_EQ(de_component_register(w2, f->t_vel, DE_COMP_SNAPSHOT).id, 2u);
    EXPECT_EQ(de_component_register(w2, f->t_health, DE_COMP_SNAPSHOT).id, 3u);
    EXPECT_EQ(de_component_register(w2, f->t_enemy, DE_COMP_TAG).id, 4u);
    EXPECT_EQ(de_component_register(w2, f->t_rot, DE_COMP_SNAPSHOT).id, 5u);
    EXPECT_EQ(de_world_snapshot_restore(w2, &snap), DE_OK);
    check_world_consistent(w2);
    EXPECT_EQ(de_world_checksum(w2, DE_COMP_SNAPSHOT), ck_snap);
    EXPECT_EQ(de_world_checksum(w2, 0), ck_ent);
    de_world_destroy(w2);

    de_world_snapshot_free(NULL, &snap);
    EXPECT_TRUE(snap.data == NULL);
    EXPECT_EQ(snap.size, 0u);
}

TEST_F(snap, handles_valid_after_restore, ecs_fx) {
    enum { N = 2000 };
    static de_entity ids[N];
    for (u32 i = 0; i < N; ++i) {
        const de_component c = i % 2u ? f->pos : f->vel;
        ASSERT_EQ(de_entity_spawn_with(f->w, &c, 1, &ids[i]), DE_OK);
    }
    for (u32 i = 0; i < N; i += 3u)
        de_entity_despawn(f->w, ids[i]);
    u32 free_before[N];
    const u32 free_count = f->w->free_indices.count;
    memcpy(free_before, f->w->free_indices.data, (size_t)free_count * sizeof(u32));

    de_world_snapshot snap;
    ASSERT_EQ(de_world_snapshot_take(f->w, &f->alloc, &snap), DE_OK);

    /* Despawn everything, then spawn twice as many: indices are reused with new generations. */
    for (u32 i = 0; i < N; ++i) {
        if (de_entity_alive(f->w, ids[i]))
            de_entity_despawn(f->w, ids[i]);
    }
    static de_entity later[2 * N];
    for (u32 i = 0; i < 2 * N; ++i)
        later[i] = de_entity_spawn(f->w);

    ASSERT_EQ(de_world_snapshot_restore(f->w, &snap), DE_OK);
    check_world_consistent(f->w);
    for (u32 i = 0; i < N; ++i) {
        const bool should = i % 3u != 0;
        ASSERT_EQ(de_entity_alive(f->w, ids[i]), should);
        if (should) {
            const ecs_record *rec = ecs_entity_record(f->w, ids[i]);
            ASSERT_EQ(rec->generation, de_entity_generation(ids[i]));
            ASSERT_TRUE(de_entity_has(f->w, ids[i], i % 2u ? f->pos : f->vel));
            ASSERT_FALSE(de_entity_has(f->w, ids[i], i % 2u ? f->vel : f->pos));
        }
    }
    for (u32 i = 0; i < 2 * N; ++i)
        ASSERT_FALSE(de_entity_alive(f->w, later[i]));
    ASSERT_EQ(f->w->records.count, (u32)N);
    ASSERT_EQ(f->w->free_indices.count, free_count);
    ASSERT_MEMEQ(f->w->free_indices.data, free_before, (size_t)free_count * sizeof(u32));
    de_world_snapshot_free(&f->alloc, &snap);
}

TEST_F(snap, memcpy_fast_path, ecs_fx) {
    enum { N = 5000 };
    static de_entity ids[N];
    for (u32 i = 0; i < N; ++i) {
        const de_component c3[3] = {f->pos, f->vel, f->rot};
        const de_component c2[2] = {f->pos, f->health};
        if (i % 4u == 0)
            ASSERT_EQ(de_entity_spawn_with(f->w, c2, 2, &ids[i]), DE_OK);
        else
            ASSERT_EQ(de_entity_spawn_with(f->w, c3, 3, &ids[i]), DE_OK);
        set_pos(f, ids[i], (f32)i, 0.0f, 1.0f);
    }
    const u64 ck = de_world_checksum(f->w, DE_COMP_SNAPSHOT);
    de_world_snapshot snap;
    ASSERT_EQ(de_world_snapshot_take(f->w, NULL, &snap), DE_OK);

    /* Only data changes: positions, rotations, health. */
    for (u32 i = 0; i < N; ++i) {
        set_pos(f, ids[i], -1.0f, -1.0f, -1.0f);
        TRot *r = (TRot *)de_entity_get(f->w, ids[i], f->rot);
        if (r != NULL)
            r->q.x = 0.5f;
        THealth *h = (THealth *)de_entity_get(f->w, ids[i], f->health);
        if (h != NULL)
            h->max = 7.0f;
    }
    ASSERT_NE(de_world_checksum(f->w, DE_COMP_SNAPSHOT), ck);

    ASSERT_EQ(de_world_snapshot_restore(f->w, &snap), DE_OK);
    EXPECT_EQ(de_world_checksum(f->w, DE_COMP_SNAPSHOT), ck);

    /* Two sections, both fast: one memcpy per column and chunk, nothing else. */
    ecs_snapshot_stats st;
    ecs_snapshot_stats_get(f->w, &st);
    EXPECT_EQ(st.fast_sections, 2u);
    EXPECT_EQ(st.ordered_sections + st.loose_sections, 0u);
    EXPECT_EQ(st.row_copies, 0u);
    EXPECT_EQ(st.moved + st.recreated + st.despawned + st.row_swaps, 0u);
    u64 expect_blocks = 0;
    for (u32 ai = 0; ai < ecs_archetype_count(f->w); ++ai) {
        const ecs_archetype *a = ecs_archetype_at(f->w, ai);
        expect_blocks += (u64)a->column_count * a->chunks.count;
    }
    EXPECT_EQ(st.block_copies, expect_blocks);

    /* Restored rows are marked changed at the current change tick. */
    f->w->change_tick = 9;
    ASSERT_EQ(de_world_snapshot_restore(f->w, &snap), DE_OK);
    const ecs_archetype *a = ecs_test_archetype_of(f->w, ids[1]);
    const u32 col = ecs_archetype_column(a, f->pos.id);
    for (u32 c = 0; c < a->chunks.count; ++c) {
        const ecs_chunk *ch = ecs_table_chunk(a, c);
        for (u32 r = 0; r < ch->count; ++r)
            ASSERT_EQ(ecs_chunk_changed(a, ch, col)[r], 9u);
    }
    de_world_snapshot_free(NULL, &snap);
}

TEST_F(snap, nonsnapshot_components_kept, ecs_fx) {
    const de_component note = register_note(f, 0); /* data, not in snapshots */
    de_entity e[6];
    for (u32 i = 0; i < 6; ++i) {
        const de_component c[3] = {f->pos, note, f->enemy};
        ASSERT_EQ(de_entity_spawn_with(f->w, c, 3, &e[i]), DE_OK);
        set_pos(f, e[i], (f32)i, 0.0f, 0.0f);
        ((TNote *)de_entity_get(f->w, e[i], note))->value = 100u + i;
    }
    de_world_snapshot snap;
    ASSERT_EQ(de_world_snapshot_take(f->w, NULL, &snap), DE_OK);
    const u64 ck = de_world_checksum(f->w, DE_COMP_SNAPSHOT);

    ((TNote *)de_entity_get(f->w, e[0], note))->value = 7u; /* kept */
    set_pos(f, e[0], 50.0f, 50.0f, 50.0f);                  /* restored */
    de_entity_remove(f->w, e[1], f->pos);                   /* pos comes back */
    ASSERT_TRUE(de_entity_add(f->w, e[2], f->vel) != NULL); /* vel goes away */
    de_entity_remove(f->w, e[3], f->enemy);                 /* tag stays removed */
    de_entity_despawn(f->w, e[4]);                          /* re-created, snapshot part only */
    de_entity extra;
    const de_component c2[2] = {f->pos, note};
    ASSERT_EQ(de_entity_spawn_with(f->w, c2, 2, &extra), DE_OK); /* despawned again */
    ASSERT_EQ(extra.bits >> 32, 2u); /* reused e[4]'s index with the next generation */

    ASSERT_EQ(de_world_snapshot_restore(f->w, &snap), DE_OK);
    check_world_consistent(f->w);
    EXPECT_EQ(de_world_checksum(f->w, DE_COMP_SNAPSHOT), ck);
    EXPECT_FALSE(de_entity_alive(f->w, extra));

    EXPECT_EQ(((TNote *)de_entity_get(f->w, e[0], note))->value, 7u);
    EXPECT_FLOAT_EQ(((TPos *)de_entity_get(f->w, e[0], f->pos))->v.x, 0.0, 0.0);
    ASSERT_TRUE(de_entity_has(f->w, e[1], f->pos));
    EXPECT_FLOAT_EQ(((TPos *)de_entity_get(f->w, e[1], f->pos))->v.x, 1.0, 0.0);
    EXPECT_TRUE(de_entity_has(f->w, e[1], note) && de_entity_has(f->w, e[1], f->enemy));
    EXPECT_FALSE(de_entity_has(f->w, e[2], f->vel));
    EXPECT_TRUE(de_entity_has(f->w, e[2], note) && de_entity_has(f->w, e[2], f->enemy));
    EXPECT_FALSE(de_entity_has(f->w, e[3], f->enemy));
    EXPECT_TRUE(de_entity_has(f->w, e[3], f->pos));
    ASSERT_TRUE(de_entity_alive(f->w, e[4]));
    EXPECT_TRUE(de_entity_has(f->w, e[4], f->pos));
    EXPECT_FALSE(de_entity_has(f->w, e[4], note));
    EXPECT_FALSE(de_entity_has(f->w, e[4], f->enemy));
    EXPECT_FLOAT_EQ(((TPos *)de_entity_get(f->w, e[4], f->pos))->v.x, 4.0, 0.0);
    EXPECT_EQ(((TNote *)de_entity_get(f->w, e[5], note))->value, 105u);

    ecs_snapshot_stats st;
    ecs_snapshot_stats_get(f->w, &st);
    EXPECT_EQ(st.despawned, 1u);
    EXPECT_EQ(st.recreated, 1u);
    de_world_snapshot_free(NULL, &snap);
}

/* A tag flagged DE_COMP_SNAPSHOT (TFlag). */
typedef struct TFlag {
    u8 unused;
} TFlag;

TEST_F(snap, snapshot_tags_and_drift, ecs_fx) {
    static const de_field_info k_fields[] = {DE_FIELD(TFlag, unused, DE_FK_U8, DE_FIELD_DEFAULT)};
    const de_type_id tf = de_type_register(f->types, &DE_TYPE_DESC(TFlag, 1, k_fields));
    const de_component flag = de_component_register(f->w, tf, DE_COMP_TAG | DE_COMP_SNAPSHOT);
    const de_component note = register_note(f, 0);
    ASSERT_TRUE(DE_HANDLE_VALID(flag));

    /* a: {pos, note}; b: {pos}; c: {flag} (a section without data columns); d: {pos, flag}. */
    de_entity a, b, c, d;
    const de_component ca[2] = {f->pos, note}, cd[2] = {f->pos, flag};
    ASSERT_EQ(de_entity_spawn_with(f->w, ca, 2, &a), DE_OK);
    ASSERT_EQ(de_entity_spawn_with(f->w, &f->pos, 1, &b), DE_OK);
    ASSERT_EQ(de_entity_spawn_with(f->w, &flag, 1, &c), DE_OK);
    ASSERT_EQ(de_entity_spawn_with(f->w, cd, 2, &d), DE_OK);
    set_pos(f, a, 1.0f, 0.0f, 0.0f);
    set_pos(f, b, 2.0f, 0.0f, 0.0f);
    set_pos(f, d, 4.0f, 0.0f, 0.0f);
    const u64 ck = de_world_checksum(f->w, DE_COMP_SNAPSHOT);
    de_world_snapshot snap;
    ASSERT_EQ(de_world_snapshot_take(f->w, NULL, &snap), DE_OK);
    EXPECT_EQ(((const u32 *)snap.data)[9], 4u); /* four sections */

    /* Drift: `a` loses its non-snapshot component, so after the restore it shares {pos} with
     * `b`, whose section can then no longer claim the table. Snapshot tags move too. */
    de_entity_remove(f->w, a, note);
    de_entity_remove(f->w, c, flag);
    de_entity_remove(f->w, d, flag);
    ASSERT_TRUE(de_entity_add(f->w, b, flag) != NULL);
    set_pos(f, a, 9.0f, 9.0f, 9.0f);
    set_pos(f, b, 9.0f, 9.0f, 9.0f);
    ASSERT_NE(de_world_checksum(f->w, DE_COMP_SNAPSHOT), ck);

    ASSERT_EQ(de_world_snapshot_restore(f->w, &snap), DE_OK);
    check_world_consistent(f->w);
    EXPECT_EQ(de_world_checksum(f->w, DE_COMP_SNAPSHOT), ck);
    EXPECT_FALSE(de_entity_has(f->w, a, note)); /* not a snapshot component: stays removed */
    EXPECT_TRUE(de_entity_has(f->w, c, flag));
    EXPECT_TRUE(de_entity_has(f->w, d, flag));
    EXPECT_FALSE(de_entity_has(f->w, b, flag));
    EXPECT_EQ(ecs_entity_record(f->w, a)->archetype, ecs_entity_record(f->w, b)->archetype);
    EXPECT_FLOAT_EQ(((TPos *)de_entity_get(f->w, a, f->pos))->v.x, 1.0, 0.0);
    EXPECT_FLOAT_EQ(((TPos *)de_entity_get(f->w, b, f->pos))->v.x, 2.0, 0.0);

    ecs_snapshot_stats st;
    ecs_snapshot_stats_get(f->w, &st);
    EXPECT_EQ(st.loose_sections, 1u); /* b's section: a's section claimed the {pos} table */
    EXPECT_GE(st.row_copies, 1u);
    de_world_snapshot_free(NULL, &snap);
}

TEST_F(snap, empty_world, ecs_fx) {
    /* No entities at all. */
    de_world_snapshot snap;
    ASSERT_EQ(de_world_snapshot_take(f->w, NULL, &snap), DE_OK);
    EXPECT_EQ(snap.size, de_world_snapshot_size(f->w));
    EXPECT_LE(snap.size, 64u);
    de_entity e = de_entity_spawn(f->w);
    ASSERT_TRUE(de_entity_add(f->w, e, f->pos) != NULL);
    ASSERT_EQ(de_world_snapshot_restore(f->w, &snap), DE_OK);
    EXPECT_FALSE(de_entity_alive(f->w, e));
    EXPECT_EQ(f->w->records.count, 0u);
    EXPECT_EQ(f->w->free_indices.count, 0u);
    check_world_consistent(f->w);
    de_world_snapshot_free(NULL, &snap);

    /* Entities without snapshot components: no sections, only the entity table. */
    de_entity ids[40];
    for (u32 i = 0; i < 40; ++i) {
        ids[i] = de_entity_spawn(f->w);
        if (i % 2u)
            ASSERT_TRUE(de_entity_add(f->w, ids[i], f->enemy) != NULL);
    }
    de_entity_despawn(f->w, ids[5]);
    ASSERT_EQ(de_world_snapshot_take(f->w, NULL, &snap), DE_OK);
    EXPECT_EQ(snap.size, de_world_snapshot_size(f->w));
    EXPECT_LE(snap.size, 256u);                 /* header, generations, alive bits, free list */
    EXPECT_EQ(((const u32 *)snap.data)[9], 0u); /* section_count */
    const u64 ck = de_world_checksum(f->w, 0xFFFFFFFFu);
    de_entity_despawn(f->w, ids[0]);
    ASSERT_TRUE(de_entity_add(f->w, ids[2], f->vel) != NULL);
    (void)de_entity_spawn(f->w);
    (void)de_entity_spawn(f->w);
    ASSERT_EQ(de_world_snapshot_restore(f->w, &snap), DE_OK);
    check_world_consistent(f->w);
    EXPECT_EQ(de_world_checksum(f->w, 0xFFFFFFFFu), ck);
    EXPECT_TRUE(de_entity_alive(f->w, ids[0]));
    EXPECT_FALSE(de_entity_has(f->w, ids[2], f->vel));
    EXPECT_FALSE(de_entity_alive(f->w, ids[5]));
    de_world_snapshot_free(NULL, &snap);
}

/* Not POD: copy and destruct hooks. */
typedef struct THandle {
    u32 handle;
} THandle;
static void thandle_copy(void *dst, const void *src) {
    *(THandle *)dst = *(const THandle *)src;
}
static void thandle_destruct(void *obj) {
    (void)obj;
}

TEST_F(snap, invalid_and_unsupported, ecs_fx) {
    de_world_snapshot snap;
    EXPECT_EQ(de_world_snapshot_take(NULL, NULL, &snap), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_world_snapshot_take(f->w, NULL, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_world_snapshot_restore(f->w, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_world_snapshot_size(NULL), 0u);
    de_world_snapshot_free(NULL, NULL);

    de_entity ids[100];
    for (u32 i = 0; i < 100; ++i) {
        const de_component c[2] = {f->pos, f->vel};
        ASSERT_EQ(de_entity_spawn_with(f->w, c, 2, &ids[i]), DE_OK);
        set_pos(f, ids[i], (f32)i, 1.0f, 2.0f);
    }
    ASSERT_EQ(de_world_snapshot_take(f->w, NULL, &snap), DE_OK);
    de_entity_despawn(f->w, ids[7]);
    const u64 ck = de_world_checksum(f->w, 0xFFFFFFFFu);

    /* Corruptions are refused before anything changes. */
    u8 *bytes = (u8 *)snap.data;
    de_world_snapshot bad = snap;
    bytes[0] ^= 1u; /* magic */
    EXPECT_EQ(de_world_snapshot_restore(f->w, &bad), DE_ERR_INVALID_ARGUMENT);
    bytes[0] ^= 1u;
    bad.size = snap.size - 64u;
    EXPECT_EQ(de_world_snapshot_restore(f->w, &bad), DE_ERR_INVALID_ARGUMENT);
    bad.size = snap.size;
    /* Generation of index 3 at take set to 0: the index would be neither alive nor free. */
    u32 *gens = (u32 *)(void *)(bytes + 40);
    const u32 g3 = gens[3];
    gens[3] = 0;
    EXPECT_EQ(de_world_snapshot_restore(f->w, &bad), DE_ERR_INVALID_ARGUMENT);
    gens[3] = 5;
    EXPECT_EQ(de_world_snapshot_restore(f->w, &bad), DE_ERR_INVALID_ARGUMENT); /* ids differ */
    gens[3] = g3;
    EXPECT_EQ(de_world_checksum(f->w, 0xFFFFFFFFu), ck);
    EXPECT_FALSE(de_entity_alive(f->w, ids[7]));

    /* A world whose components differ (pos registered without DE_COMP_SNAPSHOT). */
    de_world *w2 = NULL;
    de_world_desc d;
    memset(&d, 0, sizeof d);
    d.types = f->types;
    ASSERT_EQ(de_world_create(&d, &w2), DE_OK);
    (void)de_component_register(w2, f->t_pos, 0);
    (void)de_component_register(w2, f->t_vel, DE_COMP_SNAPSHOT);
    EXPECT_EQ(de_world_snapshot_restore(w2, &snap), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(w2->records.count, 0u);
    de_world_destroy(w2);

    /* The intact snapshot still restores. */
    ASSERT_EQ(de_world_snapshot_restore(f->w, &snap), DE_OK);
    EXPECT_TRUE(de_entity_alive(f->w, ids[7]));
    de_world_snapshot_free(NULL, &snap);

    /* A snapshot component that is not POD is refused at take. */
    static const de_field_info k_fields[] = {
        DE_FIELD(THandle, handle, DE_FK_U32, DE_FIELD_DEFAULT)};
    de_type_desc td = DE_TYPE_DESC(THandle, 1, k_fields);
    td.is_pod = false;
    td.hooks.copy = thandle_copy;
    td.hooks.destruct = thandle_destruct;
    const de_type_id th = de_type_register(f->types, &td);
    const de_component handle = de_component_register(f->w, th, DE_COMP_SNAPSHOT);
    ASSERT_TRUE(DE_HANDLE_VALID(handle));
    ASSERT_EQ(de_world_snapshot_take(f->w, NULL, &snap), DE_OK); /* not in use yet */
    de_world_snapshot_free(NULL, &snap);
    ASSERT_TRUE(de_entity_add(f->w, ids[0], handle) != NULL);
    EXPECT_EQ(de_world_snapshot_take(f->w, NULL, &snap), DE_ERR_UNSUPPORTED);
    EXPECT_TRUE(snap.data == NULL);
}

/* The world of the out-of-memory test: the roundtrip world plus a non-snapshot component. */
static void build_oom_world(ecs_fx *f, de_entity *ids) {
    const de_component note = register_note(f, 0);
    build_rt_world(f, ids);
    for (u32 i = 0; i < RT_N; i += 13u) {
        if (de_entity_alive(f->w, ids[i]))
            ASSERT_TRUE(de_entity_add(f->w, ids[i], note) != NULL);
    }
}

TEST_F(snap, oom_leaves_valid_world, ecs_fx) {
    static de_entity ids[RT_N];
    build_oom_world(f, ids);
    const u64 ck = de_world_checksum(f->w, DE_COMP_SNAPSHOT);
    const u64 ck_ent = de_world_checksum(f->w, 0);

    /* Take: the block comes from the world allocator here and fails. */
    de_world_snapshot snap;
    de_tracking_allocator_fail_after(&f->track, 0);
    EXPECT_EQ(de_world_snapshot_take(f->w, &f->alloc, &snap), DE_ERR_OUT_OF_MEMORY);
    EXPECT_TRUE(snap.data == NULL);
    de_tracking_allocator_fail_after(&f->track, -1);

    /* Restore, on a fresh world each round: mutate so the restore must allocate (its scratch,
     * entity records, archetypes, edges, chunks), then fail its n-th allocation. The world stays
     * consistent, and a second restore finishes the job. */
    bool done = false;
    u32 failures = 0;
    for (i64 n = 0; n < 200 && !done; ++n) {
        ecs_fx_destroy_world(f);
        ecs_fx_create_world(f, NULL);
        build_oom_world(f, ids);
        ASSERT_EQ(de_world_checksum(f->w, DE_COMP_SNAPSHOT), ck);
        ASSERT_EQ(de_world_snapshot_take(f->w, NULL, &snap), DE_OK);
        for (u32 i = 0; i < RT_N; ++i) {
            if (de_entity_alive(f->w, ids[i]) && i % 2u == 0)
                de_entity_despawn(f->w, ids[i]);
        }
        for (u32 k = 0; k < 2000u; ++k)
            (void)de_entity_spawn(f->w);
        de_tracking_allocator_fail_after(&f->track, n);
        const de_result r = de_world_snapshot_restore(f->w, &snap);
        de_tracking_allocator_fail_after(&f->track, -1);
        check_world_consistent(f->w);
        if (r == DE_OK) {
            done = true;
        } else {
            ++failures;
            EXPECT_EQ(r, DE_ERR_OUT_OF_MEMORY);
            ASSERT_EQ(de_world_snapshot_restore(f->w, &snap), DE_OK);
            check_world_consistent(f->w);
        }
        EXPECT_EQ(de_world_checksum(f->w, DE_COMP_SNAPSHOT), ck);
        EXPECT_EQ(de_world_checksum(f->w, 0), ck_ent);
        de_world_snapshot_free(NULL, &snap);
    }
    EXPECT_TRUE(done);
    EXPECT_GT(failures, 3u);
}

/* ---- Performance ----------------------------------------------------------------------------- */

/* Wall-clock bounds hold on an optimized, uninstrumented build on an idle host. CI runners,
 * sanitizers and Debug builds get a loose bound (Decision: Timing tests run serially and loosen
 * their bounds on CI). */
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

TEST_F(snap, perf_100k, ecs_fx) {
    enum { N = 100000, RUNS = 15 };
    const de_component c3[3] = {f->pos, f->vel, f->rot};
    de_entity *ids = (de_entity *)malloc((size_t)N * sizeof(de_entity));
    ASSERT_TRUE(ids != NULL);
    for (u32 i = 0; i < N; ++i) {
        ASSERT_EQ(de_entity_spawn_with(f->w, c3, 3, &ids[i]), DE_OK);
        set_pos(f, ids[i], (f32)i, 1.0f, 2.0f);
    }
    const u64 ck = de_world_checksum(f->w, DE_COMP_SNAPSHOT);
    de_world_snapshot snap;
    u64 t0 = de_time_now_ns();
    ASSERT_EQ(de_world_snapshot_take(f->w, NULL, &snap), DE_OK);
    const u64 take_ns = de_time_now_ns() - t0;

    u64 best = ~(u64)0, total = 0;
    for (u32 run = 0; run < RUNS; ++run) {
        for (u32 i = 0; i < N; i += 7u)
            set_pos(f, ids[i], -(f32)run, 0.0f, 0.0f);
        t0 = de_time_now_ns();
        const de_result r = de_world_snapshot_restore(f->w, &snap);
        const u64 ns = de_time_now_ns() - t0;
        ASSERT_EQ(r, DE_OK);
        best = ns < best ? ns : best;
        total += ns;
    }
    EXPECT_EQ(de_world_checksum(f->w, DE_COMP_SNAPSHOT), ck);
    ecs_snapshot_stats st;
    ecs_snapshot_stats_get(f->w, &st);
    EXPECT_EQ(st.row_copies, 0u);
    EXPECT_EQ(st.fast_sections, (u64)RUNS);

    const bool loose = timing_is_loose();
    printf("[BENCH] snap.perf_100k: %u entities x 3 snapshot components, %.2f MiB snapshot; "
           "take %.3f ms; restore best %.3f ms, mean %.3f ms over %u runs (bound %s)\n",
           (unsigned)N, (double)snap.size / (1024.0 * 1024.0), (double)take_ns / 1e6,
           (double)best / 1e6, (double)total / 1e6 / RUNS, (unsigned)RUNS,
           loose ? "loose 100 ms" : "2 ms");
    EXPECT_LT(best, loose ? (u64)100000000 : (u64)2000000);
    de_world_snapshot_free(NULL, &snap);
    free(ids);
}
