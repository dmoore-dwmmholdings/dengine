#define _CRT_SECURE_NO_WARNINGS /* C11 fopen on fixture paths, matching other importer tests. */
#include "de_test.h"
#include "cook/importers/aseprite/aseprite_parse.h"
#include "cook/importers/aseprite/aseprite_render.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct tracker {
    size_t calls, fail_at, live, bytes;
} tracker;
typedef struct allocation {
    size_t bytes, marker;
} allocation;
static void *tracked_alloc(void *user, size_t bytes, size_t align, u32 tag) {
    (void)tag;
    tracker *t = user;
    ASSERT_TRUE(align <= 16);
    if (t->calls++ == t->fail_at)
        return NULL;
    allocation *p = malloc(sizeof *p + bytes);
    if (!p)
        return NULL;
    p->bytes = bytes;
    p->marker = 0x415345;
    ++t->live;
    t->bytes += bytes;
    return p + 1;
}
static void tracked_free(void *user, void *ptr, size_t bytes) {
    if (!ptr)
        return;
    tracker *t = user;
    allocation *p = (allocation *)ptr - 1;
    ASSERT_EQ(p->marker, (size_t)0x415345);
    ASSERT_EQ(p->bytes, bytes);
    --t->live;
    t->bytes -= bytes;
    p->marker = 0;
    free(p);
}
typedef struct ase_fx {
    tracker tracking;
    de_allocator allocator;
    de_aseprite_doc *doc;
    de_aseprite_renderer *renderer;
    u8 source[8192];
    size_t bytes;
} ase_fx;
static void ase_fx_setup(ase_fx *f) {
    f->tracking.fail_at = SIZE_MAX;
    f->allocator =
        (de_allocator){.alloc = tracked_alloc, .free = tracked_free, .user = &f->tracking};
}
static void ase_fx_teardown(ase_fx *f) {
    de_aseprite_renderer_destroy(f->renderer);
    de_aseprite_free(f->doc);
    EXPECT_EQ(f->tracking.live, 0u);
    EXPECT_EQ(f->tracking.bytes, 0u);
}
static void load(ase_fx *f, const char *name) {
    char path[1024];
    int n = snprintf(path, sizeof path, "%s/%s.aseprite", DE_ASEPRITE_DATA_DIR, name);
    ASSERT_TRUE(n > 0 && (size_t)n < sizeof path);
    FILE *file = fopen(path, "rb");
    ASSERT_TRUE(file != NULL);
    f->bytes = fread(f->source, 1, sizeof f->source, file);
    bool ok = !ferror(file) && feof(file);
    fclose(file);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(f->bytes >= 128);
}
static de_aseprite_doc *parse(ase_fx *f, const char *name) {
    load(f, name);
    const char *why = NULL;
    de_result r = de_aseprite_parse(f->source, f->bytes, &f->allocator, 0, &f->doc, &why);
    if (r != DE_OK)
        fprintf(stderr, "parse %s: %s\n", name, why ? why : "unknown");
    ASSERT_EQ(r, DE_OK);
    ASSERT_TRUE(f->doc != NULL);
    ASSERT_TRUE(why == NULL);
    return f->doc;
}
static void expect_string(de_aseprite_string value, const char *expected) {
    ASSERT_EQ(value.size, strlen(expected));
    ASSERT_MEMEQ(value.data, expected, value.size);
}
static u32 get32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static void put32(u8 *p, u32 value) {
    for (u32 i = 0; i < 4; ++i)
        p[i] = (u8)(value >> (i * 8));
}
static size_t find_chunk(ase_fx *f, u16 type, u32 occurrence) {
    size_t frame = 128;
    while (frame < f->bytes) {
        size_t end = frame + get32(f->source + frame);
        size_t at = frame + 16;
        while (at < end) {
            u16 kind = (u16)(f->source[at + 4] | ((u16)f->source[at + 5] << 8));
            if (kind == type && occurrence-- == 0)
                return at;
            at += get32(f->source + at);
        }
        frame = end;
    }
    FAIL("fixture chunk missing");
    return 0;
}
static void invalid(ase_fx *f) {
    const char *why = NULL;
    ASSERT_EQ(de_aseprite_parse(f->source, f->bytes, &f->allocator, 0, &f->doc, &why),
              DE_ERR_INVALID_ARGUMENT);
    ASSERT_TRUE(f->doc == NULL);
    ASSERT_TRUE(why != NULL);
    ASSERT_EQ(f->tracking.live, 0u);
}

