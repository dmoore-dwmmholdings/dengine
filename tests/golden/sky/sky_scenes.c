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
#include "render/lighting/lighting.h"
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
    /* DENG-207: froxel volumetric fog. volumetrics High and shadows Medium (the CSM and the local
     * shadow atlas light the fog); the fog volumes are spawned at `fog_pos`. Each fog scene also
     * checks temporal stability: one more frame's readback against the captured one. */
    bool fog;
    u32 fog_count;
    de_vec3 fog_pos[2];
    DeFogVolume fog_volume[2];
    /* DENG-208: volumetric clouds (volumetrics High) at DeSky.cloud_coverage `coverage`. */
    bool clouds;
    f32 coverage;
    f32 cam_z;           /**< the camera height (m); 0: the scene's */
    f32 pan_deg;         /**< > 0: a temporal pan of this many degrees a frame over 30 frames,
                              captured at frame 30 and checked against the converged still */
    bool coverage_pair;  /**< left half coverage 0, right half coverage 1 (one run) */
    /* DENG-852: volumetrics Low; the capture (blue-noise jitter) is checked for banding against
     * the old interleaved-gradient jitter (r.clouds_blue_noise 0), both against volumetrics Ultra. */
    bool low_banding;
    /* DENG-849: no DeSky entity (de_sky_attach still runs): no sky, no sky IBL and no state; the
     * fog must still draw, without the aerial perspective. */
    bool no_sky;
    /* DENG-861: de_sky_attach is never called (implies no_sky): the renderer's own fog stage must
     * draw the fog, the same image as fog_no_sky. */
    bool no_attach;
} sky_opts;

/* DENG-207: the largest mean sRGB change allowed between two consecutive frames of a static fog
 * scene (the jittered slices with the 0.9 history blend). */
#define SKY_FOG_STABLE_MEAN 1.0
/* DENG-208: the frames of the clouds' temporal pan, and the largest mean sRGB difference allowed
 * between its last frame and the converged still at the same heading (ghosting trails). */
#define SKY_CLOUDS_PAN_FRAMES 30u
#define SKY_CLOUDS_GHOST_MEAN 3.0

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

/* DENG-853: the colour of the light on the ground (sky_ground.scene.toml's mat/ground, base colour
 * 0.22 0.26 0.16) in a 64 x 32 block centred at `fx`, 40 rows above the bottom: the block's linear
 * mean divided by the albedo, as blue / red. Neutral light is about 1; the clear sky's light in
 * the shade is blue (> 2). Under overcast it must stay below SKY_OVERCAST_GROUND_BLUE. */
#define SKY_OVERCAST_GROUND_BLUE 1.35
static f64 sky_ground_light_blue(const sky_run *s, f32 fx) {
    f32 m[3];
    sky_block_mean(s, fx, s->ctx->height - 40u, 64u, 32u, m);
    const f64 albedo[3] = {0.22, 0.26, 0.16};
    f64 lin[3];
    for (u32 k = 0; k < 3u; ++k) {
        const f64 c = (f64)m[k] / 255.0;
        lin[k] = (c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4)) / albedo[k];
    }
    return lin[0] > 0.0 ? lin[2] / lin[0] : 0.0;
}

static void sky_check_overcast_ground(sky_run *s, f32 fx) {
    const f64 blue = sky_ground_light_blue(s, fx);
    printf("  clouds overcast ground light blue/red %.2f (limit %.2f)\n", blue,
           SKY_OVERCAST_GROUND_BLUE);
    if (!(blue <= SKY_OVERCAST_GROUND_BLUE))
        de_golden_fail(s->ctx, "the ground under overcast is lit blue (blue/red %.2f > %.2f): the "
                               "sky IBL ignores the clouds", blue, SKY_OVERCAST_GROUND_BLUE);
}

/* DENG-208: the clouds' extra frames, after the first capture (in s->pixels).
 *   coverage_pair: the capture (coverage 0) must show the clear blue sky; then coverage 1 for
 *     SKY_FRAMES must turn the top grey (overcast). The left half of the result is coverage 0, the
 *     right half coverage 1.
 *   pan_deg: SKY_CLOUDS_PAN_FRAMES frames of panning, then the capture; then SKY_FRAMES still
 *     frames at the same heading: the converged picture. Their mean difference is the ghosting. */
