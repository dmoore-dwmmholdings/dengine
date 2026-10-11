/* test_crash.c: crash handler tests (SPEC §18.5, DENG-251). CTest names: crash.<test>.
 *
 * Crashing tests fork: the child installs the handler and crashes, the parent checks how the
 * child died and what it left in a fresh temporary dump directory. Checks that must happen in
 * the child (inside a chained handler) are written into the report, which the parent reads.
 * The fork tests are POSIX-only; on Windows the SEH/minidump path is verified by hand
 * (needs-hw-verify on DENG-251) and only the in-process tests run here. */
#if defined(__linux__)
#define _GNU_SOURCE /* mkdtemp, sigaction under -std=c11 */
#elif defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif

#include "de_test.h"

#include <dengine/de_base.h>
#include <dengine/de_debug.h>
#include <dengine/de_platform.h>

#include "platform/crash_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "de_alloc_guard.h"
#define DE__CRASH_FORK_TESTS 1
#endif

#if defined(__SANITIZE_THREAD__)
#define DE__TEST_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define DE__TEST_TSAN 1
#endif
#endif
#ifndef DE__TEST_TSAN
#define DE__TEST_TSAN 0
#endif

/* ---------- In-process tests: annotations, handler table, build ID ---------- */

TEST(crash, annotations) {
    de__crash_reset_for_tests();
    char v[DE_CRASH_ANNOTATION_VALUE_MAX];

    de_crash_set_annotation("level", "e1m1");
    de_crash_set_annotation("gpu", "M2 Max");
    ASSERT_TRUE(de__crash_annotation_get("level", v, sizeof v));
    EXPECT_STREQ(v, "e1m1");
    ASSERT_TRUE(de__crash_annotation_get("gpu", v, sizeof v));
    EXPECT_STREQ(v, "M2 Max");
    EXPECT_EQ(de__crash_annotation_count(), 2u);

    /* Overwrite keeps one entry. */
    de_crash_set_annotation("level", "e1m2");
    ASSERT_TRUE(de__crash_annotation_get("level", v, sizeof v));
    EXPECT_STREQ(v, "e1m2");
    EXPECT_EQ(de__crash_annotation_count(), 2u);

    /* Empty and NULL values clear; clearing a missing key does nothing. */
    de_crash_set_annotation("level", "");
    EXPECT_FALSE(de__crash_annotation_get("level", v, sizeof v));
    de_crash_set_annotation("gpu", NULL);
    EXPECT_FALSE(de__crash_annotation_get("gpu", v, sizeof v));
    de_crash_set_annotation("missing", "");
    EXPECT_EQ(de__crash_annotation_count(), 0u);

    /* NULL and empty keys are ignored. */
    de_crash_set_annotation(NULL, "x");
    de_crash_set_annotation("", "x");
    EXPECT_EQ(de__crash_annotation_count(), 0u);

    /* Over-long keys and values are cut to 63 and 255 bytes. */
    char long_key[100], long_val[400];
    memset(long_key, 'k', sizeof long_key - 1);
    long_key[sizeof long_key - 1] = '\0';
    memset(long_val, 'v', sizeof long_val - 1);
    long_val[sizeof long_val - 1] = '\0';
    de_crash_set_annotation(long_key, long_val);
    ASSERT_TRUE(de__crash_annotation_get(long_key, v, sizeof v));
    EXPECT_EQ(strlen(v), (size_t)(DE_CRASH_ANNOTATION_VALUE_MAX - 1));
    char cut_key[DE_CRASH_ANNOTATION_KEY_MAX];
    memcpy(cut_key, long_key, DE_CRASH_ANNOTATION_KEY_MAX - 1);
    cut_key[DE_CRASH_ANNOTATION_KEY_MAX - 1] = '\0';
    ASSERT_TRUE(de__crash_annotation_get(cut_key, v, sizeof v)); /* same entry */
    EXPECT_EQ(de__crash_annotation_count(), 1u);

    /* The cut never splits a UTF-8 sequence: 254 ASCII bytes + "é" (2 bytes) needs 256 + NUL. */
    char utf8[300];
    memset(utf8, 'a', 254);
    memcpy(utf8 + 254, "\xC3\xA9", 3);
    de_crash_set_annotation("utf8", utf8);
    ASSERT_TRUE(de__crash_annotation_get("utf8", v, sizeof v));
    EXPECT_EQ(strlen(v), (size_t)254);
    de__crash_reset_for_tests();
}

