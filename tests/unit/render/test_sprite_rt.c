#include "scene_as_fixture.h"
#include "render/rt/sprite_as.h"
#include "render/sprite/sprite_internal.h"
#include "cook/importers/sprite_proxy.h"

typedef struct sprite_rt_fixture {
    rt_fixture rt;
    DeSprite sprites[32];
    de_render_texture texture_record;
    de_cooked_sprite_proxy proxy;
} sprite_rt_fixture;
static const de_render_texture *sprite_texture(void *user, de_texture texture) {
    sprite_rt_fixture *f = user;
    return texture.id ? &f->texture_record : NULL;
}
static void sprite_rt_fixture_setup(sprite_rt_fixture *f) {
    rt_fixture_setup(&f->rt);
    rt_fixture *r = &f->rt;
    de__gpu_scene_destroy(r->r->gpu_scene);
    de__gpu_scene_desc desc = {.device = r->d,
                               .renderer = r->r,
                               .allocator = &r->alloc,
                               .views_in_view_graphs = true,
                               .resolver = {.user = f,
                                            .mesh = fake_mesh,
                                            .material = fake_material,
                                            .texture_bindless = texture_index,
                                            .texture_record = sprite_texture}};
    desc.initial_capacity[DE__GS_VIEWS] = 32;
    ASSERT_EQ(de__gpu_scene_create(&desc, &r->r->gpu_scene), DE_OK);
    f->sprites[0] = (DeSprite){.texture = {1},
                               .uv_rect = {0, 0, 1, 1},
                               .size = {2, 2},
                               .pivot = {.5f, .5f},
                               .color = {1, 1, 1, 1},
                               .cast_shadows = true};
    de_affine local = de__sprite_local_transform(&f->sprites[0], DE_PLANE_TOP_XY),
              entity = at(3, 0, 0);
    r->w.f.instances.sprite = f->sprites;
    r->w.f.instances.mesh[0] = (de_mesh){0};
    r->w.f.instances.transform[0] = de_affine_mul(&entity, &local);
    r->w.f.instances.prev_transform[0] = r->w.f.instances.transform[0];
    memset(r->w.f.instances.materials, 0, DE__RW_MATERIALS * sizeof(de_material));
    r->w.f.instances.flags[0] = DE__RW_INST_SPRITE | DE__RW_INST_HAS_BOUNDS |
                                DE__RW_INST_VISIBLE_IN_RT | DE__RW_INST_CAST_SHADOWS;
    f->texture_record.texture = r->texture;
    f->texture_record.width = 2;
    f->texture_record.height = 1;
}
static void sprite_rt_fixture_teardown(sprite_rt_fixture *f) {
    rt_fixture_teardown(&f->rt);
}

