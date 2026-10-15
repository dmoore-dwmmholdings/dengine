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
enum { DE__LIGHTING_ORTHO = 1u, DE__LIGHTING_SPLIT = 2u };
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

/** DeLightingView (176 bytes): one view's lighting constants for one frame. */
typedef struct de__lighting_view {
    u32 tiles_x, tiles_y, slices, tile_px;
    u32 cluster_count, max_per_cluster, list_capacity, local_capacity;
    f32 near_z, far_z, log_scale, log_bias;
    f32 cutoff, emissive_threshold, pre_exposure;
    u32 proxy_capacity;
    u32 flags, light_slots, instance_slots, reserved;
    f32 inv_proj[4][4]; /**< clip -> view, rows */
    de__lighting_inputs_gpu inputs;
} de__lighting_view;
DE_STATIC_ASSERT(sizeof(de__lighting_view) == 176, "DeLightingView is 176 bytes");

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
/** The view-space AABB of cluster `c` (cluster_build.slang's arithmetic; inv_proj rows). */
void de__cluster_bounds(const de__cluster_grid *g, const f32 inv_proj[4][4], u32 c, f32 lo[3],
                        f32 hi[3]);
/** light_cull.slang de_light_touches: may local light `l` light the AABB lo..hi? */
bool de__cluster_light_touches(const de__cluster_light *l, const f32 lo[3], const f32 hi[3]);

/** The LTC form factor (cosine-weighted solid angle / pi) of polygon `p` (n <= 16 points
 * relative to the shading point, z up), clipped to z >= 0: ltc.slang de_ltc_polygon. */
f32 de__ltc_polygon(const f32 p[][3], u32 n);

/* =================================================================================================
 * The stage
 * ============================================================================================== */

typedef struct de__lighting de__lighting;

typedef struct de__lighting_desc {
    de_gpu_device *device;
    const de_allocator *allocator; /**< NULL = the heap */
    de__vis_shared *vis;           /**< the vis frame whose contexts it lights */
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
    de_rg_resource bounds;            /**< 32 bytes per cluster (min, max float4; view space) */
    de_rg_resource lights;            /**< de__cluster_light[local_capacity] */
    de_rg_resource dir_lights;        /**< de__cluster_light[DE__LIGHTING_MAX_DIRECTIONAL] */
    de_rg_resource grid_buf;          /**< u32 x 2 per cluster: offset, count */
    de_rg_resource list;              /**< u32 per entry */
    de_rg_resource counters;          /**< DE__LIGHTING_COUNTER_* */
    de_rg_resource tile_counters;     /**< u32 per material model: tiles lit */
    de_rg_resource diffuse, specular; /**< RGBA16F split targets (invalid when not split) */
    de_rg_resource heatmap;           /**< RGBA16F cluster heatmap (r.cluster_debug) */
    bool lit;                         /**< the deferred pass was added */
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
};

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
