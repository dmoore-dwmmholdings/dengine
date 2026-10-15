/*
 * vis_stages.h: the visibility-buffer frame and the contract of its stages (SPEC §12.5; DENG-229).
 * Internal to de_render; include as "render/passes/vis/vis_stages.h".
 *
 * Every S13 task codes against this file: keep the signatures stable. A later task adds fields
 * only at the marked ends of de__vis_shared and de__vis_context, deletes its own stub from
 * vis_stages_stub.c and implements its stage in its own file.
 *
 * ---- The frame (vis_frame.c) ------------------------------------------------------------------
 *
 * The renderer registers one view stage, "vis.frame", at (DE__VIEW_SLOT_VISIBILITY,
 * DE__VIS_FRAME_ORDER), after the GPU scene's "gpu_scene.view". For each view with a camera it
 * fills a de__vis_context on the view graph's frame arena (de__vis_frame_build) and calls the
 * stages in this order, each with the view's graph:
 *
 *     de__vis_cull_instances(g, vc, PHASE_1)   instances -> work items (cull_instances.c)
 *     de__vis_cull_meshlets(g, vc, PHASE_1)    work items -> visible meshlets (DENG-231)
 *     de__vis_raster(g, vc, PHASE_1)           visible meshlets -> vis buffer + depth (DENG-234/5)
 *     de__vis_raster_alpha(g, vc, PHASE_1)     alpha-tested meshlets, same targets (DENG-236)
 *     de__vis_hzb_build(g, vc)                 depth -> HZB (DENG-233)
 *     de__vis_cull_instances(g, vc, PHASE_2)   (DENG-233: what phase 1 rejected, against the HZB)
 *     de__vis_cull_meshlets(g, vc, PHASE_2)
 *     de__vis_raster(g, vc, PHASE_2)
 *     de__vis_raster_alpha(g, vc, PHASE_2)
 *     de__vis_classify(g, vc)                  tiles by material model (DENG-237)
 *     de__vis_resolve(g, vc)                   attributes, thin G-buffer, feedback (DENG-238)
 *
 * A stage declares passes in `g` and publishes the resources later stages read in `vc` (the
 * render graph orders and culls them: a pass whose output nothing reads is culled). An invalid
 * resource handle in `vc` means "nothing this frame" (no instances, a stage that did not run):
 * a stage that reads it declares nothing, or clears its outputs.
 *
 * Each stage has an init and a shutdown (per renderer, main thread): init loads its shaders with
 * de__vis_shader_load / de__vis_kernel_create and keeps its state in `s->stage[<its id>]`; a stage
 * that keeps per-view state keys it by `vc->view_slot` and resets it when `vc->view_new` is set.
 * An init that returns DE_ERR_UNSUPPORTED (no code for this backend in this build) turns the frame
 * off with one warning; another error fails the renderer.
 *
 * Engine shaders (DENG-421): each shader a stage uses is one line of shaders/engine_shaders.cmake,
 * `de_engine_shader(<name> <source below shaders/>)`, and the stage loads it by that name with
 * de__vis_shader_load (de__engine_shader_load: cooked through the cache in Dev builds, the
 * build-time cook embedded in every other build, a placeholder on the null backend). So the frame
 * runs in Release and Shipping too.
 *
 * ---- Threads and memory ---------------------------------------------------------------------
 *
 * Create, destroy, init and shutdown on the main thread; de__vis_frame_build and the stage
 * functions on the thread that renders (the render thread), one view at a time. Pass callbacks
 * follow the render graph's rules (they run on task workers; rg_internal.h). Per-frame memory
 * comes from the graph's frame arena; persistent GPU objects are made when a view first culls and
 * replaced (deferred destruction) when a new view takes its slot. A steady frame allocates no heap.
 */
#ifndef DE_RENDER_PASSES_VIS_VIS_STAGES_H
#define DE_RENDER_PASSES_VIS_VIS_STAGES_H

#include <dengine/de_base.h>
#include <dengine/de_debug.h>
#include <dengine/de_gpu.h>
#include <dengine/de_math.h>
#include <dengine/de_memory.h>
#include <dengine/de_render.h>

#include "render/scene/gpu_scene.h"
#include "render/shaders/engine_shaders.h"
#include "render/views_internal.h"

DE_EXTERN_C_BEGIN

/* =================================================================================================
 * C mirrors of shaders/visbuffer/vis_types.slang (the test vis.types.layout_match compares them)
 * ============================================================================================== */

/** Meshlets per work item at most (one 64-thread meshlet-cull group per work item). */
#define DE__VIS_CHUNK_MESHLETS 64u
/** Thread groups along x of one dispatch at most; larger counts continue in y. */
#define DE__VIS_MAX_GROUPS_X 65535u
/** Packed vis ID: (visible_meshlet_index << 7) | triangle_in_meshlet (DENG-2 addendum). */
#define DE__VIS_ID_TRIANGLE_BITS        7u
#define DE__VIS_ID_TRIANGLE_MASK        127u
#define DE__VIS_ID_MAX_VISIBLE_MESHLETS (1u << 25)
#define DE__VIS_ID_EMPTY                0xFFFFFFFFu
/** Byte offsets of the instance-cull counters (a raw 16-byte buffer). */
#define DE__VIS_COUNTER_WORK_ITEMS        0u
#define DE__VIS_COUNTER_INSTANCES_VISIBLE 4u
#define DE__VIS_COUNTER_INSTANCES_TESTED  8u
#define DE__VIS_COUNTER_MESHLETS          12u
#define DE__VIS_COUNTER_BYTES             16u

/** DeVisWorkItem: one chunk of one visible instance's LOD. */
typedef struct de__vis_work_item {
    u32 instance, lod, meshlet_first, meshlet_count;
} de__vis_work_item;
/** DeVisMeshlet: one visible meshlet. */
typedef struct de__vis_meshlet {
    u32 instance, meshlet;
} de__vis_meshlet;
/** DeVisDispatchArgs: de_gpu_cmd_dispatch_indirect's x, y, z and the item count. */
typedef struct de__vis_dispatch_args {
    u32 x, y, z, count;
} de__vis_dispatch_args;
/** DeVisMeshArgs: de_gpu_cmd_dispatch_mesh_indirect (12 bytes apart). */
typedef struct de__vis_mesh_args {
    u32 x, y, z;
} de__vis_mesh_args;
/** DeVisDrawIndexedArgs: de_gpu_cmd_draw_indexed_indirect (20 bytes apart). */
typedef struct de__vis_draw_indexed_args {
    u32 index_count, instance_count, first_index;
    i32 vertex_offset;
    u32 first_instance;
} de__vis_draw_indexed_args;

