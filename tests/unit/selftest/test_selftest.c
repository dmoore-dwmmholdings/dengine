/*
 * Self tests for de_test.h. Failure and skip behaviour is checked by running unregistered cases
 * (DE_TEST_CASE) through de_test_run_case() in quiet mode, so this executable passes.
 *
 * Built a second time with DE_TEST_SELFTEST_FAIL (target test_selftest_fail) to add tests that
 * really fail and skip; expect_failure.cmake checks their output and exit codes.
 */
#include "de_test.h"

#include <stdint.h>

/* ---- Passing checks ----------------------------------------------------------------------- */

typedef enum color { COLOR_RED, COLOR_GREEN } color;

TEST(selftest, asserts_pass) {
    int i = -3;
    unsigned u = 7u;
    long long big = 1LL << 40;
    size_t sz = sizeof(int);
    char c = 'x';
    _Bool b = 1;
    color col = COLOR_GREEN;
    int arr[2] = {1, 2};
    const int *p = &arr[0];
    const int *null = NULL;

    ASSERT_TRUE(1);
    ASSERT_TRUE(p);
    ASSERT_FALSE(0);
    ASSERT_FALSE(null);
    ASSERT_EQ(i, -3);
    ASSERT_EQ(u, 7u);
    ASSERT_EQ(big, 1LL << 40);
    ASSERT_EQ(sz, sizeof(int));
    ASSERT_EQ(c, 'x');
    ASSERT_EQ(b, 1);
    ASSERT_EQ(col, COLOR_GREEN);
    ASSERT_NE(col, COLOR_RED);
    ASSERT_EQ(p, &arr[0]);
    ASSERT_NE(p, &arr[1]);
    ASSERT_LT(p, &arr[1]);
    ASSERT_EQ(null, NULL);
    ASSERT_LT(i, 0);
    ASSERT_LE(u, 7u);
    ASSERT_GT(u, 6);
    ASSERT_GE(big, 0);
    ASSERT_STREQ("abc", "abc");
    ASSERT_STREQ(NULL, NULL);
    ASSERT_MEMEQ(arr, arr, sizeof arr);
    ASSERT_FLOAT_EQ(0.1 + 0.2, 0.3, 1e-12);
    EXPECT_EQ(i, -3);
    EXPECT_TRUE(u == 7u);
}

/* Mixed signed/unsigned operands compare by value, unlike C's usual conversions. */
TEST(selftest, mixed_sign_compare) {
    ASSERT_LT(-1, 0u);
    ASSERT_GT(0u, -1);
    ASSERT_NE(-1, UINT64_MAX);
    ASSERT_LT(INT64_MIN, UINT64_MAX);
    ASSERT_EQ((uint8_t)255, 255);
    ASSERT_EQ((int8_t)-1, -1LL);
}

/* Operands are evaluated exactly once. */
TEST(selftest, single_evaluation) {
    int n = 0;
    ASSERT_EQ(n++, 0);
    ASSERT_TRUE(n++ == 1);
    ASSERT_FLOAT_EQ((double)n++, 2.0, 0.0);
    ASSERT_EQ(n, 3);
}

/* ---- Failure reporting (unregistered cases run quietly) ---------------------------------- */

static int g_fail_line;
static int g_after_fatal;

DE_TEST_CASE(case_fatal, inner, fatal) {
    g_fail_line = __LINE__ + 1;
    ASSERT_EQ(1 + 1, 3);
    g_after_fatal = 1;
}

TEST(selftest, failure_reports_location) {
    de_test_result r;
    g_after_fatal = 0;
    ASSERT_EQ(de_test_run_case(&case_fatal, &r, 1), DE_TEST_FAILED);
    ASSERT_EQ(r.status, DE_TEST_FAILED);
    ASSERT_EQ(r.failures, 1);
    ASSERT_STREQ(r.file, __FILE__);
    ASSERT_EQ(r.line, g_fail_line);
    ASSERT_TRUE(strstr(r.message, "ASSERT_EQ(1 + 1, 3)") != NULL);
    ASSERT_TRUE(strstr(r.message, "got 2 vs 3") != NULL);
    ASSERT_EQ(g_after_fatal, 0); /* ASSERT stops the test */
}

static int g_after_expect;