TEST(crash, annotation_overflow) {
    de__crash_reset_for_tests();
    char key[32], v[DE_CRASH_ANNOTATION_VALUE_MAX];
    for (int i = 0; i < DE_CRASH_MAX_ANNOTATIONS; i++) {
        snprintf(key, sizeof key, "key%02d", i);
        de_crash_set_annotation(key, key);
    }
    EXPECT_EQ(de__crash_annotation_count(), (u32)DE_CRASH_MAX_ANNOTATIONS);
    EXPECT_EQ(de__crash_annotation_dropped(), 0u);

    /* The 65th distinct key is dropped and counted; existing keys still update. */
    de_crash_set_annotation("one_too_many", "x");
    EXPECT_FALSE(de__crash_annotation_get("one_too_many", v, sizeof v));
    EXPECT_EQ(de__crash_annotation_dropped(), 1u);
    de_crash_set_annotation("key07", "updated");
    ASSERT_TRUE(de__crash_annotation_get("key07", v, sizeof v));
    EXPECT_STREQ(v, "updated");
    EXPECT_EQ(de__crash_annotation_dropped(), 1u);

    /* Clearing a key frees its entry for a new one. */
    de_crash_set_annotation("key13", "");
    de_crash_set_annotation("one_too_many", "now it fits");
    ASSERT_TRUE(de__crash_annotation_get("one_too_many", v, sizeof v));
    EXPECT_STREQ(v, "now it fits");
    EXPECT_EQ(de__crash_annotation_count(), (u32)DE_CRASH_MAX_ANNOTATIONS);
    for (int i = 0; i < DE_CRASH_MAX_ANNOTATIONS; i++) {
        snprintf(key, sizeof key, "key%02d", i);
        EXPECT_EQ(de__crash_annotation_get(key, v, sizeof v), i != 13);
    }
    de__crash_reset_for_tests();
}

/* Four writers: each updates its own key and a shared key, and adds and clears a key of its
 * own, so slot updates, claims and frees all race. Run under TSan (DE_SANITIZE=thread). */
#define DE__CONC_ITERS 20000

typedef struct conc_arg {
    int id;
} conc_arg;

static i32 conc_writer(void *p) {
    const conc_arg *a = (const conc_arg *)p;
    char own[16], tmp[16], val[64];
    snprintf(own, sizeof own, "own%d", a->id);
    snprintf(tmp, sizeof tmp, "tmp%d", a->id);
    for (int i = 0; i < DE__CONC_ITERS; i++) {
        /* A value repeats one character, so a torn read would show mixed characters. */
        char c = (char)('a' + (a->id * 7 + i) % 26);
        size_t n = (size_t)(1 + i % 60);
        memset(val, c, n);
        val[n] = '\0';
        de_crash_set_annotation(own, val);
        de_crash_set_annotation("shared", val);
        de_crash_set_annotation(tmp, (i & 1) ? "" : val);
    }
    snprintf(val, sizeof val, "final%d", a->id);
    de_crash_set_annotation(own, val);
    de_crash_set_annotation(tmp, "");
    return 0;
}

TEST(crash, annotations_concurrent) {
    de__crash_reset_for_tests();
    de_thread *t[4];
    conc_arg args[4];
    for (int i = 0; i < 4; i++) {
        args[i].id = i;
        ASSERT_EQ(
            de_thread_create(conc_writer, &args[i], "crash-annot", DE_THREAD_NORMAL, 0, &t[i]),
            DE_OK);
    }
    /* Read while they write: every value seen must be one character repeated. */
    char v[DE_CRASH_ANNOTATION_VALUE_MAX];
    int torn = 0;
    for (int i = 0; i < 20000; i++) {
        if (de__crash_annotation_get("shared", v, sizeof v) && v[0] != '\0')
            for (size_t j = 1; v[j] != '\0'; j++)
                if (v[j] != v[0]) {
                    torn++;
                    break;
                }
    }
    for (int i = 0; i < 4; i++)
        EXPECT_EQ(de_thread_join(t[i]), 0);
    EXPECT_EQ(torn, 0);
    char key[16], want[16];
    for (int i = 0; i < 4; i++) {
        snprintf(key, sizeof key, "own%d", i);
        snprintf(want, sizeof want, "final%d", i);
        ASSERT_TRUE(de__crash_annotation_get(key, v, sizeof v));
        EXPECT_STREQ(v, want);
    }
    EXPECT_TRUE(de__crash_annotation_get("shared", v, sizeof v));
    EXPECT_EQ(de__crash_annotation_count(), 5u); /* own0..3 + shared; every tmp is cleared */
    EXPECT_EQ(de__crash_annotation_dropped(), 0u);
    de__crash_reset_for_tests();
}

