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

/* ---------- Quality ---------- */
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

typedef struct de_render_settings {
    de_tier tier; /* clamped to device caps */
    de_quality shadows, ao, reflections, gi, volumetrics, sss;
    bool rt_shadows, rt_ao, rt_reflections, rt_gi, rt_refraction;
    de_upscaler upscaler;
    f32 render_scale;          /* 0.33..1.0 */
    f32 dynamic_res_target_ms; /* 0 = off */
    bool path_tracer;          /* reference mode; requires RT tier */
    u32 path_max_bounces;
} de_render_settings;

/* Picks a preset from device caps + a short benchmark (cached in user settings). */
DE_API de_render_settings de_render_settings_auto(de_renderer *r);

/* ---------- Renderer ---------- */
typedef struct de_renderer_desc {
    de_gpu_device *device;
    de_asset_system *assets;
    de_type_registry *types;
    const de_allocator *allocator;
    de_window window;
    u32 frames_in_flight; /* default 2 */
    bool hdr_output;      /* if display supports it */
} de_renderer_desc;

DE_API de_result de_renderer_create(const de_renderer_desc *desc, de_renderer **out);
DE_API void de_renderer_destroy(de_renderer *r);
/* Registers render components + the Extract system into the world. */
DE_API void de_renderer_attach(de_renderer *r, de_world *w);
DE_API void de_renderer_set_settings(de_renderer *r, const de_render_settings *s);
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

/* ---------- Render graph (for engine passes and game-defined passes) ---------- */
typedef struct de_rg de_rg;
typedef struct de_rg_pass de_rg_pass;
DE_HANDLE(de_rg_resource);

typedef void (*de_rg_execute_fn)(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user);

DE_API de_rg_resource de_rg_create_texture(de_rg *g, const de_gpu_texture_desc *desc);
DE_API de_rg_resource de_rg_create_buffer(de_rg *g, const de_gpu_buffer_desc *desc);
DE_API de_rg_resource de_rg_import_texture(de_rg *g, de_gpu_texture t, u32 current_access);
DE_API de_rg_pass *de_rg_add_pass(de_rg *g, const char *name, de_gpu_queue queue,
                                  de_rg_execute_fn fn, void *user);
DE_API void de_rg_read(de_rg_pass *p, de_rg_resource r, u32 access);
DE_API void de_rg_write(de_rg_pass *p, de_rg_resource r, u32 access);
DE_API de_gpu_texture de_rg_texture(const de_rg_pass *p, de_rg_resource r);
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
