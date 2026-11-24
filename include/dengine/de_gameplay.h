/* de_gameplay.h — navigation, tilemaps, video playback. SPEC.md §21.2, §21.4. */
#ifndef DE_GAMEPLAY_H
#define DE_GAMEPLAY_H
#include "de_base.h"
#include "de_math.h"
#include "de_ecs.h"
#include "de_asset.h"
#include "de_render.h" /* de_plane_2d (§12.12) */
DE_EXTERN_C_BEGIN

/* ===================== Navigation ===================== */
typedef struct de_nav de_nav;
DE_HANDLE(de_nav_agent);

typedef struct de_navmesh_build_desc {
    f32 cell_size, cell_height;
    f32 agent_radius, agent_height, agent_max_climb, agent_max_slope_rad;
    f32 tile_size;      /* meters; tiles rebuild independently */
    u32 collision_mask; /* physics layers used as input */
} de_navmesh_build_desc;

DE_API de_result de_nav_create(const de_allocator *a, de_task_system *tasks, de_nav **out);
DE_API void de_nav_destroy(de_nav *n);
DE_API void de_nav_attach(de_nav *n, de_world *w);
DE_API void de_nav_add_navmesh(de_nav *n, de_navmesh mesh); /* cooked or runtime-built */
DE_API de_result de_nav_rebuild_tiles(de_nav *n, const de_navmesh_build_desc *desc,
                                      de_aabb region); /* background */
DE_API void de_nav_add_obstacle(de_nav *n, de_entity e, de_vec3 half_extents);

/* ---- DENG-76: navmesh generation (Recast / Detour) ----
 * Main thread only. de_nav_rebuild_tiles: `region` is a world-space box; the tiles it meets are
 * rebuilt from the attached world's static colliders (collision_mask 0 = every layer) and the
 * obstacles, on a background task (inline when the nav has no task system). Call it outside
 * schedules. The new tiles appear at a later de_nav_update (the `nav.update` PRE_UPDATE system of
 * the attached world calls it). tile_size is fixed by the first build or navmesh and must divide
 * 256 m into whole tiles. Obstacles are entity-placed boxes carved from later rebuilds. */
DE_API void de_nav_update(de_nav *n);
/** Registers the `navmesh` asset type with `assets` and uses it for de_nav_add_navmesh. */
DE_API de_result de_nav_use_assets(de_nav *n, de_asset_system *assets);
DE_API de_navmesh de_asset_navmesh(de_asset_system *s, de_asset_id id);

/* ---- DENG-77: queries, off-mesh links, dynamic obstacles ----
 * Queries run on the main thread or on a frame worker of the nav's task system (one Detour
 * query object per frame worker, made with the first navmesh); not on background workers, and
 * not while de_nav_update runs. They allocate nothing but the path points (from `a`).
 * de_nav_find_path: A* over the polygons, then string pulling (corner points, `from` first).
 * `area_mask` bit i admits area i (walkable ground is area 0; off-mesh links carry their own
 * area); 0 = every area. When `to` cannot be reached the path ends at the reachable point nearest
 * to it and `partial` is true. DE_ERR_NOT_FOUND when `from` or `to` is not near the navmesh.
 * de_nav_raycast: walks the surface from `from` toward `to`; true at a wall (`*hit` is the point,
 * on the surface) or when `from` is off the navmesh (`*hit` = `from`), false when it gets there.
 * de_nav_nearest_point: the navmesh point nearest to `p` within `search_radius` m on each axis.
 * Off-mesh links (jumps, ladders, doors) join two points `radius` m from the navmesh; they are
 * built into the tile of `from` by later rebuilds (de_nav_rebuild_tiles, obstacle rebuilds).
 * Obstacles (de_nav_add_obstacle): each de_nav_update rebuilds, a few per frame, the tiles of
 * obstacles that were added, resized, removed, or moved more than two cells or turned since they
 * were carved; queries see the change when the rebuild lands. Deterministic mode (SPEC §5.7):
 * queries use de_dmath instead of libm, and de_nav_update applies every pending rebuild at once
 * (it waits for them), so the navmesh changes on the same frame on every machine. */
