/* test_rng.c: de_rng PCG32 streams (DENG-243, SPEC §5.7 rule 6).
 *
 * rng.golden compares against tests/unit/math/vectors/rng_golden.txt, the cross-platform
 * contract. To rewrite that file (only for a deliberate change of the generator, with a reason
 * in the commit message), run the test with DE_REGEN_GOLDEN=1:
 *     DE_REGEN_GOLDEN=1 build/dev/tests/unit/math/test_rng --filter=rng.golden
 */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* getenv, fopen */
#endif
#include "de_test.h"

#include <dengine/de_math.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DE_RNG_GOLDEN
#error "DE_RNG_GOLDEN (path of rng_golden.txt) must be defined by the build"
#endif

TEST(rng, pcg_reference) {
    /* pcg32-demo of the PCG paper's reference C library: pcg32_srandom_r(&rng, 42u, 54u). */
    static const u32 expected[6] = {0xa15c02b7u, 0x7b47f409u, 0xba1d3330u,
                                    0x83d2f293u, 0xbfa4784bu, 0xcbed606eu};
    de_rng r = de_rng_seed(42u, 54u);
    for (int i = 0; i < 6; i++) {
        EXPECT_EQ(de_rng_u32(&r), expected[i]);
    }
}

TEST(rng, f32_f64_ranges) {
    de_rng r = de_rng_seed(1u, 2u);
    for (int i = 0; i < 100000; i++) {
        f32 f = de_rng_f32(&r);
        ASSERT_TRUE(f >= 0.0f && f < 1.0f);
        ASSERT_TRUE(f * 16777216.0f == floorf(f * 16777216.0f)); /* a multiple of 2^-24 */
        f64 d = de_rng_f64(&r);
        ASSERT_TRUE(d >= 0.0 && d < 1.0);
    }
    /* f32 is the top 24 bits of the same output de_rng_u32 would give. */
    de_rng a = de_rng_seed(7u, 9u), b = a;
    for (int i = 0; i < 64; i++) {
        EXPECT_TRUE(de_rng_f32(&a) == (f32)(de_rng_u32(&b) >> 8) * 0x1p-24f);
    }
}

/* Chi-squared statistic of `n` draws of de_rng_range(lo, lo + k) into k buckets. */
static f64 chi2_range(de_rng *r, u32 lo, u32 k, u32 n) {
    static u32 counts[1024];
    memset(counts, 0, sizeof counts);
    for (u32 i = 0; i < n; i++) {
        u32 v = de_rng_range(r, lo, lo + k);
        ASSERT_TRUE(v >= lo && v < lo + k);
        counts[v - lo]++;
    }
    f64 e = (f64)n / k, chi2 = 0.0;
    for (u32 i = 0; i < k; i++) {
        f64 d = counts[i] - e;
        chi2 += d * d / e;
    }
    return chi2;
}

TEST(rng, range_unbiased_chi2) {
    de_rng r = de_rng_seed(0x853c49e6748fea9bull, 0xda3e39cb94b95bdbull);
    /* Upper 0.1% points of chi2 with k - 1 degrees of freedom (k = 2, 3, 7, 10, 100, 1000):
     * a fair generator fails each check with probability 0.001, and the seed is fixed. */
    static const struct {
        u32 k;
        f64 limit;
    } cases[] = {{2, 10.83}, {3, 13.82}, {7, 22.46}, {10, 27.88}, {100, 148.2}, {1000, 1143.9}};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        f64 c = chi2_range(&r, 1000u, cases[i].k, 500000u);
        printf("  range k=%-5u chi2=%9.2f (limit %.2f)\n", cases[i].k, c, cases[i].limit);
        EXPECT_TRUE(c < cases[i].limit);
    }

    /* A range of 2/3 * 2^32: plain `u32 % range` gives the low half of the values twice as
     * often as the rest, so the low half would get ~2/3 of the draws instead of 1/2. */
    u32 range = 0xAAAAAAAAu, half = range / 2, n = 400000, low = 0;
    for (u32 i = 0; i < n; i++) {
        u32 v = de_rng_range(&r, 0, range);
        ASSERT_LT(v, range);
        low += v < half;
    }
    f64 frac = (f64)low / n; /* sigma = 0.5 / sqrt(n) = 0.00079 */
    printf("  range 0xAAAAAAAA: low half %.5f\n", frac);
    EXPECT_FLOAT_EQ(frac, 0.5, 0.004);

    /* Full and degenerate ranges. */
    EXPECT_EQ(de_rng_range(&r, 5u, 6u), 5u);
    de_rng before = r;
    EXPECT_EQ(de_rng_range(&r, 9u, 9u), 9u);
    EXPECT_EQ(de_rng_range(&r, 9u, 3u), 9u);
    EXPECT_EQ(r.state, before.state); /* an empty range does not advance */
    u32 top = 0;
    for (int i = 0; i < 1000; i++) {
        u32 v = de_rng_range(&r, 0u, 0xFFFFFFFFu);
        top = v > top ? v : top;
    }
    EXPECT_GT(top, 0xF0000000u);
}