DE_STATIC_ASSERT(sizeof(de__vis_work_item) == 16, "DeVisWorkItem is 16 bytes");
DE_STATIC_ASSERT(sizeof(de__vis_meshlet) == 8, "DeVisMeshlet is 8 bytes");
DE_STATIC_ASSERT(sizeof(de__vis_dispatch_args) == 16, "DeVisDispatchArgs is 16 bytes");
DE_STATIC_ASSERT(sizeof(de__vis_mesh_args) == 12, "DeVisMeshArgs is 12 bytes");
DE_STATIC_ASSERT(sizeof(de__vis_draw_indexed_args) == 20, "DeVisDrawIndexedArgs is 20 bytes");

/** The vis ID of `triangle` of visible meshlet `visible_meshlet`. */
static inline u32 de__vis_id_pack(u32 visible_meshlet, u32 triangle) {
    return (visible_meshlet << DE__VIS_ID_TRIANGLE_BITS) | (triangle & DE__VIS_ID_TRIANGLE_MASK);
}
static inline u32 de__vis_id_meshlet(u32 id) {
    return id >> DE__VIS_ID_TRIANGLE_BITS;
}
static inline u32 de__vis_id_triangle(u32 id) {
    return id & DE__VIS_ID_TRIANGLE_MASK;
}
/** de_vis_dispatch_args: one group per item, x <= DE__VIS_MAX_GROUPS_X, x and y at least 1. */
static inline de__vis_dispatch_args de__vis_dispatch_args_for(u32 count) {
    de__vis_dispatch_args a = {.z = 1u, .count = count};
    if (count <= DE__VIS_MAX_GROUPS_X) {
        a.x = count > 0 ? count : 1u;
        a.y = 1u;
    } else {
        a.x = DE__VIS_MAX_GROUPS_X;
        a.y = (count + DE__VIS_MAX_GROUPS_X - 1u) / DE__VIS_MAX_GROUPS_X;
    }
    return a;
}

/* =================================================================================================
 * C mirror of shaders/culling/cull_common.slang
 * ============================================================================================== */

/** DeVisCullView.flags */
enum { DE__VIS_CULL_FRUSTUM = 1u, DE__VIS_CULL_FROZEN = 2u, DE__VIS_CULL_OCCLUSION = 4u };
/** Frustum planes tested: left, right, bottom, top, near (the far plane is skipped). */
#define DE__VIS_CULL_PLANES 5u

/** DeVisCullView: one view's cull constants (224 bytes). */
typedef struct de__vis_cull_view {
    f32 planes[DE__VIS_CULL_PLANES][4]; /**< normal xyz (unit, inward), d */
    f32 camera[4];                      /**< culling camera (render-origin space), near_z */
    f32 lod[4];                         /**< LOD scale, orthographic (0/1), LOD bias, 0 */
    f32 view_proj[4][4];                /**< the culling view-projection, rows */
    u32 instance_count, layer_mask, work_capacity, flags;
    /* DENG-233: the view's HZB (sampled bindless index, mip 0 size, mips; 0 = none) and its
     * visibility flags (bindless index; slots the last frame wrote, 0 = no history). */
    u32 hzb, hzb_width, hzb_height, hzb_mips;
    u32 visibility, visibility_count;
    u32 reserved0, reserved1;
} de__vis_cull_view;
DE_STATIC_ASSERT(sizeof(de__vis_cull_view) == 224, "DeVisCullView is 224 bytes");

/** The culling camera of one view. */
typedef struct de__vis_cull_camera {
    de_mat4 view_proj; /**< render-origin space -> clip (reverse-Z) */
    de_mat4 proj;      /**< view -> clip: the LOD scale, near plane and orthographic flag */
    de_vec3 position;  /**< relative to the render origin */
    u32 height;        /**< internal height in pixels */
    f32 lod_error_px;  /**< r.lod_error_px (> 0) */
    i32 lod_bias;      /**< r.lod_bias */
} de__vis_cull_camera;

/**
 * Fills the frustum, camera and LOD fields of `v` from `cam` (the rest zero; flags FRUSTUM).
 * The planes are de_frustum_from_m4's first five; the LOD scale is
 * 0.5 * height * proj[1][1] / lod_error_px. Pure.
 */
void de__vis_cull_view_init(de__vis_cull_view *v, const de__vis_cull_camera *cam);
/** Moves a culling view made at render origin `from` to origin `to` (r.cull_freeze across a
 * rebase): planes, camera and view-projection. */
void de__vis_cull_view_rebase(de__vis_cull_view *v, de_dvec3 from, de_dvec3 to);

/* CPU reference of instance_cull.slang (the same arithmetic, in the same order). */
/** de_cull_sphere: false when the sphere is entirely behind one plane. */
bool de__vis_cull_sphere(const de__vis_cull_view *v, const f32 c[3], f32 r);
/** de_cull_aabb: false when the box is entirely behind one plane. */
bool de__vis_cull_aabb(const de__vis_cull_view *v, const f32 mn[3], const f32 mx[3]);
/** The largest axis scale of an instance's transform. */
f32 de__vis_max_axis_scale(const de__gs_instance *inst);
/** The LOD instance_cull.slang picks for `inst` of mesh `m` (see the shader's header). */
u32 de__vis_select_lod(const de__vis_cull_view *v, const de__gs_instance *inst,
                       const de__gs_mesh *m);
/**
 * What cs_instance_cull decides for one instance: -1 skipped (free slot, no bounds, no READY
 * mesh with meshlets, layer mask), 0 tested and culled, 1 visible (with `*lod`). `m` may be NULL
 * when inst->mesh is DE__GS_NONE.
 */
i32 de__vis_instance_test(const de__vis_cull_view *v, const de__gs_instance *inst,
                          const de__gs_mesh *m, u32 *lod);

/* =================================================================================================
 * C mirror of shaders/culling/meshlet_cull.slang and DeVisRasterArgs (DENG-231)
 * ============================================================================================== */

/** DeVisRasterArgs (vis_types.slang): one phase's raster arguments. The mesh-shader dispatch is at
 * byte DE__VIS_RASTER_ARGS_MESH, the compute-expansion dispatch (a de__vis_dispatch_args) at
 * DE__VIS_RASTER_ARGS_EXPAND; the phase's entries of the visible list are [first, first + count).
 */
typedef struct de__vis_raster_args {
    u32 mesh_x, mesh_y, mesh_z, first;
    u32 expand_x, expand_y, expand_z, count;
} de__vis_raster_args;
DE_STATIC_ASSERT(sizeof(de__vis_raster_args) == 32, "DeVisRasterArgs is 32 bytes");
#define DE__VIS_RASTER_ARGS_MESH   0u
#define DE__VIS_RASTER_ARGS_EXPAND 16u

