#include "de_test.h"
#include "anim/sprite_clip_format.h"
#include "cook/lib/cooker_internal.h"
#include "cook/importers/sprite_clip/sprite_clip_import.h"
#include <dengine/de_platform.h>
#include <stdlib.h>
#include <string.h>
typedef struct sprite_import_fx {
    de_tracking_allocator tracking;
    de_allocator allocator;
    de_cooker *cooker;
    de__cooker_type type;
    de_asset_import_ctx ctx;
} sprite_import_fx;
static void sprite_import_fx_setup(sprite_import_fx *f) {
    de_fp_env_init_thread();
    f->allocator = de_tracking_allocator_init(&f->tracking, NULL, DE_MEM_ASSET);
    ASSERT_EQ(de_cooker_create(&(de_cooker_desc){.allocator = &f->allocator,
                                                 .verify_determinism = DE_COOK_VERIFY_ON},
                               &f->cooker),
              DE_OK);
    memcpy(f->type.name, "sprite_clip", sizeof "sprite_clip");
    f->type.import = de_sprite_clip_import;
    de__import_ctx_init(&f->ctx, f->cooker, &f->type);
}
static void sprite_import_fx_teardown(sprite_import_fx *f) {
    if (f->ctx.c)
        de__import_ctx_release(&f->ctx);
    de_cooker_destroy(f->cooker);
    EXPECT_EQ(de_tracking_allocator_stats(&f->tracking).bytes_live, 0u);
}
static de_result parse_source(sprite_import_fx *f, const char *source) {
    de__import_ctx_release(&f->ctx);
    de__import_ctx_init(&f->ctx, f->cooker, &f->type);
    f->ctx.source = source;
    f->ctx.source_size = strlen(source);
    f->ctx.source_rel = "test.sprite.toml";
    f->ctx.id_str = "test/sprite";
    f->ctx.id = de_asset_id_from(f->ctx.id_str);
    return de__cooker_run_import(&f->ctx);
}
static const char grid_source[] = "atlas='texture/hero'\ndefault_ms=100\n"
                                  "grid={columns=8,rows=4,count=30,frame_size=[16,32]}\n"
                                  "[[tags]]\nname='run'\nfrom=8\nto=15\ndirection='forward'\n"
                                  "[[tags]]\nname='bounce'\nfrom=0\nto=3\ndirection='pingpong'\n"
                                  "[[events]]\nframe=15\nname='footstep'\n"
                                  "[[events]]\nframe=8\nname='start'\n";