TEST_F(sprite_rt, shadow_mask_and_exclusion, sprite_rt_fixture) {
    rt_fixture *r = &f->rt;
    u32 intersection = 0, united = 0;
    for (u32 y = 0; y < 8; ++y)
        for (u32 x = 0; x < 16; x += 2) {
            de__rt_ray *rays = de_gpu_buffer_map(r->d, r->rays);
            for (u32 j = 0; j < 2; ++j)
                rays[j] = (de__rt_ray){
                    .origin = {2 + ((f32)(x + j) + .5f) / 8, -1 + ((f32)y + .5f) / 4, 2, .001f},
                    .direction = {0, 0, -1, 20}};
            de__rt_hit *hits = frame_rt(r);
            for (u32 j = 0; j < 2; ++j) {
                bool expected = x + j >= 8, actual = hits[j].t >= 0;
                intersection += expected && actual;
                united += expected || actual;
                if (actual)
                    ASSERT_EQ(hits[j].instance, 0u);
            }
        }
    double iou = united ? (double)intersection / united : 0;
    printf("sprite projected alpha silhouette IoU %.6f\n", iou);
    ASSERT_TRUE(iou >= .95);
    r->flags = DE__RT_EXCLUDE_SPRITES;
    de__rt_hit *hits = frame_rt(r);
    ASSERT_TRUE(hits[0].t < 0 && hits[1].t < 0);
}
TEST_F(sprite_rt, thickness_grazing, sprite_rt_fixture) {
    rt_fixture *r = &f->rt;
    de__rt_ray *rays = de_gpu_buffer_map(r->d, r->rays);
    for (u32 j = 0; j < 2; ++j)
        rays[j] = (de__rt_ray){.origin = {5, 0, .055f + .01f * (f32)j, .001f},
                               .direction = {-.996194698f, 0, -.087155743f, 20}};
    de__rt_hit *hits = frame_rt(r);
    u32 flat = (hits[0].t >= 0) + (hits[1].t >= 0);
    f->sprites[0].thickness = .1f;
    hits = frame_rt(r);
    u32 thick = (hits[0].t >= 0) + (hits[1].t >= 0);
    printf("85 degree grazing shadows: flat %u, thick %u\n", flat, thick);
    ASSERT_EQ(flat, 0u);
    ASSERT_EQ(thick, 2u);
}
TEST_F(sprite_rt, cooked_proxy_shadow_and_flip, sprite_rt_fixture) {
    rt_fixture *r = &f->rt;
    const f32 rgba[8] = {1, 1, 1, 0, 1, 1, 1, 1};
    ASSERT_EQ(de_sprite_proxy_build(&r->alloc, rgba, 2, 1, .5f, &f->proxy), DE_OK);
    ASSERT_LE(f->proxy.index_count / 3, 64u);
    f->texture_record.sprite_proxy = &f->proxy;
    f->sprites[0].thickness = .1f;
    de__rt_hit *hits = frame_rt(r);
    ASSERT_TRUE(hits[0].t < 0);
    ASSERT_TRUE(hits[1].t > 0);
    /* Rendering consumes the scene snapshot; restore it for CPU metadata inspection. */
    ASSERT_EQ(de__gpu_scene_update(r->r->gpu_scene, &r->w.f), DE_OK);
    u32 mesh;
    const u32 *materials;
    ASSERT_TRUE(de__gpu_scene_instance_slots(r->r->gpu_scene, 0, &mesh, &materials));
    de__rt_mesh_gpu metadata = {0};
    ASSERT_TRUE(de__sprite_as_metadata(r->r, mesh, &metadata));
    ASSERT_EQ(metadata.blas[1], 2u);
    u32 intersection = 0, united = 0;
    for (u32 y = 0; y < 8; ++y)
        for (u32 x = 0; x < 16; x += 2) {
            de__rt_ray *rays = de_gpu_buffer_map(r->d, r->rays);
            for (u32 j = 0; j < 2; ++j)
                rays[j] = (de__rt_ray){
                    .origin = {2 + ((f32)(x + j) + .5f) / 8, -1 + ((f32)y + .5f) / 4, 2, .001f},
                    .direction = {0, 0, -1, 20}};
            hits = frame_rt(r);
            for (u32 j = 0; j < 2; ++j) {
                bool expected = x + j >= 8, actual = hits[j].t >= 0;
                intersection += expected && actual;
                united += expected || actual;
            }
        }
    double iou = united ? (double)intersection / united : 0;
    printf("Cooked opaque sprite proxy shadow IoU %.6f\n", iou);
    ASSERT_TRUE(iou >= .9);
    de__rt_ray *rays = de_gpu_buffer_map(r->d, r->rays);
    rays[0] = (de__rt_ray){.origin = {2.5f, 0, 2, .001f}, .direction = {0, 0, -1, 20}};
    rays[1] = (de__rt_ray){.origin = {3.5f, 0, 2, .001f}, .direction = {0, 0, -1, 20}};
    f->sprites[0].flip_x = true;
    hits = frame_rt(r);
    ASSERT_TRUE(hits[0].t > 0);
    ASSERT_TRUE(hits[1].t < 0);
}
TEST_F(sprite_rt, recycled_texture_record_rebuilds_proxy_geometry, sprite_rt_fixture) {
    rt_fixture *r = &f->rt;
    const f32 first[8] = {1, 1, 1, 0, 1, 1, 1, 1};
    const f32 second[8] = {1, 1, 1, 1, 1, 1, 1, 0};
    ASSERT_EQ(de_sprite_proxy_build(&r->alloc, first, 2, 1, .5f, &f->proxy), DE_OK);
    f->texture_record.sprite_proxy = &f->proxy;
    f->sprites[0].thickness = .1f;
    de__rt_hit *hits = frame_rt(r);
    ASSERT_TRUE(hits[0].t < 0 && hits[1].t > 0);
    /* Model unload/reload between RT preparations: both CPU record and proxy storage
     * are deliberately reused while the replacement has a new RHI generation. */
    de_gpu_texture old = r->texture;
    de_gpu_wait_idle(r->d);
    de_gpu_texture_destroy(r->d, old);
    r->texture = de_gpu_texture_create(r->d, &(de_gpu_texture_desc){.width = 2,
                                                                    .height = 1,
                                                                    .format = DE_FMT_RGBA8_UNORM,
                                                                    .usage = DE_USAGE_SAMPLED});
    ASSERT_TRUE(r->texture.id);
    ASSERT_NE(r->texture.id, old.id);
    f->texture_record.texture = r->texture;
    ASSERT_EQ(de_sprite_proxy_build(&r->alloc, second, 2, 1, .5f, &f->proxy), DE_OK);
    hits = frame_rt(r);
    EXPECT_TRUE(hits[0].t > 0);
    EXPECT_TRUE(hits[1].t < 0);
}
TEST(sprite_proxy, marching_squares_deterministic_format) {
    f32 rgba[8 * 8 * 4] = {0};
    for (u32 y = 1; y < 7; ++y)
        for (u32 x = 1; x < 7; ++x)
            if (x < 4 || y < 4)
                rgba[(y * 8 + x) * 4 + 3] = 1;
    de_cooked_sprite_proxy a, b, decoded;
    ASSERT_EQ(de_sprite_proxy_build(de_heap_allocator(), rgba, 8, 8, .5f, &a), DE_OK);
    ASSERT_EQ(de_sprite_proxy_build(de_heap_allocator(), rgba, 8, 8, .5f, &b), DE_OK);
    ASSERT_EQ(memcmp(&a, &b, sizeof a), 0);
    ASSERT_LE(a.index_count / 3, 64u);
    u8 bytes[DE_SPRITE_PROXY_BYTES];
    de_sprite_proxy_write(bytes, &a);
    ASSERT_TRUE(de_sprite_proxy_parse(bytes, sizeof bytes, &decoded));
    ASSERT_EQ(memcmp(&a, &decoded, sizeof a), 0);
    bytes[400] = 255;
    bytes[401] = 255;
    bytes[402] = 255;
    bytes[403] = 255;
    ASSERT_FALSE(de_sprite_proxy_parse(bytes, sizeof bytes, &decoded));
}

