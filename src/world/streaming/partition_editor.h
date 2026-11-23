/*
 * partition_editor.h (DENG-831): what the editor service (src/host/editor/editor_stream.c, DIDE
 * world.streaming, protocol 1.26) needs from a world partition beyond the public API: per-cell
 * forces, editor sources, per-cell detail (bytes, entities, bounds, error), the sources of the
 * last update with their kind, and a dry-run preview. Implemented in world_partition.c.
 * Internal: include as "world/streaming/partition_editor.h".
 */
#ifndef DE_STREAMING_PARTITION_EDITOR_H
#define DE_STREAMING_PARTITION_EDITOR_H

#include <dengine/de_asset.h>
#include <dengine/de_ecs.h>

DE_EXTERN_C_BEGIN

/** Editor sources a partition keeps (they join the game and entity sources). */
#define DE__WP_MAX_EDITOR_SOURCES 8u

typedef enum de__wp_force {
    DE__WP_FORCE_NONE = 0,
    DE__WP_FORCE_LOAD = 1,   /* loads whatever the sources, and stays */
    DE__WP_FORCE_UNLOAD = 2, /* never loads; unloads at the next update */
} de__wp_force;

typedef enum de__wp_source_kind {
    DE__WP_SOURCE_GAME = 0,   /* de_asset_set_streaming_sources */
    DE__WP_SOURCE_ENTITY = 1, /* an entity with DeStreamingSource */
    DE__WP_SOURCE_EDITOR = 2, /* de__wp_set_editor_sources */
} de__wp_source_kind;

typedef struct de__wp_source {
    de_streaming_source s; /* radius resolved: never 0 */
    de__wp_source_kind kind;
    de_entity entity; /* kind ENTITY */
} de__wp_source;

typedef struct de__wp_cell_info {
    i32 cx, cy;
    de_world_cell_state state;
    de__wp_force force;
    u64 bytes;      /* resident bytes of the cell's bundle while loaded; 0 otherwise */
    u32 entities;   /* entities under the cell root at its last load (0 before one) */
    f64 zmin, zmax; /* content extent along z at its last load (0, 0 before one) */
    bool proxy_visible;
    f32 proxy_alpha;
    const char *error; /* state FAILED: why; NULL otherwise */
} de__wp_cell_info;

typedef struct de__wp_preview_cell {
    i32 cx, cy;
    f64 distance; /* m from the nearest source to the cell square (0 without sources) */
} de__wp_preview_cell;

/** What an update would do with only `sources`; arrays hold cell_count entries each. */
typedef struct de__wp_preview {
    de__wp_preview_cell *loads; /* nearest first, then cx, cy; at most max_resident - keep */
    u32 load_count;
    de__wp_preview_cell *keep;
    u32 keep_count;
    de__wp_preview_cell *unloads;
    u32 unload_count;
    bool capped;
} de__wp_preview;

/** Registers DeStreamingSource, DeWorldCell and DeWorldCellPin in `w` (if missing), as
 * de_world_partition_open does, so the editor can offer them before a partition is open. Any
 * out pointer may be NULL. */
de_result de__wp_register_components(de_world *w, de_component *src, de_component *cell,
                                     de_component *pin);

/** Frees a partition whose ECS world is gone: releases its cell requests and touches nothing in
 * the world (its cells and query went with it). NULL is a no-op. */
void de__wp_abandon(de_world_partition *p);

u32 de__wp_cell_count(const de_world_partition *p);
/** Cell `i` (cells sorted by cx, then cy). False when `i` is out of range. */
bool de__wp_cell_info_at(const de_world_partition *p, u32 i, de__wp_cell_info *out);
/** Index of cell (cx, cy); false when the world does not have it. */
bool de__wp_cell_index(const de_world_partition *p, i32 cx, i32 cy, u32 *out);
void de__wp_set_force(de_world_partition *p, u32 i, de__wp_force f);
void de__wp_clear_forces(de_world_partition *p);
u32 de__wp_forced_loads(const de_world_partition *p);
/** Replaces the editor sources (at most DE__WP_MAX_EDITOR_SOURCES are kept). */
void de__wp_set_editor_sources(de_world_partition *p, const de_streaming_source *s, u32 n);
/** The sources of the last update. */
u32 de__wp_sources(const de_world_partition *p, const de__wp_source **out);
u32 de__wp_max_resident(const de_world_partition *p);
u32 de__wp_max_loads(const de_world_partition *p);
f32 de__wp_load_radius(const de_world_partition *p);
f32 de__wp_unload_radius(const de_world_partition *p);
/** Failed cells and resident bytes (the stats the public struct does not have). */
void de__wp_extra_stats(const de_world_partition *p, u32 *cells_failed, u64 *bytes_resident);
/** Distance from the nearest source of the last update to cell `i`; false without sources. */
bool de__wp_cell_distance(const de_world_partition *p, u32 i, f64 *out);
/** Dry run of steps 3 and 4 of the update from the current states, as if `src` (radius 0: the
 * world's) were the only sources. Loads nothing. `out`'s arrays hold cell_count entries each. */
void de__wp_preview_run(const de_world_partition *p, const de_streaming_source *src, u32 n,
                        bool ignore_forces, de__wp_preview *out);

DE_EXTERN_C_END

#endif
