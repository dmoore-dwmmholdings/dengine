/* access.c: system access sets and the conflict test (DENG-350, SPEC §7.3).
 *
 * An access set is three sorted arrays (components, resources, event types) of {id, write}. Two
 * systems conflict when one is exclusive, or when they share an id of the same kind and at least
 * one of them writes it. Building and testing are integer-only and order-independent of hash maps
 * (SPEC §5.7 rule 5). */
#include "system_internal.h"

void ecs_access_bounds(const de_system_desc *d, u32 *components, u32 *resources, u32 *events) {
    u32 c = 0;
    for (u32 i = 0; i < d->query_count; ++i) {
        const de_query_desc *q = &d->queries[i];
        c += q->term_count;
        for (u32 j = 0; j < q->filter_count; ++j)
            if (q->filters[j].filter == DE_ADDED || q->filters[j].filter == DE_CHANGED)
                ++c;
    }
    *components = c;
    *resources = d->resource_count;
    *events = d->reads_event_count + d->writes_event_count;
}

/* Insertion sort by id, then merge equal ids (a write wins). Returns the new count. */
static u32 sort_merge(ecs_access_item *items, u32 n) {
    for (u32 i = 1; i < n; ++i) {
        const ecs_access_item x = items[i];
        u32 j = i;
        while (j > 0 && items[j - 1].id > x.id) {
            items[j] = items[j - 1];
            --j;
        }
        items[j] = x;
    }
    u32 out = 0;
    for (u32 i = 0; i < n; ++i) {
        if (out > 0 && items[out - 1].id == items[i].id) {
            items[out - 1].write |= items[i].write;
            continue;
        }
        items[out++] = items[i];
    }
    return out;
}

static bool access_is_write(de_access a) {
    return a == DE_WRITE || a == DE_OPTIONAL_WRITE;
}

void ecs_access_build(const de_system_desc *d, ecs_access_set *out) {
    u32 n = 0;
    for (u32 i = 0; i < d->query_count; ++i) {
        const de_query_desc *q = &d->queries[i];
        for (u32 j = 0; j < q->term_count; ++j) {
            out->components[n].id = q->terms[j].component.id;
            out->components[n].write = access_is_write(q->terms[j].access) ? 1u : 0u;
            ++n;
        }
        for (u32 j = 0; j < q->filter_count; ++j) {
            const de_filter f = q->filters[j].filter;
            if (f != DE_ADDED && f != DE_CHANGED)
                continue; /* WITH / WITHOUT only select archetypes: not access */
            out->components[n].id = q->filters[j].component.id;
            out->components[n].write = 0;
            ++n;
        }
    }
    out->component_count = sort_merge(out->components, n);

    n = 0;
    for (u32 i = 0; i < d->resource_count; ++i) {
        out->resources[n].id = d->resources[i].id.id;
        out->resources[n].write = access_is_write(d->resources[i].access) ? 1u : 0u;
        ++n;
    }
    out->resource_count = sort_merge(out->resources, n);

    n = 0;
    for (u32 i = 0; i < d->reads_event_count; ++i) {
        out->events[n].id = d->reads_events[i].id;
        out->events[n].write = 0;
        ++n;
    }
    for (u32 i = 0; i < d->writes_event_count; ++i) {
        out->events[n].id = d->writes_events[i].id;
        out->events[n].write = 1;
        ++n;
    }
    out->event_count = sort_merge(out->events, n);
    out->exclusive = (d->flags & DE_SYSTEM_EXCLUSIVE) != 0;
}

/* Merge walk of two sorted arrays: true at the first shared id that one side writes. */
static bool items_conflict(const ecs_access_item *a, u32 na, const ecs_access_item *b, u32 nb) {
    u32 i = 0, j = 0;
    while (i < na && j < nb) {
        if (a[i].id < b[j].id) {
            ++i;
        } else if (a[i].id > b[j].id) {
            ++j;
        } else {
            if (a[i].write | b[j].write)
                return true;
            ++i;
            ++j;
        }
    }
    return false;
}

bool ecs_access_conflict(const ecs_access_set *a, const ecs_access_set *b) {
    if (a->exclusive || b->exclusive)
        return true;
    return items_conflict(a->components, a->component_count, b->components, b->component_count) ||
           items_conflict(a->resources, a->resource_count, b->resources, b->resource_count) ||
           items_conflict(a->events, a->event_count, b->events, b->event_count);
}

const ecs_access_item *ecs_access_find(const ecs_access_item *items, u32 count, u32 id) {
    u32 lo = 0, hi = count;
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2u;
        if (items[mid].id < id)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo < count && items[lo].id == id ? &items[lo] : NULL;
}