DE_TEST_CASE(case_expect, inner, expect) {
    EXPECT_EQ(1, 2);
    EXPECT_TRUE(0);
    EXPECT_STREQ("a", "b");
    EXPECT_MEMEQ("ab", "ax", 2);
    EXPECT_FLOAT_EQ(1.0, 2.0, 0.5);
    g_after_expect = 1;
}

TEST(selftest, expect_continues) {
    de_test_result r;
    g_after_expect = 0;
    ASSERT_EQ(de_test_run_case(&case_expect, &r, 1), DE_TEST_FAILED);
    ASSERT_EQ(r.failures, 5);
    /* EXPECT does not stop the test, and the first failure is the one kept. */
    ASSERT_EQ(g_after_expect, 1);
    ASSERT_TRUE(strstr(r.message, "EXPECT_EQ(1, 2)") != NULL);
}

/* A fatal check inside a helper function still ends the test. */
static void helper_that_fails(void) {
    ASSERT_TRUE(0);
}

static int g_after_helper;

DE_TEST_CASE(case_helper, inner, helper) {
    helper_that_fails();
    g_after_helper = 1;
}

TEST(selftest, assert_in_helper) {
    de_test_result r;
    g_after_helper = 0;
    ASSERT_EQ(de_test_run_case(&case_helper, &r, 1), DE_TEST_FAILED);
    ASSERT_EQ(g_after_helper, 0);
}

DE_TEST_CASE(case_messages, inner, messages) {
    EXPECT_LT(5u, -1);
    EXPECT_MEMEQ("abcd", "abXd", 4);
}

TEST(selftest, check_messages) {
    de_test_result r;
    ASSERT_EQ(de_test_run_case(&case_messages, &r, 1), DE_TEST_FAILED);
    ASSERT_EQ(r.failures, 2);
    ASSERT_TRUE(strstr(r.message, "expected 5u < -1, got 5 vs -1") != NULL);
}

DE_TEST_CASE(case_memeq, inner, memeq) {
    ASSERT_MEMEQ("abcd", "abXd", 4);
}

TEST(selftest, memeq_reports_offset) {
    de_test_result r;
    ASSERT_EQ(de_test_run_case(&case_memeq, &r, 1), DE_TEST_FAILED);
    ASSERT_TRUE(strstr(r.message, "differ at offset 2: 0x63 vs 0x58") != NULL);
}

DE_TEST_CASE(case_fail_macro, inner, fail_macro) {
    FAIL("boom");
}

TEST(selftest, fail_macro) {
    de_test_result r;
    ASSERT_EQ(de_test_run_case(&case_fail_macro, &r, 1), DE_TEST_FAILED);
    ASSERT_TRUE(strstr(r.message, "boom") != NULL);
}

DE_TEST_CASE(case_float_nan, inner, float_nan) {
    volatile double zero = 0.0;
    double nan = zero / zero;
    ASSERT_FLOAT_EQ(nan, nan, 1.0);
}

DE_TEST_CASE(case_float_outside, inner, float_outside) {
    ASSERT_FLOAT_EQ(1.0, 1.1, 0.05);
}

TEST(selftest, float_eps) {
    volatile double zero = 0.0;
    double inf = 1.0 / zero;
    de_test_result r;

    ASSERT_FLOAT_EQ(1.0, 1.0 + 1e-7, 1e-6); /* inside eps */
    ASSERT_FLOAT_EQ(-2.5, -2.5005, 0.001);
    ASSERT_FLOAT_EQ(1.0f, 1.0f, 0.0); /* exact, eps 0 */
    ASSERT_FLOAT_EQ(inf, inf, 0.0);   /* equal infinities */
    ASSERT_FLOAT_EQ(0.25, 0.5, 0.25); /* eps is inclusive */

    ASSERT_EQ(de_test_run_case(&case_float_outside, &r, 1), DE_TEST_FAILED);
    ASSERT_TRUE(strstr(r.message, "eps 0.05") != NULL);
    ASSERT_EQ(de_test_run_case(&case_float_nan, &r, 1), DE_TEST_FAILED); /* NaN never equals */
}

/* ---- Skip ------------------------------------------------------------------------------- */

static int g_skip_line;
static int g_after_skip;

DE_TEST_CASE(case_skip, inner, skip) {
    g_skip_line = __LINE__ + 1;
    SKIP("not on this platform");
    g_after_skip = 1;
}