TEST_F(sprite_rt, atlas_region_opaque_proxy, sprite_rt_fixture) {
    rt_fixture *r = &f->rt;
    const f32 rgba[4] = {1, 1, 1, 1};
    ASSERT_EQ(de_sprite_proxy_build(&r->alloc, rgba, 1, 1, .5f, &f->proxy), DE_OK);
    f->proxy.region[0] = .5f;
    f->proxy.region[2] = .5f;
    f->texture_record.sprite_proxy = &f->proxy;
    f->sprites[0].uv_rect = (de_vec4){.5f, 0, .5f, 1};
    f->sprites[0].thickness = .1f;
    de__rt_hit *hits = frame_rt(r);
    ASSERT_TRUE(hits[0].t > 0 && hits[1].t > 0);
    ASSERT_EQ(de__gpu_scene_update(r->r->gpu_scene, &r->w.f), DE_OK);
    u32 mesh;
    const u32 *materials;
    de__rt_mesh_gpu metadata = {0};
    ASSERT_TRUE(de__gpu_scene_instance_slots(r->r->gpu_scene, 0, &mesh, &materials));
    ASSERT_TRUE(de__sprite_as_metadata(r->r, mesh, &metadata));
    ASSERT_EQ(metadata.blas[1], 2u);
}

TEST_F(sprite_rt, pixel_snap_tlas_matches_view_instances, sprite_rt_fixture) {
    rt_fixture *r = &f->rt;
    de_affine local = de__sprite_local_transform(&f->sprites[0], DE_PLANE_TOP_XY),
              entity = at(3.37f, 0, 0);
    r->w.f.instances.transform[0] = de_affine_mul(&entity, &local);
    r->w.f.instances.prev_transform[0] = r->w.f.instances.transform[0];
    de__rw_view camera = {
        .camera = {.projection = DE_PROJ_ORTHOGRAPHIC, .ortho_height = 16, .near_z = .1f},
        .offset = {3, 0, 4},
        .rotation = {0, 0, 0, 1},
        .transform = at(3, 0, 4)};
    de__view_capture_camera(r->r, r->view, &camera);
    de__rt_ray *rays = de_gpu_buffer_map(r->d, r->rays);
    rays[0] = (de__rt_ray){.origin = {3.1f, 0, 2, .001f}, .direction = {0, 0, -1, 20}};
    rays[1] = (de__rt_ray){.origin = {4.1f, 0, 2, .001f}, .direction = {0, 0, -1, 20}};
    de__rt_hit *hits = frame_rt(r);
    ASSERT_TRUE(hits[0].t < 0 && hits[1].t > 0);
    de_pixel_art_settings settings = {.internal_width = 16,
                                      .internal_height = 16,
                                      .integer_scale_only = true,
                                      .snap_sprites = true};
    de_view_set_pixel_art(r->r, r->view, &settings);
    hits = frame_rt(r);
    ASSERT_TRUE(hits[0].t > 0 && hits[1].t < 0);
    de_view_set_pixel_art(r->r, r->view, NULL);
    hits = frame_rt(r);
    ASSERT_TRUE(hits[0].t < 0 && hits[1].t > 0);
}
