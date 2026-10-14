/* test_bundle.c: the crash report bundle and its host wiring (SPEC §18.5, DENG-96). CTest names:
 * bundle.<test>; each runs in its own process.
 *
 * In-process: the log ring, the cvar snapshot, the next-launch marker scan (with hand-made
 * markers, so it runs on every OS) and the host annotations through de_host_run.
 * Fork (POSIX only): a child runs a headless game through de_host_run whose Update system crashes
 * in frame 3; the parent reads DENG-251's report and the bundle files the child left. Further
 * children play later launches that install an upload callback. DE_TEST_KEEP=1 keeps the
 * directories, DE_TEST_VERBOSE=1 keeps the children's stderr.
 *
 * Every directory is a fresh one (mkdtemp, or an emptied build-tree directory for the in-process
 * tests, see tmp_dir()), so nothing is written into the real user profile. */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* fopen */
#endif
#if defined(__linux__)
#define _GNU_SOURCE /* mkdtemp, sigaction under -std=c11 */
#elif defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif

#include "de_test.h"

#include <dengine/de_base.h>
#include <dengine/de_debug.h>
#include <dengine/de_ecs.h>
#include <dengine/de_game.h>
#include <dengine/de_platform.h>

#include "de_alloc_guard.h"
#include "host/host_crash.h"
#include "host/host_internal.h"
#include "platform/crash_internal.h"
#include "services/crash/bundle_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <wchar.h>
#include <windows.h>
#else
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define BUNDLE_FORK_TESTS 1
#endif

#ifndef DE_BUNDLE_TEST_TMP_DIR
#error "DE_BUNDLE_TEST_TMP_DIR must be defined by tests/unit/crash/CMakeLists.txt"
#endif

#if defined(DE_DEBUG)
#define EXPECTED_BUILD_TYPE "Debug"
#elif defined(DE_DEV)
#define EXPECTED_BUILD_TYPE "Dev"
#elif defined(DE_SHIPPING)
#define EXPECTED_BUILD_TYPE "Shipping"
#else
#define EXPECTED_BUILD_TYPE "Release"
#endif

/* ---- Files ------------------------------------------------------------------------------- */

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

static bool write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (f == NULL)
        return false;
    fputs(text, f);
    fclose(f);
    return true;
}

static bool file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return false;
    fclose(f);
    return true;
}

/* Deletes `path` and everything under it. Errors are ignored: a missing path is fine. */
#if defined(_WIN32)
static void rm_tree_w(const wchar_t *path) {
    DWORD attr = GetFileAttributesW(path);
    if (attr == INVALID_FILE_ATTRIBUTES)
        return;
    if ((attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        (void)DeleteFileW(path);
        return;
    }
    wchar_t p[1024];
    if (swprintf(p, 1024, L"%ls\\*", path) < 0)
        return;
    WIN32_FIND_DATAW e;
    HANDLE h = FindFirstFileW(p, &e);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(e.cFileName, L".") == 0 || wcscmp(e.cFileName, L"..") == 0)
                continue;
            if (swprintf(p, 1024, L"%ls\\%ls", path, e.cFileName) >= 0)
                rm_tree_w(p);
        } while (FindNextFileW(h, &e));
        FindClose(h);
    }
    (void)RemoveDirectoryW(path);
}