static void noop_handler(const de_crash_context *ctx, void *user) {
    (void)ctx;
    (void)user;
}

TEST(crash, handler_limit) {
    de__crash_reset_for_tests();
    EXPECT_EQ(de_crash_add_handler(NULL, NULL), DE_ERR_INVALID_ARGUMENT);
    for (int i = 0; i < DE_CRASH_MAX_HANDLERS; i++)
        EXPECT_EQ(de_crash_add_handler(noop_handler, NULL), DE_OK);
    EXPECT_EQ(de_crash_add_handler(noop_handler, NULL), DE_ERR_BUSY);
    de__crash_reset_for_tests();
}

TEST(crash, build_id) {
    const char *id = de_build_id();
    ASSERT_TRUE(id != NULL);
    ASSERT_TRUE(id[0] != '\0');
    EXPECT_STREQ(id, DE_TEST_BUILD_ID); /* what src/platform/CMakeLists.txt computed */
    /* A git hash, "unknown", or a -DDE_BUILD_ID override; CMake allows only these characters. */
    for (const char *p = id; *p != '\0'; p++)
        EXPECT_TRUE((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') ||
                    (*p >= 'A' && *p <= 'Z') || *p == '.' || *p == '_' || *p == '+' || *p == '-');
}

TEST(crash, fmt_helpers) {
    char buf[24];
    EXPECT_EQ(de__crash_fmt_u64(buf, 0, 10), (size_t)1);
    EXPECT_STREQ(buf, "0");
    de__crash_fmt_u64(buf, 18446744073709551615ull, 10);
    EXPECT_STREQ(buf, "18446744073709551615");
    de__crash_fmt_u64(buf, 0xdeadbeefull, 16);
    EXPECT_STREQ(buf, "deadbeef");
    char small[8] = "ab";
    EXPECT_TRUE(de__crash_str_append(small, sizeof small, "cde"));
    EXPECT_FALSE(de__crash_str_append(small, sizeof small, "fghij"));
    EXPECT_STREQ(small, "abcdefg");
}

#if defined(DE__CRASH_FORK_TESTS)

/* ---------- Fork helpers ---------- */

typedef struct crash_dir {
    char path[512];
} crash_dir;

static void crash_dir_make(crash_dir *d) {
    const char *tmp = getenv("TMPDIR");
    if (tmp == NULL || tmp[0] == '\0')
        tmp = "/tmp";
    snprintf(d->path, sizeof d->path, "%s/de_crash_XXXXXX", tmp);
    ASSERT_TRUE(mkdtemp(d->path) != NULL);
}

/* out = a + b; fails the test if it does not fit. */
static void path_join(char *out, size_t cap, const char *a, const char *b) {
    int n = snprintf(out, cap, "%s%s", a, b);
    ASSERT_TRUE(n > 0 && (size_t)n < cap);
}

/* DE_TEST_KEEP=1 keeps the reports for a look. */
static void crash_dir_remove(crash_dir *d) {
    const char *keep = getenv("DE_TEST_KEEP");
    if (keep != NULL && keep[0] == '1')
        return;
    DIR *dir = opendir(d->path);
    if (dir == NULL)
        return;
    struct dirent *e;
    char p[1024];
    while ((e = readdir(dir)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        snprintf(p, sizeof p, "%s/%s", d->path, e->d_name);
        (void)unlink(p);
    }
    closedir(dir);
    (void)rmdir(d->path);
}

/* A report is `crash-<t>-<pid>[-n].txt`: no dot before ".txt" (unlike `<stem>.test.txt`). */
static bool is_report_name(const char *name) {
    size_t n = strlen(name);
    if (strncmp(name, "crash-", 6) != 0 || n < 10 || strcmp(name + n - 4, ".txt") != 0)
        return false;
    return memchr(name, '.', n - 4) == NULL;
}

/* Finds the one report in `d`; *count gets the number of reports. Returns false if none. */
static bool find_report(const crash_dir *d, char *path, size_t cap, int *count) {
    DIR *dir = opendir(d->path);
    *count = 0;
    if (dir == NULL)
        return false;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        if (!is_report_name(e->d_name))
            continue;
        if (*count == 0)
            snprintf(path, cap, "%s/%s", d->path, e->d_name);
        (*count)++;
    }
    closedir(dir);
    return *count > 0;
}

static char g_file_buf[256 * 1024];

static const char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    size_t n = fread(g_file_buf, 1, sizeof g_file_buf - 1, f);
    fclose(f);
    g_file_buf[n] = '\0';
    return g_file_buf;
}

