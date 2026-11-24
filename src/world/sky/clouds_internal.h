/*
 * clouds_internal.h: raymarched volumetric clouds (DENG-208, S34; SPEC §12.13). Module sky.
 *
 * One layer between DeSky.cloud_bottom_m and cloud_top_m above sea level (default 1500 m to
 * 4000 m), a spherical shell around the planet of the physical sky (SKY_BOTTOM).
 *
 * The shape:
 *   weather map  512 x 512 RGBA8, procedural: r coverage, g cloud type (0 stratus .. 1 cumulus),
 *                b precipitation (darker bases). It tiles every DE__CLOUDS_WEATHER_TILE_M.
 *   base noise   128^3 Perlin-Worley (r) plus three Worley octaves (gba), tiling.
 *   detail noise 32^3 Worley octaves (rgb), tiling; it erodes the edges (DeSky.cloud_erosion).
 * Bindless has 2D textures only, so both noises are atlases of slices with a one-texel wrapped
 * gutter (bilinear filtering inside a tile wraps): DE__CLOUDS_*_TILE texels a slice. The noises
 * and the weather map are generated once on the GPU (clouds_noise_gen.slang) and kept.
 *
 * Large worlds: every tiling coordinate is the camera's double world position plus the wind's
 * drift, reduced modulo its tile in double precision (de__clouds_tile_offset), plus the f32
 * camera-relative offset of the sample. Precision does not degrade away from the origin.
 *
 * The passes (only when de__clouds_enabled):
 *   clouds.shadow       DE__VIEW_SLOT_LIGHTING, order DE__CLOUDS_SHADOW_STAGE_ORDER: the
 *                       transmittance toward the sun from the layer bottom plane, top-down,
 *                       DE__CLOUDS_SHADOW_N^2 over DE__CLOUDS_SHADOW_EXTENT_M around the camera,
 *                       plus (Medium and above, DENG-854) a fine cascade below it over
 *                       DE__CLOUDS_SHADOW_FINE_EXTENT_M, marched with the detail noise at High.
 *                       The sun lighting (lighting_common.slang) and the fog's light injection
 *                       sample it through LightingInputs.cloud_shadow (de_input_cloud_shadow).
 *   clouds.march        after sky.render: half width x half height (a quarter of the pixels),
 *                       one of the four pixels of each 2 x 2 block per frame (Bayer order),
 *                       blue-noise start jitter (DENG-852), up to
 *                       128 primary steps and 6 light steps, Beer-Powder, a dual-lobe
 *                       Henyey-Greenstein phase,
 *                       multi-scattering octaves, the sky's SH ambient. Out: premultiplied
 *                       in-scatter (rgb) and transmittance (a), and the in-scatter-weighted
 *                       distance.
 *   clouds.reconstruct  full resolution: the fresh pixel where this frame marched, else last
 *                       frame's result reprojected at the cloud distance, clamped to the 3 x 3
 *                       neighbourhood of this frame's march (no ghosting). Two histories alternate.
 *   clouds.composite    into the HDR target, behind opaque surfaces and in front of the sky (the
 *                       sun disk is occluded by the transmittance): L' = L T_c + S_c T_ap + S_ap
 *                       (1 - T_c), the aerial perspective at the cloud distance. With the froxel
 *                       fog on, fog.apply applies the aerial perspective to opaque pixels later.
 */
#ifndef DE_WORLD_SKY_CLOUDS_INTERNAL_H
#define DE_WORLD_SKY_CLOUDS_INTERNAL_H

#include <dengine/de_base.h>
#include <dengine/de_math.h>
#include <dengine/de_render.h>

#if DE_HAS_RENDER
#include <dengine/de_gpu.h>
#include <dengine/de_memory.h>
#endif

DE_EXTERN_C_BEGIN

/* ---------- Constants (clouds_common.slang mirrors them) ---------- */
#define DE__CLOUDS_BOTTOM_M      1500.0f
#define DE__CLOUDS_TOP_M         4000.0f
#define DE__CLOUDS_MIN_DEPTH_M   100.0f
#define DE__CLOUDS_EROSION       0.5f
#define DE__CLOUDS_WEATHER_N     512u
#define DE__CLOUDS_WEATHER_TILE_M 40960.0
#define DE__CLOUDS_BASE_N        128u
#define DE__CLOUDS_BASE_TILE     130u /* with the wrapped gutter */
#define DE__CLOUDS_BASE_COLUMNS  16u  /* atlas 2080 x 1040 */
#define DE__CLOUDS_BASE_TILE_M   6144.0
#define DE__CLOUDS_DETAIL_N      32u
#define DE__CLOUDS_DETAIL_TILE   34u
#define DE__CLOUDS_DETAIL_COLUMNS 8u  /* atlas 272 x 136 */
#define DE__CLOUDS_DETAIL_TILE_M 384.0
/** The drift wraps at a common multiple of the three tiles (seamless): 122880 m x 8192. */
#define DE__CLOUDS_DRIFT_PERIOD_M (122880.0 * 8192.0)
/** The blue-noise tile (DENG-852): N x N ranks, tiling, generated once on the CPU. */
#define DE__CLOUDS_BLUE_N        64u
#define DE__CLOUDS_SHADOW_N      256u
#define DE__CLOUDS_SHADOW_EXTENT_M 16384.0f
/** DENG-854: the fine cascade, N x N texels over this extent (8 m texels) around the camera. The
 * map is DE__CLOUDS_SHADOW_N wide and 2 N high: the coarse cascade in the top half (rows 0..N-1),
 * the fine one below it. One texture and one binding, so de_input_cloud_shadow keeps its inputs. */