static void rm_tree(const char *path) {
    wchar_t w[1024];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1024) != 0)
        rm_tree_w(w);
}
#else
static void rm_tree(const char *path) {
    DIR *dir = opendir(path);
    if (dir == NULL) {
        (void)unlink(path);
        return;
    }
    struct dirent *e;
    char p[1024];
    while ((e = readdir(dir)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        snprintf(p, sizeof p, "%s/%s", path, e->d_name);
        rm_tree(p);
    }
    closedir(dir);
    (void)rmdir(path);
}
#endif

/* DE_TEST_KEEP=1 keeps the test directories for a look afterwards. */
static bool keep_dirs(void) {
    const char *keep = getenv("DE_TEST_KEEP");
    return keep != NULL && keep[0] == '1';
}

/* A fresh, empty build-tree directory for an in-process test. The pid keeps parallel test
 * processes apart, but CTest runs thousands of processes and the OS reuses pids, so a directory
 * of that name can already exist with an earlier run's files (stale `.pending` markers made
 * upload_marker_scan see extra reports, DENG-411). It is therefore deleted first. Two live
 * processes never share a pid, so nothing else uses the directory meanwhile. */
static void tmp_dir(char *out, size_t cap, const char *name) {
    snprintf(out, cap, "%s/%s_%llu", DE_BUNDLE_TEST_TMP_DIR, name,
             (unsigned long long)de__crash_os_pid());
    rm_tree(out);
    ASSERT_EQ(de__crash_os_mkdirs(out), DE_OK);
}

/* Deletes a tmp_dir() directory at the end of a test that passed its checks (DE_TEST_KEEP=1
 * keeps it). A failed ASSERT returns before this, so its files stay for a look. */
static void tmp_dir_remove(const char *dir) {
    if (!keep_dirs())
        rm_tree(dir);
}

/* ---- Log ring ---------------------------------------------------------------------------- */

static void log_start(void) {
    de_log_desc d;
    memset(&d, 0, sizeof d);
    d.no_console_sink = true;
    d.thread_buffer_bytes = 1u << 20; /* the wrap test logs more than the default 64 KiB at once */
    ASSERT_EQ(de_log_init(&d), DE_OK);
}

TEST(bundle, log_ring_wraps) {
    log_start();
    de__crash_log_ring_reset_for_tests();
    de_log_add_sink(de__crash_log_ring_sink, NULL);
    de_log_category cat = de_log_register_category("ringtest", DE_LOG_INFO);

    for (int i = 0; i < 10; ++i)
        DE_INFO(cat, "msg-%05d|", i);
    de_log_flush();
    static char out[DE__CRASH_LOG_RING_BYTES + 16];
    size_t len = de__crash_log_ring_copy(out, sizeof out);
    ASSERT_GT(len, 0u);
    /* `<seconds>.<ms> [info][ringtest][<thread>] test_bundle.c:<line> msg-00000|` */
    EXPECT_TRUE(strstr(out, " [info][ringtest][") != NULL);
    EXPECT_TRUE(strstr(out, "] test_bundle.c:") != NULL);
    const char *p0 = strstr(out, "msg-00000|\n");
    const char *p9 = strstr(out, "msg-00009|\n");
    EXPECT_TRUE(p0 != NULL && p9 != NULL && p0 < p9);
    EXPECT_EQ((u64)len, de__crash_log_ring_written());

    /* Well past the ring size: the newest lines stay, whole and in order. */
    const int total = 4000;
    for (int i = 10; i < total; ++i)
        DE_INFO(cat, "msg-%05d| padding padding padding padding", i);
    de_log_flush();
    EXPECT_GT(de__crash_log_ring_written(), (u64)DE__CRASH_LOG_RING_BYTES * 2u);
    len = de__crash_log_ring_copy(out, sizeof out);
    EXPECT_LE(len, (size_t)DE__CRASH_LOG_RING_BYTES);
    EXPECT_GT(len, (size_t)DE__CRASH_LOG_RING_BYTES - 200u); /* at most one line skipped */
    EXPECT_TRUE(out[0] >= '0' && out[0] <= '9');             /* starts with a whole line */
    EXPECT_EQ(out[len - 1], '\n');
    EXPECT_TRUE(strstr(out, "msg-00000|") == NULL);
    char last[32];
    snprintf(last, sizeof last, "msg-%05d|", total - 1);
    EXPECT_TRUE(strstr(out, last) != NULL);
    /* Consecutive numbers from the first line on. */
    const char *m = strstr(out, "msg-");
    ASSERT_TRUE(m != NULL);
    int expect = atoi(m + 4);
    EXPECT_GT(expect, 10);
    int lines = 0;
    for (; m != NULL; m = strstr(m + 1, "msg-")) {
        EXPECT_EQ(atoi(m + 4), expect);
        ++expect;
        ++lines;
    }
    EXPECT_EQ(expect, total);
    EXPECT_GT(lines, 500);

    /* A small buffer keeps the newest bytes. */
    char small[64];
    size_t sl = de__crash_log_ring_copy(small, sizeof small);
    EXPECT_EQ(sl, sizeof small - 1);
    EXPECT_EQ(memcmp(small, out + len - sl, sl), 0);

    /* The handler's writer gives the same bytes. */
    char dir[512], path[600];
    tmp_dir(dir, sizeof dir, "ring");
    snprintf(path, sizeof path, "%s/ring.txt", dir);
    remove(path);
    bool exists = false;
    intptr_t fd = de__crash_os_create(path, &exists);
    ASSERT_TRUE(fd != -1);
    de__crash_log_ring_write_fd(fd);
    de__crash_os_close(fd);
    const char *text = read_file(path);
    ASSERT_TRUE(text != NULL);
    EXPECT_EQ(strlen(text), len);
    EXPECT_EQ(memcmp(text, out, len), 0);
    remove(path);
    tmp_dir_remove(dir);

    de_log_remove_sink(de__crash_log_ring_sink, NULL);
    de_log_shutdown();
}

/* ---- Cvar snapshot ----------------------------------------------------------------------- */

static bool snapshot_has(const char *line) {
    size_t len = 0;
    const char *s = de__crash_cvar_snapshot_current(&len);
    static char copy[DE__CRASH_SNAPSHOT_BYTES + 1];
    memcpy(copy, s, len);
    copy[len] = '\0';
    return strstr(copy, line) != NULL;
}

TEST(bundle, cvar_snapshot_refresh) {
    static const char *const modes[] = {"low", "high"};
    de__crash_cvar_snapshot_reset_for_tests();
    de_cvar b = de_cvar_register(
        &(const de_cvar_desc){.name = "snap.flag", .type = DE_CVAR_BOOL, .default_number = 1});
    de_cvar i = de_cvar_register(
        &(const de_cvar_desc){.name = "snap.count", .type = DE_CVAR_INT, .default_number = -12});
    de_cvar f = de_cvar_register(
        &(const de_cvar_desc){.name = "snap.ratio", .type = DE_CVAR_FLOAT, .default_number = 0.1});
    de_cvar w = de_cvar_register(
        &(const de_cvar_desc){.name = "snap.whole", .type = DE_CVAR_FLOAT, .default_number = 3});
    de_cvar s = de_cvar_register(&(const de_cvar_desc){
        .name = "snap.title", .type = DE_CVAR_STRING, .default_string = "say \"hi\"\\\n\x01"});
    de_cvar e = de_cvar_register(&(const de_cvar_desc){.name = "snap.mode",
                                                       .type = DE_CVAR_ENUM,
                                                       .enum_values = modes,
                                                       .enum_count = 2,
                                                       .default_string = "high"});
    ASSERT_TRUE(DE_HANDLE_VALID(b) && DE_HANDLE_VALID(i) && DE_HANDLE_VALID(f) &&
                DE_HANDLE_VALID(w) && DE_HANDLE_VALID(s) && DE_HANDLE_VALID(e));

    ASSERT_TRUE(de__crash_cvar_snapshot_refresh(true));
    EXPECT_EQ(de__crash_cvar_snapshot_count(), 1u);
    EXPECT_TRUE(snapshot_has("# DEngine cvar snapshot"));
    EXPECT_TRUE(snapshot_has("\ncheats = false\n"));
    EXPECT_TRUE(snapshot_has("\nsnap.flag = true\n"));
    EXPECT_TRUE(snapshot_has("\nsnap.count = -12\n"));
    EXPECT_TRUE(snapshot_has("\nsnap.ratio = 0.1\n"));
    EXPECT_TRUE(snapshot_has("\nsnap.whole = 3.0\n"));
    EXPECT_TRUE(snapshot_has("\nsnap.title = \"say \\\"hi\\\"\\\\\\n\\u0001\"\n"));
    EXPECT_TRUE(snapshot_has("\nsnap.mode = \"high\"\n"));

    /* The snapshot is valid TOML that reads back as the same values. */
    size_t len = 0;
    const char *text = de__crash_cvar_snapshot_current(&len);
    EXPECT_EQ(de_cvar_load_from_memory(text, len), DE_OK);
    EXPECT_EQ(de_cvar_int(i), -12);
    EXPECT_STREQ(de_cvar_string(s), "say \"hi\"\\\n\x01");

    /* Nothing changed: no new snapshot, and the check allocates nothing. */
    de_alloc_guard g;
    memset(&g, 0, sizeof g);
    g.this_thread_only = true;
    de_alloc_guard_begin(&g);
    bool again = de__crash_cvar_snapshot_refresh(false);
    de_alloc_guard_end(&g);
    EXPECT_FALSE(again);
    DE_ALLOC_GUARD_ASSERT_ZERO(&g);
    EXPECT_EQ(de__crash_cvar_snapshot_count(), 1u);

    /* A change of each kind publishes a new snapshot. */
    ASSERT_EQ(de_cvar_set(i, "99"), DE_OK);
    EXPECT_TRUE(de__crash_cvar_snapshot_refresh(false));
    EXPECT_TRUE(snapshot_has("\nsnap.count = 99\n"));
    ASSERT_EQ(de_cvar_set(f, "-0.0"), DE_OK);
    EXPECT_TRUE(de__crash_cvar_snapshot_refresh(false));
    EXPECT_TRUE(snapshot_has("\nsnap.ratio = -0.0\n"));
    ASSERT_EQ(de_cvar_set(f, "1e300"), DE_OK);
    EXPECT_TRUE(de__crash_cvar_snapshot_refresh(false));
    EXPECT_TRUE(snapshot_has("\nsnap.ratio = 1e+300\n"));
    ASSERT_EQ(de_cvar_set(f, "inf"), DE_OK);
    EXPECT_TRUE(de__crash_cvar_snapshot_refresh(false));
    EXPECT_TRUE(snapshot_has("\nsnap.ratio = inf\n"));
    ASSERT_EQ(de_cvar_set(s, "plain"), DE_OK);
    EXPECT_TRUE(de__crash_cvar_snapshot_refresh(false));
    EXPECT_TRUE(snapshot_has("\nsnap.title = \"plain\"\n"));
    ASSERT_EQ(de_cvar_set(e, "low"), DE_OK);
    EXPECT_TRUE(de__crash_cvar_snapshot_refresh(false));
    EXPECT_TRUE(snapshot_has("\nsnap.mode = \"low\"\n"));
    de_cvar late =
        de_cvar_register(&(const de_cvar_desc){.name = "snap.late", .type = DE_CVAR_INT});
    ASSERT_TRUE(DE_HANDLE_VALID(late));
    EXPECT_TRUE(de__crash_cvar_snapshot_refresh(false)); /* a new cvar counts as a change */
    EXPECT_TRUE(snapshot_has("\nsnap.late = 0\n"));

    /* After every top-level de_command_exec (the bundle's exec hook). */
    ASSERT_EQ(de__crash_bundle_install(), DE_OK);
    u64 before = de__crash_cvar_snapshot_count();
    ASSERT_EQ(de_command_exec("snap.count 1234"), DE_OK);
    EXPECT_EQ(de__crash_cvar_snapshot_count(), before + 1u);
    EXPECT_TRUE(snapshot_has("\nsnap.count = 1234\n"));
    /* The frame tick refreshes at most once per interval. */
    ASSERT_EQ(de_cvar_set(i, "5"), DE_OK);
    de__crash_bundle_tick(DE__CRASH_SNAPSHOT_INTERVAL_NS * 10u);
    EXPECT_TRUE(snapshot_has("\nsnap.count = 5\n"));
    ASSERT_EQ(de_cvar_set(i, "6"), DE_OK);
    de__crash_bundle_tick(DE__CRASH_SNAPSHOT_INTERVAL_NS * 10u + 1u);
    EXPECT_TRUE(snapshot_has("\nsnap.count = 5\n")); /* too soon */
    de__crash_bundle_tick(DE__CRASH_SNAPSHOT_INTERVAL_NS * 11u);
    EXPECT_TRUE(snapshot_has("\nsnap.count = 6\n"));

    /* More than fits: the rest is counted in a closing comment. */
    char name[64], value[260];
    memset(value, 'v', 250);
    value[250] = '\0';
    for (int k = 0; k < 200; ++k) {
        snprintf(name, sizeof name, "snap.big.s%03d", k);
        de_cvar c = de_cvar_register(
            &(const de_cvar_desc){.name = name, .type = DE_CVAR_STRING, .default_string = value});
        ASSERT_TRUE(DE_HANDLE_VALID(c));
    }
    EXPECT_TRUE(de__crash_cvar_snapshot_refresh(false));
    text = de__crash_cvar_snapshot_current(&len);
    EXPECT_LE(len, (size_t)DE__CRASH_SNAPSHOT_BYTES);
    EXPECT_GT(len, (size_t)DE__CRASH_SNAPSHOT_BYTES - 400u);
    EXPECT_TRUE(snapshot_has(" more cvars not listed: the snapshot is full\n"));
    EXPECT_EQ(text[len - 1], '\n');
}

/* ---- Next-launch upload: hand-made markers (every OS) ------------------------------------ */

typedef struct upload_rec {
    int calls;
    char dump[4][600], log[4][600], build[4][64];
} upload_rec;

static void record_upload(const de_crash_report *r, void *user) {
    upload_rec *u = (upload_rec *)user;
    if (u->calls < 4) {
        snprintf(u->dump[u->calls], sizeof u->dump[0], "%s", r->dump_path);
        snprintf(u->log[u->calls], sizeof u->log[0], "%s", r->log_path);
        snprintf(u->build[u->calls], sizeof u->build[0], "%s", r->build_id);
    }
    u->calls++;
}

static int find_build(const upload_rec *u, const char *build) {
    for (int k = 0; k < u->calls && k < 4; ++k)
        if (strcmp(u->build[k], build) == 0)
            return k;
    return -1;
}

TEST(bundle, upload_marker_scan) {
    char dir[512], p[700], text[1600];
    tmp_dir(dir, sizeof dir, "upload");
    char sep = de__crash_os_separator();

    /* Report 1: report, marker and log tail. */
    snprintf(p, sizeof p, "%s%ccrash-100-1.txt", dir, sep);
    ASSERT_TRUE(write_text(p, "DEngine crash report\n"));
    char report1[700];
    snprintf(report1, sizeof report1, "%s", p);
    snprintf(p, sizeof p, "%s%ccrash-100-1.log_tail.txt", dir, sep);
    ASSERT_TRUE(write_text(p, "a log line\n"));
    char tail1[700];
    snprintf(tail1, sizeof tail1, "%s", p);
    snprintf(text, sizeof text, "report=%s\nbuild_id=build-one\nreason=SIGSEGV\nfuture_key=x\n",
             report1);
    snprintf(p, sizeof p, "%s%ccrash-100-1.pending", dir, sep);
    ASSERT_TRUE(write_text(p, text));

    /* Report 2: a minidump, no log tail, CRLF lines. */
    char report2[700], dump2[700];
    snprintf(report2, sizeof report2, "%s%ccrash-200-2.txt", dir, sep);
    snprintf(dump2, sizeof dump2, "%s%ccrash-200-2.dmp", dir, sep);
    ASSERT_TRUE(write_text(report2, "DEngine crash report\n"));
    snprintf(text, sizeof text, "report=%s\r\ndump=%s\r\nbuild_id=build-two\r\nreason=panic\r\n",
             report2, dump2);
    snprintf(p, sizeof p, "%s%ccrash-200-2.pending", dir, sep);
    ASSERT_TRUE(write_text(p, text));

    /* Not markers: another prefix, a directory with the suffix. */
    snprintf(p, sizeof p, "%s%cnotes.pending", dir, sep);
    ASSERT_TRUE(write_text(p, "report=nothing\n"));
    snprintf(p, sizeof p, "%s%ccrash-300-3.pending", dir, sep);
    ASSERT_EQ(de__crash_os_mkdirs(p), DE_OK);

    upload_rec u;
    memset(&u, 0, sizeof u);
    ASSERT_EQ(de_crash_install(dir, record_upload, &u), DE_OK);
    ASSERT_EQ(u.calls, 2);
    int k1 = find_build(&u, "build-one");
    int k2 = find_build(&u, "build-two");
    ASSERT_TRUE(k1 >= 0 && k2 >= 0);
    EXPECT_STREQ(u.dump[k1], report1); /* no minidump: the text report */
    EXPECT_STREQ(u.log[k1], tail1);
    EXPECT_STREQ(u.dump[k2], dump2);
    EXPECT_STREQ(u.log[k2], report2); /* no log tail: the text report */

    /* Retired: `.pending` became `.sent`, and the next launch hands over nothing. */
    snprintf(p, sizeof p, "%s%ccrash-100-1.pending", dir, sep);
    EXPECT_FALSE(file_exists(p));
    snprintf(p, sizeof p, "%s%ccrash-100-1.sent", dir, sep);
    EXPECT_TRUE(file_exists(p));
    snprintf(p, sizeof p, "%s%cnotes.pending", dir, sep);
    EXPECT_TRUE(file_exists(p));
    EXPECT_EQ(de__crash_upload_pending(), 0u);
    ASSERT_EQ(de_crash_install(dir, record_upload, &u), DE_OK);
    EXPECT_EQ(u.calls, 2);

    /* A marker that cannot be read still hands over <stem>.txt. */
    snprintf(p, sizeof p, "%s%ccrash-400-4.pending", dir, sep);
    ASSERT_TRUE(write_text(p, ""));
    EXPECT_EQ(de__crash_upload_pending(), 1u);
    ASSERT_EQ(u.calls, 3);
    char want[700];
    snprintf(want, sizeof want, "%s%ccrash-400-4.txt", dir, sep);
    EXPECT_STREQ(u.dump[2], want);
    EXPECT_STREQ(u.build[2], "unknown");

    /* Without an upload callback nothing is scanned. */
    snprintf(p, sizeof p, "%s%ccrash-500-5.pending", dir, sep);
    ASSERT_TRUE(write_text(p, "report=x\n"));
    ASSERT_EQ(de_crash_install(dir, NULL, NULL), DE_OK);
    EXPECT_TRUE(file_exists(p));
    tmp_dir_remove(dir);
}

/* ---- The game the host tests run ---------------------------------------------------------- */

typedef struct bundle_game {
    de_engine *engine;
    de_log_category log;
    int update_runs;
    int crash_at;      /* Update run that crashes; -1 = never */
    int quit_at;       /* Update run that executes `quit`; -1 = never */
    bool guard;        /* begin the alloc guard right before the crash */
    bool crash_active; /* de__host_crash_active() during the game */
    de_result reload_result;
    char annot[6][DE_CRASH_ANNOTATION_VALUE_MAX];
} bundle_game;

static bundle_game g_game;
static de_alloc_guard g_guard;
static const char *const k_annot_keys[6] = {"host.game",   "host.game_library", "host.headless",
                                            "host.timing", "host.build_type",   "host.frame"};

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

/* The last chained handler: ends the guard and writes the counts into the report. */
static void guard_end_handler(const de_crash_context *ctx, void *user) {
    (void)user;
    de_alloc_guard_end(&g_guard);
    char num[24];
    size_t n = de__crash_fmt_u64(num, de_alloc_guard_offences(&g_guard), 10);
    de_crash_write(ctx, "alloc offences: ", 16);
    de_crash_write(ctx, num, n);
    de_crash_write(ctx, "\n", 1);
}

static void game_update(de_system_ctx *ctx) {
    (void)ctx;
    int run = g_game.update_runs++;
    g_game.crash_active = de__host_crash_active();
    for (int k = 0; k < 6; ++k)
        if (!de__crash_annotation_get(k_annot_keys[k], g_game.annot[k], sizeof g_game.annot[k]))
            g_game.annot[k][0] = '\0';
    if (run == g_game.quit_at) {
        /* `reload` exists and does nothing for a static game (hot reload is off). */
        g_game.reload_result = de_command_exec("reload");
        (void)de_command_exec("quit");
    }
    if (run == g_game.crash_at) {
        DE_INFO(g_game.log, "last line before the crash: update %d", run);
        de_log_flush();
        if (g_game.guard) {
            memset(&g_guard, 0, sizeof g_guard);
            g_guard.this_thread_only = true;
            de_alloc_guard_begin(&g_guard);
        }
        crash_null();
    }
}

static de_result game_init(de_engine *e) {
    g_game.engine = e;
    g_game.log = de_log_register_category("game", DE_LOG_INFO);
    de_cvar lv = de_cvar_register(&(const de_cvar_desc){
        .name = "test.bundle.level", .type = DE_CVAR_INT, .default_number = 1});
    if (!DE_HANDLE_VALID(lv))
        return DE_FAIL(DE_ERR_INTERNAL, "test.bundle.level");
    DE_TRY(de_command_exec("test.bundle.level 7"));
    if (g_game.guard)
        DE_TRY(de_crash_add_handler(guard_end_handler, NULL)); /* after the bundle's handler */
    return DE_OK;
}

static void game_shutdown(de_engine *e) {
    (void)e;
}

static void game_register_types(de_engine *e, de_type_registry *types) {
    (void)e;
    (void)types;
}

static void game_register_systems(de_engine *e, de_world *w) {
    (void)e;
    de_system_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.name = "bundle.update";
    sd.schedule = DE_SCHEDULE_UPDATE;
    sd.fn = game_update;
    sd.flags = DE_SYSTEM_MAIN_THREAD;
    (void)de_system_add(w, &sd);
}

static de_game_api bundle_api(void) {
    de_game_api api;
    memset(&api, 0, sizeof api);
    api.api_version = DE_GAME_API_VERSION;
    api.name = "bundle_game";
    api.init = game_init;
    api.shutdown = game_shutdown;
    api.register_types = game_register_types;
    api.register_systems = game_register_systems;
    return api;
}

/* de_host_desc.argv is `char **`: copies string literals into writable static storage. */
static char **make_argv(int argc, const char *const *src) {
    static char buf[8][64];
    static char *argv[9];
    for (int k = 0; k < argc && k < 8; ++k) {
        snprintf(buf[k], sizeof buf[k], "%s", src[k]);
        argv[k] = buf[k];
    }
    argv[argc < 8 ? argc : 8] = NULL;
    return argv;
}

/* Runs the game headless with `root` as game:/ and `<root>/user` as user:/ (so the crash
 * reports go to `<root>/user/crashes`). Returns de_host_run's exit code. */
static int run_game(const char *root, int argc, char **argv) {
    char user[600], cache[600];
    snprintf(user, sizeof user, "%s/user", root);
    snprintf(cache, sizeof cache, "%s/cache", root);
    (void)de__crash_os_mkdirs(user);
    (void)de__crash_os_mkdirs(cache);
    de_fs_desc fd;
    memset(&fd, 0, sizeof fd);
    fd.game_dir = root;
    fd.user_dir = user;
    fd.cache_dir = cache;
    if (de_fs_init(&fd) != DE_OK)
        return 90;
    de_game_api api = bundle_api();
    de_host_desc d;
    memset(&d, 0, sizeof d);
    d.static_api = &api;
    d.headless = true;
    d.game_toml = "game:/no_game.toml"; /* missing: the defaults */
    d.argc = argc;
    d.argv = argv;
    int code = de_host_run(&d);
    de_fs_shutdown();
    return code;
}

/* ---- Host annotations and `quit` (in-process) ------------------------------------------- */

TEST(bundle, host_annotations_present) {
    char root[512];
    tmp_dir(root, sizeof root, "annot");
    log_start();
    memset(&g_game, 0, sizeof g_game);
    g_game.crash_at = -1;
    g_game.quit_at = 2;
    static const char *const args[] = {"test", "--frames", "50"};
    EXPECT_EQ(run_game(root, 3, make_argv(3, args)), 0);
    de_log_shutdown();

    /* `quit` in Update of frame 2 ends the run after that frame. */
    EXPECT_EQ(g_game.update_runs, 3);
    EXPECT_EQ(g_game.reload_result, DE_OK);
    EXPECT_TRUE(g_game.crash_active);
    EXPECT_STREQ(g_game.annot[0], "bundle_game");
    EXPECT_STREQ(g_game.annot[1], "static");
    EXPECT_STREQ(g_game.annot[2], "true");
    EXPECT_STREQ(g_game.annot[3], "fixed 60 Hz");
    EXPECT_STREQ(g_game.annot[4], EXPECTED_BUILD_TYPE);
    EXPECT_STREQ(g_game.annot[5], "2"); /* read in frame 2 */

    /* The reports go to user:/crashes, which exists now. */
    char crashes[600], got[600];
    snprintf(crashes, sizeof crashes, "%s/user/crashes", root);
    snprintf(got, sizeof got, "%s", de__crash_dump_dir());
    for (char *c = got; *c != '\0'; ++c) /* Windows may resolve with backslashes */
        if (*c == '\\')
            *c = '/';
    EXPECT_STREQ(got, crashes);
    EXPECT_TRUE(de__crash_bundle_installed());
    tmp_dir_remove(root);
}

TEST(bundle, crash_disabled_by_cvar) {
    char root[512];
    tmp_dir(root, sizeof root, "disabled");
    log_start();
    memset(&g_game, 0, sizeof g_game);
    g_game.crash_at = -1;
    g_game.quit_at = 0;
    static const char *const args[] = {"test", "+crash.enabled", "false", "--frames", "50"};
    EXPECT_EQ(run_game(root, 5, make_argv(5, args)), 0);
    de_log_shutdown();
    EXPECT_EQ(g_game.update_runs, 1); /* `quit` works without crash handling */
    EXPECT_FALSE(g_game.crash_active);
    EXPECT_STREQ(g_game.annot[0], "");
    EXPECT_STREQ(de__crash_dump_dir(), "");
    tmp_dir_remove(root);
}

/* ---- Fork tests (POSIX) ----------------------------------------------------------------- */

#if defined(BUNDLE_FORK_TESTS)

typedef struct crash_dir {
    char path[512];
} crash_dir;

static void crash_dir_make(crash_dir *d) {
    const char *tmp = getenv("TMPDIR");
    if (tmp == NULL || tmp[0] == '\0')
        tmp = "/tmp";
    snprintf(d->path, sizeof d->path, "%s/de_bundle_XXXXXX", tmp);
    ASSERT_TRUE(mkdtemp(d->path) != NULL);
}

static void crash_dir_remove(crash_dir *d) {
    if (!keep_dirs())
        rm_tree(d->path);
}

/* A report is `crash-<t>-<pid>[-n].txt`: no dot before ".txt". Finds the newest name (reports
 * of later crashes sort after earlier ones within one test, see the stems). */
static int list_reports(const char *dir, char names[][128], int cap) {
    DIR *d = opendir(dir);
    int n = 0;
    if (d == NULL)
        return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (strncmp(e->d_name, "crash-", 6) != 0 || len < 10 || len >= 128 ||
            strcmp(e->d_name + len - 4, ".txt") != 0 || memchr(e->d_name, '.', len - 4) != NULL)
            continue;
        if (n < cap)
            memcpy(names[n], e->d_name, len + 1);
        n++;
    }
    closedir(d);
    return n;
}

