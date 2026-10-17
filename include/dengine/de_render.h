/* de_render.h — renderer, render graph, views, materials, lights, 2D, RT tiers, tonemapping.
 * SPEC.md §12. Most game code only touches the ECS components at the bottom. */
#ifndef DE_RENDER_H
#define DE_RENDER_H
#include "de_base.h"
#include "de_math.h"
#include "de_gpu.h"
#include "de_asset.h"
DE_EXTERN_C_BEGIN

typedef struct de_renderer de_renderer;
typedef struct de_world de_world;
typedef struct de_type_registry de_type_registry;
DE_HANDLE(de_view);
DE_HANDLE(de_tonemapper);
DE_HANDLE(de_material_model);

/* ---------- Quality (SPEC §12.8, §3.3; DENG-174) ----------
 * The renderer runs at one of two tiers: Base (raster + screen-space and probe fallbacks) and RT
 * (Base plus ray-traced effects). Each effect has a quality level. A preset is one fixed
 * de_render_settings per (tier, Low..Ultra); any other settings are "Custom". The preset tables
 * are documented in src/render/settings.c.
 */
typedef enum de_quality {
    DE_QUALITY_OFF,
    DE_QUALITY_LOW,
    DE_QUALITY_MEDIUM,
    DE_QUALITY_HIGH,
    DE_QUALITY_ULTRA
} de_quality;
typedef enum de_tier { DE_TIER_BASE, DE_TIER_RT } de_tier;
typedef enum de_upscaler {
    DE_UPSCALE_NONE,
    DE_UPSCALE_TAA,
    DE_UPSCALE_FSR,
    DE_UPSCALE_METALFX,
    DE_UPSCALE_AUTO
} de_upscaler;
typedef enum de_gi_mode { DE_GI_STATIC_PROBES, DE_GI_SCREEN_SPACE, DE_GI_RT } de_gi_mode;

/**
 * What the renderer draws, and how well. de_renderer_set_settings clamps every field to the
 * device (see there); `(de_render_settings){0}` is valid (Base, every effect off, then clamped:
 * render_scale 0.33, one bounce).
 */
typedef struct de_render_settings {
    /** DE_TIER_RT only on a device with ray tracing (hardware or API only) and inline ray
     * queries; otherwise clamped to DE_TIER_BASE. */
    de_tier tier;
    de_quality shadows, ao, reflections, gi, volumetrics, sss;
    /** Ray-traced replacements of the Base effects (SPEC §12.8); all false at DE_TIER_BASE. */
    bool rt_shadows, rt_ao, rt_reflections, rt_gi, rt_refraction;
    de_upscaler upscaler;
    f32 render_scale;          /**< 0.33..1.0 of the output resolution */
    f32 dynamic_res_target_ms; /**< 0 = off */
    bool path_tracer;          /**< reference mode; requires the RT tier */
    u32 path_max_bounces;      /**< 1..64 */
    de_gi_mode gi_mode;        /**< Static volumes, screen-space plus volumes, or reserved RT. */
} de_render_settings;

/**
 * The preset `quality` (DE_QUALITY_LOW .. DE_QUALITY_ULTRA; other values are clamped into that
 * range) of `tier` (an unknown tier gives DE_TIER_BASE). Presets are not clamped to a device:
 * de_renderer_set_settings does that. Pure; any thread.
 */
DE_API de_render_settings de_render_settings_preset(de_tier tier, de_quality quality);
/**
 * True when `s` equals one of the presets field for field ("Custom" otherwise). NULL is not a
 * preset. Pure; any thread.
 */
DE_API bool de_render_settings_is_preset(const de_render_settings *s);
/**
 * The settings the renderer would pick for its device: a preset chosen from the device caps
 * (RT tier + High on discrete GPUs with hardware ray tracing; Base + Medium on Apple M1/M2, the
 * Steam Deck and other integrated GPUs; SPEC R7, DENG-2 Q4), clamped to the device. S20
 * (DENG-276) adds a first-run GPU benchmark behind this call. NULL `r` gives the Base Medium
 * preset.
 */
DE_API de_render_settings de_render_settings_auto(de_renderer *r);

/* ---------- Renderer ----------
 * The renderer owns the device-facing render state: settings and tier, per-frame fences and
 * arenas, the frame graph, the swapchain (or an offscreen target without a window) and the
 * runtime asset types of de_asset.h (texture, mesh, material, model) with their GPU uploads.
 *
 * Cvars (all DE_CVAR_SAVED; changes apply at the next de_renderer_render_frame): `r.tier`
 * (auto, base, rt), `r.quality` (auto, low, medium, high, ultra, custom), `r.shadow_quality`,
 * `r.ao_quality`, `r.reflection_quality`, `r.gi_quality`, `r.volumetric_quality`,
 * `r.sss_quality` (off .. ultra), `r.rt_shadows`, `r.rt_ao`, `r.rt_reflections`, `r.rt_gi`,
 * `r.rt_refraction`, `r.upscaler`, `r.render_scale`, `r.dynamic_res_target_ms`, `r.path_tracer`,
 * `r.path_max_bounces`. Choosing a preset in `r.quality` sets every per-effect value; changing a
 * per-effect cvar makes `r.quality` name the preset the settings now equal, or "custom". The
 * renderer writes the settings it applies (clamped) back to the per-effect cvars. With several
 * renderers in one process, only the first one created reads and writes the cvars.
 */