static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* Runs fn(dir) in a child with stderr silenced (DE_TEST_VERBOSE=1 keeps it) and returns the
 * waitpid status. */
static int run_child(void (*fn)(const char *dir), const char *dir) {
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0)
        FAIL("fork failed");
    if (pid == 0) {
        const char *verbose = getenv("DE_TEST_VERBOSE");
        if (verbose == NULL || verbose[0] != '1') {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0)
                (void)dup2(devnull, 2);
        }
        fn(dir);
        _exit(99); /* fn should not return */
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
    }
    return status;
}

static void expect_signal(int status, int sig) {
    ASSERT_TRUE(WIFSIGNALED(status));
    EXPECT_EQ(WTERMSIG(status), sig);
}

/* Lines between "backtrace: N frames" and "end of backtrace". */
static int count_backtrace_frames(const char *report) {
    const char *b = strstr(report, "backtrace: ");
    const char *e = strstr(report, "end of backtrace");
    if (b == NULL || e == NULL || e < b)
        return -1;
    const char *p = strchr(b, '\n');
    int n = 0;
    while (p != NULL && p + 1 < e) {
        n++;
        p = strchr(p + 1, '\n');
    }
    return n;
}

/* UBSan's null check would report this store and exit before it faults: let it fault. */
#if defined(__clang__)
__attribute__((noinline, no_sanitize("null")))
#elif defined(__GNUC__)
__attribute__((noinline, no_sanitize_undefined))
#endif
static void crash_null(void) {
    volatile int *volatile p = NULL;
    *p = 42;
}

/* ---------- Children ---------- */

static void child_segv(const char *dir) {
    if (de_crash_install(dir, NULL, NULL) != DE_OK)
        _exit(2);
    de_crash_set_annotation("level", "e1m1");
    de_crash_set_annotation("multi", "line one\nline two");
    crash_null();
}

static void child_panic(const char *dir) {
    if (de_crash_install(dir, NULL, NULL) != DE_OK)
        _exit(2);
    DE_PANIC("boom %d", 7);
}

static void chain_handler(const de_crash_context *ctx, void *user) {
    const char *line = (const char *)user;
    de_crash_write(ctx, line, strlen(line));
}

/* Checks the paths it gets and writes `<stem>.test.txt`, with open/write only. */
static void stem_handler(const de_crash_context *ctx, void *user) {
    (void)user;
    char want[DE__CRASH_PATH_CAP];
    want[0] = '\0';
    de__crash_str_append(want, sizeof want, ctx->report_stem);
    de__crash_str_append(want, sizeof want, ".txt");
    bool ok = strcmp(want, ctx->report_path) == 0 && ctx->kind == DE_CRASH_SIGNAL &&
              ctx->code == (u32)SIGSEGV && strcmp(ctx->reason, "SIGSEGV") == 0 &&
              ctx->panic_message == NULL && ctx->report_fd >= 0 && ctx->build_id != NULL &&
              strcmp(ctx->build_id, de_build_id()) == 0 && ctx->dump_path == NULL;
    de_crash_write(ctx, ok ? "stem check: ok\n" : "stem check: FAILED\n", ok ? 15 : 19);
    de_crash_write(ctx, "stem: ", 6);
    de_crash_write(ctx, ctx->report_stem, strlen(ctx->report_stem));
    de_crash_write(ctx, "\n", 1);

    char sib[DE__CRASH_PATH_CAP];
    sib[0] = '\0';
    de__crash_str_append(sib, sizeof sib, ctx->report_stem);
    de__crash_str_append(sib, sizeof sib, ".test.txt");
    int fd = open(sib, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        (void)!write(fd, "sibling\n", 8);
        close(fd);
    }
}

