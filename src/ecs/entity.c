/* entity.c: entity ids, records, the free list, spawn and immediate access (DENG-344, SPEC §7.1).
 *
 * An id is (generation << 32) | index. Generations start at 1 and are bumped by each free, so a
 * handle held across a despawn no longer matches. An index whose generation would wrap past
 * 2^32 - 1 is retired: its generation becomes 0 (never a live generation) and it is not put back
 * on the free list. */
#include "ecs_internal.h"

#include "change_internal.h" /* DENG-347: a mutable get marks changed */

ecs_record *ecs_entity_record(const de_world *w, de_entity e) {
    const u32 index = de_entity_index(e);
    const u32 gen = de_entity_generation(e);
    if (gen == 0 || index >= w->records.count)
        return NULL;
    ecs_record *rec = (ecs_record *)de_array_at(&w->records, index);
    if (rec->generation != gen || rec->archetype == ECS_INVALID_INDEX)
        return NULL;
    return rec;
}

de_result ecs_entity_create(de_world *w, ecs_archetype *a, de_entity *out) {
    /* Pick the index first without committing, so a failure below leaves everything as it was. */
    const bool reuse = w->free_indices.count > 0;
    u32 index;
    if (reuse) {
        index = DE_ARRAY_LAST(&w->free_indices, u32);
    } else {
        if (w->records.count >= ECS_MAX_ENTITIES)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "ecs: every entity index is in use");
        index = w->records.count;
        if (DE_ARRAY_PUSH(&w->records, ecs_record, (ecs_record){ECS_INVALID_INDEX, 0, 0, 1u}) !=
            DE_OK)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "ecs: out of memory for entity records");
        /* The free list can hold every index, so ecs_entity_free never allocates. It follows the
         * records' (geometric) capacity, so it grows as rarely as they do. */
        if (w->free_indices.capacity < w->records.capacity &&
            de_array_reserve(&w->free_indices, w->records.capacity) != DE_OK) {
            de_array_pop(&w->records, NULL);
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "ecs: out of memory for entity records");
        }
    }

    ecs_record *rec = (ecs_record *)de_array_at(&w->records, index);
    const de_entity e = ecs_entity_make(index, rec->generation);
    u32 chunk = 0, row = 0;
    const de_result r = ecs_table_push_row(w, a, e, &chunk, &row);
    if (r != DE_OK) {
        if (!reuse)
            de_array_pop(&w->records, NULL);
        return r;
    }
    if (reuse)
        de_array_pop(&w->free_indices, NULL);

    rec->archetype = a->index;
    rec->chunk = chunk;
    rec->row = row;
    for (u32 col = 0; col < a->column_count; ++col)
        ecs_row_construct(w, a, chunk, row, col);
    *out = e;
    return DE_OK;
}

void ecs_entity_free(de_world *w, u32 index) {
    ecs_record *rec = (ecs_record *)de_array_at(&w->records, index);
    DE_ASSERT(rec->generation != 0);
    rec->archetype = ECS_INVALID_INDEX;
    rec->chunk = 0;
    rec->row = 0;
    if (rec->generation == 0xFFFFFFFFu) {
        rec->generation = 0; /* retired: never handed out again */
        return;
    }
    rec->generation += 1u;
    DE_ASSERT(w->free_indices.count < w->free_indices.capacity);
    const de_result r = de_array_push(&w->free_indices, &index);
    DE_VERIFY(r == DE_OK); /* capacity was reserved by ecs_entity_create */
}

de_entity de_entity_spawn(de_world *w) {
    if (w == NULL) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_entity_spawn: world is NULL");
        return DE_ENTITY_NULL;
    }
    ECS_ASSERT_STRUCTURAL(w);
    de_entity e = DE_ENTITY_NULL;
    if (ecs_entity_create(w, ecs_archetype_at(w, ECS_EMPTY_ARCHETYPE), &e) != DE_OK)
        return DE_ENTITY_NULL;
    return e;
}

bool de_entity_alive(const de_world *w, de_entity e) {
    return w != NULL && ecs_entity_record(w, e) != NULL;
}

/* The data of component `c` of `e`, the tag placeholder for a tag, or NULL. */
static void *entity_component(const de_world *w, de_entity e, de_component c) {
    if (w == NULL)
        return NULL;
    ECS_ASSERT_NOT_PENDING(e); /* DENG-349: pending ids are for commands only */
    const ecs_record *rec = ecs_entity_record(w, e);
    if (rec == NULL)
        return NULL;
    const ecs_archetype *a = ecs_archetype_at(w, rec->archetype);
    const u32 i = ecs_archetype_find_component(a, c.id);
    if (i == ECS_INVALID_INDEX)
        return NULL;
    const u32 col = a->column_of[i];
    if (col == ECS_INVALID_INDEX)
        return (void *)w->tag_placeholder; /* const only in the const-world caller */
    return ecs_row_column(a, rec->chunk, rec->row, col);
}

void *de_entity_get(de_world *w, de_entity e, de_component c) {
    /* DENG-347: a mutable get marks the row changed (current change tick; tags have no ticks). */
    void *data = entity_component(w, e, c);
    if (data != NULL)
        ecs_entity_mark_changed(w, e, c.id);
    return data;
}

const void *de_entity_get_const(const de_world *w, de_entity e, de_component c) {
    return entity_component(w, e, c);
}

bool de_entity_has(const de_world *w, de_entity e, de_component c) {
    if (w == NULL)
        return false;
    ECS_ASSERT_NOT_PENDING(e); /* DENG-349 */
    const ecs_record *rec = ecs_entity_record(w, e);
    return rec != NULL && ecs_archetype_has(ecs_archetype_at(w, rec->archetype), c.id);
}
