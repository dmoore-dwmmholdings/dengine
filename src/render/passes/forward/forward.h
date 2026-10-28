/*
 * forward.h: the forward transparent pass (SPEC §12.5 step 6; DENG-305). Internal to de_render;
 * include as "render/passes/forward/forward.h".
 *
 *   transparent_sort.c     the per-view transparent list: which materials are forward (the C
 *                          mirror of shaders/forward/forward_routing.slang), the gather from the
 *                          render world and the GPU scene, and the back-to-front sort. Pure.
 *   forward_transparent.c  the stage "forward.transparent" at (DE__VIEW_SLOT_TRANSPARENT, 0): the
 *                          pipeline (made, so pre-warmed, at renderer creation), the optional
 *                          scene-colour snapshot/pyramid and the draws, blended into the view's HDR
 * lighting target before output.basic reads it (DENG-304).
 *
 * Routing: a submesh whose material is forward (READY with DE_MATERIAL_FEATURE_FORWARD in its
 * CPU-derived features: alpha blend, or transmission > 0) never enters the visibility buffer.
 * Meshlet culling (meshlet_cull.slang meshlet_bucket) sends its meshlets to DE__VIS_BUCKET_FORWARD,
 * which is only counted; this pass draws them. The list itself is built on the CPU from the render
 * world each frame (the instance's world bounds against the view's culling frustum and layer mask),
 * sorted back to front by the view-space depth of the bounds centre, ties broken by the entity
 * bits, then by the gather order (instance slot): a total order, so the same scene and camera give
 * the same order every frame. Each listed instance draws LOD 0's meshlets (one instanced draw per
 * instance: DE__FWD_VERTICES vertices per meshlet instance), back faces first, then front faces,
 * when one of its forward materials is double-sided.
 *
 * Threads: create and destroy on the main thread; the stage and de__forward_build on the render
 * thread, one view at a time; pass callbacks on task workers.
 */
#ifndef DE_RENDER_PASSES_FORWARD_FORWARD_H
#define DE_RENDER_PASSES_FORWARD_FORWARD_H

#include <dengine/de_base.h>
#include <dengine/de_gpu.h>
#include <dengine/de_math.h>
#include <dengine/de_memory.h>
#include <dengine/de_render.h>

#include "render/materials/material_models.h"
#include "render/passes/vis/vis_stages.h"
#include "render/scene/gpu_scene.h"

DE_EXTERN_C_BEGIN

struct de__render_world_frame;
struct de__lighting;
struct de__lighting_context;

/* =================================================================================================
 * The shader's constants (forward_transparent.slang; the test forward.layout_match compares)
 * ============================================================================================== */

/** Which faces a draw keeps (ForwardParams.side). */
enum { DE__FWD_SIDE_FRONT = 0u, DE__FWD_SIDE_BACK = 1u };
/** Vertices per meshlet instance: three per triangle slot (DE_VIS_MESHLET_MAX_TRIANGLES). */
#define DE__FWD_VERTICES (3u * 124u)
/** Bytes per pixel of the scene-colour copy (the RGBA16F lighting target's texels). */
#define DE__FWD_SCENE_COLOR_BPP 8u

/** forward_transparent.slang ForwardParams (112 bytes). */
typedef struct de__fwd_params {
    u32 view, entry, scene, view_slot;
    u32 instance, meshlet_first, side, sampler;
    u32 scene_color, scene_color_pitch, width, height;
    u32 scene_depth, scene_depth_pitch, scene_color_levels, refraction;
    /* DeClusterLists */
    u32 lights, dir_lights, grid, list, counters, ltc1, ltc2, ltc_sampler;
    u32 rt_capture, rt_result, rt_path, rt_reserved;
} de__fwd_params;
DE_STATIC_ASSERT(sizeof(de__fwd_params) == 112, "ForwardParams is 112 bytes");

/* =================================================================================================
 * The list (transparent_sort.c)
 * ============================================================================================== */

/** de__transparent_item.flags */
enum {
    DE__FWD_ITEM_DOUBLE_SIDED = 1u, /**< a forward material of it draws both faces: two draws */
    DE__FWD_ITEM_TRANSMISSIVE = 2u, /**< a forward material of it transmits: the scene colour */
    DE__FWD_ITEM_BASE_STANDARD = 4u /**< every forward submesh needs only base lobes */
};

/** One instance of a view's transparent list. */
typedef struct de__transparent_item {
    f32 depth;    /**< view-space depth of its bounds centre (larger: farther) */
    u32 instance; /**< the GPU scene's instance slot (= the render world's) */
    u64 entity;   /**< the first tie-break */
    u32 index;    /**< its place in the gather (the last tie-break) */
    u32 flags;    /**< DE__FWD_ITEM_* */
    u32 participant, participant_item; /**< Zero participant means an ordinary mesh. */
    bool always_on_top;
    u32 meshlet_first, meshlet_count; /**< LOD 0's meshlets */
    i16 layer, order;                 /**< S19: authored sprite order; meshes use zero. */
} de__transparent_item;

/** The C mirror of forward_routing.slang de_material_forward: material entry `m` is READY and its
 * CPU-derived features (DENG-302 de__material_features) have DE__MATERIAL_FEATURE_FORWARD (alpha
 * blend, or transmission > 0). */