typedef struct de_renderer_desc {
    de_gpu_device *device; /**< required; must meet the Base tier (de_gpu_evaluate_tier) */
    /** The asset system the renderer registers the texture, mesh, material and model types
     * with (NULL: none). Destroy the asset system before the renderer. */
    de_asset_system *assets;
    de_type_registry *types;       /**< for the render components (DENG-178) */
    const de_allocator *allocator; /**< NULL = de_heap_allocator(); must be thread-safe */
    /** The window to present to (a swapchain is made for it). Zero: the renderer draws into an
     * offscreen target of its own. */
    de_window window;
    u32 frames_in_flight; /**< 0 = 2; clamped to 2..3 */
    bool hdr_output;      /**< if the display supports it (S18; ignored until then) */
} de_renderer_desc;

/**
 * Creates a renderer. The settings start from `r.tier` / `r.quality` and the other cvars
 * ("auto": de_render_settings_auto), clamped to the device.
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (NULL `desc`, `out` or device); DE_ERR_UNSUPPORTED
 *         when the device is below the Base tier (the message gives de_gpu_evaluate_tier's
 *         reasons) or cannot make the swapchain; DE_ERR_ALREADY_EXISTS when the asset system
 *         has the render asset types already (one renderer per asset system); the RHI's and
 *         the allocator's errors. `*out` is NULL on failure.
 */
DE_API de_result de_renderer_create(const de_renderer_desc *desc, de_renderer **out);
/** Waits until the GPU is idle, then releases everything the renderer made. Destroy the asset
 * system first (its unloads use the renderer's uploader), and the device after. NULL is
 * ignored. */
DE_API void de_renderer_destroy(de_renderer *r);
/* Registers render components + the Extract system into the world. */
/**
 * (DENG-175) Also adds the Extract systems (`de.render.extract_views`, `_meshes`, `_lights` in
 * DE_SCHEDULE_EXTRACT) that copy cameras, mesh renderers, lights and the environment into the
 * renderer's render world. Attaching the same world again changes nothing; attaching another
 * world switches the render world to it. One renderer per world. Destroy the renderer before the
 * world (de_renderer_destroy removes its systems). Main thread, not while a schedule runs.
 */
DE_API void de_renderer_attach(de_renderer *r, de_world *w);
/**
 * Requests new settings; they apply at the next de_renderer_render_frame. Clamped to the device:
 * RT tier only with ray tracing (hardware or API only) and inline ray queries; at the Base tier
 * every `rt_*` and `path_tracer` are false; qualities and the upscaler outside their enums
 * become Off and Auto; `render_scale` 0.33..1.0 (NaN: 1.0); `dynamic_res_target_ms` >= 0;
 * `path_max_bounces` 1..64. Writes the cvars (`r.tier`, `r.quality` = the matching preset or
 * "custom", and the per-effect ones). Main thread.
 */
DE_API void de_renderer_set_settings(de_renderer *r, const de_render_settings *s);
/** The settings in effect (clamped), including a request not applied yet. Any thread. */
DE_API de_render_settings de_renderer_get_settings(const de_renderer *r);
/* Called by the host on the render thread after Extract. */
/**
 * (DENG-175) The host does not call it directly: after the Extract schedule it hands the
 * extracted frame to the render thread (cvar `r.pipelined`, default 1), which calls this while
 * the simulation runs the next frame; with `r.pipelined = 0` it runs right after Extract on the
 * main thread. One thread at a time.
 */
DE_API de_result de_renderer_render_frame(de_renderer *r);

/* ---------- Views ----------
 * SPEC §12.4, §12.5, §12.9, §20.1 (DENG-176). A view is one camera's rendering: the main view
 * (made with the renderer), split-screen views, render-to-texture views (in-world screens, editor
 * viewports later) and, later, shadow views. Each view has its own render graph (so its compiled
 * graph stays cached), its own settings and its own internal resolution: its viewport in pixels
 * times its render_scale.
 *
 * Cameras: a DeCamera renders through DeCamera.view (zero: the main view). When several cameras
 * name the same view, the first one Extract copied wins. A view without a camera is skipped.
 * Projections are reverse-Z (de_m4_perspective_rz; de_m4_ortho_rz with a far plane at 100 km),
 * with the aspect ratio of the view's viewport.
 *
 * Each frame the views render in `order` (lower first; equal orders in creation order), then their
 * outputs are scaled (bilinear) into their viewports on the swapchain (or the renderer's offscreen
 * target). A render-to-texture view writes its `target` and leaves it in DE_ACCESS_SHADER_READ, so
 * a view with a higher `order`, or a material, can sample it in the same frame.
 *
 * Until the visibility buffer (S13) a view only clears to a debug colour hashed from its name.
 * Changes made with these functions apply from the next de_renderer_render_frame. Main thread
 * (de_view_color and de_view_size: inside hooks, on the render thread).
 */
