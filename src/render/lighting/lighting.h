/*
 * lighting.h: clustered lighting and the deferred lighting pass (SPEC §12.5 step 5, §12.7;
 * DENG-303). Internal to de_render; include as "render/lighting/lighting.h".
 *
 *   lighting_pass.c  the stage: per renderer state (kernels, the LTC tables, cvars), per view
 *                    and frame the lighting constants, the cluster passes (clusters.c), the
 *                    deferred lighting over the material resolve's thin G-buffer, the debug
 *                    heatmap and the stats readback
 *   clusters.c       the froxel cluster bounds and light lists of a view and frame: bounds, prep,
 *                    count, scan, fill (cluster_build.slang, light_cull.slang)
 *   light_data.c     the arithmetic of lights.slang, ltc.slang and the cluster grid in C (units,
 *                    attenuation, slices, cluster bounds, the light-vs-cluster test): the tests'
 *                    reference, and what the CPU packs into the lighting constants
 *   ltc_data.c       the GGX LTC tables (generated, tools/ltc_fit/ltc_fit.c)
 *   lighting_inputs.h the optional inputs (shadows, AO, indirect) of S14/S15 producers
 *
 * The stage "lighting" runs in DE__VIEW_SLOT_LIGHTING (views_internal.h), after the vis frame of
 * the same view (vis_frame.c keeps each view's context for the frame: de__vis_frame_context). It
 * adds its radiance to the vis frame's HDR lighting target (de__vis_context.gbuffer[DE__GB_
 * LIGHTING], which holds the resolve's emission), so it runs only when something reads that target
 * (the output pass, DENG-304); until then the render graph culls it, and the resolve with it.
 *
 * Threads: create, destroy and the cvars on the main thread; the stage and de__lighting_build on
 * the render thread, one view at a time; pass callbacks on task workers.
 */
#ifndef DE_RENDER_LIGHTING_LIGHTING_H
#define DE_RENDER_LIGHTING_LIGHTING_H

#include <dengine/de_base.h>
#include <dengine/de_gpu.h>
#include <dengine/de_math.h>
#include <dengine/de_memory.h>
#include <dengine/de_render.h>

#include "render/lighting/lighting_inputs.h"
#include "render/passes/vis/vis_stages.h"

DE_EXTERN_C_BEGIN

struct de_asset_system;
struct de__render_world_frame;

/* =================================================================================================
 * GPU layouts (lights.slang, lighting_common.slang; the test lighting.layout_match compares)
 * ============================================================================================== */

/** de_light_kind values the shaders use. */
enum {
    DE__LIGHT_DIRECTIONAL = 0,
    DE__LIGHT_POINT = 1,
    DE__LIGHT_SPOT = 2,
    DE__LIGHT_RECT = 3,
    DE__LIGHT_DISK = 4
};
/** de__cluster_light flags (axis[3] as bits). */
enum { DE__CLUSTER_LIGHT_PROXY = 1u, DE__CLUSTER_LIGHT_SHADOWS = 2u };
/** de__lighting_view.flags */
enum { DE__LIGHTING_ORTHO = 1u, DE__LIGHTING_SPLIT = 2u, DE__LIGHTING_SSS = 4u /* DENG-306 */ };
/** Directional lights shaded at most. */
#define DE__LIGHTING_MAX_DIRECTIONAL 4u
/** Byte offsets of the light counters (lighting_common.slang DE_LIGHTING_COUNTER_*). */
#define DE__LIGHTING_COUNTER_LOCAL       0u
#define DE__LIGHTING_COUNTER_DIRECTIONAL 4u
#define DE__LIGHTING_COUNTER_PROXIES     8u
#define DE__LIGHTING_COUNTER_OVERFLOW    12u
#define DE__LIGHTING_COUNTER_ENTRIES     16u
#define DE__LIGHTING_COUNTER_DROPPED     20u
#define DE__LIGHTING_COUNTER_BYTES       32u
/** Threads of cs_light_scan (one group: at most 1024 x chunk clusters). */
#define DE__LIGHTING_SCAN_GROUP 1024u

/** DeClusterLight (96 bytes). */
typedef struct de__cluster_light {
    f32 position[4];  /**< render-origin xyz, range */
    f32 direction[4]; /**< the direction it points, source radius */
    f32 color[4];     /**< colour x intensity (cd, lux, nits), cos(outer) */
    f32 shape[4];     /**< 1 / (cos inner - cos outer), area half extents, kind (bits) */
    f32 axis[4];      /**< area X axis (spot: view-space direction), flags (bits) */
    f32 bound[4];     /**< view-space culling sphere */
} de__cluster_light;
DE_STATIC_ASSERT(sizeof(de__cluster_light) == 96, "DeClusterLight is 96 bytes");

