/* Unit tests: thread-local error context, cause chains, truncation, panic hook (DENG-146).
 *
 * Threads are raw pthreads (Win32 threads on Windows): the engine's thread API (DENG-147) is not
 * a dependency of de_base. The panic tests fork on POSIX, so the abort happens in a child
 * process; on Windows the hook longjmps out instead. */
#include "de_test.h"

#include <dengine/de_base.h>

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <process.h>
#include <setjmp.h>
#if defined(_MSC_VER)
/* C4611: setjmp and C++ destructors. This is C; nothing to destroy. */
#pragma warning(disable : 4611)
#endif
#else
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

/* ---- Helpers ------------------------------------------------------------------------------- */

static int open_file_line;
static de_result open_file(const char *path) {
    open_file_line = __LINE__ + 1;
    return DE_FAIL(DE_ERR_IO, "open %s", path);
}

static size_t chain_length(const de_error *e) {
    size_t n = 0;
    for (; e != NULL && n <= 2u * DE_ERROR_CHAIN_MAX; e = e->cause)
        ++n;
    return n;
}

/* ---- Error context ------------------------------------------------------------------------- */

TEST(error, set_and_read) {
    de_error_clear();
    ASSERT_TRUE(de_error_last() == NULL);

    de_result r = open_file("save.dat");
    ASSERT_EQ(r, DE_ERR_IO);

    const de_error *e = de_error_last();
    ASSERT_TRUE(e != NULL);
    EXPECT_EQ(e->result, DE_ERR_IO);
    EXPECT_STREQ(e->message, "open save.dat");
    EXPECT_STREQ(e->file, __FILE__);
    EXPECT_EQ(e->line, (u32)open_file_line);
    EXPECT_STREQ(e->function, "open_file");
    EXPECT_TRUE(e->cause == NULL);

    /* A new DE_FAIL replaces the error and starts a new chain. */
    ASSERT_EQ(DE_FAIL(DE_ERR_PARSE, "bad token '%c' at %d", 'x', 7), DE_ERR_PARSE);
    e = de_error_last();
    ASSERT_TRUE(e != NULL);
    EXPECT_EQ(e->result, DE_ERR_PARSE);
    EXPECT_STREQ(e->message, "bad token 'x' at 7");
    EXPECT_TRUE(e->cause == NULL);
    de_error_clear();
}

static de_result try_chain(int fail) {
    DE_TRY(fail ? open_file("a") : DE_OK);
    return DE_OK;
}

TEST(error, try_propagates) {
    de_error_clear();
    EXPECT_EQ(try_chain(0), DE_OK);
    EXPECT_TRUE(de_error_last() == NULL);
    EXPECT_EQ(try_chain(1), DE_ERR_IO);
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_STREQ(de_error_last()->message, "open a");
    de_error_clear();
}

TEST(error, clear) {
    (void)DE_FAIL(DE_ERR_BUSY, "busy");
    ASSERT_TRUE(de_error_last() != NULL);
    de_error_clear();
    EXPECT_TRUE(de_error_last() == NULL);
    de_error_clear(); /* clearing twice is fine */
    EXPECT_TRUE(de_error_last() == NULL);

    /* A wrap after a clear has nothing to wrap. */
    (void)DE_FAIL_WRAP(DE_ERR_TIMEOUT, "outer");
    const de_error *e = de_error_last();
    ASSERT_TRUE(e != NULL);
    EXPECT_EQ(e->result, DE_ERR_TIMEOUT);
    EXPECT_TRUE(e->cause == NULL);
    de_error_clear();
}

/* ---- Threads ------------------------------------------------------------------------------- */

typedef struct thread_probe {
    int saw_error_at_start;
    const de_error *set_ptr;
    /* Full size, so the copy below can never truncate (GCC -Wformat-truncation, DENG-356). */
    char message[DE_ERROR_MESSAGE_MAX];
    int still_own_after;
} thread_probe;