TEST(rng, streams_independent) {
    enum { N = 200000, S = 6 };
    static f64 x[S][N];
    for (int s = 0; s < S; s++) {
        de_rng r = de_rng_seed(12345u, (u64)s);
        for (int i = 0; i < N; i++) {
            x[s][i] = de_rng_f64(&r) - 0.5;
        }
    }
    /* Same seed, different streams: no shared outputs and no correlation (|r| < 5 sigma). */
    f64 limit = 5.0 / sqrt((f64)N);
    for (int a = 0; a < S; a++) {
        for (int b = a + 1; b < S; b++) {
            f64 sab = 0.0, saa = 0.0, sbb = 0.0;
            int same = 0;
            for (int i = 0; i < N; i++) {
                sab += x[a][i] * x[b][i];
                saa += x[a][i] * x[a][i];
                sbb += x[b][i] * x[b][i];
                same += x[a][i] == x[b][i];
            }
            f64 corr = sab / sqrt(saa * sbb);
            EXPECT_TRUE(fabs(corr) < limit);
            EXPECT_LT(same, 3);
            /* Also lagged by one, which catches a stream that is the other one shifted. */
            f64 lag = 0.0;
            for (int i = 0; i + 1 < N; i++) {
                lag += x[a][i] * x[b][i + 1];
            }
            EXPECT_TRUE(fabs(lag / sqrt(saa * sbb)) < limit);
        }
    }

    /* The stream is what makes them differ: same seed and stream are identical. */
    de_rng p = de_rng_seed(99u, 3u), q = de_rng_seed(99u, 3u), o = de_rng_seed(99u, 4u);
    int diff = 0;
    for (int i = 0; i < 100; i++) {
        u32 pv = de_rng_u32(&p);
        EXPECT_EQ(pv, de_rng_u32(&q));
        diff += pv != de_rng_u32(&o);
    }
    EXPECT_GT(diff, 95);
}

TEST(rng, advance) {
    de_rng a = de_rng_seed(42u, 54u);
    de_rng b = a;
    for (int i = 0; i < 1000; i++) {
        de_rng_u32(&a);
    }
    de_rng_advance(&b, 1000u);
    EXPECT_EQ(a.state, b.state);
    EXPECT_EQ(de_rng_u32(&a), de_rng_u32(&b));

    /* (u64)-k goes back k outputs: the period is 2^64. */
    de_rng c = de_rng_seed(42u, 54u);
    de_rng start = c;
    de_rng_advance(&c, 12345u);
    de_rng_advance(&c, (u64)0 - 12345u);
    EXPECT_EQ(c.state, start.state);
    de_rng_advance(&c, 0u);
    EXPECT_EQ(c.state, start.state);
    de_rng_advance(&c, 5u);
    EXPECT_EQ(de_rng_u32(&c), 0xcbed606eu); /* the 6th reference output */
}

/* ---- Golden file ---- */

/* Each line: "<kind> <seed> <stream> <values...>", all hex. The values are what the stream gives
 * from its seed: 32 u32 outputs, 16 f32 (bits), 16 f64 (bits), 16 range(10, 10 + k) for the
 * k in the line, or 8 u32 outputs after advancing by the delta in the line. */
#define RNG_GOLDEN_SEEDS 8
static const u64 golden_seeds[RNG_GOLDEN_SEEDS][2] = {
    {42u, 54u},
    {0u, 0u},
    {1u, 1u},
    {0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull},
    {0x853c49e6748fea9bull, 0xda3e39cb94b95bdbull},
    {123456789u, 987654321u},
    {0xDEADBEEFull, 7u},
    {1ull << 63, 1ull << 62},
};
static const u32 golden_ranges[3] = {6u, 1000u, 0xAAAAAAAAu};
static const u64 golden_deltas[3] = {1u, 1000000007u, 0xFFFFFFFFFFFFFFF0ull};

