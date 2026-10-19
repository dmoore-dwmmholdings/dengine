/* DENG-44 native renderer scene-AS acceptance. Shared fixture also serves RT effects. */
#include "scene_as_fixture.h"
TEST_F(scene_as, alpha_mask, rt_fixture) {
    de__rt_hit *hit = frame_rt(f);
    printf("alpha hits %g %g instances %u %u\n", hit[0].t, hit[1].t, hit[0].instance,
           hit[1].instance);
    EXPECT_TRUE(hit[0].t < 0);
    EXPECT_FLOAT_EQ(hit[1].t, 2, 0.001);
    EXPECT_EQ(hit[1].instance, 0u);
    g_mat[0].standard.base_color.w = 0.25f;
    EXPECT_TRUE(frame_rt(f)[1].t < 0);
    g_mat[0].standard.alpha_cutoff = 0.1f;
    EXPECT_FLOAT_EQ(frame_rt(f)[1].t, 2, 0.001);
    g_mat[0].alpha_mode = DE_COOKED_ALPHA_OPAQUE;
    EXPECT_FLOAT_EQ(frame_rt(f)[0].t, 2, 0.001);
}

TEST_F(scene_as, tlas_filter, rt_fixture) {
    EXPECT_FLOAT_EQ(frame_rt(f)[1].t, 2, 0.001);
    f->w.f.instances.flags[0] &= ~DE__RW_INST_VISIBLE_IN_RT;
    EXPECT_TRUE(frame_rt(f)[1].t < 0);
    f->w.f.instances.flags[0] |= DE__RW_INST_VISIBLE_IN_RT;
    de_view_set_layer_mask(f->r, f->view, 2);
    EXPECT_TRUE(frame_rt(f)[1].t < 0);
    de_view_set_layer_mask(f->r, f->view, 1u << 24);
    f->w.f.instances.flags[0] &= ~DE__RW_INST_CAST_SHADOWS;
    f->flags = DE__RT_CAST_SHADOWS;
    EXPECT_TRUE(frame_rt(f)[1].t < 0);
    f->flags = 0;
    EXPECT_FLOAT_EQ(frame_rt(f)[1].t, 2, 0.001);
    /* Stable GPU-scene custom index, including holes in its slots. */
    world_set(&f->w, 3, 1, at(3, 0, 0), 1);
    f->w.f.instances.entity[0] = 0;
    f->w.f.instances.flags[3] |= DE__RW_INST_VISIBLE_IN_RT;
    f->w.f.instances.layer_mask[3] = 1u << 24;
    f->w.f.removed_instances[0] = 0;
    f->w.f.removed_instance_count = 1;
    EXPECT_EQ(frame_rt(f)[1].instance, 3u);
}
TEST_F(scene_as, blas_per_lod, rt_fixture) {
    de__rt_scene *scene = f->r->rt_scene;
    EXPECT_EQ(scene->mesh_count, 1u);
    EXPECT_EQ(scene->blas_count, 2u);
    EXPECT_TRUE(scene->meshes[0].blas[0].id != scene->meshes[0].blas[1].id);
    const u64 before = scene->bytes;
    EXPECT_EQ(frame_rt(f)[1].lod, 0u);
    (void)frame_rt(f);
    (void)frame_rt(f);
    EXPECT_EQ(scene->compacted_count, 2u);
    EXPECT_TRUE(scene->bytes <= before);
    /* Resident-LOD clamp: the raster metric asks for LOD0; only LOD1 is resident. */
    de_gpu_accel saved = scene->meshes[0].blas[0];
    scene->meshes[0].blas[0].id = 0;
    EXPECT_EQ(frame_rt(f)[1].lod, 1u);
    scene->meshes[0].blas[0] = saved;
    fmesh second = {0};
    mesh_plane(&second, f->d, 1, 1, 2);
    de_render_mesh *m = &second.rm;
    m->buffers[DE_RENDER_MESH_INDICES] = f->indices;
    m->bindless[DE_RENDER_MESH_INDICES] = de_gpu_buffer_bindless(f->d, f->indices);
    m->index_count = g_mesh[0].rm.index_count;
    m->lod_count = 2;
    memcpy(m->lods, g_mesh[0].rm.lods, sizeof m->lods);
    m->lod_submeshes = f->ranges;
    m->submeshes = &f->submesh;
    ASSERT_EQ(de__rt_mesh_publish(scene, m), DE_OK);
    EXPECT_EQ(scene->blas_count, 4u);
    de__rt_mesh_evict(scene, m);
    EXPECT_EQ(scene->blas_count, 2u);
    mesh_free(&second, f->d);
}
TEST_F(scene_as, base_tier_noop, rt_fixture) {
    de__view_unregister_stage(f->r, trace_stage, f);
    de_render_settings settings = {.tier = DE_TIER_BASE, .render_scale = 1};
    de_renderer_set_settings(f->r, &settings);
    de__renderer_kick_prepare(f->r);
    ASSERT_TRUE(f->r->rt_scene == NULL);
    EXPECT_EQ(de__gpu_live_count(f->d, DE__GPU_ACCEL), 0u);
    EXPECT_EQ(de_renderer_stats(f->r)->rt_blas_bytes, 0u);
    ASSERT_EQ(de_renderer_render_frame(f->r), DE_OK);
    EXPECT_EQ(de__gpu_live_count(f->d, DE__GPU_ACCEL), 0u);
}
/* Actual asset ready/retire/reload hooks, quantized position decode and GPU mesh budget. */
static void pump_asset(rt_fixture *f) {
    de_render_uploader_flush(f->r->uploader);
    de_render_uploader_acquire_submit(f->r->uploader);
    de_asset_system_update(f->assets, NULL);
    ASSERT_EQ(de__rt_prepare(f->r), DE_OK);
}
TEST_F(scene_as, reload, rt_fixture) {
    de_fp_env_init_thread();
    ASSERT_EQ(de_fs_mount("scene_as", DE_SCENE_AS_TMP, DE_MOUNT_WRITABLE, 0), DE_OK);
    ASSERT_EQ(de_task_system_create(&(de_task_system_desc){.worker_count = 2}, &f->tasks), DE_OK);
    ASSERT_EQ(de_asset_system_create(&(de_asset_system_desc){.tasks = f->tasks,
                                                             .allocator = &f->alloc,
                                                             .io_threads = 1,
                                                             .cache_dir = "scene_as:/cache",
                                                             .hot_reload = true,
                                                             .reload_retire_frames = 2},
                                     &f->assets),
              DE_OK);
    de__asset_reload_set_watching(f->assets, false);
    ASSERT_EQ(de__render_register_asset_types_ex(f->assets,
                                                 &(de__render_assets_desc){.device = f->d,
                                                                           .allocator = &f->alloc,
                                                                           .ring_bytes = 1u << 20,
                                                                           .no_import = true},
                                                 &f->r->uploader),
              DE_OK);
    de__render_uploader_set_frame_fence(f->r->uploader, f->r->frame_fence);
    f->r->assets = f->assets;
    pump_asset(f);
    de_gpu_wait_idle(f->d);
    u32 before_live[DE__GPU_KIND_COUNT];
    for (u32 k = 0; k < DE__GPU_KIND_COUNT; ++k)
        before_live[k] = de__gpu_live_count(f->d, (de__gpu_kind)k);
    lt_blob blob = lt_dmsh(8, 0, 1);
    ASSERT_EQ(de_file_write_all_atomic("scene_as:/m.dmsh", blob.data, blob.size), DE_OK);
    const char manifest[] = "[assets]\n\"rt/mesh\" = { type=\"mesh\", source=\"m.dmsh\" }\n";
    ASSERT_EQ(de_file_write_all_atomic("scene_as:/assets.toml", manifest, sizeof manifest - 1),
              DE_OK);
    ASSERT_EQ(de_asset_manifest_load(f->assets, "scene_as:/assets.toml"), DE_OK);
    const de_asset_id id = de_asset_id_from("rt/mesh");
    const de_mesh handle = de_asset_mesh(f->assets, id);
    de_load_handle request = de_asset_load(f->assets, id, 0);
    u64 start = de_time_now_ns();
    while (de_asset_status(f->assets, request, NULL) != DE_LOAD_READY &&
           de_time_now_ns() - start < LT_WAIT_NS) {
        pump_asset(f);
        (void)de__asset_wake_block(f->assets, 2000000);
    }
    ASSERT_EQ(de_asset_status(f->assets, request, NULL), DE_LOAD_READY);
    pump_asset(f);
    ASSERT_EQ(f->r->rt_scene->mesh_count, 1u);
    const de_render_mesh *old = de_render_mesh_get(f->assets, handle);
    ASSERT_TRUE(old != NULL);
    EXPECT_TRUE(old->published_);
    EXPECT_EQ(f->r->rt_scene->meshes[0].source, old);
    u64 base = old->gpu_bytes;
    EXPECT_TRUE(de_asset_get_stats(f->assets).bytes_resident[1] > base);
    de_gpu_wait_idle(f->d);
    pump_asset(f);
    de_gpu_wait_idle(f->d);
    pump_asset(f);
    EXPECT_EQ(f->r->rt_scene->compacted_count, 1u);
    EXPECT_EQ(de_asset_get_stats(f->assets).bytes_resident[1], base + f->r->rt_scene->bytes);
    /* Replace the cooked mesh, keeping its public handle; retired old BLAS remains fence-safe. */
    lt_blob replacement = lt_dmsh(12, 0, 2);
    ASSERT_EQ(de_file_write_all_atomic("scene_as:/m.dmsh", replacement.data, replacement.size),
              DE_OK);
    de__asset_reload_on_change(f->assets, "scene_as:/m.dmsh", DE_WATCH_MODIFIED, false);
    start = de_time_now_ns();
    while (de_render_mesh_get(f->assets, handle) == old && de_time_now_ns() - start < LT_WAIT_NS) {
        pump_asset(f);
        (void)de__asset_wake_block(f->assets, 2000000);
    }
    ASSERT_TRUE(de_render_mesh_get(f->assets, handle) != old);
    for (u32 i = 0; i < 5; ++i) {
        pump_asset(f);
        de_gpu_wait_idle(f->d);
    }
    ASSERT_EQ(f->r->rt_scene->mesh_count, 1u);
    const de_render_mesh *current = de_render_mesh_get(f->assets, handle);
    EXPECT_EQ(f->r->rt_scene->meshes[0].source, current);
    EXPECT_EQ(de_asset_get_stats(f->assets).bytes_resident[1],
              current->gpu_bytes + f->r->rt_scene->bytes);
    de_asset_release(f->assets, request);
    de_asset_flush_unused(f->assets);
    pump_asset(f);
    EXPECT_EQ(f->r->rt_scene->mesh_count, 0u);
    EXPECT_EQ(de_asset_get_stats(f->assets).bytes_resident[1], 0u);
    de_gpu_wait_idle(f->d);
    for (u32 k = 0; k < DE__GPU_KIND_COUNT; ++k)
        EXPECT_EQ(de__gpu_live_count(f->d, (de__gpu_kind)k), before_live[k]);
    const de_tracking_stats warm = de_tracking_allocator_stats(&f->track);
    for (u32 i = 0; i < 8; ++i)
        ASSERT_EQ(de__rt_prepare(f->r), DE_OK);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).alloc_count, warm.alloc_count);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).realloc_count, warm.realloc_count);
    lt_free(&blob);
    lt_free(&replacement);
}
