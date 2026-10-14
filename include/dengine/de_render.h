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
DE_API de_result de_renderer_render_frame(de_renderer *r);

/* ---------- Views ---------- */
typedef enum de_projection { DE_PROJ_PERSPECTIVE, DE_PROJ_ORTHOGRAPHIC } de_projection;

typedef struct de_view_desc {
    de_gpu_texture target; /* zero = swapchain */
    f32 viewport[4];       /* normalized x, y, w, h; zero = full */
    i32 order;             /* composition order */
    bool pixel_art;        /* see de_pixel_art_settings */
    const char *name;
} de_view_desc;

DE_API de_view de_view_create(de_renderer *r, const de_view_desc *desc);
DE_API void de_view_destroy(de_renderer *r, de_view v);
/* Visibility-buffer picking: entity bits under a pixel, read back next frame. */
DE_API void de_view_request_pick(de_renderer *r, de_view v, u32 x, u32 y, u64 request_id);
DE_API bool de_view_pick_result(de_renderer *r, u64 request_id, u64 *entity_bits, u32 *triangle);

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
DE_API void de_render_add_hook(de_renderer *r, de_rg_hook at, de_rg_hook_fn fn, void *user);

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

/* ---------- Material models (pluggable Slang modules) ---------- */
DE_API de_material_model de_material_model_register(de_renderer *r, const char *name,
                                                    de_shader module);

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
    f32 source_radius; /* soft shadow size */
    bool cast_shadows;
} DeLight;

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
} de_standard_material;

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
} de_render_stats;
DE_API const de_render_stats *de_renderer_stats(const de_renderer *r);

DE_EXTERN_C_END
#endif
