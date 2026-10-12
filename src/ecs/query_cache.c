/* query_cache.c: archetype matching and the matched-archetype cache of queries (DENG-346,
 * SPEC §7.2, §5.7 rule 5).
 *
 * A query tests each world archetype once, in creation order: all existing ones when it is
 * created, then each new one from the registry's ecs_on_archetype_created listener. Matches are
 * appended to the query's entries, so the entries are in creation order and updating them costs
 * O(new archetypes) per query. */
#include "query_internal.h"

bool ecs_query_matches(const ecs_query *q, const ecs_archetype *a) {
    if (a->component_count < q->required_count)
        return false;
    /* Both sets are sorted: one merge walk. */
    u32 j = 0;
    for (u32 i = 0; i < q->required_count; ++i) {
        const u32 id = q->required[i];
        while (j < a->component_count && a->components[j] < id)
            ++j;
        if (j == a->component_count || a->components[j] != id)
            return false;
        ++j;
    }
    for (u32 i = 0; i < q->excluded_count; ++i)
        if (ecs_archetype_has(a, q->excluded[i]))
            return false;
    return true;
}

static u32 column_entry(const ecs_archetype *a, u32 component) {
    const u32 i = ecs_archetype_find_component(a, component);
    if (i == ECS_INVALID_INDEX)
        return ECS_QUERY_COL_ABSENT;
    return a->column_of[i] == ECS_INVALID_INDEX ? ECS_QUERY_COL_TAG : a->column_of[i];
}

void ecs_query_columns(const ecs_query *q, const ecs_archetype *a, u32 *cols) {
    for (u32 i = 0; i < q->term_count; ++i)
        cols[i] = column_entry(a, q->terms[i].component.id);
    for (u32 j = 0; j < q->change_count; ++j) {
        cols[q->term_count + j] = column_entry(a, q->filters[q->change_filters[j]].component.id);
        DE_ASSERT(cols[q->term_count + j] < ECS_QUERY_COL_TAG); /* required, never a tag */
    }
}

de_result ecs_query_scan(de_world *w, ecs_query *q) {
    const u32 n = ecs_archetype_count(w);
    u32 entry[2u + ECS_QUERY_MAX_COLS];
    while (q->scanned < n) {
        const ecs_archetype *a = ecs_archetype_at(w, q->scanned);
        ++q->match_tests;
        if (ecs_query_matches(q, a)) {
            entry[0] = a->index;
            entry[1] = 0; /* row prefix: written by de_query_par_each */
            ecs_query_columns(q, a, entry + 2);
            if (de_array_push(&q->entries, entry) != DE_OK)
                return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "ecs query: out of memory for an archetype "
                                                     "match");
        }
        ++q->scanned;
    }
    return DE_OK;
}

void ecs_query_on_archetype_created(de_world *w, ecs_archetype *a, void *user) {
    (void)a; /* the scan starts at each query's own position, which also retries old failures */
    const ecs_query_registry *r = (const ecs_query_registry *)user;
    for (u32 i = 0; i < r->slots.count; ++i) {
        ecs_query *q = ((const ecs_query_slot *)r->slots.data)[i].q;
        if (q != NULL)
            (void)ecs_query_scan(w, q); /* on failure the iterator covers the rest */
    }
}

bool ecs_query_iter_next(const de_world *w, const ecs_query *q, ecs_query_iter *it,
                         ecs_archetype **a, const u32 **cols) {
    if (it->pos < q->entries.count) {
        const u32 *e = ecs_query_entry(q, it->pos++);
        *a = ecs_archetype_at(w, e[0]);
        *cols = e + 2;
        return true;
    }
    /* Only after an out-of-memory in the cache: match the unscanned archetypes on the fly. */
    const u32 n = ecs_archetype_count(w);
    while (it->next < n) {
        ecs_archetype *x = ecs_archetype_at(w, it->next++);
        if (ecs_query_matches(q, x)) {
            ecs_query_columns(q, x, it->cols);
            *a = x;
            *cols = it->cols;
            return true;
        }
    }
    return false;
}
