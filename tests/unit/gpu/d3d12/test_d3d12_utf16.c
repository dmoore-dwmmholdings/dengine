/* test_d3d12_utf16.c — the D3D12 export-name converter (DENG-752), on every build host.
 * CTest names: gpu_d3d12.d3d12.utf16_*
 *
 * Tests src/gpu/d3d12/d3d12_utf16.c directly, without a GPU device: long symbols, BMP and
 * surrogate-pair characters, malformed UTF-8, the NULL and empty entry rules, allocation failure
 * and leak-free cleanup (a tracking allocator).
 */
#include "de_test.h"

#include <dengine/de_memory.h>

#include "gpu/d3d12/d3d12_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct fx {
    de_tracking_allocator t;
    de_allocator a;
} fx;

static void fx_setup(fx *f) {
    f->a = de_tracking_allocator_init(&f->t, NULL, DE_MEM_RENDER);
}
static void fx_teardown(fx *f) {
    /* Every test must free what it got: no live block survives. */
    EXPECT_EQ(de_tracking_allocator_stats(&f->t).blocks_live, 0u);
}

static void free_name(fx *f, u16 *p, size_t units) {
    if (p)
        DE_FREE(&f->a, p, units * sizeof(u16));
}

TEST_F(d3d12, utf16_ascii_and_null_default, fx) {
    u16 *w = NULL;
    size_t n = 0;
    ASSERT_EQ(de__d3d12_utf8_to_utf16(&f->a, "RayGen", &w, &n), DE_OK);
    ASSERT_EQ(n, 7u);
    EXPECT_EQ(w[0], (u16)'R');
    EXPECT_EQ(w[5], (u16)'n');
    EXPECT_EQ(w[6], 0u);
    free_name(f, w, n);
    /* NULL means the default entry "main". */
    ASSERT_EQ(de__d3d12_rt_entry_to_utf16(&f->a, NULL, &w, &n), DE_OK);
    ASSERT_EQ(n, 5u);
    EXPECT_EQ(w[0], (u16)'m');
    EXPECT_EQ(w[3], (u16)'n');
    EXPECT_EQ(w[4], 0u);
    free_name(f, w, n);
}

TEST_F(d3d12, utf16_empty_string, fx) {
    u16 *w = (u16 *)(void *)f;
    size_t n = 99;
    /* The converter maps "" to the bare terminator... */
    ASSERT_EQ(de__d3d12_utf8_to_utf16(&f->a, "", &w, &n), DE_OK);
    EXPECT_EQ(n, 1u);
    EXPECT_EQ(w[0], 0u);
    free_name(f, w, n);
    /* ...and an RT entry of "" is refused: it is not "main" and not a name. */
    w = (u16 *)(void *)f;
    n = 99;
    EXPECT_EQ(de__d3d12_rt_entry_to_utf16(&f->a, "", &w, &n), DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(w == NULL);
    EXPECT_EQ(n, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->t).alloc_count, 1u); /* only the first call */
}

TEST_F(d3d12, utf16_long_symbols, fx) {
    /* Well past the old 127-unit buffer, and past 1000. */
    const size_t lens[] = {127, 128, 129, 1000, 70000};
    for (size_t k = 0; k < sizeof lens / sizeof *lens; ++k) {
        size_t len = lens[k];
        char *s = (char *)malloc(len + 1);
        ASSERT_TRUE(s != NULL);
        for (size_t i = 0; i < len; ++i)
            s[i] = (char)('a' + i % 26);
        s[len] = 0;
        u16 *w = NULL;
        size_t n = 0;
        de_result r = de__d3d12_rt_entry_to_utf16(&f->a, s, &w, &n);
        EXPECT_EQ(r, DE_OK);
        if (r == DE_OK) {
            EXPECT_EQ(n, len + 1);
            bool same = w[len] == 0;
            for (size_t i = 0; i < len; ++i)
                same = same && w[i] == (u16)s[i];
            EXPECT_TRUE(same);
        }
        free_name(f, w, n);
        free(s);
    }
}