/** DeIbl flags (ibl.slang). */
enum { DE__IBL_ENVIRONMENT = 1u };
/** Rows of the environment strip (ibl.slang DE_IBL_STRIP_FACES). */
#define DE__IBL_STRIP_FACES 6u

/** ibl.slang DeIbl (192 bytes): one view's image-based lighting constants. */
typedef struct de__ibl_gpu {
    f32 sh[9][4];    /**< irradiance SH9 E_k (rgb) x intensity; sh[0][3]: the intensity */
    f32 rotation[4]; /**< cos, sin of the rotation about +Z, mip_per_roughness, last strip mip */
    f32 ambient[4];  /**< rgb: the constant radiance without an environment */
    u32 strip, sampler, face_size, flags;
} de__ibl_gpu;
DE_STATIC_ASSERT(sizeof(de__ibl_gpu) == 192, "DeIbl is 192 bytes");

/** DeLightingView (400 bytes): one view's lighting constants for one frame. */
typedef struct de__lighting_view {
    u32 tiles_x, tiles_y, slices, tile_px;
    u32 cluster_count, max_per_cluster, list_capacity, local_capacity;
    f32 near_z, far_z, log_scale, log_bias;
    f32 cutoff, emissive_threshold, pre_exposure;
    u32 proxy_capacity;
    u32 flags, light_slots, instance_slots;
    u32 brdf_lut;       /**< the Standard model's energy LUT (de__renderer_brdf_lut; 0 = none) */
    f32 inv_proj[4][4]; /**< clip -> view, rows */
    de__lighting_inputs_gpu inputs;
    de__ibl_gpu ibl; /**< DENG-304 */
} de__lighting_view;
DE_STATIC_ASSERT(sizeof(de__lighting_view) == 400, "DeLightingView is 400 bytes");

/* =================================================================================================
 * The arithmetic (light_data.c; lights.slang and the cluster grid in C)
 * ============================================================================================== */

/** Candela of `lumens` from a point light: lumens / 4 pi. */
f32 de__light_point_candela(f32 lumens);
/** Candela of `lumens` from a spot of outer half-angle `outer`: lumens / (2 pi (1 - cos)). */
f32 de__light_spot_candela(f32 lumens, f32 outer);
/** saturate(1 - (d / r)^4)^2 (the window of the inverse square). */
f32 de__light_window(f32 d, f32 r);
/** The range where intensity `candela` falls to `cutoff` lux: sqrt(candela / cutoff). */
f32 de__light_derived_range(f32 candela, f32 cutoff);
/** The spot factor (t^2) at angle cos `c` between inner and outer half-angles. */
f32 de__light_spot_factor(f32 c, f32 inner, f32 outer);
/** Illuminance (lux, normal incidence) of a point light of `lumens` at distance `d` with range
 * `r` (0: derived with `cutoff`) and source radius `rs`. */
f32 de__light_point_illuminance(f32 lumens, f32 d, f32 r, f32 rs, f32 cutoff);

/** The cluster grid of a view (what de__lighting_view carries). */
typedef struct de__cluster_grid {
    u32 width, height; /**< the view's internal size */
    u32 tile_px, tiles_x, tiles_y, slices, count;
    f32 near_z, far_z, log_scale, log_bias;
} de__cluster_grid;
/** The grid of a view of `width` x `height` with tiles of `tile_px`, `slices` slices, the camera
 * near plane `near_z` and the start of the infinite slice `far_z`. */
de__cluster_grid de__cluster_grid_make(u32 width, u32 height, u32 tile_px, u32 slices, f32 near_z,
                                       f32 far_z);
/** The slice of view depth `depth`. */
u32 de__cluster_slice(const de__cluster_grid *g, f32 depth);
/** The near depth of slice `k` (k >= slices: a large value). */
f32 de__cluster_slice_depth(const de__cluster_grid *g, u32 k);
/** The view-space AABB of cluster `c` (cluster_build.slang's arithmetic; `inv_proj`: the 16
 * floats of de__lighting_view.inv_proj, rows). */
