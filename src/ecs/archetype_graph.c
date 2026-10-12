/* archetype_graph.c: the archetype graph's edge cache (DENG-345, SPEC §7.1).
 *
 * Adding or removing one component moves an entity to a neighbouring archetype. Each archetype
 * caches those neighbours: `add_edges[c]` and `remove_edges[c]`, sorted by component id and
 * found by binary search. An edge is created on the first lookup that needs it, by the operation
 * that needs it, so the archetypes it creates appear in operation order (SPEC §5.7 rule 5). The
 * edges are only a cache: a lookup gives the same archetype with or without them, and nothing
 * iterates them to decide a result.
 *
 * A new edge a + c -> b also records the reverse edge b - c -> a (and the other way round), so a
 * tag that is added and removed again costs one set lookup, not two. */
#include "ecs_internal.h"

/* Sets up to this size are built on the stack; larger ones take a temporary allocation. */
#define GRAPH_STACK_SET 64u
/* First capacity of an edge array. */
#define GRAPH_EDGE_MIN 4u

/* Position of `component` in the sorted `edges[0..count)`, or the insertion point (with
 * `*found` false). */
static u32 edge_search(const ecs_edge *edges, u32 count, u32 component, bool *found) {
    u32 lo = 0, hi = count;
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2u;
        const u32 v = edges[mid].component;
        if (v == component) {
            *found = true;
            return mid;
        }
        if (v < component)
            lo = mid + 1u;
        else
            hi = mid;
    }
    *found = false;
    return lo;
}

/* Inserts (component -> target) into one edge array, keeping it sorted. An existing edge is left
 * alone (it has the same target: the graph is a function of the sets). Out of memory: the edge is
 * not cached, and nothing else changes. */
static void edge_insert(de_world *w, ecs_edge **edges, u32 *count, u32 *capacity, u32 component,
                        u32 target) {
    bool found = false;
    const u32 at = edge_search(*edges, *count, component, &found);
    if (found) {
        DE_ASSERT((*edges)[at].target == target);
        return;
    }
    if (*count == *capacity) {
        const u32 new_cap = *capacity ? *capacity * 2u : GRAPH_EDGE_MIN;
        ecs_edge *grown = (ecs_edge *)DE_ALLOC(w->a, (size_t)new_cap * sizeof(ecs_edge),
                                               DE_ALIGNOF(ecs_edge), DE_MEM_ECS);
        if (grown == NULL)
            return; /* a cache: run without this edge */
        if (*count > 0)
            memcpy(grown, *edges, (size_t)*count * sizeof(ecs_edge));
        if (*edges != NULL)
            DE_FREE(w->a, *edges, (size_t)*capacity * sizeof(ecs_edge));
        *edges = grown;
        *capacity = new_cap;
    }
    memmove(*edges + at + 1u, *edges + at, (size_t)(*count - at) * sizeof(ecs_edge));
    (*edges)[at].component = component;
    (*edges)[at].target = target;
    ++*count;
}

static void add_edge_insert(de_world *w, ecs_archetype *a, u32 component, u32 target) {
    edge_insert(w, &a->add_edges, &a->add_edge_count, &a->add_edge_capacity, component, target);
}

static void remove_edge_insert(de_world *w, ecs_archetype *a, u32 component, u32 target) {
    edge_insert(w, &a->remove_edges, &a->remove_edge_count, &a->remove_edge_capacity, component,
                target);
}

/* The archetype for `a`'s set with `component` inserted (`add`) or taken out (`!add`). `a`'s set
 * is sorted and unique, so the new set is too; it is built outside w->set_scratch, as
 * ecs_archetype_get_or_create requires. */
static de_result graph_resolve(de_world *w, const ecs_archetype *a, u32 component, bool add,
                               ecs_archetype **out) {
    const u32 n = add ? a->component_count + 1u : a->component_count - 1u;
    u32 stack_set[GRAPH_STACK_SET] = {0}; /* zeroed for GCC's -Wmaybe-uninitialized */
    u32 *set = stack_set;
    const size_t heap_bytes = (size_t)n * sizeof(u32);
    if (n > GRAPH_STACK_SET) {
        set = (u32 *)DE_ALLOC(w->a, heap_bytes, DE_ALIGNOF(u32), DE_MEM_ECS);
        if (set == NULL)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "ecs: out of memory for a component set");
    }
    u32 k = 0;
    bool placed = false;
    for (u32 i = 0; i < a->component_count; ++i) {
        const u32 id = a->components[i];
        if (add && !placed && component < id) {
            set[k++] = component;
            placed = true;
        }
        if (!add && id == component)
            continue;
        set[k++] = id;
    }
    if (add && !placed)
        set[k++] = component;
    DE_ASSERT(k == n);
    const de_result r = ecs_archetype_get_or_create(w, set, n, out);
    if (set != stack_set)
        DE_FREE(w->a, set, heap_bytes);
    return r;
}

de_result ecs_archetype_add_target(de_world *w, ecs_archetype *a, u32 component,
                                   ecs_archetype **out) {
    if (ecs_archetype_has(a, component)) {
        *out = a;
        return DE_OK;
    }
    bool found = false;
    const u32 at = edge_search(a->add_edges, a->add_edge_count, component, &found);
    if (found) {
        ++a->edge_hits;
        *out = ecs_archetype_at(w, a->add_edges[at].target);
        return DE_OK;
    }
    if (ecs_component_get(w, component) == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "ecs: component id %u is not registered",
                       component);
    ++a->edge_misses;
    ecs_archetype *b = NULL;
    DE_TRY(graph_resolve(w, a, component, true, &b));
    add_edge_insert(w, a, component, b->index);
    remove_edge_insert(w, b, component, a->index);
    *out = b;
    return DE_OK;
}

de_result ecs_archetype_remove_target(de_world *w, ecs_archetype *a, u32 component,
                                      ecs_archetype **out) {
    if (!ecs_archetype_has(a, component)) {
        *out = a;
        return DE_OK;
    }
    bool found = false;
    const u32 at = edge_search(a->remove_edges, a->remove_edge_count, component, &found);
    if (found) {
        ++a->edge_hits;
        *out = ecs_archetype_at(w, a->remove_edges[at].target);
        return DE_OK;
    }
    ++a->edge_misses;
    ecs_archetype *b = NULL;
    DE_TRY(graph_resolve(w, a, component, false, &b));
    remove_edge_insert(w, a, component, b->index);
    add_edge_insert(w, b, component, a->index);
    *out = b;
    return DE_OK;
}

void ecs_archetype_edges_free(de_world *w, ecs_archetype *a) {
    if (a->add_edges != NULL)
        DE_FREE(w->a, a->add_edges, (size_t)a->add_edge_capacity * sizeof(ecs_edge));
    if (a->remove_edges != NULL)
        DE_FREE(w->a, a->remove_edges, (size_t)a->remove_edge_capacity * sizeof(ecs_edge));
    a->add_edges = NULL;
    a->remove_edges = NULL;
    a->add_edge_count = a->add_edge_capacity = 0;
    a->remove_edge_count = a->remove_edge_capacity = 0;
}

void ecs_graph_stats_get(const de_world *w, ecs_graph_stats *out) {
    memset(out, 0, sizeof *out);
    for (u32 i = 0; i < ecs_archetype_count(w); ++i) {
        const ecs_archetype *a = ecs_archetype_at(w, i);
        out->edge_hits += a->edge_hits;
        out->edge_misses += a->edge_misses;
        out->add_edges += a->add_edge_count;
        out->remove_edges += a->remove_edge_count;
    }
}