/** Byte offsets of the meshlet-cull counters (a raw 32-byte buffer, both phases). */
#define DE__VIS_MESHLET_COUNTER_VISIBLE      0u  /**< visible asked for (> capacity: dropped) */
#define DE__VIS_MESHLET_COUNTER_TESTED       4u  /**< meshlets tested */
#define DE__VIS_MESHLET_COUNTER_PHASE1       8u  /**< entries stored after phase 1 */
#define DE__VIS_MESHLET_COUNTER_ALPHA        16u /**< DENG-236: alpha-tested asked for */
#define DE__VIS_MESHLET_COUNTER_ALPHA_PHASE1 20u /**< alpha entries stored after phase 1 */
#define DE__VIS_MESHLET_COUNTER_FORWARD      24u /**< DENG-305: forward meshlets (not stored) */
#define DE__VIS_MESHLET_COUNTER_BYTES        32u
/** MeshletCullParams.flags */
enum {
    DE__VIS_MESHLET_CONE = 1u,        /**< backface-cone test (r.cull_meshlet_cone) */
    DE__VIS_MESHLET_SMALL = 2u,       /**< small-meshlet test (r.cull_small_tri) */
    DE__VIS_MESHLET_CASTERS_ONLY = 4u /**< only instances with DE__GS_INSTANCE_CAST_SHADOWS */
};
/** Output buckets of meshlet_cull.slang's meshlet_bucket. */
#define DE__VIS_BUCKET_OPAQUE 0u
#define DE__VIS_BUCKET_ALPHA  1u /**< DENG-236: the submesh material is alpha-tested (mask) */
#define DE__VIS_BUCKET_NONE   0xFFFFFFFFu
/** DENG-305: the submesh material is forward (blended or transmissive): counted, never stored;
 * the forward transparent pass (passes/forward/) draws it. */
#define DE__VIS_BUCKET_FORWARD 2u
/** DeGpuMaterial.alpha_mode of an alpha-tested material (de_cooked_alpha_mode mask). */
#define DE__VIS_ALPHA_MODE_MASK 1u
/** Margin of the small-meshlet test in pixels (DE_VIS_SMALL_EPS_PX). */
#define DE__VIS_SMALL_EPS_PX (1.0f / 32.0f)

/** The culling data of one DMSH v2 meshlet record (object space). */
typedef struct de__vis_meshlet_bounds {
    f32 sphere[4]; /**< centre xyz, radius */
    f32 apex[4];   /**< cone apex xyz, cutoff (>= 1: no cone) */
    f32 axis[3];   /**< cone axis */
    u32 submesh;
} de__vis_meshlet_bounds;

/** Decodes record `meshlet` of a DMSH v2 meshlet table (DE_COOKED_MESHLET_SIZE bytes each). */
de__vis_meshlet_bounds de__vis_meshlet_bounds_load(const u8 *records, u32 meshlet);

/** What cs_meshlet_cull decides for one meshlet (de__vis_meshlet_test). */
typedef enum de__vis_meshlet_result {
    DE__VIS_MESHLET_VISIBLE = 0,
    DE__VIS_MESHLET_CULLED_INSTANCE, /**< layer mask, or not a shadow caster in a casters view */
    DE__VIS_MESHLET_CULLED_FRUSTUM,
    DE__VIS_MESHLET_CULLED_CONE,
    DE__VIS_MESHLET_CULLED_SMALL
} de__vis_meshlet_result;

/** The kernel's inputs that are not in the cull view (its push constants). */
typedef struct de__vis_meshlet_params {
    u32 flags;                        /**< DE__VIS_MESHLET_* */
    f32 width, height;                /**< internal size in pixels */
    const de__gs_material *materials; /**< the materials table (NULL: none) */
    u32 material_count;
} de__vis_meshlet_params;

/*
 * CPU reference of meshlet_cull.slang (the same arithmetic, in the same order). The occlusion hook
 * is not modelled (the DENG-229 stub: visible).
 */
/** True when the cone test may cull meshlets of `submesh` of `inst`: a READY, single-sided
 * material in its slot. */
bool de__vis_meshlet_cone_allowed(const de__gs_instance *inst, u32 submesh,
                                  const de__vis_meshlet_params *p);
/** The object-space cone test: true when every triangle faces away from the culling camera. */
bool de__vis_meshlet_cone_culled(const de__gs_instance *inst, const de__vis_meshlet_bounds *m,
                                 const de__vis_cull_view *v);
/** The small-meshlet test of world sphere (c, r): true when it covers no pixel centre. */
bool de__vis_meshlet_small_culled(const de__vis_cull_view *v, const f32 c[3], f32 r, f32 width,
                                  f32 height);
/** The pixel rectangle the small test uses: false when the sphere reaches the near plane (the
 * test is skipped). `rect` = x0, x1, y0, y1. */
bool de__vis_meshlet_screen_rect(const de__vis_cull_view *v, const f32 c[3], f32 r, f32 width,
                                 f32 height, f32 rect[4]);
/** The world bounding sphere of meshlet `m` of `inst` (centre exact, radius x max axis scale). */
void de__vis_meshlet_world_sphere(const de__gs_instance *inst, const de__vis_meshlet_bounds *m,
                                  f32 c[3], f32 *r);
/**
 * Every test of one meshlet, in the kernel's order. `marginal` (may be NULL) is set when a test
 * on the way to the result was within a float tolerance of its threshold (the GPU may decide
 * either way there).
 */
de__vis_meshlet_result de__vis_meshlet_test(const de__vis_cull_view *v, const de__gs_instance *inst,
                                            const de__vis_meshlet_bounds *m,
                                            const de__vis_meshlet_params *p, bool *marginal);

/** What the CPU last read back of one view's meshlet culling (one to a few frames late). */
typedef struct de__vis_meshlet_stats {
    bool valid;
    u64 frame;
    u32 visible;  /**< visible meshlets asked for, both phases (> capacity: some dropped) */
    u32 tested;   /**< meshlets tested, both phases */
    u32 phase1;   /**< entries stored by phase 1 */
    u32 capacity; /**< the capacity of that frame's visible list */
    /* DENG-236: the alpha-tested bucket */
    u32 alpha;          /**< alpha-tested meshlets asked for, both phases */
    u32 alpha_phase1;   /**< alpha entries stored by phase 1 */
    u32 alpha_capacity; /**< the capacity of that frame's alpha region */
    /* DENG-305 */
    u32 forward; /**< forward meshlets (blended or transmissive) kept out of the list, both phases
                  */
} de__vis_meshlet_stats;

/* =================================================================================================
 * Stages
 * ============================================================================================== */

/** Culling phases: 1 tests against last frame's HZB, 2 re-tests what 1 rejected (DENG-233). */
typedef enum de__vis_phase { DE__VIS_PHASE_1 = 1, DE__VIS_PHASE_2 = 2 } de__vis_phase;
/** Array index of a phase in de__vis_context (phase - 1). */
#define DE__VIS_PHASE_INDEX(p) ((u32)(p) - 1u)
#define DE__VIS_PHASES         2u

/** The stages, in the order their init runs; also the index of their state in de__vis_shared. */
typedef enum de__vis_stage_id {
    DE__VIS_STAGE_CULL_INSTANCES,
    DE__VIS_STAGE_CULL_MESHLETS,
    DE__VIS_STAGE_RASTER,
    DE__VIS_STAGE_HZB_BUILD,
    DE__VIS_STAGE_RASTER_ALPHA,
    DE__VIS_STAGE_CLASSIFY,
    DE__VIS_STAGE_RESOLVE,
    DE__VIS_STAGE_COUNT
} de__vis_stage_id;