void de__cluster_bounds(const de__cluster_grid *g, const f32 *inv_proj, u32 c, f32 lo[3],
                        f32 hi[3]);
/** light_cull.slang de_light_touches: may local light `l` light the AABB lo..hi? */
bool de__cluster_light_touches(const de__cluster_light *l, const f32 lo[3], const f32 hi[3]);

/** The LTC form factor (cosine-weighted solid angle / pi) of polygon `p` (n <= 16 points, x y z
 * each, relative to the shading point, z up), clipped to z >= 0: ltc.slang de_ltc_polygon. */
f32 de__ltc_polygon(const f32 *p, u32 n);

/* =================================================================================================
 * The stage
 * ============================================================================================== */

typedef struct de__lighting de__lighting;

typedef struct de__lighting_desc {
    de_gpu_device *device;
    const de_allocator *allocator; /**< NULL = the heap */
    de__vis_shared *vis;           /**< the vis frame whose contexts it lights */
    u32 brdf_lut; /**< the energy LUT's bindless index (de__renderer_brdf_lut), 0 = none */
} de__lighting_desc;

/** Makes the lighting state: cvars, kernels (engine shaders lighting_*), the LTC textures (one
 * blocking upload). Without shader code for the backend the state is made but off (logged). */
de_result de__lighting_create_ex(const de__lighting_desc *desc, de__lighting **out);
void de__lighting_destroy(de__lighting *lt);
/** True when the kernels exist (engine shaders built for this backend). */
bool de__lighting_enabled(const de__lighting *lt);

/** What one view's lighting published this frame (graph resources of its graph). */
typedef struct de__lighting_context {
    de__cluster_grid grid;
    de__lighting_view view; /**< the constants uploaded this frame */
    u32 local_capacity, list_capacity;
    de_rg_resource bounds;        /**< 32 bytes per cluster (min, max float4; view space) */
    de_rg_resource lights;        /**< de__cluster_light[local_capacity] */
    de_rg_resource dir_lights;    /**< de__cluster_light[DE__LIGHTING_MAX_DIRECTIONAL] */
    de_rg_resource grid_buf;      /**< u32 x 2 per cluster: offset, count */
    de_rg_resource list;          /**< u32 per entry */
    de_rg_resource counters;      /**< DE__LIGHTING_COUNTER_* */
    de_rg_resource tile_counters; /**< u32 per material model: tiles lit */
    /** RGBA16F split targets (invalid when not split). DENG-306: with the subsurface input and
     * no r.split_lighting, only `diffuse` (written at SUBSURFACE pixels, not cleared). */
    de_rg_resource diffuse, specular;
    de_rg_resource heatmap; /**< RGBA16F cluster heatmap (r.cluster_debug) */
    bool lit;               /**< the deferred pass was added */
    /* ---- DENG-304 ---- */
    de_rg_resource target; /**< the HDR lighting target it lit (vc->gbuffer[DE__GB_LIGHTING]) */
    u32 width, height;     /**< its size (the view's internal size) */
    de_rg_resource strip;  /**< the environment strip it read (import), invalid without one */
    /* ---- DENG-305: what the forward transparent pass shades with (as the deferred pass) ---- */
    de_rg_resource ltc1, ltc2; /**< the LTC tables (imports, DE_ACCESS_SHADER_READ) */
    u32 constants_bindless;    /**< the view's DeLightingView ring (de_robuffer) */
    u32 constants_entry;       /**< this frame's entry in it */
    u32 sampler_bindless;      /**< the LTC tables' sampler */
    de_rg_resource probe_records, probe_list, probe_textures[64];
    u32 probe_texture_count;
    de_rg_resource gi_records, gi_textures[16], gi_buffers[8];
    u32 gi_texture_count, gi_buffer_count;
    de_rg_resource shadow_records, shadow_textures[8];
    de_rg_resource atlas_records, atlas_texture;
    u32 shadow_texture_count;
} de__lighting_context;

/**
 * One view's lighting: the cluster lists from the GPU scene's lights and the emissive proxies,
 * then the deferred pass over `vc`'s G-buffer (when the resolve ran). `*out` (may be NULL) gets
 * the context on the graph's frame arena, NULL when nothing was added (no G-buffer, the stage
 * off). Render thread.
 */
de_result de__lighting_build(de__lighting *lt, de_rg *g, de__vis_context *vc,
                             de__lighting_context **out);
/** The inputs view slot `slot` lights with in its next build (reset after it). */
de__lighting_inputs *de__lighting_view_inputs(de__lighting *lt, u32 slot);

