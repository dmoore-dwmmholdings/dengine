/* terrain.h: the CPU terrain model behind the DIDE editor IPC world.terrain, protocol 1.11
 * (DENG-580). Include as "world/terrain/terrain.h". Internal to the engine.
 *
 * The model is the one of the DIDE mock engine (dide protocol/mock/src/terrain.rs), on the
 * DeTerrain component of de_render.h: a DE_TERRAIN_RES x DE_TERRAIN_RES grid of 16-bit heights
 * (0..65535 maps to 0..height metres) over size x size metres, four splat weights per sample
 * (sum 255), a hole flag per sample, four material layers and four auto-paint rules.
 *
 *   - Sample (x, y) is at world (origin.x + x * d, origin.z + y * d), d = size / (res - 1).
 *   - Slope is atan(|central-difference gradient|) in degrees; curvature is the 4-neighbour
 *     Laplacian in 1/m. Edges clamp.
 *   - A rule (enabled, layer below 4) applies where slope, height and curvature are each in its
 *     range (a range with max <= min is unbounded). Later rules win. A rule changes the shown
 *     weights at once; terrain.bake writes them into the splat map and turns the rules off.
 *   - A stroke is a list of dabs [x, y, z, pressure]. A dab's weight at a sample is
 *     clamp(strength) * falloff(distance / radius) * clamp(pressure). The falloff curve is
 *     sampled linearly over 0..1 (empty: 1 - t). Height tools move by weight * step, with step
 *     max(radius / 10, 0.1) m.
 *   - A carve (spline.carve, DENG-777) pulls the samples near a polyline to its height minus a
 *     depth, with a smoothstep falloff.
 *   - Tiles are DE_TERRAIN_TILE samples on a side (3 x 3 for the 33 grid). Undo data holds the
 *     changed tiles only. Heightmap import and export of one tile include its far edge, shared
 *     with the next tile.
 *
 * The module is DETERMINISTIC (SPEC section 5.7): all math is de_dmath, so a stroke gives the
 * same bytes on every platform. The GPU brushes, the streamed terrain and the runtime renderer
 * (DENG-265, DENG-266) take over the evaluation for large terrains; this model stays the
 * reference. */
#ifndef DE_WORLD_TERRAIN_H
#define DE_WORLD_TERRAIN_H

#include <dengine/de_render.h>
#include <dengine/de_base.h>

#include <stddef.h>

DE_EXTERN_C_BEGIN

#define DE_TERRAIN_COMPONENT "DeTerrain"
#define DE_TERRAIN_SAMPLES   (DE_TERRAIN_RES * DE_TERRAIN_RES)
/** Tiles on a side: ceil(res / tile). */
#define DE_TERRAIN_TILES     ((DE_TERRAIN_RES + DE_TERRAIN_TILE - 1u) / DE_TERRAIN_TILE)
#define DE_TERRAIN_TOOL_COUNT 12u
#define DE_TERRAIN_MAX_ITERATIONS 200u

/** The tool names terrain.info lists, in order. */
extern const char *const de__terrain_tools[DE_TERRAIN_TOOL_COUNT];

/** A terrain.stroke value, as the protocol has it (points: 4 floats per dab). */
typedef struct de_terrain_stroke {
    const char *tool;
    size_t tool_len;
    const f32 *points;
    u32 point_count;
    f32 radius, strength;
    const f32 *falloff;
    u32 falloff_count;
    bool has_target, has_step, has_seed, has_scale, has_rotation, has_iterations, has_layer;
    f32 target, step, scale, rotation;
    u32 seed, iterations, layer;
    bool area_tile; /**< erosion over the whole tile under each dab */
    bool erase;     /**< paint and hole: take away instead of add */
} de_terrain_stroke;

/** A heightmap: w x h samples, row-major; 0..1 (png16, raw16) or metres (exr). */
typedef struct de_terrain_grid {
    u32 w, h;
    const f32 *v;
} de_terrain_grid;

/** What terrain.sample answers for one sample. */
typedef struct de_terrain_sample {
    f32 height, slope;
    f32 weights[DE_TERRAIN_LAYERS]; /**< with the enabled rules */
    f32 baked[DE_TERRAIN_LAYERS];   /**< the splat map alone */
    bool hole;
} de_terrain_sample;

/** True when the component's grid is the one this model handles. */
bool de__terrain_valid(const DeTerrain *t);
f32 de__terrain_spacing(const DeTerrain *t);
/** The sample nearest world (wx, wz) on a terrain at origin (ox, oz); false off the grid. */
bool de__terrain_nearest(const DeTerrain *t, f64 ox, f64 oz, f64 wx, f64 wz, u32 *x, u32 *y);
bool de__terrain_sample_at(const DeTerrain *t, f64 ox, f64 oz, f64 wx, f64 wz,
                           de_terrain_sample *out);

/** Applies a stroke. `stamp` is the stamp heightmap (stamp tool only, else NULL). False with a
 *  message in `err` for a bad stroke; `t` may then be partly changed (work on a copy). */
bool de__terrain_stroke(DeTerrain *t, f64 ox, f64 oz, const de_terrain_stroke *s,
                        const de_terrain_grid *stamp, char *err, size_t err_cap);
/** Writes the enabled rules into the splat map and turns them off. */
void de__terrain_bake(DeTerrain *t);

/** spline.carve (DENG-777, DIDE protocol 1.12): moves the terrain under a world-space polyline
 *  (`n` points, xyz each) to the polyline's height minus `depth`. Samples within width / 2 of the
 *  polyline (horizontal distance, to its nearest point) move all the way; the next `falloff`
 *  metres blend back with smoothstep. The terrain is at world (ox, oy, oz). Only heights change.
 *  False (and `t` unchanged) for fewer than 2 points, width <= 0, falloff < 0 or a value that is
 *  not finite. The mock's terrain::carve, in the same order of operations. */
bool de__terrain_carve(DeTerrain *t, f64 ox, f64 oy, f64 oz, const f32 *path, u32 n, f32 width,
                       f32 falloff, f32 depth);

/** The samples of a heightmap import or export: the whole grid (tx < 0), or tile (tx, ty) with
 *  its shared far edge. */
void de__terrain_region(i32 tx, i32 ty, u32 *x0, u32 *y0, u32 *w, u32 *h);
/** Resamples `g` (bilinear) over the region; `metres` for exr, else 0..1. */
void de__terrain_import(DeTerrain *t, const de_terrain_grid *g, i32 tx, i32 ty, bool metres);
/** A region's heights in metres (row-major, w * h). */
void de__terrain_export_metres(const DeTerrain *t, i32 tx, i32 ty, f32 *out);

/** The sample indices of tile (tx, ty) without the shared edge, in row order; returns the count. */
u32 de__terrain_tile_indices(u32 tx, u32 ty, u16 *out);
/** True when tile (tx, ty) differs between a and b in heights, holes or splat. */
bool de__terrain_tile_changed(const DeTerrain *a, const DeTerrain *b, u32 tx, u32 ty);
bool de__terrain_rules_equal(const DeTerrain *a, const DeTerrain *b);

/** Smooth value noise in [-1, 1] (hash lattice, smoothstep). */
f32 de__terrain_noise(f32 x, f32 z, u32 seed);

DE_EXTERN_C_END

#endif