static void thread_probe_body(thread_probe *p) {
    p->saw_error_at_start = de_error_last() != NULL;
    for (int i = 0; i < 100; ++i)
        (void)DE_FAIL(DE_ERR_NETWORK, "thread error %d", i);
    p->set_ptr = de_error_last();
    if (p->set_ptr != NULL)
        snprintf(p->message, sizeof p->message, "%s", p->set_ptr->message);
    p->still_own_after = p->set_ptr != NULL && p->set_ptr->result == DE_ERR_NETWORK;
}

#if defined(_WIN32)
static unsigned __stdcall thread_probe_main(void *arg) {
    thread_probe_body((thread_probe *)arg);
    return 0;
}
static void run_probe_thread(thread_probe *p) {
    HANDLE h = (HANDLE)_beginthreadex(NULL, 0, thread_probe_main, p, 0, NULL);
    ASSERT_TRUE(h != NULL);
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
}
#else
static void *thread_probe_main(void *arg) {
    thread_probe_body((thread_probe *)arg);
    return NULL;
}
static void run_probe_thread(thread_probe *p) {
    pthread_t t;
    ASSERT_EQ(pthread_create(&t, NULL, thread_probe_main, p), 0);
    ASSERT_EQ(pthread_join(t, NULL), 0);
}
#endif

TEST(error, thread_isolation) {
    de_error_clear();
    (void)DE_FAIL(DE_ERR_NOT_FOUND, "main thread error");
    const de_error *mine = de_error_last();
    ASSERT_TRUE(mine != NULL);

    thread_probe p;
    memset(&p, 0, sizeof p);
    run_probe_thread(&p);

    /* Thread B did not see A's error, and its own errors live in its own ring. */
    EXPECT_FALSE(p.saw_error_at_start);
    EXPECT_TRUE(p.still_own_after);
    EXPECT_STREQ(p.message, "thread error 99");
    EXPECT_TRUE(p.set_ptr != mine);

    /* A's error is untouched by B's 100 errors. */
    EXPECT_TRUE(de_error_last() == mine);
    EXPECT_EQ(de_error_last()->result, DE_ERR_NOT_FOUND);
    EXPECT_STREQ(de_error_last()->message, "main thread error");
    de_error_clear();
}

/* ---- Cause chains -------------------------------------------------------------------------- */

TEST(error, wrap_chain) {
    de_error_clear();

    /* Short chain: every link is kept, newest first. */
    (void)DE_FAIL(DE_ERR_IO, "read failed");
    (void)DE_FAIL_WRAP(DE_ERR_PARSE, "parsing %s", "level.toml");
    (void)DE_FAIL_WRAP(DE_ERR_NOT_FOUND, "loading level %d", 3);
    const de_error *e = de_error_last();
    ASSERT_EQ(chain_length(e), (size_t)3);
    EXPECT_EQ(e->result, DE_ERR_NOT_FOUND);
    EXPECT_STREQ(e->message, "loading level 3");
    EXPECT_EQ(e->cause->result, DE_ERR_PARSE);
    EXPECT_STREQ(e->cause->message, "parsing level.toml");
    EXPECT_EQ(e->cause->cause->result, DE_ERR_IO);
    EXPECT_STREQ(e->cause->cause->message, "read failed");
    EXPECT_TRUE(e->cause->cause->cause == NULL);

    /* 20 wraps on one root: the chain stops at DE_ERROR_CHAIN_MAX records, keeps the newest ones
     * and ends with NULL. */
    (void)DE_FAIL(DE_ERR_IO, "root");
    for (int i = 1; i <= 20; ++i)
        (void)DE_FAIL_WRAP(DE_ERR_INTERNAL, "wrap %d", i);
    e = de_error_last();
    ASSERT_EQ(chain_length(e), (size_t)DE_ERROR_CHAIN_MAX);
    int expect = 20;
    const de_error *oldest = NULL;
    for (const de_error *it = e; it != NULL; it = it->cause, --expect) {
        char want[32];
        snprintf(want, sizeof want, "wrap %d", expect);
        EXPECT_STREQ(it->message, want);
        oldest = it;
    }
    ASSERT_TRUE(oldest != NULL);
    EXPECT_TRUE(oldest->cause == NULL);

    /* The next wrap reuses the oldest record's slot. Quoting that record's message as an
     * argument must still work (the message is formatted before the slot is overwritten). */
    char quoted[DE_ERROR_MESSAGE_MAX];
    snprintf(quoted, sizeof quoted, "%s", oldest->message);
    (void)DE_FAIL_WRAP(DE_ERR_CANCELLED, "q: %s", oldest->message);
    char want[DE_ERROR_MESSAGE_MAX + 8];
    snprintf(want, sizeof want, "q: %s", quoted);
    EXPECT_STREQ(de_error_last()->message, want);
    EXPECT_EQ(chain_length(de_error_last()), (size_t)DE_ERROR_CHAIN_MAX);

    /* A wrap that quotes the last message (the common pattern). */
    (void)DE_FAIL(DE_ERR_IO, "disk full");
    (void)DE_FAIL_WRAP(DE_ERR_IO, "saving: %s", de_error_last()->message);
    EXPECT_STREQ(de_error_last()->message, "saving: disk full");
    EXPECT_STREQ(de_error_last()->cause->message, "disk full");

    /* A new DE_FAIL starts a fresh chain even when the ring is full. */
    (void)DE_FAIL(DE_ERR_BUSY, "fresh");
    EXPECT_EQ(chain_length(de_error_last()), (size_t)1);
    de_error_clear();
}

