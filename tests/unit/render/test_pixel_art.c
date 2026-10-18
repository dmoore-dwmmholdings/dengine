#include "pixel_fixture.h"
#include "de_test.h"
#include "render/pixel/pixel_internal.h"
#include "render/renderer_internal.h"
#include "render/world/render_world.h"
#include "render/components_internal.h"
#include "render/graph/rg_internal.h"
#include "render/lighting/lighting.h"
#include <dengine/de_reflect.h>
#include <dengine/de_ecs.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
TEST(pixel, integer_scale_math) {
    de__pixel_layout a = de__pixel_layout_make(480, 270, 1920, 1080, true);
    ASSERT_TRUE(a.scale == 4 && a.x == 0 && a.y == 0);
    a = de__pixel_layout_make(480, 270, 2560, 1440, true);
    ASSERT_TRUE(a.scale == 5 && a.x == 80 && a.y == 45 && a.width == 2400 && a.height == 1350);
    a = de__pixel_layout_make(480, 270, 2560, 1440, false);
    ASSERT_TRUE(fabsf(a.width - 2560) < .001f && fabsf(a.height - 1440) < .001f);
}
TEST(pixel, camera_snap_subtexel_double) {
    const f64 texel = 1.0 / 16;
    de_vec2 residual;
    for (u32 frame = 0; frame < 20; ++frame) {
        f64 x = 1000000000.0 + frame * .3 * texel;
        de_dvec3 p = de__pixel_snap((de_dvec3){x, 0, 3}, DE_PLANE_TOP_XY, texel, &residual);
        ASSERT_TRUE(fabs(p.x / texel - round(p.x / texel)) < 1e-6);
        ASSERT_TRUE(fabs((x - p.x) / texel - residual.x) < 1e-6);
        ASSERT_TRUE(fabs((p.x + residual.x * texel) - x) < 1e-6);
    }
    de_dvec3 p = de__pixel_snap((de_dvec3){.02, 9, .08}, DE_PLANE_SIDE_XZ, .1, &residual);
    ASSERT_TRUE(p.y == 9 && fabs(p.z - .1) < 1e-12);
}
TEST_F(pixel, graph_internal_and_output_res, pixel_fixture) {
    start_pixel(f, DE_GPU_NULL, 480, 270, 2560, 1440, true);
    pixel_frame(f);
    ASSERT_EQ(f->internal_seen, 1u);
    ASSERT_EQ(f->ui_seen, 1u);
    const de_builtin_component_ids bi = de_builtin_components(f->world);
    DeTransform *t = de_entity_get(f->world, f->camera, bi.transform);
    t->position.x = .3;
    pixel_frame(f);
    ASSERT_TRUE(fabsf(f->residual.x - .3f) < 1e-6f);
    ASSERT_TRUE(fabsf(f->view_matrix.m[12]) < 1e-6f);
    t = de_entity_get(f->world, f->camera, bi.transform);
    t->position.x = .6;
    pixel_frame(f);
    ASSERT_TRUE(fabsf(f->residual.x + .4f) < 1e-6f);
    ASSERT_TRUE(fabsf(f->view_matrix.m[12] + 1) < 1e-6f);
    t = de_entity_get(f->world, f->camera, bi.transform);
    t->position.x = 1000000000.3;
    pixel_frame(f);
    ASSERT_TRUE(fabsf(f->residual.x - .3f) < 1e-6f);
    ASSERT_TRUE(fabsf(f->view_matrix.m[12] - roundf(f->view_matrix.m[12])) < 1e-6f);
}
static void check_pattern(pixel_fixture *f, u32 scale, u32 left, u32 top) {
    const u8 *data = read_output(f);
    u32 mismatches = 0;
    for (u32 y = 0; y < f->oh; ++y)
        for (u32 x = 0; x < f->ow; ++x) {
            const u8 *p = data + ((size_t)y * f->ow + x) * 4;
            if (x < left || y < top || x >= left + 480 * scale || y >= top + 270 * scale) {
                mismatches +=
                    abs((i32)p[0] - 32) > 1 || abs((i32)p[1] - 64) > 1 || abs((i32)p[2] - 128) > 1;
            } else {
                u32 ix = (x - left) / scale, iy = (y - top) / scale;
                mismatches +=
                    p[0] != (u8)ix || p[1] != (u8)iy || p[2] != (((ix + iy) & 1) ? 255 : 0);
            }
        }
    printf("Pixel upscale %ux%u: %u mismatches\n", f->ow, f->oh, mismatches);
    ASSERT_EQ(mismatches, 0u);
}
TEST_F(pixel, integer_four_readback, pixel_fixture) {
    start_pixel(f, DE_GPU_METAL, 480, 270, 1920, 1080, false);
    make_pattern(f);
    pixel_frame(f);
    check_pattern(f, 4, 0, 0);
}
TEST_F(pixel, integer_five_letterbox_readback, pixel_fixture) {
    start_pixel(f, DE_GPU_METAL, 480, 270, 2560, 1440, false);
    make_pattern(f);
    pixel_frame(f);
    check_pattern(f, 5, 80, 45);
    de_pixel_art_settings settings = {
        .internal_width = 480, .internal_height = 270, .integer_scale_only = false};
    de_view_set_pixel_art(f->r, f->view, &settings);
    pixel_frame(f);
    const u8 *data = read_output(f);
    // Fractional mode fills the viewport: an outer edge pixel comes from the pattern, not
    // letterbox.
    ASSERT_TRUE(data[0] != 32 || data[1] != 64 || data[2] != 128);
}
TEST_F(pixel, smooth_border_residual_readback, pixel_fixture) {
    start_pixel(f, DE_GPU_METAL, 480, 270, 1920, 1080, true);
    make_pattern(f);
    pixel_frame(f);
    const u8 *before = read_output(f);
    u8 old = before[3 * 4];
    const de_builtin_component_ids bi = de_builtin_components(f->world);
    DeTransform *t = de_entity_get(f->world, f->camera, bi.transform);
    t->position.x = .3;
    pixel_frame(f);
    const u8 *after = read_output(f);
    ASSERT_TRUE(fabsf(f->residual.x - .3f) < 1e-6f);
    ASSERT_TRUE(fabsf(f->view_matrix.m[12]) < 1e-6f);
    printf("Residual .3 texel at 4x moves edge by one output pixel: %u -> %u\n", old, after[3 * 4]);
    ASSERT_EQ(old, 1u);
    ASSERT_EQ(after[3 * 4], 2u);
}

/* Retained source-review regression: fractional magnification must hold texel centers. */
TEST_F(pixel, fractional_sharp_interior_readback, pixel_fixture) {
    start_pixel(f, DE_GPU_METAL, 480, 270, 2560, 1440, false);
    de_pixel_art_settings settings = {
        .internal_width = 480, .internal_height = 270, .integer_scale_only = false};
    de_view_set_pixel_art(f->r, f->view, &settings);
    make_pattern(f);
    pixel_frame(f);
    const u8 *data = read_output(f);
    /* (7.5,2.5)/5.333 = (1.40625,.46875): well inside white texel (1,0). */
    u8 blue = data[((size_t)2 * f->ow + 7) * 4 + 2];
    printf("Fractional sharp checker interior: blue=%u (expected255)\n", blue);
    EXPECT_TRUE(blue >= 254);
}