/* The child died from the SIGSEGV, also under TSan (DENG-384). */
static bool child_crashed(int status) {
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV;
}

/* Prints what a child wrote to stderr (kept in `<dir>/child_stderr.txt`). */
static void print_child_stderr(const char *dir) {
    char path[600];
    snprintf(path, sizeof path, "%s/child_stderr.txt", dir);
    const char *text = read_file(path);
    fprintf(stderr, "---- child stderr (%s) ----\n%s---- end of child stderr ----\n", path,
            text != NULL ? text : "(none)\n");
    /* And what the crash path left: which files, and the report as far as it got. */
    char crashes[600];
    snprintf(crashes, sizeof crashes, "%s/user/crashes", dir);
    DIR *d = opendir(crashes);
    if (d == NULL)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        char file[1024]; /* the directory (< 600) and a name (< 256) */
        snprintf(file, sizeof file, "%s/%s", crashes, e->d_name);
        text = read_file(file);
        fprintf(stderr, "---- %s ----\n%s\n", e->d_name, text != NULL ? text : "(unreadable)");
    }
    closedir(d);
}

/* Runs fn(dir) in a child and returns the waitpid status. The child's stderr goes to
 * `<dir>/child_stderr.txt` (DE_TEST_VERBOSE=1: to the test's stderr). A child still running after
 * CHILD_TIMEOUT_S is killed (status: SIGKILL) and its stderr is printed. */
