/* de_gameplay.h — navigation, tilemaps, video playback. SPEC.md §21.2, §21.4. */
#ifndef DE_GAMEPLAY_H
#define DE_GAMEPLAY_H
#include "de_base.h"
#include "de_math.h"
#include "de_ecs.h"
#include "de_asset.h"
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

typedef struct de_nav_path {
    de_dvec3 *points;
    u32 count;
    bool partial;
} de_nav_path;
DE_API de_result de_nav_find_path(de_nav *n, de_dvec3 from, de_dvec3 to, u32 area_mask,
                                  const de_allocator *a, de_nav_path *out);
DE_API bool de_nav_raycast(de_nav *n, de_dvec3 from, de_dvec3 to, de_dvec3 *hit);
DE_API bool de_nav_nearest_point(de_nav *n, de_dvec3 p, f32 search_radius, de_dvec3 *out);

/* Crowd agents with local avoidance. */
typedef struct DeNavAgent {
    f32 radius, height, max_speed, max_accel;
    de_dvec3 target;
    bool has_target;
    de_vec3 velocity;   /* output */
    de_nav_agent agent; /* internal */
} DeNavAgent;

/* 2D grid / polygon navigation on the configured 2D plane. */
typedef struct de_nav2d_grid {
    u32 width, height;
    f32 cell_size;
    de_vec2 origin;
    const u8 *walkable;
} de_nav2d_grid;
DE_API de_result de_nav2d_find_path(const de_nav2d_grid *grid, de_vec2 from, de_vec2 to,
                                    bool jump_point_search, const de_allocator *a, de_vec2 **points,
                                    u32 *count);

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
