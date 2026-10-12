/* test_entity.c: entity ids, generations, the free list and spawning (DENG-344). */
#include "ecs_test_util.h"

#include <stdlib.h>

#define MILLION 1000000u

TEST_F(entity, spawn_million, ecs_fx) {
    de_entity *ids = (de_entity *)malloc(sizeof(de_entity) * MILLION);
    ASSERT_TRUE(ids != NULL);
    for (u32 i = 0; i < MILLION; ++i) {
        ids[i] = de_entity_spawn(f->w);
        if (ids[i].bits == 0) {
            free(ids);
            FAIL("spawn failed");
        }
    }
    u32 bad = 0;
    for (u32 i = 0; i < MILLION; ++i) {
        /* Fresh indices are handed out in order, with generation 1. */
        bad += de_entity_index(ids[i]) != i;
        bad += de_entity_generation(ids[i]) != 1u;
        bad += !de_entity_alive(f->w, ids[i]);
        /* A handle with any other generation is stale. */
        bad += de_entity_alive(f->w, ecs_entity_make(i, 2u));
        bad += de_entity_alive(f->w, ecs_entity_make(i, 0xFFFFFFFFu));
    }
    EXPECT_EQ(bad, 0u);

    /* All in the empty archetype, in dense chunks. */
    const ecs_archetype *empty = ecs_archetype_at(f->w, ECS_EMPTY_ARCHETYPE);
    EXPECT_EQ(empty->count, MILLION);
    EXPECT_EQ(empty->chunks.count, (MILLION + empty->rows_per_chunk - 1u) / empty->rows_per_chunk);
    for (u32 c = 0; c + 1u < empty->chunks.count; ++c)
        EXPECT_EQ(ecs_table_chunk(empty, c)->count, empty->rows_per_chunk);
    /* Row order is spawn order. */
    const de_entity *first = ecs_chunk_entities(ecs_table_chunk(empty, 1));
    EXPECT_EQ(first[0].bits, ids[empty->rows_per_chunk].bits);
    /* Storage grows geometrically: a million spawns take a few hundred allocations (chunk pool
     * pages, record and chunk-list growth), not one per spawn. */
    const de_tracking_stats st = de_tracking_allocator_stats(&f->track);
    EXPECT_LT(st.alloc_count + st.realloc_count, 1000u);
    /* Beyond the last index. */
    EXPECT_FALSE(de_entity_alive(f->w, ecs_entity_make(MILLION, 1u)));
    free(ids);
}

TEST_F(entity, null_invalid, ecs_fx) {
    EXPECT_FALSE(de_entity_alive(f->w, DE_ENTITY_NULL));
    EXPECT_FALSE(de_entity_alive(NULL, DE_ENTITY_NULL));
    EXPECT_FALSE(de_entity_has(f->w, DE_ENTITY_NULL, f->pos));
    EXPECT_TRUE(de_entity_get(f->w, DE_ENTITY_NULL, f->pos) == NULL);
    EXPECT_TRUE(de_entity_get_const(f->w, DE_ENTITY_NULL, f->pos) == NULL);
    EXPECT_TRUE(ecs_entity_record(f->w, DE_ENTITY_NULL) == NULL);

    const de_entity e = de_entity_spawn(f->w);
    EXPECT_NE(e.bits, 0u);
    /* Index 0 is a real index; with generation 0 it is never alive, nor is NULL. */
    EXPECT_EQ(de_entity_index(e), 0u);
    EXPECT_EQ(de_entity_generation(e), 1u);
    EXPECT_TRUE(de_entity_alive(f->w, e));
    EXPECT_FALSE(de_entity_alive(f->w, ecs_entity_make(0, 0)));
    EXPECT_FALSE(de_entity_alive(f->w, ecs_entity_make(1, 1))); /* never spawned */
    EXPECT_FALSE(de_entity_alive(f->w, ecs_entity_make(0xFFFFFFFFu, 1)));

    /* A NULL world. */
    EXPECT_EQ(de_entity_spawn(NULL).bits, 0u);
    EXPECT_FALSE(de_entity_has(NULL, e, f->pos));
    EXPECT_TRUE(de_entity_get(NULL, e, f->pos) == NULL);

    /* Absent and unregistered components. */
    EXPECT_FALSE(de_entity_has(f->w, e, f->pos));
    EXPECT_TRUE(de_entity_get(f->w, e, f->pos) == NULL);
    EXPECT_TRUE(de_entity_get(f->w, e, (de_component){0}) == NULL);
    EXPECT_TRUE(de_entity_get(f->w, e, (de_component){99}) == NULL);
}