TEST(selftest, skip_reports_skip) {
    de_test_result r;
    g_after_skip = 0;
    ASSERT_EQ(de_test_run_case(&case_skip, &r, 1), DE_TEST_SKIPPED);
    ASSERT_EQ(r.failures, 0);
    ASSERT_STREQ(r.message, "not on this platform");
    ASSERT_STREQ(r.file, __FILE__);
    ASSERT_EQ(r.line, g_skip_line);
    ASSERT_EQ(g_after_skip, 0);
}

/* ---- Fixtures --------------------------------------------------------------------------- */

typedef struct order_fx {
    int value;
} order_fx;

static int g_log[8];
static int g_log_n;
static int g_fx_fail_in_setup;

static void log_step(int step) {
    if (g_log_n < 8) {
        g_log[g_log_n++] = step;
    }
}

static void order_fx_setup(order_fx *f) {
    ASSERT_EQ(f->value, 0); /* zero-initialised */
    log_step(1);
    f->value = 42;
    if (g_fx_fail_in_setup) {
        FAIL("setup failed");
    }
}

static void order_fx_teardown(order_fx *f) {
    (void)f;
    log_step(3);
}

DE_TEST_F_CASE(case_fx_pass, inner, fx_pass, order_fx) {
    ASSERT_EQ(f->value, 42);
    log_step(2);
}

DE_TEST_F_CASE(case_fx_fail, inner, fx_fail, order_fx) {
    log_step(2);
    ASSERT_TRUE(0);
    log_step(99);
}

DE_TEST_F_CASE(case_fx_skip, inner, fx_skip, order_fx) {
    (void)f;
    log_step(2);
    SKIP("skipped in body");
}

TEST(selftest, fixture_order) {
    de_test_result r;

    g_log_n = 0;
    ASSERT_EQ(de_test_run_case(&case_fx_pass, &r, 1), DE_TEST_PASSED);
    ASSERT_EQ(g_log_n, 3);
    ASSERT_EQ(g_log[0], 1);
    ASSERT_EQ(g_log[1], 2);
    ASSERT_EQ(g_log[2], 3);

    /* A fatal failure in the body still runs teardown. */
    g_log_n = 0;
    ASSERT_EQ(de_test_run_case(&case_fx_fail, &r, 1), DE_TEST_FAILED);
    ASSERT_EQ(g_log_n, 3);
    ASSERT_EQ(g_log[2], 3);

    /* So does a skip. */
    g_log_n = 0;
    ASSERT_EQ(de_test_run_case(&case_fx_skip, &r, 1), DE_TEST_SKIPPED);
    ASSERT_EQ(g_log_n, 3);
    ASSERT_EQ(g_log[2], 3);

    /* A failing setup skips the body; teardown still runs. */
    g_log_n = 0;
    g_fx_fail_in_setup = 1;
    ASSERT_EQ(de_test_run_case(&case_fx_pass, &r, 1), DE_TEST_FAILED);
    g_fx_fail_in_setup = 0;
    ASSERT_EQ(g_log_n, 2);
    ASSERT_EQ(g_log[0], 1);
    ASSERT_EQ(g_log[1], 3);
    ASSERT_TRUE(strstr(r.message, "setup failed") != NULL);
}

/* A registered TEST_F, run by the normal runner. */
TEST_F(selftest, fixture_registered, order_fx) {
    ASSERT_EQ(f->value, 42);
}

/* ---- Registry --------------------------------------------------------------------------- */

TEST(selftest, registry_sorted) {
    size_t n = de_test_count();
    ASSERT_GE(n, 10u);
    for (size_t i = 1; i < n; i++) {
        const de_test_case *a = de_test_get(i - 1);
        const de_test_case *b = de_test_get(i);
        int c = strcmp(a->file, b->file);
        ASSERT_TRUE(c < 0 || (c == 0 && a->line < b->line));
    }
    ASSERT_EQ(de_test_get(n), NULL);
    /* This file's first test is asserts_pass. */
    ASSERT_STREQ(de_test_get(0)->name, "asserts_pass");
}

/* ---- Deliberate failures (only in test_selftest_fail) ------------------------------------ */

#ifdef DE_TEST_SELFTEST_FAIL
TEST(selftest, deliberate_failure) {
    ASSERT_EQ(2 + 2, 5);
}

TEST(selftest, deliberate_skip) {
    SKIP("deliberate skip");
}
#endif
