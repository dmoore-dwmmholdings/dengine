#include "font_test_util.h"
TEST(pixel_font, integer_cells_and_widths) {
    de__manifest_kv s[] = {
        {.key = "mode", .value = {.type = DE__MANIFEST_STRING, .u.s = "pixel"}},
        {.key = "map", .value = {.type = DE__MANIFEST_STRING, .u.s = "pixel.toml"}}};
    de_cook_result r = {0};
    ASSERT_EQ(font_cook("pixel.png", s, 2, &r), DE_OK);
    const u8 *p = r.data;
    EXPECT_EQ(de__font_u32(p + 8), 2u);
    ASSERT_EQ(de__font_u32(p + 12), 2u);
    EXPECT_EQ(de__font_u32(p + 64 + 16), 8u);
    EXPECT_EQ(de__font_u32(p + 64 + 48 + 8), 8u);
    u32 n = de__font_u32(p + 64 + 48 + 40);
    f32 advance;
    memcpy(&advance, &n, 4);
    EXPECT_FLOAT_EQ(advance, 7, .001);
    de_cook_result_free(&r);
}