#define CHILD_TIMEOUT_S 30
static int run_child(void (*fn)(const char *dir), const char *dir) {
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0)
        FAIL("fork failed");
    if (pid == 0) {
        const char *verbose = getenv("DE_TEST_VERBOSE");
        if (verbose == NULL || verbose[0] != '1') {
            char path[600];
            snprintf(path, sizeof path, "%s/child_stderr.txt", dir);
            int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd >= 0)
                (void)dup2(fd, 2);
        }
        fn(dir);
        _exit(99); /* fn should not return */
    }
    int status = 0;
    for (int waited_ms = 0;; waited_ms += 10) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid)
            break;
        if (r < 0 && errno != EINTR)
            FAIL("waitpid failed");
        if (waited_ms >= CHILD_TIMEOUT_S * 1000) {
            (void)kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
            print_child_stderr(dir);
            FAIL("the child did not finish in time and was killed");
        }
        struct timespec ts = {0, 10 * 1000 * 1000};
        (void)nanosleep(&ts, NULL);
    }
    /* Anything but the expected crash or a clean launch: show what the child said. */
    if (!child_crashed(status) && !(WIFEXITED(status) && WEXITSTATUS(status) < 50))
        print_child_stderr(dir);
    return status;
}

static void child_crash(const char *dir) {
    de_log_desc ld;
    memset(&ld, 0, sizeof ld);
    if (de_log_init(&ld) != DE_OK)
        _exit(2);
    memset(&g_game, 0, sizeof g_game);
    g_game.crash_at = 3;
    g_game.quit_at = -1;
    static const char *const args[] = {"child", "--frames", "50"};
    (void)run_game(dir, 3, make_argv(3, args));
    _exit(3); /* the game should have crashed */
}

