/*
 * nav_internal.h: the nav runtime's state (DENG-76), shared by nav.c (lifetime, builds,
 * updates), nav_input.c (input from the ECS world) and nav_asset.c (the `navmesh` asset type).
 * The test hooks at the end are for tests/unit/nav only. Internal, not installed.
 */
#ifndef DE_GAMEPLAY_NAV_INTERNAL_H
#define DE_GAMEPLAY_NAV_INTERNAL_H

#include "gameplay/nav/nav_build.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_ecs.h>
#include <dengine/de_gameplay.h>
#include <dengine/de_task.h>

DE_EXTERN_C_BEGIN

typedef struct de__nav_obstacle {
    de_entity entity;
    de_vec3 half_extents;
    bool dirty;   /* added or resized since it was last carved (DENG-77) */
    bool removed; /* de_nav_remove_obstacle: its tiles are rebuilt, then it is dropped */
    bool carved;  /* a rebuild took it in, at carved_pose with carved_half */
    de__nav_pose carved_pose;
    de_vec3 carved_half;
} de__nav_obstacle;

/** An off-mesh link (de_nav_add_offmesh_link), world space. */
typedef struct de__nav_link {
    de_dvec3 from, to;
    f32 radius;
    bool bidirectional;
    u8 area;
} de__nav_link;

/** One de_nav_rebuild_tiles call: its input (gathered on the main thread) and its output. */
typedef struct de__nav_job {
    de_nav *nav;
    de__nav_settings settings;
    de__nav_soup soup;
    i32 tx0, tz0, tx1, tz1; /* inclusive */
    u32 tile_count;
    u8 **data;  /* tile_count, row-major from (tx0, tz0); NULL = no tile there */
    i32 *sizes; /* tile_count */
    de_result result;
    de_task_counter counter; /* zeroed; used when the job runs on a background worker */
    bool on_worker;
    struct de__nav_job *next;
} de__nav_job;

/** The ECS resource de_nav_attach inserts. */
typedef struct DeNavRef {
    de_nav *ptr;
} DeNavRef;

struct de_nav {
    const de_allocator *alloc;
    de_task_system *tasks;
    de_log_category log;

    de__nav_mesh *mesh; /* created with the first settings */
    bool has_settings;
    de__nav_settings settings; /* the latest build's (tile_size fixed by the first) */
    bool has_origin;
    de_dvec3 origin;

    de_world *world;
    de_resource_id resource;
    de_system system;
    de_asset_system *assets;

    de__nav_obstacle *obstacles;
    u32 obstacle_count, obstacle_cap;
    u32 obstacle_cursor; /* where the next de_nav_update starts checking obstacles */

    de__nav_link *links;
    u32 link_count, link_cap;

    /* DENG-77: one Detour query object per frame worker (index de_task_worker_index()), or one
     * for the main thread without a task system. Made with the navmesh. */
    de__nav_query **queries;
    u32 query_count;
    /* de_nav_set_deterministic: de_dmath in queries, every rebuild applied at once. DENG-78
     * crowds read it too. */
    bool deterministic;

    de_dvec3 *extra; /* test geometry: 3 points per triangle */
    u32 extra_tri_count, extra_tri_cap;

    de__nav_job *jobs_head, *jobs_tail; /* in submission order */
};

/** Adds the colliders of `n->world` (static, layers in `mask`, 0 = every layer) to `soup`. */
void de__nav_gather_world(de_nav *n, de__nav_soup *soup, u32 mask);
/** Adds the obstacles (entities with a transform) to `soup`. Obstacles whose tiles lie inside
 *  the job's range [tx0, tx1] x [tz0, tz1] are marked carved at their current pose. */
void de__nav_gather_obstacles(de_nav *n, de__nav_soup *soup, i32 tx0, i32 tz0, i32 tx1, i32 tz1);
/** The inclusive tile range a box obstacle at `p` touches (footprint, border and agent radius). */
void de__nav_obstacle_tiles(const de_nav *n, const de__nav_pose *p, de_vec3 half_extents, i32 *tx0,
                            i32 *tz0, i32 *tx1, i32 *tz1);
/** Queues a rebuild of tiles [tx0, tx1] x [tz0, tz1] with the navmesh's latest settings (the
 *  navmesh must exist). As de_nav_rebuild_tiles after its checks. */
de_result de__nav_rebuild_range(de_nav *n, const de__nav_settings *st, i32 tx0, i32 tz0, i32 tx1,
                                i32 tz1);
/** The obstacle step of de_nav_update (nav_obstacles.c): rebuilds the tiles of a few obstacles
 *  that changed since they were carved. */
void de__nav_update_obstacles(de_nav *n);
/** Makes the query objects (nav_query.c) once the navmesh exists. */
de_result de__nav_queries_init(de_nav *n);
void de__nav_queries_free(de_nav *n);
/** The world pose of `e` (DeGlobalTransform, else DeTransform); false when it has neither. */
bool de__nav_entity_pose(de_world *w, de_entity e, de__nav_pose *out);

/** The bytes of a READY `navmesh` asset, or NULL. */
const u8 *de__nav_asset_bytes(de_asset_system *s, de_navmesh m, u64 *size);

/* ---- Test hooks (tests/unit/nav) ------------------------------------------------------------- */

/** Adds world-space triangles (3 points each) to every later build's input. */
de_result de__nav_add_test_tris(de_nav *n, const de_dvec3 *points, u32 tri_count);
/** Adds the tiles of a DNAV container (as de_nav_add_navmesh does with an asset's bytes). */
de_result de__nav_add_container(de_nav *n, const u8 *data, u64 size);
/** true when the world point `p` lies over a polygon, within `max_dz` m vertically. */
bool de__nav_point_on_mesh(const de_nav *n, de_dvec3 p, f32 max_dz);
/** The Detour tile reference of tile (tx, tz), 0 when there is none. */
u64 de__nav_tile_ref(const de_nav *n, i32 tx, i32 tz);
/** Tiles with data. */
u32 de__nav_tile_count(const de_nav *n);
/** The tile that holds world point `p`; false before the origin is known. */
bool de__nav_tile_of(const de_nav *n, de_dvec3 p, i32 *tx, i32 *tz);
/** The navmesh origin; false before the first build or navmesh. */
bool de__nav_origin(const de_nav *n, de_dvec3 *out);
/** Builds still waiting for de_nav_update. */
u32 de__nav_pending_jobs(const de_nav *n);
/** FNV-1a 64 of the navmesh's tile geometry (0 without a navmesh). */
u64 de__nav_mesh_checksum(const de_nav *n);
/** Obstacles in the list (removed ones stay until their rebuild is queued). */
u32 de__nav_obstacle_count(const de_nav *n);

DE_EXTERN_C_END

#endif /* DE_GAMEPLAY_NAV_INTERNAL_H */