typedef enum de_projection { DE_PROJ_PERSPECTIVE, DE_PROJ_ORTHOGRAPHIC } de_projection;

typedef struct de_view_desc {
    /** Zero: the swapchain (or offscreen target). Else a 2D texture with DE_USAGE_RENDER_TARGET |
     * DE_USAGE_SAMPLED that the view renders into (render to texture); it must outlive the view. */
    de_gpu_texture target;
    /** Normalized x, y, w, h of the target (y down); all zero = the whole target. */
    f32 viewport[4];
    i32 order;        /**< render and composition order: lower first */
    bool pixel_art;   /**< stored; S19 renders it (see de_pixel_art_settings) */
    const char *name; /**< copied (31 bytes); NULL or "": "view<n>" */
} de_view_desc;

/**
 * Creates a view. Invalid handle (error context set) when `desc` is NULL, the viewport is not a
 * rectangle inside 0..1, the target is not a live 2D texture with RENDER_TARGET | SAMPLED usage,
 * or 32 views exist already.
 */
DE_API de_view de_view_create(de_renderer *r, const de_view_desc *desc);
/**
 * Destroys a view; the handle is stale at once. Its GPU objects are released once the frames in
 * flight that used them are done. The main view cannot be destroyed (error context set, warning
 * logged); a stale handle is ignored.
 */
DE_API void de_view_destroy(de_renderer *r, de_view v);
/* ---------- Picking (SPEC §20.1; DENG-239) ----------
 * What is under a pixel of a view, read from the view's visibility buffer: the entity (its
 * de_entity bits, as the render world has them) and the triangle. Only visibility-buffer geometry
 * is pickable (opaque and alpha-tested meshes); translucent and forward-only objects are not.
 * Main thread.
 */
/** Triangle of a pick that hit nothing (and of every background pick). */
#define DE_PICK_TRIANGLE_NONE 0xFFFFFFFFu
/**
 * Asks what is under pixel (`x`, `y`) of view `v` in the frame the next de_renderer_render_frame
 * renders. The coordinates are pixels of the view's output (its viewport on its target, origin at
 * the top left); they are mapped to the view's internal resolution (render_scale). At most 16
 * requests per view and frame: more are dropped (a warning in Debug and Dev builds). A stale or
 * invalid view handle is ignored. Reusing an ID forgets the stored result of the earlier request
 * with it. No heap allocation.
 */
DE_API void de_view_request_pick(de_renderer *r, de_view v, u32 x, u32 y, u64 request_id);
/**
 * The result of request `request_id`: true once the frame that rendered it has completed on the
 * GPU (normally one or two frames later, at most three), with `*entity_bits` the entity under the
 * pixel (0: background, or outside the view) and `*triangle` the triangle there, as (meshlet << 7)
 * | triangle in the meshlet, meshlet being the index into the mesh's meshlets in DMSH order (every
 * LOD; DE_PICK_TRIANGLE_NONE for background). Results are kept for 8 frames after they become
 * available. False for an unknown or expired ID, a request still in flight, and requests of a
 * view destroyed before they were rendered. Either pointer may be NULL.
 */
DE_API bool de_view_pick_result(de_renderer *r, u64 request_id, u64 *entity_bits, u32 *triangle);
/**
 * The layers the view draws: an instance is drawn when its DeMeshRenderer.layer_mask shares a
 * bit with `mask`. 0 (the default) means every layer.
 */
DE_API void de_view_set_layer_mask(de_renderer *r, de_view v, u32 mask);

/** The main view, made with the renderer (target zero, the whole viewport, order 0, name
 * "main"). A DeCamera whose `view` is zero renders through it. */
DE_API de_view de_renderer_main_view(const de_renderer *r);
/**
 * Gives the view settings of its own (copied, clamped to the device like
 * de_renderer_set_settings). NULL: the view follows the renderer's settings again (the default).
 * Either way a view never renders above the renderer's tier.
 */
DE_API void de_view_set_settings(de_renderer *r, de_view v, const de_render_settings *s);
/**
 * Sets the view's internal resolution scale, which overrides `render_scale` of its settings
 * (clamped to 0.33..1.0 like the settings). 0, a negative value or NaN: the settings' value again.
 */
DE_API void de_view_set_render_scale(de_renderer *r, de_view v, f32 scale);
/** The settings the view renders with: its own or the renderer's, its render_scale, the
 * renderer's tier as a cap, clamped. The renderer's settings for a stale handle. */
DE_API de_render_settings de_view_get_settings(de_renderer *r, de_view v);

