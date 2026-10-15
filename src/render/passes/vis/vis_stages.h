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
 *     de__vis_hzb_build(g, vc)                 depth -> HZB (DENG-233)
 *     de__vis_cull_instances(g, vc, PHASE_2)   (DENG-233: what phase 1 rejected, against the HZB)
 *     de__vis_cull_meshlets(g, vc, PHASE_2)
 *     de__vis_raster(g, vc, PHASE_2)
 *     de__vis_raster_alpha(g, vc)              alpha-tested materials (DENG-236)
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
    u32 reserved[8];
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
    /* ---- later S13 tasks: append fields here ---- */
} de__vis_shared;

/** What the CPU last read back for one view (one to a few frames late). */
typedef struct de__vis_stats {
    bool valid;            /**< a frame of this view was read back */
    u64 frame;             /**< the frame it describes */
    u32 work_items;        /**< work items asked for (> the capacity: some were dropped) */
    u32 instances_visible; /**< phase 1 */
    u32 instances_tested;  /**< live instances with a READY meshlet mesh and a matching layer */
    u32 meshlets;          /**< meshlets of the visible instances' LODs */
    u32 work_capacity;     /**< the capacity of that frame's work list */
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
} de__vis_frame_desc;

/** de__vis_shared_create settings. */
typedef struct de__vis_shared_desc {
    de_gpu_device *device;
    const de_allocator *allocator; /**< NULL = the heap */
    de_renderer *renderer;         /**< NULL for tests */
} de__vis_shared_desc;

/**
 * Creates the vis frame's state and runs every stage's init. Without engine shaders the state is
 * made but `enabled` is false (logged once). Main thread.
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT; DE_ERR_OUT_OF_MEMORY; a stage init's error.
 */
de_result de__vis_shared_create(const de__vis_shared_desc *desc, de__vis_shared **out);
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
void de__vis_raster_alpha(de_rg *g, de__vis_context *vc);

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

/** The latest stats cull_instances read back for view slot `slot` (zeroed when none). */
de__vis_stats de__vis_view_stats(const de__vis_shared *s, u32 slot);

DE_EXTERN_C_END

#endif /* DE_RENDER_PASSES_VIS_VIS_STAGES_H */