TEST_F(d3d12, utf16_bmp_and_surrogate_pairs, fx) {
    /* U+00E9, U+20AC, U+FFFD, U+FFFF; U+10000, U+1F600, U+10FFFF. */
    const char *s = "\xC3\xA9\xE2\x82\xAC\xEF\xBF\xBD\xEF\xBF\xBF"
                    "\xF0\x90\x80\x80\xF0\x9F\x98\x80\xF4\x8F\xBF\xBF";
    const u16 want[] = {0x00E9, 0x20AC, 0xFFFD, 0xFFFF, 0xD800, 0xDC00,
                        0xD83D, 0xDE00, 0xDBFF, 0xDFFF, 0};
    u16 *w = NULL;
    size_t n = 0;
    ASSERT_EQ(de__d3d12_utf8_to_utf16(&f->a, s, &w, &n), DE_OK);
    ASSERT_EQ(n, sizeof want / sizeof *want);
    EXPECT_EQ(memcmp(w, want, sizeof want), 0);
    free_name(f, w, n);
    /* Mixed with ASCII on both sides. */
    ASSERT_EQ(de__d3d12_utf8_to_utf16(&f->a, "a\xF0\x9F\x98\x80z", &w, &n), DE_OK);
    ASSERT_EQ(n, 5u);
    EXPECT_EQ(w[0], (u16)'a');
    EXPECT_EQ(w[1], 0xD83Du);
    EXPECT_EQ(w[2], 0xDE00u);
    EXPECT_EQ(w[3], (u16)'z');
    free_name(f, w, n);
}

TEST_F(d3d12, utf16_malformed_utf8, fx) {
    const char *bad[] = {
        "\x80",                 /* stray continuation */
        "\xBF",                 /* stray continuation */
        "\xC0\x80",             /* overlong NUL */
        "\xC1\xBF",             /* overlong */
        "\xE0\x80\x80",         /* overlong 3-byte */
        "\xE0\x9F\xBF",         /* overlong 3-byte */
        "\xF0\x80\x80\x80",     /* overlong 4-byte */
        "\xF0\x8F\xBF\xBF",     /* overlong 4-byte */
        "\xED\xA0\x80",         /* U+D800, an encoded surrogate */
        "\xED\xBF\xBF",         /* U+DFFF */
        "\xF4\x90\x80\x80",     /* U+110000 */
        "\xF5\x80\x80\x80",     /* lead byte above F4 */
        "\xFE", "\xFF",         /* never valid */
        "\xC3",                 /* truncated */
        "\xE2\x82",             /* truncated */
        "\xF0\x9F\x98",         /* truncated */
        "\xC3\x28",             /* bad continuation */
        "\xE2\x28\xA1",         /* bad continuation */
        "\xF0\x9F\x28\x80",     /* bad continuation */
        "ok\xFF\x6F\x6B",       /* bad byte in the middle */
        "ok\xE2\x82",           /* truncated at the end */
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i) {
        u16 *w = (u16 *)(void *)f;
        size_t n = 99;
        EXPECT_EQ(de__d3d12_utf8_to_utf16(&f->a, bad[i], &w, &n), DE_ERR_INVALID_ARGUMENT);
        EXPECT_TRUE(w == NULL);
        EXPECT_EQ(n, 0u);
        w = NULL;
        EXPECT_EQ(de__d3d12_rt_entry_to_utf16(&f->a, bad[i], &w, &n), DE_ERR_INVALID_ARGUMENT);
    }
    /* Nothing was allocated for any of them: no replacement characters, no partial buffers. */
    EXPECT_EQ(de_tracking_allocator_stats(&f->t).alloc_count, 0u);
    u16 *w = NULL;
    size_t n = 0;
    EXPECT_EQ(de__d3d12_utf8_to_utf16(&f->a, NULL, &w, &n), DE_ERR_INVALID_ARGUMENT);
}

TEST_F(d3d12, utf16_allocation_failure, fx) {
    u16 *w = (u16 *)(void *)f;
    size_t n = 99;
    de_tracking_allocator_fail_after(&f->t, 0);
    EXPECT_EQ(de__d3d12_utf8_to_utf16(&f->a, "RayGen", &w, &n), DE_ERR_OUT_OF_MEMORY);
    EXPECT_TRUE(w == NULL);
    EXPECT_EQ(n, 0u);
    w = (u16 *)(void *)f;
    EXPECT_EQ(de__d3d12_rt_entry_to_utf16(&f->a, NULL, &w, &n), DE_ERR_OUT_OF_MEMORY);
    EXPECT_TRUE(w == NULL);
    EXPECT_EQ(de_tracking_allocator_stats(&f->t).blocks_live, 0u);
    /* Disarmed, the same call works and frees cleanly. */
    de_tracking_allocator_fail_after(&f->t, -1);
    ASSERT_EQ(de__d3d12_utf8_to_utf16(&f->a, "RayGen", &w, &n), DE_OK);
    EXPECT_EQ(de_tracking_allocator_stats(&f->t).blocks_live, 1u);
    free_name(f, w, n);
    EXPECT_EQ(de_tracking_allocator_stats(&f->t).bytes_live, 0u);
}
