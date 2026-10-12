/* structural.c: immediate structural changes: add, remove, despawn, spawn_with (DENG-345,
 * SPEC §7.1).
 *
 * Every change runs through the internal functions declared in ecs_internal.h (ecs_entity_add,
 * ecs_entity_remove, ecs_entity_despawn), so the deferred commands of DENG-349 apply through
 * exactly the same code as the immediate API.
 *
 * The "row added" path of a column is ecs_row_construct (table.c): the construct hook, or zero,
 * then ecs_row_init_ticks. Both ecs_entity_move (add) and ecs_entity_create (spawn_with) go
 * through it, once per new column, so DENG-347's tick semantics land in one place.
 *
 * Immediate structural calls are not allowed while a schedule runs (systems use commands);
 * ECS_ASSERT_STRUCTURAL checks that in Debug and Dev. */
#include "ecs_internal.h"

/* Component ids of a spawn_with set up to this size are copied on the stack. */
#define SPAWN_WITH_STACK_IDS 64u

#if defined(DE_DEBUG) || defined(DE_DEV)
static de_log_category ecs_log_category(void) {
    return de_log_register_category("ecs", DE_LOG_INFO);
}
#endif

/* The data of `component` in `e`'s row (`rec` is `e`'s record): the column's row, the tag
 * placeholder for a tag, NULL if absent. */
static void *record_data(de_world *w, const ecs_record *rec, u32 component) {
    const ecs_archetype *a = ecs_archetype_at(w, rec->archetype);
    const u32 i = ecs_archetype_find_component(a, component);
    if (i == ECS_INVALID_INDEX)
        return NULL;
    const u32 col = a->column_of[i];
    if (col == ECS_INVALID_INDEX)
        return (void *)w->tag_placeholder;
    return ecs_row_column(a, rec->chunk, rec->row, col);
}

de_result ecs_entity_add(de_world *w, de_entity e, u32 component, void **out_data) {
    ecs_record *rec = ecs_entity_record(w, e);
    if (rec == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "ecs: add to entity 0x%016llx, which is not alive",
                       (unsigned long long)e.bits);
    ecs_archetype *src = ecs_archetype_at(w, rec->archetype);
    ecs_archetype *dst = NULL;
    DE_TRY(ecs_archetype_add_target(w, src, component, &dst));
    if (dst != src)
        DE_TRY(ecs_entity_move(w, e, dst)); /* constructs the new column (ecs_row_construct) */
    if (out_data != NULL)
        *out_data = record_data(w, rec, component);
    return DE_OK;
}

de_result ecs_entity_remove(de_world *w, de_entity e, u32 component) {
    ecs_record *rec = ecs_entity_record(w, e);
    if (rec == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "ecs: remove from entity 0x%016llx, which is not alive",
                       (unsigned long long)e.bits);
    ecs_archetype *src = ecs_archetype_at(w, rec->archetype);
    ecs_archetype *dst = NULL;
    DE_TRY(ecs_archetype_remove_target(w, src, component, &dst));
    if (dst == src)
        return DE_OK;                  /* absent */
    return ecs_entity_move(w, e, dst); /* destructs the dropped column */
}

de_result ecs_entity_despawn(de_world *w, de_entity e) {
    ecs_record *rec = ecs_entity_record(w, e);
    if (rec == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "ecs: despawn of entity 0x%016llx, which is not alive",
                       (unsigned long long)e.bits);
    ecs_archetype *a = ecs_archetype_at(w, rec->archetype);
    const u32 chunk = rec->chunk, row = rec->row;
    for (u32 c = 0; c < a->column_count; ++c)
        ecs_row_destruct(w, a, chunk, row, c);
    /* Moves the table's last row into the hole and updates that entity's record. */
    ecs_table_swap_remove(w, a, chunk, row);
    ecs_entity_free(w, de_entity_index(e));
    return DE_OK;
}

/* ---- Public API ------------------------------------------------------------------------------ */

void *de_entity_add(de_world *w, de_entity e, de_component c) {
    if (w == NULL) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_entity_add: world is NULL");
        return NULL;
    }
    ECS_ASSERT_STRUCTURAL(w);
    void *data = NULL;
    if (ecs_entity_add(w, e, c.id, &data) != DE_OK)
        return NULL;
    return data;
}

void de_entity_remove(de_world *w, de_entity e, de_component c) {
    if (w == NULL)
        return;
    ECS_ASSERT_STRUCTURAL(w);
    if (ecs_entity_record(w, e) == NULL)
        return; /* a dead entity has nothing to remove */
    (void)ecs_entity_remove(w, e, c.id);
}

void de_entity_despawn(de_world *w, de_entity e) {
    if (w == NULL)
        return;
    ECS_ASSERT_STRUCTURAL(w);
    if (ecs_entity_record(w, e) == NULL) {
#if defined(DE_DEBUG) || defined(DE_DEV)
        DE_WARN(ecs_log_category(), "de_entity_despawn: entity 0x%016llx is not alive (ignored)",
                (unsigned long long)e.bits);
#endif
        return;
    }
    (void)ecs_entity_despawn(w, e);
}

de_result de_entity_spawn_with(de_world *w, const de_component *comps, u32 n, de_entity *out) {
    if (out != NULL)
        *out = DE_ENTITY_NULL;
    if (w == NULL || out == NULL || (n > 0 && comps == NULL))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_entity_spawn_with: world, out and comps "
                                                "(when n > 0) are required");
    ECS_ASSERT_STRUCTURAL(w);

    /* ecs_archetype_get_or_create takes u32 ids, outside w->set_scratch. Zeroed: GCC cannot see
     * that only the first n entries are read and warns (-Wmaybe-uninitialized). */
    u32 stack_ids[SPAWN_WITH_STACK_IDS] = {0};
    u32 *ids = stack_ids;
    const size_t heap_bytes = (size_t)n * sizeof(u32);
    if (n > SPAWN_WITH_STACK_IDS) {
        ids = (u32 *)DE_ALLOC(w->a, heap_bytes, DE_ALIGNOF(u32), DE_MEM_ECS);
        if (ids == NULL)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_entity_spawn_with: out of memory");
    }
    for (u32 i = 0; i < n; ++i)
        ids[i] = comps[i].id;
    ecs_archetype *a = NULL;
    de_result r = ecs_archetype_get_or_create(w, ids, n, &a);
    if (ids != stack_ids)
        DE_FREE(w->a, ids, heap_bytes);
    if (r != DE_OK)
        return r;
    /* Constructs every column in place (ecs_row_construct): no intermediate archetypes. */
    return ecs_entity_create(w, a, out);
}