#define DE__CLOUDS_SHADOW_FINE_EXTENT_M 2048.0f
/** The fraction of a cascade's width, at each edge, over which it fades out (to the coarser one,
 * or to no shadow for the coarse cascade). Mirrored in lighting_inputs.slang. */
#define DE__CLOUDS_SHADOW_FADE 0.1f
/** The most the fine cascade's centre moves from the camera: it stays inside the coarse cascade. */
#define DE__CLOUDS_SHADOW_SHIFT_MAX_M ((DE__CLOUDS_SHADOW_EXTENT_M - DE__CLOUDS_SHADOW_FINE_EXTENT_M) * 0.5f)
/** The clouds.shadow stage's order in DE__VIEW_SLOT_LIGHTING: after "sky.ibl", before "lighting". */
#define DE__CLOUDS_SHADOW_STAGE_ORDER (-45)
/** The history's weight in the reconstruction (the fresh quarter keeps 1 - weight). */
#define DE__CLOUDS_HISTORY_WEIGHT 0.9f

/** The resolved cloud layer of a DeSky (defaults applied, clamped). */
typedef struct de__clouds_settings {
    bool on;          /**< DeSky.clouds and coverage > 0 */
    f32 coverage;     /**< 0..1 */
    f32 bottom_m, top_m;
    f32 density;      /**< extinction scale */
    f32 erosion;      /**< 0..1 */
    f32 wind_x, wind_y; /**< the drift velocity, m/s (x east, y north) */
} de__clouds_settings;

/** The clouds of `sky`: zero fields take the defaults (bottom 1500 m, top 4000 m, density 1,
 * erosion 0.5), coverage and erosion clamp to 0..1, the top is at least bottom + 100 m. Pure. */
de__clouds_settings de__clouds_resolve(const DeSky *sky);
/** Whether the clouds render: on, and `rs` (NULL: no) has volumetrics other than OFF. Pure. */
bool de__clouds_enabled(const de__clouds_settings *c, const de_render_settings *rs);

/** One volumetrics quality's march. */
typedef struct de__clouds_quality {
    u32 primary_steps; /**< at most 128 */
    u32 light_steps;   /**< at most 6 */
    u32 octaves;       /**< multi-scattering octaves */
    u32 shadow_steps;  /**< the shadow map's steps toward the sun */
    bool shadow_fine;  /**< DENG-854: the fine shadow cascade (Medium and above) */
    bool shadow_detail; /**< DENG-854: the fine cascade's march adds the detail noise (High and above) */
} de__clouds_quality;
/** Low 48/4/2, Medium 64/5/2, High 96/6/3, Ultra 128/6/3; OFF (or anything else): zero. Pure. */
de__clouds_quality de__clouds_quality_for(de_quality q);

/** DENG-854: how much of the finer cascade replaces the coarser one at (`u`, `v`), the point's
 * position in the cascade's area (0..1 across it): 0 outside the area, 1 further than
 * DE__CLOUDS_SHADOW_FADE from every edge, linear in between. The coarse cascade's own fade to
 * "no shadow" at its border uses the same function. Pure; lighting_inputs.slang does the same. */
f32 de__clouds_shadow_weight(f32 u, f32 v);
/** The cascades' blend at a point: `coarse` and `fine` are the two cascades' shadow values and `w`
 * is de__clouds_shadow_weight of the point in the fine cascade (0 without the fine cascade). */
f32 de__clouds_shadow_blend(f32 coarse, f32 fine, f32 w);
/** A cascade's centre offset from the camera along one axis: the point `camera_world` + `shift`,
 * snapped down to a texel of `extent / n` in world space, so the map does not shimmer when the
 * camera moves. `shift` is where the sun's rays through the camera cross the layer bottom (the map
 * is indexed by that crossing, not by the receiver), 0 for none. Pure. */
