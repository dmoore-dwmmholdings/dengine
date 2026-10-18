#include "runtime_test_util.h"
#include "text/shape/shaper_internal.h"
struct reference_glyph {
    u32 glyph, cluster;
    i32 ax, ay, dx, dy;
};
#include "../../data/fonts/complex/references.h"
static de_font load_complex(runtime_fx *f, const char *name) {
    char path[64];
    snprintf(path, sizeof path, "font/%s", name);
    de_asset_id id = de_asset_id_from(path);
    de_font font = de_asset_font(f->assets, id);
    de_load_handle h = de_asset_load(f->assets, id, 0);
    ASSERT_EQ(de_asset_wait(f->assets, h, 60ULL * 1000000000), (int)DE_LOAD_READY);
    de_asset_release(f->assets, h);
    return font;
}
static void reference(runtime_fx *f, de_font font, const char *text, const char *language,
                      de_text_dir dir, const struct reference_glyph *expected, u32 count) {
    de_text_style style = {.font = font, .size_px = 48, .language = language};
    de_shaped_run run;
    ASSERT_EQ(de_text__shape(f->t, (de_str){text, strlen(text)}, &style, dir, NULL, &run), DE_OK);
    ASSERT_EQ(run.count, count);
    de_text_font *raw = de_text__font_get(f->t, font);
    f32 scale = stbtt_ScaleForMappingEmToPixels(&raw->face, 48);
    for (u32 i = 0; i < count; ++i) {
        EXPECT_EQ(run.glyphs[i].glyph, expected[i].glyph);
        EXPECT_EQ(run.glyphs[i].cluster, expected[i].cluster);
        EXPECT_FLOAT_EQ(run.glyphs[i].x_advance, (f32)expected[i].ax * scale, .001);
        EXPECT_FLOAT_EQ(run.glyphs[i].y_advance, (f32)expected[i].ay * scale, .001);
        EXPECT_FLOAT_EQ(run.glyphs[i].x_offset, (f32)expected[i].dx * scale, .001);
        EXPECT_FLOAT_EQ(run.glyphs[i].y_offset, (f32)expected[i].dy * scale, .001);
    }
    de_text__shape_free(&run);
}
TEST_F(shaper, ligatures_clusters_and_spacing, runtime_fx) {
    reference(f, f->latin, "office", "en", DE_DIR_LTR, office_reference, 4);
    de_shaped_run run;
    de_text_style style = {.font = f->latin, .size_px = 48, .letter_spacing = 1};
    ASSERT_EQ(de_text__shape(f->t, DE_STR("office"), &style, DE_DIR_LTR, NULL, &run), DE_OK);
    EXPECT_EQ(run.count, 6u);
    de_text__shape_free(&run);
}
TEST_F(shaper, kerning_reference, runtime_fx) {
    reference(f, f->latin, "AV", "en", DE_DIR_LTR, kern_reference, 2);
    de_shaped_run a, b;
    de_text_style s = {.font = f->latin, .size_px = 48};
    ASSERT_EQ(de_text__shape(f->t, DE_STR("AV"), &s, DE_DIR_LTR, NULL, &a), DE_OK);
    s.disable_features = DE_TEXT_DISABLE_KERN;
    ASSERT_EQ(de_text__shape(f->t, DE_STR("AV"), &s, DE_DIR_LTR, NULL, &b), DE_OK);
    EXPECT_TRUE(a.glyphs[0].x_advance < b.glyphs[0].x_advance);
    de_text__shape_free(&a);
    de_text__shape_free(&b);
}
TEST_F(shaper, complex_reference, runtime_fx) {
    reference(f, load_complex(f, "arabic"), "مرحبا", "ar", DE_DIR_RTL, arabic_reference, 6);
    reference(f, load_complex(f, "hebrew"), "שלום", "he", DE_DIR_RTL, hebrew_reference, 4);
    reference(f, load_complex(f, "devanagari"), "क्षि", "hi", DE_DIR_LTR, devanagari_reference, 2);
    reference(f, load_complex(f, "thai"), "กำลัง", "th", DE_DIR_LTR, thai_reference, 6);
}
