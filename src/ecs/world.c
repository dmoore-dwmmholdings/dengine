/* world.c: world create/destroy and the world tick (DENG-344, SPEC §7.1). */
#include "ecs_internal.h"

/* Compares archetype set keys by contents, so two sets with the same 64-bit hash never alias. */
static bool set_key_eq(const void *a, const void *b, u32 key_size, void *user) {
    (void)key_size;
    (void)user;
    const ecs_set_key *x = (const ecs_set_key *)a;
    const ecs_set_key *y = (const ecs_set_key *)b;
    return x->count == y->count &&
           (x->count == 0 || memcmp(x->ids, y->ids, (size_t)x->count * sizeof(u32)) == 0);
}

static u32 round_up_u32(u32 v, u32 align) {
    return (v + align - 1u) & ~(align - 1u);
}

de_result de_world_create(const de_world_desc *desc, de_world **out) {
    if (out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_world_create: out is NULL");
    *out = NULL;
    if (desc == NULL || desc->types == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_world_create: desc and desc->types are "
                                                "required");
    if (desc->chunk_bytes > ECS_MAX_CHUNK_BYTES)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_world_create: chunk_bytes %u is above the limit of %u bytes",
                       desc->chunk_bytes, ECS_MAX_CHUNK_BYTES);

    const de_allocator *a = desc->allocator ? desc->allocator : de_heap_allocator();
    u32 chunk_bytes = desc->chunk_bytes ? desc->chunk_bytes : DE_ECS_DEFAULT_CHUNK_BYTES;
    if (chunk_bytes < DE_ECS_MIN_CHUNK_BYTES)
        chunk_bytes = DE_ECS_MIN_CHUNK_BYTES;
    chunk_bytes = round_up_u32(chunk_bytes, DE_ECS_MAX_COMPONENT_ALIGN);

    de_world *w = DE_NEW(a, de_world, DE_MEM_ECS);
    if (w == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_world_create: out of memory");
    memset(w, 0, sizeof *w);
    w->a = a;
    w->types = desc->types;
    w->tasks = desc->tasks;
    w->chunk_bytes = chunk_bytes;

    DE_ARRAY_INIT_TAG(&w->records, ecs_record, a, DE_MEM_ECS);
    DE_ARRAY_INIT_TAG(&w->free_indices, u32, a, DE_MEM_ECS);
    DE_ARRAY_INIT_TAG(&w->components, ecs_component_info, a, DE_MEM_ECS);
    DE_ARRAY_INIT_TAG(&w->archetypes, ecs_archetype *, a, DE_MEM_ECS);
    DE_ARRAY_INIT_TAG(&w->set_scratch, u32, a, DE_MEM_ECS);
    DE_ARRAY_INIT_TAG(&w->archetype_listeners, ecs_archetype_listener, a, DE_MEM_ECS);

    de_result r = DE_OK;
    const de_hashmap_desc by_name = {
        .allocator = a, .key_size = sizeof(u64), .value_size = sizeof(u32), .tag = DE_MEM_ECS};
    const de_hashmap_desc by_type = {
        .allocator = a, .key_size = sizeof(u32), .value_size = sizeof(u32), .tag = DE_MEM_ECS};
    const de_hashmap_desc by_set = {.allocator = a,
                                    .key_size = sizeof(ecs_set_key),
                                    .value_size = sizeof(u32),
                                    .key_eq = set_key_eq,
                                    .tag = DE_MEM_ECS};
    /* The maps allocate nothing at init, so de_world_destroy can free them in any state. */
    if ((r = de_hashmap_init(&w->component_by_name, &by_name)) != DE_OK ||
        (r = de_hashmap_init(&w->component_by_type, &by_type)) != DE_OK ||
        (r = de_hashmap_init(&w->archetype_by_set, &by_set)) != DE_OK ||
        (r = ecs_resources_init(w)) != DE_OK || /* DENG-348 */
        (r = ecs_events_init(w)) != DE_OK ||    /* DENG-348 */
        (r = ecs_commands_init(w)) != DE_OK) {  /* DENG-349 */
        de_world_destroy(w);
        return r;
    }

    const de_pool_desc pool = {.block_size = chunk_bytes,
                               .block_align = DE_ECS_MAX_COMPONENT_ALIGN,
                               .backing = a,
                               .tag = DE_MEM_ECS};
    if ((r = de_pool_create(&pool, &w->chunk_pool)) != DE_OK) {
        de_world_destroy(w);
        return DE_FAIL_WRAP(r, "de_world_create: chunk pool");
    }

    if (desc->initial_entity_capacity > 0) {
        if ((r = de_array_reserve(&w->records, desc->initial_entity_capacity)) != DE_OK ||
            (r = de_array_reserve(&w->free_indices, desc->initial_entity_capacity)) != DE_OK) {
            de_world_destroy(w);
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_world_create: entity records");
        }
    }

    ecs_archetype *empty = NULL;
    if ((r = ecs_archetype_get_or_create(w, NULL, 0, &empty)) != DE_OK) {
        de_world_destroy(w);
        return r;
    }
    DE_ASSERT(empty->index == ECS_EMPTY_ARCHETYPE);

    *out = w;
    return DE_OK;
}

/* Runs every destruct hook, in archetype, chunk, row and column order. */
static void destruct_all(de_world *w) {
    for (u32 ai = 0; ai < ecs_archetype_count(w); ++ai) {
        ecs_archetype *a = ecs_archetype_at(w, ai);
        for (u32 col = 0; col < a->column_count; ++col) {
            const ecs_component_info *info = ecs_component_get(w, a->columns[col].component);
            if (info->hooks.destruct == NULL)
                continue;
            for (u32 c = 0; c < a->chunks.count; ++c) {
                const ecs_chunk *ch = ecs_table_chunk(a, c);
                u8 *data = (u8 *)ecs_chunk_column(a, ch, col);
                for (u32 row = 0; row < ch->count; ++row)
                    info->hooks.destruct(data + (size_t)row * a->columns[col].size);
            }
        }
    }
}

void de_world_destroy(de_world *w) {
    if (w == NULL)
        return;
    ecs_commands_destroy(w); /* DENG-349: unapplied payloads, before anything they refer to */
    destruct_all(w);
    ecs_queries_destroy(w);   /* DENG-346 */
    ecs_resources_destroy(w); /* DENG-348: after the entities, which may refer to resources */
    ecs_events_destroy(w);    /* DENG-348 */
    for (u32 ai = 0; ai < w->archetypes.count; ++ai) {
        ecs_archetype *a = ecs_archetype_at(w, ai);
        de_array_free(&a->chunks); /* the chunk blocks go back with the pool */
        /* DENG-345: the archetype's edge cache. */
        ecs_archetype_edges_free(w, a);
        DE_FREE(w->a, a, a->alloc_size);
    }
    de_pool_destroy(w->chunk_pool);
    de_array_free(&w->records);
    de_array_free(&w->free_indices);
    de_array_free(&w->components);
    de_array_free(&w->archetypes);
    de_array_free(&w->set_scratch);
    de_array_free(&w->archetype_listeners);
    de_hashmap_free(&w->component_by_name);
    de_hashmap_free(&w->component_by_type);
    de_hashmap_free(&w->archetype_by_set);
    DE_FREE(w->a, w, sizeof *w);
}

u64 de_world_tick(const de_world *w) {
    return w ? w->tick : 0;
}

/* DENG-308: the registry, for modules that register their resource types (de_audio_attach). */
de_type_registry *de_world_types(const de_world *w) {
    return w ? w->types : NULL;
}
