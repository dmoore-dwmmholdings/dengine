#if defined(__linux__)
#define _GNU_SOURCE
#elif defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include "de_test.h"
#include "de_alloc_guard.h"
#include "host/editor/editor_crash.h"
#include "de_editor_protocol.h"
#include <string.h>
#include <stdio.h>

#if !defined(_WIN32)
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static bool node_equal(mpack_node_t n, const char *s) {
    return mpack_node_strlen(n) == strlen(s) && memcmp(mpack_node_str(n), s, strlen(s)) == 0;
}

#if defined(__clang__)
__attribute__((noinline, no_sanitize("null")))
#elif defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__)
__attribute__((noinline, no_sanitize_undefined))
#endif
static void memory_fault(void) {
    volatile int *volatile pointer = NULL;
    *pointer = 42;
}

static de_alloc_guard guard;
static int result_fd;

static void guard_done(const de_crash_context *ctx, void *user) {
    (void)ctx;
    (void)user;
    de_alloc_guard_end(&guard);
    const u64 n = de_alloc_guard_offences(&guard);
    (void)write(result_fd, &n, sizeof n);
}

static void no_sigpipe(int fd) {
#ifdef SO_NOSIGPIPE
    const int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#else
    (void)fd;
#endif
}

/* Real installed crash chain, with Debug mpack tracking enabled in every serializer object. */
static void fatal_case(bool worker, bool panic) {
    if (!de_alloc_guard_counts_raw())
        SKIP("raw allocation interposition is unavailable");
    int sockets[2], counts[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
    ASSERT_EQ(pipe(counts), 0);
    char dir[] = "/tmp/deng701-crash-XXXXXX";
    ASSERT_TRUE(mkdtemp(dir) != NULL);
    const pid_t pid = fork();
    ASSERT_TRUE(pid >= 0);
    if (pid == 0) {
        close(sockets[0]);
        close(counts[0]);
        result_fd = counts[1];
        alarm(5);
        no_sigpipe(sockets[1]);
        de_ep_conn conn = {.fd = sockets[1], .max_frame = 4096};
        if (de_crash_install(dir, NULL, NULL) != DE_OK || de__ed_crash_register() != DE_OK ||
            !de__ed_crash_arm(&conn, 43) || de_crash_add_handler(guard_done, NULL) != DE_OK)
            _exit(2);
        /* Worker setup has its own TLS/alternate stack, outside the guarded fatal region. */
        if (worker) {
            extern void *de_test_editor_crash_worker(void *arg);
            pthread_t t;
            if (pthread_create(&t, NULL, de_test_editor_crash_worker, &panic) != 0)
                _exit(3);
            (void)pthread_join(t, NULL);
        } else {
            de_alloc_guard_begin(&guard);
            if (panic)
                DE_PANIC("DENG701 actual main panic");
            memory_fault();
        }
        _exit(4);
    }
    close(sockets[1]);
    close(counts[1]);
    de_ep_conn peer = {.fd = sockets[0], .max_frame = 4096};
    char bytes[4096];
    u8 kind = 0;
    size_t n = 0;
    EXPECT_EQ(de_ep_recv(&peer, bytes, sizeof bytes, &kind, &n, 5000), DE_EP_IO_OK);
    EXPECT_EQ(kind, DE_EP_KIND_NOTIFICATION);
    mpack_tree_t tree;
    mpack_tree_init_data(&tree, bytes, n);
    mpack_tree_parse(&tree);
    mpack_node_t root = mpack_tree_root(&tree);
    EXPECT_TRUE(node_equal(mpack_node_map_cstr(root, "topic"), "session.crashed"));
    EXPECT_EQ(mpack_node_u64(mpack_node_map_cstr(root, "seq")), 43u);
    mpack_node_t data = mpack_node_map_cstr(root, "data");
    mpack_node_t why = mpack_node_map_cstr(data, "reason");
    EXPECT_TRUE(node_equal(why, panic ? "DENG701 actual main panic" : "SIGSEGV") ||
                (worker && panic && node_equal(why, "DENG701 actual worker panic")));
    mpack_node_t tail = mpack_node_map_cstr(data, "logTail");
    char text[1800];
    mpack_node_copy_cstr(tail, text, sizeof text);
    EXPECT_TRUE(strstr(text, dir) != NULL);
    EXPECT_TRUE(strstr(text, "Crash report: ") != NULL);
    EXPECT_EQ(mpack_tree_destroy(&tree), mpack_ok);
    u64 offences = UINT64_MAX;
    EXPECT_EQ(read(counts[0], &offences, sizeof offences), (ssize_t)sizeof offences);
    EXPECT_EQ(offences, 0u);
    int status = 0;
    EXPECT_EQ(waitpid(pid, &status, 0), pid);
    EXPECT_TRUE(WIFSIGNALED(status));
    EXPECT_EQ(WTERMSIG(status), panic ? SIGABRT : SIGSEGV);
    close(sockets[0]);
    close(counts[0]);
    DIR *d = opendir(dir);
    struct dirent *e;
    bool report = false;
    while (d && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        if (strstr(e->d_name, ".txt") != NULL)
            report = true;
        char path[1200];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        (void)unlink(path);
    }
    if (d)
        closedir(d);
    EXPECT_TRUE(report);
    (void)rmdir(dir);
}

void *de_test_editor_crash_worker(void *arg) {
    const bool panic = *(bool *)arg;
    de_crash_thread_init();
    de_alloc_guard_begin(&guard);
    if (panic)
        DE_PANIC("DENG701 actual worker panic");
    memory_fault();
    return NULL;
}

TEST(editor_crash, actual_main_segv_no_heap) {
    fatal_case(false, false);
}
TEST(editor_crash, actual_worker_segv_no_heap) {
    fatal_case(true, false);
}
TEST(editor_crash, actual_main_panic_no_heap) {
    fatal_case(false, true);
}
TEST(editor_crash, actual_worker_panic_no_heap) {
    fatal_case(true, true);
}

TEST(editor_crash, full_socket_is_bounded_and_terminal) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
    pid_t pid = fork();
    ASSERT_TRUE(pid >= 0);
    if (pid == 0) {
        close(sockets[0]);
        alarm(2);
        static char fill[1024];
        const int flags = fcntl(sockets[1], F_GETFL);
        (void)fcntl(sockets[1], F_SETFL, flags | O_NONBLOCK);
        while (send(sockets[1], fill, sizeof fill, MSG_DONTWAIT) > 0) {
        }
        (void)fcntl(sockets[1], F_SETFL, flags);
        de_ep_conn conn = {.fd = sockets[1], .max_frame = 4096};
        if (!de__ed_crash_arm(&conn, 1))
            _exit(2);
        const de_crash_context c = {.reason = "full socket"};
        de__ed_crash_report(&c, NULL);
        /* A fatal attempt is terminal even if no bytes fit. */
        _exit(de__ed_crash_frozen() && !de__ed_crash_disarm() && !de__ed_crash_arm(&conn, 9) ? 0
                                                                                             : 3);
    }
    close(sockets[1]);
    int status;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    close(sockets[0]);
}

