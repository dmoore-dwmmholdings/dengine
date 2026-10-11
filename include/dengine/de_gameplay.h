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
