#include "de_test.h"
#include "text/layout/layout_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
TEST(bidi, unicode_15_1_character_conformance) {
    FILE *file = fopen(DE_UNICODE_DATA "/BidiCharacterTest.txt", "rb");
    ASSERT_TRUE(file);
    char line[32768];
    u32 cases = 0, failures = 0;
    while (fgets(line, sizeof line, file)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;
        char *fields[5], *at = line;
        for (u32 i = 0; i < 5; ++i) {
            fields[i] = at;
            char *end = strchr(at, ';');
            if (end) {
                *end = 0;
                at = end + 1;
            } else if (i != 4) {
                ASSERT_TRUE(false);
            }
        }
        u32 cp[4096], count = 0;
        at = fields[0];
        while (*at) {
            char *end;
            unsigned long value = strtoul(at, &end, 16);
            if (end == at)
                break;
            ASSERT_LT(count, 4096u);
            cp[count++] = (u32)value;
            at = end;
        }
        if (!count)
            continue;
        u32 direction = (u32)strtoul(fields[1], NULL, 10),
            expected_base = (u32)strtoul(fields[2], NULL, 10);
        i32 levels[4096];
        u32 level_count = 0;
        at = fields[3];
        while (*at) {
            while (*at == ' ')
                ++at;
            if (!*at)
                break;
            if (*at == 'x') {
                levels[level_count++] = -1;
                ++at;
            } else {
                char *end;
                levels[level_count++] = (i32)strtol(at, &end, 10);
                if (end == at)
                    break;
                at = end;
            }
        }
        ASSERT_EQ(level_count, count);
        u32 expected[4096], expected_count = 0;
        at = fields[4];
        while (*at && *at != '#') {
            char *end;
            unsigned long value = strtoul(at, &end, 10);
            if (end == at)
                break;
            expected[expected_count++] = (u32)value;
            at = end;
        }
        de__bidi bidi;
        ASSERT_EQ(de__text_bidi(cp, count,
                                direction == 2 ? DE_DIR_AUTO
                                : direction    ? DE_DIR_RTL
                                               : DE_DIR_LTR,
                                &bidi),
                  DE_OK);
        bool okay = SBParagraphGetBaseLevel(bidi.paragraph) == expected_base;
        SBLineRef visual = SBParagraphCreateLine(bidi.paragraph, 0, count);
        ASSERT_TRUE(visual);
        const SBRun *runs = SBLineGetRunsPtr(visual);
        u32 actual[4096], actual_count = 0;
        for (u32 i = 0; i < SBLineGetRunCount(visual); ++i) {
            const SBRun *r = &runs[i];
            /* Conformance includes L1; paragraph levels intentionally stop at I2. */
            for (u32 j = 0; j < r->length; ++j)
                if (levels[r->offset + j] >= 0 && (u32)levels[r->offset + j] != r->level)
                    okay = false;
            for (u32 j = 0; j < r->length; ++j) {
                u32 index = (u32)(r->level & 1 ? r->offset + r->length - 1 - j : r->offset + j);
                if (levels[index] >= 0)
                    actual[actual_count++] = index;
            }
        }
        if (actual_count != expected_count ||
            memcmp(actual, expected, expected_count * sizeof *expected))
            okay = false;
        if (!okay && failures++ < 8)
            printf("BidiCharacterTest case%u cp0=%04X base%u expected%u\n", cases, cp[0],
                   SBParagraphGetBaseLevel(bidi.paragraph), expected_base);
        ++cases;
        SBLineRelease(visual);
        de__text_bidi_free(&bidi);
    }
    fclose(file);
    printf("BidiCharacterTest: %u cases, %u failures, zero skipped\n", cases, failures);
    EXPECT_GT(cases, 90000u);
    EXPECT_EQ(failures, 0u);
}
TEST(bidi, auto_direction_and_line_reordering) {
    const u32 cp[] = {'a', 'b', 'c', ' ', 0x5e9, 0x5dc, 0x5d5, 0x5dd, ' ', 'd', 'e', 'f'};
    de__bidi bidi;
    ASSERT_EQ(de__text_bidi(cp, 12u, DE_DIR_AUTO, &bidi), DE_OK);
    EXPECT_EQ(SBParagraphGetBaseLevel(bidi.paragraph), 0u);
    SBLineRef line = SBParagraphCreateLine(bidi.paragraph, 0, 12u);
    ASSERT_TRUE(line);
    const SBRun *runs = SBLineGetRunsPtr(line);
    EXPECT_EQ(SBLineGetRunCount(line), 3u);
    EXPECT_EQ(runs[1].offset, 4u);
    EXPECT_EQ(runs[1].length, 4u);
    EXPECT_EQ(runs[1].level, 1u);
    SBLineRelease(line);
    de__text_bidi_free(&bidi);
}
