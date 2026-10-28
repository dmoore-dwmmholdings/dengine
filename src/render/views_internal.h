/*
 * views_internal.h: views and the per-view frame pipeline (SPEC §12.4, §12.5, §12.9, §20.1;
 * DENG-176). Internal to de_render; include as "render/views_internal.h".
 *
 * A view is one camera's rendering: the main view (made with the renderer), split-screen views,
 * render-to-texture views and, later, shadow views and editor viewports. Each view owns its render
 * graph (so its compiled-graph cache stays warm), its settings and its internal resolution
 * (viewport pixels x render_scale). views.c builds every view's graph each frame from the
 * pass-registration list below; passes/composite.c scales each view's output into its viewport.
 *
 * Later renderer sprints add their frame-pipeline stages here: S13 visibility buffer, S14
 * lighting, S15/S16 effects, S18 post, S19 pixel art. The paths of this file and views.c are
 * referenced by those plans: do not rename them.
 *
 * ---- Pass registration (internal; the public game hook API is de_render_add_hook)
 * ----------------
 *
 * A stage registers a build callback with a §12.5 slot and an order inside the slot
 * (de__view_register_stage). Each frame, every view's graph is built by calling the registered
 * stages in slot order, then by `order`, then in registration order. de_render_add_hook callbacks
 * are merged into the same sequence at fixed points:
 *
 *     DE_HOOK_AFTER_GBUFFER      end of DE__VIEW_SLOT_VISIBILITY
 *     DE_HOOK_AFTER_LIGHTING     end of DE__VIEW_SLOT_RT (direct lighting and RT effects done)
 *     DE_HOOK_AFTER_TRANSPARENT  end of DE__VIEW_SLOT_TRANSPARENT
 *     DE_HOOK_BEFORE_TONEMAP     DE__VIEW_SLOT_POST, before the stages of order
 *                                DE__VIEW_ORDER_TONEMAP (the tonemapper registers there)
 *     DE_HOOK_AFTER_TONEMAP      DE__VIEW_SLOT_POST, after the stages of order
 * DE__VIEW_ORDER_TONEMAP DE_HOOK_UI                 DE__VIEW_SLOT_OUTPUT, after the stages of order
 * DE__VIEW_ORDER_UI
 *
 * Hooks at the same point run in the order they were added. A stage that wants to run before or
 * after a hook point picks an `order` below or above it.
 *
 * Until S13 the only stage is views.c's own "view.debug_clear" (DE__VIEW_SLOT_VISIBILITY, order
 * INT32_MIN): it clears the view's colour to a debug colour hashed from the view's name.
 *
 * ---- One frame (de__views_render, on the thread in de_renderer_render_frame)
 * ---------------------
 *
 *   1. Sync: copy view changes made on the main thread (create, destroy, settings, hooks, stages)
 *      under the views lock. Destroyed views are retired: their graph and output texture are
 *      released only once the frame fence shows that the last frame using them completed.
 *   2. Bind cameras: each camera of the render world (de__render_world_current) renders through
 *      DeCamera.view (zero: the main view); the first camera in Extract order wins. A view without
 *      a camera is skipped.
 *   3. For each active view, by `order` (then creation order): its effective settings (the view's
 *      own or the renderer's, then its render_scale, clamped to the device and to the renderer's
 *      tier), its viewport in pixels of its output (the swapchain / offscreen target, or its
 *      `target`), its internal size, its projection (reverse-Z: de_m4_perspective_rz,
 *      de_m4_ortho_rz). Its graph: import its output texture (DE__VIEW_OUTPUT_FORMAT, the internal
 *      size), run the stages and hooks, then
 *        - render-to-texture: composite the output into the target's viewport; the target ends the
 *          frame in DE_ACCESS_SHADER_READ, so a later view (higher `order`) or a material can
 *          sample it in the same frame;
 *        - the output ends in DE_ACCESS_SHADER_READ (step 4 samples the swapchain views').
 *      Finalize, compile (cache hit on a steady frame), execute.
 *   4. The output graph (the renderer's frame graph): clear the swapchain image or offscreen
 *      target, composite every swapchain view into its viewport (bilinear), leave the target in
 *      its final access; it signals the frame fence.
 *
 * Every graph submits on the renderer's device in that order, so queue order makes a producer
 * view's work visible to a later consumer. No heap allocation per frame once warm (fixed view and
 * stage tables; graphs and pipelines are created when a view or a format first appears).
 *
 * Threads: de_view_* and de_render_add_hook on the main thread (they take the views lock);
 * de__views_render and the stage callbacks on the render thread. A stage callback declares
 * passes; the passes' execute callbacks follow the render graph's rules (rg_internal.h).
 */