typedef struct de_nav_path {
    de_dvec3 *points; /* from the allocator given to de_nav_find_path */
    u32 count;
    bool partial;
} de_nav_path;
DE_API de_result de_nav_find_path(de_nav *n, de_dvec3 from, de_dvec3 to, u32 area_mask,
                                  const de_allocator *a, de_nav_path *out);
/** Frees `path->points` to `a` (the allocator given to de_nav_find_path) and zeroes `path`. */
DE_API void de_nav_path_free(const de_allocator *a, de_nav_path *path);
DE_API bool de_nav_raycast(de_nav *n, de_dvec3 from, de_dvec3 to, de_dvec3 *hit);
DE_API bool de_nav_nearest_point(de_nav *n, de_dvec3 p, f32 search_radius, de_dvec3 *out);
/** Adds an off-mesh link from `from` to `to` (both ways when `bidirectional`), area `area`
 *  (0..15; de_nav_find_path's `area_mask` bit). Main thread only. */
DE_API de_result de_nav_add_offmesh_link(de_nav *n, de_dvec3 from, de_dvec3 to, f32 radius,
                                         bool bidirectional, u8 area);
/** Removes the obstacle of `e`; its tiles are rebuilt by a later de_nav_update. */
DE_API void de_nav_remove_obstacle(de_nav *n, de_entity e);
/** Deterministic query mode (off by default; SPEC §5.7). Main thread only. */
DE_API void de_nav_set_deterministic(de_nav *n, bool on);

/* Crowd agents with local avoidance. */
typedef struct DeNavAgent {
    f32 radius, height, max_speed, max_accel;
    de_dvec3 target;
    bool has_target;
    de_vec3 velocity;   /* output */
    de_nav_agent agent; /* internal */
} DeNavAgent;

/* 2D grid / polygon navigation on the configured 2D plane. */
/* ---- DENG-79: 2D navigation (pure C, deterministic, SPEC §21.2, §12.12, §5.7) ----
 * Plane coordinates are de_vec2 on the game's 2D plane (de_plane_2d, §12.12); convert with
 * de_nav2d_to_world / de_nav2d_from_world, which match 2D physics (§13.3). Every function here is
 * pure: it reads its inputs only, so any thread may call it at any time and the result does not
 * depend on call order. Paths and scratch come from `a` (NULL = de_heap_allocator()); scratch is
 * released before return. Free a path with de_nav2d_path_free. */
typedef struct de_nav2d_grid {
    u32 width, height;
    f32 cell_size;     /* > 0, in plane units */
    de_vec2 origin;    /* plane position of the corner of cell (0, 0) */
    const u8 *walkable; /* width * height cells, row-major (y * width + x); non-zero = walkable */
} de_nav2d_grid;
/**
 * Shortest 8-neighbour path on `grid` from the cell of `from` to the cell of `to`: A* with the
 * octile heuristic (straight cost 1, diagonal cost 1.4142, in fixed point) and no corner cutting
 * (a diagonal step needs both orthogonal cells walkable). `jump_point_search` uses Jump Point
 * Search instead, which finds a path of the same cost and expands fewer nodes. Ties break on
 * f, then h, then cell index, so the path is the same on every machine.
 * `*points` gets the turning points as cell centres, from the start cell to the goal cell
 * (one point when both are the same cell). DE_ERR_NOT_FOUND when `from` or `to` is outside the
 * grid or on a blocked cell, or no path exists; DE_ERR_INVALID_ARGUMENT for a bad grid.
 */
DE_API de_result de_nav2d_find_path(const de_nav2d_grid *grid, de_vec2 from, de_vec2 to,
                                    bool jump_point_search, const de_allocator *a, de_vec2 **points,
                                    u32 *count);
/** Frees `points` (`count` points from de_nav2d_find_path or de_nav2d_mesh_find_path) to `a`. */
DE_API void de_nav2d_path_free(const de_allocator *a, de_vec2 *points, u32 count);