static void child_chain(const char *dir) {
    if (de_crash_install(dir, NULL, NULL) != DE_OK)
        _exit(2);
    (void)de_crash_add_handler(chain_handler, (void *)"handler 1\n");
    (void)de_crash_add_handler(chain_handler, (void *)"handler 2\n");
    (void)de_crash_add_handler(stem_handler, NULL);
    (void)de_crash_add_handler(chain_handler, (void *)"handler 3\n");
    crash_null();
}

/* ---------- Fork tests ---------- */

TEST(crash, segv_report) {
    crash_dir d;
    crash_dir_make(&d);
    int status = run_child(child_segv, d.path);
    expect_signal(status, SIGSEGV);

    char path[1024];
    int count = 0;
    ASSERT_TRUE(find_report(&d, path, sizeof path, &count));
    EXPECT_EQ(count, 1);
    const char *r = read_file(path);
    ASSERT_TRUE(r != NULL);
    EXPECT_TRUE(strstr(r, "DEngine crash report") != NULL);
    EXPECT_TRUE(strstr(r, "reason: SIGSEGV") != NULL);
    EXPECT_TRUE(strstr(r, "fault_address: 0x0\n") != NULL);
    EXPECT_TRUE(strstr(r, "  level = e1m1\n") != NULL);
    EXPECT_TRUE(strstr(r, "  multi = line one line two\n") != NULL); /* kept on one line */
    char bid[128];
    snprintf(bid, sizeof bid, "build_id: %s\n", de_build_id());
    EXPECT_TRUE(strstr(r, bid) != NULL);
    int frames = count_backtrace_frames(r);
    EXPECT_GE(frames, 3);
#if defined(__APPLE__)
    /* macOS walks the trace from the faulting context (DENG-365): it starts at the faulting
     * function, even though crash_null is a leaf without a frame record. Linux frames are
     * addresses only (no -rdynamic), so the name is checked on Apple only. */
    EXPECT_TRUE(strstr(r, "crash_null") != NULL);
#endif
    EXPECT_TRUE(strstr(r, "end of core report") != NULL);

    /* The pending marker: `<stem>.pending` with report=<path>. */
    char marker[1100];
    snprintf(marker, sizeof marker, "%.*s.pending", (int)(strlen(path) - 4), path);
    ASSERT_TRUE(file_exists(marker));
    char want[1100];
    snprintf(want, sizeof want, "report=%s\n", path);
    const char *m = read_file(marker);
    ASSERT_TRUE(m != NULL);
    EXPECT_TRUE(strncmp(m, want, strlen(want)) == 0);
    EXPECT_TRUE(strstr(m, "reason=SIGSEGV\n") != NULL);
    snprintf(want, sizeof want, "build_id=%s\n", de_build_id());
    EXPECT_TRUE(strstr(m, want) != NULL);
    crash_dir_remove(&d);
}

TEST(crash, panic_report) {
    crash_dir d;
    crash_dir_make(&d);
    int status = run_child(child_panic, d.path);
    expect_signal(status, SIGABRT);

    char path[1024];
    int count = 0;
    ASSERT_TRUE(find_report(&d, path, sizeof path, &count));
    EXPECT_EQ(count, 1); /* the abort after the panic does not write a second report */
    const char *r = read_file(path);
    ASSERT_TRUE(r != NULL);
    EXPECT_TRUE(strstr(r, "reason: panic\n") != NULL);
    EXPECT_TRUE(strstr(r, "panic: boom 7\n") != NULL);
    EXPECT_TRUE(strstr(r, "panic_location: ") != NULL);
    EXPECT_TRUE(strstr(r, "child_panic") != NULL);
    EXPECT_GE(count_backtrace_frames(r), 3);
    crash_dir_remove(&d);
}

TEST(crash, handler_chain_order) {
    crash_dir d;
    crash_dir_make(&d);
    expect_signal(run_child(child_chain, d.path), SIGSEGV);
    char path[1024];
    int count = 0;
    ASSERT_TRUE(find_report(&d, path, sizeof path, &count));
    const char *r = read_file(path);
    ASSERT_TRUE(r != NULL);
    const char *core = strstr(r, "end of core report");
    const char *h1 = strstr(r, "handler 1\n");
    const char *h2 = strstr(r, "handler 2\n");
    const char *hs = strstr(r, "stem check: ");
    const char *h3 = strstr(r, "handler 3\n");
    ASSERT_TRUE(core && h1 && h2 && hs && h3);
    EXPECT_TRUE(core < h1 && h1 < h2 && h2 < hs && hs < h3);
    crash_dir_remove(&d);
}