#ifndef DE_RENDER_VIEWS_INTERNAL_H
#define DE_RENDER_VIEWS_INTERNAL_H

#include <dengine/de_base.h>
#include <dengine/de_gpu.h>
#include <dengine/de_math.h>
#include <dengine/de_memory.h>
#include <dengine/de_render.h>
#include "render/view_hidden.h"

DE_EXTERN_C_BEGIN

struct de__rw_view;
struct de__render_world_frame;

/** Views a renderer can have at once (the main view included). */
#define DE__VIEW_MAX 32u
/** Stages the registration list holds. */
#define DE__VIEW_MAX_STAGES 64u
/** de_render_add_hook callbacks a renderer holds. */
#define DE__VIEW_MAX_HOOKS 32u
/** Bytes of a view name (with the NUL); longer names are cut. */
#define DE__VIEW_NAME_MAX 32u
/** Format of a view's output texture: HDR scene colour (tonemapping is S18). */
#define DE__VIEW_OUTPUT_FORMAT DE_FMT_RGBA16_FLOAT
/** Far plane of orthographic views, in metres (perspective views have no far plane). */
#define DE__VIEW_ORTHO_FAR 100000.0f

/** The frame-pipeline slots of SPEC §12.5, in execution order. */
typedef enum de__view_slot {
    DE__VIEW_SLOT_VISIBILITY,  /**< GPU culling, visibility buffer, material classify/resolve */
    DE__VIEW_SLOT_LIGHTING,    /**< clustered lights, direct lighting with shadows, IBL */
    DE__VIEW_SLOT_RT,          /**< RT effects and their denoising (RT tier) */
    DE__VIEW_SLOT_TRANSPARENT, /**< transparency and transmission */
    DE__VIEW_SLOT_VOLUMETRICS, /**< volumetrics, sky, water, particles */
    DE__VIEW_SLOT_POST,        /**< upscaling/AA, DoF, motion blur, bloom, exposure, tonemap */
    DE__VIEW_SLOT_OUTPUT,      /**< UI composite and output */
    DE__VIEW_SLOT_COUNT
} de__view_slot;

/** Order of the tonemapper inside DE__VIEW_SLOT_POST (DE_HOOK_BEFORE/AFTER_TONEMAP sit there). */
#define DE__VIEW_ORDER_TONEMAP 0
/** Order of the engine's UI composite inside DE__VIEW_SLOT_OUTPUT (DE_HOOK_UI runs after it). */
#define DE__VIEW_ORDER_UI 0

/**
 * What a stage gets while one view's graph is built. Read-only except `color`.
 */
/** Sprite snapping grid of one successfully rendered view frame. Absolute camera
 * coordinates keep prior grids valid when the render origin is rebased. */
typedef struct de__pixel_snap_grid {
    de_dvec3 camera;
    f64 texel;
    de_plane_2d plane;
    bool enabled;
} de__pixel_snap_grid;
typedef struct de__view_frame {
    de_renderer *renderer;
    de_view view;
    de_rg *graph;
    const char *name; /**< the view's name */
    /** The render-world buffer of this frame and the camera bound to the view. */
    const struct de__render_world_frame *world;
    const struct de__rw_view *camera;
    const de_render_settings *settings; /**< effective (view or renderer, then clamped) */
    u64 frame_index;                    /**< the renderer's frame number */
    /** Internal resolution: the viewport in pixels x render_scale, at least 1 x 1. */
    u32 width, height;
    /** The viewport in pixels of the view's output target (x, y, w, h). */
    u32 viewport_px[4];
    /** Matrices (camera-relative render-origin space; SPEC §12.4). `prev_*` are last frame's
     * (equal to the current ones in a view's first frame). */
    de_mat4 view_matrix, proj, view_proj, prev_view_proj;
    /** The view's output (de_rg import, DE__VIEW_OUTPUT_FORMAT), at the internal size; ends the
     * frame in DE_ACCESS_SHADER_READ. */
    de_rg_resource output;
    /**
     * The colour later stages draw into. Starts as `output`. A stage that renders into a texture
     * of its own (a transient, another size) sets it; after the last stage views.c composites
     * `color` into `output` when they differ.
     */
    de_rg_resource color;
    de_rg_resource
        initial_color; /**< Original internal/output target before debug/post replacements. */
    de_gpu_format color_format;
    /** DENG-239: de_view_set_layer_mask (0: every layer). */
    u32 layer_mask;
    u32 excluded_light_mask; /**< Complement of the public selection; zero keeps legacy fixtures. */
    de_view_display display;
    const de__view_hidden *hidden;
    de_gpu_buffer hidden_buffer;
    bool probe_capture; /**< Fixed radiance capture: no temporal effects or probes. */
    const de_pixel_art_settings *pixel_art;
    de_vec2 pixel_residual;
    de__pixel_snap_grid pixel_grid, prev_pixel_grid;
    u32 output_width, output_height;
} de__view_frame;

