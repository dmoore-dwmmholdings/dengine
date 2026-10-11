/* Unit tests: de_result names, version packing, DE_VERIFY / DE_ASSERT evaluation (DENG-146). */
#include "de_test.h"

#include <dengine/de_base.h>

#include <string.h>

TEST(base, result_str_all_values) {
    for (int i = 0; i < (int)DE_RESULT_COUNT_; ++i) {
        const char *s = de_result_str((de_result)i);
        ASSERT_TRUE(s != NULL);
        EXPECT_TRUE(strncmp(s, "DE_", 3) == 0);
        EXPECT_TRUE(strcmp(s, "DE_ERR_UNKNOWN") != 0);
        /* Names are distinct. */
        for (int j = 0; j < i; ++j)
            EXPECT_TRUE(strcmp(s, de_result_str((de_result)j)) != 0);
    }
    EXPECT_STREQ(de_result_str(DE_OK), "DE_OK");
    EXPECT_STREQ(de_result_str(DE_ERR_IO), "DE_ERR_IO");
    EXPECT_STREQ(de_result_str(DE_ERR_INTERNAL), "DE_ERR_INTERNAL");
    /* Out of range, both directions. */
    EXPECT_STREQ(de_result_str(DE_RESULT_COUNT_), "DE_ERR_UNKNOWN");
    EXPECT_STREQ(de_result_str((de_result)-1), "DE_ERR_UNKNOWN");
    EXPECT_STREQ(de_result_str((de_result)1000), "DE_ERR_UNKNOWN");
}

TEST(base, version_packing) {
    u32 v = de_version();
    EXPECT_EQ(v >> 22, (u32)DE_VERSION_MAJOR);
    EXPECT_EQ((v >> 12) & 0x3FFu, (u32)DE_VERSION_MINOR);
    EXPECT_EQ(v & 0xFFFu, (u32)DE_VERSION_PATCH);
    EXPECT_EQ(v, DE_MAKE_VERSION(DE_VERSION_MAJOR, DE_VERSION_MINOR, DE_VERSION_PATCH));
    EXPECT_EQ(DE_MAKE_VERSION(1, 2, 3), (1u << 22) | (2u << 12) | 3u);
    EXPECT_EQ(DE_MAKE_VERSION(1023, 1023, 4095), 0xFFFFFFFFu);
    /* Later versions compare greater. */
    EXPECT_LT(DE_MAKE_VERSION(0, 9, 4095), DE_MAKE_VERSION(0, 10, 0));
    EXPECT_LT(DE_MAKE_VERSION(0, 1023, 4095), DE_MAKE_VERSION(1, 0, 0));
}

TEST(base, verify_evaluates_once) {
    int n = 0;
    DE_VERIFY(++n == 1);
    EXPECT_EQ(n, 1);
    DE_VERIFY(++n > 0);
    EXPECT_EQ(n, 2);
}

TEST(base, assert_evaluates_only_in_debug_and_dev) {
    int n = 0;
    DE_ASSERT(++n == 1);
#if defined(DE_DEBUG) || defined(DE_DEV)
    EXPECT_EQ(n, 1);
#else
    EXPECT_EQ(n, 0);
#endif
}
