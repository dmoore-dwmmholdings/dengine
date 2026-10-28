#define _CRT_SECURE_NO_WARNINGS
#include "de_test.h"
#include "anim/sprite_clip_format.h"
#include "cook/importers/aseprite/aseprite_import.h"
#include "cook/lib/cooker_internal.h"
#include "render/formats/de_cooked_texture.h"
#include <dengine/de_platform.h>
#include <stdio.h>
#include <string.h>

typedef struct ase_import_fx {
    de_tracking_allocator tracking;
    de_allocator allocator;
    de_cooker *cooker;
    de__cooker_type type;
    de_asset_import_ctx ctx;
    de_aseprite_doc *doc;
    de_aseprite_atlas *atlas;
    void *clip;
    u64 clip_bytes;
    u8 source[8192];
    size_t source_bytes;
} ase_import_fx;
static void ase_import_fx_setup(ase_import_fx *f) {
    de_fp_env_init_thread();
    f->allocator = de_tracking_allocator_init(&f->tracking, NULL, DE_MEM_ASSET);
    ASSERT_EQ(de_cooker_create(&(de_cooker_desc){.allocator = &f->allocator,
                                                 .verify_determinism = DE_COOK_VERIFY_ON},
                               &f->cooker),
              DE_OK);
    f->type = (de__cooker_type){.name = "aseprite", .import = de_aseprite_import};
    de__import_ctx_init(&f->ctx, f->cooker, &f->type);
    FILE *file = fopen(DE_ASEPRITE_DATA_DIR "/playback.aseprite", "rb");
    ASSERT_TRUE(file != NULL);
    f->source_bytes = fread(f->source, 1, sizeof f->source, file);
    bool ok = feof(file) && !ferror(file);
    fclose(file);
    ASSERT_TRUE(ok);
}
static void ase_import_fx_teardown(ase_import_fx *f) {
    DE_FREE(&f->allocator, f->clip, (size_t)f->clip_bytes);
    de_aseprite_atlas_free(f->atlas);
    de_aseprite_free(f->doc);
    de__import_ctx_release(&f->ctx);
    de_cooker_destroy(f->cooker);
    EXPECT_EQ(de_tracking_allocator_stats(&f->tracking).bytes_live, 0u);
}
static de_result cook(ase_import_fx *f, const de__manifest_kv *settings, u32 count) {
    de__import_ctx_release(&f->ctx);
    de__import_ctx_init(&f->ctx, f->cooker, &f->type);
    f->ctx.source = f->source;
    f->ctx.source_size = f->source_bytes;
    f->ctx.source_rel = "playback.aseprite";
    f->ctx.id_str = "hero/sprites";
    f->ctx.id = de_asset_id_from(f->ctx.id_str);
    f->ctx.settings = settings;
    f->ctx.setting_count = count;
    return de__cooker_run_import(&f->ctx);
}
TEST_F(aseprite_import, builtin_atlas_clip_geometry_tags_slices_and_keep_layers, ase_import_fx) {
    de_asset_type_desc descriptor;
    ASSERT_TRUE(de_cooker_builtin_importer("aseprite", &descriptor));
    EXPECT_EQ(descriptor.extension_count, 2u);
    EXPECT_EQ(descriptor.importer_version, 2u);
    const de__manifest_kv settings[] = {
        {.key = "layers", .value = {.type = DE_ASSET_SETTING_STRING, .u.s = "keep"}},
        {.key = "pixels_per_meter", .value = {.type = DE_ASSET_SETTING_INT, .u.i = 20}},
        {.key = "pixel_art", .value = {.type = DE_ASSET_SETTING_BOOL, .u.b = true}}};
    ASSERT_EQ(cook(f, settings, 3), DE_OK);
    ASSERT_EQ(f->ctx.sub_count, 2u);
    EXPECT_STREQ(f->ctx.subs[0].name, "atlas");
    EXPECT_STREQ(f->ctx.subs[0].type, "texture");
    EXPECT_STREQ(f->ctx.subs[1].name, "layer/Body");
    EXPECT_STREQ(f->ctx.subs[1].type, "sprite_clip");
    ASSERT_EQ(f->ctx.dep_count, 1u);
    EXPECT_EQ(f->ctx.deps[0].hash, de_asset_id_from("hero/sprites#atlas").hash);
    de_cooked_texture_view texture;
    ASSERT_TRUE(de_cooked_texture_parse(f->ctx.subs[0].data, f->ctx.subs[0].size, &texture, NULL));
    EXPECT_EQ(texture.mip_count, 1u);
    EXPECT_TRUE((texture.flags & DE_COOKED_TEXTURE_PIXEL_ART) != 0);
    de__sprite_clip_view v;
    ASSERT_TRUE(de__sprite_clip_parse(f->ctx.out, f->ctx.out_size, &v, NULL));
    EXPECT_EQ(v.version, 2u);
    EXPECT_EQ(v.frame_count, 4u);
    EXPECT_EQ(v.tag_count, 26u);
    EXPECT_EQ(v.slice_count, 4u);
    EXPECT_EQ(v.canvas_width, 8u);
    EXPECT_EQ(v.canvas_height, 6u);
    EXPECT_EQ(v.atlas, f->ctx.deps[0].hash);
    const u32 ms[] = {7, 13, 29, 41};
    const f32 pivots[][2] = {{2, 0}, {1, 0}, {0, 1}, {0, 0}};
    for (u32 i = 0; i < 4; ++i) {
        de__sprite_frame frame = de__sprite_frame_at(&v, i);
        EXPECT_EQ(frame.ms, ms[i]);
        EXPECT_EQ(frame.trim[0], i + 1);
        EXPECT_EQ(frame.trim[1], 2u);
        EXPECT_EQ(frame.trim[2], 1u);
        EXPECT_EQ(frame.trim[3], 1u);
        EXPECT_FLOAT_EQ(frame.size[0], .05f, 0);
        EXPECT_FLOAT_EQ(frame.size[1], .05f, 0);
        EXPECT_FLOAT_EQ(frame.pivot[0], pivots[i][0], 0);
        EXPECT_FLOAT_EQ(frame.pivot[1], pivots[i][1], 0);
    }
    u32 directions = 0, singles = 0;
    for (u32 i = 0; i < v.tag_count; ++i) {
        de__sprite_tag tag = de__sprite_tag_at(&v, i);
        if (tag.from == tag.to) {
            EXPECT_EQ(tag.from, 1u);
            ++singles;
        } else {
            EXPECT_EQ(tag.from, 0u);
            EXPECT_EQ(tag.to, 3u);
        }
        directions |= 1u << tag.direction;
    }
    EXPECT_EQ(directions, 15u);
    EXPECT_EQ(singles, 5u);
    de__sprite_clip_view layer;
    ASSERT_TRUE(de__sprite_clip_parse(f->ctx.subs[1].data, f->ctx.subs[1].size, &layer, NULL));
    EXPECT_EQ(layer.atlas, v.atlas);
    EXPECT_EQ(layer.frame_count, v.frame_count);
    /* Hidden green pixels at (0,0) cannot expand the measured bounds. */
    EXPECT_EQ(de__sprite_frame_at(&layer, 0).trim[0], 1u);
}
TEST_F(aseprite_import, full_cooker_twice_is_byte_identical_on_files_and_subassets, ase_import_fx) {
    de_asset_type_desc descriptor;
    ASSERT_TRUE(de_cooker_builtin_importer("aseprite", &descriptor));
    ASSERT_EQ(de_cooker_register_type(f->cooker, &descriptor), DE_OK);
    ASSERT_EQ(de_fs_mount("asefixtures", DE_ASEPRITE_DATA_DIR, 0, 0), DE_OK);
    de__manifest_kv keep = {.key = "layers",
                            .value = {.type = DE_ASSET_SETTING_STRING, .u.s = "keep"}};
    de__manifest_asset asset = {.id = "hero/sprites",
                                .type = "aseprite",
                                .source = "playback.aseprite",
                                .file = "asefixtures:/assets.toml",
                                .settings = &keep,
                                .setting_count = 1};
    de_cook_result a = {0}, b = {0};
    const de_cook_input input = {.asset = &asset, .asset_root = "asefixtures:/"};
    ASSERT_EQ(de_cooker_cook(f->cooker, &input, &a), DE_OK);
    ASSERT_EQ(de_cooker_cook(f->cooker, &input, &b), DE_OK);
    EXPECT_EQ(a.output_hash, b.output_hash);
    ASSERT_EQ(a.size, b.size);
    ASSERT_MEMEQ(a.data, b.data, (size_t)a.size);
    ASSERT_EQ(a.subasset_count, 2u);
    ASSERT_EQ(a.subasset_count, b.subasset_count);
    for (u32 i = 0; i < a.subasset_count; ++i) {
        ASSERT_EQ(a.subassets[i].size, b.subassets[i].size);
        ASSERT_MEMEQ(a.subassets[i].data, b.subassets[i].data, (size_t)a.subassets[i].size);
    }
    de_cook_result_free(&a);
    de_cook_result_free(&b);
    ASSERT_EQ(de_fs_unmount("asefixtures", DE_ASEPRITE_DATA_DIR), DE_OK);
}
TEST_F(aseprite_import, invalid_settings_and_layer_names_are_errors, ase_import_fx) {
    const de__manifest_kv bad[] = {
        {.key = "layers", .value = {.type = DE_ASSET_SETTING_STRING, .u.s = "other"}},
        {.key = "pixel_art", .value = {.type = DE_ASSET_SETTING_STRING, .u.s = "true"}},
        {.key = "pixels_per_meter", .value = {.type = DE_ASSET_SETTING_INT, .u.i = 0}},
        {.key = "pixels_per_meter", .value = {.type = DE_ASSET_SETTING_FLOAT, .u.f = 1e300}},
        {.key = "atlas_max_size", .value = {.type = DE_ASSET_SETTING_INT, .u.i = 2}},
        {.key = "pivot_slice", .value = {.type = DE_ASSET_SETTING_STRING, .u.s = "missing"}}};
    for (u32 i = 0; i < sizeof bad / sizeof bad[0]; ++i)
        EXPECT_EQ(cook(f, &bad[i], 1), DE_ERR_INVALID_ARGUMENT);
    ASSERT_EQ(cook(f, NULL, 0), DE_OK);
    EXPECT_EQ(f->ctx.sub_count, 1u);
    bool changed = false;
    for (size_t i = 0; i + 4 < f->source_bytes; ++i)
        if (!memcmp(f->source + i, "Body", 4)) {
            memcpy(f->source + i, "B#dy", 4);
            changed = true;
            break;
        }
    ASSERT_TRUE(changed);
    de__manifest_kv keep = {.key = "layers",
                            .value = {.type = DE_ASSET_SETTING_STRING, .u.s = "keep"}};
    EXPECT_EQ(cook(f, &keep, 1), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(cook(f, NULL, 0), DE_OK); /* Flattening does not invent/validate layer asset IDs. */
}
TEST_F(aseprite_import, slice_override_vectors_are_exact_typed_and_range_checked, ase_import_fx) {
    de_asset_setting_value values[5] = {0};
    for (u32 i = 0; i < 5; ++i)
        values[i] = (de_asset_setting_value){.type = DE_ASSET_SETTING_INT, .u.i = 1};
    de__manifest_kv setting = {
        .key = "slice.origin.pivot",
        .value = {.type = DE_ASSET_SETTING_ARRAY, .u.values = {.items = values, .count = 2}}};
    for (u32 count = 0; count <= 5; ++count) {
        setting.value.u.values.count = count;
        EXPECT_EQ(cook(f, &setting, 1), count == 2 ? DE_OK : DE_ERR_INVALID_ARGUMENT);
        if (count != 2)
            EXPECT_TRUE(strstr(f->ctx.msg, "slice.origin.pivot") != NULL);
    }
    setting.value.u.values.count = 2;
    const de_asset_setting_value bad[] = {
        {.type = DE_ASSET_SETTING_FLOAT, .u.f = 1},
        {.type = DE_ASSET_SETTING_BOOL, .u.b = true},
        {.type = DE_ASSET_SETTING_STRING, .u.s = "1"},
        {.type = DE_ASSET_SETTING_INT, .u.i = (i64)INT32_MIN - 1},
        {.type = DE_ASSET_SETTING_INT, .u.i = (i64)INT32_MAX + 1}};
    for (u32 i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        values[1] = bad[i];
        EXPECT_EQ(cook(f, &setting, 1), DE_ERR_INVALID_ARGUMENT);
        EXPECT_TRUE(strstr(f->ctx.msg, "slice.origin.pivot[1]") != NULL);
        EXPECT_EQ(f->ctx.out_size, 0u);
        EXPECT_EQ(f->ctx.sub_count, 0u);
    }
    values[0].u.i = INT32_MIN;
    values[1] = (de_asset_setting_value){.type = DE_ASSET_SETTING_INT, .u.i = INT32_MAX};
    ASSERT_EQ(cook(f, &setting, 1), DE_OK);
    setting.key = "slice.origin.center";
    setting.value.u.values.count = 4;
    values[2].u.i = UINT32_MAX;
    values[3].u.i = 0;
    ASSERT_EQ(cook(f, &setting, 1), DE_OK);
    for (u32 index = 2; index < 4; ++index) {
        i64 before = values[index].u.i;
        values[index].u.i = -1;
        EXPECT_EQ(cook(f, &setting, 1), DE_ERR_INVALID_ARGUMENT);
        EXPECT_TRUE(strstr(f->ctx.msg, "dimension") != NULL);
        values[index].u.i = (i64)UINT32_MAX + 1;
        EXPECT_EQ(cook(f, &setting, 1), DE_ERR_INVALID_ARGUMENT);
        values[index].u.i = before;
    }
    setting.value = (de_asset_setting_value){.type = DE_ASSET_SETTING_STRING, .u.s = "1,2,3,4"};
    EXPECT_EQ(cook(f, &setting, 1), DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(strstr(f->ctx.msg, "exactly 4 integers") != NULL);
    setting.value =
        (de_asset_setting_value){.type = DE_ASSET_SETTING_ARRAY, .u.values = {.count = 4}};
    EXPECT_EQ(cook(f, &setting, 1), DE_ERR_INVALID_ARGUMENT);
}
static de_result override_manifest_read(void *user, const char *path, const de_allocator *a,
                                        void **data, u64 *bytes) {
    (void)path;
    const char *text = user;
    size_t size = strlen(text);
    char *copy = DE_ALLOC(a, size + 1, 1, DE_MEM_ASSET);
    if (!copy)
        return DE_ERR_OUT_OF_MEMORY;
    memcpy(copy, text, size + 1);
    *data = copy;
    *bytes = size;
    return DE_OK;
}
TEST_F(aseprite_import, manifest_slice_edits_are_deterministic_preserve_pixels_and_reset,
       ase_import_fx) {
    de_asset_type_desc descriptor;
    ASSERT_TRUE(de_cooker_builtin_importer("aseprite", &descriptor));
    ASSERT_EQ(de_cooker_register_type(f->cooker, &descriptor), DE_OK);
    ASSERT_EQ(de_fs_mount("asefixtures", DE_ASEPRITE_DATA_DIR, 0, 0), DE_OK);
    char manifest[] = "[assets.\"hero/sprites\"]\ntype='aseprite'\nsource='playback.aseprite'\n"
                      "layers='keep'\n\"slice.origin.pivot\"=[-7,9]\n"
                      "\"slice.origin.center\"=[-2,3,7,0]\n\"slice.late.pivot\"=[8,-4]\n";
    de__manifest *parsed = NULL;
    ASSERT_EQ(de__manifest_parse(&(de__manifest_parse_desc){.path = "asefixtures:/assets.toml",
                                                            .allocator = &f->allocator,
                                                            .read = override_manifest_read,
                                                            .read_user = manifest},
                                 &parsed),
              DE_OK);
    const de__manifest_asset *asset = de__manifest_find_asset(parsed, "hero/sprites");
    ASSERT_TRUE(asset != NULL);
    de_cook_input input = {.asset = asset, .asset_root = "asefixtures:/"};
    de_cook_result a = {0}, b = {0}, original = {0}, reset = {0};
    ASSERT_EQ(de_cooker_cook(f->cooker, &input, &a), DE_OK);
    ASSERT_EQ(de_cooker_cook(f->cooker, &input, &b), DE_OK);
    EXPECT_EQ(a.output_hash, b.output_hash);
    ASSERT_EQ(a.size, b.size);
    ASSERT_MEMEQ(a.data, b.data, (size_t)a.size);
    ASSERT_EQ(a.subasset_count, 2u);
    ASSERT_EQ(a.subasset_count, b.subasset_count);
    for (u32 i = 0; i < a.subasset_count; ++i) {
        ASSERT_EQ(a.subassets[i].size, b.subassets[i].size);
        ASSERT_MEMEQ(a.subassets[i].data, b.subassets[i].data, (size_t)a.subassets[i].size);
    }
    de__sprite_clip_view view;
    ASSERT_TRUE(de__sprite_clip_parse(a.data, a.size, &view, NULL));
    u32 origin_keys = 0, late_keys = 0;
    for (u32 i = 0; i < view.slice_count; ++i) {
        de__sprite_slice key = de__sprite_slice_at(&view, i);
        if (key.name == de_name_from("origin").hash) {
            ++origin_keys;
            EXPECT_EQ(key.pivot_x, -7);
            EXPECT_EQ(key.pivot_y, 9);
            EXPECT_EQ(key.center_x, -2);
            EXPECT_EQ(key.center_y, 3);
            EXPECT_EQ(key.center_width, 7u);
            EXPECT_EQ(key.center_height, 0u);
        } else {
            ++late_keys;
            EXPECT_EQ(key.name, de_name_from("late").hash);
            EXPECT_EQ(key.pivot_x, 8);
            EXPECT_EQ(key.pivot_y, -4);
            EXPECT_EQ(key.flags, 3u);    /* Source late has no pivot; the edit adds it. */
            EXPECT_EQ(key.center_x, -1); /* Unedited center is retained. */
        }
    }
    EXPECT_EQ(origin_keys, 3u); /* Hidden keys are overridden without making them visible. */
    EXPECT_EQ(late_keys, 1u);
    de__manifest_kv keep = {.key = "layers",
                            .value = {.type = DE_ASSET_SETTING_STRING, .u.s = "keep"}};
    de__manifest_asset unedited = *asset;
    unedited.settings = &keep;
    unedited.setting_count = 1;
    input.asset = &unedited;
    ASSERT_EQ(de_cooker_cook(f->cooker, &input, &original), DE_OK);
    ASSERT_EQ(de_cooker_cook(f->cooker, &input, &reset), DE_OK);
    EXPECT_NE(a.output_hash, reset.output_hash);
    EXPECT_NE(a.settings_hash, reset.settings_hash);
    ASSERT_EQ(original.size, reset.size);
    ASSERT_MEMEQ(original.data, reset.data, (size_t)reset.size);
    ASSERT_EQ(a.subassets[0].size, original.subassets[0].size);
    ASSERT_MEMEQ(a.subassets[0].data, original.subassets[0].data, (size_t)a.subassets[0].size);
    void *source = NULL;
    u64 bytes;
    ASSERT_EQ(de_file_read_all("asefixtures:/playback.aseprite", &f->allocator, &source, &bytes),
              DE_OK);
    ASSERT_EQ(bytes, f->source_bytes);
    ASSERT_MEMEQ(source, f->source, (size_t)bytes);
    DE_FREE(&f->allocator, source, (size_t)bytes + 1);
    de_cook_result_free(&a);
    de_cook_result_free(&b);
    de_cook_result_free(&original);
    de_cook_result_free(&reset);
    de__manifest_destroy(parsed);
    ASSERT_EQ(de_fs_unmount("asefixtures", DE_ASEPRITE_DATA_DIR), DE_OK);
}
static void prepare_clip(ase_import_fx *f) {
    ASSERT_EQ(de_aseprite_parse(f->source, f->source_bytes, &f->allocator, 0, &f->doc, NULL),
              DE_OK);
    ASSERT_EQ(de_aseprite_atlas_build(f->doc, &f->allocator, NULL, &f->atlas, NULL), DE_OK);
    ASSERT_EQ(de_aseprite_clip_encode(f->doc, f->atlas, 0, de_asset_id_from("hero/sprites#atlas"),
                                      100, NULL, &f->allocator, &f->clip, &f->clip_bytes, NULL),
              DE_OK);
}
TEST_F(aseprite_import, slice_overrides_add_absent_fields_and_match_long_dotted_names,
       ase_import_fx) {
    FILE *file = fopen(DE_ASEPRITE_DATA_DIR "/slice-overrides.aseprite", "rb");
    ASSERT_TRUE(file != NULL);
    f->source_bytes = fread(f->source, 1, sizeof f->source, file);
    bool ok = feof(file) && !ferror(file);
    fclose(file);
    ASSERT_TRUE(ok);
    u64 source_hash = de_hash64(f->source, f->source_bytes, 0);
    char long_key[2100] = "slice.long.";
    memset(long_key + 11, 'n', 2048);
    memcpy(long_key + 2059, ".pivot", sizeof ".pivot");
    const de_asset_setting_value pivot[] = {{.type = DE_ASSET_SETTING_INT, .u.i = -17},
                                            {.type = DE_ASSET_SETTING_INT, .u.i = 23}};
    const de_asset_setting_value center[] = {{.type = DE_ASSET_SETTING_INT, .u.i = 1},
                                             {.type = DE_ASSET_SETTING_INT, .u.i = 2},
                                             {.type = DE_ASSET_SETTING_INT, .u.i = 3},
                                             {.type = DE_ASSET_SETTING_INT, .u.i = 0}};
    const de__manifest_kv settings[] = {
        {.key = "slice.bare.with.dots.pivot",
         .value = {.type = DE_ASSET_SETTING_ARRAY, .u.values = {.items = pivot, .count = 2}}},
        {.key = "slice.bare.with.dots.center",
         .value = {.type = DE_ASSET_SETTING_ARRAY, .u.values = {.items = center, .count = 4}}},
        {.key = long_key,
         .value = {.type = DE_ASSET_SETTING_ARRAY, .u.values = {.items = pivot, .count = 2}}}};
    ASSERT_EQ(cook(f, settings, 3), DE_OK);
    de__sprite_clip_view view;
    ASSERT_TRUE(de__sprite_clip_parse(f->ctx.out, f->ctx.out_size, &view, NULL));
    ASSERT_EQ(view.slice_count, 4u);
    for (u32 i = 0; i < view.slice_count; ++i) {
        de__sprite_slice key = de__sprite_slice_at(&view, i);
        EXPECT_EQ(key.pivot_x, -17);
        EXPECT_EQ(key.pivot_y, 23);
        if (key.name == de_name_from("bare.with.dots").hash) {
            EXPECT_EQ(key.flags, 3u);
            EXPECT_EQ(key.center_x, 1);
            EXPECT_EQ(key.center_y, 2);
            EXPECT_EQ(key.center_width, 3u);
            EXPECT_EQ(key.center_height, 0u);
        } else {
            EXPECT_EQ(key.flags, 2u);
            EXPECT_EQ(key.center_width, 0u);
        }
    }
    ASSERT_EQ(cook(f, NULL, 0), DE_OK);
    ASSERT_TRUE(de__sprite_clip_parse(f->ctx.out, f->ctx.out_size, &view, NULL));
    for (u32 i = 0; i < view.slice_count; ++i) {
        de__sprite_slice key = de__sprite_slice_at(&view, i);
        EXPECT_EQ(key.flags, 0u);
        EXPECT_EQ(key.pivot_x, 0);
        EXPECT_EQ(key.center_width, 0u);
    }
    EXPECT_EQ(de_hash64(f->source, f->source_bytes, 0), source_hash);
}
TEST_F(aseprite_import, geometry_format_bounds_padding_counts_and_truncation, ase_import_fx) {
    prepare_clip(f);
    de__sprite_clip_view v;
    for (u64 bytes = 0; bytes < f->clip_bytes; ++bytes)
        EXPECT_FALSE(de__sprite_clip_parse(f->clip, bytes, &v, NULL));
    ASSERT_TRUE(de__sprite_clip_parse(f->clip, f->clip_bytes, &v, NULL));
    u32 tags = de_cooked_get_u32((u8 *)f->clip + 28),
        slices = de_cooked_get_u32((u8 *)f->clip + 80);
    const u32 offsets[] = {4,          8,           24,         64,      68,        72,
                           76,         80,          84,         88,      92,        96 + 28,
                           96 + 32,    96 + 40,     96 + 48,    96 + 56, tags + 16, tags + 20,
                           slices + 8, slices + 12, slices + 56};
    for (u32 i = 0; i < sizeof offsets / sizeof offsets[0]; ++i) {
        u8 *at = (u8 *)f->clip + offsets[i];
        u32 previous = de_cooked_get_u32(at);
        de_cooked_put_u32(at, UINT32_MAX);
        EXPECT_FALSE(de__sprite_clip_parse(f->clip, f->clip_bytes, &v, NULL));
        de_cooked_put_u32(at, previous);
    }
    ASSERT_TRUE(de__sprite_clip_parse(f->clip, f->clip_bytes, &v, NULL));
}
typedef struct clip_failure {
    const de_allocator *parent;
    u32 calls, fail_at;
} clip_failure;
static void *failing_allocate(void *user, size_t bytes, size_t align, u32 tag) {
    clip_failure *f = user;
    return f->calls++ == f->fail_at ? NULL : DE_ALLOC(f->parent, bytes, align, tag);
}
static void failing_release(void *user, void *ptr, size_t bytes) {
    clip_failure *f = user;
    DE_FREE(f->parent, ptr, bytes);
}
static void *failing_reallocate(void *user, void *ptr, size_t old_size, size_t new_size,
                                size_t align, u32 tag) {
    clip_failure *f = user;
    return f->calls++ == f->fail_at ? NULL
                                    : DE_REALLOC(f->parent, ptr, old_size, new_size, align, tag);
}
TEST_F(aseprite_import, importer_override_allocation_failures_release_all_scratch, ase_import_fx) {
    const de_allocator *parent = f->cooker->alloc;
    u64 baseline = de_tracking_allocator_stats(&f->tracking).bytes_live;
    bool completed = false;
    for (u32 fail_at = 0; fail_at < 4096; ++fail_at) {
        clip_failure failure = {.parent = parent, .fail_at = fail_at};
        de_allocator allocator = {.alloc = failing_allocate,
                                  .realloc = failing_reallocate,
                                  .free = failing_release,
                                  .user = &failure};
        f->cooker->alloc = &allocator;
        de_result result = cook(f, NULL, 0);
        de__import_ctx_release(&f->ctx);
        f->cooker->alloc = parent;
        de__import_ctx_init(&f->ctx, f->cooker, &f->type);
        EXPECT_EQ(de_tracking_allocator_stats(&f->tracking).bytes_live, baseline);
        if (result == DE_OK) {
            EXPECT_EQ(failure.calls, fail_at);
            completed = true;
            break;
        }
        EXPECT_EQ(result, DE_ERR_OUT_OF_MEMORY);
    }
    EXPECT_TRUE(completed);
}
TEST_F(aseprite_import, clip_allocation_failures_and_duplicate_names_leave_no_blob, ase_import_fx) {
    prepare_clip(f);
    u64 before = de_tracking_allocator_stats(&f->tracking).bytes_live;
    for (u32 i = 0; i < 2; ++i) {
        clip_failure fail = {.parent = &f->allocator, .fail_at = i};
        de_allocator a = {.alloc = failing_allocate, .free = failing_release, .user = &fail};
        void *blob = (void *)1;
        u64 bytes = 123;
        EXPECT_EQ(de_aseprite_clip_encode(f->doc, f->atlas, 0, de_asset_id_from("atlas/test"), 100,
                                          NULL, &a, &blob, &bytes, NULL),
                  DE_ERR_OUT_OF_MEMORY);
        EXPECT_TRUE(blob == NULL);
        EXPECT_EQ(bytes, 0u);
        EXPECT_EQ(de_tracking_allocator_stats(&f->tracking).bytes_live, before);
    }
    f->doc->slices[1].name = f->doc->slices[0].name;
    void *blob = NULL;
    u64 bytes = 0;
    EXPECT_EQ(de_aseprite_clip_encode(f->doc, f->atlas, 0, de_asset_id_from("atlas/test"), 100,
                                      NULL, &f->allocator, &blob, &bytes, NULL),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(blob == NULL);
    EXPECT_EQ(bytes, 0u);
}
