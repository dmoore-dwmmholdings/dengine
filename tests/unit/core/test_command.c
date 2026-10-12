/* test_command.c: the console command registry (SPEC §18.6, DENG-96). CTest names:
 * command.<test>. Own executable: the registry and the cvars are process-wide, and CTest runs
 * each test in its own process.
 *
 * exec files are written into DE_COMMAND_TEST_TMP_DIR (build tree), mounted as `cmdtest:/`. */
#include "de_test.h"

#include <dengine/de_base.h>
#include <dengine/de_debug.h>
#include <dengine/de_platform.h>

#include "core/command_internal.h"

#include <stdio.h>
#include <string.h>

#ifndef DE_COMMAND_TEST_TMP_DIR
#error "DE_COMMAND_TEST_TMP_DIR must be defined by tests/unit/core/CMakeLists.txt"
#endif

/* ---- A capturing command ----------------------------------------------------------------- */

#define CAP_CALLS 16
#define CAP_ARGS  8

typedef struct capture {
    int calls;
    int argc[CAP_CALLS];
    char argv[CAP_CALLS][CAP_ARGS][64];
    i64 cvar_at_call[CAP_CALLS]; /* value of `watch` when the command ran */
    de_cvar watch;
    de_result result; /* what the command returns */
} capture;

static de_result capture_fn(int argc, const char *const *argv, void *user) {
    capture *c = (capture *)user;
    if (c->calls < CAP_CALLS) {
        int i = c->calls;
        c->argc[i] = argc;
        for (int a = 0; a < argc && a < CAP_ARGS; ++a)
            snprintf(c->argv[i][a], sizeof c->argv[i][a], "%s", argv[a]);
        c->cvar_at_call[i] = DE_HANDLE_VALID(c->watch) ? de_cvar_int(c->watch) : 0;
    }
    c->calls++;
    if (c->result != DE_OK)
        return DE_FAIL(c->result, "capture: asked to fail");
    return DE_OK;
}

/* ---- Log capture (for the built-ins' output) --------------------------------------------- */

static char g_log[16384];
static size_t g_log_len;

static void log_sink(const de_log_record *rec, void *user) {
    (void)user;
    const char *cat = de_log_category_name(rec->category);
    int n = snprintf(g_log + g_log_len, sizeof g_log - g_log_len, "[%s] %.*s\n", cat,
                     (int)rec->message.len, rec->message.ptr);
    if (n > 0 && (size_t)n < sizeof g_log - g_log_len)
        g_log_len += (size_t)n;
}

static void log_begin(void) {
    g_log_len = 0;
    g_log[0] = '\0';
    de_log_desc d;
    memset(&d, 0, sizeof d);
    d.no_console_sink = true;
    ASSERT_EQ(de_log_init(&d), DE_OK);
    de_log_add_sink(log_sink, NULL);
}

static void log_end(void) {
    de_log_flush();
    de_log_shutdown();
}

/* ---- Files for exec ---------------------------------------------------------------------- */

static void mount_tmp(void) {
    static bool mounted;
    if (!mounted) {
        ASSERT_EQ(de_fs_mount("cmdtest", DE_COMMAND_TEST_TMP_DIR, DE_MOUNT_WRITABLE, 0), DE_OK);
        mounted = true;
    }
}

static void write_file(const char *vpath, const char *text) {
    mount_tmp();
    ASSERT_EQ(de_file_write_all_atomic(vpath, text, strlen(text)), DE_OK);
}

/* ---- Tests ------------------------------------------------------------------------------- */