TEST_F(entity, stale_generation, ecs_fx) {
    const de_entity a = de_entity_spawn(f->w);
    const de_entity b = de_entity_spawn(f->w);
    const u32 set[] = {f->pos.id};
    ecs_test_set(f->w, a, set, 1);
    ecs_test_set(f->w, b, set, 1);
    ((TPos *)de_entity_get(f->w, b, f->pos))->v.y = 7.0f;

    ecs_test_despawn(f->w, a);
    EXPECT_FALSE(de_entity_alive(f->w, a));
    EXPECT_FALSE(de_entity_has(f->w, a, f->pos));
    EXPECT_TRUE(de_entity_get(f->w, a, f->pos) == NULL);
    /* The swap-remove moved b into a's row and updated its record. */
    EXPECT_TRUE(de_entity_alive(f->w, b));
    EXPECT_EQ(ecs_entity_record(f->w, b)->row, 0u);
    EXPECT_FLOAT_EQ(((const TPos *)de_entity_get_const(f->w, b, f->pos))->v.y, 7.0, 0.0);

    /* The index comes back with the next generation; the old handle stays stale. */
    const de_entity a2 = de_entity_spawn(f->w);
    EXPECT_EQ(de_entity_index(a2), de_entity_index(a));
    EXPECT_EQ(de_entity_generation(a2), de_entity_generation(a) + 1u);
    EXPECT_TRUE(de_entity_alive(f->w, a2));
    EXPECT_FALSE(de_entity_alive(f->w, a));
    EXPECT_FALSE(de_entity_has(f->w, a, f->pos));
    EXPECT_FALSE(de_entity_has(f->w, a2, f->pos)); /* a new entity starts empty */

    /* Many cycles on one index: every old handle stays dead. */
    de_entity cur = a2;
    for (u32 i = 0; i < 1000; ++i) {
        ecs_test_despawn(f->w, cur);
        const de_entity next = de_entity_spawn(f->w);
        EXPECT_EQ(de_entity_index(next), de_entity_index(a));
        EXPECT_FALSE(de_entity_alive(f->w, cur));
        cur = next;
    }
    EXPECT_EQ(de_entity_generation(cur), 1002u);
    EXPECT_TRUE(de_entity_alive(f->w, cur));
}

TEST_F(entity, free_list_reuse, ecs_fx) {
    de_entity e[10];
    for (u32 i = 0; i < 10; ++i)
        e[i] = de_entity_spawn(f->w);
    ecs_test_despawn(f->w, e[3]);
    ecs_test_despawn(f->w, e[7]);
    ecs_test_despawn(f->w, e[5]);
    EXPECT_EQ(f->w->free_indices.count, 3u);
    /* LIFO: last freed, first reused. */
    const u32 expect[] = {5, 7, 3, 10, 11};
    for (u32 i = 0; i < 5; ++i) {
        const de_entity n = de_entity_spawn(f->w);
        EXPECT_EQ(de_entity_index(n), expect[i]);
        EXPECT_EQ(de_entity_generation(n), i < 3 ? 2u : 1u);
    }
    EXPECT_EQ(f->w->free_indices.count, 0u);
    /* The free list can always take every index without allocating. */
    EXPECT_GE(f->w->free_indices.capacity, f->w->records.count);
}

TEST_F(entity, generation_wrap_retires, ecs_fx) {
    const de_entity a = de_entity_spawn(f->w);
    const de_entity b = de_entity_spawn(f->w);
    /* Jump index a to the last generation. */
    ecs_record *rec = (ecs_record *)de_array_at(&f->w->records, de_entity_index(a));
    rec->generation = 0xFFFFFFFFu;
    const de_entity last = ecs_entity_make(de_entity_index(a), 0xFFFFFFFFu);
    EXPECT_TRUE(de_entity_alive(f->w, last));
    EXPECT_FALSE(de_entity_alive(f->w, a));

    ecs_test_despawn(f->w, last);
    /* Retired: generation 0, not on the free list, never handed out again. */
    EXPECT_EQ(rec->generation, 0u);
    EXPECT_EQ(f->w->free_indices.count, 0u);
    EXPECT_FALSE(de_entity_alive(f->w, last));
    EXPECT_FALSE(de_entity_alive(f->w, ecs_entity_make(de_entity_index(a), 0)));
    const de_entity c = de_entity_spawn(f->w);
    EXPECT_EQ(de_entity_index(c), 2u);
    EXPECT_TRUE(de_entity_alive(f->w, b));
}

TEST_F(entity, spawn_oom_no_change, ecs_fx) {
    /* Every spawn runs with allocation failing. When it needs memory (records, the free list's
     * reserve, a chunk or a pool page) it must fail cleanly and change nothing; the retry then
     * succeeds. */
    const ecs_archetype *empty = ecs_archetype_at(f->w, ECS_EMPTY_ARCHETYPE);
    u32 failures = 0;
    for (u32 i = 0; i < 6u * empty->rows_per_chunk; ++i) {
        const u32 records = f->w->records.count;
        const u32 rows = empty->count;
        const u32 chunks = empty->chunks.count;
        de_tracking_allocator_fail_after(&f->track, 0);
        de_error_clear();
        de_entity e = de_entity_spawn(f->w);
        de_tracking_allocator_fail_after(&f->track, -1);
        if (e.bits == 0) {
            ++failures;
            ASSERT_TRUE(de_error_last() != NULL);
            EXPECT_EQ(de_error_last()->result, DE_ERR_OUT_OF_MEMORY);
            EXPECT_EQ(f->w->records.count, records);
            EXPECT_EQ(empty->count, rows);
            EXPECT_EQ(empty->chunks.count, chunks);
            e = de_entity_spawn(f->w);
        }
        ASSERT_TRUE(de_entity_alive(f->w, e));
        ASSERT_EQ(de_entity_index(e), records);
    }
    EXPECT_GT(failures, 0u);
}