f32 de__clouds_shadow_snap(f64 camera_world, f64 shift, f64 extent, u32 n);
/** `shift` for one axis: the sun's horizontal travel from height `from_z` up to the layer bottom
 * `bottom_z` along `sun` (toward the sun), clamped to +-DE__CLOUDS_SHADOW_SHIFT_MAX_M so the fine
 * cascade stays inside the coarse one (low sun: beyond it the map has no shadow, as before);
 * 0 when the sun is within 0.02 of the horizon. Pure. */
f64 de__clouds_shadow_shift(f64 sun_axis, f64 sun_z, f64 from_z, f64 bottom_z);
/** DENG-867: de__clouds_shadow_shift without the clamp: the sun's whole horizontal travel from
 * `from_z` up to `bottom_z` (0 from at or above the bottom, or with the sun within 0.02 of the
 * horizon). Pure. */
f64 de__clouds_shadow_travel(f64 sun_axis, f64 sun_z, f64 from_z, f64 bottom_z);

/** DENG-867: the half-width of the fine cascade's fully weighted part (inside its fade). */
#define DE__CLOUDS_SHADOW_FINE_INNER_M (DE__CLOUDS_SHADOW_FINE_EXTENT_M * (0.5f - DE__CLOUDS_SHADOW_FADE))

/** DENG-867: the two cascades' centre shifts from the camera along one axis (before the snap).
 * The fine cascade starts at the crossing of the ray through the camera (`camera_z`), as in
 * DENG-854. With a target (`has_target`: the crossing of the receivers the camera sees, from the
 * depth buffer a few frames ago, relative to the camera), it moves the least distance that brings
 * the target inside its fully weighted part (DE__CLOUDS_SHADOW_FINE_INNER_M): a camera high above
 * the ground then still has the fine cascade on the ground it looks at. Neither is clamped: when
 * the fine centre is more than DE__CLOUDS_SHADOW_SHIFT_MAX_M from the camera (a low sun), the
 * coarse cascade moves by the excess, so the fine cascade stays inside it and the receivers near
 * the camera keep a shadow. Both are the DENG-854 values while the fine cascade fits and covers
 * the target. Pure. */
void de__clouds_shadow_centres(f64 sun_axis, f64 sun_z, f64 camera_z, f64 bottom_z,
                               bool has_target, f64 target, f64 *fine, f64 *coarse);

/** DENG-867: the samples of the depth buffer that clouds.ground reads (an 8 x 8 grid). */
#define DE__CLOUDS_GROUND_SAMPLES 64u

/** DENG-867: where the receivers the camera sees cross the layer bottom toward the sun. `s` holds
 * `n` float4s from clouds.ground: each the receiver's position relative to the camera (x, y, z)
 * and w 1 for a hit (0: sky); the camera was at world height `camera_z`. The result, relative to
 * that camera (x, y), is the per-axis median crossing of the nearer half of the hits (the
 * receivers that the fine cascade is for). False with no hit or with the sun within 0.02 of the
 * horizon. Pure. */
bool de__clouds_receiver_crossing(const f32 *s, u32 n, f64 camera_z, de_vec3 sun, f64 bottom_z,
                                  f64 *x, f64 *y);

/** (`world` + `drift`) modulo `tile` in double precision, in [0, tile), as f32. Pure. */
f32 de__clouds_tile_offset(f64 world, f64 drift, f64 tile);

/** DENG-852: a tiling blue-noise tile by void-and-cluster (Ulichney 1993), DE__CLOUDS_BLUE_N^2
 * bytes, row-major: the rank of each texel scaled to 0..255 (every value 16 times). Deterministic.
 * Pure; about 10 ms. */
void de__clouds_blue_noise(u8 *out);

#if DE_HAS_RENDER

struct de__lighting_context;

/** One view's cloud constants (clouds_common.slang CloudsHeader; 304 bytes). */
typedef struct de__clouds_header {
    u32 tex[4];       /**< base atlas, detail atlas, weather map, linear wrap sampler */
    u32 tex2[4];      /**< transmittance LUT, linear clamp sampler, frame (jitter index), blue-noise tile (0: none) */
    u32 steps[4];     /**< primary, light, octaves, shadow steps */
    f32 layer[4];     /**< planet radius (m), bottom radius, top radius, camera altitude (m) */
    f32 shape[4];     /**< coverage, density, erosion, history weight (0: none) */
    f32 off_wb[4];    /**< tile offsets (m): weather xy, base xy */
    f32 off_d[4];     /**< detail xy, 1 / weather tile, 1 / base tile */
    f32 misc[4];      /**< 1 / detail tile, pre-exposure, camera z (render-origin), 0 */
    f32 sun[4];       /**< direction toward the sun; w: illuminance at the top (pre-exposed) */
    f32 ray00[4];     /**< camera ray (world rotation) at uv (0, 0); w: 1 */
    f32 ray_du[4];    /**< its change across the width */
    f32 ray_dv[4];    /**< its change down the height */
    f32 vray[4];      /**< view-space ray at z = -1: top-left (x, y), change (dx, dy) */
    f32 proj[4];      /**< projection m[10], m[14], m[11], m[15] (linear depth) */
    f32 camera[4];    /**< camera position (render-origin space) */
    f32 prev_vp[4][4]; /**< last frame's view-projection (rows, render-origin space) */
} de__clouds_header;
DE_STATIC_ASSERT(sizeof(de__clouds_header) == 304, "CloudsHeader is 304 bytes");

