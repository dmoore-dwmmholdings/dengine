/* Smoke test for de_tp_zstd: compress, decompress, compare. */
#include <stdlib.h>
#include <string.h>

#include <zstd.h>

#include "de_test.h"

enum { k_size = 64 * 1024 };

typedef struct zstd_fx {
    unsigned char *src, *comp, *back;
    size_t cap;
} zstd_fx;

static void zstd_fx_setup(zstd_fx *f) {
    f->cap = ZSTD_compressBound(k_size);
    f->src = malloc(k_size);
    f->comp = malloc(f->cap);
    f->back = malloc(k_size);
    ASSERT_TRUE(f->src && f->comp && f->back);
    /* Compressible but not trivial: a repeating pattern with a slow-changing byte. */
    for (size_t i = 0; i < k_size; ++i)
        f->src[i] = (unsigned char)((i % 251) ^ (i >> 10));
}

static void zstd_fx_teardown(zstd_fx *f) {
    free(f->src);
    free(f->comp);
    free(f->back);
}

TEST_F(de_tp_smoke_zstd, round_trip, zstd_fx) {
    const size_t csize = ZSTD_compress(f->comp, f->cap, f->src, k_size, 3);
    ASSERT_FALSE(ZSTD_isError(csize));
    EXPECT_LT(csize, (size_t)k_size);
    EXPECT_EQ(ZSTD_getFrameContentSize(f->comp, csize), (unsigned long long)k_size);
    const size_t dsize = ZSTD_decompress(f->back, k_size, f->comp, csize);
    ASSERT_FALSE(ZSTD_isError(dsize));
    ASSERT_EQ(dsize, (size_t)k_size);
    EXPECT_MEMEQ(f->src, f->back, k_size);
}

TEST_F(de_tp_smoke_zstd, corrupted_frame_is_rejected, zstd_fx) {
    const size_t csize = ZSTD_compress(f->comp, f->cap, f->src, k_size, 3);
    ASSERT_FALSE(ZSTD_isError(csize));
    f->comp[csize / 2] ^= 0xFF;
    f->comp[csize - 1] ^= 0xFF; /* the frame checksum */
    const size_t dsize = ZSTD_decompress(f->back, k_size, f->comp, csize);
    EXPECT_TRUE(ZSTD_isError(dsize) || memcmp(f->src, f->back, k_size) != 0);
}