/** Order of the "vis.frame" stage in DE__VIEW_SLOT_VISIBILITY (after "gpu_scene.view"). */
#define DE__VIS_FRAME_ORDER 0

/** Frames whose per-view GPU entries (cull constants, stats) are kept: frame N reuses frame
 * N - DE__VIS_FRAME_RING's, which must have completed (the renderer keeps at most 3 in flight). */
#define DE__VIS_FRAME_RING 4u

/** The vis frame's state of one renderer (de__vis_shared_create). */
typedef struct de__vis_shared {
    de_renderer *renderer; /**< NULL when a test drives the frame */
    de_gpu_device *dev;
    const de_allocator *alloc;
    de_log_category log;
    bool enabled;  /**< false: no shader code for this backend in this build */
    char why[256]; /**< why it is off */
    /** The view handle each slot culled for last (a new handle sets de__vis_context.view_new). */
    de_view views[DE__VIEW_MAX];
    /** Each stage's own state (its init allocates it, its shutdown frees it). */
    void *stage[DE__VIS_STAGE_COUNT];
    /** The cvars (process-wide; registered by de__vis_shared_create). */
    de_cvar cv_cull_freeze, cv_cull_disable, cv_lod_error_px, cv_lod_bias;
    /** de_render_stats.instances_visible: the frame the sum belongs to. */
    u64 stats_frame;
    bool stats_any;
    /* ---- DENG-233: occlusion (occlusion.c) and the HZB (hzb.c) ---- */
    void *occlusion; /**< occlusion.c's state: per-view visibility flags, the debug kernel */
    de_cvar cv_occlusion, cv_occlusion_reset, cv_occlusion_cut_m, cv_hzb_debug, cv_hzb_single_pass;
    /* ---- DENG-235: the compute-expansion raster path (raster_compute.c) ---- */
    void *raster_compute; /**< raster_compute.c's state: kernels, pipelines, per-view stats */
    /* ---- DENG-239: picking (picking.c) ---- */
    void *pick; /**< picking.c's state: request queues, readback rings, results (lock inside) */
    /* ---- later S13 tasks: append fields here ---- */
    /* ---- DENG-298: the material model registry (de__vis_shared_desc.material_models) ---- */
    const struct de__material_models *material_models;
    /* ---- DENG-303: each view slot's context of the frame being built (de__vis_frame_context):
     * the context (on that graph's frame arena) and the keys that say it is still current ---- */
    struct de__vis_context *frame_context[DE__VIEW_MAX];
    const de_rg *frame_context_graph[DE__VIEW_MAX];
    u64 frame_context_frame[DE__VIEW_MAX];
    de_view frame_context_view[DE__VIEW_MAX];
} de__vis_shared;

/** What the CPU last read back for one view (one to a few frames late). */
typedef struct de__vis_stats {
    bool valid;            /**< a frame of this view was read back */
    u64 frame;             /**< the frame it describes */
    u32 work_items;        /**< phase 1 work items asked for (> the capacity: some dropped) */
    u32 instances_visible; /**< drawn in phase 1 plus phase 2 */
    u32 instances_tested;  /**< live instances with a READY meshlet mesh and a matching layer */
    u32 meshlets;          /**< meshlets of the visible instances' LODs */
    u32 work_capacity;     /**< the capacity of that frame's work list */
    /* DENG-233 */
    u32 instances_visible_phase1; /**< drawn in phase 1 (visible last frame) */
    u32 instances_visible_phase2; /**< drawn in phase 2 (newly visible: passed the HZB test) */
    u32 work_items_phase2;        /**< phase 2 work items asked for */
} de__vis_stats;

