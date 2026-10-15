/* materials_scenes.c — the materials gallery golden scenes (DENG-307, SPEC §19.2, §23, §12.6,
 * §12.7). One of the three Milestone 1 reference scenes; S20 (DENG-278) reuses it, so the scene
 * names and tests/golden/materials/ stay where they are.
 *
 *   materials_gallery_ibl     every material layer (rows: metallic x roughness; IOR, specular
 *                             and emissive strength; clearcoat and sheen; anisotropy and
 *                             iridescence; transmission, volume, diffuse transmission,
 *                             subsurface, Unlit, alpha mask and blend) lit by a sky environment
 *   materials_gallery_lights  the same gallery lit by a sun, a point, a spot, a rect and a disk
 *                             light in physical units, no environment
 *   materials_furnace         white spheres (dielectric and metal, three roughnesses each) in a
 *                             uniform environment: an objective check in C that every sphere's
 *                             interior is the environment's radiance x exposure within 2 %
 *   materials_khr_samples     procedural stand-ins for five Khronos glTF sample models
 *                             (assets/LICENSES.md)
 *
 * Each scene runs the engine as a game does: a type registry, an ECS world, a task system, the
 * asset system in SOURCE mode over tests/golden/materials/assets/ (assets.toml; the importers
 * cook the glTF meshes, PNG and EXR textures, `.mat.toml` materials and `.scene.toml` scenes on
 * load, through a per-backend cook cache in the build tree), and the renderer on the harness's
 * device. The scene asset is instantiated (de_prefab_instantiate); its camera (entity 0x1, the
 * first root) is bound to a render-to-texture view of the harness target's size (RGBA8 sRGB, so
 * the bytes are the sRGB values the harness compares). The models load first (the scenes name
 * their meshes, sub-assets the asset system knows once a model is in), then the scene with its
 * materials, textures and environment. Frames run (r.pipelined 0; the kick also flushes the GPU
 * uploads the loads wait for) until the image settles: no pixel changes for three frames in a
 * row, also under Metal shader validation (DENG-430). The last image is copied into the harness
 * target.
 *
 * Known gaps the references hold (follow-ups, not threshold slack): deferred lighting does not
 * apply the Standard layers of opaque surfaces (DENG-429: the clearcoat, sheen, anisotropy and
 * iridescence rows look like the base); IBL has no clearcoat or sheen term (DENG-304); rough
 * transmission darkens (single scattering, DENG-301); transparents draw LOD 0 (DENG-305).
 *
 * Golden determinism: fixed EV100 on each scene's DeCamera (no auto exposure), a fixed camera,
 * output.basic (DENG-304) as the output stage, no temporal effect. assets/gen_assets.py writes
 * every asset; docs/testing/golden.md has the references and thresholds.
 */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "golden.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_debug.h>
#include <dengine/de_ecs.h>
#include <dengine/de_memory.h>
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

#define MG_LOAD_WAIT_NS  (300ull * 1000000000ull) /* the first run cooks every asset */
#define MG_FRAME_WAIT_NS (120ull * 1000000000ull)
#define MG_MIN_FRAMES    6u   /* frames before the image may count as settled */
#define MG_MAX_FRAMES    240u /* the image must settle before this */
#define MG_SETTLED       3u   /* consecutive frames in which no pixel changes */
/* Furnace: the environment's radiance (1 nit x intensity 1) times the exposure of EV100 0. */
#define MG_FURNACE_EXPECTED  (1.0 / 1.2)
#define MG_FURNACE_TOLERANCE 0.02
#define MG_FURNACE_ERODE     3 /* pixels from a sphere's silhouette that are not checked */

/* The runtime cooks sources on load (SOURCE mode) only with the importers: Debug and Dev builds
 * with the tools (DE_HAS_COOKER). */
#if defined(DE_HAS_COOKER) && DE_HAS_COOKER
#define MG_RUNTIME_COOKS 1
#else
#define MG_RUNTIME_COOKS 0
#endif

