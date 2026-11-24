/* DENG-837 (DENG-206 AC1, SPEC §12.13): the physical sky at sunrise, noon, sunset and night at
 * latitude 45 degrees on day 80 (the March equinox). The production renderer draws a ground slab
 * and four stone columns (sky/assets/), so the view has a lit G-buffer and the sky stage draws
 * where the depth is the far plane. There is no DeEnvironment: de_sky_attach() draws the sky and
 * drives the scene's directional light as the sun (or the moon). Each scene sets the time of day,
 * the camera's heading and pitch and a fixed EV100. Fixed frame count, no auto exposure, no
 * temporal effect, AgX tone mapping. The flow is the base_tier scenes' (base_tier_scenes.c). */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "golden.h"
#include "cooked_assets.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_debug.h>
#include <dengine/de_ecs.h>
#include <dengine/de_platform.h>
#include <dengine/de_reflect.h>
#include <dengine/de_render.h>
#include <dengine/de_task.h>

#include "render/components_internal.h"
#include "render/renderer_internal.h"
#include "render/passes/vis/vis_stages.h"
#include "render/views_internal.h"
#include "render/world/render_world.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SKY_PI           3.14159265358979323846
#define SKY_LOAD_WAIT_NS (300ull * 1000000000ull) /* the first run cooks every asset */
#define SKY_FRAME_WAIT_NS (120ull * 1000000000ull)
#define SKY_FRAMES       48u /* rendered frames after the scene is in */

typedef struct sky_opts {
    f32 hours;       /**< DeSky.time_of_day_hours */
    f32 heading_deg; /**< camera heading: 0 north (+y), 90 east (+x), 180 south */
    f32 pitch_deg;   /**< camera pitch above the horizon */
    f32 ev100;
    f32 moon_phase; /**< 0: the real moon of the date (DeSky.moon_phase -1); else the forced phase */
    /* What the sky must report for this time (de_sky_get_state), checked before the capture. */
    f32 sun_elev_min, sun_elev_max;
    bool expect_moon_light;
    const char *scene; /**< the scene asset; NULL: scenes/sky_ground */
    const char *mesh;  /**< a second model the scene uses (DENG-838); NULL: none */
    /* DENG-838: keep r.ambient_fallback at its default (the sky lights the IBL anyway). */
    bool keep_ambient;
} sky_opts;

#if DE_GOLDEN_COOK

typedef struct sky_run {
    de_golden_ctx *ctx;
    de_task_system *tasks;
    de_type_registry *types;
    de_world *world;
    de_asset_system *assets;
    de_renderer *r;
    de_sky *sky;
    de_gpu_texture target;
    de_gpu_buffer rb;
    de_gpu_fence fence;
    u64 fence_value;
    de_view view;
    de_load_handle model_load, mesh_load, scene_load;
    char asset_pack[256];
    u32 pitch;
    u8 *pixels;
} sky_run;

static void sky_cvar(const char *name, const char *value) {
    const de_cvar cv = de_cvar_find(name);
    if (DE_HANDLE_VALID(cv))
        (void)de_cvar_set(cv, value);
    de_error_clear();
}

static const char *sky_err(void) {
    const de_error *e = de_error_last();
    return e != NULL ? e->message : "?";
}

static const char *sky_backend_dir(de_gpu_backend b) {
    switch (b) {
    case DE_GPU_METAL:
        return "metal";
    case DE_GPU_VULKAN:
        return "vulkan";
    case DE_GPU_D3D12:
        return "d3d12";
    default:
        return "other";
    }
}

static bool sky_mount(de_golden_ctx *ctx) {
    static bool mounted;
    if (mounted)
        return true;
    if (de_fs_mount("skygold", DE_GOLDEN_SKY_ASSETS, 0, 0) != DE_OK ||
        de_fs_mount("skygoldcache", DE_GOLDEN_SKY_CACHE, DE_MOUNT_WRITABLE, 0) != DE_OK) {
        de_golden_fail(ctx, "mounting the sky assets: %s", sky_err());
        return false;
    }
    mounted = true;
    return true;
}

/* One frame: the asset update, PostUpdate (the sky system), Extract and the kick (r.pipelined 0).
 * dt is 0: nothing moves. */