/** One view's frame of the visibility buffer: what the stages read and publish. */
typedef struct de__vis_context {
    de__vis_shared *shared;
    de_rg *graph;
    de_arena *arena; /**< the graph's frame arena (pass data) */
    de__gpu_scene *gpu_scene;

    /* The view. */
    de_view view;
    u32 view_slot; /**< 0 .. DE__VIEW_MAX - 1: per-view state and the GPU scene's view slot */
    bool view_new; /**< first frame of this view in its slot: reset per-view history */
    u64 frame_index;
    u64 frames_completed; /**< frames [0, frames_completed) are done on the GPU */
    u32 width, height;    /**< internal resolution */
    de_mat4 view_matrix, proj, view_proj, prev_view_proj;
    de_vec3 camera_position; /**< relative to the render origin */
    de_dvec3 origin;         /**< the render origin */
    const de_render_settings *settings;
    u32 layer_mask; /**< instances need a common bit (~0u: all) */

    /* The GPU scene (DE_ACCESS_SHADER_READ imports; invalid when the table has no buffer). */
    u32 instance_count; /**< instance slots (high-water mark) */
    de_rg_resource scene_instances, scene_meshes, scene_materials, scene_views;
    u32 scene_views_bindless; /**< the scene-constants table (view entry `view_slot`) */

    /* Cvars of this frame. */
    bool cull_freeze, cull_disable;
    f32 lod_error_px;
    i32 lod_bias;

    /* Instance culling (cull_instances.c), per phase (DE__VIS_PHASE_INDEX). */
    de__vis_cull_view cull;  /**< this frame's cull constants */
    u32 cull_view_bindless;  /**< UPLOAD buffer of de__vis_cull_view, de_robuffer */
    u32 cull_view_index;     /**< this frame's entry in it */
    u32 work_capacity;       /**< entries of work_items */
    u32 work_capacity_fixed; /**< tests: a fixed capacity (0 = grow as needed) */
    de_rg_resource work_items[DE__VIS_PHASES];        /**< de__vis_work_item[work_capacity] */
    de_rg_resource cull_args[DE__VIS_PHASES];         /**< de__vis_dispatch_args (INDIRECT) */
    de_rg_resource instance_counters[DE__VIS_PHASES]; /**< DE__VIS_COUNTER_* (raw) */
    de__vis_stats stats;                              /**< the latest read back (late) */

    /* Meshlet culling (cull_meshlets.c, DENG-231). One visible list per view and frame: phase 1
     * fills it from entry 0, phase 2 appends after phase 1 (raster_args[i].first). */
    bool casters_only;               /**< the view draws shadow casters only (no such view yet) */
    u32 visible_capacity;            /**< entries of visible_meshlets */
    de_rg_resource visible_meshlets; /**< de__vis_meshlet[visible_capacity] */
    de_rg_resource meshlet_counters; /**< DE__VIS_MESHLET_COUNTER_* (raw) */
    de_rg_resource raster_args[DE__VIS_PHASES]; /**< de__vis_raster_args (INDIRECT) */
    de__vis_meshlet_stats meshlet_stats;        /**< the latest read back (late) */

    /* ---- DENG-233: depth, HZB and two-phase occlusion (all invalid / 0 without instances) ---- */
    /**
     * The view's depth: DE_FMT_D32_FLOAT at depth_width x depth_height (the internal size),
     * reverse-Z (0 = far). "vis.depth_clear" clears it before phase 1; each raster pass loads
     * and writes it (de_rg_read DE_ACCESS_DEPTH_READ + de_rg_write DE_ACCESS_DEPTH_READ |
     * DE_ACCESS_DEPTH_WRITE). hzb_build reads it (DE_ACCESS_SHADER_READ) after phase 1. Tests
     * may give their own (de__vis_frame_desc.depth_override, any float format).
     */
    de_rg_resource depth;
    u32 depth_width, depth_height;
    /** The view's HZB (hzb.c; imported, R32F, min depth per texel; mip 0 is the previous power
     * of two of the depth size). hzb_build writes it, phase 2 reads it (SHADER_READ). */
    de_rg_resource hzb;
    u32 hzb_width, hzb_height, hzb_mips;
    u32 hzb_bindless; /**< sampled (all mips, Load) */
    /** The view's visibility flags (occlusion.c; imported raw buffer, u32 per instance slot, 1 =
     * visible after phase 2). Phase 1 reads them, phase 2 rewrites slots [0, instance_count). */
    de_rg_resource visibility;
    u32 visibility_bindless;
    u32 visibility_count; /**< slots the last frame wrote; 0 = no history this frame */
    bool occlusion;       /**< r.occlusion: two-phase occlusion on */
    bool occlusion_reset; /**< no history this frame (new view, camera cut, r.occlusion_reset) */

    /* ---- DENG-234: the raster stages' outputs (raster_mesh.c; vis_stages_stub.c picks the path)
     * ---- */
    /** True when `depth` is the frame's D32 depth (a raster target); false for a test's
     * depth_override, which no raster pass may write (the raster stages then add nothing). */
    bool depth_target;
    /** The raster path of this frame (de__vis_raster_choose). */
    u32 raster_path;
    /**
     * The vis buffer: DE_FMT_R32_UINT at width x height, one packed vis ID per pixel
     * (de__vis_id_pack: the entry of visible_meshlets << 7 | the triangle in the meshlet),
     * DE__VIS_ID_EMPTY where nothing was drawn. Made and cleared by de__vis_raster_targets in
     * phase 1; each raster pass loads and writes it (DE_ACCESS_RENDER_TARGET), together with
     * `depth`. Invalid when no raster ran (no instances, no depth target).
     */
    de_rg_resource vis_ids;

    /* ---- DENG-236: the alpha-tested bucket (cull_meshlets.c, raster_alpha.c) ---- */
    /** Entries of the alpha bucket's region of visible_meshlets: [visible_capacity,
     * visible_capacity + visible_alpha_capacity). Its packed vis IDs share the list's index space,
     * so a reader decodes them like the opaque ones. */
    u32 visible_alpha_capacity;
    /** Each phase's raster arguments of the alpha bucket (de__vis_raster_args, INDIRECT): `first`
     * is an entry of the alpha region. Invalid when the phase did not cull. */
    de_rg_resource raster_args_alpha[DE__VIS_PHASES];

    /* ---- DENG-237: material classification (classify.c; all invalid / 0 when it did not run) ----
     */
    /** Entries of scene_materials (the GPU scene's material table count; vis_frame.c). */
    u32 scene_material_count;
    /** The vis IDs as a raw buffer (u32 per pixel, rows `vis_ids_pitch` bytes apart; copied from
     * vis_ids by "vis.classify.copy"): vis_ids has no sampled or storage view, and a uint image in
     * the float-typed bindless tables fails Vulkan validation. DE_ACCESS_SHADER_READ for readers
     * (resolve, DENG-238). */
    de_rg_resource vis_ids_buffer;
    u32 vis_ids_pitch;
    /** Tiles of classify_tile x classify_tile pixels: classify_tiles_x x classify_tiles_y. */
    u32 classify_tile, classify_tiles_x, classify_tiles_y, classify_tile_count;
    /**
     * Per list (DE__VIS_CLASSIFY_LISTS: material model m < DE__GS_MATERIAL_MODELS, then
     * DE__VIS_CLASSIFY_BACKGROUND), the tiles that hold it: raw u32 entries, list k at entry
     * k * classify_tile_count, each tile packed as x | y << 16 (DE__VIS_CLASSIFY_TILE_*), in no
     * particular order. A tile appears once in the list of each model it holds, and in the
     * background list when a pixel holds no ID.
     */
    de_rg_resource classify_tiles;
    /** Per list, a DeVisDispatchArgs (16 bytes, list k at byte 16 k; INDIRECT): one group per
     * tile of the list (de_vis_dispatch_args), count = the list's length. */
    de_rg_resource classify_args;
    /** Per tile, its model mask (bit m: model m; raw u32 at entry y * classify_tiles_x + x). */
    de_rg_resource classify_masks;

    /* ---- DENG-239: picking (picking.c) ---- */
    /** The view's output size in pixels (its viewport): pick coordinates are in it. Equal to the
     * internal size when the desc gives none. */
    u32 output_width, output_height;

    /* ---- DENG-238: material resolve (resolve.c; gbuffer.h has the layout; invalid when it did not
     * run) ---- */
    /** The thin G-buffer at width x height, indexed DE__GB0 .. DE__GB_LIGHTING (gbuffer.h):
     * GB0..GB2, motion vectors and the HDR lighting target (emissive). Left in
     * DE_ACCESS_SHADER_WRITE; readers declare their access (STORAGE | SAMPLED | COPY_SRC). */
    de_rg_resource gbuffer[5];
    /** Tests (de__vis_resolve_set_debug): RGBA32F world position (xyz, 1) and UV0 / UV1 per
     * pixel of a resolved surface. Invalid otherwise. */
    de_rg_resource resolve_debug_pos, resolve_debug_uv;
    /* ---- later S13 tasks: append fields here ---- */
} de__vis_context;

/** de__vis_frame_build inputs. */
typedef struct de__vis_frame_desc {
    de__gpu_scene *gpu_scene;
    de_view view;
    u32 view_slot;
    u64 frame_index;
    u64 frames_completed;
    u32 width, height;
    de_mat4 view_matrix, proj, prev_view_proj; /**< prev zero: the current view_proj */
    de_vec3 camera_position;
    de_dvec3 origin;
    const de_render_settings *settings;
    u32 layer_mask;          /**< 0: all layers */
    u32 work_capacity_fixed; /**< tests: 0 = automatic */
    /** DENG-233, tests: the phase-1 depth comes from this resource of the graph (a float texture
     * the caller wrote, of the given size) instead of the cleared D32 depth and raster. */
    de_rg_resource depth_override;
    u32 depth_override_width, depth_override_height;
    /** DENG-239: the view's output size (pick coordinates); 0: the internal size. */
    u32 output_width, output_height;
} de__vis_frame_desc;

