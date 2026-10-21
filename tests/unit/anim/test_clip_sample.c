#include "anim_fixture.h"
#include "de_alloc_guard.h"
TEST_F(anim, sample_keys_interpolation, anim_fixture) {
    load_assets(f, 2);
    de_cooked_skeleton_view skeleton;
    de_cooked_clip_view clip;
    const char *why = NULL;
    ASSERT_TRUE(de_cooked_skeleton_parse(f->skel_blob.data, f->skel_blob.bytes, &skeleton, &why));
    ASSERT_TRUE(de_cooked_clip_parse(f->clip_blob.data, f->clip_blob.bytes, &clip, &why));
    de_cooked_joint_pose pose[2];
    for (u32 i = 0; i < 5; ++i) {
        f32 time = i * .25f;
        ASSERT_TRUE(de__anim_sample(&clip, &skeleton, time, pose));
        EXPECT_TRUE(fabsf(pose[0].t[0] - time * 2) < 1e-4f);
        EXPECT_TRUE(fabsf(pose[0].s[0] - (1 + time)) < 1e-4f);
        de_cooked_clip_track rotation = de_cooked_clip_get_track(&clip, 1);
        f32 a[4], b[4], reference[4];
        de_cooked_clip_key_value(&rotation, 0, a);
        de_cooked_clip_key_value(&rotation, 1, b);
        de_cooked_clip_nlerp(a, b, time, reference);
        f64 dot = 0, length_a = 0, length_b = 0;
        for (u32 c = 0; c < 4; ++c) {
            dot += (f64)pose[0].r[c] * reference[c];
            length_a += (f64)pose[0].r[c] * pose[0].r[c];
            length_b += (f64)reference[c] * reference[c];
        }
        f64 angle = 2 * acos(fmin(1, fabs(dot) / sqrt(length_a * length_b)));
        EXPECT_TRUE(angle < .01 * .01745329252);
    }
}
TEST_F(anim, loop_speed_and_end_event, anim_fixture) {
    load_assets(f, 2);
    de_entity entity = spawn_animator(f);
    DeAnimator *a = de_entity_get(f->world, entity, de_anim_component(f->anim));
    a->speed = 100.3f;
    a->time = .7f;
    anim_frame(f, 1);
    a = de_entity_get(f->world, entity, de_anim_component(f->anim));
    EXPECT_TRUE(fabsf(a->time) < 1e-4f);
    a->speed = -2.5f;
    a->time = .1f;
    anim_frame(f, 1);
    a = de_entity_get(f->world, entity, de_anim_component(f->anim));
    EXPECT_TRUE(fabsf(a->time - .6f) < 1e-5f);
    a->loop = false;
    a->speed = 1;
    a->time = .9f;
    anim_frame(f, .2f);
    a = de_entity_get(f->world, entity, de_anim_component(f->anim));
    EXPECT_FALSE(a->playing);
    EXPECT_TRUE(a->time == 1);
    de_event_reader reader = de_event_reader_create(f->world, de_anim_event_type(f->anim));
    const de_anim_event *event = de_event_next(f->world, &reader);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, DE_ANIM_EV_CLIP_END);
    EXPECT_EQ(event->entity.bits, entity.bits);
    anim_frame(f, .2f);
    EXPECT_FALSE(de_event_next(f->world, &reader));
}
TEST_F(anim, hierarchy_and_double_world_bone, anim_fixture) {
    load_assets(f, 3);
    de_entity entity = spawn_animator(f);
    DeAnimator *a = de_entity_get(f->world, entity, de_anim_component(f->anim));
    a->playing = false;
    a->time = 0;
    DeTransform *transform =
        de_entity_get(f->world, entity, de_builtin_components(f->world).transform);
    transform->position = (de_dvec3){1000000000.25, 2, 3};
    anim_frame(f, 0);
    de_transform bone;
    ASSERT_TRUE(de_anim_bone_transform(f->world, entity, de_name_from("joint2"), &bone));
    de__anim_entry *entry = &f->anim->entries[de_entity_index(entity)];
    EXPECT_TRUE(fabs(bone.position.x - (1000000000.25 + entry->model[2].m[9])) < 1e-6);
    EXPECT_TRUE(fabs(bone.position.y - 4) < 1e-4);
    EXPECT_TRUE(fabs(bone.position.z - 3) < 1e-4);
    for (u32 j = 0; j < 3; ++j)
        for (u32 k = 0; k < 12; ++k) {
            de_affine identity = de_affine_identity();
            EXPECT_TRUE(fabsf(entry->palette[j].m[k] - identity.m[k]) < 1e-3f);
        }
}
typedef struct stage_probe {
    anim_fixture *fixture;
    de_entity entity;
    u32 count;
    de_result mutation_result;
    de__anim_stage order[DE_ANIM_STAGE_COUNT];
} stage_probe;
typedef struct stage_probe_hook {
    stage_probe *probe;
    de__anim_stage stage;
} stage_probe_hook;
static void probe_stage(de_anim_system *s, void *user) {
    stage_probe_hook *hook = user;
    stage_probe *p = hook->probe;
    ASSERT_TRUE(p->count < DE_ANIM_STAGE_COUNT);
    p->order[p->count++] = hook->stage;
    ASSERT_EQ(s->job_count, 1u);
    de__anim_entry *e = &s->entries[s->jobs[0]];
    ASSERT_TRUE(e->ready);
    if (hook->stage == DE_ANIM_STAGE_SAMPLE) {
        /* Actual clip sampling has already run, not merely a stage notification. */
        EXPECT_TRUE(fabsf(e->local.translations[0].x - .5f) < 1e-5f);
        EXPECT_TRUE(e->local.root_translation.x == 0 && e->local.root_rotation.w == 1);
        p->mutation_result = de_anim__register_stage(s, DE_ANIM_STAGE_BLEND, probe_stage, user);
    } else if (hook->stage == DE_ANIM_STAGE_BLEND) {
        e->local.translations[0].x += 1;
        e->local.root_translation.x = .125f;
    } else if (hook->stage == DE_ANIM_STAGE_ROOT_MOTION) {
        DeTransform *t = de_entity_get(p->fixture->world, p->entity, s->builtins.transform);
        t->position.x = 1000000000.25;
    } else if (hook->stage == DE_ANIM_STAGE_IK) {
        e->local.translations[0].y = 2;
    } else {
        EXPECT_TRUE(e->finalized);
        EXPECT_TRUE(e->local.root_translation.x == .125f);
        EXPECT_TRUE(fabsf(e->model[0].m[9] - 1.5f) < 1e-5f);
        EXPECT_TRUE(fabsf(e->model[0].m[10] - 2) < 1e-5f);
    }
}
TEST_F(anim, postupdate_pipeline_precedes_transform_and_finalizes_output, anim_fixture) {
    load_assets(f, 2);
    de_entity entity = spawn_animator(f);
    stage_probe probe = {.fixture = f, .entity = entity};
    stage_probe_hook hooks[DE_ANIM_STAGE_COUNT];
    for (u32 i = 0; i < DE_ANIM_STAGE_COUNT; ++i) {
        hooks[i] = (stage_probe_hook){&probe, (de__anim_stage)i};
        ASSERT_EQ(de_anim__register_stage(f->anim, (de__anim_stage)i, probe_stage, &hooks[i]),
                  DE_OK);
    }
    EXPECT_EQ(de_anim__register_stage(f->anim, DE_ANIM_STAGE_SAMPLE, probe_stage, &hooks[0]),
              DE_ERR_ALREADY_EXISTS);
    EXPECT_EQ(de_anim__register_stage(f->anim, DE_ANIM_STAGE_COUNT, probe_stage, &hooks[0]),
              DE_ERR_INVALID_ARGUMENT);
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_UPDATE, .25f, 0), DE_OK);
    EXPECT_EQ(probe.count, 0u);
    const DeAnimator *a = de_entity_get_const(f->world, entity, de_anim_component(f->anim));
    EXPECT_TRUE(a->time == 0.f);
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_POST_UPDATE, .25f, 0), DE_OK);
    EXPECT_EQ(probe.count, (u32)DE_ANIM_STAGE_COUNT);
    for (u32 i = 0; i < probe.count; ++i)
        EXPECT_EQ(probe.order[i], (de__anim_stage)i);
    EXPECT_EQ(probe.mutation_result, DE_ERR_INVALID_ARGUMENT);
    /* Newly added global transforms become visible at the PostUpdate sync point. */
    de_transform bone;
    ASSERT_TRUE(de_anim_bone_transform(f->world, entity, de_name_from("joint0"), &bone));
    EXPECT_TRUE(fabs(bone.position.x - 1000000001.75) < 1e-6);
    EXPECT_TRUE(fabs(bone.position.y - 2) < 1e-5);
}
TEST_F(anim, no_alloc_thousand_sixty_bones, anim_fixture) {
    load_assets(f, 60);
    for (u32 i = 0; i < 1000; ++i)
        (void)spawn_animator(f);
    for (u32 i = 0; i < 8; ++i)
        anim_frame(f, 1.f / 60);
    de_alloc_guard guard = {0};
    de_alloc_guard_begin(&guard);
    const u32 frames = 1000;
    u64 start = de_time_now_ns();
    for (u32 i = 0; i < frames; ++i)
        anim_frame(f, 1.f / 60);
    u64 elapsed = de_time_now_ns() - start;
    de_alloc_guard_end(&guard);
    printf("1000x60 task sampled/finalized mean %.4fms, allocation offences %llu\n",
           (double)elapsed / ((double)frames * 1e6),
           (unsigned long long)de_alloc_guard_offences(&guard));
    DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
    /* Only the hardware-checked M1 job qualifies the original S27 budget.
     * Compare integer elapsed time, not the rounded diagnostic above. */
    const char *qualify_m1 = getenv("DE_ANIM_M1_ACCEPTANCE");
    if (qualify_m1 && strcmp(qualify_m1, "1") == 0)
        EXPECT_LE(elapsed, (u64)frames * UINT64_C(2000000));
}