static bool sky_frame(sky_run *s) {
    de_asset_system_update(s->assets, NULL);
    de_result r = de_world_run_schedule(s->world, DE_SCHEDULE_POST_UPDATE, 0.0f, 0.0f);
    if (r == DE_OK)
        r = de_world_run_schedule(s->world, DE_SCHEDULE_EXTRACT, 0.0f, 0.0f);
    if (r == DE_OK)
        r = de__render_extract_and_kick(s->r, s->world);
    if (r != DE_OK) {
        de_golden_fail(s->ctx, "sky frame: %s", sky_err());
        return false;
    }
    return true;
}

static bool sky_wait(sky_run *s, de_load_handle h, const char *what, u64 t0) {
    de_load_state st = DE_LOAD_PENDING;
    while (de_time_now_ns() - t0 < SKY_LOAD_WAIT_NS) {
        st = de_asset_wait(s->assets, h, 200ull * 1000000ull);
        if (st == DE_LOAD_READY || st == DE_LOAD_FAILED || st == DE_LOAD_PARTIAL ||
            st == DE_LOAD_CANCELLED)
            break;
        if (!sky_frame(s))
            return false;
    }
    if (st == DE_LOAD_READY)
        return true;
    de_load_info info;
    memset(&info, 0, sizeof info);
    (void)de_asset_status(s->assets, h, &info);
    de_golden_fail(s->ctx, "loading %s: %s", what,
                   info.first_error != DE_OK ? info.first_error_msg : "timed out");
    return false;
}

/* Reads the view's target (left in DE_ACCESS_SHADER_READ) into s->pixels. */
static bool sky_read(sky_run *s) {
    de_golden_ctx *ctx = s->ctx;
    de_gpu_cmd *c = de_gpu_cmd_begin(ctx->device, DE_QUEUE_GRAPHICS, "sky readback");
    if (c == NULL) {
        de_golden_fail(ctx, "readback command list: %s", sky_err());
        return false;
    }
    de_gpu_barrier b = {
        .texture = s->target, .before = DE_ACCESS_SHADER_READ, .after = DE_ACCESS_COPY_SRC};
    de_gpu_cmd_barriers(c, &b, 1);
    de_gpu_cmd_copy_texture_to_buffer(c, s->rb, 0, s->pitch, s->target, 0, 0);
    b.before = DE_ACCESS_COPY_SRC;
    b.after = DE_ACCESS_SHADER_READ;
    de_gpu_cmd_barriers(c, &b, 1);
    const u64 v = ++s->fence_value;
    const de_gpu_submit sub = {.cmds = &c, .cmd_count = 1, .signal = s->fence, .signal_value = v};
    if (de_gpu_queue_submit(ctx->device, DE_QUEUE_GRAPHICS, &sub) != DE_OK ||
        de_gpu_fence_wait(ctx->device, s->fence, v, SKY_FRAME_WAIT_NS) != DE_OK) {
        de_golden_fail(ctx, "readback: %s", sky_err());
        return false;
    }
    const u8 *src = (const u8 *)de_gpu_buffer_map(ctx->device, s->rb);
    if (src == NULL) {
        de_golden_fail(ctx, "readback map: %s", sky_err());
        return false;
    }
    memcpy(s->pixels, src, (size_t)s->pitch * ctx->height);
    return true;
}

static void sky_close(sky_run *s) {
    if (s->r != NULL)
        de__render_world_wait_idle(s->r);
    de_sky_detach(s->sky); /* before the renderer: it unregisters its view stage */
    if (s->assets != NULL && DE_HANDLE_VALID(s->scene_load))
        de_asset_release(s->assets, s->scene_load);
    if (s->assets != NULL && DE_HANDLE_VALID(s->model_load))
        de_asset_release(s->assets, s->model_load);
    if (s->assets != NULL && DE_HANDLE_VALID(s->mesh_load))
        de_asset_release(s->assets, s->mesh_load);
    if (s->assets != NULL)
        de_asset_system_destroy(s->assets);
    if (de_golden_pack_cleanup(s->asset_pack) != DE_OK)
        de_golden_fail(s->ctx, "cleaning the temporary asset pack: %s", sky_err());
    if (s->r != NULL)
        de_renderer_destroy(s->r);
    if (s->world != NULL)
        de_world_destroy(s->world);
    if (s->types != NULL)
        de_type_registry_destroy(s->types);
    if (s->tasks != NULL)
        de_task_system_destroy(s->tasks);
    de_gpu_device *d = s->ctx->device;
    if (DE_HANDLE_VALID(s->fence))
        de_gpu_fence_destroy(d, s->fence);
    if (DE_HANDLE_VALID(s->rb))
        de_gpu_buffer_destroy(d, s->rb);
    if (DE_HANDLE_VALID(s->target))
        de_gpu_texture_destroy(d, s->target);
    free(s->pixels);
    sky_cvar("r.pipelined", "1");
}

