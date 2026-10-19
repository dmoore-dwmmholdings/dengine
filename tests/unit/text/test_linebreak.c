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