/* ---------- Render graph (for engine passes and game-defined passes) ----------
 * SPEC §12.3 (DENG-171). A de_rg is one frame's graph. Passes declare what they read and write,
 * the queue they run on and an execute callback; the graph derives the dependencies, culls passes
 * whose results nobody uses, and issues every barrier (passes never do, SPEC §12.1).
 *
 * Declaration rules:
 *   - Each write makes a new version of the resource (of the written subresources). A read sees
 *     the latest version written by an earlier pass; passes run in declaration order unless the
 *     compiler reorders independent ones.
 *   - A pass that reads and then writes the same subresources (blending, depth test + write,
 *     read-modify-write storage) declares both: de_rg_read for the old contents and de_rg_write
 *     for the new ones. Read bits in a de_rg_write access without that de_rg_read are an error.
 *     A render target that is blended onto or loaded is read with DE_ACCESS_RENDER_TARGET; the
 *     graph cannot see blending, so without that read an earlier writer can be culled.
 *   - Outputs are imported resources that some pass writes, resources passed to de_rg_export and
 *     passes marked with de_rg_pass_side_effect. Every other pass whose writes do not reach an
 *     output is culled: it never executes, and transients only it uses are never allocated.
 *   - Access bits are de_gpu_access values. Writes: SHADER_WRITE, RENDER_TARGET, DEPTH_WRITE,
 *     COPY_DST, ACCEL_WRITE. Reads: SHADER_READ, DEPTH_READ, INDIRECT, COPY_SRC, ACCEL_READ,
 *     and RENDER_TARGET (the load or blend of a target that the pass also writes).
 *     DE_ACCESS_PRESENT is only a final state (de_rg_export), never a pass access.
 *   - Compute passes cannot use RENDER_TARGET or DEPTH_* access; copy passes only COPY_*.
 *
 * Building is single-threaded: one thread declares a graph. Bad handles, descs and queues are
 * refused in every build; the access rules above are checked in Debug and Dev builds. A refused
 * declaration sets the error context, logs to the `render` category and is dropped. Handles and
 * pass pointers are valid for one frame only.
 */
typedef struct de_rg de_rg;
typedef struct de_rg_pass de_rg_pass;
DE_HANDLE(de_rg_resource);

/**
 * A range of mips and array layers of a texture. A count of 0 means "to the last one", so
 * `(de_rg_subresource){0}` is the whole texture. Buffers are always accessed whole.
 */
typedef struct de_rg_subresource {
    u32 mip_first, mip_count, layer_first, layer_count;
} de_rg_subresource;

/** Records the pass. `cmd` is open on the pass's queue; de_rg_texture / de_rg_buffer give the
 * physical resources. Runs once per frame for each pass that is not culled. */
typedef void (*de_rg_execute_fn)(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user);

/** A transient texture: the graph allocates (and may alias) it for this frame only. `desc` is
 * copied; `transient` is forced on. Invalid handle (error context set) for a bad desc. */
DE_API de_rg_resource de_rg_create_texture(de_rg *g, const de_gpu_texture_desc *desc);
/** A transient buffer (GPU_ONLY memory only), as de_rg_create_texture. */
DE_API de_rg_resource de_rg_create_buffer(de_rg *g, const de_gpu_buffer_desc *desc);
/**
 * An external texture (swapchain image, history buffer) that is in `current_access` now.
 * Importing the same texture twice in one frame returns the first handle. Its mip and layer
 * counts are not known to the graph, so subresource ranges on it are not range-checked.
 */
DE_API de_rg_resource de_rg_import_texture(de_rg *g, de_gpu_texture t, u32 current_access);
/** An external buffer that is in `current_access` now, as de_rg_import_texture. */
DE_API de_rg_resource de_rg_import_buffer(de_rg *g, de_gpu_buffer b, u32 current_access);
/**
 * Marks `r` as a graph output: the passes that produce its final contents are never culled, and
 * the graph leaves it in `final_access` at the end of the frame (DE_ACCESS_NONE: its last
 * access; DE_ACCESS_PRESENT for a swapchain image). An exported transient is not aliased.
 */
DE_API void de_rg_export(de_rg *g, de_rg_resource r, u32 final_access);
/** Names a resource for messages and the graph dump (imports have no name otherwise). The
 * string is copied. */
DE_API void de_rg_set_name(de_rg *g, de_rg_resource r, const char *name);

/** Adds a pass. `name` is copied; `fn` may be NULL (a pass that only orders work). NULL when
 * `queue` is not a de_gpu_queue value. */
DE_API de_rg_pass *de_rg_add_pass(de_rg *g, const char *name, de_gpu_queue queue,
                                  de_rg_execute_fn fn, void *user);
/** The pass has effects outside the graph (readback, a GPU counter): it is never culled. */
DE_API void de_rg_pass_side_effect(de_rg_pass *p);
/** Declares that `p` reads the whole of `r` with `access` (read bits, or RENDER_TARGET). */
DE_API void de_rg_read(de_rg_pass *p, de_rg_resource r, u32 access);
/** Declares that `p` writes the whole of `r` with `access` (write bits; read bits need a
 * matching de_rg_read). */
DE_API void de_rg_write(de_rg_pass *p, de_rg_resource r, u32 access);
/** de_rg_read of a mip/layer range of a texture (an HZB pass reads mip N-1 and writes mip N). */
DE_API void de_rg_read_sub(de_rg_pass *p, de_rg_resource r, de_rg_subresource sub, u32 access);
/** de_rg_write of a mip/layer range of a texture. */
DE_API void de_rg_write_sub(de_rg_pass *p, de_rg_resource r, de_rg_subresource sub, u32 access);
/**
 * The physical texture of `r`. Valid only inside `p`'s execute callback, and only for resources
 * `p` declared; otherwise (checked in Debug and Dev) an invalid handle and an error.
 */