#if DE_ANIM_HAS_COOKER && DE_HAS_RENDER && DE_HAS_GPU_METAL
#include "cook/lib/de_cooker.h"
#include "cook/importers/model/model_import.h"
#include "asset/manifest_internal.h"
#include "render/renderer_internal.h"
#include "render/components_internal.h"
#include "render/world/render_world.h"
#include "render/skin/skin_internal.h"
#include "render/loaders/render_assets.h"
#include "../render/loader_test_util.h"
static void native_frame(anim_fixture *f, f32 dt) {
    anim_frame(f, dt);
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_EXTRACT, dt, 0), DE_OK);
    ASSERT_EQ(de__render_extract_and_kick(f->renderer, f->world), DE_OK);
    de_gpu_wait_idle(f->device);
}
static const u8 *native_readback(anim_fixture *f) {
    if (!f->readback.id)
        f->readback =
            de_gpu_buffer_create(f->device, &(de_gpu_buffer_desc){.size = 256 * 256 * 4,
                                                                  .usage = DE_USAGE_COPY_DST,
                                                                  .memory = DE_MEM_READBACK});
    de_gpu_cmd *cmd = de_gpu_cmd_begin(f->device, DE_QUEUE_GRAPHICS, "Animated glTF readback");
    de_gpu_barrier b = {
        .texture = f->target, .before = DE_ACCESS_SHADER_READ, .after = DE_ACCESS_COPY_SRC};
    de_gpu_cmd_barriers(cmd, &b, 1);
    de_gpu_cmd_copy_texture_to_buffer(cmd, f->readback, 0, 1024, f->target, 0, 0);
    b.before = DE_ACCESS_COPY_SRC;
    b.after = DE_ACCESS_SHADER_READ;
    de_gpu_cmd_barriers(cmd, &b, 1);
    ASSERT_EQ(de_gpu_queue_submit(f->device, DE_QUEUE_GRAPHICS,
                                  &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
              DE_OK);
    de_gpu_wait_idle(f->device);
    return de_gpu_buffer_map(f->device, f->readback);
}
TEST_F(anim, cooked_cesium_gpu_playback, anim_fixture) {
    ASSERT_EQ(de_fs_mount("cesium", DE_ANIM_DATA_DIR, 0, 0), DE_OK);
    de_cooker *cooker;
    ASSERT_EQ(de_cooker_create(&(de_cooker_desc){.allocator = &f->allocator}, &cooker), DE_OK);
    ASSERT_EQ(de_cooker_register_type(
                  cooker, &(de_asset_type_desc){.name = "model",
                                                .extensions = de_model_import_extensions,
                                                .extension_count = DE_MODEL_IMPORT_EXTENSION_COUNT,
                                                .importer_version = DE_MODEL_IMPORTER_VERSION,
                                                .import = de_model_import}),
              DE_OK);
    de__manifest_asset input = {.id = "test/cesium",
                                .type = "model",
                                .source = "CesiumMan.glb",
                                .file = "cesium:/assets.toml"};
    de_cook_result result;
    ASSERT_EQ(de_cooker_cook(cooker, &(de_cook_input){.asset = &input, .asset_root = "cesium:/"},
                             &result),
              DE_OK);
    const de_cook_subasset *mesh = NULL, *skeleton = NULL, *clip = NULL;
    for (u32 i = 0; i < result.subasset_count; ++i) {
        const de_cook_subasset *sub = &result.subassets[i];
        if (!strcmp(sub->type, "mesh") && !mesh)
            mesh = sub;
        if (!strcmp(sub->type, "skeleton") && !skeleton)
            skeleton = sub;
        if (!strcmp(sub->type, "anim_clip") && !clip)
            clip = sub;
    }
    ASSERT_TRUE(mesh && skeleton && clip);
    ASSERT_EQ(de_file_write_all_atomic("animtest:/cesium.dmesh", mesh->data, mesh->size), DE_OK);
    ASSERT_EQ(de_file_write_all_atomic("animtest:/cesium.dskl", skeleton->data, skeleton->size),
              DE_OK);
    ASSERT_EQ(de_file_write_all_atomic("animtest:/cesium.dclp", clip->data, clip->size), DE_OK);
    de_cook_result_free(&result);
    de_cooker_destroy(cooker);
    lt_blob material = lt_dmat(NULL, NULL, 0, 1);
    de_cooked_put_u32(material.data + 8, DE_COOKED_MAT_DOUBLE_SIDED | DE_COOKED_MAT_UNLIT);
    de_cooked_put_u32(material.data + 12, DE_COOKED_ALPHA_OPAQUE);
    ASSERT_EQ(de_file_write_all_atomic("animtest:/white.dmat", material.data, material.size),
              DE_OK);
    lt_free(&material);
    const char *manifest = "[assets]\n\"test/mesh\"={type=\"mesh\",source=\"cesium.dmesh\"}\n"
                           "\"test/skel\"={type=\"skeleton\",source=\"cesium.dskl\"}\n\"test/"
                           "anim\"={type=\"anim_clip\",source=\"cesium.dclp\"}\n"
                           "\"test/white\"={type=\"material\",source=\"white.dmat\"}\n";
    ASSERT_EQ(de_file_write_all_atomic("animtest:/native.toml", manifest, strlen(manifest)), DE_OK);
    ASSERT_EQ(de_asset_manifest_load(f->assets, "animtest:/native.toml"), DE_OK);
    ASSERT_EQ(de_gpu_device_create(&(de_gpu_device_desc){.backend = DE_GPU_METAL,
                                                         .debug_layer = true,
                                                         .allocator = &f->allocator,
                                                         .pipeline_cache_dir = ""},
                                   &f->device),
              DE_OK);
    ASSERT_EQ(de_renderer_create(&(de_renderer_desc){.device = f->device,
                                                     .assets = f->assets,
                                                     .types = f->types,
                                                     .allocator = &f->allocator},
                                 &f->renderer),
              DE_OK);
    de_renderer_attach(f->renderer, f->world);
    de_anim_set_renderer(f->anim, f->renderer);
    de_cvar pipelined = de_cvar_find("r.pipelined");
    ASSERT_EQ(de_cvar_set(pipelined, "0"), DE_OK);
    de_render_settings settings = de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_LOW);
    de_renderer_set_settings(f->renderer, &settings);
    de_load_handle mesh_load =
        de_asset_load(f->assets, de_asset_id_from("test/mesh"), DE_LOAD_NO_EVENTS);
    de_load_handle material_load =
        de_asset_load(f->assets, de_asset_id_from("test/white"), DE_LOAD_NO_EVENTS);
    f->skeleton_load = de_asset_load(f->assets, de_asset_id_from("test/skel"), DE_LOAD_NO_EVENTS);
    f->clip_load = de_asset_load(f->assets, de_asset_id_from("test/anim"), DE_LOAD_NO_EVENTS);
    u64 deadline = de_time_now_ns() + 10000000000ull;
    while (de_time_now_ns() < deadline) {
        de_render_uploader_flush(f->renderer->uploader);
        de_render_uploader_acquire_submit(f->renderer->uploader);
        de_asset_system_update(f->assets, NULL);
        if (de_asset_status(f->assets, mesh_load, NULL) == DE_LOAD_READY &&
            de_asset_status(f->assets, material_load, NULL) == DE_LOAD_READY &&
            de_asset_status(f->assets, f->skeleton_load, NULL) == DE_LOAD_READY &&
            de_asset_status(f->assets, f->clip_load, NULL) == DE_LOAD_READY)
            break;
        de_task_pump_main(f->tasks, 64);
        de_sleep_ns(1000000u);
    }
    ASSERT_EQ(de_asset_status(f->assets, mesh_load, NULL), DE_LOAD_READY);
    ASSERT_EQ(de_asset_status(f->assets, material_load, NULL), DE_LOAD_READY);
    f->skeleton = de_anim_skeleton(f->anim, de_asset_id_from("test/skel"));
    f->clip = de_anim_clip_asset(f->anim, de_asset_id_from("test/anim"));
    de_mesh mesh_handle = de_asset_mesh(f->assets, de_asset_id_from("test/mesh"));
    const de_render_mesh *source = de_render_mesh_get(f->assets, mesh_handle);
    ASSERT_TRUE(source);
    de_entity entity = spawn_animator(f);
    const DeRenderComponents *ids = de__render_components(f->world);
    DeMeshRenderer *mr = de_entity_add(f->world, entity, ids->mesh_renderer);
    ASSERT_TRUE(mr);
    mr->mesh = mesh_handle;
    for (u32 i = 0; i < 8; ++i)
        mr->materials[i] = de_asset_material(f->assets, de_asset_id_from("test/white"));
    de_entity_mark_changed(f->world, entity, ids->mesh_renderer);
    f->target = de_gpu_texture_create(
        f->device, &(de_gpu_texture_desc){.width = 256,
                                          .height = 256,
                                          .format = DE_FMT_RGBA8_UNORM,
                                          .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED |
                                                   DE_USAGE_COPY_SRC});
    de_view view = de_view_create(
        f->renderer, &(de_view_desc){.target = f->target, .name = "CesiumMan playback"});
    de_builtin_component_ids bi = de_builtin_components(f->world);
    de_entity camera;
    de_component components[] = {bi.transform, ids->camera};
    ASSERT_EQ(de_entity_spawn_with(f->world, components, 2, &camera), DE_OK);
    de_vec3 center = {(source->bounds_min[0] + source->bounds_max[0]) * .5f,
                      (source->bounds_min[1] + source->bounds_max[1]) * .5f,
                      (source->bounds_min[2] + source->bounds_max[2]) * .5f};
    f32 radius = fmaxf(source->bounds_max[2] - source->bounds_min[2],
                       source->bounds_max[0] - source->bounds_min[0]);
    de_vec3 eye = {center.x, center.y - radius * 2, center.z + radius * .25f};
    de_mat4 view_matrix = de_m4_look_at(eye, center, (de_vec3){0, 0, 1}),
            inverse = de_m4_inverse_affine(&view_matrix);
    de_affine affine = de_affine_from_m4(&inverse);
    de_transform transform;
    ASSERT_TRUE(de_transform_from_affine(&affine, (de_dvec3){0}, &transform));
    *(DeTransform *)de_entity_get(f->world, camera, bi.transform) = transform;
    *(DeCamera *)de_entity_get(f->world, camera, ids->camera) =
        (DeCamera){.view = view, .projection = DE_PROJ_PERSPECTIVE, .fov_y = .8f, .near_z = .01f};
    native_frame(f, 0);
    const u8 *pixels = native_readback(f);
    u8 *first = malloc(256 * 256 * 4);
    ASSERT_TRUE(first);
    memcpy(first, pixels, 256 * 256 * 4);
    native_frame(f, .4f);
    pixels = native_readback(f);
    u32 changed = 0, visible = 0;
    for (u32 i = 0; i < 256 * 256; ++i) {
        changed += memcmp(first + i * 4, pixels + i * 4, 3) != 0;
        visible += pixels[i * 4] > 32;
    }
    de__anim_entry *pose = &f->anim->entries[de_entity_index(entity)];
    ASSERT_EQ(pose->joints, 19u);
    u32 slot = de__rw_pool_find(de__rw_of(f->renderer), DE__RW_KIND_INSTANCE, entity);
    de__skin_entry *skin = de__skin_instance(f->renderer, slot);
    ASSERT_TRUE(skin && skin->initialized);
    printf("Cooked CesiumMan %u vertices/%u joints, %u visible pixels/%u changed pose pixels\n",
           source->vertex_count, pose->joints, visible, changed);
    EXPECT_TRUE(visible > 100);
    EXPECT_TRUE(changed > 20);
    ASSERT_EQ(de_file_write_all_atomic("animtest:/cesium-pose0.rgba", first, 256 * 256 * 4), DE_OK);
    ASSERT_EQ(de_file_write_all_atomic("animtest:/cesium-pose1.rgba", pixels, 256 * 256 * 4),
              DE_OK);
    free(first);
    de_asset_release(f->assets, mesh_load);
    de_asset_release(f->assets, material_load);
    ASSERT_EQ(de_cvar_set(pipelined, "1"), DE_OK);
}
#endif

#include "anim/anim_host.h"
TEST_F(anim, explicit_renderer_overrides_host_follow, anim_fixture) {
    de_renderer *host = (de_renderer *)(uintptr_t)1, *explicit = (de_renderer *)(uintptr_t)2;
    de__anim_bind_renderer(f->anim, host);
    ASSERT_TRUE(f->anim->renderer == host);
    de__anim_bind_renderer(f->anim, NULL);
    ASSERT_TRUE(f->anim->renderer == NULL);
    de_anim_set_renderer(f->anim, explicit);
    de__anim_bind_renderer(f->anim, NULL);
    ASSERT_TRUE(f->anim->renderer == explicit);
    de__anim_bind_renderer(f->anim, host);
    ASSERT_TRUE(f->anim->renderer == explicit);
    de_anim_set_renderer(f->anim, NULL);
    de__anim_bind_renderer(f->anim, host);
    ASSERT_TRUE(f->anim->renderer == NULL);
}
