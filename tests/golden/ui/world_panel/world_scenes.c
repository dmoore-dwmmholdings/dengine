#include "world_fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <stb_image_write.h>
static void world_scene(de_golden_ctx *ctx, u32 variant) {
#if DE_HAS_UI && DE_HAS_RENDER && defined(DE_GOLDEN_COOK)
    ui_scene s = {0};
    de_result result = setup(&s, ctx, false);
    const bool far = variant == 1, shifted = variant == 5;
    const f64 shift = shifted ? 100000 : 0;
    if (result == DE_OK) {
        DeTransform pose = de_transform_identity();
        pose.position = (de_dvec3){shift, shift, 0};
        *(DeTransform *)add(&s, s.root, "DeTransform") = pose;
        DeUiWorldPanel *panel = add(&s, s.root, "DeUiWorldPanel");
        if (panel)
            *panel = (DeUiWorldPanel){.size_m = {1.6f, .9f},
                                      .pixels_per_meter = 400,
                                      .lit = variant == 4,
                                      .depth_test = variant != 3};
        pose.position.z = far ? 20 : 1;
        *(DeTransform *)de_entity_get(s.world, s.camera, component(&s, "DeTransform")) = pose;
        DeCamera *camera = de_entity_get(s.world, s.camera, component(&s, "DeCamera"));
        if (far)
            camera->fov_y = 2 * atanf(tanf(.6f) / 20);
        label(&s, s.root, 32, 28, 576, 48, "WORLD TERMINAL", 32);
        label(&s, s.root, 32, 92, 576, 36, "Real MSDF / physical panel", 20);
        node(&s, s.root, 32, 152, 576, 2, ink, 0, 0);
        node(&s, s.root, 32, 184, 272, 112, accent, 1, 4);
        node(&s, s.root, 328, 184, 272, 112, (de_vec4){.2f, .1f, .025f, 1}, 1, 4);
        label(&s, s.root, 48, 208, 240, 40, "640 x 360", 24);
        label(&s, s.root, 344, 208, 240, 40, "1.6 x 0.9 m", 24);
        label(&s, s.root, 32, 320, 576, 28, far ? "Camera distance: 20 m" : "Camera distance: 1 m",
              16);
        de_render_set_2d_plane(s.renderer, DE_PLANE_TOP_XY);
        if (variant == 2 || variant == 3) {
            de_entity wall = de_entity_spawn(s.world);
            pose.position = (de_dvec3){shift, shift, .35};
            *(DeTransform *)add(&s, wall, "DeTransform") = pose;
            DeSprite *sprite = add(&s, wall, "DeSprite");
            if (sprite)
                *sprite = (DeSprite){.size = {.4f, 1.4f},
                                     .pivot = {.5f, .5f},
                                     .uv_rect = {0, 0, 1, 1},
                                     .color = {.09f, .12f, .16f, 1}};
        }
        { /* Identical lamp in lit/unlit/origin scenes: only panel.lit changes. */
            de_entity light = de_entity_spawn(s.world);
            pose.position = (de_dvec3){shift - .3, shift + .4, .6};
            *(DeTransform *)add(&s, light, "DeTransform") = pose;
            DeLight *lamp = add(&s, light, "DeLight");
            if (lamp)
                *lamp =
                    (DeLight){.kind = DE_LIGHT_POINT, .color = {1, .1f, .02f}, .intensity = 300};
        }
        result = render(&s, 1);
    }
    u8 *pixels = NULL;
    if (result == DE_OK) {
        pixels = malloc((size_t)s.width * s.height * 4);
        if (!pixels)
            result = DE_ERR_OUT_OF_MEMORY;
    }
    if (result == DE_OK)
        result = capture(&s, pixels);
    if (result == DE_OK && far) {
        /* A second actual readback keeps the base1.2-radian FOV at20m. The reference retains
         * legible telephoto text; this oracle additionally exercises the genuinely smaller
         * MSDF screen footprint rather than testing only identical projected pixels. */
        size_t bytes = (size_t)s.width * s.height * 4;
        u8 *fixed_fov = malloc(bytes), *empty = malloc(bytes);
        if (!fixed_fov || !empty)
            result = DE_ERR_OUT_OF_MEMORY;
        if (result == DE_OK) {
            ((DeCamera *)de_entity_get(s.world, s.camera, component(&s, "DeCamera")))->fov_y = 1.2f;
            for (u32 i = 0; i < 4 && result == DE_OK; ++i)
                result = pump(&s);
            if (result == DE_OK)
                result = capture(&s, fixed_fov);
#if defined(_WIN32)
            char *far_capture = NULL;
            size_t far_capture_bytes = 0;
            (void)_dupenv_s(&far_capture, &far_capture_bytes, "DE_UI_FAR_CAPTURE");
#else
            const char *far_capture = getenv("DE_UI_FAR_CAPTURE");
#endif
            if (result == DE_OK && far_capture && far_capture[0])
                (void)stbi_write_png(far_capture, (int)s.width, (int)s.height, 4, fixed_fov,
                                     (int)s.width * 4);
#if defined(_WIN32)
            free(far_capture);
#endif
            ((DeUiNode *)de_entity_get(s.world, s.root, component(&s, "DeUiNode")))->display =
                DE_UI_NONE;
            for (u32 i = 0; i < 2 && result == DE_OK; ++i)
                result = pump(&s);
            if (result == DE_OK)
                result = capture(&s, empty);
            if (result == DE_OK) {
                u32 covered = 0;
                for (u64 i = 0; i < (u64)s.width * s.height; ++i)
                    covered += memcmp(fixed_fov + i * 4, empty + i * 4, 4) != 0;
                printf("20m fixed-FOV actual UI coverage: %u pixels\n", covered);
                if (covered < 100 || covered > 5000)
                    result =
                        DE_FAIL(DE_ERR_INTERNAL, "20m fixed-FOV UI footprint %u pixels", covered);
            }
        }
        free(fixed_fov);
        free(empty);
    }
    if (result == DE_OK)
        output(&s, pixels);
    else
        de_golden_fail(ctx, "World UI variant%u: %s", variant, de_result_str(result));
    free(pixels);
    cleanup(&s);
#else
    (void)variant;
    de_golden_skip(ctx, "World UI requires UI/render/cooker");
#endif
}
DE_GOLDEN_SCENE(ui_world_1m) {
    world_scene(ctx, 0);
}
DE_GOLDEN_SCENE(ui_world_20m) {
    world_scene(ctx, 1);
}
DE_GOLDEN_SCENE(ui_world_occluded) {
    world_scene(ctx, 2);
}
DE_GOLDEN_SCENE(ui_world_overlay) {
    world_scene(ctx, 3);
}
DE_GOLDEN_SCENE(ui_world_lit) {
    world_scene(ctx, 4);
}
DE_GOLDEN_SCENE(ui_world_origin_100km) {
    world_scene(ctx, 5);
}