/** What the CPU last read back of a view's light lists (one to a few frames late). */
typedef struct de__lighting_stats {
    bool valid;
    u64 frame;
    u32 local, directional, proxies;   /**< asked for (may exceed the capacities) */
    u32 overflow;                      /**< entries over r.cluster_max_lights */
    u32 entries, dropped;              /**< list entries kept / cut by the list capacity */
    u32 tiles[DE__GS_MATERIAL_MODELS]; /**< tiles the deferred pass lit per material model */
} de__lighting_stats;
de__lighting_stats de__lighting_view_stats(const de__lighting *lt, u32 slot);

/** Test overrides. */
typedef struct de__lighting_debug {
    bool split;    /**< allocate and write the split targets (as r.split_lighting 1) */
    bool heatmap;  /**< build the heatmap (as r.cluster_debug 1) */
    bool readback; /**< read the counters back every frame */
} de__lighting_debug;
void de__lighting_set_debug(de__lighting *lt, const de__lighting_debug *d);

/** The `public static const uint` values of the lighting shaders, for the layout test. */
typedef struct de__lighting_const {
    const char *name;
    u32 value;
} de__lighting_const;
const de__lighting_const *de__lighting_layout_consts(u32 *count);

/**
 * DENG-304: the context de__lighting_build published for view `view` in graph `g` in frame
 * `frame_index` (the output pass reads its lighting target), or NULL when that frame lit nothing.
 * Render thread, while `g` is being built.
 */
de__lighting_context *de__lighting_frame_context(const de__lighting *lt, de_view view,
                                                 const de_rg *g, u64 frame_index);

/* ---------- Image-based lighting (ibl.c, DENG-304) ---------- */

/** The environment lighting uses: snapshotted at the kick from the render world's DeEnvironment
 * and its HDRI's texture record (de__lighting_prepare_environment), or set by a test. */
typedef struct de__ibl_environment {
    bool valid;          /**< false: no environment (r.ambient_fallback) */
    de_gpu_texture cube; /**< the cooked prefiltered cube (DE_ACCESS_SHADER_READ between graphs) */
    u32 format;          /**< de_gpu_format */
    u32 face_size, mip_count;
    f32 mip_per_roughness; /**< mip = perceptual roughness x this */
    f32 sh9[9][3];         /**< irradiance coefficients E_k (de_cooked_env.h) */
    f32 intensity;         /**< DeEnvironment.intensity */
    f32 rotation;          /**< DeEnvironment.rotation (radians about +Z) */
} de__ibl_environment;

/** Sets the environment of the next builds. Main thread with the render thread idle (the kick), or
 * a test between frames. NULL: none. */
void de__lighting_set_environment(de__lighting *lt, const de__ibl_environment *env);
/**
 * The kick's snapshot (de__renderer_kick_prepare, main thread, render thread idle; DENG-422: the
 * only place lighting reads asset records): the first DeEnvironment of `frame` (may be NULL) and
 * its HDRI's record from `assets` (may be NULL). An HDRI that is not loaded, not a cooked
 * environment or not a cube gives no environment.
 */
void de__lighting_prepare_environment(de__lighting *lt, struct de_asset_system *assets,
                                      const struct de__render_world_frame *frame);
/** The strip mips of a `face_size` cube of `mip_count` mips in `format`: every mip, but for block
 * formats only those whose faces are at least one block (4 texels). */
u32 de__ibl_strip_mips(u32 format, u32 face_size, u32 mip_count);
/** Packs DeIbl: `env` (NULL or invalid: the ambient `ambient_nits`), the strip's bindless index and
 * mips, the sampler. Pure. */
void de__ibl_pack(const de__ibl_environment *env, u32 strip, u32 strip_mips, u32 sampler,
                  f32 ambient_nits, de__ibl_gpu *out);
/** ibl.slang de_ibl_irradiance in C: E(n) (rgb) of `g` at unit world normal `n`. */
void de__ibl_irradiance(const de__ibl_gpu *g, const f32 n[3], f32 out[3]);
/** ibl.slang de_ibl_rotate in C. */
void de__ibl_rotate(const de__ibl_gpu *g, const f32 d[3], f32 out[3]);

/* ---------- Exposure (exposure.c, DENG-304) ---------- */

/** Default of r.exposure_ev100_default: EV100 15, the sunny-16 rule (the default directional light
 * is 100000 lux). */
