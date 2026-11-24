/*
 * test_fog_stage.c: the renderer's froxel fog stage (DENG-861, follow-up of DENG-849). The fog is
 * registered by the renderer (the view stage "fog"), not by de_sky_attach: a world that never
 * attaches a sky still gets it, and attaching or detaching the sky neither adds a second one nor
 * removes it. Null backend; the frames themselves (fog without a sky) are the golden fog_no_attach.
 */
#include "de_test.h"

#include <dengine/de_base.h>
#include <dengine/de_ecs.h>
#include <dengine/de_gpu.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_reflect.h>
#include <dengine/de_render.h>

#include "render/renderer_internal.h"
#include "render/views_internal.h"

#include <string.h>

typedef struct fogfx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_gpu_device *dev;
    de_type_registry *types;
    de_world *w;
    de_renderer *r;
    de_sky *sky;
} fogfx;

static void fogfx_setup(fogfx *f) {
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_GENERAL);
    de_fp_env_init_thread();
}

static void fogfx_teardown(fogfx *f) {
    de_sky_detach(f->sky);
    if (f->r != NULL)
        de_renderer_destroy(f->r);
    if (f->w != NULL)
        de_world_destroy(f->w);
    if (f->types != NULL)
        de_type_registry_destroy(f->types);
    if (f->dev != NULL)
        de_gpu_device_destroy(f->dev);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0u);
}

static void open_renderer(fogfx *f) {
    ASSERT_EQ(de_gpu_device_create(&(de_gpu_device_desc){.backend = DE_GPU_NULL,
                                                         .allocator = &f->alloc,
                                                         .pipeline_cache_dir = ""},
                                   &f->dev),
              DE_OK);
    if (!de_gpu_get_caps(f->dev)->base_tier_ok)
        SKIP("GPU below Base tier");
    ASSERT_EQ(de_type_registry_create(&f->alloc, &f->types), DE_OK);
    ASSERT_EQ(de_world_create(&(de_world_desc){.allocator = &f->alloc, .types = f->types}, &f->w),
              DE_OK);
    ASSERT_EQ(de_render_register_components(f->w), DE_OK);
    ASSERT_EQ(de_renderer_create(&(de_renderer_desc){.device = f->dev,
                                                     .allocator = &f->alloc,
                                                     .types = f->types,
                                                     .frames_in_flight = 2},
                                 &f->r),
              DE_OK);
    de_renderer_attach(f->r, f->w);
}

/* One fog stage, no sky stage: a world that never calls de_sky_attach has its fog. */
TEST_F(fog_stage, test_fog_stage_without_sky, fogfx) {
    open_renderer(f);
    EXPECT_EQ(de__view_stage_count(f->r, "fog"), 1u);
    EXPECT_EQ(de__view_stage_count(f->r, "sky"), 0u);
    EXPECT_EQ(de__view_stage_count(f->r, "sky.ibl"), 0u);
    EXPECT_TRUE(f->r->fog != NULL);
}

/* With the sky attached the fog stage is still registered exactly once, next to the sky's stages. */
TEST_F(fog_stage, test_fog_stage_registered_once_with_sky, fogfx) {
    open_renderer(f);
    ASSERT_EQ(de_sky_attach(f->w, f->r, &f->sky), DE_OK);
    EXPECT_EQ(de__view_stage_count(f->r, "fog"), 1u);
    EXPECT_EQ(de__view_stage_count(f->r, "sky"), 1u);
    EXPECT_EQ(de__view_stage_count(f->r, "sky.ibl"), 1u);
}

/* Detach keeps the fog; reattach neither duplicates it nor loses the sky's stages. */
TEST_F(fog_stage, test_fog_stage_detach_and_reattach, fogfx) {
    open_renderer(f);
    for (u32 round = 0; round < 3u; ++round) {
        ASSERT_EQ(de_sky_attach(f->w, f->r, &f->sky), DE_OK);
        EXPECT_EQ(de__view_stage_count(f->r, "fog"), 1u);
        EXPECT_EQ(de__view_stage_count(f->r, "sky"), 1u);
        EXPECT_EQ(de__view_stage_count(f->r, "sky.ibl"), 1u);
        de_sky_detach(f->sky);
        f->sky = NULL;
        EXPECT_EQ(de__view_stage_count(f->r, "fog"), 1u);
        EXPECT_EQ(de__view_stage_count(f->r, "sky"), 0u);
        EXPECT_EQ(de__view_stage_count(f->r, "sky.ibl"), 0u);
    }
}