typedef struct mg_opts {
    const char *scene; /**< the scene asset */
    bool furnace;      /**< check the furnace in C */
} mg_opts;

#if DE_GOLDEN_COOK && MG_RUNTIME_COOKS

/* The gallery's models (assets.toml): each scene names their meshes. */
#define MG_MODEL_COUNT 4u
static const char *const k_models[MG_MODEL_COUNT] = {"mesh/sphere", "mesh/card", "mesh/box",
                                                     "mesh/cylinder"};

typedef struct mg {
    de_golden_ctx *ctx;
    de_task_system *tasks;
    de_type_registry *types;
    de_world *world;
    de_asset_system *assets;
    de_renderer *r;
    de_gpu_texture target;
    de_gpu_buffer rb;
    de_gpu_fence fence;
    u64 fence_value;
    de_view view;
    de_load_handle scene_load;
    de_load_handle model_load[MG_MODEL_COUNT];
    u32 pitch;
    u8 *now, *prev;
} mg;

static void mg_cvar(const char *name, const char *value) {
    const de_cvar cv = de_cvar_find(name);
    if (DE_HANDLE_VALID(cv))
        (void)de_cvar_set(cv, value);
    de_error_clear();
}

static const char *mg_err(void) {
    const de_error *e = de_error_last();
    return e != NULL ? e->message : "?";
}