/* The camera's rotation: pitch about +X (from looking down -Z), then heading about +Z. Pitch 90
 * looks north along +y; heading turns clockwise seen from above (90: east, +x). */
static de_quat sky_camera_rotation(f32 heading_deg, f32 pitch_deg) {
    const f64 p = (90.0 + (f64)pitch_deg) * SKY_PI / 180.0 * 0.5;
    const f64 y = -(f64)heading_deg * SKY_PI / 180.0 * 0.5;
    const f64 sx = sin(p), cx = cos(p), sz = sin(y), cz = cos(y);
    return (de_quat){(f32)(cz * sx), (f32)(sz * sx), (f32)(sz * cx), (f32)(cz * cx)};
}

static f32 sky_azimuth_deg(de_vec3 d) {
    f64 a = atan2((f64)d.x, (f64)d.y) * 180.0 / SKY_PI;
    return (f32)(a < 0.0 ? a + 360.0 : a);
}

/* The mean sRGB colour of a w x h block centred on the column fraction `fx`, the row `y0`. */
static void sky_block_mean(const sky_run *s, f32 fx, u32 y0, u32 w, u32 h, f32 out[3]) {
    const u32 cx = (u32)(fx * (f32)s->ctx->width);
    const u32 x0 = cx > w / 2u ? cx - w / 2u : 0u;
    f64 sum[3] = {0, 0, 0};
    u32 n = 0;
    for (u32 y = y0; y < y0 + h && y < s->ctx->height; ++y)
        for (u32 x = x0; x < x0 + w && x < s->ctx->width; ++x, ++n)
            for (u32 k = 0; k < 3u; ++k)
                sum[k] += s->pixels[(size_t)y * s->pitch + (size_t)x * 4u + k];
    for (u32 k = 0; k < 3u; ++k)
        out[k] = n != 0u ? (f32)(sum[k] / n) : 0.0f;
}

