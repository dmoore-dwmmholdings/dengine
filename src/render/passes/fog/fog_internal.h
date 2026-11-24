/*
 * fog_internal.h: froxel volumetric fog (DENG-207, S34; SPEC §12.13). Module render (moved from sky by DENG-861).
 *
 * A per-view froxel grid (de__fog_grid_for: 160 x 90 x 64 at High) whose depth slices are
 * exponential from DE__FOG_NEAR to r.fog.max_distance (default 128 m). Four passes, after the
 * sky stage's sky.render:
 *   fog.media      the media of every DeFogVolume (fog_volumes.c packs them camera-relative):
 *                  scattering rgb and extinction, and the phase anisotropy.
 *   fog.light      in-scattered light: the sun through the CSM (its colour already carries the
 *                  sky's transmittance), the clustered point and spot lights with atlas shadows,
 *                  the sky's SH ambient; Henyey-Greenstein phase. Jittered depth inside each
 *                  slice, blended with last frame's reprojection (weight DE__FOG_HISTORY_WEIGHT),
 *                  history rejected off-screen and where the medium changed (disocclusion).
 *   fog.integrate  front to back along each column: in-scatter (rgb) and transmittance (a) from
 *                  the camera to the end of every slice.
 *   fog.apply      the opaque surfaces and the sky in the HDR target, before transparency, with
 *                  the aerial perspective behind the fog: L' = S_fog + T_fog (S_ap + T_ap L).
 * The forward transparent pass applies the same volume (froxel_common.slang froxel_apply) through
 * the lighting context (de__lighting_context.fog, fog_header).
 *
 * Volumes are 2D atlases (bindless has 2D textures only): slice k is the tile at column
 * k % DE__FOG_ATLAS_COLUMNS, row k / DE__FOG_ATLAS_COLUMNS.
 *
 * Global fog is a DeFogVolume with a zero extent: density exp(-height_falloff (z - entity z)).
 */
#ifndef DE_RENDER_PASSES_FOG_INTERNAL_H
#define DE_RENDER_PASSES_FOG_INTERNAL_H

#include <dengine/de_base.h>
#include <dengine/de_gpu.h>
#include <dengine/de_math.h>
#include <dengine/de_memory.h>
#include <dengine/de_render.h>

#if DE_HAS_RENDER

DE_EXTERN_C_BEGIN

struct de__fog_snapshot;
struct de__lighting_context;

/** The volumes packed per frame at most (render_world.h keeps 32). */
#define DE__FOG_MAX_VOLUMES 32u
/** Tiles per atlas row. */
#define DE__FOG_ATLAS_COLUMNS 8u
/** Where slice 0 starts (m); nearer media count as slice 0's. */
#define DE__FOG_NEAR 0.25f
/** Default of r.fog.max_distance (m). */
#define DE__FOG_MAX_DISTANCE 128.0f
/** The history's weight in the temporal blend. */
#define DE__FOG_HISTORY_WEIGHT 0.9f

/** A froxel grid; all zero when volumetrics are off. */
typedef struct de__fog_grid {
    u32 w, h, d;
} de__fog_grid;

/** The grid for a volumetrics quality: Low 80x45x32, Medium 120x68x48, High 160x90x64,
 * Ultra 160x90x128; OFF (or anything else): zero. */
de__fog_grid de__fog_grid_for(de_quality q);
/** The atlas size of grid `g` (columns DE__FOG_ATLAS_COLUMNS, rows ceil(d / columns)). */
void de__fog_atlas_size(de__fog_grid g, u32 *w, u32 *h);
/** The view depth at slice coordinate `s` in [0, d]: near (far / near)^(s / d). */
f32 de__fog_slice_depth(f32 s, u32 d, f32 near_m, f32 far_m);
/** The inverse: the slice coordinate of view depth `z` (unclamped; < 0 nearer than `near_m`). */
f32 de__fog_slice_of(f32 z, u32 d, f32 near_m, f32 far_m);
/** Whether fog renders: volumetrics on, at least one volume with a density > 0, far > near. */
bool de__fog_enabled(const de_render_settings *s, const struct de__fog_snapshot *volumes, u32 n,
                     f32 max_distance);

/** One packed volume (froxel_common.slang FogVolume; 96 bytes). */
typedef struct de__fog_volume_gpu {
    f32 inv[3][4];          /**< rows: camera-relative position -> box-local position */
    f32 extent_density[4];  /**< half extents (x < 0: global fog), density (1/m) */
    f32 albedo_g[4];        /**< scattering albedo rgb, anisotropy g */
    f32 params[4];          /**< height falloff (1/m), noise (0..1), edge fade (m), base z
                                 (camera-relative; global fog) */
} de__fog_volume_gpu;
DE_STATIC_ASSERT(sizeof(de__fog_volume_gpu) == 96, "FogVolume is 96 bytes");