DE_API de_gpu_texture de_rg_texture(const de_rg_pass *p, de_rg_resource r);
/** The physical buffer of `r`, as de_rg_texture. */
DE_API de_gpu_buffer de_rg_buffer(const de_rg_pass *p, de_rg_resource r);

/* Insertion points for custom passes in the per-view graph. */
typedef enum de_rg_hook {
    DE_HOOK_AFTER_GBUFFER,
    DE_HOOK_AFTER_LIGHTING,
    DE_HOOK_AFTER_TRANSPARENT,
    DE_HOOK_BEFORE_TONEMAP,
    DE_HOOK_AFTER_TONEMAP,
    DE_HOOK_UI
} de_rg_hook;
typedef void (*de_rg_hook_fn)(de_rg *g, de_view v, void *user);
/**
 * (DENG-176) Adds `fn` to every view's graph at point `at`: it is called on the render thread
 * while the view's graph is built, with that graph and view, and declares passes there. Where
 * each point sits in the frame pipeline (SPEC §12.5): AFTER_GBUFFER after the visibility and
 * material stages; AFTER_LIGHTING after lighting and RT effects; AFTER_TRANSPARENT after
 * transparency; BEFORE_TONEMAP / AFTER_TONEMAP around the tonemapper in post; UI after the
 * engine's UI composite. Hooks at one point run in the order they were added. At most 32 hooks;
 * a NULL `fn`, an unknown point or a full list is refused (error context set and logged). Main
 * thread; applies from the next frame. There is no removal yet.
 */
DE_API void de_render_add_hook(de_renderer *r, de_rg_hook at, de_rg_hook_fn fn, void *user);
/**
 * Inside a de_render_add_hook callback for view `v` (render thread): the colour target its later
 * passes draw into (a de_rg resource of `v`'s graph, DE_FMT_RGBA16_FLOAT: linear HDR scene colour,
 * at the view's internal size). A pass that
 * draws onto it declares de_rg_read(p, color, DE_ACCESS_RENDER_TARGET) and de_rg_write(...).
 * An invalid handle anywhere else.
 */
DE_API de_rg_resource de_view_color(de_renderer *r, de_view v);
/** Inside a hook for `v`: the view's internal size in pixels; 0 x 0 anywhere else. */
DE_API void de_view_size(de_renderer *r, de_view v, u32 *width, u32 *height);

/* ---------- Tonemappers (pluggable) ---------- */
typedef enum de_builtin_tonemapper {
    DE_TM_AGX,
    DE_TM_ACES,
    DE_TM_PBR_NEUTRAL,
    DE_TM_REINHARD,
    DE_TM_NONE
} de_builtin_tonemapper;
DE_API de_tonemapper de_tonemapper_builtin(de_renderer *r, de_builtin_tonemapper which);
/* Custom: a Slang shader asset implementing ITonemapper + a parameter block. */
DE_API de_tonemapper de_tonemapper_register(de_renderer *r, const char *name, de_shader shader,
                                            u32 param_bytes);

/* ---------- Material models (SPEC §12.6; DENG-298) ----------
 * A material model is a Slang type implementing IMaterialModel
 * (shaders/materials/material_interface.slang: resolve_gbuffer, evaluate, sample, pdf). Its id is
 * what a material's `model` selects; the vis-buffer resolve runs one kernel per model, specialised
 * for that type at compile time. Built-in ids: "standard" 0, "unlit" 1, "sprite" 2, "terrain" 3,
 * "water" 4 (sprite, terrain and water resolve as standard until their sprints). Game models get
 * ids 5 to 31 in registration order: at most 32 models in all, because material classification
 * keeps a screen tile's models in one 32-bit mask.
 */

/**
 * Registers game material model `name` (1 to 31 bytes, unique) from `module`, a READY `shader`
 * asset of `r`'s asset system whose source implements the model and instantiates the resolve
 * kernel for it:
 *
 *     import "visbuffer/resolve_model";  // relative to the source: the engine's shaders/
 *                                         // mounted with the project's sources
 *     struct ToonModel : IMaterialModel { ... }
 *     [shader("compute")] [numthreads(64, 1, 1)]
 *     void cs_resolve_model(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID,
 *                           uniform ResolveParams pc)
 *     { de_resolve_tiles<ToonModel>(group, thread.x, pc); }
 *
 * A type that does not implement IMaterialModel fails the shader's cook (Slang names the missing
 * requirement); a shader without that kernel is rejected here. The model's pipeline is made now;
 * later reloads of the shader do not replace it. Main thread, between frames.
 * @return the model (de_material_model_id gives its id); invalid on failure, with the reason in
 *         the error context: a NULL argument, no asset system, a bad or taken name, the shader not
 *         loaded (DE_ERR_BUSY), a shader without a valid `cs_resolve_model`, or all 32 ids taken
 *         (DE_ERR_BUSY).
 */
