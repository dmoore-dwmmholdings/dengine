/* DENG-343: production renderer Base-tier scenes; procedural CC0 assets. */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "golden.h"
#include "cooked_assets.h"

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
#include "render/passes/ssr/ssr_internal.h"
#include "render/passes/vis/vis_stages.h"
#include "render/views_internal.h"
#include "render/probes/probes_internal.h"
#include "render/probes/irradiance_internal.h"
#include "render/world/render_world.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MG_LOAD_WAIT_NS  (300ull * 1000000000ull) /* the first run cooks every asset */
#define MG_FRAME_WAIT_NS (120ull * 1000000000ull)

typedef struct mg_opts {
    const char *scene; /**< the scene asset */
    de_quality quality;
} mg_opts;

#if DE_GOLDEN_COOK

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
    char asset_pack[256];
    u32 pitch;
    u8 *now, *prev;
    bool saved_refraction, refraction_changed;
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

/* The mounts: the assets read-only as "basetier", the cook cache writable as "basecache" (CMake
 * makes the directory). */
static bool mg_mount(de_golden_ctx *ctx) {
    static bool mounted;
    if (mounted)
        return true;
    if (de_fs_mount("basetier", DE_GOLDEN_BASE_TIER_ASSETS, 0, 0) != DE_OK) {
        de_golden_fail(ctx, "mounting %s: %s", DE_GOLDEN_BASE_TIER_ASSETS, mg_err());
        return false;
    }
    if (de_fs_mount("basecache", DE_GOLDEN_BASE_TIER_CACHE, DE_MOUNT_WRITABLE, 0) != DE_OK) {
        de_golden_fail(ctx, "mounting %s: %s", DE_GOLDEN_BASE_TIER_CACHE, mg_err());
        return false;
    }
    if (de_fs_mount("dev", DE_GOLDEN_BASE_TIER_BAKES, DE_MOUNT_WRITABLE, 100) != DE_OK)
        return false;
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

/* Waits for load `h` (asset `what`), running frames (the asset update, and the kick that
 * flushes the renderer's uploads); fails the scene on an error or
 * when MG_LOAD_WAIT_NS have passed since `t0`. */
static bool mg_wait(mg *m, de_load_handle h, const char *what, u64 t0) {
    de_load_state st = DE_LOAD_PENDING;
    while (de_time_now_ns() - t0 < MG_LOAD_WAIT_NS) {
        st = de_asset_wait(m->assets, h, 200ull * 1000000ull);
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
    if (m->refraction_changed)
        mg_cvar("r.refraction", m->saved_refraction ? "1" : "0");
    if (m->assets != NULL && DE_HANDLE_VALID(m->scene_load))
        de_asset_release(m->assets, m->scene_load);
    for (u32 i = 0; m->assets != NULL && i < MG_MODEL_COUNT; ++i)
        if (DE_HANDLE_VALID(m->model_load[i]))
            de_asset_release(m->assets, m->model_load[i]);
    /* The order of the engine's own teardown (the material instances test): the asset system,
     * then the renderer (waits for the GPU), then the world. */
    if (m->assets != NULL)
        de_asset_system_destroy(m->assets);
    if (de_golden_pack_cleanup(m->asset_pack) != DE_OK)
        de_golden_fail(m->ctx, "cleaning temporary asset pack: %s", mg_err());
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

static void mg_find_entity(const de_query_chunk *chunk, void *user) {
    if (chunk->count)
        *(de_entity *)user = chunk->entities[0];
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
    (void)de_cvar_register(
        &(de_cvar_desc){.name = "r.rg_gpu_timers", .type = DE_CVAR_BOOL, .default_number = 0});
    mg_cvar("r.rg_gpu_timers", "0");
    m.pitch = ctx->width * 4u;
    const size_t bytes = (size_t)m.pitch * ctx->height;
    m.now = (u8 *)calloc(bytes, 1);
    m.prev = (u8 *)calloc(bytes, 1);
    de_fp_env_init_thread();
    char cache[96];
    snprintf(cache, sizeof cache, "basecache:/%s", mg_backend_dir(ctx->backend));
    if (!de_path_exists(cache))
        (void)de_dir_create(cache);
    if (m.now == NULL || m.prev == NULL ||
        de_task_system_create(&(de_task_system_desc){.worker_count = 2}, &m.tasks) != DE_OK ||
        de_type_registry_create(NULL, &m.types) != DE_OK ||
        de_world_create(&(de_world_desc){.types = m.types, .tasks = m.tasks}, &m.world) != DE_OK ||
        de_asset_system_create(&(de_asset_system_desc){.mode = DE_GOLDEN_ASSET_MODE,
                                                       .tasks = m.tasks,
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
    m.saved_refraction = de_cvar_bool(de_cvar_find("r.refraction"));
    m.refraction_changed = true;
    mg_cvar("r.refraction", o->quality == DE_QUALITY_OFF ? "0" : "1");
    de_renderer_attach(m.r, m.world);
    mg_cvar("r.pipelined", "0");
    mg_cvar("r.rg_gpu_timers", "0");
    /* Vulkan: one command list per submission (CI's VVL 1.3.275 checks bindless accesses against
     * the layouts at the end of each list; DENG-235, DENG-427). */
    if (ctx->backend == DE_GPU_VULKAN)
        mg_cvar("r.rg_record_cost", "1048576");
    de_render_settings rs = de_render_settings_preset(
        DE_TIER_BASE, o->quality == DE_QUALITY_OFF ? DE_QUALITY_MEDIUM : o->quality);
    rs.shadows = rs.ao = rs.reflections = rs.gi = DE_QUALITY_OFF;
    if (strstr(o->scene, "shadows_") != NULL)
        rs.shadows = o->quality;
    if (strstr(o->scene, "ao_gtao") != NULL)
        rs.ao = o->quality;
    if (strstr(o->scene, "reflections_") != NULL)
        rs.reflections = o->quality;
    if (strstr(o->scene, "gi_") != NULL) {
        rs.gi = o->quality;
        rs.gi_mode = o->quality == DE_QUALITY_MEDIUM ? DE_GI_STATIC_PROBES : DE_GI_SCREEN_SPACE;
    }
    mg_cvar("r.shadow_atlas_updates_per_frame", "192");
    mg_cvar("r.shadow_distance", "160");
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
    if (de_asset_manifest_load(m.assets, "basetier:/assets.toml") != DE_OK) {
        de_golden_fail(ctx, "assets.toml: %s", mg_err());
        mg_close(&m);
        return;
    }
    if (de_golden_pack_assets(m.assets, "basetier:/", m.asset_pack) != DE_OK) {
        de_golden_fail(ctx, "preparing packed gallery assets: %s", mg_err());
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
    char scene_name[128];
    const bool capture_bake = getenv("DE_GOLDEN_CAPTURE_BAKES") != NULL;
    snprintf(scene_name, sizeof scene_name, "%s%s", o->scene, capture_bake ? "_capture" : "");
    const de_asset_id id = de_asset_id_from(scene_name);
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

    /* Fixed simulation/frame count: temporal effects and PCSS use this exact sequence. */
    if (m.r->frame_index > 48u) {
        de_golden_fail(ctx, "asset preparation exceeded fixed capture pre-roll (%llu)",
                       (unsigned long long)m.r->frame_index);
        mg_close(&m);
        return;
    }
    while (m.r->frame_index < 96u) {
        if (!mg_frame(&m)) {
            mg_close(&m);
            return;
        }
    }
    if (!mg_read(&m)) {
        mg_close(&m);
        return;
    }
    if (getenv("DE_DENG604_READBACK") &&
        !de__ssr_diagnostic_dump(m.r->ssr, m.view, getenv("DE_DENG604_READBACK"))) {
        de_golden_fail(ctx, "DENG604 final-frame SSR readback failed");
        mg_close(&m);
        return;
    }
    if (strstr(o->scene, "reflections_") != NULL || strstr(o->scene, "gi_") != NULL) {
        const bool gi = strstr(o->scene, "gi_") != NULL;
        de_entity entity = DE_ENTITY_NULL;
        const de_query_term term = {
            .component = gi ? ids->irradiance_volume : ids->reflection_probe, .access = DE_READ};
        de_query query =
            de_query_create(m.world, &(de_query_desc){.terms = &term, .term_count = 1});
        de_query_each(m.world, query, mg_find_entity, &entity);
        de_query_destroy(m.world, query);
        de__render_world_wait_idle(m.r);
        de__probe_info probe = {0};
        de__irradiance_info volume = {0};
        bool ready = gi ? de__irradiance_get(m.r, entity.bits, &volume) && volume.ready
                        : de__probes_info(m.r, entity.bits, &probe) && probe.ready;
        if (!ready)
            de_golden_fail(ctx, "fixed warmup did not finish %s bake",
                           gi ? "irradiance" : "reflection");
        if (!capture_bake && ready && !(gi ? volume.baked : probe.baked))
            de_golden_fail(ctx, "canonical committed %s bake was not loaded",
                           gi ? "irradiance" : "reflection");
        if (capture_bake && ready) {
            de_result br = de_command_exec(gi ? "r.gi_bake" : "r.probes_bake");
            char path[128];
            snprintf(path, sizeof path, "dev:/%s-%016llx.%s", gi ? "gi" : "probe",
                     (unsigned long long)entity.bits, gi ? "degi" : "deprobe");
            void *blob = NULL;
            u64 count = 0;
            if (br != DE_OK || de_file_read_all(path, NULL, &blob, &count) != DE_OK)
                de_golden_fail(ctx, "capture bake: %s", mg_err());
            else {
                char canonical[1024];
                snprintf(canonical, sizeof canonical, "%s/bakes/%s", DE_GOLDEN_BASE_TIER_ASSETS,
                         gi ? "irradiance.degi" : "reflection.deprobe");
                const bool verify = getenv("DE_GOLDEN_VERIFY_BAKES") != NULL;
                FILE *f = fopen(canonical, verify ? "rb" : "wb");
                if (!f)
                    de_golden_fail(ctx, "opening canonical bake");
                else if (verify) {
                    u8 *expected = malloc((size_t)count);
                    bool same = expected && fread(expected, 1, (size_t)count, f) == count &&
                                fgetc(f) == EOF && memcmp(blob, expected, (size_t)count) == 0;
                    printf("  %s capture/bake: %llu bytes %s\n", gi ? "DEGI" : "DEPR",
                           (unsigned long long)count, same ? "exact" : "DIFFERENT");
                    if (!same)
                        de_golden_fail(ctx, "capture differs from canonical bake");
                    free(expected);
                    fclose(f);
                } else {
                    if (fwrite(blob, 1, (size_t)count, f) != count)
                        de_golden_fail(ctx, "writing bake");
                    fclose(f);
                }
                DE_FREE(de_heap_allocator(), blob, (size_t)count + 1u);
            }
        }
    }
    /* Into the harness target through an UPLOAD buffer the harness owns. */
    const de_gpu_buffer up =
        de_golden_upload_buffer(ctx, m.now, (u64)bytes, DE_USAGE_COPY_SRC, "materials pixels");
    if (DE_HANDLE_VALID(up)) {
        de_golden_target_barrier(ctx, DE_ACCESS_COPY_DST);
        de_gpu_cmd_copy_buffer_to_texture(ctx->cmd, ctx->target, 0, 0, up, 0, m.pitch);
    }
    mg_close(&m);
}

#else /* !DE_GOLDEN_COOK */

static void mg_run(de_golden_ctx *ctx, const mg_opts *o) {
    (void)o;
    de_golden_skip(ctx, "gallery test-side asset preparation is unavailable (DE_GOLDEN_COOK)");
}

#endif

DE_GOLDEN_SCENE(shadows_csm_medium) {
    mg_run(ctx, &(mg_opts){.scene = "scenes/shadows_csm", .quality = DE_QUALITY_MEDIUM});
}
DE_GOLDEN_SCENE(shadows_csm_high) {
    mg_run(ctx, &(mg_opts){.scene = "scenes/shadows_csm", .quality = DE_QUALITY_HIGH});
}
DE_GOLDEN_SCENE(shadows_csm_off) {
    mg_run(ctx, &(mg_opts){.scene = "scenes/shadows_csm", .quality = DE_QUALITY_OFF});
}
DE_GOLDEN_SCENE(shadows_local_medium) {
    mg_run(ctx, &(mg_opts){.scene = "scenes/shadows_local", .quality = DE_QUALITY_MEDIUM});
}
DE_GOLDEN_SCENE(shadows_local_high) {
    mg_run(ctx, &(mg_opts){.scene = "scenes/shadows_local", .quality = DE_QUALITY_HIGH});
}
DE_GOLDEN_SCENE(shadows_local_off) {
    mg_run(ctx, &(mg_opts){.scene = "scenes/shadows_local", .quality = DE_QUALITY_OFF});
}
DE_GOLDEN_SCENE(shadows_pcss_contact_medium) {
    mg_run(ctx, &(mg_opts){.scene = "scenes/shadows_pcss_contact", .quality = DE_QUALITY_MEDIUM});
}
DE_GOLDEN_SCENE(shadows_pcss_contact_high) {
    mg_run(ctx, &(mg_opts){.scene = "scenes/shadows_pcss_contact", .quality = DE_QUALITY_HIGH});
}
DE_GOLDEN_SCENE(shadows_pcss_contact_off) {
    mg_run(ctx, &(mg_opts){.scene = "scenes/shadows_pcss_contact", .quality = DE_QUALITY_OFF});
}
DE_GOLDEN_SCENE(ao_gtao_medium) {
    mg_run(ctx, &(mg_opts){.scene = "scenes/ao_gtao", .quality = DE_QUALITY_MEDIUM});
}
DE_GOLDEN_SCENE(ao_gtao_high) {
    mg_run(ctx, &(mg_opts){.scene = "scenes/ao_gtao", .quality = DE_QUALITY_HIGH});
}
DE_GOLDEN_SCENE(ao_gtao_off) {
    mg_run(ctx, &(mg_opts){.scene = "scenes/ao_gtao", .quality = DE_QUALITY_OFF});
}
DE_GOLDEN_SCENE(reflections_ssr_probes_medium) {
    mg_run(ctx, &(mg_opts){.scene = "scenes/reflections_ssr_probes", .quality = DE_QUALITY_MEDIUM});
}
DE_GOLDEN_SCENE(reflections_ssr_probes_high) {
    mg_run(ctx, &(mg_opts){.scene = "scenes/reflections_ssr_probes", .quality = DE_QUALITY_HIGH});
}
DE_GOLDEN_SCENE(reflections_ssr_probes_off) {
    mg_run(ctx, &(mg_opts){.scene = "scenes/reflections_ssr_probes", .quality = DE_QUALITY_OFF});
}
DE_GOLDEN_SCENE(gi_probes_ssgi_medium) {
    mg_run(ctx, &(mg_opts){.scene = "scenes/gi_probes_ssgi", .quality = DE_QUALITY_MEDIUM});
}
DE_GOLDEN_SCENE(gi_probes_ssgi_high) {
    mg_run(ctx, &(mg_opts){.scene = "scenes/gi_probes_ssgi", .quality = DE_QUALITY_HIGH});
}
DE_GOLDEN_SCENE(gi_probes_ssgi_off) {
    mg_run(ctx, &(mg_opts){.scene = "scenes/gi_probes_ssgi", .quality = DE_QUALITY_OFF});
}
DE_GOLDEN_SCENE(refraction_ss_medium) {
    mg_run(ctx, &(mg_opts){.scene = "scenes/refraction_ss", .quality = DE_QUALITY_MEDIUM});
}
DE_GOLDEN_SCENE(refraction_ss_high) {
    mg_run(ctx, &(mg_opts){.scene = "scenes/refraction_ss", .quality = DE_QUALITY_HIGH});
}
DE_GOLDEN_SCENE(refraction_ss_off) {
    mg_run(ctx, &(mg_opts){.scene = "scenes/refraction_ss", .quality = DE_QUALITY_OFF});
}
