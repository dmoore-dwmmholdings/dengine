#include "cook_test_util.h"
#include "asset/cook_preview_internal.h"
#include "asset/asset_internal.h"
#include "audio/sound_format.h"
#include "render/formats/de_cooked_texture.h"
#include "render/formats/de_cooked_mesh.h"
#include <dengine/de_render.h>

#if DE_HAS_COOKER
typedef struct fixture {
    de_tracking_allocator tracking;
    de_allocator allocator;
    de_type_registry *types;
    de_world *world;
    de_asset_system *assets;
    char root[128];
} fixture;
static void setup(fixture *f, const char *name, const char *text) {
    memset(f, 0, sizeof *f);
    ckt_mount();
    static bool source_mounted;
    if (!source_mounted) {
        ASSERT_EQ(de_fs_mount("pvsource", DE_PREVIEW_SOURCE_DIR, DE_MOUNT_READ_ONLY, 0), DE_OK);
        source_mounted = true;
    }
    char path[128];
    snprintf(path, sizeof path, "preview_%s/assets.toml", name);
    ckt_put(path, text);
    snprintf(f->root, sizeof f->root, "ckt:/preview_%s", name);
    f->allocator = de_tracking_allocator_init(&f->tracking, NULL, DE_MEM_ASSET);
    ASSERT_EQ(de_type_registry_create(&f->allocator, &f->types), DE_OK);
    ASSERT_EQ(de_world_create(&(de_world_desc){.types = f->types}, &f->world), DE_OK);
    ASSERT_EQ(de_render_register_components(f->world), DE_OK);
    ASSERT_EQ(
        de_asset_system_create(
            &(de_asset_system_desc){.allocator = &f->allocator, .types = f->types}, &f->assets),
        DE_OK);
    char manifest[160];
    snprintf(manifest, sizeof manifest, "%s/assets.toml", f->root);
    ASSERT_EQ(de_asset_manifest_load(f->assets, manifest), DE_OK);
}
static void copy(fixture *f, const char *from, const char *to) {
    char source[512], dest[512];
    snprintf(source, sizeof source, "pvsource:/%s", from);
    snprintf(dest, sizeof dest, "%s/%s", f->root, to);
    void *data = NULL;
    u64 size = 0;
    ASSERT_EQ(de_file_read_all(source, &f->allocator, &data, &size), DE_OK);
    ASSERT_EQ(de_file_write_all_atomic(dest, data, size), DE_OK);
    DE_FREE(&f->allocator, data, (size_t)size + 1);
}
static void teardown(fixture *f) {
    de_asset_system_destroy(f->assets);
    de_world_destroy(f->world);
    de_type_registry_destroy(f->types);
    EXPECT_EQ(de_tracking_allocator_stats(&f->tracking).bytes_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->tracking).blocks_live, 0u);
}
TEST(cook_preview, actual_texture_compression_defaults_and_failure_atomicity) {
    fixture f;
    setup(&f, "texture",
          "# "
          "untouched\n[project]\nasset_root='.'\n[defaults.texture]\ncompression='bc7'\nmips="
          "false\n[assets.tex]\ntype='texture'\nsource='photo.png'\n");
    /* Above 64px, BC1/BC7 rows no longer both round up to the same 256-byte pitch. */
    copy(&f, "tests/unit/import/data/gltf/CesiumLogoFlat.png", "photo.png");
    const de__manifest_kv settings[] = {
        {"compression", {.type = DE__MANIFEST_STRING, .u.s = "bc1"}}};
    de__cook_preview result = {0};
    ASSERT_EQ(
        de__asset_cook_preview(f.assets, "tex", settings, 1, DE_COOK_PLATFORM_WINDOWS, &result),
        DE_OK);
    de_cooked_texture_view before, after;
    const char *why = NULL;
    ASSERT_TRUE(de_cooked_texture_parse(result.before.data, result.before.size, &before, &why));
    ASSERT_TRUE(de_cooked_texture_parse(result.after.data, result.after.size, &after, &why));
    EXPECT_EQ(before.format, DE_FMT_BC7_SRGB);
    EXPECT_EQ(after.format, DE_FMT_BC1_SRGB);
    EXPECT_EQ(after.mip_count, 1u); // omitted entry key inherits the manifest default
    EXPECT_TRUE(result.after.total_bytes < result.before.total_bytes);
    const de__manifest_asset *saved = de__manifest_find_asset(f.assets->manifest, "tex");
    EXPECT_STREQ(
        de__manifest_find_setting(saved->settings, saved->setting_count, "compression")->value.u.s,
        "bc7");
    de__asset_cook_preview_free(&result);
    EXPECT_TRUE(result.private_ == NULL);
    de__asset_cook_preview_free(&result);
    /* v1 variants remain ignored, including a matching requested target platform. */
    const char *variant_keys[] = {"windows"}, *setting_keys[] = {"compression"};
    const de__manifest_value variant_settings[] = {{.type = DE__MANIFEST_STRING, .u.s = "bc1"}};
    const de__manifest_value variants[] = {
        {.type = DE__MANIFEST_TABLE, .u.table = {setting_keys, variant_settings, 1}}};
    const de__manifest_kv variant_entry[] = {
        {"variants", {.type = DE__MANIFEST_TABLE, .u.table = {variant_keys, variants, 1}}}};
    ASSERT_EQ(de__asset_cook_preview(f.assets, "tex", variant_entry, 1, DE_COOK_PLATFORM_WINDOWS,
                                     &result),
              DE_OK);
    EXPECT_EQ(result.before.size, result.after.size);
    EXPECT_MEMEQ(result.before.data, result.after.data, (size_t)result.before.size);
    de__asset_cook_preview_free(&result);
    // No changed source/type or duplicate keys; every failure preserves caller output.
    const de__manifest_kv bad[] = {{"source", {.type = DE__MANIFEST_STRING, .u.s = "other.png"}}};
    memset(&result, 0xa5, sizeof result);
    de__cook_preview sentinel = result;
    EXPECT_EQ(de__asset_cook_preview(f.assets, "missing", NULL, 0, DE_COOK_PLATFORM_HOST, &result),
              DE_ERR_NOT_FOUND);
    EXPECT_MEMEQ(&result, &sentinel, sizeof result);
    EXPECT_EQ(de__asset_cook_preview(f.assets, "tex", bad, 1, DE_COOK_PLATFORM_HOST, &result),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_MEMEQ(&result, &sentinel, sizeof result);
    const de__manifest_kv duplicate[] = {settings[0], settings[0]};
    EXPECT_EQ(de__asset_cook_preview(f.assets, "tex", duplicate, 2, DE_COOK_PLATFORM_HOST, &result),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_MEMEQ(&result, &sentinel, sizeof result);
    teardown(&f);
}
TEST(cook_preview, actual_model_lods_subassets_and_accounted_bytes) {
    fixture f;
    setup(&f, "model",
          "[project]\nasset_root='.'\n[assets.model]\ntype='model'\nsource='inspection.gltf'"
          "\ngenerate_lods=true\nlod_error=0.1\n");
    copy(&f, "examples/model_inspection/inspection.gltf", "inspection.gltf");
    const de__manifest_kv settings[] = {
        {"generate_lods", {.type = DE__MANIFEST_BOOL, .u.b = false}}};
    de__cook_preview result = {0};
    ASSERT_EQ(
        de__asset_cook_preview(f.assets, "model", settings, 1, DE_COOK_PLATFORM_HOST, &result),
        DE_OK);
    u64 accounted[2] = {result.before.size, result.after.size};
    const de__cook_preview_side *sides[] = {&result.before, &result.after};
    u32 lods[2] = {0};
    for (u32 side = 0; side < 2; ++side) {
        for (u32 i = 0; i < sides[side]->child_count; ++i) {
            de__cook_preview_child child;
            ASSERT_TRUE(de__asset_cook_preview_child(sides[side], i, &child));
            accounted[side] += child.size;
            if (!strcmp(child.name, "Body") && !strcmp(child.type, "mesh")) {
                de_cooked_mesh_view mesh;
                const char *why = NULL;
                ASSERT_TRUE(de_cooked_mesh_parse(child.data, child.size, &mesh, &why));
                lods[side] = mesh.lod_count;
            }
        }
        EXPECT_EQ(accounted[side], sides[side]->total_bytes);
    }
    EXPECT_TRUE(lods[0] > 1);
    EXPECT_EQ(lods[1], 1u);
    EXPECT_TRUE(result.after.total_bytes < result.before.total_bytes);
    de__cook_preview_child child = {0};
    EXPECT_FALSE(de__asset_cook_preview_child(&result.after, result.after.child_count, &child));
    de__asset_cook_preview_free(&result);
    teardown(&f);
}
TEST(cook_preview, actual_audio_codec_sizes_and_locale_override) {
    fixture f;
    setup(&f, "sound",
          "[project]\nasset_root='.'\n[defaults.sound]\nstream=false\ncompression='pcm'\n[assets."
          "sound]\ntype='sound'\nsource='tone.wav'\n");
    u8 wav[44 + 4096 * 2] = {0};
    memcpy(wav, "RIFF", 4);
    de__snd_w32(wav + 4, sizeof wav - 8);
    memcpy(wav + 8, "WAVEfmt ", 8);
    de__snd_w32(wav + 16, 16);
    wav[20] = 1;
    wav[22] = 1;
    de__snd_w32(wav + 24, 48000);
    de__snd_w32(wav + 28, 96000);
    wav[32] = 2;
    wav[34] = 16;
    memcpy(wav + 36, "data", 4);
    de__snd_w32(wav + 40, sizeof wav - 44);
    char path[160];
    snprintf(path, sizeof path, "%s/tone.wav", f.root);
    ASSERT_EQ(de_file_write_all_atomic(path, wav, sizeof wav), DE_OK);
    const de__manifest_kv settings[] = {
        {"compression", {.type = DE__MANIFEST_STRING, .u.s = "adpcm"}}};
    de__cook_preview result = {0};
    ASSERT_EQ(
        de__asset_cook_preview(f.assets, "sound", settings, 1, DE_COOK_PLATFORM_HOST, &result),
        DE_OK);
    de__sound_view before, after;
    ASSERT_EQ(de__sound_parse(result.before.data, result.before.size, &before), DE_OK);
    ASSERT_EQ(de__sound_parse(result.after.data, result.after.size, &after), DE_OK);
    EXPECT_EQ(before.codec, DE__DSND_PCM_F32);
    EXPECT_EQ(after.codec, DE__DSND_ADPCM);
    EXPECT_EQ(after.frames, 4096u);
    EXPECT_TRUE(result.after.total_bytes < result.before.total_bytes);
    de__asset_cook_preview_free(&result);
    // A proposed locale source is interpreted normally and fails if it does not exist.
    de_asset_set_locale(f.assets, "fr-CA");
    const char *keys[] = {"fr"};
    const de__manifest_value values[] = {{.type = DE__MANIFEST_STRING, .u.s = "missing.wav"}};
    const de__manifest_kv localized[] = {
        {"locale", {.type = DE__MANIFEST_TABLE, .u.table = {keys, values, 1}}}};
    EXPECT_EQ(
        de__asset_cook_preview(f.assets, "sound", localized, 1, DE_COOK_PLATFORM_HOST, &result),
        DE_ERR_NOT_FOUND);
    EXPECT_TRUE(result.private_ == NULL);
    /* Parent-tag fallback chooses only the proposed source. */
    de__snd_w32(wav + 4, 44 + 1024 * 2 - 8);
    de__snd_w32(wav + 40, 1024 * 2);
    snprintf(path, sizeof path, "%s/short.wav", f.root);
    ASSERT_EQ(de_file_write_all_atomic(path, wav, 44 + 1024 * 2), DE_OK);
    const de__manifest_value present[] = {{.type = DE__MANIFEST_STRING, .u.s = "short.wav"}};
    const de__manifest_kv source[] = {
        {"locale", {.type = DE__MANIFEST_TABLE, .u.table = {keys, present, 1}}}};
    ASSERT_EQ(de__asset_cook_preview(f.assets, "sound", source, 1, DE_COOK_PLATFORM_HOST, &result),
              DE_OK);
    ASSERT_EQ(de__sound_parse(result.before.data, result.before.size, &before), DE_OK);
    ASSERT_EQ(de__sound_parse(result.after.data, result.after.size, &after), DE_OK);
    EXPECT_EQ(before.frames, 4096u);
    EXPECT_EQ(after.frames, 1024u);
    EXPECT_STREQ(de__manifest_find_asset(f.assets->manifest, "sound")->source, "tone.wav");
    de__asset_cook_preview_free(&result);
    teardown(&f);
}
#else
TEST(cook_preview, cooker_free_failure_preserves_output) {
    de__cook_preview result, before;
    memset(&result, 0xa5, sizeof result);
    before = result;
    EXPECT_EQ(de__asset_cook_preview(NULL, "x", NULL, 0, DE_COOK_PLATFORM_HOST, &result),
              DE_ERR_UNSUPPORTED);
    EXPECT_MEMEQ(&result, &before, sizeof result);
}
#endif
