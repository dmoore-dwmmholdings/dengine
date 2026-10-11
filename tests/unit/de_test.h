/*
 * de_test.h: DEngine's minimal C11 unit-test framework (SPEC §19.1).
 *
 * Dependency-free on purpose (no engine headers), so it can test de_base itself. Test-only:
 * this header is not part of the public API and is never installed.
 *
 * Writing tests
 *
 *     #include "de_test.h"
 *
 *     TEST(ecs, spawn_returns_valid_handle) {
 *         ASSERT_TRUE(h.id != 0);
 *         EXPECT_EQ(count, 3u);
 *     }
 *
 *     typedef struct world_fx { de_world *w; } world_fx;   // needs at least one member
 *     static void world_fx_setup(world_fx *f)    { ... }  // names: <fixture>_setup and
 *     static void world_fx_teardown(world_fx *f) { ... }  //        <fixture>_teardown
 *
 *     TEST_F(ecs, query_empty_world, world_fx) {           // body receives `world_fx *f`
 *         ASSERT_EQ(de_query_count(f->w), 0);
 *     }
 *
 * The fixture is zero-initialised, then setup, body and teardown run in that order. Teardown
 * always runs, even when setup or the body failed or skipped, so it must cope with a fixture that
 * setup left half-initialised.
 *
 * ASSERT_* stops the test at the first failure (it longjmps back to the runner, so it also works
 * inside helper functions). EXPECT_* records the failure and continues. SKIP(msg) ends the test
 * as skipped. Every check evaluates each operand exactly once.
 *
 *     ASSERT_TRUE(c)  ASSERT_FALSE(c)
 *     ASSERT_EQ(a, b) ASSERT_NE ASSERT_LT ASSERT_LE ASSERT_GT ASSERT_GE
 *         Integers (any signed/unsigned type, enums, _Bool, char) and object pointers, selected
 *         with _Generic. Mixed signed/unsigned operands compare by mathematical value
 *         (-1 < 0u is true). Floating-point operands do not compile: use ASSERT_FLOAT_EQ.
 *     ASSERT_FLOAT_EQ(a, b, eps)   |a - b| <= eps, as double. NaN never compares equal.
 *     ASSERT_STREQ(a, b)           strcmp() == 0; NULL equals only NULL.
 *     ASSERT_MEMEQ(a, b, n)        memcmp() == 0; reports the first differing offset.
 *     FAIL(msg)                    unconditional fatal failure.
 *     EXPECT_<same names>          non-fatal variants.
 *
 * Test names are C identifiers. A test is addressed as `suite.name`. Each test executable is built
 * by de_add_test() (cmake/DeTesting.cmake), which links de_test.c (it provides main()) and
 * registers every TEST as its own CTest entry.
 *
 * The runner sorts tests by (file, line), so the order never depends on link order or on the
 * platform's constructor order. Runner options: see de_test_main() in de_test.c, or run the test
 * executable with --help.
 *
 * No heap allocation happens in the assert path: messages are formatted into static buffers.
 * The framework is single-threaded: call checks only from the thread that runs the test.
 */
#ifndef DE_TEST_H
#define DE_TEST_H

#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
#error "de_test.h is C-only (it uses _Generic). Write tests in C11."
#endif

/* ---- Test registry ---------------------------------------------------------------------- */

/** One test. `run` is invoked by the runner inside its failure trap. */
typedef struct de_test_case {
    const char *suite;
    const char *name;
    const char *file;
    int line;
    void (*run)(void);
} de_test_case;

typedef enum de_test_status {
    DE_TEST_PASSED = 0,
    DE_TEST_FAILED = 1,
    DE_TEST_SKIPPED = 2,
} de_test_status;

/** Outcome of one test run. File, line and message describe the first failure (or the skip). */
typedef struct de_test_result {
    de_test_status status;
    int failures;     /**< failed checks, fatal and non-fatal */
    const char *file; /**< NULL when the test passed */
    int line;
    char message[512];
    double ms; /**< wall time of setup + body + teardown */
} de_test_result;

/** Adds a test to the registry. TEST and TEST_F call it before main(). */
void de_test_register(const de_test_case *tc);

/** Number of registered tests. */
size_t de_test_count(void);

/** The i-th registered test, in registration order: sorted by (file, line). */
const de_test_case *de_test_get(size_t i);

/**
 * Runs one test and fills *out. With quiet != 0 nothing is printed. May be called from inside a
 * running test (the framework's own self tests do this to check failure and skip reporting); the
 * outer test's state is saved and restored. Returns out->status.
 */
de_test_status de_test_run_case(const de_test_case *tc, de_test_result *out, int quiet);

/** The runner. de_test.c's main() calls it. Returns the process exit code. */
int de_test_main(int argc, char **argv);

/* ---- Registration macros ---------------------------------------------------------------- */

#define DE__TNAME(prefix, suite, name) de__##prefix##_##suite##__##name

/* Runs `fn` before main(). MSVC/clang-cl: a pointer in the CRT initialiser section .CRT$XCU,
 * kept alive with /include. Everything else (Clang, GCC, MinGW): the constructor attribute. */
