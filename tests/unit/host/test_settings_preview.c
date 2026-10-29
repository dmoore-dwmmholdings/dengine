#include "de_test.h"
#include "host/editor/editor_settings_value.h"
#include "host/editor/editor_settings_image.h"
#include "render/formats/de_cooked_texture.h"
#include <math.h>
#include <string.h>

TEST(settings_values, owns_nested_tables_and_string_arrays_and_sorts_keys) {
    char bytes[256];
    mpack_writer_t w;
    mpack_writer_init(&w, bytes, sizeof bytes);
    mpack_start_map(&w, 2);
    mpack_write_cstr(&w, "z");
    mpack_start_array(&w, 2);
    mpack_write_cstr(&w, "a");
    mpack_write_cstr(&w, "b");
    mpack_finish_array(&w);
    mpack_write_cstr(&w, "a");
    mpack_start_map(&w, 1);
    mpack_write_cstr(&w, "on");
    mpack_write_bool(&w, true);
    mpack_finish_map(&w);
    mpack_finish_map(&w);
    size_t size = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    de_ep_kv_raw item = {.key = de_ep_cstr("nested"), .value = {bytes, size}};
    de_ep_map_raw map = {&item, 1};
    u8 storage[8192];
    de_ep_arena arena = {storage, sizeof storage, 0};
    de__manifest_kv *out = NULL;
    ASSERT_EQ(de__ed_settings_values(&map, &arena, &out), DE_OK);
    memset(bytes, 0, sizeof bytes);
    ASSERT_EQ(out[0].value.type, DE__MANIFEST_TABLE);
    EXPECT_STREQ(out[0].value.u.table.keys[0], "a");
    EXPECT_STREQ(out[0].value.u.table.keys[1], "z");
    EXPECT_TRUE(out[0].value.u.table.items[0].u.table.items[0].u.b);
    const de__manifest_value *array = &out[0].value.u.table.items[1];
    ASSERT_EQ(array->type, DE__MANIFEST_STRING_ARRAY);
    EXPECT_EQ(array->u.arr.count, 2u);
    EXPECT_STREQ(array->u.arr.items[1], "b");
}
TEST(settings_values, rejects_non_toml_duplicates_depth_and_arena_exhaustion) {
    char bytes[256];
    u8 storage[4096];
    for (u32 variant = 0; variant < 5; ++variant) {
        mpack_writer_t w;
        mpack_writer_init(&w, bytes, sizeof bytes);
        if (variant == 0)
            mpack_write_nil(&w);
        if (variant == 1)
            mpack_write_u64(&w, UINT64_MAX);
        if (variant == 2)
            mpack_write_double(&w, INFINITY);
        if (variant == 3)
            mpack_write_str(&w, "a\0b", 3);
        if (variant == 4) {
            for (u32 i = 0; i < 18; ++i)
                mpack_start_array(&w, 1);
            mpack_write_i32(&w, 1);
            for (u32 i = 0; i < 18; ++i)
                mpack_finish_array(&w);
        }
        size_t n = mpack_writer_buffer_used(&w);
        ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
        de_ep_kv_raw item = {.key = de_ep_cstr("key"), .value = {bytes, n}};
        de_ep_map_raw map = {&item, 1};
        de_ep_arena arena = {storage, sizeof storage, 0};
        de__manifest_kv *out = (void *)(uintptr_t)123;
        EXPECT_EQ(de__ed_settings_values(&map, &arena, &out), DE_ERR_INVALID_ARGUMENT);
        EXPECT_TRUE(out == (void *)(uintptr_t)123);
    }
    const char one = 1;
    de_ep_kv_raw duplicate[2] = {{.key = de_ep_cstr("key"), .value = {&one, 1}},
                                 {.key = de_ep_cstr("key"), .value = {&one, 1}}};
    de_ep_map_raw map = {duplicate, 2};
    de_ep_arena arena = {storage, sizeof storage, 0};
    de__manifest_kv *out = NULL;
    EXPECT_EQ(de__ed_settings_values(&map, &arena, &out), DE_ERR_INVALID_ARGUMENT);
    arena.size = 1;
    arena.used = 0;
    EXPECT_EQ(de__ed_settings_values(&map, &arena, &out), DE_ERR_OUT_OF_MEMORY);
}
static void texture(u8 *blob, u32 format, u32 rows) {
    memset(blob, 0, 1536);
    memcpy(blob, "DTEX", 4);
    de_cooked_put_u32(blob + 4, 1);
    de_cooked_put_u32(blob + 8, DE_TEX_2D);
    de_cooked_put_u32(blob + 12, format);
    de_cooked_put_u32(blob + 16, 4);
    de_cooked_put_u32(blob + 20, 4);
    de_cooked_put_u32(blob + 24, 1);
    de_cooked_put_u32(blob + 28, 1);
    de_cooked_put_u32(blob + 32,
                      de_cooked_texture_format_is_srgb(format) ? DE_COOKED_TEXTURE_SRGB : 0);
    de_cooked_put_u32(blob + 40, 1);
    de_cooked_put_u64(blob + 48, 512 + (u64)rows * 256);
    de_cooked_put_u64(blob + 64, 512);
    de_cooked_put_u64(blob + 72, (u64)rows * 256);
    de_cooked_put_u32(blob + 80, 256);
    de_cooked_put_u32(blob + 84, rows);
}
TEST(settings_pixels, decodes_actual_bc1_and_pitched_rgba_and_rejects_truncation) {
    u8 blob[1536], pixels[64];
    u32 width = 0, height = 0;
    const char *format = NULL;
    texture(blob, DE_FMT_BC1_SRGB, 1);
    blob[512] = 0;
    blob[513] = 0xf8; // RGB565 red, every selector0
    ASSERT_EQ(de__ed_settings_pixels(blob, 768, 4, 4, pixels, &width, &height, &format), DE_OK);
    EXPECT_STREQ(format, "BC1 sRGB");
    EXPECT_EQ(width, 4u);
    EXPECT_EQ(height, 4u);
    for (u32 i = 0; i < 16; ++i) {
        EXPECT_EQ(pixels[i * 4], 255u);
        EXPECT_EQ(pixels[i * 4 + 1], 0u);
        EXPECT_EQ(pixels[i * 4 + 3], 255u);
    }
    texture(blob, DE_FMT_RGBA8_SRGB, 4);
    for (u32 y = 0; y < 4; ++y)
        for (u32 x = 0; x < 4; ++x) {
            u8 *p = blob + 512 + y * 256 + x * 4;
            p[0] = (u8)(x * 40);
            p[1] = (u8)(y * 60);
            p[3] = 255;
        }
    ASSERT_EQ(de__ed_settings_pixels(blob, sizeof blob, 2, 2, pixels, &width, &height, &format),
              DE_OK);
    EXPECT_EQ(pixels[0], 0u);
    EXPECT_EQ(pixels[4], 80u);
    EXPECT_EQ(pixels[9], 120u);
    memset(pixels, 0xa5, sizeof pixels);
    EXPECT_EQ(de__ed_settings_pixels(blob, sizeof blob - 1, 2, 2, pixels, &width, &height, &format),
              DE_ERR_PARSE);
    EXPECT_EQ(pixels[0], 0xa5u);
    EXPECT_EQ(de__ed_settings_pixels(blob, sizeof blob, 257, 2, pixels, &width, &height, &format),
              DE_ERR_INVALID_ARGUMENT);
}