static const char *mg_backend_dir(de_gpu_backend b) {
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

/* The mounts: the assets read-only as "galmat", the cook cache writable as "galcache" (CMake
 * makes the directory). */
static bool mg_mount(de_golden_ctx *ctx) {
    static bool mounted;
    if (mounted)
        return true;
    if (de_fs_mount("galmat", DE_GOLDEN_MATERIALS_ASSETS, 0, 0) != DE_OK) {
        de_golden_fail(ctx, "mounting %s: %s", DE_GOLDEN_MATERIALS_ASSETS, mg_err());
        return false;
    }
    if (de_fs_mount("galcache", DE_GOLDEN_MATERIALS_CACHE, DE_MOUNT_WRITABLE, 0) != DE_OK) {
        de_golden_fail(ctx, "mounting %s: %s", DE_GOLDEN_MATERIALS_CACHE, mg_err());
        return false;
    }
    mounted = true;
    return true;
}

/* One frame of the game loop: the asset update, PostUpdate, Extract and the kick (r.pipelined 0:
 * the frame is rendered and submitted at once). */
static bool mg_frame(mg *m) {
    de_asset_system_update(m->assets, NULL);
    de_result r = de_world_run_schedule(m->world, DE_SCHEDULE_POST_UPDATE, 1.0f / 60.0f, 0.0f);
    if (r == DE_OK)
        r = de_world_run_schedule(m->world, DE_SCHEDULE_EXTRACT, 1.0f / 60.0f, 0.0f);
    if (r == DE_OK)
        r = de__render_extract_and_kick(m->r, m->world);
    if (r != DE_OK) {
        de_golden_fail(m->ctx, "frame: %s", mg_err());
        return false;
    }
    return true;
}

/* Reads the view's target into m->now on a submission of its own (queue order puts it after the
 * frame), and waits. The view leaves its target in DE_ACCESS_SHADER_READ; so does this. */
static bool mg_read(mg *m) {
    de_golden_ctx *ctx = m->ctx;
    de_gpu_cmd *c = de_gpu_cmd_begin(ctx->device, DE_QUEUE_GRAPHICS, "materials readback");
    if (c == NULL) {
        de_golden_fail(ctx, "readback command list: %s", mg_err());
        return false;
    }
    de_gpu_barrier b = {
        .texture = m->target, .before = DE_ACCESS_SHADER_READ, .after = DE_ACCESS_COPY_SRC};
    de_gpu_cmd_barriers(c, &b, 1);
    de_gpu_cmd_copy_texture_to_buffer(c, m->rb, 0, m->pitch, m->target, 0, 0);
    b.before = DE_ACCESS_COPY_SRC;
    b.after = DE_ACCESS_SHADER_READ;
    de_gpu_cmd_barriers(c, &b, 1);
    const u64 v = ++m->fence_value;
    const de_gpu_submit s = {.cmds = &c, .cmd_count = 1, .signal = m->fence, .signal_value = v};
    if (de_gpu_queue_submit(ctx->device, DE_QUEUE_GRAPHICS, &s) != DE_OK ||
        de_gpu_fence_wait(ctx->device, m->fence, v, MG_FRAME_WAIT_NS) != DE_OK) {
        de_golden_fail(ctx, "readback: %s", mg_err());
        return false;
    }
    const u8 *src = (const u8 *)de_gpu_buffer_map(ctx->device, m->rb);
    if (src == NULL) {
        de_golden_fail(ctx, "readback map: %s", mg_err());
        return false;
    }
    memcpy(m->now, src, (size_t)m->pitch * ctx->height);
    return true;
}

/* Pixels of m->now that differ from m->prev by more than one code value in a channel. */
static u32 mg_changed(const mg *m) {
    u32 changed = 0;
    const size_t n = (size_t)m->pitch * m->ctx->height;
    for (size_t i = 0; i < n; i += 4) {
        bool moved = false;
        for (u32 c = 0; c < 3; ++c) {
            const int d = (int)m->now[i + c] - (int)m->prev[i + c];
            moved |= d > 1 || d < -1;
        }
        changed += moved ? 1u : 0u;
    }
    return changed;
}

static f64 mg_srgb_to_linear(u8 v) {
    const f64 c = (f64)v / 255.0;
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

/* The furnace: every pixel at least MG_FURNACE_ERODE pixels inside a sphere's silhouette (the
 * background is black) must be MG_FURNACE_EXPECTED within MG_FURNACE_TOLERANCE, per channel. */
static void mg_check_furnace(mg *m) {
    de_golden_ctx *ctx = m->ctx;
    const u32 w = ctx->width, h = ctx->height;
    u8 *cover = (u8 *)calloc((size_t)w * h, 1);
    if (cover == NULL) {
        de_golden_fail(ctx, "out of memory");
        return;
    }
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            const u8 *p = m->now + (size_t)y * m->pitch + (size_t)x * 4u;
            cover[(size_t)y * w + x] = (p[0] | p[1] | p[2]) > 8u;
        }
    u32 checked = 0, bad = 0;
    f64 lo = 1e9, hi = -1e9;
    const i32 e = MG_FURNACE_ERODE;
    for (i32 y = e; y < (i32)h - e; ++y)
        for (i32 x = e; x < (i32)w - e; ++x) {
            bool inside = true;
            for (i32 dy = -e; dy <= e && inside; ++dy)
                for (i32 dx = -e; dx <= e && inside; ++dx)
                    inside = cover[(size_t)(y + dy) * w + (size_t)(x + dx)] != 0;
            if (!inside)
                continue;
            const u8 *p = m->now + (size_t)y * m->pitch + (size_t)x * 4u;
            checked++;
            bool ok = true;
            for (u32 c = 0; c < 3; ++c) {
                const f64 v = mg_srgb_to_linear(p[c]);
                lo = v < lo ? v : lo;
                hi = v > hi ? v : hi;
                ok &= fabs(v - MG_FURNACE_EXPECTED) <= MG_FURNACE_TOLERANCE * MG_FURNACE_EXPECTED;
            }
            if (!ok && bad++ < 4)
                printf("  furnace: pixel %d,%d = %u %u %u (linear %.4f %.4f %.4f), expected %.4f\n",
                       x, y, p[0], p[1], p[2], mg_srgb_to_linear(p[0]), mg_srgb_to_linear(p[1]),
                       mg_srgb_to_linear(p[2]), MG_FURNACE_EXPECTED);
        }
    free(cover);
    printf("  furnace: %u interior pixels, linear %.4f .. %.4f (expected %.4f +- %.0f %%), %u "
           "outside\n",
           checked, lo, hi, MG_FURNACE_EXPECTED, MG_FURNACE_TOLERANCE * 100.0, bad);
    if (checked < 1000u)
        de_golden_fail(ctx, "furnace: only %u interior sphere pixels", checked);
    else if (bad != 0)
        de_golden_fail(ctx,
                       "furnace: %u of %u interior pixels differ from %.4f by more than "
                       "%.0f %% (range %.4f .. %.4f)",
                       bad, checked, MG_FURNACE_EXPECTED, MG_FURNACE_TOLERANCE * 100.0, lo, hi);
}

/* Waits for load `h` (asset `what`), running frames (the asset update, and the kick that
 * flushes the renderer's uploads); fails the scene on an error or
 * when MG_LOAD_WAIT_NS have passed since `t0`. */
static bool mg_wait(mg *m, de_load_handle h, const char *what, u64 t0) {
    de_load_state st = DE_LOAD_PENDING;
    while (de_time_now_ns() - t0 < MG_LOAD_WAIT_NS) {
        st = de_asset_wait(m->assets, h, 20ull * 1000000ull);
        if (st == DE_LOAD_READY || st == DE_LOAD_FAILED || st == DE_LOAD_PARTIAL ||
            st == DE_LOAD_CANCELLED)
            break;
        if (!mg_frame(m)) /* the renderer's frames flush the GPU uploads loads wait for */
            return false;
    }
    if (st == DE_LOAD_READY)
        return true;
    de_load_info info;
    memset(&info, 0, sizeof info);
    (void)de_asset_status(m->assets, h, &info);
    de_golden_fail(m->ctx, "loading %s: %s", what,
                   info.first_error != DE_OK ? info.first_error_msg : "timed out");
    return false;
}

static void mg_close(mg *m) {
    if (m->r != NULL)
        de__render_world_wait_idle(m->r);
    if (m->assets != NULL && DE_HANDLE_VALID(m->scene_load))
        de_asset_release(m->assets, m->scene_load);
    for (u32 i = 0; m->assets != NULL && i < MG_MODEL_COUNT; ++i)
        if (DE_HANDLE_VALID(m->model_load[i]))
            de_asset_release(m->assets, m->model_load[i]);
    /* The order of the engine's own teardown (the material instances test): the asset system,
     * then the renderer (waits for the GPU), then the world. */
    if (m->assets != NULL)
        de_asset_system_destroy(m->assets);
    if (m->r != NULL)
        de_renderer_destroy(m->r);
    if (m->world != NULL)
        de_world_destroy(m->world);
    if (m->types != NULL)
        de_type_registry_destroy(m->types);
    if (m->tasks != NULL)
        de_task_system_destroy(m->tasks);
    de_gpu_device *d = m->ctx->device;
    if (DE_HANDLE_VALID(m->fence))
        de_gpu_fence_destroy(d, m->fence);
    if (DE_HANDLE_VALID(m->rb))
        de_gpu_buffer_destroy(d, m->rb);
    if (DE_HANDLE_VALID(m->target))
        de_gpu_texture_destroy(d, m->target);
    free(m->now);
    free(m->prev);
    mg_cvar("r.pipelined", "1");
}

static void mg_run(de_golden_ctx *ctx, const mg_opts *o) {
    if (!ctx->caps->base_tier_ok) {
        de_golden_skip(ctx, "below the Base tier (the renderer needs it): %s",
                       ctx->caps->base_tier_reason);
        return;
    }
    if (ctx->format != DE_FMT_RGBA8_UNORM || (ctx->width * 4u) % 256u != 0u) {
        de_golden_fail(ctx,
                       "scenes.toml: %s needs format rgba8 and a width that is a multiple "
                       "of 64",
                       o->scene);
        return;
    }
    if (!mg_mount(ctx))
        return;
    mg m;
    memset(&m, 0, sizeof m);
    m.ctx = ctx;
    m.pitch = ctx->width * 4u;
    const size_t bytes = (size_t)m.pitch * ctx->height;
    m.now = (u8 *)calloc(bytes, 1);
    m.prev = (u8 *)calloc(bytes, 1);
    de_fp_env_init_thread();
    char cache[96];
    snprintf(cache, sizeof cache, "galcache:/%s", mg_backend_dir(ctx->backend));
    if (!de_path_exists(cache))
        (void)de_dir_create(cache);
    if (m.now == NULL || m.prev == NULL ||
        de_task_system_create(&(de_task_system_desc){.worker_count = 2}, &m.tasks) != DE_OK ||
        de_type_registry_create(NULL, &m.types) != DE_OK ||
        de_world_create(&(de_world_desc){.types = m.types, .tasks = m.tasks}, &m.world) != DE_OK ||
        de_asset_system_create(&(de_asset_system_desc){.tasks = m.tasks,
                                                       .io_threads = 1,
                                                       .cache_dir = cache,
                                                       .staging_ring_bytes = 32u << 20,
                                                       .types = m.types},
                               &m.assets) != DE_OK ||
        de_renderer_create(
            &(de_renderer_desc){.device = ctx->device, .assets = m.assets, .types = m.types},
            &m.r) != DE_OK) {
        de_golden_fail(ctx, "setting up the engine: %s", mg_err());
        mg_close(&m);
        return;
    }
    const de__vis_shared *vis = de__views_vis(m.r);
    if (vis == NULL || !vis->enabled) {
        de_golden_skip(ctx, "the vis frame is off in this build: %s",
                       vis != NULL ? vis->why : "no vis frame");
        mg_close(&m);
        return;
    }
    de_renderer_attach(m.r, m.world);
    mg_cvar("r.pipelined", "0");
    /* Vulkan: one command list per submission (CI's VVL 1.3.275 checks bindless accesses against
     * the layouts at the end of each list; DENG-235, DENG-427). */
    if (ctx->backend == DE_GPU_VULKAN)
        mg_cvar("r.rg_record_cost", "1048576");
    const de_render_settings rs = de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_HIGH);
    de_renderer_set_settings(m.r, &rs);

    m.target = de_gpu_texture_create(
        ctx->device, &(de_gpu_texture_desc){.format = DE_FMT_RGBA8_SRGB,
                                            .width = ctx->width,
                                            .height = ctx->height,
                                            .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED |
                                                     DE_USAGE_COPY_SRC,
                                            .name = "materials gallery"});
    m.rb = de_gpu_buffer_create(ctx->device, &(de_gpu_buffer_desc){.size = (u64)bytes,
                                                                   .usage = DE_USAGE_COPY_DST,
                                                                   .memory = DE_MEM_READBACK,
                                                                   .name = "materials readback"});
    m.fence = de_gpu_fence_create(ctx->device, 0);
    if (DE_HANDLE_VALID(m.target))
        m.view = de_view_create(m.r, &(de_view_desc){.target = m.target, .name = "gallery"});
    if (!DE_HANDLE_VALID(m.target) || !DE_HANDLE_VALID(m.rb) || !DE_HANDLE_VALID(m.fence) ||
        !DE_HANDLE_VALID(m.view)) {
        de_golden_fail(ctx, "the gallery view: %s", mg_err());
        mg_close(&m);
        return;
    }

    /* The scene: its meshes, materials, textures and environment load (and cook) as its
     * dependencies before it is READY. */
    if (de_asset_manifest_load(m.assets, "galmat:/assets.toml") != DE_OK) {
        de_golden_fail(ctx, "assets.toml: %s", mg_err());
        mg_close(&m);
        return;
    }
    /* The models first: their meshes are sub-assets (`<model>#<mesh>`), which the asset system
     * knows once the model is in, and the scenes name them. */
    const u64 t0 = de_time_now_ns();
    for (u32 i = 0; i < MG_MODEL_COUNT; ++i) {
        m.model_load[i] = de_asset_load(m.assets, de_asset_id_from(k_models[i]), 0);
        if (!mg_wait(&m, m.model_load[i], k_models[i], t0)) {
            mg_close(&m);
            return;
        }
    }
    const de_asset_id id = de_asset_id_from(o->scene);
    m.scene_load = de_asset_load(m.assets, id, 0);
    if (!mg_wait(&m, m.scene_load, o->scene, t0)) {
        mg_close(&m);
        return;
    }
    printf("  %s ready after %.1f s\n", o->scene, (f64)(de_time_now_ns() - t0) / 1e9);
    de_entity cam = DE_ENTITY_NULL;
    if (de_prefab_instantiate(m.assets, m.world, de_asset_scene(m.assets, id), DE_ENTITY_NULL,
                              &cam) != DE_OK) {
        de_golden_fail(ctx, "instantiating %s: %s", o->scene, mg_err());
        mg_close(&m);
        return;
    }
    const DeRenderComponents *ids = de__render_components(m.world);
    DeCamera *camera = ids != NULL && de_entity_alive(m.world, cam)
                           ? (DeCamera *)de_entity_get(m.world, cam, ids->camera)
                           : NULL;
    if (camera == NULL) {
        de_golden_fail(ctx, "%s: entity 0x1 (the first root) has no DeCamera", o->scene);
        mg_close(&m);
        return;
    }
    camera->view = m.view;

    /* Frames until the image settles: no pixel changes by more than one code value (light-list
     * order comes from atomics, so the last bit may flicker) for MG_SETTLED frames in a row. Before
     * DENG-430 textured surfaces flickered here under Metal shader validation. */
    u32 settled = 0, k = 0, changed = 0;
    for (; k < MG_MAX_FRAMES && settled < MG_SETTLED; ++k) {
        if (!mg_frame(&m) || !mg_read(&m)) {
            mg_close(&m);
            return;
        }
        changed = k > 0 ? mg_changed(&m) : ctx->width * ctx->height;
        settled = k >= MG_MIN_FRAMES && changed == 0u ? settled + 1u : 0u;
        u8 *t = m.prev;
        m.prev = m.now;
        m.now = t;
    }
    /* m.prev holds the last image. */
    u8 *last = m.prev;
    m.prev = m.now;
    m.now = last;
    printf("  %s settled after %u frames (%u pixels changed in the last one)\n", o->scene, k,
           changed);
    if (settled < MG_SETTLED) {
        de_golden_fail(ctx, "%s: the image did not settle in %u frames", o->scene, MG_MAX_FRAMES);
        mg_close(&m);
        return;
    }
    if (o->furnace)
        mg_check_furnace(&m);

    /* Into the harness target through an UPLOAD buffer the harness owns. */
    const de_gpu_buffer up =
        de_golden_upload_buffer(ctx, m.now, (u64)bytes, DE_USAGE_COPY_SRC, "materials pixels");
    if (DE_HANDLE_VALID(up)) {
        de_golden_target_barrier(ctx, DE_ACCESS_COPY_DST);
        de_gpu_cmd_copy_buffer_to_texture(ctx->cmd, ctx->target, 0, 0, up, 0, m.pitch);
    }
    mg_close(&m);
}

#else /* !(DE_GOLDEN_COOK && MG_RUNTIME_COOKS) */

static void mg_run(de_golden_ctx *ctx, const mg_opts *o) {
    (void)o;
    de_golden_skip(ctx, "this build's asset system cannot cook sources (Release and Shipping have "
                        "no importers; DE_HAS_COOKER): the gallery's assets are cooked on load");
}

#endif

DE_GOLDEN_SCENE(materials_gallery_ibl) {
    mg_run(ctx, &(mg_opts){.scene = "scenes/gallery_ibl"});
}

DE_GOLDEN_SCENE(materials_gallery_lights) {
    mg_run(ctx, &(mg_opts){.scene = "scenes/gallery_lights"});
}

DE_GOLDEN_SCENE(materials_furnace) {
    mg_run(ctx, &(mg_opts){.scene = "scenes/furnace", .furnace = true});
}

DE_GOLDEN_SCENE(materials_khr_samples) {
    mg_run(ctx, &(mg_opts){.scene = "scenes/khr_samples"});
}