static bool sky_clouds_checks(sky_run *s, const sky_opts *o, DeSky *sky, DeTransform *cam_xf,
                              size_t bytes) {
    de_golden_ctx *ctx = s->ctx;
    if (o->coverage_pair) {
        f32 clear[3];
        sky_block_mean(s, 0.5f, 4u, 64u, 16u, clear);
        printf("  clouds coverage 0: ground light blue/red %.2f\n", sky_ground_light_blue(s, 0.25f));
        printf("  clouds coverage 0: top sRGB %.1f %.1f %.1f\n", (f64)clear[0], (f64)clear[1],
               (f64)clear[2]);
        if (!(clear[2] > clear[0] + 8.0f))
            de_golden_fail(ctx, "coverage 0 is not the clear sky (%.0f %.0f %.0f)", (f64)clear[0],
                           (f64)clear[1], (f64)clear[2]);
        u8 *first = (u8 *)malloc(bytes);
        if (first == NULL) {
            de_golden_fail(ctx, "no memory for the coverage pair");
            return false;
        }
        memcpy(first, s->pixels, bytes);
        sky->cloud_coverage = 1.0f;
        bool ok = true;
        for (u32 i = 0; ok && i < SKY_FRAMES; ++i)
            ok = sky_frame(s);
        ok = ok && sky_read(s);
        if (ok) {
            f32 over[3];
            sky_block_mean(s, 0.5f, 4u, 64u, 16u, over);
            printf("  clouds coverage 1: top sRGB %.1f %.1f %.1f\n", (f64)over[0], (f64)over[1],
                   (f64)over[2]);
            if (over[2] > over[0] + 25.0f)
                de_golden_fail(ctx, "coverage 1 is not overcast: the top is still blue (%.0f %.0f "
                                    "%.0f)", (f64)over[0], (f64)over[1], (f64)over[2]);
            sky_check_overcast_ground(s, 0.25f); /* DENG-853 */
            for (u32 y = 0; y < ctx->height; ++y)
                memcpy(s->pixels + (size_t)y * s->pitch, first + (size_t)y * s->pitch,
                       (size_t)s->pitch / 2u);
        }
        free(first);
        return ok;
    }
    if (o->pan_deg > 0.0f) {
        f32 heading = o->heading_deg;
        bool ok = true;
        for (u32 i = 0; ok && i < SKY_CLOUDS_PAN_FRAMES; ++i) {
            heading += o->pan_deg;
            cam_xf->rotation = sky_camera_rotation(heading, o->pitch_deg);
            ok = sky_frame(s);
        }
        ok = ok && sky_read(s);
        u8 *pan = ok ? (u8 *)malloc(bytes) : NULL;
        if (pan == NULL) {
            if (ok)
                de_golden_fail(ctx, "no memory for the pan readback");
            return false;
        }
        memcpy(pan, s->pixels, bytes);
        for (u32 i = 0; ok && i < SKY_FRAMES; ++i)
            ok = sky_frame(s);
        ok = ok && sky_read(s);
        f64 diff = 0.0;
        for (size_t i = 0; ok && i < bytes; ++i)
            if (i % 4u != 3u)
                diff += fabs((f64)s->pixels[i] - (f64)pan[i]);
        diff /= (f64)ctx->width * (f64)ctx->height * 3.0;
        memcpy(s->pixels, pan, bytes);
        free(pan);
        if (!ok)
            return false;
        printf("  clouds pan frame %u vs converged: sRGB mean difference %.3f\n",
               SKY_CLOUDS_PAN_FRAMES, diff);
        if (diff > SKY_CLOUDS_GHOST_MEAN)
            de_golden_fail(ctx, "the clouds ghost in a pan: a mean difference of %.3f from the "
                                "converged frame (limit %.1f)", diff, SKY_CLOUDS_GHOST_MEAN);
    }
    return true;
}

/* DENG-852: the banding of a cloud picture `test` against a reference `ref` (sRGB8 RGBA, the
 * view's pitch), over the sky rows [8, 45% of the height). e is the linear-luminance error over
 * the reference's mean luminance. Banding is the RMS of box4(e) - box16(e): the error's structure
 * at the 4-16 pixel scale of the march's steps (the smooth bias of fewer steps cancels, and so
 * does the per-pixel grain). *noise gets the plain RMS of e. */