TEST(command, tokenize) {
    char line[256];
    char *argv[8];
    int argc = 0;

    snprintf(line, sizeof line, "%s", "echo \"a b\" c");
    ASSERT_EQ(de__command_tokenize(line, 8, argv, &argc), DE_OK);
    ASSERT_EQ(argc, 3);
    EXPECT_STREQ(argv[0], "echo");
    EXPECT_STREQ(argv[1], "a b");
    EXPECT_STREQ(argv[2], "c");

    /* Escaped quotes and backslashes, inside and outside quotes; other backslashes stay. */
    snprintf(line, sizeof line, "%s", "  say \"he said \\\"hi\\\"\"\t\\\"x\\\\y C:\\dir  ");
    ASSERT_EQ(de__command_tokenize(line, 8, argv, &argc), DE_OK);
    ASSERT_EQ(argc, 4);
    EXPECT_STREQ(argv[0], "say");
    EXPECT_STREQ(argv[1], "he said \"hi\"");
    EXPECT_STREQ(argv[2], "\"x\\y");
    EXPECT_STREQ(argv[3], "C:\\dir");

    /* An empty quoted word, and quotes in the middle of a word. */
    snprintf(line, sizeof line, "%s", "set \"\" a\"b c\"d");
    ASSERT_EQ(de__command_tokenize(line, 8, argv, &argc), DE_OK);
    ASSERT_EQ(argc, 3);
    EXPECT_STREQ(argv[1], "");
    EXPECT_STREQ(argv[2], "ab cd");

    /* Blank lines give no words. */
    snprintf(line, sizeof line, "%s", " \t \r\n");
    ASSERT_EQ(de__command_tokenize(line, 8, argv, &argc), DE_OK);
    EXPECT_EQ(argc, 0);

    /* Unclosed quote, too many words. */
    snprintf(line, sizeof line, "%s", "echo \"open");
    EXPECT_EQ(de__command_tokenize(line, 8, argv, &argc), DE_ERR_INVALID_ARGUMENT);
    snprintf(line, sizeof line, "%s", "a b c d");
    EXPECT_EQ(de__command_tokenize(line, 3, argv, &argc), DE_ERR_INVALID_ARGUMENT);

    /* Through de_command_exec: a capturing `echo` sees argv [echo, a b, c]. */
    capture cap;
    memset(&cap, 0, sizeof cap);
    ASSERT_EQ(de_command_unregister("echo"), DE_OK);
    ASSERT_EQ(de_command_register("echo", "capturing echo", capture_fn, &cap), DE_OK);
    ASSERT_EQ(de_command_exec("echo \"a b\" c"), DE_OK);
    ASSERT_EQ(cap.calls, 1);
    ASSERT_EQ(cap.argc[0], 3);
    EXPECT_STREQ(cap.argv[0][0], "echo");
    EXPECT_STREQ(cap.argv[0][1], "a b");
    EXPECT_STREQ(cap.argv[0][2], "c");

    /* Empty lines and comments do nothing; an unclosed quote fails before the command runs. */
    EXPECT_EQ(de_command_exec(""), DE_OK);
    EXPECT_EQ(de_command_exec("   "), DE_OK);
    EXPECT_EQ(de_command_exec("# echo x"), DE_OK);
    EXPECT_EQ(de_command_exec("echo \"x"), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_command_exec(NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(cap.calls, 1);

    /* Line and word limits. */
    static char long_line[DE_COMMAND_LINE_MAX + 2];
    memset(long_line, 'x', sizeof long_line - 1);
    long_line[sizeof long_line - 1] = '\0';
    EXPECT_EQ(de_command_exec(long_line), DE_ERR_INVALID_ARGUMENT);
    char many[DE_COMMAND_MAX_ARGS * 2 + 16];
    size_t n = 0;
    for (int i = 0; i <= DE_COMMAND_MAX_ARGS; ++i) {
        many[n++] = 'w';
        many[n++] = ' ';
    }
    many[n] = '\0';
    EXPECT_EQ(de_command_exec(many), DE_ERR_INVALID_ARGUMENT);
    de__command_reset_for_tests();
}

TEST(command, register_duplicate) {
    capture cap;
    memset(&cap, 0, sizeof cap);
    u32 builtins = de__command_count();
    EXPECT_EQ(builtins, 4u); /* help, cvars, echo, exec */

    ASSERT_EQ(de_command_register("god", "toggle god mode", capture_fn, &cap), DE_OK);
    EXPECT_EQ(de_command_register("god", NULL, capture_fn, &cap), DE_ERR_ALREADY_EXISTS);
    EXPECT_EQ(de_command_register("GOD", NULL, capture_fn, &cap), DE_ERR_ALREADY_EXISTS);
    EXPECT_EQ(de_command_register("help", NULL, capture_fn, &cap), DE_ERR_ALREADY_EXISTS);
    EXPECT_EQ(de__command_count(), builtins + 1u);

    /* Names are looked up without regard to case. */
    ASSERT_EQ(de_command_exec("GoD on"), DE_OK);
    ASSERT_EQ(cap.calls, 1);
    EXPECT_STREQ(cap.argv[0][0], "GoD"); /* argv[0] is the name as typed */
    EXPECT_STREQ(cap.argv[0][1], "on");

    /* Unknown names (no command, no cvar). */
    EXPECT_EQ(de_command_exec("no_such_command 1"), DE_ERR_NOT_FOUND);
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_TRUE(strstr(de_error_last()->message, "no_such_command") != NULL);

    /* Bad registrations. */
    EXPECT_EQ(de_command_register(NULL, NULL, capture_fn, &cap), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_command_register("", NULL, capture_fn, &cap), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_command_register("two words", NULL, capture_fn, &cap), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_command_register("quo\"te", NULL, capture_fn, &cap), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_command_register("#comment", NULL, capture_fn, &cap), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_command_register("ok", NULL, NULL, &cap), DE_ERR_INVALID_ARGUMENT);
    char long_name[DE_COMMAND_NAME_MAX + 2];
    memset(long_name, 'n', sizeof long_name - 1);
    long_name[sizeof long_name - 1] = '\0';
    EXPECT_EQ(de_command_register(long_name, NULL, capture_fn, &cap), DE_ERR_INVALID_ARGUMENT);
    long_name[DE_COMMAND_NAME_MAX] = '\0'; /* exactly the limit */
    EXPECT_EQ(de_command_register(long_name, NULL, capture_fn, &cap), DE_OK);

    /* Unregister (any case), then the name is free again. */
    EXPECT_EQ(de_command_unregister("GOD"), DE_OK);
    EXPECT_EQ(de_command_unregister("god"), DE_ERR_NOT_FOUND);
    EXPECT_EQ(de_command_exec("god"), DE_ERR_NOT_FOUND);
    EXPECT_EQ(de_command_register("god", NULL, capture_fn, &cap), DE_OK);

    /* A command's error comes back from de_command_exec, with its message. */
    cap.result = DE_ERR_UNSUPPORTED;
    EXPECT_EQ(de_command_exec("god"), DE_ERR_UNSUPPORTED);
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_STREQ(de_error_last()->message, "capture: asked to fail");
    de__command_reset_for_tests();
    EXPECT_EQ(de__command_count(), builtins);
}

TEST(command, cvar_passthrough) {
    de_cvar n = de_cvar_register(
        &(const de_cvar_desc){.name = "test.cmd.value", .type = DE_CVAR_INT, .default_number = 5});
    de_cvar s = de_cvar_register(&(const de_cvar_desc){
        .name = "test.cmd.name", .type = DE_CVAR_STRING, .default_string = "x"});
    de_cvar ro = de_cvar_register(&(const de_cvar_desc){
        .name = "test.cmd.locked", .type = DE_CVAR_BOOL, .flags = DE_CVAR_READONLY});
    ASSERT_TRUE(DE_HANDLE_VALID(n) && DE_HANDLE_VALID(s) && DE_HANDLE_VALID(ro));

    log_begin();
    ASSERT_EQ(de_command_exec("test.cmd.value 42"), DE_OK);
    EXPECT_EQ(de_cvar_int(n), 42);
    ASSERT_EQ(de_command_exec("TEST.CMD.VALUE 0x10"), DE_OK); /* names ignore case */
    EXPECT_EQ(de_cvar_int(n), 16);
    /* Several value words are joined with single spaces; quotes keep spacing. */
    ASSERT_EQ(de_command_exec("test.cmd.name hello   big world"), DE_OK);
    EXPECT_STREQ(de_cvar_string(s), "hello big world");
    ASSERT_EQ(de_command_exec("test.cmd.name \"a  b\""), DE_OK);
    EXPECT_STREQ(de_cvar_string(s), "a  b");
    /* Without a value: logs the value, changes nothing. */
    ASSERT_EQ(de_command_exec("test.cmd.value"), DE_OK);
    EXPECT_EQ(de_cvar_int(n), 16);
    /* de_cvar_set's errors come back. */
    EXPECT_EQ(de_command_exec("test.cmd.value lots"), DE_ERR_PARSE);
    EXPECT_EQ(de_cvar_int(n), 16);
    EXPECT_EQ(de_command_exec("test.cmd.locked 1"), DE_ERR_UNSUPPORTED);
    /* A command with the same name wins over the cvar. */
    capture cap;
    memset(&cap, 0, sizeof cap);
    ASSERT_EQ(de_command_register("test.cmd.value", NULL, capture_fn, &cap), DE_OK);
    ASSERT_EQ(de_command_exec("test.cmd.value 99"), DE_OK);
    EXPECT_EQ(cap.calls, 1);
    EXPECT_EQ(de_cvar_int(n), 16);
    log_end();
    EXPECT_TRUE(strstr(g_log, "[console] test.cmd.value = 16\n") != NULL);
    de__command_reset_for_tests();
}

TEST(command, exec_file) {
    capture cap;
    memset(&cap, 0, sizeof cap);
    cap.watch = de_cvar_register(
        &(const de_cvar_desc){.name = "test.exec.n", .type = DE_CVAR_INT, .default_number = 0});
    de_cvar f =
        de_cvar_register(&(const de_cvar_desc){.name = "test.exec.f", .type = DE_CVAR_FLOAT});
    ASSERT_TRUE(DE_HANDLE_VALID(cap.watch) && DE_HANDLE_VALID(f));
    ASSERT_EQ(de_command_register("capture", NULL, capture_fn, &cap), DE_OK);

    write_file("cmdtest:/order.cfg", "# a comment line\n"
                                     "capture first\n"
                                     "test.exec.n 1\r\n"
                                     "   # an indented comment\n"
                                     "\n"
                                     "capture second \"two words\"\n"
                                     "test.exec.n 2\n"
                                     "test.exec.f 0.5\n"
                                     "capture third"); /* no newline at the end */
    log_begin();
    ASSERT_EQ(de_command_exec("exec cmdtest:/order.cfg"), DE_OK);
    ASSERT_EQ(cap.calls, 3);
    EXPECT_STREQ(cap.argv[0][1], "first");
    EXPECT_EQ(cap.cvar_at_call[0], 0);
    EXPECT_STREQ(cap.argv[1][1], "second");
    EXPECT_STREQ(cap.argv[1][2], "two words");
    EXPECT_EQ(cap.cvar_at_call[1], 1);
    EXPECT_STREQ(cap.argv[2][1], "third");
    EXPECT_EQ(cap.cvar_at_call[2], 2);
    EXPECT_FLOAT_EQ(de_cvar_float(f), 0.5, 0.0);

    /* A bad line is reported with its line number; the other lines still run. */
    cap.calls = 0;
    write_file("cmdtest:/bad.cfg", "capture a\nnot_a_command\ntest.exec.n 7\ncapture b\n");
    EXPECT_EQ(de_command_exec("exec cmdtest:/bad.cfg"), DE_ERR_NOT_FOUND);
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_TRUE(strstr(de_error_last()->message, "line 2") != NULL);
    EXPECT_EQ(cap.calls, 2);
    EXPECT_EQ(de_cvar_int(cap.watch), 7);

    /* Missing file, wrong usage, a file that executes itself (depth limit). */
    EXPECT_EQ(de_command_exec("exec cmdtest:/missing.cfg"), DE_ERR_NOT_FOUND);
    EXPECT_EQ(de_command_exec("exec"), DE_ERR_INVALID_ARGUMENT);
    write_file("cmdtest:/self.cfg", "exec cmdtest:/self.cfg\n");
    EXPECT_EQ(de_command_exec("exec cmdtest:/self.cfg"), DE_ERR_INVALID_ARGUMENT);
    log_end();
    EXPECT_TRUE(strstr(g_log, "nested more than") != NULL);
    de__command_reset_for_tests();
}

static int g_hook_calls;

static void count_hook(void *user) {
    (void)user;
    g_hook_calls++;
}

TEST(command, exec_hook_once_per_line) {
    de_cvar n = de_cvar_register(&(const de_cvar_desc){.name = "test.hook.n", .type = DE_CVAR_INT});
    ASSERT_TRUE(DE_HANDLE_VALID(n));
    de__command_set_exec_hook(count_hook, NULL);
    g_hook_calls = 0;
    ASSERT_EQ(de_command_exec("test.hook.n 3"), DE_OK);
    EXPECT_EQ(g_hook_calls, 1);
    EXPECT_EQ(de_command_exec("nope"), DE_ERR_NOT_FOUND); /* failures count too */
    EXPECT_EQ(g_hook_calls, 2);
    write_file("cmdtest:/hook.cfg", "test.hook.n 1\ntest.hook.n 2\ntest.hook.n 3\n");
    ASSERT_EQ(de_command_exec("exec cmdtest:/hook.cfg"), DE_OK);
    EXPECT_EQ(g_hook_calls, 3); /* once for the whole file */
    de__command_set_exec_hook(NULL, NULL);
    ASSERT_EQ(de_command_exec("test.hook.n 4"), DE_OK);
    EXPECT_EQ(g_hook_calls, 3);
    de__command_reset_for_tests();
}

TEST(command, builtins_log) {
    de_cvar v = de_cvar_register(&(const de_cvar_desc){.name = "test.builtin.speed",
                                                       .help = "how fast",
                                                       .type = DE_CVAR_FLOAT,
                                                       .default_number = 2.5});
    ASSERT_TRUE(DE_HANDLE_VALID(v));
    log_begin();
    EXPECT_EQ(de_command_exec("echo hello \"big world\""), DE_OK);
    EXPECT_EQ(de_command_exec("help ex"), DE_OK);
    EXPECT_EQ(de_command_exec("cvars test.builtin"), DE_OK);
    EXPECT_EQ(de_command_exec("HELP"), DE_OK);
    log_end();
    EXPECT_TRUE(strstr(g_log, "[console] hello big world\n") != NULL);
    EXPECT_TRUE(strstr(g_log, "exec <vpath>") != NULL);
    EXPECT_TRUE(strstr(g_log, "1 command starting with ex\n") != NULL);
    EXPECT_TRUE(strstr(g_log, "test.builtin.speed = 2.5  # how fast\n") != NULL);
    EXPECT_TRUE(strstr(g_log, "1 cvar starting with test.builtin\n") != NULL);
    EXPECT_TRUE(strstr(g_log, "4 commands\n") != NULL);
}

/* A command may register, run and unregister commands (no lock is held during the call). */
static de_result reentrant_fn(int argc, const char *const *argv, void *user) {
    (void)argc;
    (void)argv;
    capture *cap = (capture *)user;
    DE_TRY(de_command_register("inner", NULL, capture_fn, cap));
    DE_TRY(de_command_exec("inner x"));
    return de_command_unregister("inner");
}

TEST(command, reentrant) {
    capture cap;
    memset(&cap, 0, sizeof cap);
    ASSERT_EQ(de_command_register("outer", NULL, reentrant_fn, &cap), DE_OK);
    ASSERT_EQ(de_command_exec("outer"), DE_OK);
    EXPECT_EQ(cap.calls, 1);
    EXPECT_EQ(de_command_exec("inner"), DE_ERR_NOT_FOUND);
    de__command_reset_for_tests();
}