/** A closed polygon: `count` >= 3 points, either winding, no self-intersections. */
typedef struct de_nav2d_polygon {
    const de_vec2 *points;
    u32 count;
} de_nav2d_polygon;
/** de_nav2d_mesh_build input. Polygons are walkable areas already inset by the author for the
 *  agent radius (no erosion is done). Each hole lies inside one outline (the first outline that
 *  contains its first point) and does not touch other holes. */
typedef struct de_nav2d_mesh_desc {
    const de_nav2d_polygon *outlines;
    u32 outline_count;
    const de_nav2d_polygon *holes;
    u32 hole_count;
} de_nav2d_mesh_desc;
typedef struct de_nav2d_mesh de_nav2d_mesh;
/**
 * Triangulates the walkable area (ear clipping, holes bridged into their outline) and links
 * the triangles across shared edges (outlines that share vertices are joined), then builds the
 * visibility graph of the reflex corners (O(corners^2 * boundary edges)). The mesh is
 * immutable and owned by the caller. DE_ERR_INVALID_ARGUMENT for an empty or degenerate input.
 */
DE_API de_result de_nav2d_mesh_build(const de_nav2d_mesh_desc *desc, const de_allocator *a,
                                     de_nav2d_mesh **out);
DE_API void de_nav2d_mesh_destroy(de_nav2d_mesh *m);
/**
 * Shortest path from `from` to `to` inside the mesh: a straight line when in sight, otherwise
 * A* over the visibility graph of the reflex boundary corners (built by de_nav2d_mesh_build).
 * The points are `from`, the corners, then `to`. DE_ERR_NOT_FOUND when an
 * end is outside every triangle (snap it with de_nav2d_mesh_nearest_point) or the two are not
 * connected.
 */
DE_API de_result de_nav2d_mesh_find_path(const de_nav2d_mesh *m, de_vec2 from, de_vec2 to,
                                         const de_allocator *a, de_vec2 **points, u32 *count);
/** The mesh point nearest to `p` (`p` itself when inside). False for an empty mesh. */
DE_API bool de_nav2d_mesh_nearest_point(const de_nav2d_mesh *m, de_vec2 p, de_vec2 *out);
/** Plane point to world: DE_PLANE_SIDE_XZ gives (x, depth, y), DE_PLANE_TOP_XY (x, y, depth). */
DE_API de_dvec3 de_nav2d_to_world(de_plane_2d plane, de_vec2 p, f64 depth);
/** World point to plane: drops the out-of-plane axis (y for SIDE_XZ, z for TOP_XY). */
DE_API de_vec2 de_nav2d_from_world(de_plane_2d plane, de_dvec3 p);

/* ===================== Tilemaps (LDtk / Tiled / Aseprite tilesets) ===================== */
typedef struct DeTilemap {
    de_tilemap map;
    u32 layer_mask;          /* which layers to render */
    bool generate_colliders; /* → DeCollider2D chains */
    bool spawn_entities;     /* map entities → prefabs */
    bool lit;                /* full PBR/RT lighting like sprites */
} DeTilemap;

DE_API u32 de_tilemap_get_tile(de_asset_system *s, de_tilemap m, u32 layer, i32 x, i32 y);
DE_API void de_tilemap_set_tile(de_world *w, de_entity e, u32 layer, i32 x, i32 y,
                                u32 tile); /* runtime edit */