TEST(crash, handler_report_stem) {
    crash_dir d;
    crash_dir_make(&d);
    expect_signal(run_child(child_chain, d.path), SIGSEGV);
    char path[1024];
    int count = 0;
    ASSERT_TRUE(find_report(&d, path, sizeof path, &count));
    const char *r = read_file(path);
    ASSERT_TRUE(r != NULL);
    EXPECT_TRUE(strstr(r, "stem check: ok\n") != NULL);

    char stem[1024];
    snprintf(stem, sizeof stem, "%.*s", (int)(strlen(path) - 4), path);
    char want[1100];
    snprintf(want, sizeof want, "stem: %s\n", stem);
    EXPECT_TRUE(strstr(r, want) != NULL);

    char sib[1100];
    snprintf(sib, sizeof sib, "%s.test.txt", stem);
    const char *s = read_file(sib);
    ASSERT_TRUE(s != NULL);
    EXPECT_STREQ(s, "sibling\n");
    crash_dir_remove(&d);
}

static int g_upload_cookie;
static void test_upload(const de_crash_report *report, void *user) {
    (void)report;
    (void)user;
}

static char g_second_dir[512];

static void child_reinstall(const char *dir) {
    char sub[600];
    snprintf(sub, sizeof sub, "%s/first", dir);
    if (de_crash_install(sub, NULL, NULL) != DE_OK)
        _exit(2);
    struct sigaction before, after;
    sigaction(SIGSEGV, NULL, &before);

    /* Second install: new directory (created, with parents) and upload callback only. */
    if (de_crash_install(g_second_dir, test_upload, &g_upload_cookie) != DE_OK)
        _exit(3);
    sigaction(SIGSEGV, NULL, &after);
    void *user = NULL;
    bool ok = before.sa_sigaction == after.sa_sigaction && before.sa_flags == after.sa_flags &&
              strcmp(de__crash_dump_dir(), g_second_dir) == 0 &&
              de__crash_upload(&user) == test_upload && user == &g_upload_cookie;

    /* A third install with a too-long path fails and changes nothing. */
    static char too_long[DE__CRASH_DIR_MAX + 10];
    memset(too_long, 'x', sizeof too_long - 1);
    ok = ok && de_crash_install(too_long, NULL, NULL) == DE_ERR_INVALID_ARGUMENT &&
         strcmp(de__crash_dump_dir(), g_second_dir) == 0 && de__crash_upload(NULL) == test_upload;

    de_crash_set_annotation("check", ok ? "ok" : "FAILED");
    crash_null();
}

TEST(crash, reinstall_idempotent) {
    crash_dir d;
    crash_dir_make(&d);
    path_join(g_second_dir, sizeof g_second_dir, d.path, "/second/nested");
    expect_signal(run_child(child_reinstall, d.path), SIGSEGV);

    crash_dir first, second;
    path_join(first.path, sizeof first.path, d.path, "/first");
    snprintf(second.path, sizeof second.path, "%s", g_second_dir);
    char path[1024];
    int count = 0;
    EXPECT_FALSE(find_report(&first, path, sizeof path, &count));
    ASSERT_TRUE(find_report(&second, path, sizeof path, &count));
    EXPECT_EQ(count, 1);
    const char *r = read_file(path);
    ASSERT_TRUE(r != NULL);
    EXPECT_TRUE(strstr(r, "  check = ok\n") != NULL);

    crash_dir_remove(&second);
    path_join(second.path, sizeof second.path, d.path, "/second");
    crash_dir_remove(&second);
    crash_dir_remove(&first);
    crash_dir_remove(&d);
}

/* The whole signal path, from the fault to the last chained handler, under the alloc guard:
 * the last handler ends the guard and writes the counts into the report. */
static de_alloc_guard g_guard;

static void guard_end_handler(const de_crash_context *ctx, void *user) {
    (void)user;
    de_alloc_guard_end(&g_guard);
    char num[24];
    size_t n = de__crash_fmt_u64(num, de_alloc_guard_offences(&g_guard), 10);
    de_crash_write(ctx, "alloc offences: ", 16);
    de_crash_write(ctx, num, n);
    de_crash_write(ctx, "\n", 1);
}