static void sky_run_scene(de_golden_ctx *ctx, const sky_opts *o) {
    if (!ctx->caps->base_tier_ok) {
        de_golden_skip(ctx, "below the Base tier (the renderer needs it): %s",
                       ctx->caps->base_tier_reason);
        return;
    }
    if (ctx->format != DE_FMT_RGBA8_UNORM || (ctx->width * 4u) % 256u != 0u) {
        de_golden_fail(ctx, "scenes.toml: the sky scenes need format rgba8 and a width that is a "
                            "multiple of 64");
        return;
    }
    if (!sky_mount(ctx))
        return;
    sky_run s;
    memset(&s, 0, sizeof s);
    s.ctx = ctx;
    (void)de_cvar_register(
        &(de_cvar_desc){.name = "r.rg_gpu_timers", .type = DE_CVAR_BOOL, .default_number = 0});
    sky_cvar("r.rg_gpu_timers", "0");
    s.pitch = ctx->width * 4u;
    const size_t bytes = (size_t)s.pitch * ctx->height;
    s.pixels = (u8 *)calloc(bytes, 1);
    de_fp_env_init_thread();
    char cache[96];
    snprintf(cache, sizeof cache, "skygoldcache:/%s", sky_backend_dir(ctx->backend));
    if (!de_path_exists(cache))
        (void)de_dir_create(cache);
    if (s.pixels == NULL ||
        de_task_system_create(&(de_task_system_desc){.worker_count = 2}, &s.tasks) != DE_OK ||
        de_type_registry_create(NULL, &s.types) != DE_OK ||
        de_world_create(&(de_world_desc){.types = s.types, .tasks = s.tasks}, &s.world) != DE_OK ||
        de_asset_system_create(&(de_asset_system_desc){.mode = DE_GOLDEN_ASSET_MODE,
                                                       .tasks = s.tasks,
                                                       .io_threads = 1,
                                                       .cache_dir = cache,
                                                       .staging_ring_bytes = 16u << 20,
                                                       .types = s.types},
                               &s.assets) != DE_OK ||
        de_renderer_create(
            &(de_renderer_desc){.device = ctx->device, .assets = s.assets, .types = s.types},
            &s.r) != DE_OK) {
        de_golden_fail(ctx, "setting up the engine: %s", sky_err());
        sky_close(&s);
        return;
    }
    const de__vis_shared *vis = de__views_vis(s.r);
    if (vis == NULL || !vis->enabled) {
        de_golden_skip(ctx, "the vis frame is off in this build: %s",
                       vis != NULL ? vis->why : "no vis frame");
        sky_close(&s);
        return;
    }
    de_renderer_attach(s.r, s.world);
    sky_cvar("r.pipelined", "0");
    /* No environment: the ambient term is the constant fallback; 0 keeps the night dark and the
     * picture down to the sky and its driven light (the sky as ambient is DENG-838). */
    if (!o->keep_ambient)
        sky_cvar("r.ambient_fallback", "0");
    if (ctx->backend == DE_GPU_VULKAN)
        sky_cvar("r.rg_record_cost", "1048576"); /* as base_tier_scenes.c (DENG-235, DENG-427) */
    de_render_settings rs = de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_MEDIUM);
    rs.render_scale = 1;
    rs.dynamic_res_target_ms = 0;
    rs.upscaler = DE_UPSCALE_NONE;
    rs.shadows = rs.ao = rs.reflections = rs.gi = rs.volumetrics = rs.sss = DE_QUALITY_OFF;
    rs.rt_shadows = rs.rt_ao = rs.rt_reflections = rs.rt_gi = rs.rt_refraction = false;
    de_renderer_set_settings(s.r, &rs);

    s.target = de_gpu_texture_create(
        ctx->device, &(de_gpu_texture_desc){.format = DE_FMT_RGBA8_SRGB,
                                            .width = ctx->width,
                                            .height = ctx->height,
                                            .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED |
                                                     DE_USAGE_COPY_SRC,
                                            .name = "sky golden"});
    s.rb = de_gpu_buffer_create(ctx->device, &(de_gpu_buffer_desc){.size = (u64)bytes,
                                                                   .usage = DE_USAGE_COPY_DST,
                                                                   .memory = DE_MEM_READBACK,
                                                                   .name = "sky readback"});
    s.fence = de_gpu_fence_create(ctx->device, 0);
    if (DE_HANDLE_VALID(s.target))
        s.view = de_view_create(s.r, &(de_view_desc){.target = s.target, .name = "sky"});
    if (!DE_HANDLE_VALID(s.target) || !DE_HANDLE_VALID(s.rb) || !DE_HANDLE_VALID(s.fence) ||
        !DE_HANDLE_VALID(s.view)) {
        de_golden_fail(ctx, "the sky view: %s", sky_err());
        sky_close(&s);
        return;
    }
    if (de_asset_manifest_load(s.assets, "skygold:/assets.toml") != DE_OK ||
        de_golden_pack_assets(s.assets, "skygold:/", s.asset_pack) != DE_OK) {
        de_golden_fail(ctx, "the sky assets: %s", sky_err());
        sky_close(&s);
        return;
    }
    const u64 t0 = de_time_now_ns();
    s.model_load = de_asset_load(s.assets, de_asset_id_from("mesh/box"), 0);
    if (!sky_wait(&s, s.model_load, "mesh/box", t0)) {
        sky_close(&s);
        return;
    }
    if (o->mesh != NULL) {
        s.mesh_load = de_asset_load(s.assets, de_asset_id_from(o->mesh), 0);
        if (!sky_wait(&s, s.mesh_load, o->mesh, t0)) {
            sky_close(&s);
            return;
        }
    }
    const char *scene_name = o->scene != NULL ? o->scene : "scenes/sky_ground";
    const de_asset_id scene_id = de_asset_id_from(scene_name);
    s.scene_load = de_asset_load(s.assets, scene_id, 0);
    if (!sky_wait(&s, s.scene_load, scene_name, t0)) {
        sky_close(&s);
        return;
    }
    de_entity cam = DE_ENTITY_NULL;
    if (de_prefab_instantiate(s.assets, s.world, de_asset_scene(s.assets, scene_id),
                              DE_ENTITY_NULL, &cam) != DE_OK) {
        de_golden_fail(ctx, "instantiating %s: %s", scene_name, sky_err());
        sky_close(&s);
        return;
    }
    const DeRenderComponents *ids = de__render_components(s.world);
    DeCamera *camera = ids != NULL && de_entity_alive(s.world, cam)
                           ? (DeCamera *)de_entity_get(s.world, cam, ids->camera)
                           : NULL;
    const de_component c_transform = de_component_find(s.world, "DeTransform");
    DeTransform *cam_xf = camera != NULL && DE_HANDLE_VALID(c_transform)
                              ? (DeTransform *)de_entity_get(s.world, cam, c_transform)
                              : NULL;
    if (camera == NULL || cam_xf == NULL) {
        de_golden_fail(ctx, "%s: the first root has no DeCamera or DeTransform", scene_name);
        sky_close(&s);
        return;
    }
    camera->view = s.view;
    camera->exposure_ev100 = o->ev100;
    camera->tonemapper = de_tonemapper_builtin(s.r, DE_TM_AGX);
    cam_xf->rotation = sky_camera_rotation(o->heading_deg, o->pitch_deg);

    /* The sky: latitude 45, day 80, the reference turbidity, the real moon. */
    const de_component c_sky = de_component_find(s.world, "DeSky");
    const de_entity sky_e = de_entity_spawn(s.world);
    DeSky *sky = DE_HANDLE_VALID(c_sky) ? (DeSky *)de_entity_add(s.world, sky_e, c_sky) : NULL;
    if (sky == NULL) {
        de_golden_fail(ctx, "adding DeSky: %s", sky_err());
        sky_close(&s);
        return;
    }
    *sky = (DeSky){.time_of_day_hours = o->hours,
                   .latitude_deg = 45.0f,
                   .turbidity = 2.0f,
                   .day_of_year = 80.0f,
                   .moon_phase = o->moon_phase > 0.0f ? o->moon_phase : -1.0f,
                   .ground_albedo = 0.3f,
                   .drive_sun = true};
    if (de_sky_attach(s.world, s.r, &s.sky) != DE_OK) {
        de_golden_fail(ctx, "de_sky_attach: %s", sky_err());
        sky_close(&s);
        return;
    }

    for (u32 i = 0; i < SKY_FRAMES; ++i)
        if (!sky_frame(&s)) {
            sky_close(&s);
            return;
        }
    de_sky_state st;
    if (!de_sky_get_state(s.sky, &st)) {
        de_golden_fail(ctx, "the sky has no state after %u frames", SKY_FRAMES);
        sky_close(&s);
        return;
    }
    printf("  sky %.2f h: sun elev %.2f az %.1f, moon elev %.2f az %.1f lit %.2f, light %.4g lx "
           "(%s)\n",
           (f64)o->hours, (f64)st.sun_elevation_deg, (f64)sky_azimuth_deg(st.sun_direction),
           (f64)st.moon_elevation_deg, (f64)sky_azimuth_deg(st.moon_direction),
           (f64)st.moon_illuminated_fraction, (f64)st.light_illuminance,
           st.light_is_moon ? "moon" : "sun");
    if (st.sun_elevation_deg < o->sun_elev_min || st.sun_elevation_deg > o->sun_elev_max)
        de_golden_fail(ctx, "sun elevation %.2f outside [%.1f, %.1f]",
                       (f64)st.sun_elevation_deg, (f64)o->sun_elev_min, (f64)o->sun_elev_max);
    if (st.light_is_moon != o->expect_moon_light)
        de_golden_fail(ctx, "the driven light is the %s", st.light_is_moon ? "moon" : "sun");
    if (!sky_read(&s)) {
        sky_close(&s);
        return;
    }
    /* The sky drew: the top rows are not the view's clear or debug colour. By day the zenith side
     * is blue (b > r); at night it is dark. */
    f32 top[3];
    sky_block_mean(&s, 0.5f, 4u, 64u, 16u, top);
    printf("  sky top-centre sRGB mean %.1f %.1f %.1f\n", (f64)top[0], (f64)top[1], (f64)top[2]);
    if (o->sun_elev_min > 0.0f && !(top[2] > top[0] + 8.0f))
        de_golden_fail(ctx, "the daytime sky is not blue at the top (%.0f %.0f %.0f)",
                       (f64)top[0], (f64)top[1], (f64)top[2]);
    if (o->sun_elev_min > 0.0f && top[2] < 40.0f)
        de_golden_fail(ctx, "the daytime sky is dark at the top (%.0f)", (f64)top[2]);
    const de_gpu_buffer up =
        de_golden_upload_buffer(ctx, s.pixels, (u64)bytes, DE_USAGE_COPY_SRC, "sky pixels");
    if (DE_HANDLE_VALID(up)) {
        de_golden_target_barrier(ctx, DE_ACCESS_COPY_DST);
        de_gpu_cmd_copy_buffer_to_texture(ctx->cmd, ctx->target, 0, 0, up, 0, s.pitch);
    }
    sky_close(&s);
}

