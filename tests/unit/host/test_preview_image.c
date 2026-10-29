#include "de_test.h"
#include "host/editor/editor_preview_image.h"
#include <stb_image.h>
#include <stdlib.h>
#include <string.h>

TEST(preview_image, independent_decoder_recovers_rows_across_deflate_blocks) {
    const u32 width = 257, height = 70;
    const size_t bytes = (size_t)width * height * 4u;
    u8 *rgba = malloc(bytes), *png = malloc(de__ed_preview_png_size(width, height));
    ASSERT_TRUE(rgba && png);
    for (size_t i = 0; i < bytes; ++i)
        rgba[i] = (u8)(i * 19u + i / 103u);
    size_t written = 0;
    ASSERT_EQ(de__ed_preview_png(width, height, rgba, png, de__ed_preview_png_size(width, height),
                                 &written),
              DE_OK);
    int x, y, channels;
    u8 *decoded = stbi_load_from_memory(png, (int)written, &x, &y, &channels, 4);
    ASSERT_TRUE(decoded != NULL);
    EXPECT_EQ(x, (int)width);
    EXPECT_EQ(y, (int)height);
    EXPECT_EQ(channels, 4);
    EXPECT_EQ(memcmp(rgba, decoded, bytes), 0);
    stbi_image_free(decoded);
    free(rgba);
    free(png);
}
TEST(preview_image, invalid_dimensions_and_short_buffers_preserve_output) {
    u8 output[80], pixels[4] = {10, 20, 30, 255};
    memset(output, 0xa5, sizeof output);
    size_t written = 991;
    EXPECT_EQ(de__ed_preview_png_size(0, 1), 0u);
    EXPECT_EQ(de__ed_preview_png_size(UINT32_MAX, UINT32_MAX), 0u);
    EXPECT_EQ(de__ed_preview_png_size(1024, 1025), 0u);
    EXPECT_TRUE(de__ed_preview_png_size(1024, 1024) < 5u * 1024u * 1024u);
    EXPECT_EQ(de__ed_preview_png(1, 1, pixels, output, 72, &written), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(written, 991u);
    for (u32 i = 0; i < sizeof output; ++i)
        EXPECT_EQ(output[i], 0xa5u);
    ASSERT_EQ(de__ed_preview_png(1, 1, pixels, output, sizeof output, &written), DE_OK);
    EXPECT_EQ(written, 73u);
    int width, height, components;
    u8 *decoded = stbi_load_from_memory(output, (int)written, &width, &height, &components, 4);
    ASSERT_TRUE(decoded != NULL);
    EXPECT_EQ(memcmp(decoded, pixels, sizeof pixels), 0);
    stbi_image_free(decoded);
}
TEST(preview_image, pitched_half_float_conversion_handles_extremes_and_padding) {
    const u16 pixels[16] = {0x0000, 0x3800, 0x3c00, 0, 0xffff, 0xffff, 0xffff, 0xffff,
                            0xbc00, 0x7c00, 0x7e00, 0, 0xffff, 0xffff, 0xffff, 0xffff};
    u8 rgba[8];
    de__ed_preview_rgba(1, 2, pixels, 16, rgba);
    EXPECT_EQ(rgba[0], 0u);
    EXPECT_TRUE(rgba[1] >= 187 && rgba[1] <= 188);
    EXPECT_EQ(rgba[2], 255u);
    EXPECT_EQ(rgba[3], 255u);
    EXPECT_EQ(rgba[4], 0u);
    EXPECT_EQ(rgba[5], 255u);
    EXPECT_EQ(rgba[6], 0u);
    EXPECT_EQ(rgba[7], 255u);
}
