/* query.c: the per-world query registry, query create and destroy, and the change window
 * (DENG-346, SPEC §7.2). */
#include "query_internal.h"

/* ---- Registry ----------------------------------------------------------------------------- */

static de_query handle_make(u32 slot, u32 generation) {
    de_query h;
    h.id = (generation << ECS_QUERY_INDEX_BITS) | (slot + 1u);
    return h;
}

ecs_query *ecs_query_get(const de_world *w, de_query q) {
    if (w == NULL || w->queries == NULL || q.id == 0)
        return NULL;
    const u32 index = q.id & ECS_QUERY_INDEX_MASK;
    if (index == 0 || index > w->queries->slots.count)
        return NULL;
    const ecs_query_slot *s = &DE_ARRAY_AT(&w->queries->slots, ecs_query_slot, index - 1u);
    if (s->q == NULL || s->generation != (q.id >> ECS_QUERY_INDEX_BITS))
        return NULL;
    return s->q;
}

/* Creates the registry and subscribes its archetype listener. */
static de_result registry_get(de_world *w, ecs_query_registry **out) {
    if (w->queries != NULL) {
        *out = w->queries;
        return DE_OK;
    }
    ecs_query_registry *r = DE_NEW(w->a, ecs_query_registry, DE_MEM_ECS);
    if (r == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_query_create: out of memory");
    memset(r, 0, sizeof *r);
    DE_ARRAY_INIT_TAG(&r->slots, ecs_query_slot, w->a, DE_MEM_ECS);
    DE_ARRAY_INIT_TAG(&r->free_slots, u32, w->a, DE_MEM_ECS);
    if (ecs_on_archetype_created(w, ecs_query_on_archetype_created, r) != DE_OK) {
        DE_FREE(w->a, r, sizeof *r);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_query_create: out of memory");
    }
    w->queries = r;
    *out = r;
    return DE_OK;
}

static void query_free(de_world *w, ecs_query *q) {
    de_array_free(&q->entries);
    de_array_free(&q->range_commands);
    DE_FREE(w->a, q, q->alloc_size);
}

void ecs_queries_destroy(de_world *w) {
    ecs_query_registry *r = w->queries;
    if (r == NULL)
        return;
    for (u32 i = 0; i < r->slots.count; ++i) {
        ecs_query_slot *s = &DE_ARRAY_AT(&r->slots, ecs_query_slot, i);
        if (s->q != NULL)
            query_free(w, s->q);
        s->q = NULL;
    }
    ecs_off_archetype_created(w, ecs_query_on_archetype_created, r);
    de_array_free(&r->slots);
    de_array_free(&r->free_slots);
    DE_FREE(w->a, r, sizeof *r);
    w->queries = NULL;
}

/* ---- Validation ----------------------------------------------------------------------------- */

static bool access_valid(de_access a) {
    return a == DE_READ || a == DE_WRITE || a == DE_OPTIONAL_READ || a == DE_OPTIONAL_WRITE;
}

static bool access_optional(de_access a) {
    return a == DE_OPTIONAL_READ || a == DE_OPTIONAL_WRITE;
}

static bool filter_valid(de_filter f) {
    return f == DE_WITH || f == DE_WITHOUT || f == DE_ADDED || f == DE_CHANGED;
}

static de_result validate(const de_world *w, const de_query_desc *d, u32 *out_change_count) {
    if ((d->term_count > 0 && d->terms == NULL) || (d->filter_count > 0 && d->filters == NULL))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_query_create: NULL terms or filters with a "
                                                "non-zero count");
    if (d->term_count > DE_QUERY_MAX_TERMS || d->filter_count > DE_QUERY_MAX_FILTERS)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_query_create: %u terms and %u filters (limits %u and %u)", d->term_count,
                       d->filter_count, DE_QUERY_MAX_TERMS, DE_QUERY_MAX_FILTERS);
    for (u32 i = 0; i < d->term_count; ++i) {
        const de_query_term *t = &d->terms[i];
        if (ecs_component_get(w, t->component.id) == NULL)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "de_query_create: term %u: component %u is not registered", i,
                           t->component.id);
        if (!access_valid(t->access))
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_query_create: term %u: unknown access %d",
                           i, (int)t->access);
        for (u32 j = 0; j < i; ++j)
            if (d->terms[j].component.id == t->component.id)
                return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                               "de_query_create: component %u is in terms %u and %u",
                               t->component.id, j, i);
    }
    u32 change_count = 0;
    for (u32 i = 0; i < d->filter_count; ++i) {
        const de_query_filter *f = &d->filters[i];
        const ecs_component_info *info = ecs_component_get(w, f->component.id);
        if (info == NULL)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "de_query_create: filter %u: component %u is not registered", i,
                           f->component.id);
        if (!filter_valid(f->filter))
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_query_create: filter %u: unknown filter %d",
                           i, (int)f->filter);
        if (f->filter == DE_ADDED || f->filter == DE_CHANGED) {
            if (info->size == 0)
                return DE_FAIL(DE_ERR_UNSUPPORTED,
                               "de_query_create: filter %u: ADDED/CHANGED on tag '%s' (tags have "
                               "no change ticks)",
                               i, info->name);
            ++change_count;
        }
        if (f->filter != DE_WITHOUT)
            continue;
        for (u32 j = 0; j < d->term_count; ++j)
            if (d->terms[j].component.id == f->component.id)
                return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                               "de_query_create: component %u is both WITHOUT (filter %u) and term "
                               "%u",
                               f->component.id, i, j);
        for (u32 j = 0; j < d->filter_count; ++j)
            if (j != i && d->filters[j].component.id == f->component.id &&
                d->filters[j].filter != DE_WITHOUT)
                return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                               "de_query_create: component %u is WITHOUT (filter %u) and also "
                               "filter %u",
                               f->component.id, i, j);
    }
    *out_change_count = change_count;
    return DE_OK;
}