/* ---- Truncation ---------------------------------------------------------------------------- */

TEST(error, truncation) {
    static char big[2000];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = '\0';

    (void)DE_FAIL(DE_ERR_PARSE, "%s", big);
    const de_error *e = de_error_last();
    ASSERT_TRUE(e != NULL);
    EXPECT_EQ(strlen(e->message), (size_t)(DE_ERROR_MESSAGE_MAX - 1));
    EXPECT_EQ(e->message[DE_ERROR_MESSAGE_MAX - 1], '\0');
    EXPECT_EQ(e->message[0], 'x');
    EXPECT_EQ(e->message[DE_ERROR_MESSAGE_MAX - 2], 'x');

    /* Exactly 511 characters fit; 512 are cut to 511. */
    big[DE_ERROR_MESSAGE_MAX - 1] = '\0';
    (void)DE_FAIL(DE_ERR_PARSE, "%s", big);
    EXPECT_EQ(strlen(de_error_last()->message), (size_t)(DE_ERROR_MESSAGE_MAX - 1));
    big[DE_ERROR_MESSAGE_MAX - 1] = 'x';
    big[DE_ERROR_MESSAGE_MAX] = '\0';
    (void)DE_FAIL(DE_ERR_PARSE, "%s", big);
    EXPECT_EQ(strlen(de_error_last()->message), (size_t)(DE_ERROR_MESSAGE_MAX - 1));

    /* Wraps truncate the same way and keep the chain. */
    big[DE_ERROR_MESSAGE_MAX] = 'x';
    (void)DE_FAIL_WRAP(DE_ERR_PARSE, "ctx %s", big);
    EXPECT_EQ(strlen(de_error_last()->message), (size_t)(DE_ERROR_MESSAGE_MAX - 1));
    EXPECT_TRUE(de_error_last()->cause != NULL);
    de_error_clear();
}

/* ---- Panic --------------------------------------------------------------------------------- */

/* Panics when `really` is set. Returns the line of its DE_PANIC, for the checks. */
static int do_panic(int really) {
    int line = __LINE__ + 2;
    if (really)
        DE_PANIC("boom %d %s", 42, "here");
    return line;
}

#if !defined(_WIN32)

enum { PANIC_ABORT_EXIT = 42 };

static int panic_hook_fd = -1;
static int panic_hook_calls;

/* Writes one record per call: "<call>|<message>|<function>|<line>;". */
static void record_hook(const de_panic_info *info, void *user) {
    char buf[1400];
    int n = snprintf(buf, sizeof buf, "%d|%s|%s|%u;", ++panic_hook_calls, info->message,
                     info->function, (unsigned)info->line);
    if (n > 0 && write(panic_hook_fd, buf, (size_t)n) < 0)
        _exit(3);
    if (user != NULL) /* recursion test: panic again from inside the hook */
        DE_PANIC("panic from hook");
}

static void abort_to_exit(int sig) {
    (void)sig;
    _exit(PANIC_ABORT_EXIT);
}