static void child_crash_guarded(const char *dir) {
    de_log_desc ld;
    memset(&ld, 0, sizeof ld);
    if (de_log_init(&ld) != DE_OK)
        _exit(2);
    memset(&g_game, 0, sizeof g_game);
    g_game.guard = true;
    g_game.crash_at = 3;
    g_game.quit_at = -1;
    static const char *const args[] = {"child", "--frames", "50"};
    (void)run_game(dir, 3, make_argv(3, args));
    _exit(3);
}

/* Paths of the one crash in `<root>/user/crashes`. */
typedef struct crash_files {
    char report[800], tail[800], cvars[800], marker[800];
} crash_files;

static bool find_crash(const char *root, crash_files *f) {
    char dir[600];
    snprintf(dir, sizeof dir, "%s/user/crashes", root);
    char names[4][128];
    if (list_reports(dir, names, 4) != 1)
        return false;
    size_t stem = strlen(names[0]) - 4;
    snprintf(f->report, sizeof f->report, "%s/%s", dir, names[0]);
    snprintf(f->tail, sizeof f->tail, "%s/%.*s.log_tail.txt", dir, (int)stem, names[0]);
    snprintf(f->cvars, sizeof f->cvars, "%s/%.*s.cvars.toml", dir, (int)stem, names[0]);
    snprintf(f->marker, sizeof f->marker, "%s/%.*s.pending", dir, (int)stem, names[0]);
    return true;
}

