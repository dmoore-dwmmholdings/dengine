/*
 * tilemap_internal.h: the tilemap runtime's internals (DENG-80), and the engine-internal API the
 * editor service (src/host/editor/) uses to paint tiles: picking a cell from a world point, the
 * grid geometry, the tile palette (UV rects and tileset textures) and live edits.
 * Include as "gameplay/tilemap/tilemap_internal.h". Main thread, outside running schedules
 * (the editor service runs between frames), unless noted.
 */
#ifndef DE_GAMEPLAY_TILEMAP_INTERNAL_H
#define DE_GAMEPLAY_TILEMAP_INTERNAL_H

#include "gameplay/tilemap/tilemap_cooked.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_ecs.h>
#include <dengine/de_gameplay.h>
#include <dengine/de_math.h>
#include <dengine/de_physics.h>

DE_EXTERN_C_BEGIN

/* ======================== Editor-facing API ======================== */

/** One layer of an entity's map, in map space (see de_gameplay.h and tilemap_cooked.h). */
typedef struct de__tilemap_layer_info {
    const char *name;   /**< "ground", or "<level>/<layer>" for LDtk; valid while the map is */
    u32 width, height;  /**< cells */
    f32 cell_w, cell_h; /**< map units (meters) per cell */
    de_vec2 origin;     /**< map position of the top-left corner of cell (0, 0) */
    f32 depth;          /**< offset along the view axis (layer_spacing * index) */
    u32 flags;          /**< DE__TM_LAYER_* */
    bool visible;       /**< shown under DeTilemap.layer_mask */
} de__tilemap_layer_info;

/** One tile of the palette. */
typedef struct de__tilemap_tile_info {
    de_vec4 uv_rect;    /**< (u, v, du, dv) in the tileset image, v down: DeSprite.uv_rect */
    de_asset_id texture; /**< the tileset texture asset ({0}: none) */
    de_texture texture_handle; /**< typed handle (invalid when no texture type / not declared) */
    de_vec2 size;       /**< sprite size in map units */
    u32 tileset;        /**< index into the tilesets */
    bool animated, solid;
    u32 shape_count;    /**< collision shapes of the tile */
} de__tilemap_tile_info;

/** One tileset (a contiguous range of tile indices). */
typedef struct de__tilemap_tileset_info {
    const char *name;
    de_asset_id texture;
    u32 first_tile, tile_count, columns, image_w, image_h;
} de__tilemap_tileset_info;

/** Layers of the live map of `e` (0 when `e` has no built map yet: call de_tilemap_flush). */
u32 de__tilemap_layer_count(de_world *w, de_entity e);
bool de__tilemap_layer(de_world *w, de_entity e, u32 layer, de__tilemap_layer_info *out);
/** Tiles and tilesets of the live map of `e` (the palette). */
u32 de__tilemap_tile_count(de_world *w, de_entity e);
bool de__tilemap_tile(de_world *w, de_entity e, u32 tile_index, de__tilemap_tile_info *out);
u32 de__tilemap_tileset_count(de_world *w, de_entity e);
bool de__tilemap_tileset(de_world *w, de_entity e, u32 tileset,
                              de__tilemap_tileset_info *out);

/**
 * World point -> cell of `layer`: the point is taken into the entity's space (its global
 * transform, else its local one), projected on the 2D plane (the depth axis is ignored), and
 * mapped to the layer's grid. False when the map is not built or the point is outside the layer
 * (`*x`, `*y` are still written then, so a tool can extend the hit test).
 */
bool de__tilemap_pick(de_world *w, de_entity e, de_dvec3 world_point, u32 layer, i32 *x, i32 *y);
/** Like de__tilemap_pick over every visible layer, topmost first: the first layer whose cell at
 * the point holds a tile; else the topmost layer that contains the point. */
bool de__tilemap_pick_top(de_world *w, de_entity e, de_dvec3 world_point, u32 *layer, i32 *x,
                          i32 *y);
/** The world positions of a cell's corners (bottom-left, bottom-right, top-right, top-left), for
 * a highlight. False when the map is not built or the layer is out of range. */
bool de__tilemap_cell_corners(de_world *w, de_entity e, u32 layer, i32 x, i32 y,
                              de_dvec3 corners[4]);

/* Live edits: de_tilemap_set_tile / de_tilemap_get_cell (de_gameplay.h); de_tilemap_flush
 * applies them at once outside the frame loop. */

/* ======================== Runtime internals ======================== */

/** The loaded object of a `tilemap` asset: the view of a copy of the cooked bytes. */
typedef struct de__tm_asset {
    const de_allocator *alloc;
    size_t bytes; /* the whole block: this header + the copy */
    de__tm_view view;
} de__tm_asset;

