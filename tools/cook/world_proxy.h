/*
 * world_proxy.h: the far proxy bake of `de-cook world` (DENG-830; DENG-271 follow-up).
 *
 * For each cell, world_cells.c lists the meshes its entities draw (DeMeshRenderer.mesh names a
 * `model#mesh` sub-asset of a glTF model asset) with the transform of each, relative to the cell
 * origin. The baker loads each model once (glTF or GLB, the model importer's front end, then its
 * axis conversion and `scale`) and merges the listed meshes into one triangle list. Triangles are
 * grouped by base colour (a DeMeshRenderer material override wins over the model's own; textures
 * are not sampled), at most 8 groups (past that, a triangle joins the nearest colour), and each
 * group is welded by position and simplified with meshoptimizer to `ratio` of its triangles (never
 * below 12, or all of them when there are fewer). The result is written as a small glTF: flat-
 * shaded triangles (POSITION and NORMAL, no UVs) in one mesh named "proxy", with one primitive and
 * one material per group, whose base colour is the group's area-weighted mean.
 *
 * Terrain (DENG-871): each terrain tile that overlaps the cell (de__wproxy_tile, the DENG-857 tile
 * data) adds a decimated heightfield: every `step`-th sample of the tile on each axis, its far
 * edge sample always kept, where `step` is the largest power of two up to 16 with
 * step * step * ratio <= 1 (so about `ratio` of the tile's triangles, never fewer). Each
 * decimated quad whose centre lies in the cell, and none of whose source samples is a hole, gives
 * two triangles. The samples follow the cooked tile convention (Z up): sample (i, j) is at
 * origin + (i * spacing, j * spacing, heights[] / 65535 * height). The terrain is not passed to
 * the simplifier; it is one more primitive after the mesh groups, with the area-weighted mean of
 * the tiles' colours. A bake without tiles is byte-identical to one before DENG-871.
 *
 * The output depends only on the input bytes: the mesh code is built with the deterministic float
 * flags (DE_DETERMINISTIC_FLAGS) and floats are written in their shortest round-trip form.
 */
#ifndef DE_COOK_WORLD_PROXY_H
#define DE_COOK_WORLD_PROXY_H

#include <dengine/de_base.h>
#include <dengine/de_memory.h>

#include "core/de_strbuf.h"

DE_EXTERN_C_BEGIN

/** One mesh drawn in a cell. */
typedef struct de__wproxy_item {
    const char *model_vpath; /**< the model's source as a normalised vpath */
    const char *mesh;        /**< the sub-asset name after '#', or "" for the model's only mesh */
    f64 scale;               /**< the model asset's `scale` setting (1 when not set) */
    f64 m[12];               /**< row-major 3x4 transform to the cell's frame (metres, Z up) */
    /** Bit i: submesh i takes colors[i] (the DeMeshRenderer material's base colour, linear)
     * instead of the model material's. */
    u32 color_mask;
    f32 colors[8][3];
    const char *file;        /**< for messages */
    int line;
} de__wproxy_item;

/** One terrain tile that overlaps a cell (DENG-871), in the cell's frame. */
typedef struct de__wproxy_tile {
    f64 origin[3];       /**< sample (0, 0) of the terrain grid, relative to the cell origin */
    f64 spacing, height; /**< metres between samples; metres at height 65535 */
    u32 res;             /**< samples per side of the grid (the row stride of the arrays) */
    u32 x0, y0, w, h;    /**< the tile's samples: columns x0..x0+w-1, rows y0..y0+h-1 */
    const u16 *heights;  /**< res * res samples, row-major (row j along +y, column i along +x) */
    const u8 *holes;     /**< res * res; non-zero: no surface. NULL: none */
    f64 cell[2];         /**< the cell's extent on x and y, from 0 (its origin) */
    f32 color[3];        /**< linear base colour */
} de__wproxy_tile;

/** Reports a problem with one item (it is left out of the proxy). */
typedef void (*de__wproxy_warn_fn)(void *user, const de__wproxy_item *item, const char *msg);

typedef struct de__wproxy de__wproxy;

/** A baker with an empty model cache; NULL when out of memory. */
de__wproxy *de__wproxy_create(const de_allocator *a, de__wproxy_warn_fn warn, void *user);
void de__wproxy_destroy(de__wproxy *b);

/**
 * Bakes `items` and the terrain `tiles` into the glTF text `out` (cleared first). `*tris` is the
 * proxy's triangle count; 0 (and `out` empty) when nothing gave geometry. DE_OK, or
 * DE_ERR_OUT_OF_MEMORY.
 */
de_result de__wproxy_bake(de__wproxy *b, const de__wproxy_item *items, u32 n,
                          const de__wproxy_tile *tiles, u32 tile_n, f32 ratio, de_strbuf *out,
                          u32 *tris);

/** The terrain sample step of `ratio` (1, 2, 4, 8 or 16). */
u32 de__wproxy_terrain_step(f32 ratio);

DE_EXTERN_C_END

#endif /* DE_COOK_WORLD_PROXY_H */
