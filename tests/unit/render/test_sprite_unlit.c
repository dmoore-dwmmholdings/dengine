/* DENG-763: unlit sprites (DeSprite.unlit) take the Unlit model, so each texel reaches the
 * output exactly with tonemapper NONE at exposure 1. Native backend, real renderer: an sRGB
 * pixel-art atlas of all 256 grey levels plus a 32-colour palette, read back with zero
 * tolerance, opaque (deferred resolve) and translucent (forward). */
#include "pixel_fixture.h"
#include "render/scene/gpu_scene.h"
#include "render/loaders/render_assets.h"
#include "render/formats/de_cooked_texture.h"

enum { ATLAS_W = 32, ATLAS_H = 9, SCALE = 2 };

static de_render_texture atlas;
static u8 atlas_texels[ATLAS_W * ATLAS_H * 4];

static const de_render_mesh *no_mesh(void *user, de_mesh mesh) {
    (void)user;
    (void)mesh;
    return NULL;
}
static const de_render_texture *atlas_record(void *user, de_texture texture) {
    (void)user;
    return texture.id ? &atlas : NULL;
}
static u32 atlas_index(void *user, de_texture texture) {
    (void)user;
    return texture.id ? atlas.bindless : 0;
}

/* Rows 0..7: grey levels 0..255, row by row. Row 8: a palette (PICO-8's sixteen colours, then
 * sixteen colours whose channels differ, chosen from the levels the 8-bit G-buffer rounds). */
static void fill_atlas(void) {
    static const u8 palette[32][3] = {
        {0, 0, 0},       {29, 43, 83},    {126, 37, 83},   {0, 135, 81},    {171, 82, 54},
        {95, 87, 79},    {194, 195, 199}, {255, 241, 232}, {255, 0, 77},    {255, 163, 0},
        {255, 236, 39},  {0, 228, 54},    {41, 173, 255},  {131, 118, 156}, {255, 119, 168},
        {255, 204, 170}, {76, 77, 78},    {79, 80, 81},    {82, 83, 84},    {85, 86, 87},
        {101, 102, 103}, {149, 150, 151}, {200, 201, 202}, {250, 251, 252}, {1, 2, 3},
        {10, 100, 200},  {200, 100, 10},  {77, 154, 231},  {33, 66, 99},    {254, 1, 128},
        {128, 254, 1},   {1, 128, 254}};
    for (u32 i = 0; i < 256; ++i) {
        u8 *p = atlas_texels + i * 4;
        p[0] = p[1] = p[2] = (u8)i;
        p[3] = 255;
    }
    for (u32 i = 0; i < 32; ++i) {
        u8 *p = atlas_texels + (256 + i) * 4;
        memcpy(p, palette[i], 3);
        p[3] = 255;
    }
}

static void upload_atlas(pixel_fixture *f) {
    fill_atlas();
    atlas = (de_render_texture){.width = ATLAS_W,
                                .height = ATLAS_H,
                                .mip_count = 1,
                                .format = DE_FMT_RGBA8_SRGB,
                                .flags = DE_COOKED_TEXTURE_SRGB | DE_COOKED_TEXTURE_PIXEL_ART};
    atlas.texture = de_gpu_texture_create(
        f->d, &(de_gpu_texture_desc){.width = ATLAS_W,
                                     .height = ATLAS_H,
                                     .format = DE_FMT_RGBA8_SRGB,
                                     .usage = DE_USAGE_COPY_DST | DE_USAGE_SAMPLED});
    ASSERT_TRUE(atlas.texture.id != 0);
    atlas.bindless = de_gpu_texture_bindless(f->d, atlas.texture, 0, false);
    const u32 pitch = 256;
    de_gpu_buffer upload = de_gpu_buffer_create(
        f->d, &(de_gpu_buffer_desc){
                  .size = pitch * ATLAS_H, .usage = DE_USAGE_COPY_SRC, .memory = DE_MEM_UPLOAD});
    u8 *data = de_gpu_buffer_map(f->d, upload);
    ASSERT_TRUE(data != NULL);
    for (u32 y = 0; y < ATLAS_H; ++y)
        memcpy(data + y * pitch, atlas_texels + y * ATLAS_W * 4, ATLAS_W * 4);
    de_gpu_cmd *cmd = de_gpu_cmd_begin(f->d, DE_QUEUE_GRAPHICS, "unlit sprite atlas upload");
    de_gpu_barrier barrier = {.texture = atlas.texture, .after = DE_ACCESS_COPY_DST};
    de_gpu_cmd_barriers(cmd, &barrier, 1);
    de_gpu_cmd_copy_buffer_to_texture(cmd, atlas.texture, 0, 0, upload, 0, pitch);
    barrier.before = DE_ACCESS_COPY_DST;
    barrier.after = DE_ACCESS_SHADER_READ;
    de_gpu_cmd_barriers(cmd, &barrier, 1);
    ASSERT_EQ(de_gpu_queue_submit(f->d, DE_QUEUE_GRAPHICS,
                                  &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
              DE_OK);
    de_gpu_wait_idle(f->d);
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
                                                                .texture_record = atlas_record,
                                                                .texture_bindless = atlas_index}},
                             &f->r->gpu_scene),
        DE_OK);
}