#else /* !DE_GOLDEN_COOK */

static void sky_run_scene(de_golden_ctx *ctx, const sky_opts *o) {
    (void)o;
    de_golden_skip(ctx, "the sky goldens cook their assets (DE_GOLDEN_COOK)");
}

#endif

DE_GOLDEN_SCENE(sky_sunrise) {
    sky_run_scene(ctx, &(sky_opts){.hours = 6.5f,
                                   .heading_deg = 90.0f,
                                   .pitch_deg = 12.0f,
                                   .ev100 = 12.0f,
                                   .sun_elev_min = 2.0f,
                                   .sun_elev_max = 12.0f});
}
DE_GOLDEN_SCENE(sky_noon) {
    sky_run_scene(ctx, &(sky_opts){.hours = 12.0f,
                                   .heading_deg = 180.0f,
                                   .pitch_deg = 22.0f,
                                   .ev100 = 14.5f,
                                   .sun_elev_min = 44.0f,
                                   .sun_elev_max = 46.5f});
}
DE_GOLDEN_SCENE(sky_sunset) {
    sky_run_scene(ctx, &(sky_opts){.hours = 17.5f,
                                   .heading_deg = 270.0f,
                                   .pitch_deg = 12.0f,
                                   .ev100 = 12.0f,
                                   .sun_elev_min = 2.0f,
                                   .sun_elev_max = 12.0f});
}
DE_GOLDEN_SCENE(sky_night) {
    sky_run_scene(ctx, &(sky_opts){.hours = 21.0f,
                                   .heading_deg = 125.0f,
                                   .pitch_deg = 12.0f,
                                   .ev100 = -3.0f,
                                   .moon_phase = 0.5f,
                                   .sun_elev_min = -60.0f,
                                   .sun_elev_max = -18.0f,
                                   .expect_moon_light = true});
}
/* DENG-840: the aerial perspective. Afternoon, looking north (the sun behind-left), 25 m up, just
 * above the horizon: columns from 200 m to 24 km fade toward the horizon colour with the distance. */
