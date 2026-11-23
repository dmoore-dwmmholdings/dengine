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
} de__nav_obstacle;

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

    de_dvec3 *extra; /* test geometry: 3 points per triangle */
    u32 extra_tri_count, extra_tri_cap;

    de__nav_job *jobs_head, *jobs_tail; /* in submission order */
};

/** Adds the colliders of `n->world` (static, layers in `mask`, 0 = every layer) to `soup`. */
void de__nav_gather_world(de_nav *n, de__nav_soup *soup, u32 mask);
/** Adds the obstacles (entities with a transform) to `soup`. */
void de__nav_gather_obstacles(de_nav *n, de__nav_soup *soup);
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

DE_EXTERN_C_END

#endif /* DE_GAMEPLAY_NAV_INTERNAL_H */