/** The cloud shadow's header (lighting_inputs.slang de_input_cloud_shadow; 64 bytes). */
typedef struct de__cloud_shadow_gpu {
    u32 tex[4];   /**< shadow map (sampled), linear clamp sampler, flags (1: on, 2: fine cascade), 0 */
    f32 area[4];  /**< centre x, y (render-origin space), 1 / extent, layer bottom z */
    f32 sun[4];   /**< direction toward the sun, strength (0..1) */
    f32 fine[4];  /**< DENG-854: the fine cascade's centre x, y, 1 / extent, 0 (flags bit 1: on) */
} de__cloud_shadow_gpu;
DE_STATIC_ASSERT(sizeof(de__cloud_shadow_gpu) == 64, "CloudShadow is 64 bytes");

typedef struct de__clouds de__clouds;

/** Loads the pipelines (invalid where the backend has no code: de__clouds_ready false). The noise
 * textures are made on the first frame that draws clouds. `alloc` NULL: the heap. */
de_result de__clouds_create(de_gpu_device *dev, const de_allocator *alloc, de__clouds **out);
/** Frees the textures and buffers; NULL is ignored. The caller waited for the GPU. */
void de__clouds_destroy(de__clouds *c);
/** Every pipeline loaded. */
bool de__clouds_ready(const de__clouds *c);

/** What both cloud stages of one view read. */
typedef struct de__clouds_frame {
    const de_render_settings *settings;
    de__clouds_settings clouds;
    f64 camera_world[3]; /**< the camera's world position (double; z above sea level) */
    f64 drift[2];        /**< the wind's accumulated drift (m) */
    de_vec3 camera;      /**< the camera in render-origin space */
    de_vec3 sun_dir;     /**< toward the sun */
    f32 sun_illuminance; /**< at the top of the atmosphere, lux */
    u32 slot;            /**< the view slot (< 32) */
    u64 frame_index;
    u32 transmittance;   /**< the sky's transmittance LUT (bindless) */
    u32 sampler;         /**< linear clamp (bindless) */
} de__clouds_frame;

/** The shadow map of one frame (for de__lighting_inputs). */
typedef struct de__clouds_shadow_output {
    de_rg_resource texture; /**< invalid: none */
    u32 header, entry;
} de__clouds_shadow_output;

/**
 * Adds clouds.shadow for one view, or nothing when de__clouds_enabled is false. The noise is
 * generated first when it does not exist (one submission, outside the graph). No heap allocation
 * after the view's first cloud frame. Returns the passes added.
 */
u32 de__clouds_add_shadow_pass(de__clouds *c, de_rg *g, de_arena *arena, const de__clouds_frame *f,
                               de__clouds_shadow_output *out);

/** What one view's march, reconstruction and composite read. */
typedef struct de__clouds_inputs {
    de__clouds_frame frame;
    u32 width, height; /**< the view's internal size */
    de_mat4 view_matrix, proj, prev_view_proj; /**< render-origin space */
    de_vec3 ray00, ray_du, ray_dv; /**< camera rays with the view's rotation only (sky.render's) */
    bool history_ok;   /**< false: the history is dropped (origin rebase, probe capture) */
    f32 pre_exposure;
    de_rg_resource depth;  /**< reverse-Z depth */
    de_rg_resource target; /**< the HDR lighting target (read and written by the composite) */
    const struct de__lighting_context *lc; /**< NULL: no SH ambient */
    de_rg_resource aerial; /**< the sky's aerial perspective atlas (invalid: none) */
    u32 aerial_sampler;
    f32 aerial_range_km;
    bool aerial_on_opaque; /**< false: a later pass (fog.apply) applies it to opaque pixels */
} de__clouds_inputs;

/**
 * Adds clouds.march, clouds.reconstruct and clouds.composite, or nothing when de__clouds_enabled
 * is false or the projection is orthographic. Pass data goes on `arena`; no heap allocation after
 * the view's first cloud frame (and a size change). Returns the passes added; on a resource or
 * arena failure, ++g->error_count and fewer passes.
 */
u32 de__clouds_add_passes(de__clouds *c, de_rg *g, de_arena *arena, const de__clouds_inputs *in);

#endif /* DE_HAS_RENDER */

DE_EXTERN_C_END

#endif /* DE_WORLD_SKY_CLOUDS_INTERNAL_H */
