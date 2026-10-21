/* DENG-180: fixed cooked S19 poses through the real animation and GPU skinning path.
 * The same scene can run against the pre-S27 player to establish an independent reference. */
#include "golden.h"
#include <dengine/de_anim.h>
#include <dengine/de_debug.h>
#include <dengine/de_platform.h>
#include <dengine/de_reflect.h>
#include <dengine/de_render.h>
#include <dengine/de_task.h>
#include "render/components_internal.h"
#include "render/renderer_internal.h"
#include "render/world/render_world.h"
#include "render/loaders/render_assets.h"
#include "render/formats/de_cooked_material.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#if defined(DE_GOLDEN_COOK)
#include "cook/lib/de_cooker.h"
#include "cook/importers/model/model_import.h"
#include "asset/manifest_internal.h"

typedef struct anim_scene {
    de_golden_ctx *ctx;
    de_task_system *tasks;
    de_type_registry *types;
    de_world *world;
    de_asset_system *assets;
    de_anim_system *anim;
    de_renderer *renderer;
    de_entity actor;
    de_load_handle loads[4];
    de_gpu_texture target;
    de_gpu_buffer readback;
    bool source_mounted, cache_mounted;
} anim_scene;

static de_result cook_assets(void) {
    de_cooker *cooker = NULL;
    DE_TRY(de_cooker_create(&(de_cooker_desc){0}, &cooker));
    de_result r = de_cooker_register_type(
        cooker, &(de_asset_type_desc){.name = "model",
                                      .extensions = de_model_import_extensions,
                                      .extension_count = DE_MODEL_IMPORT_EXTENSION_COUNT,
                                      .importer_version = DE_MODEL_IMPORTER_VERSION,
                                      .import = de_model_import});
    de_cook_result result = {0};
    de__manifest_asset input = {.id = "golden/cesium",
                                .type = "model",
                                .source = "CesiumMan.glb",
                                .file = "animgoldsrc:/assets.toml"};
    if (r == DE_OK)
        r = de_cooker_cook(cooker, &(de_cook_input){.asset = &input, .asset_root = "animgoldsrc:/"},
                           &result);
    const char *types[] = {"mesh", "skeleton", "anim_clip"};
    const char *paths[] = {"animgold:/mesh.dmesh", "animgold:/skeleton.dskl",
                           "animgold:/clip.dclp"};
    for (u32 k = 0; k < 3 && r == DE_OK; ++k) {
        const de_cook_subasset *found = NULL;
        for (u32 i = 0; i < result.subasset_count && !found; ++i)
            if (!strcmp(result.subassets[i].type, types[k]))
                found = &result.subassets[i];
        r = found ? de_file_write_all_atomic(paths[k], found->data, found->size) : DE_ERR_NOT_FOUND;
    }
    de_cook_result_free(&result);
    de_cooker_destroy(cooker);
    DE_TRY(r);
    u8 material[DE_COOKED_MATERIAL_HEADER_SIZE + DE_COOKED_MATERIAL_PARAMS_SIZE] = {0};
    memcpy(material, DE_COOKED_MATERIAL_MAGIC, 4);
    de_cooked_put_u32(material + 4, DE_COOKED_MATERIAL_VERSION);
    de_cooked_put_u32(material + 8, DE_COOKED_MAT_DOUBLE_SIDED | DE_COOKED_MAT_UNLIT);
    de_cooked_put_u32(material + 12, DE_COOKED_ALPHA_OPAQUE);
    de_cooked_put_u32(material + 16, DE_COOKED_MATERIAL_PARAMS_SIZE);
    de_cooked_put_u32(material + 24, sizeof material);
    de_cooked_material_params params = de_cooked_material_params_default();
    const f32 *values = (const f32 *)&params;
    for (u32 i = 0; i < DE_COOKED_MATERIAL_PARAM_FLOATS; ++i)
        de_cooked_put_f32(material + DE_COOKED_MATERIAL_HEADER_SIZE + 4 * i, values[i]);
    DE_TRY(de_file_write_all_atomic("animgold:/white.dmat", material, sizeof material));
    const char *manifest = "[assets]\n"
                           "\"golden/mesh\"={type=\"mesh\",source=\"mesh.dmesh\"}\n"
                           "\"golden/skeleton\"={type=\"skeleton\",source=\"skeleton.dskl\"}\n"
                           "\"golden/clip\"={type=\"anim_clip\",source=\"clip.dclp\"}\n"
                           "\"golden/white\"={type=\"material\",source=\"white.dmat\"}\n";
    return de_file_write_all_atomic("animgold:/assets.toml", manifest, strlen(manifest));
}
static de_result setup(anim_scene *s) {
    de_fp_env_init_thread();
    DE_TRY(de_fs_mount("animgoldsrc", DE_GOLDEN_ANIM_SOURCE, DE_MOUNT_READ_ONLY, 0));
    s->source_mounted = true;
    DE_TRY(de_fs_mount("animgold", DE_GOLDEN_ANIM_TMP, DE_MOUNT_WRITABLE, 0));
    s->cache_mounted = true;
    DE_TRY(cook_assets());
    DE_TRY(de_task_system_create(&(de_task_system_desc){.worker_count = 3}, &s->tasks));
    DE_TRY(de_type_registry_create(NULL, &s->types));
    DE_TRY(de_world_create(&(de_world_desc){.types = s->types, .tasks = s->tasks}, &s->world));
    DE_TRY(de_asset_system_create(
        &(de_asset_system_desc){
            .tasks = s->tasks, .types = s->types, .io_threads = 1, .cache_dir = "animgold:/cache"},
        &s->assets));
    DE_TRY(de_anim_create(NULL, s->assets, &s->anim));
    de_anim_set_task_system(s->anim, s->tasks);
    de_anim_attach(s->anim, s->world);
    DE_TRY(de_renderer_create(
        &(de_renderer_desc){.device = s->ctx->device, .assets = s->assets, .types = s->types},
        &s->renderer));
    de_renderer_attach(s->renderer, s->world);
    de_anim_set_renderer(s->anim, s->renderer);
    DE_TRY(de_cvar_set(de_cvar_find("r.pipelined"), "0"));
    de_render_settings settings = de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_LOW);
    settings.render_scale = 1;
    settings.ao = settings.reflections = settings.gi = settings.sss = settings.shadows =
        DE_QUALITY_OFF;
    de_renderer_set_settings(s->renderer, &settings);
    DE_TRY(de_asset_manifest_load(s->assets, "animgold:/assets.toml"));
    const char *names[] = {"golden/mesh", "golden/skeleton", "golden/clip", "golden/white"};
    for (u32 i = 0; i < 4; ++i)
        s->loads[i] = de_asset_load(s->assets, de_asset_id_from(names[i]), DE_LOAD_NO_EVENTS);
    u64 deadline = de_time_now_ns() + 120000000000ull;
    for (;;) {
        de_render_uploader_flush(s->renderer->uploader);
        de_render_uploader_acquire_submit(s->renderer->uploader);
        de_asset_system_update(s->assets, NULL);
        bool ready = true;
        for (u32 i = 0; i < 4; ++i) {
            de_load_state status = de_asset_status(s->assets, s->loads[i], NULL);
            if (status == DE_LOAD_FAILED)
                return DE_ERR_IO;
            ready &= status == DE_LOAD_READY;
        }
        if (ready)
            break;
        if (de_time_now_ns() >= deadline)
            return DE_ERR_TIMEOUT;
        de_task_pump_main(s->tasks, 64);
        de_sleep_ns(1000000);
    }
    const DeRenderComponents *ids = de__render_components(s->world);
    de_builtin_component_ids bi = de_builtin_components(s->world);
    de_component components[] = {bi.transform, ids->mesh_renderer, de_anim_component(s->anim)};
    DE_TRY(de_entity_spawn_with(s->world, components, 3, &s->actor));
    *(DeTransform *)de_entity_get(s->world, s->actor, bi.transform) = de_transform_identity();
    *(DeAnimator *)de_entity_get(s->world, s->actor, components[2]) =
        (DeAnimator){.skeleton = de_anim_skeleton(s->anim, de_asset_id_from(names[1])),
                     .clip = de_anim_clip_asset(s->anim, de_asset_id_from(names[2])),
                     .speed = 1,
                     .playing = true,
                     .loop = true};
    DeMeshRenderer *mesh = de_entity_get(s->world, s->actor, ids->mesh_renderer);
    mesh->mesh = de_asset_mesh(s->assets, de_asset_id_from(names[0]));
    for (u32 i = 0; i < 8; ++i)
        mesh->materials[i] = de_asset_material(s->assets, de_asset_id_from(names[3]));
    const de_render_mesh *source = de_render_mesh_get(s->assets, mesh->mesh);
    if (!source)
        return DE_ERR_NOT_FOUND;
    s->target = de_gpu_texture_create(
        s->ctx->device, &(de_gpu_texture_desc){.width = 256,
                                               .height = 256,
                                               .format = DE_FMT_RGBA8_UNORM,
                                               .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED |
                                                        DE_USAGE_COPY_SRC});
    s->readback =
        de_gpu_buffer_create(s->ctx->device, &(de_gpu_buffer_desc){.size = 256 * 256 * 4,
                                                                   .usage = DE_USAGE_COPY_DST,
                                                                   .memory = DE_MEM_READBACK});
    if (!s->target.id || !s->readback.id)
        return DE_ERR_OUT_OF_MEMORY;
    de_view view = de_view_create(s->renderer,
                                  &(de_view_desc){.target = s->target, .name = "S19 pose golden"});
    de_entity camera;
    de_component camera_components[] = {bi.transform, ids->camera};
    DE_TRY(de_entity_spawn_with(s->world, camera_components, 2, &camera));
    de_vec3 center = {(source->bounds_min[0] + source->bounds_max[0]) * .5f,
                      (source->bounds_min[1] + source->bounds_max[1]) * .5f,
                      (source->bounds_min[2] + source->bounds_max[2]) * .5f};
    f32 radius = fmaxf(source->bounds_max[2] - source->bounds_min[2],
                       source->bounds_max[0] - source->bounds_min[0]);
    de_vec3 eye = {center.x, center.y - radius * 2, center.z + radius * .25f};
    de_mat4 vm = de_m4_look_at(eye, center, (de_vec3){0, 0, 1});
    de_mat4 inverse = de_m4_inverse_affine(&vm);
    de_affine affine = de_affine_from_m4(&inverse);
    de_transform transform;
    if (!de_transform_from_affine(&affine, (de_dvec3){0}, &transform))
        return DE_ERR_INVALID_ARGUMENT;
    *(DeTransform *)de_entity_get(s->world, camera, bi.transform) = transform;
    *(DeCamera *)de_entity_get(s->world, camera, ids->camera) =
        (DeCamera){.view = view,
                   .projection = DE_PROJ_PERSPECTIVE,
                   .fov_y = .8f,
                   .near_z = .01f,
                   .exposure_ev100 = 0};
    return DE_OK;
}
static de_result pose_pixels(anim_scene *s, f32 time, u8 *output) {
    ((DeAnimator *)de_entity_get(s->world, s->actor, de_anim_component(s->anim)))->time = time;
    /* Zero dt fixes the pose on both the legacy Update and staged PostUpdate players. */
    DE_TRY(de_world_run_schedule(s->world, DE_SCHEDULE_UPDATE, 0, 0));
    DE_TRY(de_world_run_schedule(s->world, DE_SCHEDULE_POST_UPDATE, 0, 0));
    DE_TRY(de_world_run_schedule(s->world, DE_SCHEDULE_EXTRACT, 0, 0));
    DE_TRY(de__render_extract_and_kick(s->renderer, s->world));
    de_gpu_wait_idle(s->ctx->device);
    de_gpu_cmd *cmd = de_gpu_cmd_begin(s->ctx->device, DE_QUEUE_GRAPHICS, "S19 pose readback");
    if (!cmd)
        return DE_ERR_OUT_OF_MEMORY;
    de_gpu_barrier barrier = {
        .texture = s->target, .before = DE_ACCESS_SHADER_READ, .after = DE_ACCESS_COPY_SRC};
    de_gpu_cmd_barriers(cmd, &barrier, 1);
    de_gpu_cmd_copy_texture_to_buffer(cmd, s->readback, 0, 1024, s->target, 0, 0);
    barrier.before = DE_ACCESS_COPY_SRC;
    barrier.after = DE_ACCESS_SHADER_READ;
    de_gpu_cmd_barriers(cmd, &barrier, 1);
    DE_TRY(de_gpu_queue_submit(s->ctx->device, DE_QUEUE_GRAPHICS,
                               &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}));
    de_gpu_wait_idle(s->ctx->device);
    const u8 *pixels = de_gpu_buffer_map(s->ctx->device, s->readback);
    if (!pixels)
        return DE_ERR_OUT_OF_MEMORY;
    for (u32 y = 0; y < 256; ++y)
        memcpy(output + y * 2048, pixels + y * 1024, 1024);
    return DE_OK;
}
static void cleanup(anim_scene *s) {
    if (s->renderer)
        de__render_world_wait_idle(s->renderer);
    if (s->anim)
        de_anim_destroy(s->anim);
    if (s->assets) {
        for (u32 i = 0; i < 4; ++i)
            if (s->loads[i].id)
                de_asset_release(s->assets, s->loads[i]);
        de_asset_system_destroy(s->assets);
    }
    if (s->renderer)
        de_renderer_destroy(s->renderer);
    de_gpu_buffer_destroy(s->ctx->device, s->readback);
    de_gpu_texture_destroy(s->ctx->device, s->target);
    if (s->world)
        de_world_destroy(s->world);
    if (s->types)
        de_type_registry_destroy(s->types);
    if (s->tasks)
        de_task_system_destroy(s->tasks);
    if (s->cache_mounted)
        de_fs_unmount("animgold", DE_GOLDEN_ANIM_TMP);
    if (s->source_mounted)
        de_fs_unmount("animgoldsrc", DE_GOLDEN_ANIM_SOURCE);
}
#endif
static void scene(de_golden_ctx *ctx, bool frozen) {
#if defined(DE_GOLDEN_COOK)
    if (ctx->width != 512 || ctx->height != 256 || ctx->format != DE_FMT_RGBA8_UNORM) {
        de_golden_fail(ctx, "pose pair requires 512x256 rgba8");
        return;
    }
    anim_scene s = {.ctx = ctx};
    const de_gpu_buffer upload =
        de_golden_upload_buffer(ctx, NULL, 512 * 256 * 4, DE_USAGE_COPY_SRC, "S19 pose pair");
    if (!upload.id)
        return;
    u8 *pixels = de_gpu_buffer_map(ctx->device, upload);
    if (!pixels) {
        de_golden_fail(ctx, "pose-pair upload map");
        return;
    }
    de_result r = setup(&s);
    if (r == DE_OK)
        r = pose_pixels(&s, 0, pixels);
    if (r == DE_OK)
        r = pose_pixels(&s, frozen ? 0 : .4f, pixels + 1024);
    if (r == DE_OK) {
        u32 visible[2] = {0}, changed = 0;
        for (u32 y = 0; y < 256; ++y)
            for (u32 x = 0; x < 256; ++x) {
                const u8 *a = pixels + y * 2048 + x * 4, *b = a + 1024;
                visible[0] += a[0] > 32;
                visible[1] += b[0] > 32;
                changed += memcmp(a, b, 3) != 0;
            }
        printf("  Cesium pose pair: visible %u/%u, changed %u%s\n", visible[0], visible[1], changed,
               frozen ? " (intentional freeze)" : "");
        if (visible[0] < 1000 || visible[1] < 1000 || (!frozen && changed < 500))
            de_golden_fail(ctx, "missing skinned pose content");
        de_golden_target_barrier(ctx, DE_ACCESS_COPY_DST);
        de_gpu_cmd_copy_buffer_to_texture(ctx->cmd, ctx->target, 0, 0, upload, 0, 2048);
    } else {
        de_golden_fail(ctx, "pose-pair setup/render: %d", r);
    }
    cleanup(&s);
#else
    (void)frozen;
    de_golden_skip(ctx, "pose-pair scene requires the model cooker");
#endif
}
DE_GOLDEN_SCENE(anim_cesium_pose_pair) {
    scene(ctx, false);
}
DE_GOLDEN_SCENE(anim_cesium_frozen_selftest) {
    scene(ctx, true);
}