/** de__vis_shared_create settings. */
typedef struct de__vis_shared_desc {
    de_gpu_device *device;
    const de_allocator *allocator; /**< NULL = the heap */
    de_renderer *renderer;         /**< NULL for tests */
    /** DENG-298: the material model registry whose game models the resolve dispatches (their
     * resolve pipelines); NULL = the built-in models only. Must outlive the vis frame. */
    const struct de__material_models *material_models;
} de__vis_shared_desc;

/**
 * Creates the vis frame's state and runs every stage's init. Without engine shaders the state is
 * made but `enabled` is false (logged once). Main thread.
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT; DE_ERR_OUT_OF_MEMORY; a stage init's error.
 */
de_result de__vis_shared_create(const de__vis_shared_desc *desc, de__vis_shared **out);
/**
 * DENG-303: the context de__vis_frame_build made for view `view` in graph `g` in frame
 * `frame_index` (a later stage of the same view graph reads it: lighting), or NULL when that frame
 * built none (the frame is off, the view has no camera). Render thread, while `g` is being built.
 */
struct de__vis_context *de__vis_frame_context(const de__vis_shared *s, de_view view, const de_rg *g,
                                              u64 frame_index);
/** Runs every stage's shutdown and frees `s` (GPU objects through deferred destruction). */
void de__vis_shared_destroy(de__vis_shared *s);
/**
 * One view's frame: fills a context on `g`'s frame arena and calls the stages (header). Between
 * de__rg_begin and de__rg_finalize, after the GPU scene's passes. `*out` (may be NULL) gets the
 * context, NULL when the frame is off.
 */
de_result de__vis_frame_build(de__vis_shared *s, de_rg *g, const de__vis_frame_desc *d,
                              de__vis_context **out);

/* ---------- Engine shaders ---------- */

/** A compute pipeline of one entry point. */
typedef struct de__vis_kernel {
    de_gpu_pipeline pipeline;
    u32 threadgroup[3];
    u32 push_bytes;
} de__vis_kernel;

/**
 * Loads entry points `entries[0 .. count)` of engine shader `name` (a line of
 * shaders/engine_shaders.cmake) for this device with de__engine_shader_load. Free `*out` with
 * de__engine_shader_code_free once the pipelines are made. DE_ERR_UNSUPPORTED (error set, with the
 * reason) when this build has no code for the backend. Main thread.
 */
de_result de__vis_shader_load(de__vis_shared *s, const char *name, const char *const *entries,
                              u32 count, de__engine_shader_code *out);
/**
 * The compute pipeline of entry `entry` of `code`, named `name`. `threadgroup` is the entry's
 * [numthreads] (the reflection must agree); `push_bytes` the size of its push-constant struct (the
 * reflection's, when larger, wins).
 */
de_result de__vis_kernel_create(de__vis_shared *s, const de__engine_shader_code *code, u32 entry,
                                const char *name, const u32 threadgroup[3], u32 push_bytes,
                                de__vis_kernel *out);
void de__vis_kernel_destroy(de__vis_shared *s, de__vis_kernel *k);

/* ---------- Stage functions (per renderer: init / shutdown; per view and frame: the stage) ---- */

de_result de__vis_cull_instances_init(de__vis_shared *s);
void de__vis_cull_instances_shutdown(de__vis_shared *s);
void de__vis_cull_instances(de_rg *g, de__vis_context *vc, de__vis_phase phase);

de_result de__vis_cull_meshlets_init(de__vis_shared *s);
void de__vis_cull_meshlets_shutdown(de__vis_shared *s);
void de__vis_cull_meshlets(de_rg *g, de__vis_context *vc, de__vis_phase phase);

de_result de__vis_raster_init(de__vis_shared *s);
void de__vis_raster_shutdown(de__vis_shared *s);
void de__vis_raster(de_rg *g, de__vis_context *vc, de__vis_phase phase);

de_result de__vis_hzb_build_init(de__vis_shared *s);
void de__vis_hzb_build_shutdown(de__vis_shared *s);
void de__vis_hzb_build(de_rg *g, de__vis_context *vc);

de_result de__vis_raster_alpha_init(de__vis_shared *s);
void de__vis_raster_alpha_shutdown(de__vis_shared *s);
/** DENG-236: called after each phase's opaque raster (vis_frame.c), so phase 1's HZB holds the
 * alpha-tested occluders too. */
void de__vis_raster_alpha(de_rg *g, de__vis_context *vc, de__vis_phase phase);

de_result de__vis_classify_init(de__vis_shared *s);
void de__vis_classify_shutdown(de__vis_shared *s);
void de__vis_classify(de_rg *g, de__vis_context *vc);

de_result de__vis_resolve_init(de__vis_shared *s);
void de__vis_resolve_shutdown(de__vis_shared *s);
void de__vis_resolve(de_rg *g, de__vis_context *vc);

/* ---------- Tests ---------- */

/** One `public static const uint` of vis_types.slang or cull_common.slang and its C value. */
typedef struct de__vis_layout_const {
    const char *name;
    u32 value;
} de__vis_layout_const;
const de__vis_layout_const *de__vis_layout_consts(u32 *count);

/* ---------- HZB (hzb.c) and occlusion (occlusion.c), DENG-233 ---------- */

/** HZB reduction ops (hzb_build.slang DE_HZB_OP_*). */
enum { DE__VIS_HZB_OP_MIN = 0u, DE__VIS_HZB_OP_MAX = 1u };
/** Most HZB mips (mip 0 at most 2^15 wide). */
#define DE__VIS_HZB_MAX_MIPS 16u

/** The HZB size for a source of `w` x `h`: the previous power of two in each dimension (at least
 * 1), and its mip count (a full chain). */
void de__vis_hzb_size(u32 w, u32 h, u32 *hzb_w, u32 *hzb_h, u32 *mips);
/** An HZB to build: a texture of the graph (an import, or a transient: R32F, `mips` mips,
 * STORAGE | SAMPLED) and its size. Its storage views are resolved when the passes execute. */
typedef struct de__vis_hzb_target {
    de_rg_resource resource;
    u32 width, height, mips;
} de__vis_hzb_target;
/**
 * Adds the passes that build `t` from `src` (a sampled float texture of the graph, `src_w` x
 * `src_h`, read DE_ACCESS_SHADER_READ) with reduction `op`: "vis.hzb.level0", then either
 * "vis.hzb.reduce<n>" (6 mips per dispatch, `single_pass`) or one "vis.hzb.mip<n>" per mip.
 * Main use: the hzb_build stage; tests and S15 call it directly.
 */
void de__vis_hzb_add_passes(de__vis_shared *s, de_rg *g, de_rg_resource src, u32 src_w, u32 src_h,
                            const de__vis_hzb_target *t, u32 op, bool single_pass);
/** Before the stages (vis_frame.c): makes the view's HZB the size of its depth and fills the
 * context's HZB fields. */