TEST(editor_crash, disarmed_handle_is_never_used) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
    pid_t pid = fork();
    ASSERT_TRUE(pid >= 0);
    if (pid == 0) {
        close(sockets[0]);
        de_ep_conn conn = {.fd = sockets[1], .max_frame = 4096};
        for (u32 i = 0; i < 100; ++i) {
            if (!de__ed_crash_arm(&conn, i) || !de__ed_crash_disarm())
                _exit(2);
        }
        /* Reuse the cached descriptor number for an unrelated connection after disarm. */
        close(sockets[1]);
        int replacement[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, replacement) != 0)
            _exit(3);
        int watched = replacement[0] == sockets[1] ? replacement[1] : replacement[0];
        if (replacement[0] != sockets[1] && replacement[1] != sockets[1]) {
            if (dup2(replacement[1], sockets[1]) < 0)
                _exit(4);
        }
        (void)fcntl(watched, F_SETFL, O_NONBLOCK);
        const de_crash_context c = {.reason = "not authenticated"};
        de__ed_crash_report(&c, NULL);
        char unexpected;
        const ssize_t n = read(watched, &unexpected, 1);
        _exit(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : 5);
    }
    close(sockets[1]);
    char c;
    EXPECT_EQ(read(sockets[0], &c, 1), 0);
    int status;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    close(sockets[0]);
}

TEST(editor_crash, closed_peer_cannot_replace_fatal_signal_with_sigpipe) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
    close(sockets[0]); /* peer is gone before the child inherits its sending descriptor */
    pid_t pid = fork();
    ASSERT_TRUE(pid >= 0);
    if (pid == 0) {
        alarm(2);
        no_sigpipe(sockets[1]);
        const de_ep_conn conn = {.fd = sockets[1], .max_frame = 4096};
        if (!de__ed_crash_arm(&conn, 1))
            _exit(2);
        const de_crash_context c = {.reason = "peer disconnected"};
        de__ed_crash_report(&c, NULL);
        _exit(de__ed_crash_frozen() ? 0 : 3);
    }
    close(sockets[1]);
    int status;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}

typedef struct busy_writer {
    de_ep_conn conn;
    de_ep_io result;
} busy_writer;

static char busy_bytes[1 << 20];
static void *write_busy(void *arg) {
    busy_writer *b = arg;
    b->result = de__ed_crash_send(&b->conn, DE_EP_KIND_BLOB, busy_bytes, sizeof busy_bytes, 99);
    return NULL;
}

