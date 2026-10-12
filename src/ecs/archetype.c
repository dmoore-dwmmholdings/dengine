/* archetype.c: archetypes (unique component sets), their chunk layout and creation listeners
 * (DENG-344, SPEC §7.1).
 *
 * Archetypes live in an array in creation order; that array is the only order anything iterates
 * them in (SPEC §5.7 rule 5). The set -> archetype hash map is for lookups only. */
#include "ecs_internal.h"

static u64 align_up_u64(u64 v, u64 align) {
    return (v + align - 1u) & ~(align - 1u);
}

/* Bytes a chunk of `rows` rows takes with archetype `a`'s columns. With `write`, also stores each
 * column's offsets. Layout: entities, then each column's data at its alignment, then each
 * column's added and changed tick arrays. */
static u64 chunk_layout(const de_world *w, ecs_archetype *a, u32 rows, bool write) {
    u64 off = (u64)rows * sizeof(de_entity);
    for (u32 c = 0; c < a->column_count; ++c) {
        ecs_column *col = &a->columns[c];
        const ecs_component_info *info = ecs_component_get(w, col->component);
        off = align_up_u64(off, info->align);
        if (write)
            col->data_offset = (u32)off;
        off += (u64)rows * col->size;
    }
    off = align_up_u64(off, sizeof(u32));
    for (u32 c = 0; c < a->column_count; ++c) {
        ecs_column *col = &a->columns[c];
        if (write)
            col->added_offset = (u32)off;
        off += (u64)rows * sizeof(u32);
        if (write)
            col->changed_offset = (u32)off;
        off += (u64)rows * sizeof(u32);
    }
    return off;
}

u32 ecs_archetype_find_component(const ecs_archetype *a, u32 component) {
    u32 lo = 0, hi = a->component_count;
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2u;
        const u32 v = a->components[mid];
        if (v == component)
            return mid;
        if (v < component)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return ECS_INVALID_INDEX;
}

static u64 set_hash(const u32 *sorted_ids, u32 count) {
    return de_hash64(sorted_ids, (size_t)count * sizeof(u32), 0);
}

ecs_archetype *ecs_archetype_find(const de_world *w, const u32 *sorted_ids, u32 count) {
    ecs_set_key key;
    memset(&key, 0, sizeof key);
    key.ids = sorted_ids;
    key.count = count;
    const u32 *index = DE_HASHMAP_GET(&w->archetype_by_set, u32, set_hash(sorted_ids, count), &key);
    return index ? ecs_archetype_at(w, *index) : NULL;
}