/* Inserts `id` into the sorted unique array `set[0..*n)`. */
static void set_insert(u32 *set, u32 *n, u32 id) {
    u32 j = *n;
    while (j > 0 && set[j - 1u] > id)
        --j;
    if (j > 0 && set[j - 1u] == id)
        return;
    memmove(set + j + 1u, set + j, (size_t)(*n - j) * sizeof(u32));
    set[j] = id;
    ++*n;
}

/* ---- Create and destroy --------------------------------------------------------------------- */

/* Builds the query block from a validated desc. */
static ecs_query *query_build(de_world *w, const de_query_desc *d, u32 change_count) {
    const u32 nt = d->term_count, nf = d->filter_count;
    /* Layout: struct, terms, filters, then u32 arrays: required (nt + nf), excluded (nf),
     * change_filters (change_count). */
    size_t size = sizeof(ecs_query);
    const size_t terms_off = size;
    size += (size_t)nt * sizeof(de_query_term);
    const size_t filters_off = size;
    size += (size_t)nf * sizeof(de_query_filter);
    const size_t u32_off = size;
    size += ((size_t)nt + 2u * nf + change_count) * sizeof(u32);

    u8 *mem = (u8 *)DE_ALLOC(w->a, size, DE_ALIGNOF(ecs_query), DE_MEM_ECS);
    if (mem == NULL)
        return NULL;
    memset(mem, 0, size);
    ecs_query *q = (ecs_query *)(void *)mem;
    q->alloc_size = size;
    q->term_count = nt;
    q->filter_count = nf;
    q->terms = (de_query_term *)(void *)(mem + terms_off);
    q->filters = (de_query_filter *)(void *)(mem + filters_off);
    u32 *u = (u32 *)(void *)(mem + u32_off);
    q->required = u;
    q->excluded = u + nt + nf;
    q->change_filters = q->excluded + nf;
    if (nt > 0)
        memcpy(q->terms, d->terms, (size_t)nt * sizeof(de_query_term));
    if (nf > 0)
        memcpy(q->filters, d->filters, (size_t)nf * sizeof(de_query_filter));

    for (u32 i = 0; i < nt; ++i)
        if (!access_optional(q->terms[i].access))
            set_insert(q->required, &q->required_count, q->terms[i].component.id);
    for (u32 i = 0; i < nf; ++i) {
        const de_query_filter *f = &q->filters[i];
        if (f->filter == DE_WITHOUT) {
            set_insert(q->excluded, &q->excluded_count, f->component.id);
        } else {
            set_insert(q->required, &q->required_count, f->component.id);
            if (f->filter == DE_ADDED || f->filter == DE_CHANGED)
                q->change_filters[q->change_count++] = i;
        }
    }
    DE_ASSERT(q->change_count == change_count);
    q->col_count = nt + change_count;
    q->entry_stride = 2u + q->col_count;
    q->change_filter = ecs_change_filter_default;
    de_array_init(&q->entries, w->a, q->entry_stride * (u32)sizeof(u32), (u32)sizeof(u32),
                  DE_MEM_ECS);
    DE_ARRAY_INIT_TAG(&q->range_commands, de_commands *, w->a, DE_MEM_ECS);
    return q;
}

