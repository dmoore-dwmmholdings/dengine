#define _CRT_SECURE_NO_WARNINGS
#include "de_test.h"
#include "cook/importers/aseprite/atlas_pack.h"
#include "render/formats/de_cooked_texture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct texture_fx {
    de_allocator allocator;
    size_t calls, fail_at, live, allocated;
    de_aseprite_doc *doc;
    de_aseprite_atlas *atlas;
    void *blob[2];
    u64 bytes[2];
    u8 source[8192];
} texture_fx;
typedef struct tracked_block {
    size_t bytes, marker;
} tracked_block;
static void *allocate(void *user, size_t bytes, size_t align, u32 tag) {
    (void)tag;
    texture_fx *f = user;
    ASSERT_TRUE(align <= 16);
    if (f->calls++ == f->fail_at)
        return NULL;
    tracked_block *block = malloc(sizeof *block + bytes);
    if (!block)
        return NULL;
    *block = (tracked_block){bytes, 0x41544c4153u};
    ++f->live;
    f->allocated += bytes;
    return block + 1;
}
static void release(void *user, void *ptr, size_t bytes) {
    if (!ptr)
        return;
    texture_fx *f = user;
    tracked_block *block = (tracked_block *)ptr - 1;
    ASSERT_EQ(block->marker, (size_t)0x41544c4153u);
    ASSERT_EQ(block->bytes, bytes);
    --f->live;
    f->allocated -= bytes;
    free(block);
}
static void texture_fx_setup(texture_fx *f) {
    f->fail_at = SIZE_MAX;
    f->allocator = (de_allocator){.alloc = allocate, .free = release, .user = f};
}
static void texture_fx_teardown(texture_fx *f) {
    for (u32 i = 0; i < 2; ++i)
        DE_FREE(&f->allocator, f->blob[i], (size_t)f->bytes[i]);
    de_aseprite_atlas_free(f->atlas);
    de_aseprite_free(f->doc);
    EXPECT_EQ(f->live, 0u);
    EXPECT_EQ(f->allocated, 0u);
}
static void build(texture_fx *f) {
    FILE *file = fopen(DE_ASEPRITE_DATA_DIR "/atlas.aseprite", "rb");
    ASSERT_TRUE(file != NULL);
    size_t size = fread(f->source, 1, sizeof f->source, file);
    bool ok = feof(file) && !ferror(file);
    fclose(file);
    ASSERT_TRUE(ok);
    ASSERT_EQ(de_aseprite_parse(f->source, size, &f->allocator, 0, &f->doc, NULL), DE_OK);
    const de_aseprite_atlas_desc desc = {.max_size = 32};
    ASSERT_EQ(de_aseprite_atlas_build(f->doc, &f->allocator, &desc, &f->atlas, NULL), DE_OK);
}
static void verify_payload(texture_fx *f, u32 blob, bool pixel_art) {
    de_cooked_texture_view view;
    const char *why = NULL;
    ASSERT_TRUE(de_cooked_texture_parse(f->blob[blob], f->bytes[blob], &view, &why));
    ASSERT_TRUE(why == NULL);
    ASSERT_EQ(view.width, f->atlas->width);
    ASSERT_EQ(view.height, f->atlas->height);
    ASSERT_EQ(view.format, (u32)DE_FMT_RGBA8_SRGB);
    ASSERT_EQ(view.dim, (u32)DE_TEX_2D);
    ASSERT_EQ(view.mip_count, 1u);
    ASSERT_EQ(view.depth_or_layers, 1u);
    ASSERT_EQ(view.flags & DE_COOKED_TEXTURE_PIXEL_ART,
              pixel_art ? (u32)DE_COOKED_TEXTURE_PIXEL_ART : 0u);
    ASSERT_TRUE(view.flags & DE_COOKED_TEXTURE_SRGB);
    ASSERT_TRUE(view.flags & DE_COOKED_TEXTURE_HAS_ALPHA);
    de_cooked_texture_subresource sub;
    ASSERT_TRUE(de_cooked_texture_subresource_at(&view, 0, &sub));
    for (u32 y = 0; y < view.height; ++y) {
        const u8 *row = (const u8 *)f->blob[blob] + sub.offset + (size_t)y * sub.row_pitch;
        ASSERT_MEMEQ(row, f->atlas->rgba + (size_t)y * view.width * 4u, (size_t)view.width * 4u);
        for (u32 x = view.width * 4u; x < sub.row_pitch; ++x)
            ASSERT_EQ(row[x], 0u);
    }
    for (u64 at = DE_COOKED_TEXTURE_HEADER_SIZE + DE_COOKED_TEXTURE_SUBRESOURCE_SIZE;
         at < sub.offset; ++at)
        ASSERT_EQ(((const u8 *)f->blob[blob])[at], 0u);
}
TEST_F(aseprite_texture, actual_atlas_dtex_exact_pixels_determinism_and_sampling, texture_fx) {
    build(f);
    for (u32 i = 0; i < 2; ++i)
        ASSERT_EQ(
            de_aseprite_atlas_encode(f->atlas, &f->allocator, true, &f->blob[i], &f->bytes[i]),
            DE_OK);
    ASSERT_EQ(f->bytes[0], f->bytes[1]);
    ASSERT_MEMEQ(f->blob[0], f->blob[1], (size_t)f->bytes[0]);
    verify_payload(f, 0, true);
    DE_FREE(&f->allocator, f->blob[1], (size_t)f->bytes[1]);
    f->blob[1] = NULL;
    ASSERT_EQ(de_aseprite_atlas_encode(f->atlas, &f->allocator, false, &f->blob[1], &f->bytes[1]),
              DE_OK);
    verify_payload(f, 1, false);
}
TEST_F(aseprite_texture, every_writer_allocation_failure_cleans_up, texture_fx) {
    build(f);
    size_t calls = f->calls, live = f->live, bytes = f->allocated;
    ASSERT_EQ(de_aseprite_atlas_encode(f->atlas, &f->allocator, true, &f->blob[0], &f->bytes[0]),
              DE_OK);
    size_t count = f->calls - calls;
    DE_FREE(&f->allocator, f->blob[0], (size_t)f->bytes[0]);
    f->blob[0] = NULL;
    f->bytes[0] = 0;
    for (size_t i = 0; i < count; ++i) {
        f->fail_at = f->calls + i;
        ASSERT_EQ(
            de_aseprite_atlas_encode(f->atlas, &f->allocator, true, &f->blob[0], &f->bytes[0]),
            DE_ERR_OUT_OF_MEMORY);
        ASSERT_TRUE(f->blob[0] == NULL);
        ASSERT_EQ(f->bytes[0], 0u);
        ASSERT_EQ(f->live, live);
        ASSERT_EQ(f->allocated, bytes);
    }
    f->fail_at = SIZE_MAX;
}
TEST_F(aseprite_texture, invalid_inputs_clear_outputs_without_allocating, texture_fx) {
    build(f);
    size_t calls = f->calls;
    f->blob[0] = (void *)1;
    f->bytes[0] = 123;
    ASSERT_EQ(de_aseprite_atlas_encode(NULL, &f->allocator, true, &f->blob[0], &f->bytes[0]),
              DE_ERR_INVALID_ARGUMENT);
    ASSERT_TRUE(f->blob[0] == NULL);
    ASSERT_EQ(f->bytes[0], 0u);
    de_aseprite_atlas invalid = *f->atlas;
    --invalid.rgba_bytes;
    ASSERT_EQ(de_aseprite_atlas_encode(&invalid, &f->allocator, true, &f->blob[0], &f->bytes[0]),
              DE_ERR_INVALID_ARGUMENT);
    ASSERT_EQ(f->calls, calls);
}