TEST(bundle, written_on_crash) {
    crash_dir d;
    crash_dir_make(&d);
    int status = run_child(child_crash, d.path);
    ASSERT_TRUE(child_crashed(status));

    crash_files f;
    ASSERT_TRUE(find_crash(d.path, &f));
    EXPECT_TRUE(file_exists(f.marker));

    /* The core report: DENG-251's header, the host annotations, the build ID, the bundle. */
    const char *r = read_file(f.report);
    ASSERT_TRUE(r != NULL);
    char line[900];
    snprintf(line, sizeof line, "\nbuild_id: %s\n", de_build_id());
    EXPECT_TRUE(strstr(r, line) != NULL);
    EXPECT_TRUE(strstr(r, "reason: SIGSEGV") != NULL);
    EXPECT_TRUE(strstr(r, "  host.game = bundle_game\n") != NULL);
    EXPECT_TRUE(strstr(r, "  host.frame = 3\n") != NULL);
    EXPECT_TRUE(strstr(r, "  host.headless = true\n") != NULL);
    EXPECT_TRUE(strstr(r, "  host.game_library = static\n") != NULL);
    EXPECT_TRUE(strstr(r, "  host.timing = fixed 60 Hz\n") != NULL);
    EXPECT_TRUE(strstr(r, "end of core report\n") != NULL);
    snprintf(line, sizeof line, "log_tail: %s\n", f.tail);
    EXPECT_TRUE(strstr(r, line) != NULL);
    snprintf(line, sizeof line, "cvars: %s\n", f.cvars);
    EXPECT_TRUE(strstr(r, line) != NULL);

    /* The log tail ends with the last line the game flushed. */
    const char *t = read_file(f.tail);
    ASSERT_TRUE(t != NULL);
    EXPECT_TRUE(strncmp(t, "# Log tail", 10) == 0);
    const char *last = strstr(t, "last line before the crash: update 3\n");
    EXPECT_TRUE(last != NULL);
    EXPECT_TRUE(last != NULL && last[strlen("last line before the crash: update 3\n")] == '\0');
    EXPECT_TRUE(strstr(t, "[info][game][") != NULL);

    /* The cvar dump has the current values, including one set through de_command_exec. */
    const char *c = read_file(f.cvars);
    ASSERT_TRUE(c != NULL);
    EXPECT_TRUE(strstr(c, "\ntest.bundle.level = 7\n") != NULL);
    EXPECT_TRUE(strstr(c, "\ncrash.enabled = true\n") != NULL);
    EXPECT_TRUE(strstr(c, "\ncrash.dir = \"user:/crashes\"\n") != NULL);
    crash_dir_remove(&d);
}