/**
 * Packs the volumes with a density > 0 relative to the camera at `camera` (render-origin space):
 * the inverse transform maps (p - camera) to box space. A zero extent packs as global fog.
 * Returns the number written (at most `cap`). No allocation.
 */
u32 de__fog_pack_volumes(const struct de__fog_snapshot *src, u32 n, de_vec3 camera,
                         de__fog_volume_gpu *out, u32 cap);

/** One view's fog constants (froxel_common.slang FogHeader; 224 bytes). */
typedef struct de__fog_header {
    u32 grid[4];      /**< w, h, d, atlas columns */
    u32 tex[4];       /**< integrated volume (sampled), linear clamp sampler, volumes buffer,
                           volume count */
    f32 depth[4];     /**< near, far (max distance), ln(far / near), slice jitter (0..1) */
    f32 ray00[4];     /**< camera-relative offset per unit view depth at uv (0, 0); w: history
                           weight (0: no history) */
    f32 ray_du[4];    /**< its change across the width; w: pre-exposure */
    f32 ray_dv[4];    /**< its change down the height; w: orthographic (0/1) */
    f32 org00[4];     /**< orthographic: camera-relative origin at uv (0, 0) (else 0); w: first
                           volume in the volumes buffer */
    f32 org_du[4];
    f32 org_dv[4];
    f32 camera[4];    /**< camera position (render-origin space) */
    f32 prev_vp[4][4]; /**< last frame's view-projection (rows, render-origin space) */
} de__fog_header;
DE_STATIC_ASSERT(sizeof(de__fog_header) == 224, "FogHeader is 224 bytes");

typedef struct de__fog de__fog;

/** Loads the pipelines (an invalid one where the backend has no code: de__fog_ready false) and
 * registers r.fog.max_distance. `alloc` NULL: the heap. */
de_result de__fog_create(de_gpu_device *dev, const de_allocator *alloc, de__fog **out);
/** Frees every view's volumes and buffers; NULL is ignored. The caller waited for the GPU. */
void de__fog_destroy(de__fog *f);
/** Every pipeline loaded. */
bool de__fog_ready(const de__fog *f);
/** r.fog.max_distance. */
f32 de__fog_max_distance(const de__fog *f);

/** What one view's fog reads. */
typedef struct de__fog_inputs {
    const de_render_settings *settings;
    const struct de__fog_snapshot *volumes;
    u32 volume_count;
    u32 slot;          /**< the view slot (< 32) */
    u64 frame_index;
    u32 width, height; /**< the view's internal size */
    de_mat4 view_matrix, proj, prev_view_proj; /**< render-origin space */
    bool history_ok;   /**< false: the history is dropped (origin rebase, probe capture) */
    f32 pre_exposure;
    de_rg_resource depth;  /**< reverse-Z depth */
    de_rg_resource target; /**< the HDR lighting target (read and written by fog.apply) */
    const struct de__lighting_context *lc; /**< NULL: no direct light (ambient only) */
    u32 ambient;           /**< 1: add the IBL's SH ambient (needs `lc`) */
    de_rg_resource aerial; /**< the sky's aerial perspective atlas (invalid: none) */
    u32 aerial_sampler;
    f32 aerial_range_km;
    u32 sampler; /**< linear clamp (bindless) */
} de__fog_inputs;

/** What the passes produced: for the forward transparent pass. */
typedef struct de__fog_output {
    de_rg_resource integrated; /**< invalid: no fog this frame */
    u32 header;                /**< the header ring (bindless; 0: none); entry frame % ring */
} de__fog_output;

/**
 * Adds fog.media, fog.light, fog.integrate and fog.apply to `g` for one view, or nothing when
 * de__fog_enabled is false (volumetrics OFF: no fog pass at all). Pass data goes on `arena`; no
 * heap allocation after a view's first fog frame (and a grid change). Returns the number of passes
 * added; on a resource or arena failure, ++g->error_count and fewer passes.
 */
u32 de__fog_add_passes(de__fog *f, de_rg *g, de_arena *arena, const de__fog_inputs *in,
                       de__fog_output *out);

DE_EXTERN_C_END

#endif /* DE_HAS_RENDER */
#endif /* DE_RENDER_PASSES_FOG_INTERNAL_H */
