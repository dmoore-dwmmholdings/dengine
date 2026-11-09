#include "de_test.h"
#include "asset/diagnostic_internal.h"
#include <string.h>

TEST(asset_diagnostic, compiler_locations_and_windows_drives) {
    char path[512];
    uint32_t line = 0;
    EXPECT_TRUE(de__asset_diagnostic_location("shaders/lit.slang(12,5): error 30015: bad", path,
                                              sizeof path, &line));
    EXPECT_STREQ(path, "shaders/lit.slang");
    EXPECT_EQ(line, 12u);
    EXPECT_TRUE(de__asset_diagnostic_location("C:\\Game\\shader.slang:42:3: fatal error: bad", path,
                                              sizeof path, &line));
    EXPECT_STREQ(path, "C:\\Game\\shader.slang");
    EXPECT_EQ(line, 42u);
    EXPECT_TRUE(de__asset_diagnostic_location(
        "compiler banner\n dev:/shaders/inc.slang(7): warning: bad", path, sizeof path, &line));
    EXPECT_STREQ(path, "dev:/shaders/inc.slang");
    EXPECT_EQ(line, 7u);
    EXPECT_TRUE(de__asset_diagnostic_location("asset \"shader:lit\": file2.slang:11: error: bad",
                                              path, sizeof path, &line));
    EXPECT_STREQ(path, "file2.slang");
    EXPECT_EQ(line, 11u);
}
TEST(asset_diagnostic, rejects_unknown_truncated_and_overflow_locations) {
    char path[8] = "keep";
    uint32_t line = 9;
    EXPECT_FALSE(de__asset_diagnostic_location("file(0): error: bad", path, sizeof path, &line));
    EXPECT_FALSE(
        de__asset_diagnostic_location("file(4294967296): error: bad", path, sizeof path, &line));
    EXPECT_FALSE(
        de__asset_diagnostic_location("file:4294967296:3: error: bad", path, sizeof path, &line));
    EXPECT_FALSE(de__asset_diagnostic_location("file(12", path, sizeof path, &line));
    EXPECT_FALSE(
        de__asset_diagnostic_location("long-name.slang(12): error: bad", path, sizeof path, &line));
    EXPECT_FALSE(de__asset_diagnostic_location("time(12): done", path, sizeof path, &line));
    EXPECT_STREQ(path, "keep");
    EXPECT_EQ(line, 9u);
}

TEST(asset_diagnostic, rich_slang_locations) {
    char path[512];
    uint32_t line = 0;
    /* Captured pinned Slang 2026.19 form, including a filename containing spaces. */
    EXPECT_TRUE(de__asset_diagnostic_location(
        "error[E30015]: undefined identifier\n --> project space/a.slang:3:18\n  |\n"
        "3 | void f() { missing(); }\n  |                  ^^^^^^^ undefined identifier\n",
        path, sizeof path, &line));
    EXPECT_STREQ(path, "project space/a.slang");
    EXPECT_EQ(line, 3u);
    EXPECT_TRUE(de__asset_diagnostic_location(
        "asset \"shader:main\": spirv (no defines): error[E30015]: undefined identifier\r\n"
        " --> C:\\project space\\imported.slang:7:2\r\n",
        path, sizeof path, &line));
    EXPECT_STREQ(path, "C:\\project space\\imported.slang");
    EXPECT_EQ(line, 7u);
    EXPECT_TRUE(de__asset_diagnostic_location(
        "warning[W1]: conversion\n --> dev:/include.slang:9:1\n", path, sizeof path, &line));
    EXPECT_STREQ(path, "dev:/include.slang");
    EXPECT_TRUE(de__asset_diagnostic_location(
        "spirv (permutation \"USE_TINT=1\"): error[E1]: bad\n --> import.slang:9:1\n", path,
        sizeof path, &line));
    EXPECT_STREQ(path, "import.slang");
    EXPECT_EQ(line, 9u);
}
TEST(asset_diagnostic, rich_diagnostics_do_not_borrow_note_locations) {
    char path[32] = "keep";
    uint32_t line = 9;
    const char *bad[] = {" --> orphan.slang:1:2\n",
                         "errorish: not a heading\n --> file.slang:1:2\n",
                         "error: missing position\nnote: imported here\n --> other.slang:2:3\n",
                         "error: bad\n --> file.slang:0:2\n",
                         "error: bad\n --> file.slang:4294967296:2\n",
                         "error: bad\n --> file.slang:1:4294967296\n",
                         "error: bad\n --> file.slang:1:",
                         "error: bad\n --> file.slang:1:2 junk\n",
                         "error: bad\n --> a-filename-too-long-for-the-output-buffer.slang:1:2\n"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        EXPECT_FALSE(de__asset_diagnostic_location(bad[i], path, sizeof path, &line));
        EXPECT_STREQ(path, "keep");
        EXPECT_EQ(line, 9u);
    }
    EXPECT_TRUE(de__asset_diagnostic_location(
        "error: no location\nnote: imported here\n --> note.slang:7:2\n"
        "error: actual failure\n --> actual.slang:4:2\n",
        path, sizeof path, &line));
    EXPECT_STREQ(path, "actual.slang");
    EXPECT_EQ(line, 4u);
}