static size_t rng_golden_text(char *out, size_t cap) {
    size_t n = 0;
#define EMIT(...)                                                                                  \
    do {                                                                                           \
        int w_ = snprintf(out + n, cap - n, __VA_ARGS__);                                          \
        ASSERT_TRUE(w_ >= 0 && (size_t)w_ < cap - n);                                              \
        n += (size_t)w_;                                                                           \
    } while (0)
    EMIT("# rng_golden.txt: de_rng (PCG32) reference outputs (DENG-243). Generated by test_rng\n"
         "# with DE_REGEN_GOLDEN=1; never edit by hand. Every platform must match it exactly.\n");
    for (int s = 0; s < RNG_GOLDEN_SEEDS; s++) {
        u64 seed = golden_seeds[s][0], stream = golden_seeds[s][1];
        de_rng r = de_rng_seed(seed, stream);
        EMIT("u32 %llx %llx", (unsigned long long)seed, (unsigned long long)stream);
        for (int i = 0; i < 32; i++) {
            EMIT(" %08x", de_rng_u32(&r));
        }
        EMIT("\nf32 %llx %llx", (unsigned long long)seed, (unsigned long long)stream);
        r = de_rng_seed(seed, stream);
        for (int i = 0; i < 16; i++) {
            f32 f = de_rng_f32(&r);
            u32 u;
            memcpy(&u, &f, sizeof u);
            EMIT(" %08x", u);
        }
        EMIT("\nf64 %llx %llx", (unsigned long long)seed, (unsigned long long)stream);
        r = de_rng_seed(seed, stream);
        for (int i = 0; i < 16; i++) {
            f64 d = de_rng_f64(&r);
            u64 u;
            memcpy(&u, &d, sizeof u);
            EMIT(" %016llx", (unsigned long long)u);
        }
        EMIT("\n");
        for (int k = 0; k < 3; k++) {
            r = de_rng_seed(seed, stream);
            EMIT("range %llx %llx %x", (unsigned long long)seed, (unsigned long long)stream,
                 golden_ranges[k]);
            for (int i = 0; i < 16; i++) {
                EMIT(" %08x", de_rng_range(&r, 10u, 10u + golden_ranges[k]));
            }
            EMIT("\n");
        }
        for (int k = 0; k < 3; k++) {
            r = de_rng_seed(seed, stream);
            de_rng_advance(&r, golden_deltas[k]);
            EMIT("advance %llx %llx %llx", (unsigned long long)seed, (unsigned long long)stream,
                 (unsigned long long)golden_deltas[k]);
            for (int i = 0; i < 8; i++) {
                EMIT(" %08x", de_rng_u32(&r));
            }
            EMIT("\n");
        }
    }
#undef EMIT
    return n;
}

TEST(rng, golden) {
    static char now[64 * 1024];
    static char file[64 * 1024];
    size_t n = rng_golden_text(now, sizeof now);

    const char *regen = getenv("DE_REGEN_GOLDEN");
    if (regen && strcmp(regen, "1") == 0) {
        FILE *f = fopen(DE_RNG_GOLDEN, "wb");
        ASSERT_TRUE(f != NULL);
        ASSERT_EQ(fwrite(now, 1, n, f), n);
        fclose(f);
        SKIP("DE_REGEN_GOLDEN=1: rewrote " DE_RNG_GOLDEN);
    }

    FILE *f = fopen(DE_RNG_GOLDEN, "rb");
    ASSERT_TRUE(f != NULL);
    size_t m = fread(file, 1, sizeof file, f);
    fclose(f);
    /* Line by line, so a mismatch names the line. Ignore '\r' (a Windows checkout). */
    size_t i = 0, j = 0;
    int line = 1, bad = 0;
    while (i < n || j < m) {
        if (j < m && file[j] == '\r') {
            j++;
            continue;
        }
        if (i >= n || j >= m || now[i] != file[j]) {
            printf("rng_golden.txt differs at line %d\n", line);
            bad = 1;
            break;
        }
        line += now[i] == '\n';
        i++;
        j++;
    }
    EXPECT_EQ(bad, 0);
}