TEST(editor_crash, interrupted_writer_is_never_waited_on_or_interleaved) {
    int sockets[2], ready[2], go[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
    ASSERT_EQ(pipe(ready), 0);
    ASSERT_EQ(pipe(go), 0);
    pid_t pid = fork();
    ASSERT_TRUE(pid >= 0);
    if (pid == 0) {
        close(sockets[0]);
        close(ready[0]);
        close(go[1]);
        alarm(5);
        const int capacity = 4096;
        (void)setsockopt(sockets[1], SOL_SOCKET, SO_SNDBUF, &capacity, sizeof capacity);
        busy_writer b = {.conn = {.fd = sockets[1], .max_frame = 2 << 20}};
        if (!de__ed_crash_arm(&b.conn, 7))
            _exit(2);
        pthread_t thread;
        if (pthread_create(&thread, NULL, write_busy, &b) != 0)
            _exit(3);
        /* The parent observes the ordinary frame header without draining the full socket,
         * so the writer demonstrably owns the gate before the synthetic fault starts. */
        char proceed;
        if (read(go[0], &proceed, 1) != 1 || de__ed_crash_disarm())
            _exit(6);
        const de_crash_context c = {.reason = "busy writer"};
        de__ed_crash_report(&c, NULL);
        if (de__ed_crash_frozen())
            _exit(4);
        (void)write(ready[1], "x", 1);
        (void)pthread_join(thread, NULL);
        if (b.result != DE_EP_IO_OK || !de__ed_crash_arm(&b.conn, 99))
            _exit(5);
        de__ed_crash_report(&c, NULL);
        _exit(0);
    }
    close(sockets[1]);
    close(ready[1]);
    close(go[0]);
    struct pollfd waiting = {.fd = sockets[0], .events = POLLIN};
    EXPECT_EQ(poll(&waiting, 1, 3000), 1);
    EXPECT_EQ(write(go[1], "x", 1), 1);
    char signal;
    EXPECT_EQ(read(ready[0], &signal, 1), 1);
    de_ep_conn peer = {.fd = sockets[0], .max_frame = 2 << 20};
    char *bytes = malloc(sizeof busy_bytes);
    ASSERT_TRUE(bytes != NULL);
    size_t n = 0;
    u8 kind = 0;
    EXPECT_EQ(de_ep_recv(&peer, bytes, sizeof busy_bytes, &kind, &n, 5000), DE_EP_IO_OK);
    EXPECT_EQ(kind, DE_EP_KIND_BLOB);
    EXPECT_EQ(n, sizeof busy_bytes);
    EXPECT_EQ(memcmp(bytes, busy_bytes, n), 0);
    EXPECT_EQ(de_ep_recv(&peer, bytes, sizeof busy_bytes, &kind, &n, 5000), DE_EP_IO_OK);
    EXPECT_EQ(kind, DE_EP_KIND_NOTIFICATION);
    mpack_tree_t tree;
    mpack_tree_init_data(&tree, bytes, n);
    mpack_tree_parse(&tree);
    EXPECT_EQ(mpack_node_u64(mpack_node_map_cstr(mpack_tree_root(&tree), "seq")), 99u);
    EXPECT_EQ(mpack_tree_destroy(&tree), mpack_ok);
    int status;
    EXPECT_EQ(waitpid(pid, &status, 0), pid);
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    free(bytes);
    close(ready[0]);
    close(go[1]);
    close(sockets[0]);
}

TEST(editor_crash, invalid_utf8_is_bounded_valid_and_first_only) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
    pid_t pid = fork();
    ASSERT_TRUE(pid >= 0);
    if (pid == 0) {
        close(sockets[0]);
        de_ep_conn conn = {.fd = sockets[1], .max_frame = 4096};
        if (!de__ed_crash_arm(&conn, 11))
            _exit(2);
        char reason[1024], path[4096];
        memset(reason, 'a', sizeof reason);
        reason[0] = (char)0xff;
        reason[1] = (char)0xed;
        reason[2] = (char)0xa0;
        reason[3] = (char)0x80;
        reason[sizeof reason - 1] = '\0';
        memset(path, 'b', sizeof path);
        path[sizeof path - 1] = '\0';
        const de_crash_context c = {.panic_message = reason, .report_path = path};
        de__ed_crash_report(&c, NULL);
        de__ed_crash_report(&c, NULL); /* must not append or overwrite the in-flight buffer */
        _exit(0);
    }
    close(sockets[1]);
    de_ep_conn peer = {.fd = sockets[0], .max_frame = 4096};
    char bytes[4096];
    u8 kind;
    size_t n;
    EXPECT_EQ(de_ep_recv(&peer, bytes, sizeof bytes, &kind, &n, 5000), DE_EP_IO_OK);
    mpack_tree_t tree;
    mpack_tree_init_data(&tree, bytes, n);
    mpack_tree_parse(&tree);
    mpack_node_t data = mpack_node_map_cstr(mpack_tree_root(&tree), "data");
    mpack_node_t reason = mpack_node_map_cstr(data, "reason");
    EXPECT_EQ(mpack_node_strlen(reason), 511u);
    EXPECT_TRUE(mpack_node_str(reason)[0] == '?');
    mpack_node_check_utf8(reason);
    mpack_node_check_utf8(mpack_node_map_cstr(data, "logTail"));
    EXPECT_EQ(mpack_tree_destroy(&tree), mpack_ok);
    EXPECT_EQ(de_ep_recv(&peer, bytes, sizeof bytes, &kind, &n, 5000), DE_EP_IO_CLOSED);
    int status;
    EXPECT_EQ(waitpid(pid, &status, 0), pid);
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    close(sockets[0]);
}
#else
TEST(editor_crash, native_fault_path) {
    SKIP("POSIX fork qualification; Windows native run pending");
}
#endif