void de__vis_hzb_prepare(de_rg *g, de__vis_context *vc);
/** Before the stages (vis_frame.c): the view's visibility flags (grown with the instance table,
 * history copied), the cut detection and the context's occlusion fields. */
void de__vis_occlusion_prepare(de_rg *g, de__vis_context *vc);
/** occlusion.c's state (vis_frame.c create / destroy). */
de_result de__vis_occlusion_init(de__vis_shared *s);
void de__vis_occlusion_shutdown(de__vis_shared *s);
/**
 * r.hzb_debug: a pass that draws HZB mip `mip` of `vc`'s view into a new RGBA16F texture of the
 * internal size, returned (invalid when there is no HZB). The renderer makes it the view's colour.
 */
de_rg_resource de__vis_hzb_debug(de_rg *g, de__vis_context *vc, u32 mip);

/** The latest stats cull_instances read back for view slot `slot` (zeroed when none). */
de__vis_stats de__vis_view_stats(const de__vis_shared *s, u32 slot);

/** The `public static const uint` values of meshlet_cull.slang (DENG-231). */
const de__vis_layout_const *de__vis_meshlet_layout_consts(u32 *count);
/** The latest stats cull_meshlets read back for view slot `slot` (zeroed when none). */
de__vis_meshlet_stats de__vis_view_meshlet_stats(const de__vis_shared *s, u32 slot);
/** Test overrides of meshlet culling (zero: none). */
typedef struct de__vis_meshlet_debug {
    u32 capacity_fixed; /**< a fixed visible-list capacity (0: grow as needed) */
    bool casters_only;  /**< force de__vis_context.casters_only */
} de__vis_meshlet_debug;
void de__vis_meshlet_set_debug(de__vis_shared *s, const de__vis_meshlet_debug *d);

/* ---------- Raster (DENG-234: raster_mesh.c; DENG-235: the compute path) ---------- */

/** The raster paths (r.raster_path; de__vis_context.raster_path). */
typedef enum de__vis_raster_path {
    DE__VIS_RASTER_NONE = 0, /**< nothing draws (no path available) */
    DE__VIS_RASTER_MESH,     /**< mesh shaders (raster_mesh.c, DENG-234) */
    DE__VIS_RASTER_COMPUTE   /**< compute expansion (DENG-235) */
} de__vis_raster_path;
/**
 * The path of this frame: r.raster_path (auto, mesh, compute); `auto` and `mesh` pick the mesh
 * path when the device has mesh shaders (de_gpu_caps.mesh_shaders) and its pipeline was made,
 * else the compute path. vis_stages_stub.c dispatches on it.
 */
de__vis_raster_path de__vis_raster_choose(const de__vis_shared *s);
/**
 * The vis buffer of `vc` (de__vis_context.vis_ids): made once per frame (later calls return it)
 * and cleared to DE__VIS_ID_EMPTY by "vis.vis_ids_clear". Invalid without a depth target or
 * instances. Every raster path calls it before its first pass.
 */
de_rg_resource de__vis_raster_targets(de_rg *g, de__vis_context *vc);
/** The mesh path (raster_mesh.c): state, and one phase's passes ("vis.raster.mesh[.phase2]"). */
de_result de__vis_raster_mesh_init(de__vis_shared *s);
void de__vis_raster_mesh_shutdown(de__vis_shared *s);
void de__vis_raster_mesh(de_rg *g, de__vis_context *vc, de__vis_phase phase);
/** True when the mesh path can draw on this device (its pipeline exists). */
bool de__vis_raster_mesh_ready(const de__vis_shared *s);

/** Mesh-shader groups of one dispatch at most (vis_mesh.slang DE_VIS_MESH_GROUPS_MAX; 2^22, the
 * total-group limit of D3D12 and VK_EXT_mesh_shader). More visible meshlets take several. */
#define DE__VIS_MESH_GROUPS_MAX (1u << 22)
/** Mesh dispatches a phase records for a visible list of `capacity` entries (at least 1). */
u32 de__vis_mesh_dispatch_count(u32 capacity);
/** The groups (x, y) of mesh dispatch `k` of a phase of `count` meshlets (cs_raster_mesh_split:
 * (1, 1) for a dispatch past the count, whose one group draws nothing). */
void de__vis_mesh_dispatch_groups(u32 count, u32 k, u32 *x, u32 *y);
/** The `public static const uint` values of vis_decode.slang and vis_mesh.slang (DENG-234). */
const de__vis_layout_const *de__vis_raster_layout_consts(u32 *count);

/* ---------- Alpha-tested raster (DENG-236: raster_alpha.c) ---------- */

/** The `public static const uint` values of vis_alpha.slang. */
const de__vis_layout_const *de__vis_raster_alpha_layout_consts(u32 *count);
/** Alpha-bucket entries at least (the region grows from what was read back, as the list). */
#define DE__VIS_ALPHA_MIN_CAPACITY 4096u
/** Pipelines raster_alpha.c made (tests: the opaque paths' pipelines carry no alpha state). */
typedef struct de__vis_raster_alpha_info {
    bool mesh;    /**< the mesh-shader alpha pipeline exists */
    bool compute; /**< the compute path's alpha draw pipeline exists */
    u32 sampler;  /**< the bindless index of the sampler the alpha test uses */
} de__vis_raster_alpha_info;
de__vis_raster_alpha_info de__vis_raster_alpha_get_info(const de__vis_shared *s);

/* ---------- Compute-expansion raster (raster_compute.c), DENG-235 ---------- */

/** Byte offsets of the expansion counters (vis_expand.slang DE_VIS_EXPAND_COUNTER_*; a raw
 * 16-byte buffer, both phases). */
#define DE__VIS_EXPAND_COUNTER_TRIANGLES 0u  /**< kept triangles asked for (> capacity: dropped) */
#define DE__VIS_EXPAND_COUNTER_PHASE1    4u  /**< triangle slots stored after phase 1 */
#define DE__VIS_EXPAND_COUNTER_TESTED    8u  /**< triangles of the expanded meshlets */
#define DE__VIS_EXPAND_COUNTER_MESHLETS  12u /**< meshlets expanded */
#define DE__VIS_EXPAND_COUNTER_BYTES     16u
/** ExpandParams.flags: per-triangle culling (r.raster_tri_cull). */
#define DE__VIS_EXPAND_TRI_CULL 1u
/** Threads per group of cs_expand (one per triangle of a meshlet). */
#define DE__VIS_EXPAND_GROUP 128u
/** Default of r.raster_compute_max_tris, and the largest value it takes. */
#define DE__VIS_RASTER_COMPUTE_MAX_TRIS_DEFAULT (1u << 24)
#define DE__VIS_RASTER_COMPUTE_MAX_TRIS_LIMIT   (1u << 26)