TEST_F(sprite_import, grid_uv_tags_events_and_repeat_bytes, sprite_import_fx) {
    ASSERT_EQ(parse_source(f, grid_source), DE_OK);
    de__sprite_clip_view v;
    ASSERT_TRUE(de__sprite_clip_parse(f->ctx.out, f->ctx.out_size, &v, NULL));
    EXPECT_EQ(v.frame_count, 30u);
    EXPECT_EQ(v.width, 128u);
    EXPECT_EQ(v.height, 128u);
    EXPECT_EQ(v.total_ms, 3000u);
    EXPECT_EQ(f->ctx.dep_count, 1u);
    EXPECT_EQ(f->ctx.deps[0].hash, de_asset_id_from("texture/hero").hash);
    for (u32 i = 0; i < 30; ++i) {
        de__sprite_frame fr = de__sprite_frame_at(&v, i);
        EXPECT_FLOAT_EQ(fr.rect[0], (f32)(i % 8) / 8, 1e-6);
        EXPECT_FLOAT_EQ(fr.rect[1], (f32)(i / 8) / 4, 1e-6);
        EXPECT_FLOAT_EQ(fr.rect[2], .125f, 1e-6);
        EXPECT_FLOAT_EQ(fr.rect[3], .25f, 1e-6);
        EXPECT_EQ(fr.ms, 100u);
    }
    EXPECT_EQ(v.tag_count, 2u);
    EXPECT_EQ(v.event_count, 2u);
    EXPECT_EQ(de__sprite_event_at(&v, 0).frame, 8u);
    EXPECT_EQ(de__sprite_event_at(&v, 1).frame, 15u);
    u64 bytes = f->ctx.out_size;
    void *copy = malloc((size_t)bytes);
    ASSERT_TRUE(copy);
    memcpy(copy, f->ctx.out, (size_t)bytes);
    ASSERT_EQ(parse_source(f, grid_source), DE_OK);
    EXPECT_EQ(f->ctx.out_size, bytes);
    EXPECT_EQ(memcmp(copy, f->ctx.out, (size_t)bytes), 0);
    free(copy);
}
TEST_F(sprite_import, explicit_duration_pivot_and_atlas_extent, sprite_import_fx) {
    ASSERT_EQ(parse_source(f,
                           "atlas='atlas/test'\natlas_size=[64,32]\ndefault_ms=77\n"
                           "frames=[{rect=[16,8,16,8],ms=25,pivot=[0.5,0.0]},{rect=[0,0,8,8]}]\n"),
              DE_OK);
    de__sprite_clip_view v;
    ASSERT_TRUE(de__sprite_clip_parse(f->ctx.out, f->ctx.out_size, &v, NULL));
    de__sprite_frame fr = de__sprite_frame_at(&v, 0);
    EXPECT_FLOAT_EQ(fr.rect[0], .25f, 0);
    EXPECT_FLOAT_EQ(fr.rect[1], .25f, 0);
    EXPECT_FLOAT_EQ(fr.rect[2], .25f, 0);
    EXPECT_FLOAT_EQ(fr.rect[3], .25f, 0);
    EXPECT_FLOAT_EQ(fr.pivot[1], 0, 0);
    EXPECT_EQ(fr.ms, 25u);
    EXPECT_EQ(de__sprite_frame_at(&v, 1).ms, 77u);
    EXPECT_EQ(v.total_ms, 102u);
}
TEST_F(sprite_import, malformed_input_has_source_line_and_no_output, sprite_import_fx) {
    const char *bad[] = {
        "grid={columns=1,rows=1,frame_size=[8,8]}\n",
        "atlas='atlas/test'\ngrid={columns=1,rows=1,frame_size=[8,8]}\ndefault_ms=0\n",
        "atlas='atlas/"
        "test'\ngrid={columns=1,rows=1,frame_size=[8,8]}\n[[tags]]\nname='bad'\nfrom=0\nto=1\n",
        "atlas='atlas/test'\natlas_size=[8,8]\nframes=[{rect=[0,0,8,8],ms=0}]\n",
        "atlas='atlas/test'\natlas_size=[8,8]\nframes=[{rect=[0,0,9,8]}]\n",
        "atlas='atlas/test'\ngrid={columns=4294967295,rows=4294967295,frame_size=[8,8]}\n",
        "atlas='atlas/"
        "test'\ngrid={columns=1,rows=1,frame_size=[8,8]}\n[[tags]]\nname='same'\nfrom=0\nto=0\n[["
        "tags]]\nname='same'\nfrom=0\nto=0\n"};
    for (u32 i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        ASSERT_EQ(parse_source(f, bad[i]), DE_ERR_PARSE);
        EXPECT_TRUE(strstr(f->ctx.msg, "test.sprite.toml:") != NULL);
        if (i == 1)
            EXPECT_TRUE(strstr(f->ctx.msg, "test.sprite.toml:3:") != NULL);
        EXPECT_EQ(f->ctx.out_size, 0u);
    }
}
TEST_F(sprite_import, cooked_bounds_padding_nan_and_truncation_rejected, sprite_import_fx) {
    ASSERT_EQ(parse_source(f, grid_source), DE_OK);
    u64 size = f->ctx.out_size;
    u8 *p = malloc((size_t)size);
    ASSERT_TRUE(p);
    de__sprite_clip_view v;
    for (u64 n = 0; n < size; ++n)
        EXPECT_FALSE(de__sprite_clip_parse(f->ctx.out, n, &v, NULL));
    const u32 offsets[] = {4, 8, 12, 16, 20, 24, 28, 32, 44, 64 + 16, 64 + 28};
    for (u32 i = 0; i < sizeof offsets / sizeof offsets[0]; ++i) {
        memcpy(p, f->ctx.out, (size_t)size);
        de_cooked_put_u32(p + offsets[i], UINT32_MAX);
        EXPECT_FALSE(de__sprite_clip_parse(p, size, &v, NULL));
    }
    memcpy(p, f->ctx.out, (size_t)size);
    de_cooked_put_u32(p + 64, 0x7fc00000u);
    EXPECT_FALSE(de__sprite_clip_parse(p, size, &v, NULL));
    free(p);
}
TEST_F(sprite_import, builtin_cooker_reads_source_and_verifies_determinism, sprite_import_fx) {
    de_asset_type_desc descriptor;
    ASSERT_TRUE(de_cooker_builtin_importer("sprite_clip", &descriptor));
    EXPECT_TRUE(de_cooker_is_engine_type("sprite_clip"));
    ASSERT_EQ(de_cooker_register_type(f->cooker, &descriptor), DE_OK);
    ASSERT_EQ(de_fs_mount("spritecook", DE_ANIM_TEST_TMP, DE_MOUNT_WRITABLE, 0), DE_OK);
    ASSERT_EQ(de_file_write_all_atomic("spritecook:/registry.sprite.toml", grid_source,
                                       sizeof grid_source - 1),
              DE_OK);
    de__manifest_asset asset = {.id = "test/sprite",
                                .type = "sprite_clip",
                                .source = "registry.sprite.toml",
                                .file = "spritecook:/assets.toml"};
    de_cook_result result = {0};
    ASSERT_EQ(de_cooker_cook(f->cooker,
                             &(de_cook_input){.asset = &asset, .asset_root = "spritecook:/"},
                             &result),
              DE_OK);
    de__sprite_clip_view view;
    EXPECT_TRUE(de__sprite_clip_parse(result.data, result.size, &view, NULL));
    EXPECT_EQ(view.frame_count, 30u);
    EXPECT_EQ(result.dependency_count, 1u);
    de_cook_result_free(&result);
    ASSERT_EQ(de_file_delete("spritecook:/registry.sprite.toml"), DE_OK);
    ASSERT_EQ(de_fs_unmount("spritecook", DE_ANIM_TEST_TMP), DE_OK);
}