static void child_no_malloc(const char *dir) {
    if (de_crash_install(dir, NULL, NULL) != DE_OK)
        _exit(2);
    (void)de_crash_add_handler(chain_handler, (void *)"handler 1\n");
    (void)de_crash_add_handler(guard_end_handler, NULL);
    de_crash_set_annotation("level", "e1m1");
    de_alloc_guard_begin(&g_guard);
    crash_null();
}

TEST(crash, handler_no_malloc) {
    if (!de_alloc_guard_counts_raw())
        SKIP("this build does not count raw malloc calls (Windows, ASan, TSan)");
    crash_dir d;
    crash_dir_make(&d);
    expect_signal(run_child(child_no_malloc, d.path), SIGSEGV);
    char path[1024];
    int count = 0;
    ASSERT_TRUE(find_report(&d, path, sizeof path, &count));
    const char *r = read_file(path);
    ASSERT_TRUE(r != NULL);
    EXPECT_TRUE(strstr(r, "alloc offences: 0\n") != NULL);
    crash_dir_remove(&d);
}

/* Unbounded recursion; the volatile buffer keeps every frame (no tail call, no folding). The
 * never-set flag only keeps the compiler from proving that it never ends. */
static volatile int g_recursion_stop;

static int recurse_forever(int depth) {
    if (g_recursion_stop)
        return depth;
    volatile char pad[512];
    pad[0] = (char)depth;
    pad[511] = (char)(depth + 1);
    return recurse_forever(depth + 1) + pad[0] + pad[511];
}

static void child_stack_overflow(const char *dir) {
    if (de_crash_install(dir, NULL, NULL) != DE_OK)
        _exit(2);
    (void)recurse_forever(0);
}

static i32 overflow_thread(void *user) {
    (void)user;
    de_crash_thread_init();
    de_crash_thread_init(); /* twice is harmless */
    return recurse_forever(0);
}

static void child_thread_overflow(const char *dir) {
    if (de_crash_install(dir, NULL, NULL) != DE_OK)
        _exit(2);
    de_thread *t = NULL;
    if (de_thread_create(overflow_thread, NULL, "crash-worker", DE_THREAD_NORMAL, 256 * 1024, &t) !=
        DE_OK)
        _exit(3);
    (void)de_thread_join(t);
    _exit(4);
}

/* A stack overflow faults with SIGSEGV (Linux, macOS) or SIGBUS (some macOS versions). */
static void expect_overflow_report(const crash_dir *d, int status, const char *thread_line) {
    ASSERT_TRUE(WIFSIGNALED(status));
    int sig = WTERMSIG(status);
    EXPECT_TRUE(sig == SIGSEGV || sig == SIGBUS);
    char path[1024];
    int count = 0;
    ASSERT_TRUE(find_report(d, path, sizeof path, &count));
    EXPECT_EQ(count, 1);
    const char *r = read_file(path);
    ASSERT_TRUE(r != NULL);
    EXPECT_TRUE(strstr(r, sig == SIGSEGV ? "reason: SIGSEGV" : "reason: SIGBUS") != NULL);
    EXPECT_TRUE(strstr(r, "end of core report") != NULL);
    if (thread_line != NULL)
        EXPECT_TRUE(strstr(r, thread_line) != NULL);
#if defined(__APPLE__)
    /* The context walk follows the overflowing thread's frame chain (main thread and worker
     * stacks have different bounds), so the recursion shows in the trace. */
    EXPECT_GE(count_backtrace_frames(r), 3);
    EXPECT_TRUE(strstr(r, "recurse_forever") != NULL);
#endif
}

TEST(crash, stack_overflow_main) {
#if DE__TEST_TSAN
    /* Under TSan on macOS the child recursed for minutes without reaching the end of the main
     * thread's stack; the worker-thread variant below covers the same path with a fixed stack. */
    SKIP("main-thread stack overflow is not reached under TSan");
#endif
    crash_dir d;
    crash_dir_make(&d);
    expect_overflow_report(&d, run_child(child_stack_overflow, d.path), NULL);
    crash_dir_remove(&d);
}

TEST(crash, stack_overflow_thread) {
    crash_dir d;
    crash_dir_make(&d);
    expect_overflow_report(&d, run_child(child_thread_overflow, d.path), "thread: crash-worker (");
    crash_dir_remove(&d);
}

#endif /* DE__CRASH_FORK_TESTS */
