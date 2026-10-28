#define _CRT_SECURE_NO_WARNINGS
#include "de_test.h"
#include "anim/sprite_clip_format.h"
#include "cook/lib/de_cooker.h"
#include "render/formats/de_cooked_texture.h"
#include <dengine/de_platform.h>
#include <stb_image.h>
#include <stdio.h>
#include <string.h>

typedef struct reference_fx {
    de_tracking_allocator tracking;
    de_allocator allocator;
    de_cooker *cooker;
    de_cook_result result;
    u8 *pixels, *png;
    size_t bytes;
} reference_fx;
static void reference_fx_setup(reference_fx *f) {
    de_fp_env_init_thread();
    f->allocator = de_tracking_allocator_init(&f->tracking, NULL, DE_MEM_ASSET);
    ASSERT_EQ(de_cooker_create(&(de_cooker_desc){.allocator = &f->allocator,
                                                 .verify_determinism = DE_COOK_VERIFY_ON},
                               &f->cooker),
              DE_OK);
    de_asset_type_desc descriptor;
    ASSERT_TRUE(de_cooker_builtin_importer("aseprite", &descriptor));
    ASSERT_EQ(de_cooker_register_type(f->cooker, &descriptor), DE_OK);
    ASSERT_EQ(de_fs_mount("aseref", DE_ASEPRITE_DATA_DIR, 0, 0), DE_OK);
}
static void reference_fx_teardown(reference_fx *f) {
    stbi_image_free(f->png);
    DE_FREE(&f->allocator, f->pixels, f->bytes);
    de_cook_result_free(&f->result);
    de_cooker_destroy(f->cooker);
    EXPECT_EQ(de_fs_unmount("aseref", DE_ASEPRITE_DATA_DIR), DE_OK);
    EXPECT_EQ(de_tracking_allocator_stats(&f->tracking).bytes_live, 0u);
}
static const struct {
    const char *source, *layer, *reference;
} references[] = {{"rgba", NULL, "rgba"},
                  {"indexed", NULL, "indexed"},
                  {"gray", NULL, "gray"},
                  {"legacy6", NULL, "legacy6"},
                  {"legacy8", NULL, "legacy8"},
                  {"mixed-palette", NULL, "mixed-palette"},
                  {"palette-packets", NULL, "palette-packets"},
                  {"nested", NULL, "nested"},
                  {"group-flag", NULL, "group-flag"},
                  {"z-order", NULL, "z-order"},
                  {"alpha", NULL, "alpha"},
                  {"group-order", NULL, "group-order"},
                  {"legacy-group-order", NULL, "legacy-group-order"},
                  {"indexed-background", NULL, "indexed-background"},
                  {"atlas", NULL, "atlas"},
                  {"atlas-grid", NULL, "atlas-grid"},
                  {"playback", NULL, "playback"},
                  {"deep", NULL, "deep"},
                  {"background-opacity", NULL, "background-opacity"},
                  {"transparent-rgb", NULL, "transparent-rgb"},
                  {"rgba", "Body", "rgba-body"},
                  {"nested", "Red", "nested-red"},
                  {"nested", "Green", "nested-green"},
                  {"playback", "Body", "playback-body"}};

