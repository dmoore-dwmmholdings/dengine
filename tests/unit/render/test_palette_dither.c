#include "pixel_fixture.h"
#include "render/scene/gpu_scene.h"
#include "render/loaders/render_assets.h"
TEST(palette, oklab_cube_nearest) {
    const de_vec4 colors[] = {{0, 0, 0, 1}, {1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 1, 1}};
    de_vec4 *lut = malloc(32768 * sizeof *lut);
    ASSERT_TRUE(lut);
    de__palette_build(colors, 5, lut);
    for (u32 i = 0; i < 5; ++i)
        ASSERT_EQ(de__palette_nearest(colors, 5, (de_vec3){colors[i].x, colors[i].y, colors[i].z}),
                  i);
    ASSERT_TRUE(lut[0].x == 0 && lut[32767].x == 1 && lut[32767].y == 1 && lut[32767].z == 1);
    for (u32 i = 0; i < 32768; ++i) {
        bool found = false;
        for (u32 p = 0; p < 5; ++p)
            found |= memcmp(&lut[i], &colors[p], sizeof *lut) == 0;
        ASSERT_TRUE(found);
    }
    free(lut);
}
TEST(palette, bayer_permutation) {
    u64 seen = 0;
    for (u32 y = 0; y < 8; ++y)
        for (u32 x = 0; x < 8; ++x) {
            u32 value = de__pixel_bayer(x, y);
            ASSERT_LT(value, 64u);
            ASSERT_FALSE((seen >> value) & 1u);
            seen |= (u64)1 << value;
            ASSERT_EQ(value, de__pixel_bayer(x + 8, y + 8));
        }
    ASSERT_EQ(seen, UINT64_MAX);
}
typedef struct snap_observer {
    de_view view;
    f32 expected;
    bool pixel;
    u32 called;
} snap_observer;
static void observe_snap(de__view_frame *vf, void *user) {
    snap_observer *o = user;
    if (vf->view.id != o->view.id)
        return;
    de__gpu_scene_table_info table = de__gpu_scene_table(vf->renderer->gpu_scene, DE__GS_INSTANCES);
    const de__gs_instance *instances =
        o->pixel ? de_gpu_buffer_map(vf->renderer->dev, table.buffer) : NULL;
    for (u32 i = 0; i < vf->world->instances.count; ++i)
        if (vf->world->instances.flags[i] & DE__RW_INST_SPRITE) {
            de__gs_instance original;
            de__gpu_scene_pack_frame_instance(vf->renderer->gpu_scene, vf->world, i, &original);
            const de__gs_instance *instance = o->pixel ? &instances[i] : &original;
            const DeSprite *s = &vf->world->instances.sprite[i];
            f32 x = instance->transform[0].v[3] + instance->transform[0].v[0] * s->pivot.x +
                    instance->transform[0].v[1] * s->pivot.y;
            ASSERT_TRUE(fabsf(x - o->expected) < 1e-5f);
            if (!o->pixel)
                ASSERT_TRUE(!vf->pixel_art);
            ++o->called;
            break;
        }
}
TEST_F(palette, two_pixel_grids_and_nonpixel, pixel_fixture) {
    start_pixel(f, DE_GPU_NULL, 16, 16, 64, 64, false);
    const de_builtin_component_ids bi = de_builtin_components(f->world);
    DeTransform *transform = de_entity_get(f->world, f->sprite, bi.transform);
    transform->position.x = 10.37;
    de_pixel_art_settings a = {.internal_width = 16,
                               .internal_height = 16,
                               .integer_scale_only = true,
                               .snap_sprites = true};
    de_view_set_pixel_art(f->r, f->view, &a);
    de_view second = de_view_create(
        f->r, &(de_view_desc){.target = f->target, .order = 1, .name = "pixel-half-grid"});
    de_view third = de_view_create(
        f->r, &(de_view_desc){.target = f->target, .order = 2, .name = "ordinary-view"});
    a.internal_width = a.internal_height = 32;
    de_view_set_pixel_art(f->r, second, &a);
    const DeRenderComponents *ids = de__render_components(f->world);
    for (u32 n = 0; n < 2; ++n) {
        de_component components[] = {bi.transform, ids->camera};
        de_entity camera;
        ASSERT_EQ(de_entity_spawn_with(f->world, components, 2, &camera), DE_OK);
        *(DeTransform *)de_entity_get(f->world, camera, bi.transform) = de_transform_identity();
        *(DeCamera *)de_entity_get(f->world, camera, ids->camera) =
            (DeCamera){.view = n ? third : second,
                       .projection = DE_PROJ_ORTHOGRAPHIC,
                       .ortho_height = 16,
                       .near_z = .1f};
    }
    snap_observer observers[] = {
        {f->view, 10, true, 0}, {second, 10.5f, true, 0}, {third, 10.37f, false, 0}};
    for (u32 i = 0; i < 3; ++i)
        ASSERT_EQ(de__view_register_stage(f->r, DE__VIEW_SLOT_POST, INT32_MAX - 3, "test.snap.grid",
                                          observe_snap, &observers[i]),
                  DE_OK);
    u64 start = de_time_now_ns();
    for (u32 frame = 0; frame < 4; ++frame)
        pixel_frame(f);
    printf("Three-view frame CPU mean %.6fms\n", (double)(de_time_now_ns() - start) / 4e6);
    for (u32 i = 0; i < 3; ++i)
        ASSERT_EQ(observers[i].called, 4u);
    transform = de_entity_get(f->world, f->sprite, bi.transform);
    ASSERT_TRUE(fabs(transform->position.x - 10.37) < 1e-12);
}
static de_render_texture palette_texture;
static const de_render_mesh *no_mesh(void *user, de_mesh mesh) {
    (void)user;
    (void)mesh;
    return NULL;
}
static const de_render_texture *record(void *user, de_texture texture) {
    (void)user;
    return texture.id ? &palette_texture : NULL;
}
static u32 texture_index(void *user, de_texture texture) {
    (void)user;
    return texture.id ? palette_texture.bindless : 0;
}
static void upload_palette(pixel_fixture *f) {
    palette_texture =
        (de_render_texture){.width = 16, .height = 1, .mip_count = 1, .format = DE_FMT_RGBA8_UNORM};
    palette_texture.texture = de_gpu_texture_create(
        f->d,
        &(de_gpu_texture_desc){.width = 16,
                               .height = 1,
                               .format = DE_FMT_RGBA8_UNORM,
                               .usage = DE_USAGE_COPY_DST | DE_USAGE_COPY_SRC | DE_USAGE_SAMPLED});
    palette_texture.bindless = de_gpu_texture_bindless(f->d, palette_texture.texture, 0, false);
    de_gpu_buffer upload = de_gpu_buffer_create(
        f->d,
        &(de_gpu_buffer_desc){.size = 256, .usage = DE_USAGE_COPY_SRC, .memory = DE_MEM_UPLOAD});
    u8 *data = de_gpu_buffer_map(f->d, upload);
    for (u32 i = 0; i < 16; ++i) {
        data[i * 4] = (u8)((i % 4) * 85);
        data[i * 4 + 1] = (u8)((i / 4) * 85);
        data[i * 4 + 2] = (i & 1) ? 255 : 0;
        data[i * 4 + 3] = 255;
    }
    de_gpu_cmd *cmd = de_gpu_cmd_begin(f->d, DE_QUEUE_GRAPHICS, "palette test upload");
    de_gpu_barrier barrier = {.texture = palette_texture.texture, .after = DE_ACCESS_COPY_DST};
    de_gpu_cmd_barriers(cmd, &barrier, 1);
    de_gpu_cmd_copy_buffer_to_texture(cmd, palette_texture.texture, 0, 0, upload, 0, 256);
    barrier.before = DE_ACCESS_COPY_DST;
    barrier.after = DE_ACCESS_SHADER_READ;
    de_gpu_cmd_barriers(cmd, &barrier, 1);
    ASSERT_EQ(de_gpu_queue_submit(f->d, DE_QUEUE_GRAPHICS,
                                  &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
              DE_OK);
    de_gpu_buffer_destroy(f->d, upload);
    de__gpu_scene_destroy(f->r->gpu_scene);
    ASSERT_EQ(
        de__gpu_scene_create(&(de__gpu_scene_desc){.device = f->d,
                                                   .renderer = f->r,
                                                   .allocator = de_heap_allocator(),
                                                   .views_in_view_graphs = true,
                                                   .initial_capacity[DE__GS_VIEWS] = DE__VIEW_MAX,
                                                   .builtin_scatter = true,
                                                   .resolver = {.mesh = no_mesh,
                                                                .texture_record = record,
                                                                .texture_bindless = texture_index}},
                             &f->r->gpu_scene),
        DE_OK);
}
TEST_F(palette, output_histogram_and_static_dither, pixel_fixture) {
    start_pixel(f, DE_GPU_METAL, 16, 16, 64, 64, false);
    upload_palette(f);
    make_pattern(f);
    de_pixel_art_settings settings = {.internal_width = 16,
                                      .internal_height = 16,
                                      .integer_scale_only = true,
                                      .snap_camera = true,
                                      .palette_lut = {1},
                                      .dither_strength = .5f};
    de_view_set_pixel_art(f->r, f->view, &settings);
    pixel_frame(f);
    const u8 *output = read_output(f);
    u32 distinct = 0;
    u16 histogram = 0;
    for (u32 y = 0; y < 64; ++y)
        for (u32 x = 0; x < 64; ++x) {
            const u8 *p = output + ((size_t)y * 64 + x) * 4;
            bool found = false;
            for (u32 i = 0; i < 16; ++i)
                if (p[0] == (i % 4) * 85 && p[1] == (i / 4) * 85 && p[2] == ((i & 1) ? 255 : 0)) {
                    found = true;
                    histogram |= (u16)(1u << i);
                    break;
                }
            if (!found)
                printf("Non-palette output at %u,%u = %u,%u,%u\n", x, y, p[0], p[1], p[2]);
            ASSERT_TRUE(found);
        }
    for (u32 i = 0; i < 16; ++i)
        distinct += (histogram >> i) & 1;
    printf("Palette histogram: %u of16 exact colors\n", distinct);
    ASSERT_TRUE(distinct >= 2);
    u8 before[64 * 64 * 4];
    memcpy(before, output, sizeof before);
    pixel_frame(f);
    output = read_output(f);
    ASSERT_EQ(memcmp(before, output, sizeof before), 0);
    const de_builtin_component_ids bi = de_builtin_components(f->world);
    DeTransform *camera = de_entity_get(f->world, f->camera, bi.transform);
    camera->position.x = 1;
    pixel_frame(f);
    output = read_output(f);
    ASSERT_TRUE(memcmp(before, output, sizeof before) != 0);
    de_gpu_texture_destroy(f->d, palette_texture.texture);
}

TEST_F(palette, ten_thousand_snap_cpu_cost, pixel_fixture) {
    start_pixel(f, DE_GPU_NULL, 16, 16, 64, 64, false);
    const de_builtin_component_ids bi = de_builtin_components(f->world);
    const DeRenderComponents *ids = de__render_components(f->world);
    const de_component components[] = {bi.transform, ids->sprite};
    for (u32 i = 0; i < 10000; ++i) {
        de_entity entity;
        ASSERT_EQ(de_entity_spawn_with(f->world, components, 2, &entity), DE_OK);
        DeTransform transform = de_transform_identity();
        transform.position = (de_dvec3){10.37, 0, -5};
        *(DeTransform *)de_entity_get(f->world, entity, bi.transform) = transform;
        *(DeSprite *)de_entity_get(f->world, entity, ids->sprite) =
            (DeSprite){.size = {1, 1}, .color = {1, 1, 1, 1}, .uv_rect = {0, 0, 1, 1}};
    }
    de_pixel_art_settings settings = {.internal_width = 16,
                                      .internal_height = 16,
                                      .integer_scale_only = true,
                                      .snap_sprites = true};
    de_view_set_pixel_art(f->r, f->view, &settings);
    for (u32 i = 0; i < 3; ++i)
        pixel_frame(f);
    u64 start = de_time_now_ns();
    for (u32 i = 0; i < 5; ++i)
        pixel_frame(f);
    printf("10001 sprites, warmed pixel view CPU frame mean %.6fms (includes Extract/upload/null "
           "graph)\n",
           (double)(de_time_now_ns() - start) / 5e6);
}

typedef struct motion_snap_observer {
    de_view view;
    de_dvec3 current, previous;
    bool snapshot;
    u32 calls;
    de_dvec3 origin;
} motion_snap_observer;
static void observe_motion_snap(de__view_frame *vf, void *user) {
    motion_snap_observer *o = user;
    if (vf->view.id != o->view.id)
        return;
    o->origin = vf->world->origin;
    de__gpu_scene_table_info table = de__gpu_scene_table(vf->renderer->gpu_scene, DE__GS_INSTANCES);
    const de__gs_instance *instances =
        o->snapshot ? de_gpu_buffer_map(vf->renderer->dev, table.buffer) : NULL;
    if (o->snapshot)
        ASSERT_TRUE(instances != NULL);
    for (u32 i = 0; i < vf->world->instances.count; ++i) {
        if (!(vf->world->instances.flags[i] & DE__RW_INST_SPRITE))
            continue;
        de__gs_instance original;
        de__gpu_scene_pack_frame_instance(vf->renderer->gpu_scene, vf->world, i, &original);
        const de__gs_instance *item = o->snapshot ? &instances[i] : &original;
        const DeSprite *sprite = &vf->world->instances.sprite[i];
        const f64 origin[] = {vf->world->origin.x, vf->world->origin.y, vf->world->origin.z};
        const f64 current[] = {o->current.x, o->current.y, o->current.z};
        const f64 previous[] = {o->previous.x, o->previous.y, o->previous.z};
        for (u32 axis = 0; axis < 3; ++axis) {
            f64 a = origin[axis] + item->transform[axis].v[3] +
                    item->transform[axis].v[0] * sprite->pivot.x +
                    item->transform[axis].v[1] * sprite->pivot.y;
            f64 b = origin[axis] + item->prev_transform[axis].v[3] +
                    item->prev_transform[axis].v[0] * sprite->pivot.x +
                    item->prev_transform[axis].v[1] * sprite->pivot.y;
            EXPECT_TRUE(fabs(a - current[axis]) < 0.002);
            EXPECT_TRUE(fabs(b - previous[axis]) < 0.002);
        }
        ++o->calls;
        return;
    }
    ASSERT_TRUE(false);
}

TEST_F(palette, previous_sprite_pose_uses_prior_camera_grid_and_snap_toggle, pixel_fixture) {
    start_pixel(f, DE_GPU_NULL, 16, 16, 64, 64, false);
    de__view_unregister_stage(f->r, observe_internal, f);
    de__view_unregister_stage(f->r, observe_ui, f);
    de__view_unregister_stage(f->r, pattern_stage, f);
    const de_builtin_component_ids bi = de_builtin_components(f->world);
    const DeRenderComponents *ids = de__render_components(f->world);
    de_view second = de_view_create(
        f->r, &(de_view_desc){.target = f->target, .order = 1, .name = "history-second"});
    de_pixel_art_settings other = {
        .internal_width = 32, .internal_height = 32, .snap_sprites = true};
    de_view_set_pixel_art(f->r, second, &other);
    de_entity camera2;
    de_component components[] = {bi.transform, ids->camera};
    ASSERT_EQ(de_entity_spawn_with(f->world, components, 2, &camera2), DE_OK);
    DeTransform camera_transform = de_transform_identity();
    camera_transform.position.x = 1.2;
    *(DeTransform *)de_entity_get(f->world, camera2, bi.transform) = camera_transform;
    *(DeCamera *)de_entity_get(f->world, camera2, ids->camera) = (DeCamera){
        .view = second, .projection = DE_PROJ_ORTHOGRAPHIC, .ortho_height = 16, .near_z = .1f};
    motion_snap_observer o[2] = {{.view = f->view}, {.view = second, .snapshot = true}};
    for (u32 i = 0; i < 2; ++i)
        ASSERT_EQ(de__view_register_stage(f->r, DE__VIEW_SLOT_POST, INT32_MAX - 3,
                                          "test.snap.motion", observe_motion_snap, &o[i]),
                  DE_OK);
    const struct {
        f64 sprite, camera;
        f32 height, scale;
        u32 resolution;
        bool snap;
        f64 current, previous, current2, previous2;
    } cases[] = {{10.6, 0, 16, 1, 16, true, 11, 11, 10.7, 10.7},
                 {10.37, 0, 16, 1, 16, true, 10, 11, 10.2, 10.7},
                 {10.37, .3, 8, 2, 16, true, 10.3, 10, 10.2, 10.2},
                 {10.63, .6, 8, 2, 32, true, 10.6, 10.3, 10.7, 10.2},
                 {10.87, .6, 8, 2, 32, false, 10.87, 10.6, 10.7, 10.7},
                 {11.12, .6, 8, 2, 32, false, 11.12, 10.87, 11.2, 10.7},
                 {11.37, 0, 16, 2, 16, true, 11, 11.12, 11.2, 11.2}};
    bool previous_snap = true;
    for (u32 i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        if (i == 3)
            de_render_set_2d_plane(f->r, DE_PLANE_TOP_XY);
        DeTransform *sprite = de_entity_get(f->world, f->sprite, bi.transform);
        sprite->position.x = cases[i].sprite;
        sprite->position.y = .37;
        sprite->scale.x = cases[i].scale;
        ((DeTransform *)de_entity_get(f->world, f->camera, bi.transform))->position.x =
            cases[i].camera;
        ((DeCamera *)de_entity_get(f->world, f->camera, ids->camera))->ortho_height =
            cases[i].height;
        de_pixel_art_settings grid = {.internal_width = cases[i].resolution,
                                      .internal_height = cases[i].resolution,
                                      .snap_sprites = true};
        de_view_set_pixel_art(f->r, f->view, cases[i].snap ? &grid : NULL);
        o[0].current = (de_dvec3){cases[i].current,
                                  i < 3    ? .37
                                  : i == 3 ? .25
                                  : i < 6  ? .37
                                           : 0,
                                  -5};
        o[0].previous = (de_dvec3){cases[i].previous, i < 4 ? .37 : i == 4 ? .25 : .37, -5};
        o[0].snapshot = cases[i].snap || previous_snap;
        o[1].current = (de_dvec3){cases[i].current2, i < 3 ? .37 : .5, -5};
        o[1].previous = (de_dvec3){cases[i].previous2, i < 4 ? .37 : .5, -5};
        pixel_frame(f);
        previous_snap = cases[i].snap;
    }
    EXPECT_EQ(o[0].calls, 7u);
    EXPECT_EQ(o[1].calls, 7u);
    EXPECT_TRUE(fabs(((DeTransform *)de_entity_get(f->world, f->sprite, bi.transform))->position.x -
                     11.37) < 1e-12);
}
TEST_F(palette, previous_sprite_grid_survives_large_world_origin_rebase, pixel_fixture) {
    start_pixel(f, DE_GPU_NULL, 16, 16, 64, 64, false);
    de__view_unregister_stage(f->r, observe_internal, f);
    de__view_unregister_stage(f->r, observe_ui, f);
    de__view_unregister_stage(f->r, pattern_stage, f);
    const de_builtin_component_ids bi = de_builtin_components(f->world);
    const DeRenderComponents *ids = de__render_components(f->world);
    const f64 position = 1000000010.37, first_camera = 1000000000.3, next_camera = 1000002000.6;
    ((DeTransform *)de_entity_get(f->world, f->sprite, bi.transform))->position.x = position;
    ((DeTransform *)de_entity_get(f->world, f->camera, bi.transform))->position.x = first_camera;
    ((DeCamera *)de_entity_get(f->world, f->camera, ids->camera))->ortho_height = 11.2f;
    de_pixel_art_settings grid = {
        .internal_width = 16, .internal_height = 16, .snap_sprites = true};
    de_view_set_pixel_art(f->r, f->view, &grid);
    f64 texel = (f64)11.2f / 16;
    f64 first = first_camera + round((position - first_camera) / texel) * texel;
    f64 z = round(-5 / texel) * texel;
    motion_snap_observer o = {
        .view = f->view, .current = {first, 0, z}, .previous = {first, 0, z}, .snapshot = true};
    ASSERT_EQ(de__view_register_stage(f->r, DE__VIEW_SLOT_POST, INT32_MAX - 3, "test.snap.rebase",
                                      observe_motion_snap, &o),
              DE_OK);
    pixel_frame(f);
    de_dvec3 original = o.origin;
    ((DeTransform *)de_entity_get(f->world, f->camera, bi.transform))->position.x = next_camera;
    o.current.x = next_camera + round((position - next_camera) / texel) * texel;
    pixel_frame(f);
    EXPECT_TRUE(fabs(o.origin.x - original.x) > 500);
    EXPECT_EQ(o.calls, 2u);
}