/** A stage's build callback: declares the stage's passes in `vf->graph`. Render thread. */
typedef void (*de__view_stage_fn)(de__view_frame *vf, void *user);
/** Share an already immutable policy (editor Scene views share one list). Main thread. */
de_result de__view_set_hidden_policy(de_renderer *r, de_view v, const de__view_hidden *policy);

/**
 * Adds a stage to the registration list of `r`. `name` is copied (for messages). Main thread,
 * never from inside a stage. Takes effect from the next frame.
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (NULL `r` or `fn`, bad slot); DE_ERR_OUT_OF_MEMORY (the
 *         list is full, DE__VIEW_MAX_STAGES).
 */
de_result de__view_register_stage(de_renderer *r, de__view_slot slot, i32 order, const char *name,
                                  de__view_stage_fn fn, void *user);
/** Removes every stage registered with (`fn`, `user`). Main thread. */
void de__view_unregister_stage(de_renderer *r, de__view_stage_fn fn, void *user);

/** Main-thread override for a generic radiance capture view. NULL disables it. */
void de__view_capture_camera(de_renderer *r, de_view view, const struct de__rw_view *camera);
/* ---------- renderer.c calls ---------- */

/** Creates the views of `r` (the main view, the debug-clear stage, the composite). */
de_result de__views_create(de_renderer *r);
/** Releases every view (the GPU must be idle) and the composite. NULL members are skipped. */
void de__views_destroy(de_renderer *r);
/**
 * Steps 1 to 4 of the frame above for frame `frame_index`: every view's graph, then the output
 * graph into `target` (left in `final_access`), which signals the frame fence with
 * `frame_index + 1`. Every graph is submitted even when one fails; the frame fence is always
 * signalled and `target` always ends in `final_access`.
 * @return DE_OK, or the first error (logged).
 */
de_result de__views_render(de_renderer *r, de_arena *arena, de_gpu_texture target, u32 final_access,
                           u64 frame_index);
/** The size of the swapchain or offscreen target the output graph renders into (views.c reads
 * it for the viewports). Render thread. */
void de__views_set_output_size(de_renderer *r, u32 width, u32 height, de_gpu_format format);

/* ---------- Composite (passes/composite.c) ---------- */

/** The composite's GPU state of a renderer: shaders, a pipeline per colour format, a sampler. */
typedef struct de__composite de__composite;

/** One source drawn by a composite pass: `src` (a sampled texture of the graph) scaled into the
 * pixel rectangle `rect` (x, y, w, h) of the destination, bilinear. */
typedef struct de__composite_item {
    de_rg_resource src;
    f32 rect[4];
} de__composite_item;

/**
 * Creates the composite of `r`: the linear-clamp sampler and the engine shader "composite"
 * (shaders/common/composite.slang, loaded through de__engine_shader_load; DENG-421). Without code
 * for the backend (a cross build without the build-time cook, DXIL without DXC) `*out` is still
 * made, but de__composite_ready is false and composite passes only clear; the reason is logged
 * once.
 */
