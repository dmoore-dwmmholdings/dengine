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