TEST(bundle, no_alloc_in_handler) {
    crash_dir d;
    crash_dir_make(&d);
    int status = run_child(child_crash_guarded, d.path);
    ASSERT_TRUE(child_crashed(status));
    crash_files f;
    ASSERT_TRUE(find_crash(d.path, &f));
    const char *r = read_file(f.report);
    ASSERT_TRUE(r != NULL);
    /* The guard ran from right before the fault to the last chained handler, which runs after
     * the bundle's (both files were written). Engine allocations are always counted; raw malloc
     * where de_alloc_guard_counts_raw() (macOS and Linux without ASan/TSan). */
    EXPECT_TRUE(strstr(r, "alloc offences: 0\n") != NULL);
    char line[900];
    snprintf(line, sizeof line, "log_tail: %s\n", f.tail);
    EXPECT_TRUE(strstr(r, line) != NULL);
    snprintf(line, sizeof line, "cvars: %s\n", f.cvars);
    EXPECT_TRUE(strstr(r, line) != NULL);
    EXPECT_TRUE(file_exists(f.tail));
    EXPECT_TRUE(file_exists(f.cvars));
    crash_dir_remove(&d);
}

/* A later launch: installs an upload callback into the same directory and exits with the number
 * of calls, plus 100 when a report's fields were wrong. */