static f64 sky_band_metric(const sky_run *s, const u8 *test, const u8 *ref, f64 *noise) {
    const u32 w = s->ctx->width, y0 = 8u, h = (u32)((f32)s->ctx->height * 0.45f) - y0;
    f64 *sat = (f64 *)calloc((size_t)(w + 1u) * (h + 1u), sizeof(f64));
    f64 *e = (f64 *)malloc((size_t)w * h * sizeof(f64));
    if (sat == NULL || e == NULL) {
        free(sat);
        free(e);
        *noise = -1.0;
        return -1.0;
    }
    f64 mean = 0.0;
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            const size_t i = (size_t)(y0 + y) * s->pitch + (size_t)x * 4u;
            f64 yt = 0.0, yr = 0.0;
            static const f64 k_w[3] = {0.2126, 0.7152, 0.0722};
            for (u32 k = 0; k < 3u; ++k) {
                const f64 ct = test[i + k] / 255.0, cr = ref[i + k] / 255.0;
                yt += k_w[k] * (ct <= 0.04045 ? ct / 12.92 : pow((ct + 0.055) / 1.055, 2.4));
                yr += k_w[k] * (cr <= 0.04045 ? cr / 12.92 : pow((cr + 0.055) / 1.055, 2.4));
            }
            e[(size_t)y * w + x] = yt - yr;
            mean += yr;
        }
    mean /= (f64)w * h;
    const f64 inv = mean > 0.0 ? 1.0 / mean : 0.0;
    f64 n2 = 0.0;
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            const f64 v = e[(size_t)y * w + x] * inv;
            n2 += v * v;
            sat[(size_t)(y + 1u) * (w + 1u) + x + 1u] = v + sat[(size_t)y * (w + 1u) + x + 1u] +
                                                        sat[(size_t)(y + 1u) * (w + 1u) + x] -
                                                        sat[(size_t)y * (w + 1u) + x];
        }
    f64 b2 = 0.0;
    u32 n = 0;
    for (u32 y = 8u; y + 8u <= h; ++y)
        for (u32 x = 8u; x + 8u <= w; ++x, ++n) {
#define SKY_BOX(r)                                                                                 \
    ((sat[(size_t)(y + (r)) * (w + 1u) + x + (r)] - sat[(size_t)(y - (r)) * (w + 1u) + x + (r)] -    \
      sat[(size_t)(y + (r)) * (w + 1u) + x - (r)] + sat[(size_t)(y - (r)) * (w + 1u) + x - (r)]) / \
     (4.0 * (r) * (r)))
            const f64 d = SKY_BOX(2u) - SKY_BOX(8u);
#undef SKY_BOX
            b2 += d * d;
        }
    free(sat);
    free(e);
    *noise = sqrt(n2 / ((f64)w * h));
    return n != 0u ? sqrt(b2 / n) : 0.0;
}

/* DENG-852 AC: at volumetrics Low the blue-noise jitter bands less than the old interleaved
 * gradient noise. The capture (s->pixels, blue noise) is kept; SKY_FRAMES more with
 * r.clouds_blue_noise 0 give the IGN picture, then SKY_FRAMES at volumetrics Ultra (blue noise)
 * the reference. Fails when blue noise's banding is not below IGN's. */
