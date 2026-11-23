/*
 * nav_recast_bridge.h: the C interface of the Recast / Detour bridge (DENG-76). The only C++ of
 * the nav module is nav_recast_bridge.cpp, which implements these functions; no Recast or Detour
 * type crosses this header. Internal (de_nav_build), not installed.
 *
 * Coordinates are Recast space (Y up), relative to the navmesh origin: see nav_build.h for the
 * mapping from engine space (Z up).
 *
 * Memory: every Recast and Detour allocation goes through a de_allocator. The bridge installs
 * process-wide hooks (rcAllocSetCustom, dtAllocSetCustom) once. Each block records the allocator
 * it came from, so a block is freed to its allocator from any thread. Every function below that
 * can allocate takes the allocator and makes it the thread's current allocator for the call; a
 * Recast or Detour allocation outside such a call fails (and is counted, see
 * de__nav_rc_unscoped_allocs). The allocator must be thread-safe when tiles build on background
 * workers.
 */
#ifndef DE_GAMEPLAY_NAV_RECAST_BRIDGE_H
#define DE_GAMEPLAY_NAV_RECAST_BRIDGE_H

#include <dengine/de_base.h>
#include <dengine/de_memory.h>

DE_EXTERN_C_BEGIN

/** Settings of one tile build, in Recast units (cells, voxels) and space. */
typedef struct de__nav_rc_tile_config {
    f32 cs, ch;                /* cell size, cell height (m) */
    f32 walkable_slope_deg;    /* max walkable slope */
    i32 walkable_height;       /* voxels (ceil(agent_height / ch)) */
    i32 walkable_climb;        /* voxels (floor(agent_max_climb / ch)) */
    i32 walkable_radius;       /* cells (ceil(agent_radius / cs)) */
    i32 tile_cells;            /* tile width in cells (tile_size / cs) */
    i32 border;                /* cells of input around the tile */
    i32 tx, tz;                /* tile coordinates */
    f32 bmin[3], bmax[3];      /* tile bounds without the border; y is the input height range */
    f32 agent_radius, agent_height, agent_max_climb; /* m, stored in the tile header */
    /* Obstacle footprints made unwalkable (then inflated by the agent radius), each
     * DE__NAV_RC_OBSTACLE_FLOATS floats: 4 xz corners (convex, any winding), then ymin, ymax. */
    const f32 *obstacles;
    i32 obstacle_count;
    /* Off-mesh links (DENG-77), each DE__NAV_RC_OFFMESH_FLOATS floats: start xyz, end xyz,
     * radius, bidirectional (0 or 1), area (0..15). Detour keeps those that start in the tile. */
    const f32 *offmesh;
    i32 offmesh_count;
} de__nav_rc_tile_config;

/** Floats per obstacle in de__nav_rc_tile_config.obstacles. */
#define DE__NAV_RC_OBSTACLE_FLOATS 10
/** Floats per off-mesh link in de__nav_rc_tile_config.offmesh. */
#define DE__NAV_RC_OFFMESH_FLOATS 9

/**
 * Builds one Detour tile from a triangle soup (`verts`: nv x 3 floats, `tris`: nt x 3 indices,
 * front faces counter-clockwise seen from +Y). The triangles may reach outside the tile; Recast
 * clips them. On success `*out_data` is NULL (no walkable polygon, `*out_size` 0) or a tile
 * owned by the caller, from `a`; free it with de__nav_rc_free or give it to
 * de__nav_mesh_set_tile. Deterministic: the same input gives the same bytes.
 * @return DE_OK, DE_ERR_OUT_OF_MEMORY or DE_ERR_INTERNAL (a Recast stage failed).
 */
de_result de__nav_rc_build_tile(const de_allocator *a, const de__nav_rc_tile_config *cfg,
                                const f32 *verts, i32 nv, const i32 *tris, i32 nt,
                                u8 **out_data, i32 *out_size);

/** A Detour-owned block of `size` bytes from `a` (for a tile copied from a cooked navmesh). */
u8 *de__nav_rc_alloc(const de_allocator *a, i32 size);
/** Frees a block of de__nav_rc_build_tile or de__nav_rc_alloc. NULL is ignored. */
void de__nav_rc_free(void *p);

