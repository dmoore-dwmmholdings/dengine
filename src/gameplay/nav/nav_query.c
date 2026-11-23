/*
 * nav_query.c: navmesh queries (DENG-77, SPEC §21.2): paths (A* and string pulling), surface
 * raycasts, nearest points, and deterministic mode (SPEC §5.7). The Detour calls are in the
 * bridge (nav_recast_bridge.cpp); this file maps engine space to Recast space and picks the
 * query object of the calling thread.
 *
 * Threading: one Detour query object per frame worker of the nav's task system, chosen by
 * de_task_worker_index(), so frame-worker jobs can query in parallel; without a task system there
 * is one, for the main thread. The navmesh changes only in de_nav_update, which must not overlap
 * queries.
 */
#include "gameplay/nav/nav_internal.h"

#include <dengine/de_base.h>
#include <dengine/de_gameplay.h>
#include <dengine/de_memory.h>
#include <dengine/de_task.h>

#include <string.h>

de_result de__nav_queries_init(de_nav *n) {
    if (n->queries != NULL)
        return DE_OK;
    const u32 count = n->tasks != NULL ? de_task_worker_count(n->tasks) : 1u;
    if (count == 0)
        return DE_ERR_INVALID_ARGUMENT;
    n->queries = (de__nav_query **)DE_ALLOC(n->alloc, sizeof(de__nav_query *) * count, 16,
                                            DE_MEM_GENERAL);
    if (n->queries == NULL)
        return DE_ERR_OUT_OF_MEMORY;
    memset(n->queries, 0, sizeof(de__nav_query *) * count);
    n->query_count = count;
    for (u32 i = 0; i < count; ++i) {
        DE_TRY(de__nav_query_create(n->mesh, &n->queries[i]));
        de__nav_query_set_deterministic(n->queries[i], n->deterministic);
    }
    return DE_OK;
}

void de__nav_queries_free(de_nav *n) {
    if (n->queries == NULL)
        return;
    for (u32 i = 0; i < n->query_count; ++i)
        de__nav_query_destroy(n->queries[i]);
    DE_FREE(n->alloc, n->queries, sizeof(de__nav_query *) * n->query_count);
    n->queries = NULL;
    n->query_count = 0;
}

void de_nav_set_deterministic(de_nav *n, bool on) {
    if (n == NULL)
        return;
    n->deterministic = on;
    for (u32 i = 0; i < n->query_count; ++i)
        de__nav_query_set_deterministic(n->queries[i], on);
}

/** The calling thread's query object, or NULL (no navmesh, or not a frame worker). */
static de__nav_query *query_of(const de_nav *n) {
    if (n->mesh == NULL || n->query_count == 0 || !n->has_origin)
        return NULL;
    if (n->tasks == NULL)
        return n->queries[0];
    const u32 i = de_task_worker_index();
    return i < n->query_count ? n->queries[i] : NULL;
}

static void make_args(const de_nav *n, u32 area_mask, de__nav_query_args *args) {
    const f32 r = n->has_settings ? n->settings.desc.agent_radius : 0.5f;
    const f32 h = n->has_settings ? n->settings.desc.agent_height : 2.0f;
    const f32 xz = 2.0f * r > 1.0f ? 2.0f * r : 1.0f;
    args->ext[0] = xz;
    args->ext[1] = h > 2.0f ? h : 2.0f;
    args->ext[2] = xz;
    const u32 m = area_mask & 0xFFFFu;
    args->include = (u16)(m != 0 ? m : 0xFFFFu);
}

de_result de_nav_find_path(de_nav *n, de_dvec3 from, de_dvec3 to, u32 area_mask,
                           const de_allocator *a, de_nav_path *out) {
    if (out != NULL)
        memset(out, 0, sizeof *out);
    if (n == NULL || out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_nav_find_path: NULL argument");
    if (a == NULL)
        a = de_heap_allocator();
    if (n->mesh == NULL || !n->has_origin)
        return DE_FAIL(DE_ERR_NOT_FOUND, "de_nav_find_path: the nav has no navmesh");
    de__nav_query *q = query_of(n);
    if (q == NULL)
        return DE_FAIL(DE_ERR_UNSUPPORTED,
                       "de_nav_find_path: called from a thread that is not a frame worker");
    de__nav_query_args args;
    make_args(n, area_mask, &args);
    f32 s[3], e[3];
    de__nav_world_to_recast(&n->origin, from, s);
    de__nav_world_to_recast(&n->origin, to, e);
    const f32 *pts = NULL;
    i32 count = 0;
    bool partial = false, bad_end = false;
    const de_result r = de__nav_query_path(q, &args, s, e, &pts, &count, &partial, &bad_end);
    if (r == DE_ERR_NOT_FOUND)
        return DE_FAIL(DE_ERR_NOT_FOUND, "de_nav_find_path: no navmesh near the %s (%g, %g, %g)",
                       bad_end ? "end" : "start", bad_end ? to.x : from.x,
                       bad_end ? to.y : from.y, bad_end ? to.z : from.z);
    if (r != DE_OK)
        return DE_FAIL(r, "de_nav_find_path: Detour query failed");
    de_dvec3 *p = (de_dvec3 *)DE_ALLOC(a, sizeof(de_dvec3) * (size_t)count, DE_ALIGNOF(de_dvec3),
                                       DE_MEM_GENERAL);
    if (p == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_nav_find_path: %d points", (int)count);
    for (i32 i = 0; i < count; ++i)
        p[i] = de__nav_recast_to_world(&n->origin, &pts[i * 3]);
    out->points = p;
    out->count = (u32)count;
    out->partial = partial;
    return DE_OK;
}

void de_nav_path_free(const de_allocator *a, de_nav_path *path) {
    if (path == NULL)
        return;
    if (path->points != NULL)
        DE_FREE(a != NULL ? a : de_heap_allocator(), path->points,
                sizeof(de_dvec3) * path->count);
    memset(path, 0, sizeof *path);
}

bool de_nav_raycast(de_nav *n, de_dvec3 from, de_dvec3 to, de_dvec3 *hit) {
    if (n == NULL)
        return false;
    de__nav_query *q = query_of(n);
    if (q == NULL) {
        if (hit != NULL)
            *hit = from;
        return true; /* no navmesh to walk on */
    }
    de__nav_query_args args;
    make_args(n, 0, &args);
    f32 s[3], e[3], h[3];
    de__nav_world_to_recast(&n->origin, from, s);
    de__nav_world_to_recast(&n->origin, to, e);
    const i32 r = de__nav_query_raycast(q, &args, s, e, h);
    if (r == 0)
        return false;
    if (hit != NULL)
        *hit = r > 0 ? de__nav_recast_to_world(&n->origin, h) : from;
    return true;
}

bool de_nav_nearest_point(de_nav *n, de_dvec3 p, f32 search_radius, de_dvec3 *out) {
    if (n == NULL || !(search_radius > 0.0f))
        return false;
    de__nav_query *q = query_of(n);
    if (q == NULL)
        return false;
    de__nav_query_args args;
    make_args(n, 0, &args);
    args.ext[0] = args.ext[1] = args.ext[2] = search_radius;
    f32 s[3], o[3];
    de__nav_world_to_recast(&n->origin, p, s);
    if (!de__nav_query_nearest(q, &args, s, o))
        return false;
    if (out != NULL)
        *out = de__nav_recast_to_world(&n->origin, o);
    return true;
}