/** The compute path's stages (vis_stages_stub.c's raster switch calls them). */
de_result de__vis_raster_compute_init(de__vis_shared *s);
void de__vis_raster_compute_shutdown(de__vis_shared *s);
void de__vis_raster_compute(de_rg *g, de__vis_context *vc, de__vis_phase phase);

/** What the CPU last read back of one view's expansion (one to a few frames late). */
typedef struct de__vis_raster_compute_stats {
    bool valid;
    u64 frame;
    u32 triangles; /**< kept triangles asked for, both phases (> capacity: some not drawn) */
    u32 phase1;    /**< triangle slots stored by phase 1 */
    u32 tested;    /**< triangles of the expanded meshlets, both phases */
    u32 meshlets;  /**< meshlets expanded, both phases */
    u32 capacity;  /**< triangle slots of that frame's buffers */
} de__vis_raster_compute_stats;
/** The latest stats raster_compute read back for view slot `slot` (zeroed when none). */
de__vis_raster_compute_stats de__vis_view_raster_compute_stats(const de__vis_shared *s, u32 slot);
/** Test overrides of the compute path (zero: none). */
typedef struct de__vis_raster_compute_debug {
    u32 capacity_fixed; /**< a fixed triangle capacity (0: grow as needed), still at most
                             r.raster_compute_max_tris */
} de__vis_raster_compute_debug;
void de__vis_raster_compute_set_debug(de__vis_shared *s, const de__vis_raster_compute_debug *d);
/** The `public static const uint` values of vis_expand.slang. */
const de__vis_layout_const *de__vis_raster_compute_layout_consts(u32 *count);

/* ---------- Material classification (classify.c), DENG-237 ---------- */

/** Lists of classify_tiles: one per material model, then the background list. */
#define DE__VIS_CLASSIFY_LISTS      (DE__GS_MATERIAL_MODELS + 1u)
#define DE__VIS_CLASSIFY_BACKGROUND DE__GS_MATERIAL_MODELS
/** A tile entry: x in the low 16 bits, y in the high 16. */
#define DE__VIS_CLASSIFY_TILE_BITS 16u
/** Threads per group of cs_classify (one group per tile). */
#define DE__VIS_CLASSIFY_GROUP 64u
/** The model of a material entry as classification reads it: its `model` when the slot names a
 * READY entry of the table, else DE__GS_MATERIAL_MODEL_STANDARD (the default material). */
u32 de__vis_classify_model(const de__gs_material *materials, u32 material_count, u32 slot);
/** The `public static const uint` values of classify.slang. */
const de__vis_layout_const *de__vis_classify_layout_consts(u32 *count);

/* ---------- Picking (picking.c), DENG-239 ---------- */

/** Requests per view and frame (de_view_request_pick). */
#define DE__VIS_PICK_MAX 16u
/** Frames a result stays after it became available. */
#define DE__VIS_PICK_KEEP_FRAMES 8u
/** Results kept at once: every view's requests of DE__VIS_PICK_KEEP_FRAMES frames. */
#define DE__VIS_PICK_RESULTS (DE__VIEW_MAX * DE__VIS_PICK_MAX * DE__VIS_PICK_KEEP_FRAMES)
/** One result as the pick kernel writes it (pick.slang DePickResult, 16 bytes). */
typedef struct de__vis_pick_gpu_result {
    u32 entity_lo, entity_hi, triangle, hit;
} de__vis_pick_gpu_result;
DE_STATIC_ASSERT(sizeof(de__vis_pick_gpu_result) == 16, "DePickResult is 16 bytes");

/** Picking's state (always made, also when the frame is off: requests then never complete). The
 * kernel is loaded when the frame is on. Called by de__vis_shared_create / _destroy. */
de_result de__vis_pick_init(de__vis_shared *s);
void de__vis_pick_shutdown(de__vis_shared *s);
/**
 * The pick stage (vis_frame.c, after phase 2's alpha raster, before classify): takes the view's
 * queued requests, maps them to internal pixels and adds "vis.pick.copy" (the vis IDs into a raw
 * buffer), "vis.pick" (one 16-thread group: vis ID -> visible meshlet -> instance -> entity bits,
 * triangle) and "vis.pick.readback" (into the view's readback ring). No pass without requests.
 * Also collects completed frames' results.
 */
void de__vis_pick(de_rg *g, de__vis_context *vc);
/** de_view_request_pick on the vis frame (main thread). False when the request was dropped. */
bool de__vis_pick_request(de__vis_shared *s, de_view v, u32 x, u32 y, u64 id);
/** de_view_pick_result on the vis frame (main thread). */
bool de__vis_pick_result(de__vis_shared *s, u64 id, u64 *entity_bits, u32 *triangle);
/** Counters for tests. */
typedef struct de__vis_pick_stats {
    u64 requested;  /**< requests queued */
    u64 dropped;    /**< requests over DE__VIS_PICK_MAX (or stale views) */
    u64 dispatched; /**< requests that reached a pick pass */
    u64 completed;  /**< results stored */
    u64 lost;       /**< requests whose frame was overwritten before it completed */
    u32 warnings;   /**< drop warnings logged */
} de__vis_pick_stats;
de__vis_pick_stats de__vis_pick_get_stats(const de__vis_shared *s);
/** The `public static const uint` values of pick.slang. */
const de__vis_layout_const *de__vis_pick_layout_consts(u32 *count);

/* ---------- Material resolve (resolve.c), DENG-238 ---------- */

/** Most sampled-texture indices the feedback buffer covers (u32 each). */
#define DE__VIS_FEEDBACK_MAX 65536u
/** The value of a texture no recorded pixel sampled (feedback.slang DE_FEEDBACK_NONE). */
#define DE__VIS_FEEDBACK_NONE 255u
/** What the CPU last read back of one view's resolve (one to a few frames late). */
typedef struct de__vis_resolve_stats {
    bool valid;
    u64 frame;
    u32 tiles[DE__GS_MATERIAL_MODELS]; /**< tiles each model's dispatch processed */
    const u32 *feedback; /**< finest mip per sampled-texture index, DE__VIS_FEEDBACK_NONE: not
                              sampled (valid until the view's next frame reads back) */
    u32 feedback_count;  /**< entries of `feedback` */
} de__vis_resolve_stats;
de__vis_resolve_stats de__vis_view_resolve_stats(const de__vis_shared *s, u32 slot);
/** Test overrides of the resolve. */
typedef struct de__vis_resolve_debug {
    bool targets;  /**< also write resolve_debug_pos / resolve_debug_uv */
    bool readback; /**< read the feedback and tile counters back (as r.texture_feedback 1) */
} de__vis_resolve_debug;
void de__vis_resolve_set_debug(de__vis_shared *s, const de__vis_resolve_debug *d);
/** The `public static const uint` values of resolve_standard.slang, gbuffer.slang and
 * feedback.slang. */
const de__vis_layout_const *de__vis_resolve_layout_consts(u32 *count);

DE_EXTERN_C_END

#endif /* DE_RENDER_PASSES_VIS_VIS_STAGES_H */