DE_API de_material_model de_material_model_register(de_renderer *r, const char *name,
                                                    de_shader module);
/** The model registered as `name` (a built-in name or a game model's); invalid if there is none. */
DE_API de_material_model de_material_model_find(de_renderer *r, const char *name);
/** The id of `model` (what a material's model id holds, 0..31); 0 (standard) when invalid. */
DE_API u32 de_material_model_id(de_material_model model);

/* ===================== ECS components ===================== */

typedef struct DeCamera {
    de_projection projection;
    f32 fov_y;        /* radians, perspective */
    f32 ortho_height; /* meters, orthographic */
    f32 near_z;
    de_view view;       /* zero = main view */
    f32 exposure_ev100; /* NaN = auto exposure */
    de_tonemapper tonemapper;
    f32 tonemap_params[8];
} DeCamera;

typedef struct DeMeshRenderer {
    de_mesh mesh;
    de_material materials[8]; /* per submesh; zero = mesh default */
    u32 layer_mask;
    bool cast_shadows, visible_in_rt;
} DeMeshRenderer;

typedef enum de_light_kind {
    DE_LIGHT_DIRECTIONAL,
    DE_LIGHT_POINT,
    DE_LIGHT_SPOT,
    DE_LIGHT_RECT,
    DE_LIGHT_DISK
} de_light_kind;
typedef struct DeLight {
    de_light_kind kind;
    de_vec3 color; /* linear */
    f32 intensity; /* lux (directional), lumens (local), nits (area) */
    f32 range;     /* meters; 0 = physical falloff cutoff */
    f32 spot_inner, spot_outer;
    de_vec2 area_size;
    f32 source_radius; /* directional: angular radius (radians); local: radius (metres) */
    bool cast_shadows;
} DeLight;

/** Local radiance influence. Extent is half size (sphere uses extent.x), in metres. */
typedef enum de_probe_shape { DE_PROBE_BOX, DE_PROBE_SPHERE } de_probe_shape;
typedef enum de_probe_update {
    DE_PROBE_ONCE,
    DE_PROBE_ON_REQUEST,
    DE_PROBE_EVERY_N_FRAMES
} de_probe_update;
typedef struct DeReflectionProbe {
    de_probe_shape shape;
    de_vec3 extent;
    f32 blend_distance;
    bool box_projection;
    u32 resolution; /**< 0 = 128; otherwise 64, 128 or 256. */
    de_probe_update update;
    u32 update_interval; /**< Frames between periodic updates; 0 = 60. */
    i32 priority;        /**< Higher priority captures and blends first; ties use entity bits. */
    f32 intensity;
    de_blob bake; /**< Optional versioned .deprobe blob; stale settings trigger capture. */
} DeReflectionProbe;
/** Schedule a probe for recapture. Main thread, between Extract kicks. */
DE_API de_result de_render_probe_request_update(de_renderer *r, de_entity entity);

typedef struct DeEnvironment {
    de_texture hdri; /* zero = procedural sky (DeSky) */
    f32 intensity;
    f32 rotation;
} DeEnvironment;

/* Standard material parameters as stored in material assets (glTF + KHR extensions). */
typedef struct de_standard_material {
    de_vec4 base_color;
    de_texture base_color_tex;
    f32 metallic, roughness;
    de_texture metal_rough_tex;
    de_texture normal_tex;
    f32 normal_scale;
    de_texture occlusion_tex;
    de_vec3 emissive;
    f32 emissive_strength;
    de_texture emissive_tex;
    f32 ior, specular;
    de_vec3 specular_color;
    f32 clearcoat, clearcoat_roughness;
    de_texture clearcoat_normal_tex;
    de_vec3 sheen_color;
    f32 sheen_roughness;
    f32 transmission;
    f32 thickness;
    de_vec3 attenuation_color;
    f32 attenuation_distance;
    f32 diffuse_transmission;
    f32 subsurface;
    de_vec3 subsurface_radius;
    f32 anisotropy, anisotropy_rotation;
    f32 iridescence, iridescence_ior, iridescence_thickness_min, iridescence_thickness_max;
    f32 alpha_cutoff;
    u32 alpha_mode; /* opaque, mask, blend */
    bool double_sided, unlit;
    /* DENG-302: KHR_materials_diffuse_transmission's colour factor, linear (cooked default white;
     * a zeroed struct is black). */
    de_vec3 diffuse_transmission_color;
} de_standard_material;

/* ---------- Material instances (SPEC §12.6; DENG-302) ----------
 * Every material is a parameter block in one bindless buffer: a loaded `material` asset (cooked
 * from a model or a `.mat.toml` file), or a runtime instance, a copy of another material for
 * per-entity tweaks. Changing parameters never creates or rebuilds a pipeline: the renderer
 * uploads the changed blocks at the next frame, and a change of the features a material uses
 * (clearcoat 0 -> 0.5) selects another of the shader permutations compiled when the renderer was
 * made (de_render_stats.permutations_per_model, material_permutation_switches). All four are
 * main-thread calls, between frames; the change shows from the next frame on.
 */