/** Recast / Detour allocations that came with no current allocator (should stay 0). */
u64 de__nav_rc_unscoped_allocs(void);

/** A tiled Detour navmesh. */
typedef struct de__nav_mesh de__nav_mesh;

/** Most tiles of one navmesh. */
#define DE__NAV_MAX_TILES 4096

/** Creates an empty navmesh with square tiles of `tile_size` m, its tile grid at 0. */
de_result de__nav_mesh_create(const de_allocator *a, f32 tile_size, de__nav_mesh **out);
/** Destroys a navmesh and its tiles. NULL is ignored. */
void de__nav_mesh_destroy(de__nav_mesh *m);
/**
 * Replaces the tile at (tx, tz): removes the old one (if any), then adds `data` (if not NULL),
 * which the navmesh then owns. `data` must come from the navmesh's allocator. On failure `data`
 * is freed. @return DE_OK, DE_ERR_OUT_OF_MEMORY (tile limit) or DE_ERR_PARSE (bad tile data).
 */
de_result de__nav_mesh_set_tile(de__nav_mesh *m, i32 tx, i32 tz, u8 *data, i32 size);
/** The Detour tile reference of (tx, tz), or 0 when there is no tile there. */
u64 de__nav_mesh_tile_ref(const de__nav_mesh *m, i32 tx, i32 tz);
/** Tiles with data. */
u32 de__nav_mesh_tile_count(const de__nav_mesh *m);
/** Polygons over every tile. */
u32 de__nav_mesh_poly_count(const de__nav_mesh *m);
/**
 * true when the Recast-space point `p` lies over a polygon of the tile that contains it, in the
 * xz plane, and within `max_dy` m of the polygon's height range.
 */
bool de__nav_mesh_point_on_poly(const de__nav_mesh *m, const f32 p[3], f32 max_dy);
/** FNV-1a 64 of every tile's position and geometry (vertices, polygons, detail mesh, off-mesh
 *  connections), in tile-grid order; links between polygons are left out. 0 for NULL. */
u64 de__nav_mesh_hash(const de__nav_mesh *m);

/* ---- Queries (DENG-77) ----------------------------------------------------------------------- */

/** A Detour query object with its own buffers; one per thread that queries. */
typedef struct de__nav_query de__nav_query;

/** Longest polygon corridor of one path query; a longer one makes the path partial. */
#define DE__NAV_QUERY_MAX_POLYS 512
/** Most corner points of one path. */
#define DE__NAV_QUERY_MAX_POINTS 256

/** Creates a query object for `m` (from the navmesh's allocator). */
de_result de__nav_query_create(de__nav_mesh *m, de__nav_query **out);
/** NULL is ignored. */
void de__nav_query_destroy(de__nav_query *q);
/** Deterministic mode: Detour's math calls de_dmath during this object's queries. */
void de__nav_query_set_deterministic(de__nav_query *q, bool on);

/** Query input shared by the calls below: polygons need a flag of `include` (bit i = area i). */
typedef struct de__nav_query_args {
    f32 ext[3]; /* half extents of the box searched for the nearest polygon */
    u16 include;
} de__nav_query_args;

/**
 * Path from `start` to `end`. On DE_OK `*points` (`*count` corners, 3 floats each, valid until the
 * next call on `q`) starts at the start point and `*partial` tells whether the end was reached.
 * DE_ERR_NOT_FOUND when no polygon is near the start (`*bad_end` false) or the end (true);
 * DE_ERR_INTERNAL when Detour fails.
 */
de_result de__nav_query_path(de__nav_query *q, const de__nav_query_args *args, const f32 start[3],
                             const f32 end[3], const f32 **points, i32 *count, bool *partial,
                             bool *bad_end);
/** Surface raycast: 1 = hit a wall (`hit` set), 0 = reached `end`, -1 = start off the mesh. */
i32 de__nav_query_raycast(de__nav_query *q, const de__nav_query_args *args, const f32 start[3],
                          const f32 end[3], f32 hit[3]);
/** The nearest point on a polygon within `args->ext` of `p`; false when there is none. */
bool de__nav_query_nearest(de__nav_query *q, const de__nav_query_args *args, const f32 p[3],
                           f32 out[3]);

DE_EXTERN_C_END

#endif /* DE_GAMEPLAY_NAV_RECAST_BRIDGE_H */