de_query de_query_create(de_world *w, const de_query_desc *desc) {
    const de_query invalid = {0};
    if (w == NULL || desc == NULL) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_query_create: world and desc are required");
        return invalid;
    }
    ECS_ASSERT_STRUCTURAL(w);
    u32 change_count = 0;
    if (validate(w, desc, &change_count) != DE_OK)
        return invalid;

    ecs_query_registry *r = NULL;
    if (registry_get(w, &r) != DE_OK)
        return invalid;

    /* Take a slot: the most recently freed, else a new one. The free list's capacity follows the
     * slot count, so destroy never allocates. */
    u32 slot;
    bool new_slot = false;
    if (r->free_slots.count > 0) {
        slot = DE_ARRAY_LAST(&r->free_slots, u32);
    } else {
        if (r->slots.count >= ECS_QUERY_MAX_SLOTS) {
            DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_query_create: too many queries");
            return invalid;
        }
        /* Geometric growth; the free list follows the slots' capacity. */
        u32 cap = r->slots.capacity;
        if (cap <= r->slots.count)
            cap = cap < 8u ? 8u : (cap > ECS_QUERY_MAX_SLOTS / 2u ? ECS_QUERY_MAX_SLOTS : cap * 2u);
        if (de_array_reserve(&r->slots, cap) != DE_OK ||
            de_array_reserve(&r->free_slots, r->slots.capacity) != DE_OK) {
            DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_query_create: out of memory");
            return invalid;
        }
        slot = r->slots.count;
        new_slot = true;
    }

    ecs_query *q = query_build(w, desc, change_count);
    if (q == NULL) {
        DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_query_create: out of memory");
        return invalid;
    }
    if (ecs_query_scan(w, q) != DE_OK) {
        query_free(w, q);
        return invalid; /* error context set by the scan */
    }

    if (new_slot) {
        const ecs_query_slot s = {q, 1u, 0u};
        de_result pr = de_array_push(&r->slots, &s); /* reserved above: cannot fail */
        DE_ASSERT(pr == DE_OK);
        (void)pr;
    } else {
        de_array_pop(&r->free_slots, NULL);
        DE_ARRAY_AT(&r->slots, ecs_query_slot, slot).q = q;
    }
    return handle_make(slot, DE_ARRAY_AT(&r->slots, ecs_query_slot, slot).generation);
}

void de_query_destroy(de_world *w, de_query q) {
    ecs_query *query = ecs_query_get(w, q);
    if (query == NULL)
        return;
    ECS_ASSERT_STRUCTURAL(w);
    ecs_query_registry *r = w->queries;
    const u32 slot = (q.id & ECS_QUERY_INDEX_MASK) - 1u;
    ecs_query_slot *s = &DE_ARRAY_AT(&r->slots, ecs_query_slot, slot);
    query_free(w, query);
    s->q = NULL;
    if (s->generation == ECS_QUERY_GEN_MAX)
        return; /* retired: the generation would wrap */
    ++s->generation;
    de_result pr = de_array_push(&r->free_slots, &slot); /* capacity >= slots: cannot fail */
    DE_ASSERT(pr == DE_OK);
    (void)pr;
}

void ecs_query_set_change_window(de_world *w, de_query q, u32 last_run_tick, u32 this_run_tick) {
    ecs_query *query = ecs_query_get(w, q);
    if (query == NULL)
        return;
    query->last_run_tick = last_run_tick;
    query->this_run_tick = this_run_tick;
    query->change_window_set = true; /* DENG-347 */
}