/**
 * Sets every parameter of material `m` (READY asset or runtime instance) to `*params`, textures
 * included: the six texture fields replace those slots (zero clears one; the caller keeps those
 * textures loaded); slots de_standard_material has no field for keep their textures. `unlit`
 * switches a built-in material between the Standard and Unlit models. A hot reload of an asset
 * replaces what this set with the cooked values.
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (NULL `r` or `params`, an invalid or stale `m`, an
 *         `alpha_mode` above 2); DE_ERR_BUSY (the asset is not loaded yet).
 */
DE_API de_result de_material_set_params(de_renderer *r, de_material m,
                                        const de_standard_material *params);
/** The parameters of material `m` into `*out` (as de_material_set_params takes them).
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT, DE_ERR_BUSY (not loaded). */
DE_API de_result de_material_get_params(de_renderer *r, de_material m, de_standard_material *out);
/**
 * A runtime instance of READY material `base`: a copy of its parameters, textures and model that
 * de_material_set_params changes without touching `base`. Use it like any material
 * (DeMeshRenderer.materials). It does not keep `base` or its textures loaded. At most 16384
 * instances are alive at once.
 * @return the instance; invalid on failure, with the reason in the error context (NULL `r`, an
 *         invalid `base`, DE_ERR_BUSY for a `base` not loaded yet, DE_ERR_OUT_OF_MEMORY).
 */
DE_API de_material de_material_create_instance(de_renderer *r, de_material base);
/** Destroys runtime instance `m` (its handle becomes stale; instances that still use it draw the
 * default material). Not for asset materials (release their load handles). */
DE_API void de_material_destroy_instance(de_renderer *r, de_material m);

/* ---------- 2D ---------- */
typedef enum de_plane_2d { DE_PLANE_SIDE_XZ, DE_PLANE_TOP_XY } de_plane_2d;

typedef struct DeSprite {
    de_texture texture; /* or atlas */
    de_vec4 uv_rect;    /* atlas region */
    de_vec2 size;       /* meters */
    de_vec2 pivot;      /* 0..1 */
    de_vec4 color;
    de_texture normal_tex, rough_metal_tex, emissive_tex, height_tex;
    f32 thickness;    /* meters, for RT shadows */
    i16 layer, order; /* translucent sorting */
    bool flip_x, flip_y, translucent, cast_shadows;
} DeSprite;

typedef struct de_pixel_art_settings {
    u32 internal_width, internal_height; /* e.g. 480x270 */
    bool integer_scale_only;             /* false = sharp-bilinear fractional */
    bool snap_camera, snap_sprites, smooth_subpixel_scroll;
    de_texture palette_lut; /* optional */
    f32 dither_strength;
} de_pixel_art_settings;
DE_API void de_view_set_pixel_art(de_renderer *r, de_view v, const de_pixel_art_settings *s);
DE_API void de_render_set_2d_plane(de_renderer *r, de_plane_2d plane);

/* ---------- World systems (SPEC §12.13); configured through components ---------- */
typedef struct DeSky {
    f32 time_of_day_hours;
    f32 latitude_deg;
    f32 turbidity;
    bool clouds;
    f32 cloud_coverage;
} DeSky;
typedef struct DeFogVolume {
    de_vec3 extent;
    f32 density;
    de_vec3 albedo;
    f32 anisotropy;
} DeFogVolume;
typedef struct DeDecal {
    de_material material;
    de_vec3 extent;
    f32 angle_fade;
    i32 sort;
} DeDecal;
typedef struct DeParticleSystem {
    de_blob effect;
    bool playing;
    f32 time_scale;
    u32 seed;
} DeParticleSystem;
typedef struct DeTerrain {
    de_blob heightmap;
    de_material layers[8];
    u32 layer_count;
    f32 cell_size;
    f32 height_scale;
} DeTerrain;
typedef struct DeWater {
    de_material material;
    bool ocean;
    de_vec2 wind;
    f32 choppiness;
} DeWater;

/* ---------- Component registration (DENG-178; SPEC §9.1, §20.1) ---------- */

