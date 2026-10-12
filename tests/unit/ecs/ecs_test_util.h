/*
 * ecs_test_util.h: shared test components and the world fixture of the ECS tests (S05).
 *
 * Shared by every tests/unit/ecs/ test file. Stable after DENG-344 merged: add components and
 * helpers, do not change the existing ones.
 *
 * Components (registered in this order by ecs_fx_setup, so their ids are 1..5):
 *   TPos {de_vec3}, TVel {de_vec3}, THealth {f32 current, max}, TEnemy (tag), TRot {de_quat}
 *   (16-byte aligned).
 *
 * The fixture's world allocates through a tracking allocator; teardown destroys the world and
 * checks that every block came back. The type registry uses the default heap.
 *
 * Tests may include "ecs/ecs_internal.h" (the test targets have src/ as an include root).
 */
#ifndef DE_ECS_TEST_UTIL_H
#define DE_ECS_TEST_UTIL_H

#include "de_test.h"

#include <dengine/de_ecs.h>
#include <dengine/de_math.h>

#include "ecs/ecs_internal.h"

#include <string.h>

typedef struct TPos {
    de_vec3 v;
} TPos;
typedef struct TVel {
    de_vec3 v;
} TVel;
typedef struct THealth {
    f32 current, max;
} THealth;
/* A tag: the type needs a field to be a valid reflected type; the ECS stores no data for it. */
typedef struct TEnemy {
    u8 unused;
} TEnemy;
typedef struct TRot {
    de_quat q;
} TRot;

typedef struct ecs_fx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_type_registry *types;
    de_world *w;
    de_type_id t_pos, t_vel, t_health, t_enemy, t_rot;
    de_component pos, vel, health, enemy, rot;
} ecs_fx;

/* Registers the test types in `types` (once per registry). */
static inline void ecs_test_register_types(ecs_fx *f) {
    static const de_field_info k_tpos_fields[] = {DE_FIELD(TPos, v, DE_FK_VEC3, DE_FIELD_DEFAULT)};
    static const de_field_info k_tvel_fields[] = {DE_FIELD(TVel, v, DE_FK_VEC3, DE_FIELD_DEFAULT)};
    static const de_field_info k_thealth_fields[] = {
        DE_FIELD(THealth, current, DE_FK_F32, DE_FIELD_DEFAULT),
        DE_FIELD(THealth, max, DE_FK_F32, DE_FIELD_DEFAULT),
    };
    static const de_field_info k_tenemy_fields[] = {
        DE_FIELD(TEnemy, unused, DE_FK_U8, DE_FIELD_DEFAULT)};
    static const de_field_info k_trot_fields[] = {DE_FIELD(TRot, q, DE_FK_QUAT, DE_FIELD_DEFAULT)};
    f->t_pos = de_type_register(f->types, &DE_TYPE_DESC(TPos, 1, k_tpos_fields));
    f->t_vel = de_type_register(f->types, &DE_TYPE_DESC(TVel, 1, k_tvel_fields));
    f->t_health = de_type_register(f->types, &DE_TYPE_DESC(THealth, 1, k_thealth_fields));
    f->t_enemy = de_type_register(f->types, &DE_TYPE_DESC(TEnemy, 1, k_tenemy_fields));
    f->t_rot = de_type_register(f->types, &DE_TYPE_DESC(TRot, 1, k_trot_fields));
    ASSERT_TRUE(DE_HANDLE_VALID(f->t_pos) && DE_HANDLE_VALID(f->t_vel) &&
                DE_HANDLE_VALID(f->t_health) && DE_HANDLE_VALID(f->t_enemy) &&
                DE_HANDLE_VALID(f->t_rot));
}

/* Creates the world (`desc` may be NULL: defaults) and registers the five test components. */
static inline void ecs_fx_create_world(ecs_fx *f, const de_world_desc *desc) {
    de_world_desc d;
    if (desc != NULL)
        d = *desc;
    else
        memset(&d, 0, sizeof d);
    d.allocator = &f->alloc;
    d.types = f->types;
    ASSERT_EQ(de_world_create(&d, &f->w), DE_OK);
    f->pos = de_component_register(f->w, f->t_pos, DE_COMP_SNAPSHOT);
    f->vel = de_component_register(f->w, f->t_vel, DE_COMP_SNAPSHOT);
    f->health = de_component_register(f->w, f->t_health, DE_COMP_SNAPSHOT);
    f->enemy = de_component_register(f->w, f->t_enemy, DE_COMP_TAG);
    f->rot = de_component_register(f->w, f->t_rot, DE_COMP_SNAPSHOT);
    ASSERT_EQ(f->pos.id, 1u);
    ASSERT_EQ(f->vel.id, 2u);
    ASSERT_EQ(f->health.id, 3u);
    ASSERT_EQ(f->enemy.id, 4u);
    ASSERT_EQ(f->rot.id, 5u);
}

/* Destroys the world and checks that the tracking allocator has nothing live. */
static inline void ecs_fx_destroy_world(ecs_fx *f) {
    if (f->w == NULL)
        return;
    de_world_destroy(f->w);
    f->w = NULL;
    const de_tracking_stats s = de_tracking_allocator_stats(&f->track);
    EXPECT_EQ(s.bytes_live, 0u);
    EXPECT_EQ(s.blocks_live, 0u);
}

static inline void ecs_fx_setup(ecs_fx *f) {
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_ECS);
    ASSERT_EQ(de_type_registry_create(NULL, &f->types), DE_OK);
    ecs_test_register_types(f);
    ecs_fx_create_world(f, NULL);
}

static inline void ecs_fx_teardown(ecs_fx *f) {
    ecs_fx_destroy_world(f);
    de_type_registry_destroy(f->types);
}

/* ---- Helpers (internal API) ------------------------------------------------------------------ */

/* Moves `e` to the archetype of exactly `ids[0..n)` (component ids, any order). */
static inline void ecs_test_set(de_world *w, de_entity e, const u32 *ids, u32 n) {
    ecs_archetype *a = NULL;
    ASSERT_EQ(ecs_archetype_get_or_create(w, ids, n, &a), DE_OK);
    ASSERT_EQ(ecs_entity_move(w, e, a), DE_OK);
}

/* Despawn through the internal API (de_entity_despawn is DENG-345's): destruct every column,
 * swap-remove the row, free the index. */
static inline void ecs_test_despawn(de_world *w, de_entity e) {
    ecs_record *rec = ecs_entity_record(w, e);
    ASSERT_TRUE(rec != NULL);
    ecs_archetype *a = ecs_archetype_at(w, rec->archetype);
    for (u32 c = 0; c < a->column_count; ++c)
        ecs_row_destruct(w, a, rec->chunk, rec->row, c);
    ecs_table_swap_remove(w, a, rec->chunk, rec->row);
    ecs_entity_free(w, de_entity_index(e));
}

/* The archetype `e` lives in. */
static inline ecs_archetype *ecs_test_archetype_of(de_world *w, de_entity e) {
    ecs_record *rec = ecs_entity_record(w, e);
    ASSERT_TRUE(rec != NULL);
    return ecs_archetype_at(w, rec->archetype);
}

#endif /* DE_ECS_TEST_UTIL_H */
