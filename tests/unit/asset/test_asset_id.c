/*
 * test_asset_id.c: asset IDs (DENG-139, SPEC §11.1). CTest names asset_id.<test>.
 *
 * The vectors are XXH3-64 (seed 0) of the path bytes, computed with the reference xxHash 0.8
 * (XXH3_64bits) outside the engine. They are part of the pack format (S08): if one of them
 * changes, every cooked pack is invalid. Never update them to make a test pass.
 */
#include "de_test.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>

#include "asset/resolve_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

TEST(asset_id, vectors) {
    static const struct {
        const char *path;
        u64 hash;
    } k_vectors[] = {
        {"player/model#Body", 0xe7816acda331ee4dull},
        {"player/model", 0x9299b4e41871f33full},
        {"env/castle", 0x4cf33964be0a3ebeull},
        {"hero/sprites", 0xde14019fe1e6a351ull},
        {"shaders/hologram", 0xed77c999ab0e75d9ull},
        {"ui/font", 0x8308ae540cdf0f55ull},
        {"a", 0xe6c632b61e964e1full},
    };
    for (size_t i = 0; i < sizeof k_vectors / sizeof k_vectors[0]; ++i) {
        de_error_clear();
        de_asset_id id = de_asset_id_from(k_vectors[i].path);
        EXPECT_EQ(id.hash, k_vectors[i].hash);
        EXPECT_TRUE(de_error_last() == NULL);
        /* The same as hashing the bytes directly. */
        EXPECT_EQ(id.hash, de_hash64_str(k_vectors[i].path));
    }
    /* A sub-asset is a different ID from its asset; stable across calls. */
    EXPECT_NE(de_asset_id_from("player/model#Body").hash, de_asset_id_from("player/model").hash);
    EXPECT_EQ(de_asset_id_from("player/model#Body").hash,
              de_asset_id_from("player/model#Body").hash);
    /* Case-sensitive. */
    EXPECT_NE(de_asset_id_from("Player/Model").hash, de_asset_id_from("player/model").hash);
}

TEST(asset_id, invalid) {
    static const char *const k_bad[] = {
        "",                 /* empty */
        "/a",               /* leading / */
        "a/",               /* trailing / */
        "a//b",             /* empty segment */
        "a/../b",           /* .. segment */
        "a/./b",            /* . segment */
        "./a",              /* . segment */
        "..",               /* .. segment */
        "a#b#c",            /* two # */
        "a#",               /* empty sub-asset */
        "#b",               /* empty path */
        "/a#b",             /* leading / before # */
        "a/#b",             /* trailing / before # */
        "a\\b",             /* backslash */
        "ui/*",             /* glob characters */
        "ui/?",             /* glob characters */
        "a\tb",             /* control character */
        "a\x7f",            /* DEL */
        "caf\xc3",          /* truncated UTF-8 */
        "\xc0\xaf",         /* overlong '/' */
        "\xed\xa0\x80",     /* surrogate */
        "\xf4\x90\x80\x80", /* above U+10FFFF */
        "a#b\x01",          /* control character in the sub-asset */
    };
    for (size_t i = 0; i < sizeof k_bad / sizeof k_bad[0]; ++i) {
        de_error_clear();
        de_asset_id id = de_asset_id_from(k_bad[i]);
        if (id.hash != 0)
            fprintf(stderr, "accepted: \"%s\"\n", k_bad[i]);
        EXPECT_EQ(id.hash, 0u);
        const de_error *e = de_error_last();
        ASSERT_TRUE(e != NULL);
        EXPECT_EQ(e->result, DE_ERR_INVALID_ARGUMENT);
        EXPECT_TRUE(de__asset_id_check(k_bad[i], strlen(k_bad[i])) != NULL);
    }
    de_error_clear();
    EXPECT_EQ(de_asset_id_from(NULL).hash, 0u);
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    /* The reason names the rule. */
    EXPECT_STREQ(de__asset_id_check("a//b", 4), "empty segment ('//')");
    EXPECT_STREQ(de__asset_id_check("a#b#c", 5), "more than one '#'");
    /* A NUL inside the checked bytes is a control character. */
    EXPECT_TRUE(de__asset_id_check("a\0b", 3) != NULL);
}

TEST(asset_id, valid_forms) {
    static const char *const k_good[] = {
        "a",
        "player/model",
        "player/model#Body",
        "env/castle#Root/Tower 1", /* a sub-asset name may contain '/' and spaces */
        "a.b/c..d/.e",             /* dots inside a segment are fine */
        "ui/caf\xc3\xa9",          /* UTF-8 */
        "\xe6\x97\xa5\xe6\x9c\xac/\xf0\x9f\x8e\xae",
        "level_01/tiles-2",
    };
    for (size_t i = 0; i < sizeof k_good / sizeof k_good[0]; ++i) {
        const char *why = de__asset_id_check(k_good[i], strlen(k_good[i]));
        if (why != NULL)
            fprintf(stderr, "refused \"%s\": %s\n", k_good[i], why);
        EXPECT_TRUE(why == NULL);
        EXPECT_NE(de_asset_id_from(k_good[i]).hash, 0u);
    }
}

TEST(asset_id, zero_remap) {
    /* No known path hashes to 0, so the remap is checked on the helper every ID goes through
     * (de_asset_id_from and the resolver both build IDs with de__asset_id_make). */
    EXPECT_EQ(de__asset_id_make(0).hash, 1u);
    EXPECT_EQ(de__asset_id_make(1).hash, 1u);
    EXPECT_EQ(de__asset_id_make(0x9299b4e41871f33full).hash, 0x9299b4e41871f33full);
    EXPECT_EQ(de__asset_id_make(UINT64_MAX).hash, UINT64_MAX);
}