/**
 * Registers the render components above with the world: their reflected types (fields, units,
 * ranges, tooltips) in the world's type registry (de_world_types), the enums `de_projection`
 * and `de_light_kind`, and each type as an ECS component (DE_COMP_SNAPSHOT | DE_COMP_SERIALIZE),
 * so de_component_find(w, "DeCamera") finds it and scenes and prefabs (TOML, DENG-2 Q9) can name
 * it. It also registers the ECS built-in components (de_builtin_components: DeTransform, ...),
 * which every rendered entity has. de_renderer_attach calls it; a tool or a server without a
 * renderer (de-cook) calls it directly, because it needs no GPU.
 *
 * Field flags: every authorable field is DE_FIELD_SERIALIZE | DE_FIELD_SNAPSHOT; every asset
 * handle also DE_FIELD_ASSET_REF, so it is written as its asset ID, never as the handle value;
 * the runtime handles `DeCamera.view` and `DeCamera.tonemapper` are DE_FIELD_SNAPSHOT |
 * DE_FIELD_EDITOR_HIDDEN only (never serialized). Units are "m", "rad", "m/s", "h", "deg",
 * "1/m", "EV100", and "lux|lm|nit" for DeLight.intensity.
 *
 * Defaults: each type has a construct hook, so de_entity_add, de_type_construct and the scene
 * reader start every component (also from zeroed memory) at these values; other fields are 0.
 * - DeCamera: perspective, fov_y 60 degrees (1.0471976 rad), ortho_height 10 m, near_z 0.1 m,
 *   exposure_ev100 NaN (auto exposure).
 * - DeMeshRenderer: layer_mask ~0u, cast_shadows and visible_in_rt true.
 * - DeLight: DE_LIGHT_POINT, white, de_light_default_intensity(DE_LIGHT_POINT), range 0
 *   (physical cutoff), spot_outer pi/4 (spot_inner 0), area_size 1 x 1 m, source_radius
 *   0.05 m, cast_shadows true.
 * - DeEnvironment: intensity 1. DeSprite: uv_rect (0, 0, 1, 1), size 1 x 1 m, pivot (0.5, 0.5),
 *   white, cast_shadows true.
 * - DeSky: 12 h, latitude 45 degrees, turbidity 2, cloud_coverage 0.5.
 * - DeFogVolume: extent 10 x 10 x 10 m, density 0.02 /m, white albedo.
 * - DeDecal: extent 1 x 1 x 1 m, angle_fade 1. DeParticleSystem: playing, time_scale 1.
 * - DeTerrain: cell_size 1 m, height_scale 100 m. DeWater: wind (5, 0) m/s, choppiness 1.
 *
 * Idempotent per world: a second call on the same world (de_renderer_attach after a world
 * reload) changes nothing. Worlds that share a registry share the types. Main thread, not while
 * a schedule runs.
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (`w` NULL); DE_ERR_ALREADY_EXISTS (the registry has
 *         another type of one of the names); the registry's and the ECS's errors.
 */
DE_API de_result de_render_register_components(de_world *w);

/**
 * A physically plausible intensity for a light of `kind`, in its unit (DeLight.intensity):
 * directional 100000 lux (direct sunlight), point and spot 800 lm (a 60 W incandescent bulb),
 * rect and disk 1000 nit. An unknown kind gives the point value. Pure; any thread.
 */
DE_API f32 de_light_default_intensity(de_light_kind kind);

/* ---------- Stats ---------- */
typedef struct de_render_stats {
    f32 gpu_ms, cpu_record_ms;
    u32 instances_visible, meshlets_visible;
    u64 triangles_visible;
    u64 rays_traced;
    u32 pass_count;
    const char *const *pass_names;
    const f32 *pass_gpu_ms;
    /* Extract (DENG-175), written on the main thread at each kick: */
    u32 extract_instances; /**< mesh instances in the render world */
    u32 extract_lights;    /**< lights in the render world */
    f32 extract_cpu_ms;    /**< from taking the render-world buffer to the kick */
    /* GPU scene (DENG-177), written on the thread that renders: */
    u64 gpu_scene_upload_bytes; /**< bytes the last frame uploaded into the scene tables */
    u32 gpu_scene_instances;    /**< instance table slots in use (high-water mark) */
    u32 gpu_scene_lights;       /**< light table slots in use */
    u32 gpu_scene_meshes;       /**< meshes the instances use */
    u32 gpu_scene_materials;    /**< materials the instances use */
    u64 gpu_scene_gpu_bytes;    /**< GPU memory of the scene tables and scratch buffers */
    /* Materials (DENG-302), written at each kick: */
    u32 permutations_per_model[32]; /**< resolve permutations compiled per model id (0 = Standard;
                                       sprite, terrain, water share Standard's) */
    u32 material_permutation_switches; /**< materials whose permutation changed at the last kick */
    u32 material_pipelines; /**< material pipelines created so far (permutations + game models +
                               the forward transparent pass's, DENG-305) */
    u32 material_uploads;   /**< material parameter blocks the last frame uploaded */
    u32 material_instances; /**< live runtime instances (de_material_create_instance) */
    /* Local shadows: summed over active caster-mask groups of this renderer frame. */
    u32 shadow_atlas_occupied_texels; /**< allocated tile texels, including guards */
    u32 shadow_atlas_tiles_rendered;  /**< tile renders in successfully submitted view graphs */
} de_render_stats;
DE_API const de_render_stats *de_renderer_stats(const de_renderer *r);

/** Static diffuse GI volume, in the global transform's rotated local coordinates. Extent is
 * half-size in metres. Zero counts derive max(2,ceil(2*extent/spacing)) (spacing 0 defaults to 2m).
 * Probes are cell-centered with step 2*extent/counts; influence covers the entire extent.
 * Explicit counts must be >=2 on each axis. The baker uses a deterministic whole-grid iteration;
 * bounces 0 defaults to 2, maximum 8. A matching blob skips capture in Shipping too. */
typedef struct DeIrradianceVolume {
    de_vec3 extent;
    f32 spacing;
    u32 counts[3];
    f32 intensity, blend_distance;
    i32 priority;
    u32 bounces;
    de_blob bake;
} DeIrradianceVolume;

DE_EXTERN_C_END
#endif