TEST_F(aseprite_reference, complete_cooked_frames_equal_real_aseprite_exports, reference_fx) {
    u32 compared = 0;
    for (u32 which = 0; which < sizeof references / sizeof references[0]; ++which) {
        const char *layer_name = references[which].layer;
        char source[256], selected[256];
        int n = snprintf(source, sizeof source, "%s.aseprite", references[which].source);
        ASSERT_TRUE(n > 0 && (size_t)n < sizeof source);
        de__manifest_kv settings = {
            .key = "layers",
            .value = {.type = DE_ASSET_SETTING_STRING, .u.s = layer_name ? "keep" : "flatten"}};
        de__manifest_asset asset = {.id = "reference/sprite",
                                    .type = "aseprite",
                                    .source = source,
                                    .file = "aseref:/assets.toml",
                                    .settings = &settings,
                                    .setting_count = 1};
        ASSERT_EQ(de_cooker_cook(f->cooker,
                                 &(de_cook_input){.asset = &asset, .asset_root = "aseref:/"},
                                 &f->result),
                  DE_OK);
        const de_cook_subasset *atlas = NULL, *layer = NULL;
        if (layer_name) {
            n = snprintf(selected, sizeof selected, "layer/%s", layer_name);
            ASSERT_TRUE(n > 0 && (size_t)n < sizeof selected);
        }
        for (u32 i = 0; i < f->result.subasset_count; ++i) {
            const de_cook_subasset *sub = &f->result.subassets[i];
            if (!strcmp(sub->name, "atlas"))
                atlas = sub;
            if (layer_name && !strcmp(sub->name, selected))
                layer = sub;
        }
        ASSERT_TRUE(atlas != NULL);
        ASSERT_TRUE(!layer_name || layer != NULL);
        de_cooked_texture_view texture;
        ASSERT_TRUE(de_cooked_texture_parse(atlas->data, atlas->size, &texture, NULL));
        de_cooked_texture_subresource resource;
        ASSERT_TRUE(de_cooked_texture_subresource_at(&texture, 0, &resource));
        de__sprite_clip_view clip;
        ASSERT_TRUE(de__sprite_clip_parse(layer ? layer->data : f->result.data,
                                          layer ? layer->size : f->result.size, &clip, NULL));
        ASSERT_TRUE(clip.canvas_width <= 256 && clip.canvas_height <= 256);
        f->bytes = (size_t)clip.canvas_width * clip.canvas_height * 4u;
        f->pixels = DE_ALLOC(&f->allocator, f->bytes, 16, DE_MEM_ASSET);
        ASSERT_TRUE(f->pixels != NULL);
        for (u32 i = 0; i < clip.frame_count; ++i) {
            de__sprite_frame frame = de__sprite_frame_at(&clip, i);
            memset(f->pixels, 0, f->bytes);
            u32 x = (u32)(frame.rect[0] * texture.width + .5f);
            u32 y = (u32)(frame.rect[1] * texture.height + .5f);
            ASSERT_TRUE((u64)x + frame.trim[2] <= texture.width);
            ASSERT_TRUE((u64)y + frame.trim[3] <= texture.height);
            for (u32 row = 0; row < frame.trim[3]; ++row)
                memcpy(f->pixels +
                           ((size_t)(frame.trim[1] + row) * clip.canvas_width + frame.trim[0]) * 4u,
                       (const u8 *)atlas->data + resource.offset +
                           (size_t)(y + row) * resource.row_pitch + x * 4u,
                       (size_t)frame.trim[2] * 4u);
            char path[1024];
            n = snprintf(path, sizeof path, "%s/reference/%s-%u.png", DE_ASEPRITE_DATA_DIR,
                         references[which].reference, i);
            ASSERT_TRUE(n > 0 && (size_t)n < sizeof path);
            int width, height, channels;
            f->png = stbi_load(path, &width, &height, &channels, 4);
            ASSERT_TRUE(f->png != NULL);
            ASSERT_EQ((u32)width, clip.canvas_width);
            ASSERT_EQ((u32)height, clip.canvas_height);
            bool same = memcmp(f->pixels, f->png, f->bytes) == 0;
            if (!same) {
                size_t at = 0;
                while (at < f->bytes && f->pixels[at] == f->png[at])
                    ++at;
                fprintf(stderr, "reference %s frame%u: byte%zu cooked=%u Aseprite=%u\n",
                        references[which].reference, i, at, f->pixels[at], f->png[at]);
            }
            EXPECT_TRUE(same);
            ++compared;
            stbi_image_free(f->png);
            f->png = NULL;
        }
        DE_FREE(&f->allocator, f->pixels, f->bytes);
        f->pixels = NULL;
        de_cook_result_free(&f->result);
    }
    EXPECT_EQ(compared, 71u);
    fprintf(stderr, "Compared %u complete cooked frames across24 Aseprite export sets\n", compared);
}
