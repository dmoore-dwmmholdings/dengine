#include "runtime_test_util.h"
TEST_F(fallback, latin_cjk_emoji_notdef, runtime_fx) {
    de_font out;
    u32 glyph;
    ASSERT_TRUE(de_text__resolve(f->t, f->latin, 'A', &out, &glyph));
    EXPECT_EQ(out.id, f->latin.id);
    ASSERT_TRUE(de_text__resolve(f->t, f->latin, 0x6f22, &out, &glyph));
    EXPECT_EQ(out.id, f->cjk.id);
    ASSERT_TRUE(de_text__resolve(f->t, f->latin, 0x1f600, &out, &glyph));
    EXPECT_EQ(out.id, f->emoji.id);
    EXPECT_FALSE(de_text__resolve(f->t, f->latin, 0x10fffd, &out, &glyph));
    EXPECT_EQ(out.id, f->latin.id);
    EXPECT_EQ(glyph, 0u);
}
TEST_F(fallback, cache_chain_change_and_budget, runtime_fx) {
    de_font out;
    u32 glyph;
    for (u32 i = 0; i < 200; ++i)
        ASSERT_TRUE(de_text__resolve(f->t, f->latin, 0x4e00 + i, &out, &glyph));
    u64 start = de_time_now_ns();
    for (u32 i = 0; i < 10000; ++i)
        ASSERT_TRUE(de_text__resolve(f->t, f->latin, 0x4e00 + i % 200, &out, &glyph));
    u64 elapsed = de_time_now_ns() - start;
    printf("10000 cached fallback lookups %.3f ms\n", (double)elapsed / 1e6);
#if defined(DE_DEV)
    EXPECT_LT(elapsed, 500000u); /* The acceptance budget is explicitly Dev, not Debug/ASan. */
#endif
    de_text_set_fallbacks(f->t, f->latin, NULL, 0);
    EXPECT_FALSE(de_text__resolve(f->t, f->latin, 0x6f22, &out, &glyph));
    de_font too_many[9] = {0};
    de_text_set_fallbacks(f->t, f->latin, too_many, 9);
    EXPECT_FALSE(de_text__resolve(f->t, f->latin, 0x6f22, &out, &glyph));
}