/* Creates the archetype for the sorted, unique, registered set `ids` (not yet in the world). */
static de_result archetype_create(de_world *w, const u32 *ids, u32 count, u64 hash,
                                  ecs_archetype **out) {
    if (w->archetypes.count >= ECS_INVALID_INDEX - 1u)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "ecs: too many archetypes");
    u32 column_count = 0;
    u64 row_bytes = sizeof(de_entity);
    for (u32 i = 0; i < count; ++i) {
        const ecs_component_info *info = ecs_component_get(w, ids[i]);
        if (info->size > 0) {
            ++column_count;
            row_bytes += (u64)info->size + 2u * sizeof(u32);
        }
    }

    const size_t alloc_size = sizeof(ecs_archetype) + (size_t)column_count * sizeof(ecs_column) +
                              2u * (size_t)count * sizeof(u32);
    ecs_archetype *a =
        (ecs_archetype *)DE_ALLOC(w->a, alloc_size, DE_ALIGNOF(ecs_archetype), DE_MEM_ECS);
    if (a == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "ecs: out of memory for an archetype");
    memset(a, 0, alloc_size);
    a->alloc_size = alloc_size;
    a->index = w->archetypes.count;
    a->component_count = count;
    a->column_count = column_count;
    a->columns = (ecs_column *)(void *)(a + 1);
    a->components = (u32 *)(void *)(a->columns + column_count);
    a->column_of = a->components + count;
    a->set_hash = hash;
    DE_ARRAY_INIT_TAG(&a->chunks, ecs_chunk, w->a, DE_MEM_ECS);

    u32 col = 0;
    for (u32 i = 0; i < count; ++i) {
        const ecs_component_info *info = ecs_component_get(w, ids[i]);
        a->components[i] = ids[i];
        if (info->size > 0) {
            a->columns[col].component = ids[i];
            a->columns[col].size = info->size;
            a->column_of[i] = col++;
        } else {
            a->column_of[i] = ECS_INVALID_INDEX;
        }
    }

    /* Rows per chunk: the most rows whose exact layout (with alignment padding) fits. The first
     * guess ignores padding, so it is never too small; step down until it fits. */
    u64 rows = w->chunk_bytes / row_bytes;
    while (rows > 0 && chunk_layout(w, a, (u32)rows, false) > w->chunk_bytes)
        --rows;
    if (rows == 0) {
        DE_FREE(w->a, a, alloc_size);
        return DE_FAIL(DE_ERR_UNSUPPORTED,
                       "ecs: one row of a %u-component archetype (%llu bytes) does not fit a "
                       "%u-byte chunk",
                       count, (unsigned long long)row_bytes, w->chunk_bytes);
    }
    a->rows_per_chunk = (u32)rows;
    a->chunk_used = (u32)chunk_layout(w, a, (u32)rows, true);

    /* Append (geometric growth), then index; undo the append if the map cannot grow. */
    if (de_array_push(&w->archetypes, &a) != DE_OK) {
        DE_FREE(w->a, a, alloc_size);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "ecs: out of memory for an archetype");
    }
    ecs_set_key key;
    memset(&key, 0, sizeof key);
    key.ids = a->components;
    key.count = count;
    if (de_hashmap_put(&w->archetype_by_set, hash, &key, &a->index) != DE_OK) {
        de_array_pop(&w->archetypes, NULL);
        DE_FREE(w->a, a, alloc_size);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "ecs: out of memory for an archetype");
    }

    /* Listeners run in subscription order; a listener added during the loop is not called for
     * this archetype (it matches existing archetypes itself). */
    const u32 listener_count = w->archetype_listeners.count;
    for (u32 i = 0; i < listener_count && i < w->archetype_listeners.count; ++i) {
        const ecs_archetype_listener l =
            DE_ARRAY_AT(&w->archetype_listeners, ecs_archetype_listener, i);
        l.fn(w, a, l.user);
    }
    *out = a;
    return DE_OK;
}

de_result ecs_archetype_get_or_create(de_world *w, const u32 *ids, u32 count, ecs_archetype **out) {
    if (count > 0 && ids == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "ecs: component set is NULL");
    DE_ASSERT(count == 0 || ids != (const u32 *)w->set_scratch.data);

    /* Canonical form: sorted ascending, no duplicates. Insertion sort: sets are small. */
    if (de_array_resize(&w->set_scratch, count) != DE_OK)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "ecs: out of memory");
    u32 *set = (u32 *)w->set_scratch.data;
    u32 n = 0;
    for (u32 i = 0; i < count; ++i) {
        const u32 id = ids[i];
        if (ecs_component_get(w, id) == NULL)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "ecs: component id %u is not registered", id);
        u32 j = n;
        while (j > 0 && set[j - 1u] > id)
            --j;
        if (j > 0 && set[j - 1u] == id)
            continue; /* duplicate */
        memmove(set + j + 1u, set + j, (size_t)(n - j) * sizeof(u32));
        set[j] = id;
        ++n;
    }

    const u64 hash = set_hash(set, n);
    ecs_set_key key;
    memset(&key, 0, sizeof key);
    key.ids = set;
    key.count = n;
    const u32 *index = DE_HASHMAP_GET(&w->archetype_by_set, u32, hash, &key);
    if (index != NULL) {
        *out = ecs_archetype_at(w, *index);
        return DE_OK;
    }
    return archetype_create(w, set, n, hash, out);
}

de_result ecs_on_archetype_created(de_world *w, ecs_archetype_created_fn fn, void *user) {
    if (fn == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "ecs: archetype listener is NULL");
    const ecs_archetype_listener l = {fn, user};
    if (de_array_push(&w->archetype_listeners, &l) != DE_OK)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "ecs: out of memory for an archetype listener");
    return DE_OK;
}

void ecs_off_archetype_created(de_world *w, ecs_archetype_created_fn fn, void *user) {
    for (u32 i = 0; i < w->archetype_listeners.count; ++i) {
        const ecs_archetype_listener *l =
            &DE_ARRAY_AT(&w->archetype_listeners, ecs_archetype_listener, i);
        if (l->fn == fn && l->user == user) {
            de_array_remove_ordered(&w->archetype_listeners, i);
            return;
        }
    }
}
