/* test_fpenv.c: de_fp_env_init_thread / de_fp_env_check (DENG-243, SPEC §5.7 rule 4). */
#include "de_test.h"
#include "fpenv_util.h"

#include <dengine/de_math.h>
#include <dengine/de_platform.h>

#include <float.h>
#include <string.h>

/* volatile: the products must happen at run time, in the current FP mode. */
static f32 mul32(f32 a, f32 b) {
    volatile f32 va = a, vb = b;
    volatile f32 r = va * vb;
    return r;
}
static f64 mul64(f64 a, f64 b) {
    volatile f64 va = a, vb = b;
    volatile f64 r = va * vb;
    return r;
}
static u32 bits32(f32 x) {
    u32 u;
    memcpy(&u, &x, sizeof u);
    return u;
}
static f32 from32(u32 u) {
    f32 x;
    memcpy(&x, &u, sizeof x);
    return x;
}

TEST(fpenv, ftz_flushes) {
    if (!DE_TEST_FPENV_KNOWN) {
        SKIP("no FP control register access on this target");
    }
    u64 saved = de_test_fpenv_get();

    /* Default mode: a product below FLT_MIN is a denormal, a denormal input is used. */
    f32 denorm = mul32(1e-30f, 1e-10f);
    EXPECT_NE(bits32(denorm), 0u);
    EXPECT_LT(bits32(denorm), 0x00800000u);
    EXPECT_NE(bits32(mul32(from32(0x00000100u), 2.0f)), 0u);
    EXPECT_FALSE(mul64(1e-300, 1e-20) == 0.0);

    de_fp_env_init_thread();
    /* Flush to zero: the denormal result becomes +-0 ... */
    EXPECT_EQ(bits32(mul32(1e-30f, 1e-10f)), 0u);
    EXPECT_EQ(bits32(mul32(-1e-30f, 1e-10f)), 0x80000000u);
    /* ... a denormal input reads as zero (DAZ on x86, FZ on arm64) ... */
    EXPECT_EQ(bits32(mul32(from32(0x00000100u), 2.0f)), 0u);
    /* ... and f64 too. */
    EXPECT_TRUE(mul64(1e-300, 1e-20) == 0.0);
    /* Normal arithmetic is untouched, round to nearest even: 1 + 2^-24 is a tie, goes to 1. */
    EXPECT_EQ(bits32(mul32(FLT_MIN, 1.0f)), 0x00800000u);
    volatile f32 one = 1.0f, half_ulp = 0x1p-24f;
    volatile f32 sum = one + half_ulp;
    EXPECT_EQ(bits32(sum), 0x3F800000u);

    de_test_fpenv_set(saved);
    EXPECT_NE(bits32(mul32(1e-30f, 1e-10f)), 0u);
}

static i32 thread_init_and_check(void *user) {
    bool *out = (bool *)user;
    out[0] = de_fp_env_check();
    de_fp_env_init_thread();
    out[1] = de_fp_env_check();
    return 0;
}

TEST(fpenv, check_reports) {
    if (!DE_TEST_FPENV_KNOWN) {
        SKIP("no FP control register access on this target");
    }
    u64 saved = de_test_fpenv_get();

    EXPECT_FALSE(de_fp_env_check()); /* the OS default has no FTZ */
    de_fp_env_init_thread();
    EXPECT_TRUE(de_fp_env_check());
    u64 after = de_test_fpenv_get();
    de_fp_env_init_thread(); /* idempotent */
    EXPECT_EQ(de_test_fpenv_get(), after);
    EXPECT_TRUE(de_fp_env_check());

    /* Another rounding mode is reported, and init puts round-to-nearest back. */
    de_test_fpenv_set(DE_TEST_FPENV_ROUND_DOWN(after));
    EXPECT_FALSE(de_fp_env_check());
    de_fp_env_init_thread();
    EXPECT_TRUE(de_fp_env_check());

    /* The mode is per thread: a new thread starts without it and sets its own. */
    de_test_fpenv_set(saved);
    EXPECT_FALSE(de_fp_env_check());
    bool seen[2] = {true, false};
    de_thread *t = NULL;
    ASSERT_EQ(de_thread_create(thread_init_and_check, seen, "fpenv", DE_THREAD_NORMAL, 0, &t),
              DE_OK);
    de_thread_join(t);
    EXPECT_FALSE(seen[0]);
    EXPECT_TRUE(seen[1]);
    EXPECT_FALSE(de_fp_env_check()); /* this thread is unchanged */

    de_test_fpenv_set(saved);
}