/* One pixel-art view (32 x 9 internal, 2x integer upscale) that shows the whole atlas on one
 * unlit sprite: 1 m per internal pixel, texel centres on pixel centres. */
static void start_unlit(pixel_fixture *f, bool translucent) {
    f->target_format = DE_FMT_RGBA8_SRGB; /* the output encodes sRGB, as a swapchain does */
    start_pixel(f, DE_GPU_METAL, ATLAS_W, ATLAS_H, ATLAS_W * SCALE, ATLAS_H * SCALE, false);
    upload_atlas(f);
    de_render_set_2d_plane(f->r, DE_PLANE_TOP_XY); /* facing the identity camera */
    const DeRenderComponents *ids = de__render_components(f->world);
    DeSprite *s = de_entity_get(f->world, f->sprite, ids->sprite);
    s->texture = (de_texture){1};
    s->size = (de_vec2){ATLAS_W, ATLAS_H};
    s->translucent = translucent;
    s->cast_shadows = false;
    s->unlit = true;
    DeCamera *camera = de_entity_get(f->world, f->camera, ids->camera);
    camera->tonemapper = de_tonemapper_builtin(f->r, DE_TM_NONE);
    camera->exposure_ev100 = -log2f(1.2f); /* exposure 1 / (1.2 x 2^EV100) = 1 */
    de_post_settings post = {.bloom_disable = true, .grading_disable = true};
    de_view_set_post(f->r, f->view, &post);
}

/* Every output pixel equals its texel; reports each mismatch (none are allowed). */
static void expect_exact(pixel_fixture *f, const char *what) {
    for (u32 i = 0; i < 4; ++i)
        pixel_frame(f);
    const u8 *out = read_output(f);
    u32 wrong = 0, grey_wrong = 0;
    for (u32 y = 0; y < f->oh; ++y)
        for (u32 x = 0; x < f->ow; ++x) {
            const u8 *o = out + ((size_t)y * f->ow + x) * 4;
            const u32 texel = (y / SCALE) * ATLAS_W + x / SCALE;
            const u8 *t = atlas_texels + texel * 4;
            if (o[0] == t[0] && o[1] == t[1] && o[2] == t[2])
                continue;
            if (wrong < 8)
                printf("%s: pixel %u,%u texel %u: got %u,%u,%u want %u,%u,%u\n", what, x, y, texel,
                       o[0], o[1], o[2], t[0], t[1], t[2]);
            ++wrong;
            grey_wrong += texel < 256;
        }
    printf("%s: %u of %u pixels differ (%u in the grey ramp)\n", what, wrong, f->ow * f->oh,
           grey_wrong);
    EXPECT_EQ(wrong, 0u);
}

static void cleanup_atlas(pixel_fixture *f) {
    (void)make_pattern; /* pixel_fixture.h helper this test does not use */
    if (f->d && atlas.texture.id) {
        de_gpu_wait_idle(f->d);
        de_gpu_texture_destroy(f->d, atlas.texture);
    }
    atlas = (de_render_texture){0};
}

TEST_F(sprite_unlit, exact_grey_ramp_and_palette_opaque, pixel_fixture) {
    start_unlit(f, false);
    expect_exact(f, "opaque unlit sprite");
    cleanup_atlas(f);
}

TEST_F(sprite_unlit, exact_grey_ramp_and_palette_translucent, pixel_fixture) {
    start_unlit(f, true);
    expect_exact(f, "translucent unlit sprite");
    cleanup_atlas(f);
}

/* The atlas region and flips apply on the Unlit path as on the Sprite path. */
TEST_F(sprite_unlit, atlas_region_and_flip, pixel_fixture) {
    start_unlit(f, false);
    const DeRenderComponents *ids = de__render_components(f->world);
    DeSprite *s = de_entity_get(f->world, f->sprite, ids->sprite);
    /* Texels 8..23 of rows 2..4, drawn 16 x 3 and mirrored left to right. */
    s->uv_rect = (de_vec4){8.f / ATLAS_W, 2.f / ATLAS_H, 16.f / ATLAS_W, 3.f / ATLAS_H};
    s->size = (de_vec2){16, 3};
    s->flip_x = true;
    for (u32 i = 0; i < 4; ++i)
        pixel_frame(f);
    const u8 *out = read_output(f);
    /* The 16 x 3 sprite is centred: internal columns 8..23, rows 3..5. */
    u32 wrong = 0;
    for (u32 y = 0; y < 3; ++y)
        for (u32 x = 0; x < 16; ++x) {
            const u32 ox = (8 + x) * SCALE, oy = (3 + y) * SCALE;
            const u8 *o = out + ((size_t)oy * f->ow + ox) * 4;
            const u8 *t = atlas_texels + ((2 + y) * ATLAS_W + 8 + (15 - x)) * 4;
            if (o[0] != t[0] || o[1] != t[1] || o[2] != t[2]) {
                if (wrong < 8)
                    printf("region pixel %u,%u: got %u,%u,%u want %u,%u,%u\n", x, y, o[0], o[1],
                           o[2], t[0], t[1], t[2]);
                ++wrong;
            }
        }
    EXPECT_EQ(wrong, 0u);
    cleanup_atlas(f);
}
