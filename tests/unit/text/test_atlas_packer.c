#include "de_test.h"
#include "text/atlas_packer.h"
#include "runtime_test_util.h"
#include <math.h>
#include <dengine/de_memory.h>
#include <stdio.h>
TEST(atlas_packer, padding_no_overlap_and_determinism) {
    const de_allocator *a = de_heap_allocator();
    de_atlas_packer *p = DE_NEW(a, de_atlas_packer, DE_MEM_UI),
                    *q = DE_NEW(a, de_atlas_packer, DE_MEM_UI);
    ASSERT_TRUE(p && q);
    de_atlas_packer_init(p, 256, 256, 2);
    de_atlas_packer_init(q, 256, 256, 2);
    u32 xy[200][4], count = 0;
    for (u32 i = 0; i < 200; ++i) {
        u32 w = 8 + i % 17, h = 8 + i % 13, x, y, x2, y2;
        bool ok = de_atlas_packer_insert(p, w, h, &x, &y);
        EXPECT_EQ(ok, de_atlas_packer_insert(q, w, h, &x2, &y2));
        if (!ok)
            continue;
        EXPECT_EQ(x, x2);
        EXPECT_EQ(y, y2);
        EXPECT_GE(x, 2u);
        EXPECT_GE(y, 2u);
        EXPECT_LE(x + w + 2, 256u);
        EXPECT_LE(y + h + 2, 256u);
        for (u32 j = 0; j < count; ++j)
            EXPECT_TRUE(x + w + 4 <= xy[j][0] || xy[j][0] + xy[j][2] + 4 <= x ||
                        y + h + 4 <= xy[j][1] || xy[j][1] + xy[j][3] + 4 <= y);
        xy[count][0] = x;
        xy[count][1] = y;
        xy[count][2] = w;
        xy[count++][3] = h;
    }
    DE_FREE(a, p, sizeof *p);
    DE_FREE(a, q, sizeof *q);
}
TEST_F(atlas_packer, cjk_2000_fill, runtime_fx) {
    de_text_font *font = de_text__font_get(f->t, f->cjk);
    de_atlas_packer *p = DE_NEW(de_heap_allocator(), de_atlas_packer, DE_MEM_UI);
    ASSERT_TRUE(p);
    de_atlas_packer_init(p, 1024, 1024, 0);
    f32 scale = stbtt_ScaleForMappingEmToPixels(&font->face, 48);
    u64 used = 0, area = 0;
    u32 pages = 1;
    for (u32 cp = 0x4e00; cp < 0x4e00 + 2000; ++cp) {
        int gid = stbtt_FindGlyphIndex(&font->face, (int)cp), x0, y0, x1, y1;
        ASSERT_GT(gid, 0);
        ASSERT_TRUE(stbtt_GetGlyphBox(&font->face, gid, &x0, &y0, &x1, &y1));
        u32 w = (u32)(ceilf((f32)x1 * scale) - floorf((f32)x0 * scale)) + 12,
            h = (u32)(ceilf((f32)y1 * scale) - floorf((f32)y0 * scale)) + 12, x, y;
        if (!de_atlas_packer_insert(p, w, h, &x, &y)) {
            used += p->used;
            area += 1024 * 1024;
            ++pages;
            de_atlas_packer_init(p, 1024, 1024, 0);
            ASSERT_TRUE(de_atlas_packer_insert(p, w, h, &x, &y));
        }
    }
    used += p->used;
    area += 1024 * 1024;
    double fill = (double)used / (double)area;
    printf("Actual2000 CJK outline pack fill %.3f pages%u\n", fill, pages);
    EXPECT_TRUE(fill >= .8);
    DE_FREE(de_heap_allocator(), p, sizeof *p);
}