TEST_F(aseprite, layers_links_and_user_data, ase_fx) {
    de_aseprite_doc *d = parse(f, "rgba");
    ASSERT_EQ(d->width, 2u);
    ASSERT_EQ(d->height, 1u);
    ASSERT_EQ(d->depth, 32u);
    ASSERT_EQ(d->frame_count, 2u);
    ASSERT_EQ(d->frames[0].ms, 100u);
    ASSERT_EQ(d->frames[1].ms, 200u);
    ASSERT_EQ(d->layer_count, 3u);
    ASSERT_EQ(d->cel_count, 3u);
    ASSERT_EQ(d->layers[0].parent, UINT32_MAX);
    ASSERT_EQ(d->layers[0].opacity, 128u);
    ASSERT_EQ(d->layers[1].parent, 0u);
    ASSERT_EQ(d->layers[2].flags & 1u, 0u);
    expect_string(d->layers[1].user.text, "layer-note");
    expect_string(d->cels[0].user.text, "cel-note");
    expect_string(d->user.text, "sprite-note");
    ASSERT_EQ(d->cels[2].pixels, d->cels[0].pixels);
    ASSERT_EQ(d->cels[2].width, 2u);
    ASSERT_EQ(d->cels[2].x, -1);
    ASSERT_EQ(d->cels[2].z, 2);
    ASSERT_EQ(d->cels[2].opacity, 200u);
    const u8 pixels[] = {255, 0, 0, 255, 0, 0, 255, 128};
    ASSERT_MEMEQ(d->cels[0].pixels, pixels, sizeof pixels);
}
TEST_F(aseprite, tags_and_slice_keys, ase_fx) {
    de_aseprite_doc *d = parse(f, "rgba");
    ASSERT_EQ(d->tag_count, 4u);
    for (u32 i = 0; i < 4; ++i) {
        ASSERT_EQ(d->tags[i].direction, i);
        ASSERT_EQ(d->tags[i].repeats, i);
        ASSERT_EQ(d->tags[i].from, 0u);
        ASSERT_EQ(d->tags[i].to, 1u);
        char text[16];
        snprintf(text, sizeof text, "tag-%u", i);
        expect_string(d->tags[i].user.text, text);
    }
    ASSERT_EQ(d->slice_count, 1u);
    const de_aseprite_slice *s = &d->slices[0];
    expect_string(s->name, "panel");
    expect_string(s->user.text, "slice-note");
    ASSERT_EQ(s->flags, 3u);
    ASSERT_EQ(s->key_count, 2u);
    ASSERT_EQ(s->keys[0].x, -1);
    ASSERT_EQ(s->keys[0].center_width, 2u);
    ASSERT_EQ(s->keys[0].pivot_x, 1);
    ASSERT_EQ(s->keys[0].pivot_y, -2);
    ASSERT_EQ(s->keys[1].frame, 1u);
    ASSERT_EQ(s->keys[1].width, 0u);
}
TEST_F(aseprite, indexed_compression_and_animated_palette, ase_fx) {
    de_aseprite_doc *d = parse(f, "indexed");
    ASSERT_EQ(d->depth, 8u);
    ASSERT_TRUE(d->has_new_palette);
    const u8 pixels[] = {1, 0}, red[] = {255, 0, 0, 255}, green[] = {0, 255, 0, 255};
    ASSERT_MEMEQ(d->cels[0].pixels, pixels, 2);
    ASSERT_EQ(d->cels[1].pixels, d->cels[0].pixels);
    ASSERT_MEMEQ(d->frames[0].palette[1], red, 4);
    ASSERT_MEMEQ(d->frames[1].palette[1], green, 4);
    ASSERT_EQ(d->transparent_index, 0u);
    de_aseprite_free(f->doc);
    f->doc = NULL;
    d = parse(f, "mixed-palette");
    ASSERT_EQ(d->frame_count, 3u);
    ASSERT_MEMEQ(d->frames[0].palette[1], red, 4);
    ASSERT_MEMEQ(d->frames[1].palette[1], green, 4);
    ASSERT_MEMEQ(d->frames[2].palette[1], green, 4);
}
TEST_F(aseprite, grayscale_and_legacy_palettes, ase_fx) {
    de_aseprite_doc *d = parse(f, "gray");
    ASSERT_EQ(d->depth, 16u);
    const u8 gray[] = {129, 64};
    ASSERT_MEMEQ(d->cels[0].pixels, gray, 2);
    de_aseprite_free(f->doc);
    f->doc = NULL;
    d = parse(f, "legacy6");
    const u8 six[] = {0, 130, 255, 255};
    ASSERT_MEMEQ(d->frames[0].palette[0], six, 4);
    de_aseprite_free(f->doc);
    f->doc = NULL;
    d = parse(f, "legacy8");
    const u8 eight[] = {0, 123, 255, 255};
    ASSERT_MEMEQ(d->frames[0].palette[0], eight, 4);
    de_aseprite_free(f->doc);
    f->doc = NULL;
    d = parse(f, "palette-packets");
    const u8 blue[] = {0, 0, 255, 255}, green[] = {0, 255, 0, 255};
    ASSERT_MEMEQ(d->frames[0].palette[2], blue, 4);
    ASSERT_MEMEQ(d->frames[0].palette[5], green, 4);
}
TEST_F(aseprite, forward_links_and_invalid_link_graphs, ase_fx) {
    de_aseprite_doc *d = parse(f, "forward");
    ASSERT_EQ(d->cels[0].pixels, d->cels[1].pixels);
    ASSERT_EQ(d->cels[0].pixels[0], 11u);
    de_aseprite_free(f->doc);
    f->doc = NULL;
    load(f, "cycle");
    invalid(f);
    load(f, "missing-link");
    invalid(f);
}
static de_aseprite_doc *render_parse(ase_fx *f, const char *name) {
    de_aseprite_renderer_destroy(f->renderer);
    f->renderer = NULL;
    de_aseprite_free(f->doc);
    f->doc = NULL;
    de_aseprite_doc *doc = parse(f, name);
    ASSERT_EQ(de_aseprite_renderer_create(doc, &f->allocator, 0, NULL, NULL, &f->renderer, NULL),
              DE_OK);
    return doc;
}
static void render(ase_fx *f, u32 frame, u32 layer, u8 *pixels, size_t bytes) {
    const char *why = "unchanged";
    ASSERT_EQ(de_aseprite_render_frame(f->renderer, frame, layer, pixels,
                                       (size_t)f->doc->width * 4u, bytes, &why),
              DE_OK);
    ASSERT_TRUE(why == NULL);
}
TEST_F(aseprite, composite_nested_groups_and_keep_layers, ase_fx) {
    render_parse(f, "nested");
    size_t calls = f->tracking.calls;
    u8 pixels[16];
    const u8 expected[] = {63, 64, 127, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255};
    render(f, 0, UINT32_MAX, pixels, sizeof pixels);
    ASSERT_MEMEQ(pixels, expected, sizeof pixels);
    render(f, 0, 2, pixels, sizeof pixels);
    const u8 red[] = {255, 0, 0, 128}, green[] = {0, 255, 0, 64}, zero[16] = {0};
    ASSERT_MEMEQ(pixels, red, 4);
    ASSERT_MEMEQ(pixels + 4, zero, 12);
    render(f, 0, 4, pixels, sizeof pixels);
    ASSERT_MEMEQ(pixels, green, 4);
    ASSERT_MEMEQ(pixels + 4, zero, 12);
    ASSERT_TRUE(de_aseprite_layer_visible(f->renderer, 4));
    ASSERT_FALSE(de_aseprite_layer_visible(f->renderer, 6));
    render(f, 0, 6, pixels, sizeof pixels);
    ASSERT_MEMEQ(pixels, zero, sizeof pixels);
    render(f, 1, UINT32_MAX, pixels, sizeof pixels);
    const u8 moved[] = {0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 64, 191, 255};
    ASSERT_MEMEQ(pixels, moved, sizeof pixels);
    render(f, 0, UINT32_MAX, pixels, sizeof pixels);
    ASSERT_MEMEQ(pixels, expected, sizeof pixels);
    ASSERT_EQ(f->tracking.calls, calls);
}
TEST_F(aseprite, composite_depths_palettes_and_alpha, ase_fx) {
    u8 pixels[8];
    render_parse(f, "indexed");
    render(f, 0, UINT32_MAX, pixels, sizeof pixels);
    const u8 red[] = {255, 0, 0, 255, 0, 0, 0, 0};
    ASSERT_MEMEQ(pixels, red, sizeof pixels);
    render(f, 1, UINT32_MAX, pixels, sizeof pixels);
    const u8 green[] = {0, 255, 0, 255, 0, 0, 0, 0};
    ASSERT_MEMEQ(pixels, green, sizeof pixels);
    render_parse(f, "gray");
    render(f, 0, UINT32_MAX, pixels, sizeof pixels);
    const u8 gray[] = {129, 129, 129, 64};
    ASSERT_MEMEQ(pixels, gray, 4);
    render_parse(f, "indexed-background");
    render(f, 0, UINT32_MAX, pixels, sizeof pixels);
    const u8 background[] = {255, 0, 0, 255, 0, 255, 0, 255};
    ASSERT_MEMEQ(pixels, background, sizeof pixels);
    render_parse(f, "alpha");
    render(f, 0, UINT32_MAX, pixels, sizeof pixels);
    const u8 alpha[] = {85, 0, 170, 192};
    ASSERT_MEMEQ(pixels, alpha, 4);
}
TEST_F(aseprite, composite_z_order_hidden_slots_and_group_boundaries, ase_fx) {
    u8 pixels[4];
    render_parse(f, "z-order");
    const u8 red[] = {255, 0, 0, 255}, blue[] = {0, 0, 255, 255};
    for (u32 frame = 0; frame < 4; ++frame) {
        render(f, frame, UINT32_MAX, pixels, sizeof pixels);
        ASSERT_MEMEQ(pixels, frame == 1 || frame == 2 ? red : blue, sizeof pixels);
    }
    render_parse(f, "group-order");
    render(f, 0, UINT32_MAX, pixels, sizeof pixels);
    ASSERT_MEMEQ(pixels, blue, sizeof pixels);
    render_parse(f, "legacy-group-order");
    render(f, 0, UINT32_MAX, pixels, sizeof pixels);
    ASSERT_MEMEQ(pixels, red, sizeof pixels);
}
typedef struct warnings {
    u32 count, blends, tilemaps;
} warnings;
static void capture_warning(void *user, de_aseprite_string layer, const char *message) {
    warnings *w = user;
    ++w->count;
    if (strstr(message, "tilemap")) {
        ++w->tilemaps;
        expect_string(layer, "Tiles");
    } else {
        ++w->blends;
        expect_string(layer, "Multiply");
    }
}
TEST_F(aseprite, composite_warns_once_with_layer_names, ase_fx) {
    parse(f, "warnings");
    warnings w = {0};
    ASSERT_EQ(de_aseprite_renderer_create(f->doc, &f->allocator, 0, capture_warning, &w,
                                          &f->renderer, NULL),
              DE_OK);
    ASSERT_EQ(w.count, 2u);
    ASSERT_EQ(w.blends, 1u);
    ASSERT_EQ(w.tilemaps, 1u);
    u8 pixels[4];
    const u8 red[] = {255, 0, 0, 255};
    render(f, 0, UINT32_MAX, pixels, sizeof pixels);
    ASSERT_MEMEQ(pixels, red, sizeof pixels);
    render(f, 0, UINT32_MAX, pixels, sizeof pixels);
    ASSERT_EQ(w.count, 2u);
}
TEST_F(aseprite, composite_group_opacity_uses_independent_header_flag, ase_fx) {
    de_aseprite_doc *doc = render_parse(f, "group-flag");
    ASSERT_EQ(doc->flags, 2u);
    ASSERT_EQ(doc->layers[1].opacity, 128u);
    ASSERT_EQ(doc->layers[3].opacity, 128u);
    u8 pixels[16];
    render(f, 0, UINT32_MAX, pixels, sizeof pixels);
    const u8 expected[] = {63, 64, 127, 255};
    ASSERT_MEMEQ(pixels, expected, sizeof expected);
}
TEST_F(aseprite, composite_budget_oom_stride_and_argument_failures, ase_fx) {
    parse(f, "nested");
    size_t calls = f->tracking.calls, live = f->tracking.live;
    ASSERT_EQ(de_aseprite_renderer_create(f->doc, &f->allocator, 1, NULL, NULL, &f->renderer, NULL),
              DE_ERR_INVALID_ARGUMENT);
    ASSERT_TRUE(f->renderer == NULL);
    ASSERT_EQ(f->tracking.calls, calls);
    f->tracking.fail_at = calls;
    ASSERT_EQ(de_aseprite_renderer_create(f->doc, &f->allocator, 0, NULL, NULL, &f->renderer, NULL),
              DE_ERR_OUT_OF_MEMORY);
    ASSERT_TRUE(f->renderer == NULL);
    ASSERT_EQ(f->tracking.live, live);
    f->tracking.fail_at = SIZE_MAX;
    ASSERT_EQ(de_aseprite_renderer_create(f->doc, &f->allocator, 0, NULL, NULL, &f->renderer, NULL),
              DE_OK);
    u8 pixels[24], saved[24];
    memset(pixels, 0xcc, sizeof pixels);
    ASSERT_EQ(de_aseprite_render_frame(f->renderer, 0, UINT32_MAX, pixels, 12, 20, NULL), DE_OK);
    for (u32 i = 8; i < 12; ++i) {
        ASSERT_EQ(pixels[i], 0xccu);
        ASSERT_EQ(pixels[i + 12], 0xccu);
    }
    memcpy(saved, pixels, sizeof pixels);
    ASSERT_EQ(de_aseprite_render_frame(f->renderer, 2, UINT32_MAX, pixels, 12, 20, NULL),
              DE_ERR_INVALID_ARGUMENT);
    ASSERT_EQ(de_aseprite_render_frame(f->renderer, 0, 1, pixels, 12, 20, NULL),
              DE_ERR_INVALID_ARGUMENT);
    ASSERT_EQ(de_aseprite_render_frame(f->renderer, 0, UINT32_MAX, pixels, 7, 20, NULL),
              DE_ERR_INVALID_ARGUMENT);
    ASSERT_EQ(de_aseprite_render_frame(f->renderer, 0, UINT32_MAX, pixels, 12, 19, NULL),
              DE_ERR_INVALID_ARGUMENT);
    ASSERT_EQ(
        de_aseprite_render_frame(f->renderer, 0, UINT32_MAX, pixels, SIZE_MAX, SIZE_MAX, NULL),
        DE_ERR_INVALID_ARGUMENT);
    ASSERT_MEMEQ(pixels, saved, sizeof pixels);
}
TEST_F(aseprite, composite_maximum_supported_group_depth, ase_fx) {
    render_parse(f, "deep");
    u8 pixels[4];
    render(f, 0, UINT32_MAX, pixels, sizeof pixels);
    const u8 red[] = {255, 0, 0, 255};
    ASSERT_MEMEQ(pixels, red, sizeof pixels);
    render(f, 0, 127, pixels, sizeof pixels);
    ASSERT_MEMEQ(pixels, red, sizeof pixels);
}
TEST_F(aseprite, every_truncation_and_chunk_boundaries, ase_fx) {
    load(f, "rgba");
    size_t full = f->bytes;
    for (size_t i = 0; i < full; ++i) {
        if (i >= 128)
            put32(f->source, (u32)i);
        f->bytes = i;
        invalid(f);
    }
    load(f, "rgba");
    size_t chunk = find_chunk(f, 0x2005, 0);
    put32(f->source + chunk, UINT32_MAX);
    invalid(f);
    load(f, "rgba");
    chunk = find_chunk(f, 0x2005, 0);
    put32(f->source + chunk, 5);
    invalid(f);
    load(f, "rgba");
    put32(f->source + 140, UINT32_MAX);
    invalid(f);
}
TEST_F(aseprite, invalid_image_sizes_and_zlib_checksum, ase_fx) {
    load(f, "indexed");
    size_t cel = find_chunk(f, 0x2005, 0);
    size_t end = cel + get32(f->source + cel);
    f->source[end - 1] ^= 1;
    invalid(f);
    load(f, "indexed");
    cel = find_chunk(f, 0x2005, 0);
    f->source[cel + 6 + 16] = 1;
    invalid(f); /* Inflate would exceed its declared one-byte image. */
    load(f, "gray");
    cel = find_chunk(f, 0x2005, 0);
    f->source[cel + 6 + 16] = 0;
    invalid(f);
}
TEST_F(aseprite, invalid_hierarchy_strings_and_duplicate_cels, ase_fx) {
    load(f, "rgba");
    size_t layer = find_chunk(f, 0x2004, 0);
    f->source[layer + 6 + 2] = 0;
    invalid(f); /* Parent is no longer a group. */
    load(f, "rgba");
    layer = find_chunk(f, 0x2004, 0);
    f->source[layer + 6 + 18] = 0xff;
    invalid(f);
    load(f, "rgba");
    size_t cel = find_chunk(f, 0x2005, 1);
    f->source[cel + 6] = 1;
    invalid(f);
}
TEST_F(aseprite, budget_and_every_allocation_failure_clean_up, ase_fx) {
    const char *names[] = {"rgba", "indexed"};
    for (size_t fixture = 0; fixture < 2; ++fixture) {
        f->tracking.calls = 0;
        f->tracking.fail_at = SIZE_MAX;
        parse(f, names[fixture]);
        size_t calls = f->tracking.calls;
        de_aseprite_free(f->doc);
        f->doc = NULL;
        const char *why = NULL;
        ASSERT_EQ(de_aseprite_parse(f->source, f->bytes, &f->allocator,
                                    sizeof(de_aseprite_doc) + 1u, &f->doc, &why),
                  DE_ERR_INVALID_ARGUMENT);
        ASSERT_TRUE(f->doc == NULL);
        ASSERT_EQ(f->tracking.live, 0u);
        for (size_t i = 0; i < calls; ++i) {
            f->tracking.calls = 0;
            f->tracking.fail_at = i;
            ASSERT_EQ(de_aseprite_parse(f->source, f->bytes, &f->allocator, 0, &f->doc, &why),
                      DE_ERR_OUT_OF_MEMORY);
            ASSERT_TRUE(f->doc == NULL);
            ASSERT_TRUE(why != NULL);
            ASSERT_EQ(f->tracking.live, 0u);
        }
    }
}