de_result de__composite_create(de_renderer *r, de__composite **out);
void de__composite_destroy(de_renderer *r, de__composite *c);
/** True when composite passes can draw (shaders and the sampler exist). */
bool de__composite_ready(const de__composite *c);
/** Where the composite's shaders came from (de__engine_shader_origin; DENG-421). */
u32 de__composite_shader_origin(const de__composite *c);
/**
 * Adds a graphics pass `name` that renders into `dst` (format `dst_format`): cleared to `clear`
 * when it is not NULL, else loaded; then each item drawn into its rectangle. Declares the reads of
 * every source (SHADER_READ) and the write (and, when loading, the read) of `dst` as a render
 * target. `items` is copied onto `arena`. Creates the pipeline of `dst_format` at first use.
 * NULL when the pass could not be added (error set).
 */
de_rg_pass *de__composite_add(de__composite *c, de_rg *g, de_arena *arena, const char *name,
                              de_rg_resource dst, de_gpu_format dst_format, const f32 *clear,
                              const de__composite_item *items, u32 item_count);

/* ---------- Test hooks ---------- */

/** What views.c knows about one view (render side, as of the last frame). */
typedef struct de__view_info {
    bool active;       /**< rendered in the last frame (it had a camera) */
    u32 width, height; /**< internal size */
    u32 viewport_px[4];
    u32 executed; /**< frames whose graph was executed */
    i32 order;
    de_render_settings settings; /**< effective settings of the last frame */
    u32 layer_mask;              /**< DENG-239: de_view_set_layer_mask as of the last frame */
} de__view_info;

/** False for a stale handle or a view the render side has not seen yet. Call while no frame is
 * rendering (unpipelined, or after de__render_world_wait_idle). */
bool de__view_info_get(const de_renderer *r, de_view v, de__view_info *out);
/** Compile counters of the view's graph (de__rg_compile_stats); zero for a stale handle. */
void de__view_graph_stats(const de_renderer *r, de_view v, u64 *compiles, u64 *hits);
/** Retired views whose GPU objects are not released yet. */
u32 de__views_retired_count(const de_renderer *r);
/** Metal pre-record retirement drains. Read only with the render thread idle. */
u64 de__views_resource_drains(const de_renderer *r);
/** The order in which the last frame built the views (handles), and its count. */
u32 de__views_last_order(const de_renderer *r, de_view *out, u32 cap);
/** The debug colour of a view named `name` (the "view.debug_clear" stage; linear RGBA). */
void de__view_debug_color(const char *name, f32 out[4]);
/** The renderer's composite (tests draw with it from a hook). */
de__composite *de__views_composite(const de_renderer *r);

/* ---------- Visibility buffer (passes/vis/vis_frame.c; DENG-229) ---------- */

struct de__vis_shared;
/**
 * Creates the renderer's visibility-buffer frame (src/render/passes/vis/vis_stages.h) and
 * registers its view stage "vis.frame" at (DE__VIEW_SLOT_VISIBILITY, 0). Without shader code for
 * the backend (a build without the build-time cook, DENG-421) the frame is made but off, with a
 * warning. Main thread, from de__views_create.
 */
de_result de__vis_frame_create(de_renderer *r, struct de__vis_shared **out);
/** Releases it (the GPU must be idle). NULL is ignored. */
void de__vis_frame_destroy(de_renderer *r, struct de__vis_shared *s);
/** DENG-239: the renderer's visibility-buffer frame (picking reaches it); NULL before
 * de__views_create. */
struct de__vis_shared *de__views_vis(const de_renderer *r);
/** DENG-304: the renderer's lighting (render/lighting/lighting.h); NULL before de__views_create.
 * The kick snapshots the environment into it; the GPU scene's view stage asks it for exposure. */
struct de__lighting *de__views_lighting(const de_renderer *r);
/** DENG-305: the renderer's forward transparent pass (render/passes/forward/forward.h); NULL before
 * de__views_create or without a vis frame. Its pipelines count in
 * de_render_stats.material_pipelines. */
struct de__forward *de__views_forward(const de_renderer *r);
/** DENG-239: true when `v` is a live view of `r` (takes the views lock). Any thread. */
bool de__view_valid(const de_renderer *r, de_view v);

void de__views_prepare_pixels(de_renderer *r);
void de__views_freeze(de_renderer *r, struct de__render_world_frame *world);

DE_EXTERN_C_END

#endif /* DE_RENDER_VIEWS_INTERNAL_H */