static inline bool de__forward_material(const de__gs_material *m) {
    return (m->flags & DE__GS_MATERIAL_READY) != 0u &&
           (m->features & DE__MATERIAL_FEATURE_FORWARD) != 0u;
}

/** Conservative base specialization: unknown models/features retain the full shader. */
static inline bool de__forward_base_material(const de__gs_material *m) {
    const u32 allowed = DE__MATERIAL_FEATURE_FORWARD | DE__MATERIAL_FEATURE_DOUBLE_SIDED |
                        DE__MATERIAL_FEATURE_UNLIT | DE__MATERIAL_FEATURE_ALPHA_MASK;
    return de__forward_material(m) &&
           (m->model == DE__GS_MATERIAL_MODEL_STANDARD || m->model == DE__GS_MATERIAL_MODEL_UNLIT ||
            m->model == DE__GS_MATERIAL_MODEL_SPRITE) &&
           (m->features & ~allowed) == 0u;
}

/** True when `a` draws before `b`: farther first, then the smaller entity, then the smaller index
 * (a total order on distinct indices). NaN depths count as the farthest. */
bool de__transparent_before(const de__transparent_item *a, const de__transparent_item *b);
/** Stable radix sort matching de__transparent_before; disjoint `scratch` holds n items.
 * Uses only the supplied arrays, with no frame heap allocation. */
void de__transparent_sort(de__transparent_item *items, u32 n, de__transparent_item *scratch);

/** What de__transparent_gather reads. */
typedef struct de__transparent_view {
    const de__view_hidden *hidden;
    const de__gpu_scene *gs;
    const struct de__render_world_frame *world;
    const de__vis_cull_view *cull; /**< frustum (when its flags have DE__VIS_CULL_FRUSTUM) */
    u32 layer_mask;                /**< instances need a common bit */
    de_mat4 view_matrix;           /**< render-origin space -> view space (camera along -z) */
} de__transparent_view;

/** What the gather counted. */
typedef struct de__transparent_counts {
    u32 forward_materials;  /**< forward materials among the GPU scene's referenced materials */
    u32 candidates;         /**< live instances with a forward submesh and a drawable mesh */
    u32 listed;             /**< ... that passed the layer mask and the frustum */
    bool allocation_failed; /**< distinguish an empty list from arena exhaustion */
} de__transparent_counts;

/**
 * The view's transparent list, unsorted, in instance-slot order: every live instance with bounds,
 * a READY meshlet mesh and a forward material on one of its submeshes, in the view's layers and
 * frustum. `*out` (NULL when empty) is on `arena`; returns the count (0 also when the arena is
 * full: the error is set). No forward material in the scene: nothing is read past the materials.
 */
u32 de__transparent_gather(const de__transparent_view *v, de_arena *arena,
                           de__transparent_item **out, de__transparent_counts *counts);

/* =================================================================================================
 * The pass (forward_transparent.c)
 * ============================================================================================== */

typedef struct de__forward de__forward;

/** One draw of the pass, in order. */
typedef struct de__forward_draw {
    u32 instance, side, meshlet_first, meshlet_count;
    u32 participant, participant_item;
    bool base_standard;
} de__forward_draw;

/** What one view's pass did this frame (the graph's frame arena). */
typedef struct de__forward_context {
    const de__transparent_item *items; /**< the sorted list */
    u32 count;
    const de__forward_draw *draws;
    u32 draw_count;
    de__transparent_counts counts;
    de_rg_resource target;      /**< the HDR lighting target it blended into */
    de_rg_resource scene_color; /**< the scene-colour pyramid (packed RGBA16F rows), invalid when no
                                     listed material transmits */
    u32 scene_color_pitch;
    de_rg_resource scene_depth;
    u32 scene_depth_pitch, scene_color_levels;
} de__forward_context;

/** Makes the pass's state on `vis`'s device: its pipeline (pre-warmed: no pipeline is made while
 * frames render) and the material sampler. Off (logged) without shader code, or with `vis` or
 * `lighting` off. Main thread. */
de_result de__forward_create_ex(de__vis_shared *vis, struct de__lighting *lighting,
                                de__forward **out);
void de__forward_destroy(de__forward *fw);
bool de__forward_enabled(const de__forward *fw);
/** Pipelines the pass holds (DENG-302's permutation count): 1 when enabled. */
u32 de__forward_pipeline_count(const de__forward *fw);

/**
 * One view's pass: the list from `world` (de__transparent_gather on `vc`'s GPU scene, culling
 * view and matrices), sorted, then "forward.scene_color" (only when a listed material transmits)
 * and "forward.transparent" into `lc`'s lighting target. Adds nothing when the list is empty, the
 * view lit nothing (`lc` NULL or not lit) or has no D32 depth. `*out` (may be NULL) gets the
 * context, NULL when nothing was added. Render thread.
 */
de_result de__forward_build(de__forward *fw, de_rg *g, de__vis_context *vc,
                            struct de__lighting_context *lc,
                            const struct de__render_world_frame *world, de__forward_context **out);

/** Creates the renderer's pass and registers its view stage "forward.transparent" at
 * (DE__VIEW_SLOT_TRANSPARENT, 0). Main thread, from de__views_create. */
de_result de__forward_create(de_renderer *r, de__vis_shared *vis, struct de__lighting *lighting,
                             de__forward **out);

DE_EXTERN_C_END

#endif /* DE_RENDER_PASSES_FORWARD_FORWARD_H */
