/*
 * de_test.c: registry, checks and runner for de_test.h. Provides main().
 *
 * Usage: <test-exe> [options]
 *   --filter=PATTERNS  Run only matching tests. PATTERNS is POS[-NEG]: ':'-separated globs
 *                      (`*`, `?`) on `suite.name`; tests matching a NEG glob are excluded.
 *                      Examples: --filter=ecs.*   --filter=*-ecs.slow_*
 *   --list             Print the selected test names, one per line, in run order, and exit.
 *   --repeat=N         Run the selection N times (N >= 1).
 *   --shuffle          Shuffle the run order. Deterministic for a given --seed.
 *   --seed=N           Shuffle seed (default: derived from the clock, and printed).
 *   --help             Print this text.
 *
 * Exit code: 0 when every selected test passed (or some skipped), 1 when a test failed or the
 * filter selected nothing, 2 on a usage or registry error, 77 when every selected test skipped
 * (de_add_test() sets SKIP_RETURN_CODE 77, so CTest reports those as skipped).
 */
#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
/* C4611: setjmp and C++ destructors. This is C; nothing to destroy. */
#pragma warning(disable : 4611)
#endif

#include "de_test.h"

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DE_TEST_MAX_CASES 8192

/* ---- Registry ---------------------------------------------------------------------------- */

static const de_test_case *g_cases[DE_TEST_MAX_CASES];
static size_t g_count;
static int g_overflow;
static int g_sorted;

void de_test_register(const de_test_case *tc) {
    if (g_count < DE_TEST_MAX_CASES) {
        g_cases[g_count++] = tc;
    } else {
        g_overflow = 1;
    }
    g_sorted = 0;
}

static int case_cmp(const void *a, const void *b) {
    const de_test_case *x = *(const de_test_case *const *)a;
    const de_test_case *y = *(const de_test_case *const *)b;
    int c = strcmp(x->file, y->file);
    if (c != 0) {
        return c;
    }
    if (x->line != y->line) {
        return x->line < y->line ? -1 : 1;
    }
    /* Same file and line only for macro-generated tests; keep a total order. */
    c = strcmp(x->suite, y->suite);
    return c != 0 ? c : strcmp(x->name, y->name);
}

/* Constructor order is unspecified across translation units (and differs between linkers), so
 * the registry is sorted by (file, line) before anyone reads it. */
static void ensure_sorted(void) {
    if (!g_sorted) {
        /* The explicit cast: MSVC warns (C4090) when `const de_test_case **` converts to
         * `void *` implicitly, although only the pointers are moved, never the cases. */
        qsort((void *)g_cases, g_count, sizeof g_cases[0], case_cmp);
        g_sorted = 1;
    }
}

size_t de_test_count(void) {
    return g_count;
}

const de_test_case *de_test_get(size_t i) {
    ensure_sorted();
    return i < g_count ? g_cases[i] : NULL;
}

/* ---- Current-test state ------------------------------------------------------------------- */

typedef struct run_state {
    jmp_buf *trap; /* where a fatal failure or skip jumps to */
    de_test_result *res;
    int quiet;
} run_state;

static run_state g_cur;
static char g_msg[1024]; /* failure text; static so the assert path never allocates */