#if defined(_MSC_VER)
#pragma section(".CRT$XCU", read)
#if defined(_WIN64)
#define DE__SYM_PREFIX ""
#else
#define DE__SYM_PREFIX "_"
#endif
#define DE__CONSTRUCTOR_(fn)                                                                       \
    static void fn(void);                                                                          \
    __declspec(allocate(".CRT$XCU")) void (*fn##_ptr)(void) = fn;                                  \
    __pragma(comment(linker, "/include:" DE__SYM_PREFIX #fn "_ptr")) static void fn(void)
#else
#define DE__CONSTRUCTOR_(fn)                                                                       \
    static void fn(void) __attribute__((constructor));                                             \
    static void fn(void)
#endif
/* Indirection so that a DE__TNAME(...) argument is expanded before ## and # see it. */
#define DE__CONSTRUCTOR(fn) DE__CONSTRUCTOR_(fn)

/* A TEST_F body need not use `f`. GCC/Clang: mark the parameter unused. MSVC has no C11
 * spelling for that, and a pragma cannot be scoped to a body that follows the macro, so C4100
 * (unreferenced parameter) is off in test translation units. */
#if defined(_MSC_VER)
#pragma warning(disable : 4100)
#endif
#if defined(__GNUC__) || defined(__clang__)
#define DE__UNUSED_PARAM __attribute__((unused))
#else
#define DE__UNUSED_PARAM
#endif

#define DE__CASE_INIT(suite, name, run) {#suite, #name, __FILE__, __LINE__, run}

#define DE__REGISTER(suite, name)                                                                  \
    DE__CONSTRUCTOR(DE__TNAME(r, suite, name)) {                                                   \
        de_test_register(&DE__TNAME(c, suite, name));                                              \
    }

/* Fixture plumbing: typed thunks (no function-pointer casts) and a runner function that owns the
 * zeroed fixture on its stack while de__test_run_fixture() traps failures in each phase. */
#define DE__FIXTURE_DECL(suite, name, fixture)                                                     \
    static void DE__TNAME(t, suite, name)(fixture * f DE__UNUSED_PARAM);                           \
    static void DE__TNAME(fs, suite, name)(void *p) {                                              \
        fixture##_setup((fixture *)p);                                                             \
    }                                                                                              \
    static void DE__TNAME(fb, suite, name)(void *p) {                                              \
        DE__TNAME(t, suite, name)((fixture *)p);                                                   \
    }                                                                                              \
    static void DE__TNAME(ft, suite, name)(void *p) {                                              \
        fixture##_teardown((fixture *)p);                                                          \
    }                                                                                              \
    static void DE__TNAME(fr, suite, name)(void) {                                                 \
        fixture de__fx;                                                                            \
        memset(&de__fx, 0, sizeof de__fx);                                                         \
        de__test_run_fixture(&de__fx, DE__TNAME(fs, suite, name), DE__TNAME(fb, suite, name),      \
                             DE__TNAME(ft, suite, name));                                          \
    }

/** Defines and registers a test. Follow it with the body in braces. */
#define TEST(suite, name)                                                                          \
    static void DE__TNAME(t, suite, name)(void);                                                   \
    static const de_test_case DE__TNAME(c, suite, name) =                                          \
        DE__CASE_INIT(suite, name, DE__TNAME(t, suite, name));                                     \
    DE__REGISTER(suite, name)                                                                      \
    static void DE__TNAME(t, suite, name)(void)

/** Defines and registers a fixture test. The body receives `fixture *f`. */
#define TEST_F(suite, name, fixture)                                                               \
    DE__FIXTURE_DECL(suite, name, fixture)                                                         \
    static const de_test_case DE__TNAME(c, suite, name) =                                          \
        DE__CASE_INIT(suite, name, DE__TNAME(fr, suite, name));                                    \
    DE__REGISTER(suite, name)                                                                      \
    static void DE__TNAME(t, suite, name)(fixture * f DE__UNUSED_PARAM)

/** Like TEST, but not registered: defines `static const de_test_case var` for de_test_run_case. */
#define DE_TEST_CASE(var, suite, name)                                                             \
    static void DE__TNAME(t, suite, name)(void);                                                   \
    static const de_test_case var = DE__CASE_INIT(suite, name, DE__TNAME(t, suite, name));         \
    static void DE__TNAME(t, suite, name)(void)

/** Like TEST_F, but not registered. */
#define DE_TEST_F_CASE(var, suite, name, fixture)                                                  \
    DE__FIXTURE_DECL(suite, name, fixture)                                                         \
    static const de_test_case var = DE__CASE_INIT(suite, name, DE__TNAME(fr, suite, name));        \
    static void DE__TNAME(t, suite, name)(fixture * f DE__UNUSED_PARAM)

/* ---- Check implementation (called by the macros below; not for direct use) -------------- */

enum { DE__TV_SIGNED = 1, DE__TV_UNSIGNED = 2, DE__TV_POINTER = 3 };
enum { DE__OP_EQ, DE__OP_NE, DE__OP_LT, DE__OP_LE, DE__OP_GT, DE__OP_GE };

/** A comparable value: an integer (signed or unsigned) or an object pointer. */
typedef struct de__tv {
    int kind;
    long long i;
    unsigned long long u;
    const void *p;
} de__tv;

de__tv de__tv_s(long long v);
de__tv de__tv_u(unsigned long long v);
de__tv de__tv_p(const volatile void *v);

/* Only the selected function is called, so each operand converts through a matching prototype:
 * no integer<->pointer casts, and a double or struct operand is a compile error. */
#define DE__TV(x)                                                                                  \
    _Generic((x),                                                                                  \
        _Bool: de__tv_u,                                                                           \
        char: de__tv_s,                                                                            \
        signed char: de__tv_s,                                                                     \
        short: de__tv_s,                                                                           \
        int: de__tv_s,                                                                             \
        long: de__tv_s,                                                                            \
        long long: de__tv_s,                                                                       \
        unsigned char: de__tv_u,                                                                   \
        unsigned short: de__tv_u,                                                                  \
        unsigned int: de__tv_u,                                                                    \
        unsigned long: de__tv_u,                                                                   \
        unsigned long long: de__tv_u,                                                              \
        default: de__tv_p)(x)

void de__test_bool(int fatal, int expected, int value, const char *expr, const char *file,
                   int line);
void de__test_cmp(int fatal, int op, de__tv a, de__tv b, const char *as, const char *bs,
                  const char *file, int line);
void de__test_float(int fatal, double a, double b, double eps, const char *as, const char *bs,
                    const char *file, int line);
void de__test_str(int fatal, const char *a, const char *b, const char *as, const char *bs,
                  const char *file, int line);
void de__test_mem(int fatal, const void *a, const void *b, size_t n, const char *as, const char *bs,
                  const char *file, int line);
_Noreturn void de__test_fail(const char *msg, const char *file, int line);
_Noreturn void de__test_skip(const char *msg, const char *file, int line);
void de__test_run_fixture(void *fixture, void (*setup)(void *), void (*body)(void *),
                          void (*teardown)(void *));

/* ---- Checks ----------------------------------------------------------------------------- */

#define DE__CMP(fatal, op, a, b)                                                                   \
    de__test_cmp(fatal, op, DE__TV(a), DE__TV(b), #a, #b, __FILE__, __LINE__)

#define ASSERT_TRUE(c)  de__test_bool(1, 1, (c) ? 1 : 0, #c, __FILE__, __LINE__)
#define ASSERT_FALSE(c) de__test_bool(1, 0, (c) ? 1 : 0, #c, __FILE__, __LINE__)
#define ASSERT_EQ(a, b) DE__CMP(1, DE__OP_EQ, a, b)
#define ASSERT_NE(a, b) DE__CMP(1, DE__OP_NE, a, b)
#define ASSERT_LT(a, b) DE__CMP(1, DE__OP_LT, a, b)
#define ASSERT_LE(a, b) DE__CMP(1, DE__OP_LE, a, b)
#define ASSERT_GT(a, b) DE__CMP(1, DE__OP_GT, a, b)
#define ASSERT_GE(a, b) DE__CMP(1, DE__OP_GE, a, b)
#define ASSERT_FLOAT_EQ(a, b, eps)                                                                 \
    de__test_float(1, (double)(a), (double)(b), (double)(eps), #a, #b, __FILE__, __LINE__)
#define ASSERT_STREQ(a, b)    de__test_str(1, (a), (b), #a, #b, __FILE__, __LINE__)
#define ASSERT_MEMEQ(a, b, n) de__test_mem(1, (a), (b), (n), #a, #b, __FILE__, __LINE__)

#define EXPECT_TRUE(c)  de__test_bool(0, 1, (c) ? 1 : 0, #c, __FILE__, __LINE__)
#define EXPECT_FALSE(c) de__test_bool(0, 0, (c) ? 1 : 0, #c, __FILE__, __LINE__)
#define EXPECT_EQ(a, b) DE__CMP(0, DE__OP_EQ, a, b)
#define EXPECT_NE(a, b) DE__CMP(0, DE__OP_NE, a, b)
#define EXPECT_LT(a, b) DE__CMP(0, DE__OP_LT, a, b)
#define EXPECT_LE(a, b) DE__CMP(0, DE__OP_LE, a, b)
#define EXPECT_GT(a, b) DE__CMP(0, DE__OP_GT, a, b)
#define EXPECT_GE(a, b) DE__CMP(0, DE__OP_GE, a, b)
#define EXPECT_FLOAT_EQ(a, b, eps)                                                                 \
    de__test_float(0, (double)(a), (double)(b), (double)(eps), #a, #b, __FILE__, __LINE__)
#define EXPECT_STREQ(a, b)    de__test_str(0, (a), (b), #a, #b, __FILE__, __LINE__)
#define EXPECT_MEMEQ(a, b, n) de__test_mem(0, (a), (b), (n), #a, #b, __FILE__, __LINE__)

#define FAIL(msg) de__test_fail((msg), __FILE__, __LINE__)
#define SKIP(msg) de__test_skip((msg), __FILE__, __LINE__)

#endif /* DE_TEST_H */