static void read_all(int fd, char *out, size_t cap) {
    size_t len = 0;
    for (;;) {
        ssize_t n = read(fd, out + len, cap - 1 - len);
        if (n <= 0)
            break;
        len += (size_t)n;
        if (len == cap - 1)
            break;
    }
    out[len] = '\0';
}

typedef struct panic_run {
    int exited;      /* the child exited through the SIGABRT handler */
    char hook[2048]; /* what the hook wrote */
    char err[2048];  /* what the child wrote to stderr */
} panic_run;

/* Forks a child that installs record_hook and panics. SIGABRT is turned into _exit(42), so the
 * test proves abort() ran without leaving a crash report behind. */
static void run_panic_child(int recurse, panic_run *out) {
    int hook_pipe[2], err_pipe[2];
    ASSERT_EQ(pipe(hook_pipe), 0);
    ASSERT_EQ(pipe(err_pipe), 0);
    fflush(NULL);
    pid_t pid = fork();
    ASSERT_TRUE(pid >= 0);
    if (pid == 0) {
        close(hook_pipe[0]);
        close(err_pipe[0]);
        dup2(err_pipe[1], 2);
        panic_hook_fd = hook_pipe[1];
        signal(SIGABRT, abort_to_exit);
        de_panic_set_hook(record_hook, recurse ? &panic_hook_fd : NULL);
        (void)do_panic(1);
        _exit(4); /* not reached */
    }
    close(hook_pipe[1]);
    close(err_pipe[1]);
    read_all(hook_pipe[0], out->hook, sizeof out->hook);
    read_all(err_pipe[0], out->err, sizeof out->err);
    close(hook_pipe[0]);
    close(err_pipe[0]);
    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    out->exited = WIFEXITED(status) && WEXITSTATUS(status) == PANIC_ABORT_EXIT;
}

TEST(error, panic_hook_called) {
    static panic_run run;
    run_panic_child(0, &run);
    EXPECT_TRUE(run.exited);

    /* Exactly one hook call, with the formatted message, function and line. */
    char want[256];
    snprintf(want, sizeof want, "1|boom 42 here|do_panic|%d;", do_panic(0));
    EXPECT_STREQ(run.hook, want);

    /* Then the message went to stderr with its location. */
    EXPECT_TRUE(strstr(run.err, "DE_PANIC at ") != NULL);
    EXPECT_TRUE(strstr(run.err, "test_error.c:") != NULL);
    EXPECT_TRUE(strstr(run.err, "(do_panic): boom 42 here") != NULL);
}

TEST(error, panic_recursion_aborts) {
    static panic_run run;
    run_panic_child(1, &run);
    EXPECT_TRUE(run.exited);

    /* The hook ran once; its own DE_PANIC aborted at once without calling it again. */
    char want[256];
    snprintf(want, sizeof want, "1|boom 42 here|do_panic|%d;", do_panic(0));
    EXPECT_STREQ(run.hook, want);
    EXPECT_TRUE(strstr(run.err, "panic while panicking") != NULL);
    EXPECT_TRUE(strstr(run.err, "boom 42 here") == NULL);
}

#else /* _WIN32: no fork. The hook longjmps out of the panic. */

static jmp_buf panic_jmp;
static int panic_hook_calls;
static char panic_message[256];

static void longjmp_hook(const de_panic_info *info, void *user) {
    (void)user;
    ++panic_hook_calls;
    snprintf(panic_message, sizeof panic_message, "%s|%s", info->message, info->function);
    longjmp(panic_jmp, 1);
}

/* After the longjmp this thread still counts as panicking, so a later DE_PANIC on it aborts at
 * once. CTest runs each test in its own process, so that does not leak into other tests. */
TEST(error, panic_hook_called) {
    de_panic_set_hook(longjmp_hook, NULL);
    if (setjmp(panic_jmp) == 0)
        (void)do_panic(1);
    de_panic_set_hook(NULL, NULL);
    EXPECT_EQ(panic_hook_calls, 1);
    EXPECT_STREQ(panic_message, "boom 42 here|do_panic");
}

TEST(error, panic_recursion_aborts) {
    SKIP("needs fork(); covered on POSIX hosts");
}

#endif