/* ---- DENG-80: tilemap runtime ----
 *
 * A `tilemap` asset (manifest `type = "tilemap"`, an LDtk `.ldtk` or Tiled `.tmj` / `.tmx`
 * source, cooked by the built-in importer) holds layers of cells. Cells are addressed per layer
 * as (x, y), x right and y down from the layer's top-left cell, like the editors. A tile value is
 * 0 for an empty cell, else (tile index + 1) | DE_TILEMAP_FLIP_* bits; the tile index counts the
 * map's tiles over all its tilesets, in tileset order (Tiled's gid minus the tileset's firstgid,
 * plus the tile counts of the tilesets before it).
 *
 * An entity with DeTilemap (and DeTransform; it gets an identity one if it has none) shows the
 * map once the asset is READY: de_tilemap_attach adds the PostUpdate system `tilemap.update`,
 * which builds the map in 32 x 32 cell chunks. Each chunk is a child entity of the map entity
 * whose children draw the chunk's tiles through the Sprite material model (DeSprite: lit, in the
 * visibility buffer and RT like any sprite, SPEC §12.12). `layer_mask` 0 shows every layer; else
 * bit i shows layer i (layers 32 and up follow bit 31). `generate_colliders` adds static 2D
 * colliders (DeCollider2D root entities): solid tiles merged into boxes per chunk, and the
 * tiles' own collision shapes (polygons, chains of at most 8 points). `spawn_entities`
 * instantiates the prefab mapped to each map entity, once, at its position, with its fields
 * applied by reflection path. `lit` sets the sprites' `cast_shadows`.
 *
 * Map space is the configured 2D plane (SPEC §12.12): side view maps (u, v) to world (X, Z),
 * top-down to (X, Y); layer i sits `layer_spacing * i` towards the camera.
 *
 * de_tilemap_set_tile changes one cell of the entity's live copy of the map (not the asset) and
 * marks its chunk; the next run of `tilemap.update` (or de_tilemap_flush) rebuilds that chunk's
 * tiles and colliders only. de_tilemap_get_tile reads the asset's cooked cells (0 outside the
 * layer, or when the asset is not READY).
 */
#define DE_TILEMAP_TILE_MASK 0x00FFFFFFu /**< tile index + 1 */
#define DE_TILEMAP_FLIP_D    (1u << 29)  /**< diagonal flip (applied first), as Tiled */
#define DE_TILEMAP_FLIP_V    (1u << 30)
#define DE_TILEMAP_FLIP_H    (1u << 31)

/** de_tilemap_attach() parameters; `(de_tilemap_desc){0}` is valid. */
typedef struct de_tilemap_desc {
    bool top_down;       /**< false: side view (X-Z plane), true: top-down (X-Y plane) */
    f32 layer_spacing;   /**< meters between layers along the view axis; 0 = 0.01 */
} de_tilemap_desc;

/**
 * Registers the `tilemap` asset type with `assets` (if no type of that name exists; Debug and Dev
 * builds with the cooker get the built-in importer), the reflected DeTilemap component, the
 * resource `DeTilemapSystemRef` and the system `tilemap.update` (PostUpdate, exclusive, main
 * thread, before transform propagation) in `w`. Attaching the same world again does nothing. The
 * asset system must outlive the world's use of tilemaps; the runtime state is freed with the world
 * (or by de_tilemap_detach). Main thread, outside schedules.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (NULL `w`), DE_ERR_OUT_OF_MEMORY, or the registration's
 *         error.
 */
DE_API de_result de_tilemap_attach(de_world *w, de_asset_system *assets,
                                   const de_tilemap_desc *desc);
/** Removes the system and the resource and frees the runtime state. Entities stay. */
DE_API void de_tilemap_detach(de_world *w);
/** Runs the work of `tilemap.update` now (no animation time passes): builds new maps and dirty
 * chunks. For tools and editors outside the frame loop. Main thread, outside schedules. */
DE_API void de_tilemap_flush(de_world *w);
/** Typed access to a `tilemap` asset (see de_asset_texture). */
DE_API de_tilemap de_asset_tilemap(de_asset_system *s, de_asset_id id);
/** The live tile of the entity's map (after set_tile edits), or 0 (no live map, outside). */
DE_API u32 de_tilemap_get_cell(de_world *w, de_entity e, u32 layer, i32 x, i32 y);
/* ---- end DENG-80 ---- */

/* ===================== Video (AV1 + Opus, WebM/MKV) ===================== */
typedef struct DeVideoPlayer {
    de_video video;
    de_texture output; /* written by the decoder; use in materials or UI */
    bool playing, loop;
    f32 volume;
    f64 time;      /* output; seconds */
    bool finished; /* output */
} DeVideoPlayer;

DE_API void de_video_seek(de_world *w, de_entity e, f64 seconds);

DE_EXTERN_C_END
#endif