DE_GOLDEN_SCENE(sky_aerial_perspective) {
    sky_run_scene(ctx, &(sky_opts){.hours = 15.5f,
                                   .heading_deg = 0.0f,
                                   .pitch_deg = 2.0f,
                                   .ev100 = 14.0f,
                                   .sun_elev_min = 15.0f,
                                   .sun_elev_max = 35.0f,
                                   .scene = "scenes/sky_aerial"});
}
/* DENG-838 (DENG-206 AC4): with no HDRI the sky is the IBL environment. Noon, looking south at a
 * chrome sphere (the sky's prefiltered strip: blue above, the sunlit ground below) and a white one
 * (its SH9 irradiance on the side away from the sun). r.ambient_fallback stays at its default: the
 * sky's environment replaces it. */
DE_GOLDEN_SCENE(sky_ibl_sphere) {
    sky_run_scene(ctx, &(sky_opts){.hours = 12.0f,
                                   .heading_deg = 180.0f,
                                   .pitch_deg = -4.0f,
                                   .ev100 = 14.5f,
                                   .sun_elev_min = 44.0f,
                                   .sun_elev_max = 46.5f,
                                   .scene = "scenes/sky_ibl",
                                   .mesh = "mesh/sphere",
                                   .keep_ambient = true});
}
