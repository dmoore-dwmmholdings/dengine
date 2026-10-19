#include "de_test.h"
#include "text/layout/layout_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void break_conformance(const char *filename, bool graphemes) {
    char path[1024];
    snprintf(path, sizeof path, "%s/auxiliary/%s", DE_UNICODE_DATA, filename);
    FILE *file = fopen(path, "rb");
    ASSERT_TRUE(file);
    char line[32768];
    u32 cases = 0, failures = 0;
    while (fgets(line, sizeof line, file)) {
        char *at = line;
        while (*at == ' ')
            ++at;
        if (*at == '#' || *at == '\n' || *at == '\r' || !*at)
            continue;
        u32 cp[4096], count = 0;
        u8 expected[4097], actual[4097];
        while (*at && *at != '#') {
            while (*at == ' ' || *at == '\t')
                ++at;
            if ((u8)at[0] == 0xc3 && ((u8)at[1] == 0xb7 || (u8)at[1] == 0x97)) {
                expected[count] = (u8)at[1] == 0xb7;
                at += 2;
            } else {
                char *end;
                unsigned long value = strtoul(at, &end, 16);
                if (end == at)
                    break;
                ASSERT_LT(count, 4096u);
                cp[count++] = (u32)value;
                at = end;
            }
        }
        if (!count)
            continue;
        if (graphemes)
            de__text_graphemes(cp, count, actual);
        else
            ASSERT_EQ(de__text_linebreak(cp, count, actual, de_heap_allocator()), DE_OK);
        bool okay = true;
        for (u32 i = 0; i <= count; ++i)
            if ((actual[i] != 0) != (expected[i] != 0)) {
                okay = false;
                if (failures < 8)
                    printf("%s case%u boundary%u: %04X|%04X expected%u actual%u\n", filename, cases,
                           i, i ? cp[i - 1] : 0, i < count ? cp[i] : 0, expected[i], actual[i]);
            }
        if (!okay)
            ++failures;
        ++cases;
    }
    fclose(file);
    printf("%s: %u cases, %u failures, zero skipped\n", filename, cases, failures);
    EXPECT_GT(cases, graphemes ? 1000u : 10000u);
    EXPECT_EQ(failures, 0u);
}
TEST(linebreak, unicode_15_1_conformance) {
    break_conformance("LineBreakTest.txt", false);
}
TEST(linebreak, unicode_15_1_grapheme_conformance) {
    break_conformance("GraphemeBreakTest.txt", true);
}
TEST(linebreak, cjk_and_emergency_cluster_boundaries) {
    const u32 cp[] = {0x4e16, 0x754c, 0x6f22};
    u8 b[4];
    ASSERT_EQ(de__text_linebreak(cp, 3, b, de_heap_allocator()), DE_OK);
    EXPECT_EQ(b[1], 1u);
    EXPECT_EQ(b[2], 1u);
    const u32 emoji[] = {0x1f469, 0x200d, 0x1f4bb, 'a', 0x301};
    u8 gb[6];
    de__text_graphemes(emoji, 5, gb);
    EXPECT_EQ(gb[1], 0u);
    EXPECT_EQ(gb[2], 0u);
    EXPECT_EQ(gb[3], 1u);
    EXPECT_EQ(gb[4], 0u);
}
TEST(text_graphemes, public_byte_offsets_capacity_invalid_and_large_input) {
    const char *text = "a\xcc\x81"
                       "👩‍👩‍👧‍👦"
                       "b";
    u32 offsets[8] = {0};
    EXPECT_EQ(de_text_grapheme_offsets(de_str_from_cstr(text), offsets, 8), 4u);
    EXPECT_EQ(offsets[0], 0u);
    EXPECT_EQ(offsets[1], 3u);
    EXPECT_EQ(offsets[2], 28u);
    EXPECT_EQ(offsets[3], 29u);
    u32 short_out[2] = {99, 99};
    EXPECT_EQ(de_text_grapheme_offsets(de_str_from_cstr(text), short_out, 2), 4u);
    EXPECT_EQ(short_out[1], 3u);
    EXPECT_EQ(de_text_grapheme_offsets((de_str){0}, NULL, 0), 1u);
    const char invalid[] = {'a', (char)0xff, (char)0xc0, 'b'};
    EXPECT_EQ(de_text_grapheme_offsets((de_str){invalid, 4}, offsets, 8), 5u);
    for (u32 i = 0; i < 5; ++i)
        EXPECT_EQ(offsets[i], i);
    char large[4096];
    memset(large, 'a', sizeof large);
    EXPECT_EQ(de_text_grapheme_offsets((de_str){large, sizeof large}, NULL, 0), 4097u);
    EXPECT_EQ(de_text_grapheme_offsets((de_str){text, (u64)INT32_MAX}, NULL, 0), 0u);
    EXPECT_EQ(de_error_last()->result, DE_ERR_UNSUPPORTED);
    u64 before = de_mem_stats_for_tag(DE_MEM_UI).alloc_count;
    for (u32 i = 0; i < 100; ++i)
        (void)de_text_grapheme_offsets(de_str_from_cstr(text), offsets, 8);
    EXPECT_EQ(de_mem_stats_for_tag(DE_MEM_UI).alloc_count, before);
}