static void child_launch(const char *dir) {
    char crashes[600];
    snprintf(crashes, sizeof crashes, "%s/user/crashes", dir);
    upload_rec u;
    memset(&u, 0, sizeof u);
    if (de_crash_install(crashes, record_upload, &u) != DE_OK)
        _exit(98);
    bool ok = true;
    for (int k = 0; k < u.calls && k < 4; ++k) {
        size_t dl = strlen(u.dump[k]), ll = strlen(u.log[k]);
        ok = ok && strcmp(u.build[k], de_build_id()) == 0 && dl > 4 &&
             strcmp(u.dump[k] + dl - 4, ".txt") == 0 && file_exists(u.dump[k]) && ll > 13 &&
             strcmp(u.log[k] + ll - 13, ".log_tail.txt") == 0 && file_exists(u.log[k]) &&
             strncmp(u.dump[k], u.log[k], dl - 4) == 0;
    }
    _exit(u.calls + (ok ? 0 : 100));
}

static int launch(const char *dir) {
    int status = run_child(child_launch, dir);
    if (!WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

TEST(bundle, upload_next_launch) {
    crash_dir d;
    crash_dir_make(&d);
    int status = run_child(child_crash, d.path);
    ASSERT_TRUE(child_crashed(status));

    EXPECT_EQ(launch(d.path), 1); /* the next launch hands the report over once */
    EXPECT_EQ(launch(d.path), 0); /* and never again */

    /* A second crash (a new stem: another pid): only it is handed over. */
    status = run_child(child_crash, d.path);
    ASSERT_TRUE(child_crashed(status));
    EXPECT_EQ(launch(d.path), 1);
    EXPECT_EQ(launch(d.path), 0);

    char crashes[600];
    snprintf(crashes, sizeof crashes, "%s/user/crashes", d.path);
    char names[4][128];
    EXPECT_EQ(list_reports(crashes, names, 4), 2);
    crash_dir_remove(&d);
}

#endif /* BUNDLE_FORK_TESTS */