/** The live state of one chunk of one layer of a map instance. */
typedef struct de__tm_chunk {
    u32 layer, cx, cy;
    u32 *cells;          /* DE__TM_CHUNK_CELLS live cell values */
    de_entity root;      /* chunk entity (child of the map entity) */
    de_entity *sprites;  /* DE__TM_CHUNK_CELLS, or NULL before the first build with tiles */
    u16 *animated;       /* cell indices with animated tiles; DE__TM_CHUNK_CELLS capacity */
    u32 animated_count;
    u32 *anim_frame;     /* current frame per animated entry */
    de_entity *colliders;
    u32 collider_count, collider_cap;
    de_shape *shapes;    /* chain shapes this chunk created (destroyed with its colliders) */
    u32 shape_count, shape_cap;
    bool dirty_mesh, dirty_colliders;
    u32 mesh_builds, collider_builds; /* counters, for tests */
} de__tm_chunk;

typedef struct de__tm_instance {
    de_entity entity;
    DeTilemap comp; /* as last seen */
    const de__tm_asset *asset;
    de__tm_chunk *chunks; /* view.h.chunk_count */
    de_texture *textures; /* per tileset */
    de_entity *spawned;
    u32 spawned_count;
    /* Counts of the arrays above, kept here: the asset may be gone when the instance is freed
     * (the engine destroys the asset system before the world). */
    u32 chunk_count, tileset_count, spawn_count;
    f64 anim_ms;
    bool built, spawned_done, seen;
} de__tm_instance;

typedef struct de__tilemap_sys {
    const de_allocator *alloc;
    de_world *w;
    de_asset_system *assets;
    de_tilemap_desc desc;
    de_component comp, transform, global, sprite, collider;
    de_type_id ref_type;
    de_resource_id resource;
    de_system update;
    de_log_category log;
    de__tm_instance *inst;
    u32 inst_count, inst_cap;
    de_entity *scratch; /* entities collected from the query */
    u32 scratch_cap;
    de_physics2d_world *p2d; /* looked up at build time; NULL without 2D physics */
} de__tilemap_sys;

/** The resource of an attached world. */
typedef struct DeTilemapSystemRef {
    de__tilemap_sys *ptr;
} DeTilemapSystemRef;

de__tilemap_sys *de__tilemap_sys_of(de_world *w);
de__tm_instance *de__tilemap_instance(de__tilemap_sys *s, de_entity e);
/** Creates the instance of `e` when its map is READY (no building). */
de__tm_instance *de__tilemap_instance_ensure(de__tilemap_sys *s, de_entity e);
/** The asset object of a tilemap handle when READY, else NULL. */
const de__tm_asset *de__tilemap_asset(de_asset_system *s, de_tilemap m);
/** The `tilemap.update` work (`dt` seconds of animation). */
void de__tilemap_update(de__tilemap_sys *s, f32 dt);

/* Plane mapping: map (u, v) + depth -> entity-local position. */
de_dvec3 de__tm_local(const de__tilemap_sys *s, f64 u, f64 v, f64 depth);
f32 de__tm_layer_depth(const de__tilemap_sys *s, u32 layer);
bool de__tm_layer_visible(const DeTilemap *c, u32 layer);
/** The entity's world transform (global if present, else local, else identity). */
de_transform de__tm_entity_xform(de__tilemap_sys *s, de_entity e);

/* tilemap_chunks.c */
void de__tm_chunk_build_mesh(de__tilemap_sys *s, de__tm_instance *in, de__tm_chunk *c);
void de__tm_animate(de__tilemap_sys *s, de__tm_instance *in);
/** The (rotation quarter-turns, flip_x) that shows a tile with the DE_TILEMAP_FLIP_* bits of
 * `cell` (exposed for tests). */
void de__tm_flip_to_rotation(u32 cell, u32 *quarter_turns, bool *flip_x);
/** The tile index (0-based) a cell shows at animation time `ms`. */
u32 de__tm_anim_tile(const de__tm_view *v, u32 tile, f64 ms);

/* tilemap_collision.c */
/** One merged rectangle of solid cells, in chunk cell coordinates. */
typedef struct de__tm_rect {
    u32 x, y, w, h;
} de__tm_rect;
/** Greedy merge of `solid` (DE__TM_CHUNK x DE__TM_CHUNK, row-major) into rectangles; returns
 * their count (at most DE__TM_CHUNK_CELLS). Rows first: each rectangle is as wide as it can be,
 * then as tall as its full width allows. */
u32 de__tm_merge_rects(const u8 *solid, de__tm_rect *out);
/** True when cell value `cell` of layer `l` collides as a whole cell. */
bool de__tm_cell_solid(const de__tm_view *v, const de__tm_layer *l, u32 cell);
void de__tm_chunk_build_colliders(de__tilemap_sys *s, de__tm_instance *in, de__tm_chunk *c);
void de__tm_chunk_clear_colliders(de__tilemap_sys *s, de__tm_chunk *c, bool world_alive);

/* tilemap_spawn.c */
void de__tm_spawn_entities(de__tilemap_sys *s, de__tm_instance *in);
/** Sets one field record on entity `e` (component found by the path's first segment; added
 * when missing). False (with a warning) when the path or the value does not fit. */
bool de__tm_apply_field(de__tilemap_sys *s, de_entity e, const de__tm_view *v,
                        const de__tm_field *f);

DE_EXTERN_C_END

#endif /* DE_GAMEPLAY_TILEMAP_INTERNAL_H */