#define DE__EXPOSURE_EV100_DEFAULT 15.0
/** The exposure of EV100 `ev100`: 1 / (1.2 x 2^ev100) (lens and sensor constants q K / S folded
 * into the 1.2 of the saturation-based ISO speed). */
f32 de__exposure_from_ev100(f32 ev100);
/** One view's exposure: this frame's and the previous frame's. */
typedef struct de__exposure_state {
    de_view view; /**< the view it belongs to (another view in the slot resets it) */
    u64 frame;    /**< the frame `current` is for */
    bool valid;
    f32 ev100;    /**< this frame's EV100 */
    f32 current;  /**< this frame's exposure */
    f32 previous; /**< the previous frame's (this frame's in the view's first frame) */
} de__exposure_state;
/**
 * The exposure of `view` in frame `frame` from the camera's EV100 (NaN: `fallback_ev100`; S18's
 * auto exposure, DENG-160, replaces only this source). The first call of a frame shifts `current`
 * into `previous`; later calls of the same frame return the same values. `*prev` (may be NULL)
 * gets the previous frame's exposure.
 */
f32 de__exposure_update(de__exposure_state *s, de_view view, u64 frame, f32 camera_ev100,
                        f32 fallback_ev100, f32 *prev);
/** The exposure of view slot `slot` for `frame` (de__exposure_update with r.exposure_ev100_default
 * as the fallback). Render thread (the GPU scene's view stage, then lighting). */
f32 de__lighting_view_exposure(de__lighting *lt, u32 slot, de_view view, u64 frame,
                               f32 camera_ev100, f32 *prev);

/* ---------- The output pass (passes/output_basic.c, DENG-304) ---------- */

typedef struct de__output_basic de__output_basic;
/** Makes the output pass's state (engine shader post_output_basic) on `vis`'s device. Off without
 * shader code (logged); NULL `vis` or a frame that is off: off. */
de_result de__output_basic_create_ex(de__vis_shared *vis, de__output_basic **out);
void de__output_basic_destroy(de__output_basic *ob);
bool de__output_basic_enabled(const de__output_basic *ob);
/**
 * Adds "output.basic" to `g`: `src` (the pre-exposed HDR lighting target, sampled) clamped to
 * [0, 1] into a new RGBA16F texture of `width` x `height` (linear: the sRGB swapchain encodes),
 * or with `encode` the sRGB OETF applied too (for targets that do not encode). Returns the new
 * texture, invalid when nothing was added. `arena` holds the pass data.
 */
de_rg_resource de__output_basic_add(de__output_basic *ob, de_rg *g, de_arena *arena,
                                    de_rg_resource src, u32 width, u32 height, bool encode);
/** Creates the renderer's output pass and registers its view stage "output.basic" at
 * (DE__VIEW_SLOT_POST, DE__VIEW_ORDER_TONEMAP), where DE_HOOK_BEFORE_TONEMAP and
 * DE_HOOK_AFTER_TONEMAP sit around it. Main thread, from de__views_create. */
de_result de__output_basic_create(de_renderer *r, de__lighting *lt, de__output_basic **out);

/* ---------- The renderer (views.c) ---------- */

/** Creates the renderer's lighting and registers its view stage "lighting" at
 * (DE__VIEW_SLOT_LIGHTING, 0). Main thread, from de__views_create. */
de_result de__lighting_create(de_renderer *r, struct de__vis_shared *vis, de__lighting **out);

/* =================================================================================================
 * Internals (lighting_pass.c, clusters.c)
 * ============================================================================================== */

/** Bytes of the tile counters (u32 per material model) and of one readback entry (the light
 * counters, then the tile counters). */
#define DE__LIGHTING_TILE_COUNTER_BYTES (DE__GS_MATERIAL_MODELS * 4u)
#define DE__LIGHTING_READBACK_STRIDE    256u
DE_STATIC_ASSERT(DE__LIGHTING_COUNTER_BYTES + DE__LIGHTING_TILE_COUNTER_BYTES <=
                     DE__LIGHTING_READBACK_STRIDE,
                 "one readback entry holds both counter blocks");

/** Per view slot. Its buffers are made when the view first lights (a new view in the slot
 * releases them, deferred). */
