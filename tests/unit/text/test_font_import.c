#include "font_test_util.h"
#include <stb_truetype.h>
#include <math.h>
static f32 getf(const u8 *p) {
    u32 u = de__font_u32(p);
    f32 f;
    memcpy(&f, &u, 4);
    return f;
}
TEST(font, charset_expansion) {
    u8 bits[0x110000 / 8] = {0};
    ASSERT_EQ(de__font_charset("latin-ext", bits, sizeof bits), DE_OK);
    for (u32 i = 0x100; i <= 0x24f; ++i)
        ASSERT_TRUE(bits[i >> 3] & (1u << (i & 7)));
    EXPECT_EQ(de__font_charset("bad", bits, sizeof bits), DE_ERR_INVALID_ARGUMENT);
}
TEST(font, system_lifetime) {
    de_text_system *t = NULL;
    ASSERT_EQ(de_text_create(NULL, NULL, &t), DE_OK);
    ASSERT_TRUE(t != NULL);
    de_text_destroy(t);
}
TEST(font, latin_metrics_and_determinism) {
    de__manifest_kv s = {.key = "charset",
                         .value = {.type = DE__MANIFEST_STRING, .u.s = "latin-ext"}};
    de_cook_result r = {0}, r2 = {0};
    ASSERT_EQ(font_cook("NotoSans.ttf", &s, 1, &r), DE_OK);
    ASSERT_EQ(font_cook("NotoSans.ttf", &s, 1, &r2), DE_OK);
    ASSERT_EQ(r.size, r2.size);
    ASSERT_MEMEQ(r.data, r2.data, (size_t)r.size);
    const u8 *p = r.data;
    ASSERT_MEMEQ(p, "DFNT", 4);
    u32 ng = de__font_u32(p + 12), nc = de__font_u32(p + 16), ns = de__font_u32(p + 28);
    const u8 *cm = p + 64 + (u64)ng * 48;
    const u8 *source = cm + (u64)nc * 8;
    stbtt_fontinfo f;
    ASSERT_TRUE(stbtt_InitFont(&f, source, 0));
    for (u32 cp = 0x100; cp <= 0x24f; ++cp) {
        u32 g = (u32)stbtt_FindGlyphIndex(&f, (int)cp);
        if (!g)
            continue;
        bool found = false;
        for (u32 j = 0; j < nc; ++j)
            if (de__font_u32(cm + j * 8) == cp) {
                EXPECT_EQ(de__font_u32(cm + j * 8 + 4), g);
                found = true;
            }
        EXPECT_TRUE(found);
    }
    EXPECT_GT(ns, 0u);
    EXPECT_TRUE(getf(p + 40) > 0.5f);
    EXPECT_TRUE(getf(p + 44) < 0.0f);
    de_cook_result_free(&r);
    de_cook_result_free(&r2);
}
static f32 sample(const u8 *page, u32 pw, u32 x, u32 y, u32 w, u32 h, f32 u, f32 v) {
    f32 fx = u * (f32)w - .5f, fy = v * (f32)h - .5f;
    int ix = (int)floorf(fx), iy = (int)floorf(fy);
    f32 tx = fx - (f32)ix, ty = fy - (f32)iy, val[3] = {0};
    for (u32 k = 0; k < 3; ++k) {
        for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx) {
                int ax = ix + dx, ay = iy + dy;
                if (ax < 0)
                    ax = 0;
                if (ay < 0)
                    ay = 0;
                if (ax >= (int)w)
                    ax = (int)w - 1;
                if (ay >= (int)h)
                    ay = (int)h - 1;
                val[k] += (f32)page[4 * ((u64)(y + (u32)ay) * pw + x + (u32)ax) + k] *
                          (dx ? tx : 1 - tx) * (dy ? ty : 1 - ty);
            }
    }
    return fmaxf(fminf(val[0], val[1]), fminf(fmaxf(val[0], val[1]), val[2]));
}
TEST(font, msdf_a_iou_4em) {
    de__manifest_kv s[] = {
        {.key = "charset", .value = {.type = DE__MANIFEST_STRING, .u.s = "ascii"}},
        {.key = "chars", .value = {.type = DE__MANIFEST_STRING, .u.s = "a"}}};
    de_cook_result r = {0};
    ASSERT_EQ(font_cook("NotoSans.ttf", s, 2, &r), DE_OK);
    const u8 *p = r.data;
    u32 ng = de__font_u32(p + 12), nc = de__font_u32(p + 16), ns = de__font_u32(p + 28),
        pw = de__font_u32(p + 52), ph = de__font_u32(p + 56);
    const u8 *source = p + 64 + (u64)ng * 48 + (u64)nc * 8;
    stbtt_fontinfo f;
    ASSERT_TRUE(stbtt_InitFont(&f, source, 0));
    u32 gid = (u32)stbtt_FindGlyphIndex(&f, 'a');
    const u8 *g = NULL;
    for (u32 i = 0; i < ng; ++i)
        if (de__font_u32(p + 64 + i * 48) == gid)
            g = p + 64 + i * 48;
    ASSERT_TRUE(g != NULL);
    u32 page = de__font_u32(g + 4), x = de__font_u32(g + 8), y = de__font_u32(g + 12),
        w = de__font_u32(g + 16), h = de__font_u32(g + 20);
    const u8 *atlas = source + ns + (u64)page * pw * ph * 4;
    f32 scale = stbtt_ScaleForMappingEmToPixels(&f, 192);
    int rw, rh, rx, ry;
    u8 *ref = stbtt_GetGlyphBitmap(&f, scale, scale, (int)gid, &rw, &rh, &rx, &ry);
    ASSERT_TRUE(ref != NULL);
    u32 inter = 0, uni = 0;
    f32 left = getf(g + 24) * 192, top = getf(g + 36) * 192;
    for (int yy = 0; yy < rh; ++yy)
        for (int xx = 0; xx < rw; ++xx) {
            bool a = ref[yy * rw + xx] >= 128;
            f32 u = ((f32)(rx + xx) + .5f - left) / (getf(g + 32) * 192 - left),
                v = ((f32)(ry + yy) + .5f + top) / (top - getf(g + 28) * 192);
            bool b = sample(atlas, pw, x, y, w, h, u, v) >= 127.5f;
            if (a && b)
                ++inter;
            if (a || b)
                ++uni;
        }
    f32 iou = (f32)inter / (f32)uni;
    printf("MSDF a IoU %.6f\n", (double)iou);
    EXPECT_GE((u32)(iou * 100000), 97000u);
    stbtt_FreeBitmap(ref, NULL);
    de_cook_result_free(&r);
}

TEST(font, cff_outlines_and_ranges) {
    const char *const ranges[] = {"0x100-0x10f"};
    de__manifest_kv s[] = {
        {.key = "ranges", .value = {.type = DE__MANIFEST_STRING_ARRAY, .u.arr = {ranges, 1}}},
        {.key = "charset", .value = {.type = DE__MANIFEST_STRING, .u.s = "ascii"}}};
    de_cook_result r = {0};
    ASSERT_EQ(font_cook("SourceCodePro.otf", s, 2, &r), DE_OK);
    EXPECT_GT(r.size, 1000u);
    de_cook_result_free(&r);
}
