#include "de_test.h"
#include "host/editor/editor_preview_material.h"
#include "asset/hot_reload_internal.h"
#include "asset/slots_internal.h"
#include "render/loaders/render_assets.h"
#include <dengine/de_gpu.h>

typedef struct material_preview {
    de_tracking_allocator track;
    de_allocator allocator;
    de_asset_system *assets;
    de_gpu_device *device;
    de_render_uploader *uploader;
} material_preview;
static void material_preview_setup(material_preview *f) {
    f->allocator = de_tracking_allocator_init(&f->track, NULL, DE_MEM_RENDER);
    ASSERT_EQ(de_asset_system_create(&(de_asset_system_desc){.allocator = &f->allocator,
                                                             .staging_ring_bytes = 1u << 20},
                                     &f->assets),
              DE_OK);
    ASSERT_EQ(de_gpu_device_create(&(de_gpu_device_desc){.backend = DE_GPU_NULL,
                                                         .allocator = &f->allocator,
                                                         .pipeline_cache_dir = ""},
                                   &f->device),
              DE_OK);
    ASSERT_EQ(de__render_register_asset_types_ex(
                  f->assets,
                  &(de__render_assets_desc){
                      .device = f->device, .allocator = &f->allocator, .no_import = true},
                  &f->uploader),
              DE_OK);
}
static void material_preview_teardown(material_preview *f) {
    if (f->device)
        de_gpu_wait_idle(f->device);
    de_asset_system_destroy(f->assets);
    de_render_uploader_destroy(f->uploader);
    if (f->device)
        de_gpu_device_destroy(f->device);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).bytes_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0u);
}
TEST_F(material_preview, geometry_is_accounted_pinned_reused_and_completely_released,
       material_preview) {
    de__ed_material_meshes meshes = {0};
    for (u32 shape = 0; shape < DE__RENDER_PREVIEW_SHAPES; ++shape) {
        de_mesh handle = {0};
        de_result result = DE_ERR_BUSY;
        for (u32 attempt = 0; result == DE_ERR_BUSY && attempt < 100; ++attempt) {
            de_render_uploader_flush(f->uploader);
            (void)de_render_uploader_acquire_submit(f->uploader);
            result =
                de__ed_material_mesh(f->assets, &meshes, (de__render_preview_shape)shape, &handle);
        }
        ASSERT_EQ(result, DE_OK);
        ASSERT_TRUE(handle.id != 0);
        const de_render_mesh *record = de_render_mesh_get(f->assets, handle);
        ASSERT_TRUE(record != NULL);
        EXPECT_GT(record->gpu_bytes, 0u);
        EXPECT_EQ(record->lod_count, 1u);
        u32 handles = 0, refs = 0;
        de__slot_refs(de__slot_get(f->assets, handle.id), &handles, &refs);
        EXPECT_EQ(refs, 1u);
        EXPECT_EQ(handles, 0u);
        de_mesh reused = {0};
        ASSERT_EQ(
            de__ed_material_mesh(f->assets, &meshes, (de__render_preview_shape)shape, &reused),
            DE_OK);
        EXPECT_EQ(reused.id, handle.id);
    }
    de_asset_stats stats = de_asset_get_stats(f->assets);
    EXPECT_EQ(stats.assets_resident, 3u);
    EXPECT_GT(stats.bytes_resident[DE_ASSET_BYTES_GPU_MESH], 0u);
    de_asset_flush_unused(f->assets);
    EXPECT_EQ(de_asset_get_stats(f->assets).assets_resident, 3u);
    de__asset_reload_stats reload;
    de__asset_reload_get_stats(f->assets, &reload);
    EXPECT_EQ(reload.watches, 0u);
}
TEST_F(material_preview, authored_reserved_id_collision_never_overwrites_a_slot, material_preview) {
    u32 slot = 0;
    ASSERT_EQ(de__slot_create(f->assets, de_asset_id_from("__didePreview/sphere"),
                              de_hash64_str("mesh"), &slot),
              DE_OK);
    de__ed_material_meshes meshes = {0};
    de_mesh sentinel = {123};
    EXPECT_EQ(de__ed_material_mesh(f->assets, &meshes, DE__RENDER_PREVIEW_SPHERE, &sentinel),
              DE_ERR_ALREADY_EXISTS);
    EXPECT_EQ(sentinel.id, 123u);
    EXPECT_EQ(meshes.handles[0].id, 0u);
    EXPECT_EQ(de_asset_get_stats(f->assets).assets_resident, 0u);
    EXPECT_EQ(de__slot_object(de__slot_get(f->assets, slot)), NULL);
}