static bool sky_clouds_banding(sky_run *s, de_render_settings *rs, size_t bytes) {
    de_golden_ctx *ctx = s->ctx;
    u8 *blue = (u8 *)malloc(bytes), *ign = (u8 *)malloc(bytes);
    bool ok = blue != NULL && ign != NULL;
    if (!ok)
        de_golden_fail(ctx, "no memory for the banding readbacks");
    if (ok) {
        memcpy(blue, s->pixels, bytes);
        sky_cvar("r.clouds_blue_noise", "0");
        for (u32 i = 0; ok && i < SKY_FRAMES; ++i)
            ok = sky_frame(s);
        ok = ok && sky_read(s);
        sky_cvar("r.clouds_blue_noise", "1");
    }
    if (ok) {
        memcpy(ign, s->pixels, bytes);
        rs->volumetrics = DE_QUALITY_ULTRA;
        de_renderer_set_settings(s->r, rs);
        for (u32 i = 0; ok && i < SKY_FRAMES; ++i)
            ok = sky_frame(s);
        ok = ok && sky_read(s);
    }
    if (ok) {
        f64 nb, ni;
        const f64 bb = sky_band_metric(s, blue, s->pixels, &nb);
        const f64 bi = sky_band_metric(s, ign, s->pixels, &ni);
        printf("  clouds Low vs Ultra banding (4-16 px error RMS): blue noise %.5f, IGN %.5f "
               "(ratio %.2f); per-pixel error RMS: blue %.5f, IGN %.5f\n",
               bb, bi, bi > 0.0 ? bb / bi : 0.0, nb, ni);
        if (bb < 0.0 || bi < 0.0)
            de_golden_fail(ctx, "no memory for the banding metric");
        else if (!(bb < bi))
            de_golden_fail(ctx, "at volumetrics Low the blue-noise jitter bands no less than IGN "
                                "(%.5f vs %.5f)", bb, bi);
        memcpy(s->pixels, blue, bytes);
    }
    free(blue);
    free(ign);
    return ok;
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
    if (o->fog) {
        rs.volumetrics = DE_QUALITY_HIGH;
        rs.shadows = DE_QUALITY_MEDIUM;
    }
    if (o->clouds)
        rs.volumetrics = o->low_banding ? DE_QUALITY_LOW : DE_QUALITY_HIGH; /* DENG-208, DENG-852 */
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
    if (o->cam_z > 0.0f)
        cam_xf->position.z = o->cam_z;

    /* The sky: latitude 45, day 80, the reference turbidity, the real moon. DENG-849: no_sky
     * spawns no DeSky at all. */
    const de_component c_sky = de_component_find(s.world, "DeSky");
    DeSky *sky = NULL;
    if (!o->no_sky) {
        const de_entity sky_e = de_entity_spawn(s.world);
        sky = DE_HANDLE_VALID(c_sky) ? (DeSky *)de_entity_add(s.world, sky_e, c_sky) : NULL;
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
                       .drive_sun = true,
                       .clouds = o->clouds,
                       .cloud_coverage = o->coverage_pair ? 0.0f : o->coverage};
    }
    if (!o->no_attach && de_sky_attach(s.world, s.r, &s.sky) != DE_OK) {
        de_golden_fail(ctx, "de_sky_attach: %s", sky_err());
        sky_close(&s);
        return;
    }
    /* DENG-207: the fog volumes, spawned with their transform and world transform in one row
     * (these frames run no transform propagation). */
    const de_component c_fog = de_component_find(s.world, "DeFogVolume");
    const de_component c_global = de_component_find(s.world, "DeGlobalTransform");
    for (u32 i = 0; i < o->fog_count && i < 2u; ++i) {
        const de_component comps[3] = {c_transform, c_global, c_fog};
        de_entity fe = DE_ENTITY_NULL;
        DeTransform *fxf = NULL;
        DeGlobalTransform *gxf = NULL;
        DeFogVolume *fv = NULL;
        if (de_entity_spawn_with(s.world, comps, 3u, &fe) == DE_OK) {
            fxf = (DeTransform *)de_entity_get(s.world, fe, c_transform);
            gxf = (DeGlobalTransform *)de_entity_get(s.world, fe, c_global);
            fv = (DeFogVolume *)de_entity_get(s.world, fe, c_fog);
        }
        if (fxf == NULL || gxf == NULL || fv == NULL) {
            de_golden_fail(ctx, "adding DeFogVolume: %s", sky_err());
            sky_close(&s);
            return;
        }
        fxf->position = (de_dvec3){o->fog_pos[i].x, o->fog_pos[i].y, o->fog_pos[i].z};
        fxf->rotation = (de_quat){0.0f, 0.0f, 0.0f, 1.0f};
        fxf->scale = (de_vec3){1.0f, 1.0f, 1.0f};
        *gxf = *fxf;
        *fv = o->fog_volume[i];
    }

    /* DENG-844: the sky's IBL lights the frame that receives it. A frame that handed an
     * environment to lighting must have packed it in its own lighting build (the "sky.ibl" stage
     * runs before it); the old order (the sky stage after lighting) left one unpacked after every
     * handover, until the next frame. At least one handover must happen. */
    const de__lighting *lt = de__views_lighting(s.r);
    for (u32 i = 0; i < SKY_FRAMES; ++i) {
        if (!sky_frame(&s)) {
            sky_close(&s);
            return;
        }
        if (lt != NULL && de__lighting_sky_unlit(lt) != 0u) {
            de_golden_fail(ctx, "frame %u: a sky environment was handed over after the lighting "
                                "build (a frame of lag)", i);
            sky_close(&s);
            return;
        }
    }
    if (!o->no_sky && !o->no_attach) {
        if (lt == NULL || lt->sky_serial == 0u) {
            de_golden_fail(ctx, "the sky's IBL environment never reached lighting in %u frames",
                           SKY_FRAMES);
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
    }
    if (!sky_read(&s)) {
        sky_close(&s);
        return;
    }
    /* The sky drew: the top rows are not the view's clear or debug colour. By day the zenith side
     * is blue (b > r); at night it is dark. */
    f32 top[3];
    sky_block_mean(&s, 0.5f, 4u, 64u, 16u, top);
    printf("  sky top-centre sRGB mean %.1f %.1f %.1f\n", (f64)top[0], (f64)top[1], (f64)top[2]);
    if (o->clouds && o->coverage >= 0.9f && o->pitch_deg > 0.0f)
        sky_check_overcast_ground(&s, 0.25f); /* DENG-853: clouds_overcast */
    if (o->clouds && !sky_clouds_checks(&s, o, sky, cam_xf, bytes)) {
        sky_close(&s);
        return;
    }
    if (o->low_banding && !sky_clouds_banding(&s, &rs, bytes)) {
        sky_close(&s);
        return;
    }
    if (o->clouds) {
        sky_block_mean(&s, 0.5f, 4u, 64u, 16u, top);
        /* The frame budget (AC4), after the capture: r.rg_gpu_timers on for 16 more frames, the
         * clouds' passes against the whole frame averaged over the last 8 (printed only; on Metal
         * the timers serialise the passes, so these are upper bounds). */
        sky_cvar("r.rg_gpu_timers", "1");
        f64 cms = 0.0, fms = 0.0;
        u32 timed = 0;
        for (u32 i = 0; i < 16u && sky_frame(&s); ++i) {
            const de_render_stats *st = de_renderer_stats(s.r);
            if (i < 8u || st == NULL || st->pass_gpu_ms == NULL || st->gpu_ms <= 0.0f)
                continue;
            for (u32 k = 0; k < st->pass_count; ++k)
                if (strncmp(st->pass_names[k], "clouds.", 7) == 0)
                    cms += st->pass_gpu_ms[k];
            fms += st->gpu_ms;
            ++timed;
        }
        sky_cvar("r.rg_gpu_timers", "0");
        if (timed > 0u)
            printf("  clouds GPU %.3f ms of a %.3f ms frame (%ux%u, volumetrics %s, %u frames)\n",
                   cms / timed, fms / timed, ctx->width, ctx->height,
                   rs.volumetrics == DE_QUALITY_ULTRA ? "Ultra" : "High", timed);
    }
    if (!o->fog && !o->clouds && o->sun_elev_min > 0.0f && !(top[2] > top[0] + 8.0f))
        de_golden_fail(ctx, "the daytime sky is not blue at the top (%.0f %.0f %.0f)",
                       (f64)top[0], (f64)top[1], (f64)top[2]);
    if (!o->fog && !o->clouds && o->sun_elev_min > 0.0f && top[2] < 40.0f)
        de_golden_fail(ctx, "the daytime sky is dark at the top (%.0f)", (f64)top[2]);
    if (o->fog) {
        /* DENG-207 temporal stability: a static scene's next frame stays within
         * SKY_FOG_STABLE_MEAN of the captured one (the slice jitter is averaged by the history). */
        u8 *first = (u8 *)malloc(bytes);
        if (first == NULL) {
            de_golden_fail(ctx, "no memory for the stability readback");
            sky_close(&s);
            return;
        }
        memcpy(first, s.pixels, bytes);
        const bool ok = sky_frame(&s) && sky_read(&s);
        f64 diff = 0.0;
        for (size_t i = 0; ok && i < bytes; ++i)
            if (i % 4u != 3u)
                diff += fabs((f64)s.pixels[i] - (f64)first[i]);
        diff /= (f64)ctx->width * (f64)ctx->height * 3.0;
        memcpy(s.pixels, first, bytes);
        free(first);
        if (!ok) {
            sky_close(&s);
            return;
        }
        printf("  fog frame-to-frame sRGB mean change %.3f\n", diff);
        if (diff > SKY_FOG_STABLE_MEAN)
            de_golden_fail(ctx, "the fog flickers: a mean change of %.3f between two frames "
                                "(limit %.1f)", diff, SKY_FOG_STABLE_MEAN);
    }
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

/* ---- DENG-207: froxel volumetric fog ---- */

/* A global height fog at the ground (a zero extent). */
static DeFogVolume sky_fog_global(f32 density, f32 falloff, f32 g) {
    return (DeFogVolume){.density = density,
                         .albedo = {1.0f, 1.0f, 1.0f},
                         .anisotropy = g,
                         .height_falloff = falloff};
}

/* A spot light 6 m up over a short column, at night in a thin global fog: the cone is a lit
 * shaft with the column's atlas shadow cut out of it. */
DE_GOLDEN_SCENE(fog_spot_shaft) {
    sky_run_scene(ctx, &(sky_opts){.hours = 21.0f,
                                   .heading_deg = 0.0f,
                                   .pitch_deg = 8.0f,
                                   .ev100 = 1.0f,
                                   .moon_phase = 0.5f,
                                   .sun_elev_min = -60.0f,
                                   .sun_elev_max = -18.0f,
                                   .expect_moon_light = true,
                                   .scene = "scenes/fog_spot",
                                   .fog = true,
                                   .fog_count = 1u,
                                   .fog_volume = {sky_fog_global(0.04f, 0.05f, 0.3f)}});
}
/* The low sun through a gap in a wall west of the camera: forward-scattering (g 0.6) fog makes
 * the CSM-shadowed shaft through the gap visible. */
DE_GOLDEN_SCENE(fog_sun_window) {
    sky_run_scene(ctx, &(sky_opts){.hours = 17.5f,
                                   .heading_deg = 250.0f,
                                   .pitch_deg = 6.0f,
                                   .ev100 = 12.0f,
                                   .sun_elev_min = 2.0f,
                                   .sun_elev_max = 12.0f,
                                   .scene = "scenes/fog_window",
                                   .fog = true,
                                   .fog_count = 1u,
                                   .fog_volume = {sky_fog_global(0.03f, 0.04f, 0.6f)}});
}
/* A local box of noisy, soft-edged fog in front of the columns at noon; no global fog. */
DE_GOLDEN_SCENE(fog_local_volume) {
    sky_run_scene(ctx, &(sky_opts){.hours = 12.0f,
                                   .heading_deg = 180.0f,
                                   .pitch_deg = 4.0f,
                                   .ev100 = 14.5f,
                                   .sun_elev_min = 44.0f,
                                   .sun_elev_max = 46.5f,
                                   .fog = true,
                                   .fog_count = 1u,
                                   .fog_pos = {{2.0f, -14.0f, 2.0f}},
                                   .fog_volume = {{.extent = {12.0f, 6.0f, 4.0f},
                                                   .density = 0.35f,
                                                   .albedo = {0.95f, 0.95f, 1.0f},
                                                   .anisotropy = 0.2f,
                                                   .noise = 0.6f,
                                                   .edge_fade = 1.5f}}});
}
/* DENG-849: the spot-lit column in global fog with no DeSky entity (an interior): the fog still
 * runs, with no aerial perspective behind it. The constant ambient fallback lights the room. */
DE_GOLDEN_SCENE(fog_no_sky) {
    sky_run_scene(ctx, &(sky_opts){.heading_deg = 0.0f,
                                   .pitch_deg = 8.0f,
                                   .ev100 = 4.0f,
                                   .scene = "scenes/fog_spot",
                                   .no_sky = true,
                                   .keep_ambient = true,
                                   .fog = true,
                                   .fog_count = 1u,
                                   .fog_volume = {sky_fog_global(0.04f, 0.05f, 0.3f)}});
}
/* DENG-861: fog_no_sky with no de_sky_attach at all: the renderer's fog stage draws it. */
DE_GOLDEN_SCENE(fog_no_attach) {
    sky_run_scene(ctx, &(sky_opts){.heading_deg = 0.0f,
                                   .pitch_deg = 8.0f,
                                   .ev100 = 4.0f,
                                   .scene = "scenes/fog_spot",
                                   .no_sky = true,
                                   .no_attach = true,
                                   .keep_ambient = true,
                                   .fog = true,
                                   .fog_count = 1u,
                                   .fog_volume = {sky_fog_global(0.04f, 0.05f, 0.3f)}});
}
/* A blended glass panel in global fog: the forward transparent pass applies the same froxel
 * volume (froxel_apply), so the panel fades with the columns behind it. */
DE_GOLDEN_SCENE(fog_transparent) {
    sky_run_scene(ctx, &(sky_opts){.hours = 12.0f,
                                   .heading_deg = 180.0f,
                                   .pitch_deg = 4.0f,
                                   .ev100 = 14.5f,
                                   .sun_elev_min = 44.0f,
                                   .sun_elev_max = 46.5f,
                                   .scene = "scenes/fog_glass",
                                   .fog = true,
                                   .fog_count = 1u,
                                   .fog_volume = {sky_fog_global(0.025f, 0.0f, 0.2f)}});
}
/* The aerial perspective scene (columns 200 m to 24 km) in a thin global fog: the fog covers the
 * first 128 m (r.fog.max_distance) and the aerial perspective continues beyond with no seam. */
DE_GOLDEN_SCENE(fog_ap_seam) {
    sky_run_scene(ctx, &(sky_opts){.hours = 15.5f,
                                   .heading_deg = 0.0f,
                                   .pitch_deg = 2.0f,
                                   .ev100 = 14.0f,
                                   .sun_elev_min = 15.0f,
                                   .sun_elev_max = 35.0f,
                                   .scene = "scenes/sky_aerial",
                                   .fog = true,
                                   .fog_count = 1u,
                                   .fog_volume = {sky_fog_global(0.006f, 0.02f, 0.4f)}});
}

/* ---- DENG-208: volumetric clouds (S34) ---- */

DE_GOLDEN_SCENE(clouds_overcast) {
    sky_run_scene(ctx, &(sky_opts){.hours = 12.0f,
                                   .heading_deg = 180.0f,
                                   .pitch_deg = 22.0f,
                                   .ev100 = 14.0f,
                                   .sun_elev_min = 44.0f,
                                   .sun_elev_max = 46.5f,
                                   .clouds = true,
                                   .coverage = 0.95f});
}
DE_GOLDEN_SCENE(clouds_cumulus_noon) {
    sky_run_scene(ctx, &(sky_opts){.hours = 12.0f,
                                   .heading_deg = 180.0f,
                                   .pitch_deg = 22.0f,
                                   .ev100 = 14.5f,
                                   .sun_elev_min = 44.0f,
                                   .sun_elev_max = 46.5f,
                                   .clouds = true,
                                   .coverage = 0.45f,
                                   .pan_deg = 0.5f});
}
DE_GOLDEN_SCENE(clouds_sunset) {
    sky_run_scene(ctx, &(sky_opts){.hours = 17.5f,
                                   .heading_deg = 270.0f,
                                   .pitch_deg = 12.0f,
                                   .ev100 = 12.0f,
                                   .sun_elev_min = 2.0f,
                                   .sun_elev_max = 12.0f,
                                   .clouds = true,
                                   .coverage = 0.5f});
}
DE_GOLDEN_SCENE(clouds_shadow) {
    sky_run_scene(ctx, &(sky_opts){.hours = 12.0f,
                                   .heading_deg = 180.0f,
                                   .pitch_deg = -35.0f,
                                   .ev100 = 14.5f,
                                   .sun_elev_min = 44.0f,
                                   .sun_elev_max = 46.5f,
                                   .clouds = true,
                                   .coverage = 0.5f,
                                   .cam_z = 600.0f});
}
DE_GOLDEN_SCENE(clouds_coverage_pair) {
    sky_run_scene(ctx, &(sky_opts){.hours = 12.0f,
                                   .heading_deg = 180.0f,
                                   .pitch_deg = 22.0f,
                                   .ev100 = 14.5f,
                                   .sun_elev_min = 44.0f,
                                   .sun_elev_max = 46.5f,
                                   .clouds = true,
                                   .coverage_pair = true});
}
/* DENG-852: cumulus at volumetrics Low, the blue-noise jitter's banding against IGN's. */
DE_GOLDEN_SCENE(clouds_low_banding) {
    sky_run_scene(ctx, &(sky_opts){.hours = 12.0f,
                                   .heading_deg = 180.0f,
                                   .pitch_deg = 22.0f,
                                   .ev100 = 14.5f,
                                   .sun_elev_min = 44.0f,
                                   .sun_elev_max = 46.5f,
                                   .clouds = true,
                                   .coverage = 0.45f,
                                   .low_banding = true});
}