static double now_ms(void) {
    struct timespec ts;
    if (timespec_get(&ts, TIME_UTC) != TIME_UTC) {
        return 0.0;
    }
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

static void copy_str(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static _Noreturn void outside_test(const char *file, int line, const char *msg) {
    fprintf(stderr, "[FAIL] %s:%d: %s (check used outside a running test)\n", file, line, msg);
    fflush(stderr);
    abort();
}

/* Records a failure of the current test; jumps back to the runner when fatal. */
static void report_failure(int fatal, const char *file, int line, const char *msg) {
    de_test_result *res = g_cur.res;
    if (res == NULL) {
        outside_test(file, line, msg);
    }
    if (res->failures == 0) {
        res->file = file;
        res->line = line;
        copy_str(res->message, sizeof res->message, msg);
    }
    res->failures++;
    res->status = DE_TEST_FAILED;
    if (!g_cur.quiet) {
        printf("[FAIL] %s:%d: %s\n", file, line, msg);
        fflush(stdout);
    }
    if (fatal) {
        longjmp(*g_cur.trap, 1);
    }
}

/* ---- Checks ------------------------------------------------------------------------------ */

/* Field assignments, not a braced initialiser with runtime values: older MSVC flags that as
 * the C89 extension C4204 at /W4. */
static de__tv tv_make(int kind) {
    de__tv t;
    memset(&t, 0, sizeof t);
    t.kind = kind;
    return t;
}

de__tv de__tv_s(long long v) {
    de__tv t = tv_make(DE__TV_SIGNED);
    t.i = v;
    return t;
}

de__tv de__tv_u(unsigned long long v) {
    de__tv t = tv_make(DE__TV_UNSIGNED);
    t.u = v;
    return t;
}

de__tv de__tv_p(const volatile void *v) {
    de__tv t = tv_make(DE__TV_POINTER);
    t.p = (const void *)(uintptr_t)v;
    return t;
}

static const char *const k_op_name[] = {"EQ", "NE", "LT", "LE", "GT", "GE"};
static const char *const k_op_sym[] = {"==", "!=", "<", "<=", ">", ">="};

/* Three-way compare by mathematical value. A pointer against an integer compares the address. */
static int tv_cmp(de__tv a, de__tv b) {
    if (a.kind == DE__TV_POINTER || b.kind == DE__TV_POINTER) {
        uintptr_t x = a.kind == DE__TV_POINTER    ? (uintptr_t)a.p
                      : a.kind == DE__TV_UNSIGNED ? (uintptr_t)a.u
                                                  : (uintptr_t)a.i;
        uintptr_t y = b.kind == DE__TV_POINTER    ? (uintptr_t)b.p
                      : b.kind == DE__TV_UNSIGNED ? (uintptr_t)b.u
                                                  : (uintptr_t)b.i;
        return (x > y) - (x < y);
    }
    if (a.kind == DE__TV_SIGNED && b.kind == DE__TV_SIGNED) {
        return (a.i > b.i) - (a.i < b.i);
    }
    if (a.kind == DE__TV_UNSIGNED && b.kind == DE__TV_UNSIGNED) {
        return (a.u > b.u) - (a.u < b.u);
    }
    if (a.kind == DE__TV_SIGNED) { /* signed vs unsigned */
        if (a.i < 0) {
            return -1;
        }
        return ((unsigned long long)a.i > b.u) - ((unsigned long long)a.i < b.u);
    }
    if (b.i < 0) { /* unsigned vs signed */
        return 1;
    }
    return (a.u > (unsigned long long)b.i) - (a.u < (unsigned long long)b.i);
}

static void tv_format(char *buf, size_t cap, de__tv v) {
    switch (v.kind) {
    case DE__TV_SIGNED:
        snprintf(buf, cap, "%lld", v.i);
        break;
    case DE__TV_UNSIGNED:
        snprintf(buf, cap, "%llu", v.u);
        break;
    default:
        snprintf(buf, cap, "%p", v.p);
        break;
    }
}

void de__test_bool(int fatal, int expected, int value, const char *expr, const char *file,
                   int line) {
    if (value == expected) {
        return;
    }
    snprintf(g_msg, sizeof g_msg, "%s_%s(%s): got %s", fatal ? "ASSERT" : "EXPECT",
             expected ? "TRUE" : "FALSE", expr, value ? "true" : "false");
    report_failure(fatal, file, line, g_msg);
}

void de__test_cmp(int fatal, int op, de__tv a, de__tv b, const char *as, const char *bs,
                  const char *file, int line) {
    int c = tv_cmp(a, b);
    int ok = 0;
    switch (op) {
    case DE__OP_EQ:
        ok = c == 0;
        break;
    case DE__OP_NE:
        ok = c != 0;
        break;
    case DE__OP_LT:
        ok = c < 0;
        break;
    case DE__OP_LE:
        ok = c <= 0;
        break;
    case DE__OP_GT:
        ok = c > 0;
        break;
    case DE__OP_GE:
        ok = c >= 0;
        break;
    default:
        break;
    }
    if (ok) {
        return;
    }
    char av[32];
    char bv[32];
    tv_format(av, sizeof av, a);
    tv_format(bv, sizeof bv, b);
    snprintf(g_msg, sizeof g_msg, "%s_%s(%s, %s): expected %s %s %s, got %s vs %s",
             fatal ? "ASSERT" : "EXPECT", k_op_name[op], as, bs, as, k_op_sym[op], bs, av, bv);
    report_failure(fatal, file, line, g_msg);
}

void de__test_float(int fatal, double a, double b, double eps, const char *as, const char *bs,
                    const char *file, int line) {
    /* a == b first, so equal infinities pass; NaN fails every comparison below. */
    if (a == b) {
        return;
    }
    double d = a > b ? a - b : b - a;
    if (d <= eps) {
        return;
    }
    snprintf(g_msg, sizeof g_msg, "%s_FLOAT_EQ(%s, %s): got %.9g vs %.9g (diff %.9g > eps %.9g)",
             fatal ? "ASSERT" : "EXPECT", as, bs, a, b, d, eps);
    report_failure(fatal, file, line, g_msg);
}

void de__test_str(int fatal, const char *a, const char *b, const char *as, const char *bs,
                  const char *file, int line) {
    if (a == b || (a != NULL && b != NULL && strcmp(a, b) == 0)) {
        return;
    }
    snprintf(g_msg, sizeof g_msg, "%s_STREQ(%s, %s): got \"%.200s\" vs \"%.200s\"",
             fatal ? "ASSERT" : "EXPECT", as, bs, a ? a : "(null)", b ? b : "(null)");
    report_failure(fatal, file, line, g_msg);
}

void de__test_mem(int fatal, const void *a, const void *b, size_t n, const char *as, const char *bs,
                  const char *file, int line) {
    if (n == 0 || a == b) {
        return;
    }
    if (a == NULL || b == NULL) {
        snprintf(g_msg, sizeof g_msg, "%s_MEMEQ(%s, %s): NULL pointer", fatal ? "ASSERT" : "EXPECT",
                 as, bs);
        report_failure(fatal, file, line, g_msg);
        return;
    }
    if (memcmp(a, b, n) == 0) {
        return;
    }
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    size_t i = 0;
    while (x[i] == y[i]) {
        i++;
    }
    snprintf(g_msg, sizeof g_msg,
             "%s_MEMEQ(%s, %s): %zu bytes differ at offset %zu: 0x%02x vs 0x%02x",
             fatal ? "ASSERT" : "EXPECT", as, bs, n, i, x[i], y[i]);
    report_failure(fatal, file, line, g_msg);
}

_Noreturn void de__test_fail(const char *msg, const char *file, int line) {
    snprintf(g_msg, sizeof g_msg, "FAIL: %s", msg ? msg : "");
    report_failure(1, file, line, g_msg);
    abort(); /* not reached: report_failure() jumps for fatal failures */
}

_Noreturn void de__test_skip(const char *msg, const char *file, int line) {
    de_test_result *res = g_cur.res;
    if (res == NULL) {
        outside_test(file, line, "SKIP");
    }
    if (res->status == DE_TEST_PASSED) {
        res->status = DE_TEST_SKIPPED;
        res->file = file;
        res->line = line;
        copy_str(res->message, sizeof res->message, msg ? msg : "");
    }
    longjmp(*g_cur.trap, 1);
}

void de__test_run_fixture(void *fixture, void (*setup)(void *), void (*body)(void *),
                          void (*teardown)(void *)) {
    jmp_buf *outer = g_cur.trap;
    jmp_buf trap;
    g_cur.trap = &trap;
    if (setjmp(trap) == 0) {
        setup(fixture);
    }
    /* g_cur.res is a global, so reading it after a longjmp is well defined. */
    if (g_cur.res->status == DE_TEST_PASSED && setjmp(trap) == 0) {
        body(fixture);
    }
    if (setjmp(trap) == 0) {
        teardown(fixture);
    }
    g_cur.trap = outer;
}

de_test_status de_test_run_case(const de_test_case *tc, de_test_result *out, int quiet) {
    run_state saved = g_cur;
    jmp_buf trap;
    memset(out, 0, sizeof *out);
    out->status = DE_TEST_PASSED;
    g_cur.trap = &trap;
    g_cur.res = out;
    g_cur.quiet = quiet;
    /* volatile: read after a possible longjmp back into this frame. */
    volatile double t0 = now_ms();
    if (setjmp(trap) == 0) {
        tc->run();
    }
    out->ms = now_ms() - t0;
    g_cur = saved;
    return out->status;
}

/* ---- Runner ------------------------------------------------------------------------------ */

static int glob_match(const char *pat, const char *pat_end, const char *s) {
    const char *star = NULL;
    const char *resume = NULL;
    while (*s != '\0') {
        if (pat < pat_end && (*pat == '?' || *pat == *s)) {
            pat++;
            s++;
        } else if (pat < pat_end && *pat == '*') {
            star = pat++;
            resume = s;
        } else if (star != NULL) {
            pat = star + 1;
            s = ++resume;
        } else {
            return 0;
        }
    }
    while (pat < pat_end && *pat == '*') {
        pat++;
    }
    return pat == pat_end;
}

/* True if `name` matches one of the ':'-separated globs in [list, end). */
static int glob_list_match(const char *list, const char *end, const char *name) {
    while (list < end) {
        const char *sep = list;
        while (sep < end && *sep != ':') {
            sep++;
        }
        if (sep > list && glob_match(list, sep, name)) {
            return 1;
        }
        list = sep + 1;
    }
    return 0;
}

static int filter_match(const char *filter, const char *name) {
    if (filter == NULL) {
        return 1;
    }
    const char *end = filter + strlen(filter);
    const char *dash = strchr(filter, '-');
    const char *pos_end = dash ? dash : end;
    int pos = pos_end == filter ? 1 : glob_list_match(filter, pos_end, name);
    if (!pos) {
        return 0;
    }
    return dash ? !glob_list_match(dash + 1, end, name) : 1;
}

/* splitmix64: tiny, well-distributed, and identical on every platform. */
static uint64_t rng_next(uint64_t *state) {
    uint64_t z = (*state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static void shuffle(const de_test_case **v, size_t n, uint64_t *state) {
    for (size_t i = n; i > 1; i--) {
        size_t j = (size_t)(rng_next(state) % i);
        const de_test_case *t = v[i - 1];
        v[i - 1] = v[j];
        v[j] = t;
    }
}

static void full_name(char *buf, size_t cap, const de_test_case *tc) {
    snprintf(buf, cap, "%s.%s", tc->suite, tc->name);
}

static int parse_u64(const char *s, unsigned long long *out) {
    char *end = NULL;
    if (*s == '\0' || *s == '-') {
        return 0;
    }
    *out = strtoull(s, &end, 10);
    return *end == '\0';
}

static void usage(FILE *f, const char *argv0) {
    fprintf(f,
            "usage: %s [--filter=POS[-NEG]] [--list] [--repeat=N] [--shuffle] [--seed=N]\n"
            "  --filter   ':'-separated globs on suite.name; after '-', globs to exclude\n"
            "  --list     print selected tests in run order and exit\n"
            "  --repeat   run the selection N times\n"
            "  --shuffle  shuffle the order (deterministic for a given --seed)\n",
            argv0);
}

static const de_test_case *g_sel[DE_TEST_MAX_CASES];

int de_test_main(int argc, char **argv) {
    const char *filter = NULL;
    int list = 0;
    int do_shuffle = 0;
    int have_seed = 0;
    unsigned long long seed = 0;
    unsigned long long repeat = 1;
    const char *argv0 = argc > 0 ? argv[0] : "test";

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strncmp(a, "--filter=", 9) == 0) {
            filter = a + 9;
        } else if (strcmp(a, "--list") == 0) {
            list = 1;
        } else if (strncmp(a, "--repeat=", 9) == 0) {
            if (!parse_u64(a + 9, &repeat) || repeat == 0) {
                fprintf(stderr, "error: --repeat needs a positive integer\n");
                return 2;
            }
        } else if (strcmp(a, "--shuffle") == 0) {
            do_shuffle = 1;
        } else if (strncmp(a, "--seed=", 7) == 0) {
            if (!parse_u64(a + 7, &seed)) {
                fprintf(stderr, "error: --seed needs a non-negative integer\n");
                return 2;
            }
            have_seed = 1;
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(stdout, argv0);
            return 0;
        } else {
            fprintf(stderr, "error: unknown argument '%s'\n", a);
            usage(stderr, argv0);
            return 2;
        }
    }

    if (g_overflow) {
        fprintf(stderr, "error: more than %d tests in one executable\n", DE_TEST_MAX_CASES);
        return 2;
    }
    ensure_sorted();

    char name[256];
    size_t nsel = 0;
    for (size_t i = 0; i < g_count; i++) {
        full_name(name, sizeof name, g_cases[i]);
        for (size_t j = 0; j < i; j++) {
            /* Quadratic, but only over a few hundred tests, once per process. */
            if (strcmp(g_cases[j]->suite, g_cases[i]->suite) == 0 &&
                strcmp(g_cases[j]->name, g_cases[i]->name) == 0) {
                fprintf(stderr, "error: duplicate test %s (%s:%d and %s:%d)\n", name,
                        g_cases[j]->file, g_cases[j]->line, g_cases[i]->file, g_cases[i]->line);
                return 2;
            }
        }
        if (filter_match(filter, name)) {
            g_sel[nsel++] = g_cases[i];
        }
    }

    if (do_shuffle && !have_seed) {
        seed = (unsigned long long)time(NULL);
    }
    uint64_t rng = (uint64_t)seed;

    if (list) {
        if (do_shuffle) {
            shuffle(g_sel, nsel, &rng);
        }
        for (size_t i = 0; i < nsel; i++) {
            full_name(name, sizeof name, g_sel[i]);
            printf("%s\n", name);
        }
        return 0;
    }

    if (nsel == 0) {
        fprintf(stderr, "error: no test matches --filter=%s\n", filter ? filter : "");
        return 1;
    }

    size_t passed = 0;
    size_t failed = 0;
    size_t skipped = 0;
    double total_ms = 0.0;
    if (do_shuffle) {
        printf("[==========] shuffle seed %llu\n", seed);
    }
    for (unsigned long long r = 0; r < repeat; r++) {
        if (repeat > 1) {
            printf("[==========] repeat %llu/%llu\n", r + 1, repeat);
        }
        if (do_shuffle) {
            shuffle(g_sel, nsel, &rng);
        }
        for (size_t i = 0; i < nsel; i++) {
            de_test_result res;
            full_name(name, sizeof name, g_sel[i]);
            printf("[ RUN ] %s\n", name);
            fflush(stdout);
            de_test_run_case(g_sel[i], &res, 0);
            total_ms += res.ms;
            switch (res.status) {
            case DE_TEST_PASSED:
                passed++;
                printf("[ OK ] %s (%.3f ms)\n", name, res.ms);
                break;
            case DE_TEST_SKIPPED:
                skipped++;
                printf("[SKIP] %s:%d: %s\n", res.file, res.line, res.message);
                printf("[ SKIPPED ] %s (%.3f ms)\n", name, res.ms);
                break;
            default:
                failed++;
                printf("[ FAILED ] %s (%.3f ms)\n", name, res.ms);
                break;
            }
            fflush(stdout);
        }
    }

    printf("[==========] %zu run (%.3f ms): %zu passed, %zu failed, %zu skipped\n",
           passed + failed + skipped, total_ms, passed, failed, skipped);
    if (failed > 0) {
        return 1;
    }
    return passed == 0 ? 77 : 0;
}

#ifndef DE_TEST_NO_MAIN
int main(int argc, char **argv) {
    return de_test_main(argc, argv);
}
#endif