typedef struct de__lighting_view_state {
    de_view handle;
    /* The lighting constants: an UPLOAD ring of DE__VIS_FRAME_RING entries; frame N writes entry
     * N % ring while building, and the deferred pass's execute fills in the inputs' indices. */
    de_gpu_buffer constants;
    u8 *constants_map;
    u32 constants_bindless;
    u32 entry;
    /* Stats: a READBACK ring of DE__VIS_FRAME_RING entries of DE__LIGHTING_READBACK_STRIDE. */
    de_gpu_buffer rb;
    const u8 *rb_map;
    u32 rb_access;
    u64 slot_frame[DE__VIS_FRAME_RING];
    bool slot_live[DE__VIS_FRAME_RING];
    de__lighting_stats stats;
    /* The inputs of the next build (de__lighting_view_inputs). */
    de__lighting_inputs inputs;
} de__lighting_view_state;

struct de__lighting {
    de_gpu_device *dev;
    const de_allocator *alloc;
    de_log_category log;
    struct de__vis_shared *vis;
    u32 brdf_lut; /**< de__lighting_desc.brdf_lut */
    bool enabled;
    char why[256];
    de__vis_kernel cluster_build; /* lighting_clusters */
    de__vis_kernel light_clear, light_prep, light_count, light_scan, light_fill; /* lighting_cull */
    de__vis_kernel deferred, clear_split, heatmap; /* lighting_deferred */
    /* The LTC tables: RGBA32F and RG32F, DE__LTC_SIZE square, uploaded at create (one blocking
     * copy), DE_ACCESS_SHADER_READ ever after. */
    de_gpu_texture ltc1, ltc2;
    de_gpu_sampler sampler; /**< linear, clamp (the LTC tables) */
    u32 sampler_bindless;
    de_cvar cv_tile, cv_slices, cv_far, cv_max_lights, cv_cutoff, cv_emissive_threshold,
        cv_emissive_max, cv_split, cv_debug;
    de__lighting_debug debug;
    de__lighting_view_state views[DE__VIEW_MAX];
    /* ---- DENG-304 ---- */
    /* Each view slot's context of the frame being built (de__lighting_frame_context) and the keys
     * that say it is still current. */
    de__lighting_context *frame_context[DE__VIEW_MAX];
    const de_rg *frame_context_graph[DE__VIEW_MAX];
    u64 frame_context_frame[DE__VIEW_MAX];
    de_view frame_context_view[DE__VIEW_MAX];
    /* Image-based lighting (ibl.c): the environment, its strip (a 2D texture, the cube's faces
     * stacked; DE_ACCESS_SHADER_READ between graphs once copied), the trilinear sampler. */
    de__ibl_environment env;
    de_gpu_texture strip;
    de__ibl_environment strip_env; /**< what `strip` holds */
    u32 strip_bindless, strip_mips;
    bool strip_ready; /**< the copy into `strip` was added to a graph */
    de_gpu_sampler ibl_sampler;
    u32 ibl_sampler_bindless;
    de_cvar cv_ambient, cv_ev100_default;
    /* Exposure (exposure.c), per view slot. */
    de__exposure_state exposure[DE__VIEW_MAX];
};

/** ibl.c: the sampler and cvars (from de__lighting_create_ex) and their release. */
de_result de__ibl_init(de__lighting *lt);
void de__ibl_shutdown(de__lighting *lt);
/**
 * ibl.c, from de__lighting_build: packs `lc->view.ibl` from the current environment and, when the
 * environment's cube changed, makes the strip and adds "lighting.ibl_strip_read" /
 * "lighting.ibl_strip_write" (the cube's faces through a transient buffer into the strip) to `g`.
 * Sets `lc->strip` to the strip's import when the deferred pass samples it.
 */
void de__ibl_build(de__lighting *lt, de_rg *g, de__lighting_context *lc);

/**
 * Adds the cluster passes of one view (clusters.c): "lighting.cluster_bounds",
 * "lighting.lights_clear", "_prep", "_count", "_scan" and "_fill", and makes lc's buffers (graph
 * transients). `lc->view`, `lc->grid` and the view state's constants entry must be written. False
 * when they could not be added (the error is set).
 */
bool de__clusters_build(de__lighting *lt, de_rg *g, de__vis_context *vc,
                        de__lighting_view_state *vs, de__lighting_context *lc);
/** The view-space inverse projection rows of `proj` (clip -> view). */
void de__lighting_inv_proj(const de_mat4 *proj, f32 out[4][4]);

DE_EXTERN_C_END

#endif /* DE_RENDER_LIGHTING_LIGHTING_H */
