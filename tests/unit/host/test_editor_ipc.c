#include "de_test.h"
#if !defined(DE_SHIPPING)
#include "host/editor/editor_crash.h"
#include "host/editor/editor_internal.h"
#include "host/host_internal.h"
#include "platform/de_atomic.h"
#include <dengine/de_ecs.h>
#include <dengine/de_game.h>
#include <dengine/de_platform.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

/* DENG-784: the editor service runs its IPC on its own thread (DIDE "API: Editor IPC": "The engine
 * runs the service on its own thread and never blocks a frame on IPC. Read requests are answered
 * from a copy made at the last frame boundary."). A real service started by de__editor_start, a
 * real client on the OS transport (a Unix socket; on Windows the named pipe, DENG-806), and a
 * frame thread that runs the frame-begin hooks at 120 Hz. No mock transport (DENG-555). */

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define IPC_TSAN 1
#endif
#endif
#if defined(__SANITIZE_THREAD__)
#define IPC_TSAN 1
#endif

#define IPC_TOKEN    "deng-784-test-token"
#define IPC_FRAME_NS 8333333ull /* 120 Hz, as in DENG-784's measurement */
#define IPC_CONNECT_MS 1000u    /* how long a client keeps trying to connect */

/* One client link: a Unix socket, or a client handle on the named pipe (overlapped, so reads and
 * writes can time out). */
typedef struct cl_conn {
#ifdef _WIN32
    HANDLE pipe, ev;
#else
    int fd;
#endif
} cl_conn;

typedef struct ipc_fx {
    de_engine engine;
    de__host host;
    char endpoint[104];
    char *argv[3];
    de_thread *frames;
    de_atomic_u32 run, pump, frames_done;
    cl_conn cl;
    u64 next_id;
    char buf[1u << 16];
} ipc_fx;

static void cl_init(cl_conn *c) {
#ifdef _WIN32
    c->pipe = INVALID_HANDLE_VALUE;
    c->ev = NULL;
#else
    c->fd = -1;
#endif
}

static void cl_close(cl_conn *c) {
#ifdef _WIN32
    if (c->pipe != INVALID_HANDLE_VALUE)
        (void)CloseHandle(c->pipe);
    if (c->ev != NULL)
        (void)CloseHandle(c->ev);
#else
    if (c->fd >= 0)
        close(c->fd);
#endif
    cl_init(c);
}

static i32 frame_loop(void *user) {
    ipc_fx *f = (ipc_fx *)user;
    while (de_atomic_load_u32(&f->run, DE_ATOMIC_ACQUIRE)) {
        const u64 t0 = de_time_now_ns();
        if (de_atomic_load_u32(&f->pump, DE_ATOMIC_ACQUIRE)) {
            for (u32 i = 0; i < f->host.loop_hooks.frame_begin_count; ++i)
                (void)f->host.loop_hooks.frame_begin[i].fn(&f->host,
                                                           f->host.loop_hooks.frame_begin[i].user);
            ++f->host.loop_frames;
            de_atomic_fetch_add_u32(&f->frames_done, 1, DE_ATOMIC_RELEASE);
        }
        de_sleep_until_ns(t0 + IPC_FRAME_NS);
    }
    return 0;
}

static void ipc_fx_setup(ipc_fx *f) {
    memset(f, 0, sizeof *f);
    cl_init(&f->cl);
    ASSERT_EQ(de_log_init(&(de_log_desc){.no_console_sink = true}), DE_OK);
    ASSERT_EQ(de_type_registry_create(NULL, &f->engine.types), DE_OK);
    ASSERT_EQ(de_world_create(&(de_world_desc){.types = f->engine.types}, &f->engine.world), DE_OK);
    (void)de_builtin_components(f->engine.world);
    f->host.engine = &f->engine;
#ifdef _WIN32
    /* The pipe is made with FILE_FLAG_FIRST_PIPE_INSTANCE: a fresh name for every fixture. */
    static u32 serial;
    snprintf(f->endpoint, sizeof f->endpoint, "\\\\.\\pipe\\de-ipc-%lu-%u",
             (unsigned long)GetCurrentProcessId(), (unsigned)++serial);
    ASSERT_EQ(_putenv_s("DE_EDITOR_TOKEN", IPC_TOKEN), 0);
#else
    snprintf(f->endpoint, sizeof f->endpoint, "/tmp/de-ipc-%d.sock", (int)getpid());
    unlink(f->endpoint);
    setenv("DE_EDITOR_TOKEN", IPC_TOKEN, 1);
#endif
    f->argv[0] = "test_editor_ipc";
    f->argv[1] = "--editor-service";
    f->argv[2] = f->endpoint;
    f->host.desc.argc = 3;
    f->host.desc.argv = f->argv;
    ASSERT_EQ(de__editor_start(&f->host), DE_OK);
    ASSERT_TRUE(f->host.editor != NULL);
    de_atomic_init_u32(&f->run, 1);
    de_atomic_init_u32(&f->pump, 1);
    de_atomic_init_u32(&f->frames_done, 0);
    ASSERT_EQ(de_thread_create(frame_loop, f, "test-frames", DE_THREAD_NORMAL, 0, &f->frames),
              DE_OK);
}

static void ipc_fx_teardown(ipc_fx *f) {
    cl_close(&f->cl);
    if (f->frames) {
        de_atomic_store_u32(&f->run, 0, DE_ATOMIC_RELEASE);
        (void)de_thread_join(f->frames);
    }
    de__editor_stop(&f->host);
#ifndef _WIN32
    unlink(f->endpoint);
#endif
    de_world_destroy(f->engine.world);
    de_type_registry_destroy(f->engine.types);
    de_log_shutdown();
}

/* ---------- A minimal IDE ---------- */

#ifdef _WIN32

/* Connects within timeout_ms. The pipe has one instance: while another client holds it, the
 * open fails with ERROR_PIPE_BUSY and WaitNamedPipeW waits for the server to listen again (a
 * reconnect); before the server listens, the name does not exist yet. */
static bool cl_connect(cl_conn *c, const char *endpoint, u32 timeout_ms) {
    wchar_t name[128];
    size_t n = strlen(endpoint);
    if (n >= sizeof name / sizeof name[0])
        return false;
    for (size_t i = 0; i <= n; ++i)
        name[i] = (wchar_t)(unsigned char)endpoint[i]; /* the test's names are ASCII */
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        /* SECURITY_IDENTIFICATION: the server may identify this client, not impersonate it. */
        const HANDLE h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                                     FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT |
                                         SECURITY_IDENTIFICATION,
                                     NULL);
        if (h != INVALID_HANDLE_VALUE) {
            const HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
            if (ev == NULL) {
                (void)CloseHandle(h);
                return false;
            }
            c->pipe = h;
            c->ev = ev;
            return true;
        }
        const DWORD e = GetLastError();
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline)
            return false;
        const DWORD left = (DWORD)(deadline - now);
        if (e == ERROR_PIPE_BUSY)
            (void)WaitNamedPipeW(name, left < 50 ? left : 50);
        else if (e == ERROR_FILE_NOT_FOUND)
            Sleep(5);
        else
            return false;
    }
}

/* One overlapped read or write of up to n bytes. 1 some bytes moved (*done), 0 none within
 * timeout_ms, -1 closed or failed. A byte-mode pipe read completes as soon as any data is there,
 * as recv does. */
static int cl_io(cl_conn *c, bool write_, void *p, size_t n, DWORD timeout_ms, DWORD *done) {
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    ov.hEvent = c->ev;
    (void)ResetEvent(c->ev);
    const DWORD chunk = n > 0x40000000u ? 0x40000000u : (DWORD)n;
    const BOOL ok = write_ ? WriteFile(c->pipe, p, chunk, NULL, &ov)
                           : ReadFile(c->pipe, p, chunk, NULL, &ov);
    if (!ok && GetLastError() != ERROR_IO_PENDING)
        return -1;
    if (WaitForSingleObject(c->ev, timeout_ms) != WAIT_OBJECT_0)
        (void)CancelIoEx(c->pipe, &ov);
    /* Waits for the cancel too; a transfer that finished meanwhile still counts. */
    *done = 0;
    if (!GetOverlappedResult(c->pipe, &ov, done, TRUE))
        return GetLastError() == ERROR_OPERATION_ABORTED ? 0 : -1;
    return *done > 0 ? 1 : -1;
}

static bool cl_write_all(cl_conn *c, const void *p, size_t n) {
    char *b = (char *)(uintptr_t)p;
    while (n > 0) {
        DWORD k = 0;
        if (cl_io(c, true, b, n, 10000, &k) != 1)
            return false;
        b += k;
        n -= k;
    }
    return true;
}

/* 1 a frame, 0 none within timeout_ms, -1 closed or failed. */
static int cl_read_all(cl_conn *c, void *p, size_t n, int timeout_ms) {
    char *b = (char *)p;
    bool started = false;
    while (n > 0) {
        DWORD k = 0;
        const int r =
            cl_io(c, false, b, n, started ? 5000u : (timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms),
                  &k);
        if (r == 0)
            return started ? -1 : 0;
        if (r < 0)
            return -1;
        started = true;
        b += k;
        n -= k;
    }
    return 1;
}

#else /* POSIX */

static bool cl_connect(cl_conn *c, const char *endpoint, u32 timeout_ms) {
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", endpoint);
    for (u32 tries = 0; tries < timeout_ms / 5u; ++tries) {
        const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
            return false;
#ifdef SO_NOSIGPIPE
        const int one = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        if (connect(fd, (struct sockaddr *)&a, sizeof a) == 0) {
            c->fd = fd;
            return true;
        }
        close(fd);
        de_sleep_ns(5000000ull);
    }
    return false;
}

static bool cl_write_all(cl_conn *c, const void *p, size_t n) {
    const char *b = (const char *)p;
    while (n > 0) {
        const ssize_t k = send(c->fd, b, n, 0);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            return false;
        b += k;
        n -= (size_t)k;
    }
    return true;
}

/* 1 a frame, 0 none within timeout_ms, -1 closed or failed. */
static int cl_read_all(cl_conn *c, void *p, size_t n, int timeout_ms) {
    char *b = (char *)p;
    bool started = false;
    while (n > 0) {
        struct pollfd pf = {.fd = c->fd, .events = POLLIN};
        const int r = poll(&pf, 1, started ? 5000 : timeout_ms);
        if (r < 0 && errno == EINTR)
            continue;
        if (r == 0)
            return started ? -1 : 0;
        const ssize_t k = recv(c->fd, b, n, 0);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            return -1;
        started = true;
        b += k;
        n -= (size_t)k;
    }
    return 1;
}

#endif

static int cl_recv(ipc_fx *f, u8 *kind, size_t *len, int timeout_ms) {
    unsigned char h[5];
    const int r = cl_read_all(&f->cl, h, 5, timeout_ms);
    if (r != 1)
        return r;
    const u32 n = (u32)h[0] | (u32)h[1] << 8 | (u32)h[2] << 16 | (u32)h[3] << 24;
    if (n < 1 || n - 1 > sizeof f->buf)
        return -1;
    *kind = h[4];
    *len = n - 1;
    return cl_read_all(&f->cl, f->buf, n - 1, 5000) == 1 ? 1 : -1;
}

typedef void (*params_fn)(mpack_writer_t *w, const void *user);

static void no_params(mpack_writer_t *w, const void *user) {
    (void)user;
    mpack_start_map(w, 0);
    mpack_finish_map(w);
}

static u64 cl_request(ipc_fx *f, const char *method, params_fn params, const void *user) {
    char frame[4096];
    mpack_writer_t w;
    mpack_writer_init(&w, frame + 5, sizeof frame - 5);
    const u64 id = ++f->next_id;
    de_ep_write_request_begin(&w, id, method);
    params(&w, user);
    de_ep_write_request_end(&w);
    const size_t n = mpack_writer_buffer_used(&w);
    if (mpack_writer_destroy(&w) != mpack_ok)
        return 0;
    const u32 len = (u32)n + 1;
    for (u32 i = 0; i < 4; ++i)
        frame[i] = (char)(len >> (8 * i));
    frame[4] = (char)DE_EP_KIND_REQUEST;
    return cl_write_all(&f->cl, frame, n + 5) ? id : 0;
}

/* Reads frames until the response to `id`; its tree is left in `tree` (destroy it). Skips
 * notifications and blobs, and responses in `skip`-able order are an error. 1, 0 timeout, -1. */
static int cl_response(ipc_fx *f, u64 id, int timeout_ms, mpack_tree_t *tree) {
    const u64 deadline = de_time_now_ns() + (u64)timeout_ms * 1000000ull;
    for (;;) {
        const u64 now = de_time_now_ns();
        if (now >= deadline)
            return 0;
        u8 kind = 0;
        size_t len = 0;
        const int r = cl_recv(f, &kind, &len, (int)((deadline - now) / 1000000ull) + 1);
        if (r != 1)
            return r;
        if (kind != DE_EP_KIND_RESPONSE)
            continue;
        mpack_tree_init_data(tree, f->buf, len);
        mpack_tree_parse(tree);
        if (mpack_tree_error(tree) != mpack_ok) {
            (void)mpack_tree_destroy(tree);
            return -1;
        }
        const mpack_node_t root = mpack_tree_root(tree);
        if (mpack_node_u64(mpack_node_map_cstr(root, "id")) == id)
            return 1;
        (void)mpack_tree_destroy(tree);
        return -1; /* a response for some other request: out of order for this test */
    }
}

static void hello_params(mpack_writer_t *w, const void *user) {
    (void)user;
    mpack_start_map(w, 4);
    mpack_write_cstr(w, "protocol");
    mpack_write_cstr(w, DE_EP_PROTOCOL_VERSION);
    mpack_write_cstr(w, "token");
    mpack_write_cstr(w, IPC_TOKEN);
    mpack_write_cstr(w, "capabilities");
    mpack_start_array(w, 0);
    mpack_finish_array(w);
    mpack_write_cstr(w, "schema");
    mpack_write_cstr(w, DE_EP_SCHEMA_HASH);
    mpack_finish_map(w);
}

/* Connects and says hello; returns the epoch (0 on failure). */
static u32 cl_hello(ipc_fx *f) {
    if (!cl_connect(&f->cl, f->endpoint, IPC_CONNECT_MS))
        return 0;
    const u64 id = cl_request(f, "hello", hello_params, NULL);
    mpack_tree_t t;
    if (id == 0 || cl_response(f, id, 2000, &t) != 1)
        return 0;
    const mpack_node_t result = mpack_node_map_cstr_optional(mpack_tree_root(&t), "result");
    u32 epoch = 0;
    if (mpack_node_type(result) == mpack_type_map)
        epoch = (u32)mpack_node_u64(mpack_node_map_cstr(result, "epoch"));
    (void)mpack_tree_destroy(&t);
    return epoch;
}

static int ping_once(ipc_fx *f, int timeout_ms) {
    const u64 id = cl_request(f, "ping", no_params, NULL);
    if (id == 0)
        return -1;
    mpack_tree_t t;
    const int r = cl_response(f, id, timeout_ms, &t);
    if (r == 1)
        (void)mpack_tree_destroy(&t);
    return r;
}

static void mode_params(mpack_writer_t *w, const void *user) {
    mpack_start_map(w, 1);
    mpack_write_cstr(w, "mode");
    mpack_write_cstr(w, (const char *)user);
    mpack_finish_map(w);
}

static int compare_u64(const void *a, const void *b) {
    const u64 x = *(const u64 *)a, y = *(const u64 *)b;
    return x < y ? -1 : x > y;
}

/* ---------- Tests ---------- */

/* DIDE Constraints, "Editor IPC request round trip (small read) < 2 ms p95"; the conformance
 * check measures 200 pings the same way. Before DENG-784 the p95 was a frame (9.40 ms). */
TEST_F(editor_ipc, small_read_p95_is_under_2ms_while_frames_run_at_120hz, ipc_fx) {
    ASSERT_NE(cl_hello(f), 0u);
    u64 times[200];
    for (u32 i = 0; i < 200; ++i) {
        const u64 t0 = de_time_now_ns();
        ASSERT_EQ(ping_once(f, 1000), 1);
        times[i] = de_time_now_ns() - t0;
    }
    qsort(times, 200, sizeof times[0], compare_u64);
    const u64 p95 = times[200 * 95 / 100];
    printf("editor_ipc: ping p50 %.3f ms, p95 %.3f ms, max %.3f ms\n", (double)times[100] / 1e6,
           (double)p95 / 1e6, (double)times[199] / 1e6);
    EXPECT_LT(p95, 2000000ull);
}

/* A read never waits for a frame; a world change applies at the frame (the sync point), and a
 * read sent after its reply sees it. */
TEST_F(editor_ipc, reads_are_answered_while_the_frame_is_stalled_and_changes_wait_for_it, ipc_fx) {
    ASSERT_NE(cl_hello(f), 0u);
    de_atomic_store_u32(&f->pump, 0, DE_ATOMIC_RELEASE);
    de_sleep_ns(2 * IPC_FRAME_NS); /* any frame in progress has ended */
    const u32 before = de_atomic_load_u32(&f->frames_done, DE_ATOMIC_ACQUIRE);

    const u64 change = cl_request(f, "session.setMode", mode_params, "paused");
    ASSERT_NE(change, 0u);
    EXPECT_EQ(ping_once(f, 200), 1); /* answered with no frame at all */
    mpack_tree_t t;
    EXPECT_EQ(cl_response(f, change, 100, &t), 0); /* the change waits for the frame */
    EXPECT_EQ(de_atomic_load_u32(&f->frames_done, DE_ATOMIC_ACQUIRE), before);

    de_atomic_store_u32(&f->pump, 1, DE_ATOMIC_RELEASE);
    ASSERT_EQ(cl_response(f, change, 2000, &t), 1);
    EXPECT_TRUE(mpack_node_map_contains_cstr(mpack_tree_root(&t), "result"));
    (void)mpack_tree_destroy(&t);

    const u64 info = cl_request(f, "session.info", no_params, NULL);
    ASSERT_EQ(cl_response(f, info, 1000, &t), 1);
    const mpack_node_t mode =
        mpack_node_map_cstr(mpack_node_map_cstr(mpack_tree_root(&t), "result"), "mode");
    EXPECT_EQ(mpack_node_strlen(mode), 6u);
    EXPECT_EQ(memcmp(mpack_node_str(mode), "paused", 6), 0);
    (void)mpack_tree_destroy(&t);
}

/* Replies from the service thread and replies and notifications from the frame share one
 * connection: every frame stays whole and every request is answered exactly once. Run under
 * DE_SANITIZE=thread for the race check. */
TEST_F(editor_ipc, both_threads_share_the_connection_without_tearing_frames, ipc_fx) {
    ASSERT_NE(cl_hello(f), 0u);
    enum { ROUNDS = 40, BATCH = 12 };
    static const char *const modes[] = {"paused", "edit"};
    u32 notes = 0;
    for (u32 round = 0; round < ROUNDS; ++round) {
        u64 ids[BATCH];
        bool seen[BATCH] = {0};
        for (u32 i = 0; i < BATCH; ++i) {
            ids[i] = i % 3 == 0   ? cl_request(f, "session.setMode", mode_params, modes[round & 1])
                     : i % 3 == 1 ? cl_request(f, "ping", no_params, NULL)
                                  : cl_request(f, "session.info", no_params, NULL);
            ASSERT_NE(ids[i], 0u);
        }
        for (u32 got = 0; got < BATCH;) {
            u8 kind = 0;
            size_t len = 0;
            ASSERT_EQ(cl_recv(f, &kind, &len, 3000), 1);
            mpack_tree_t t;
            mpack_tree_init_data(&t, f->buf, len);
            mpack_tree_parse(&t);
            ASSERT_EQ(mpack_tree_error(&t), mpack_ok);
            if (kind == DE_EP_KIND_RESPONSE) {
                const u64 id = mpack_node_u64(mpack_node_map_cstr(mpack_tree_root(&t), "id"));
                ASSERT_GE(id, ids[0]);
                ASSERT_LE(id, ids[BATCH - 1]);
                EXPECT_FALSE(seen[id - ids[0]]);
                seen[id - ids[0]] = true;
                ++got;
            } else {
                EXPECT_EQ(kind, DE_EP_KIND_NOTIFICATION);
                ++notes;
            }
            (void)mpack_tree_destroy(&t);
        }
    }
    EXPECT_GT(notes, 0u); /* session.state from the frame thread interleaved with the replies */
}

/* A dropped IDE is noticed by the service thread; the frame resets and a new IDE gets the next
 * epoch. Goodbye is answered, then the connection closes and the engine quits. */
TEST_F(editor_ipc, reconnect_gets_a_new_epoch_and_goodbye_closes_after_its_reply, ipc_fx) {
    const u32 first = cl_hello(f);
    ASSERT_NE(first, 0u);
    cl_close(&f->cl);
    const u32 second = cl_hello(f);
    EXPECT_EQ(second, first + 1);
    EXPECT_EQ(ping_once(f, 1000), 1);

    const u64 bye = cl_request(f, "goodbye", no_params, NULL);
    mpack_tree_t t;
    ASSERT_EQ(cl_response(f, bye, 2000, &t), 1);
    (void)mpack_tree_destroy(&t);
    u8 kind = 0;
    size_t len = 0;
    int r = 1;
    while (r == 1)
        r = cl_recv(f, &kind, &len, 2000);
    EXPECT_EQ(r, -1); /* closed, not a timeout */
    EXPECT_TRUE(de_engine_quit_requested(&f->engine));
}

/* DENG-701's fatal notification still reaches the IDE when the service runs on its own thread
 * (the crash path is armed by that thread). fork() with live threads is not TSan-safe. Windows has
 * no fork(): the report would run in this process, and the fatal path freezes the service for the
 * rest of the process, so this in-process suite cannot run it there. */
TEST_F(editor_ipc, crash_notification_reaches_the_ide, ipc_fx) {
#if defined(_WIN32)
    SKIP("needs fork(): a fatal report freezes the service for the whole process on Windows");
#elif defined(IPC_TSAN)
    SKIP("fork() with live threads under ThreadSanitizer");
#else
    ASSERT_NE(cl_hello(f), 0u);
    ASSERT_EQ(ping_once(f, 1000), 1);
    const pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        const de_crash_context ctx = {.reason = "DENG-784 test crash",
                                      .report_path = "",
                                      .thread_name = "main",
                                      .build_id = "test"};
        de__ed_crash_report(&ctx, NULL);
        _exit(0);
    }
    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    bool crashed = false;
    for (int i = 0; i < 50 && !crashed; ++i) {
        u8 kind = 0;
        size_t len = 0;
        if (cl_recv(f, &kind, &len, 100) != 1)
            continue;
        if (kind != DE_EP_KIND_NOTIFICATION)
            continue;
        mpack_tree_t t;
        mpack_tree_init_data(&t, f->buf, len);
        mpack_tree_parse(&t);
        if (mpack_tree_error(&t) == mpack_ok) {
            const mpack_node_t topic = mpack_node_map_cstr_optional(mpack_tree_root(&t), "topic");
            crashed = mpack_node_type(topic) == mpack_type_str &&
                      mpack_node_strlen(topic) == strlen("session.crashed") &&
                      memcmp(mpack_node_str(topic), "session.crashed", 15) == 0;
        }
        (void)mpack_tree_destroy(&t);
    }
    EXPECT_TRUE(crashed);
#endif
}

/* DENG-555, IPC "Limits": "256 requests in flight ... A request over a limit gets `busy` with
 * `data: { retryAfterMs }`." With the frame stalled, 256 world changes wait for it; the next is
 * refused at once, reads still answer, and every waiting change is answered once the frame runs. */
TEST_F(editor_ipc, a_request_over_the_in_flight_limit_gets_busy_with_retry_after, ipc_fx) {
    ASSERT_NE(cl_hello(f), 0u);
    de_atomic_store_u32(&f->pump, 0, DE_ATOMIC_RELEASE);
    de_sleep_ns(2 * IPC_FRAME_NS);

    u64 first = 0;
    for (u32 i = 0; i < DE_EP_LIMIT_REQUESTS_IN_FLIGHT; ++i) {
        const u64 id = cl_request(f, "session.setMode", mode_params, "paused");
        ASSERT_NE(id, 0u);
        if (i == 0)
            first = id;
    }
    const u64 over = cl_request(f, "session.setMode", mode_params, "paused");
    ASSERT_NE(over, 0u);
    mpack_tree_t t;
    ASSERT_EQ(cl_response(f, over, 1000, &t), 1);
    const mpack_node_t err = mpack_node_map_cstr_optional(mpack_tree_root(&t), "error");
    ASSERT_EQ(mpack_node_type(err), mpack_type_map);
    EXPECT_EQ(mpack_node_i64(mpack_node_map_cstr(err, "code")), (i64)DE_EP_ERR_BUSY);
    const mpack_node_t data = mpack_node_map_cstr_optional(err, "data");
    ASSERT_EQ(mpack_node_type(data), mpack_type_map);
    EXPECT_GT(mpack_node_u64(mpack_node_map_cstr(data, "retryAfterMs")), 0u);
    (void)mpack_tree_destroy(&t);
    EXPECT_EQ(ping_once(f, 500), 1); /* a read answered on the service thread is not in flight */

    de_atomic_store_u32(&f->pump, 1, DE_ATOMIC_RELEASE);
    static bool seen[DE_EP_LIMIT_REQUESTS_IN_FLIGHT];
    memset(seen, 0, sizeof seen);
    for (u32 got = 0; got < DE_EP_LIMIT_REQUESTS_IN_FLIGHT;) {
        u8 kind = 0;
        size_t len = 0;
        ASSERT_EQ(cl_recv(f, &kind, &len, 3000), 1);
        if (kind != DE_EP_KIND_RESPONSE)
            continue;
        mpack_tree_init_data(&t, f->buf, len);
        mpack_tree_parse(&t);
        ASSERT_EQ(mpack_tree_error(&t), mpack_ok);
        const u64 id = mpack_node_u64(mpack_node_map_cstr(mpack_tree_root(&t), "id"));
        ASSERT_GE(id, first);
        ASSERT_LT(id - first, (u64)DE_EP_LIMIT_REQUESTS_IN_FLIGHT);
        EXPECT_FALSE(seen[id - first]);
        EXPECT_TRUE(mpack_node_map_contains_cstr(mpack_tree_root(&t), "result"));
        seen[id - first] = true;
        ++got;
        (void)mpack_tree_destroy(&t);
    }
    /* The answered requests released their places. */
    const u64 again = cl_request(f, "session.setMode", mode_params, "edit");
    ASSERT_EQ(cl_response(f, again, 2000, &t), 1);
    EXPECT_TRUE(mpack_node_map_contains_cstr(mpack_tree_root(&t), "result"));
    (void)mpack_tree_destroy(&t);
}

/* Reads hello's limits.frame_size from a fresh link (0 on failure). */
static u32 cl_hello_frame_size(ipc_fx *f) {
    if (!cl_connect(&f->cl, f->endpoint, IPC_CONNECT_MS))
        return 0;
    const u64 id = cl_request(f, "hello", hello_params, NULL);
    mpack_tree_t t;
    if (id == 0 || cl_response(f, id, 2000, &t) != 1)
        return 0;
    u32 size = 0;
    const mpack_node_t result = mpack_node_map_cstr_optional(mpack_tree_root(&t), "result");
    if (mpack_node_type(result) == mpack_type_map) {
        const mpack_node_t limits = mpack_node_map_cstr_optional(result, "limits");
        if (mpack_node_type(limits) == mpack_type_map)
            size = (u32)mpack_node_u64(mpack_node_map_cstr(limits, "frameSize"));
    }
    (void)mpack_tree_destroy(&t);
    return size;
}

/* Sends a ping whose frame length field (kind byte + payload) is exactly `frame_len`. */
static u64 cl_padded_ping(ipc_fx *f, u32 frame_len) {
    const size_t payload = (size_t)frame_len - 1;
    char *frame = (char *)malloc(payload + 5);
    if (frame == NULL)
        return 0;
    const u64 id = ++f->next_id;
    size_t pad = payload - 256, used = 0;
    for (int pass = 0; pass < 2; ++pass) {
        mpack_writer_t w;
        mpack_writer_init(&w, frame + 5, payload);
        de_ep_write_request_begin(&w, id, "ping");
        mpack_start_map(&w, 1);
        mpack_write_cstr(&w, "pad");
        mpack_start_bin(&w, (u32)pad);
        mpack_write_bytes(&w, frame + 5, pad); /* content is irrelevant */
        mpack_finish_bin(&w);
        mpack_finish_map(&w);
        de_ep_write_request_end(&w);
        used = mpack_writer_buffer_used(&w);
        if (mpack_writer_destroy(&w) != mpack_ok) {
            free(frame);
            return 0;
        }
        pad += payload - used; /* the bin32 header stays the same size */
    }
    bool ok = used == payload;
    for (u32 i = 0; i < 4; ++i)
        frame[i] = (char)(frame_len >> (8 * i));
    frame[4] = (char)DE_EP_KIND_REQUEST;
    ok = ok && cl_write_all(&f->cl, frame, payload + 5);
    free(frame);
    return ok ? id : 0;
}

/* DENG-555, IPC "Limits": hello's frame_size is the size the engine really accepts. A frame of
 * exactly that size is answered; one byte over closes the connection ("A frame over the size
 * limit closes the connection"). Before DENG-555 hello said 64 MiB and the engine closed at 16. */
TEST_F(editor_ipc, hello_frame_size_is_the_size_the_engine_accepts, ipc_fx) {
    const u32 limit = cl_hello_frame_size(f);
    ASSERT_GT(limit, 1024u);
    const u64 id = cl_padded_ping(f, limit);
    ASSERT_NE(id, 0u);
    mpack_tree_t t;
    ASSERT_EQ(cl_response(f, id, 5000, &t), 1);
    EXPECT_TRUE(mpack_node_map_contains_cstr(mpack_tree_root(&t), "result"));
    (void)mpack_tree_destroy(&t);

    const u32 over = limit + 1;
    const unsigned char head[5] = {(unsigned char)over, (unsigned char)(over >> 8),
                                   (unsigned char)(over >> 16), (unsigned char)(over >> 24),
                                   (unsigned char)DE_EP_KIND_REQUEST};
    ASSERT_TRUE(cl_write_all(&f->cl, head, sizeof head));
    u8 kind = 0;
    size_t len = 0;
    int r = 1;
    while (r == 1)
        r = cl_recv(f, &kind, &len, 2000);
    EXPECT_EQ(r, -1); /* closed, not a timeout */
}

/* IPC: "A second `hello` while a connection is live is refused with `conflict`." DENG-790: a
 * second IDE never hangs, and the live link keeps working. On POSIX the second client connects,
 * its hello is answered `conflict` and the link closes. The Windows pipe has one instance, so the
 * second client cannot connect at all: its open fails within its timeout. */
TEST_F(editor_ipc, a_second_client_is_refused_while_one_is_live, ipc_fx) {
    ASSERT_NE(cl_hello(f), 0u);
    cl_conn live = f->cl;
    cl_init(&f->cl);
    const u64 t0 = de_time_now_ns();
#ifdef _WIN32
    const bool connected = cl_connect(&f->cl, f->endpoint, 500);
    EXPECT_FALSE(connected);
    EXPECT_LT(de_time_now_ns() - t0, 2000000000ull);
#else
    ASSERT_TRUE(cl_connect(&f->cl, f->endpoint, IPC_CONNECT_MS));
    const u64 id = cl_request(f, "hello", hello_params, NULL);
    ASSERT_NE(id, 0u);
    mpack_tree_t t;
    ASSERT_EQ(cl_response(f, id, 2000, &t), 1);
    const mpack_node_t err = mpack_node_map_cstr_optional(mpack_tree_root(&t), "error");
    ASSERT_EQ(mpack_node_type(err), mpack_type_map);
    EXPECT_EQ(mpack_node_i64(mpack_node_map_cstr(err, "code")), (i64)DE_EP_ERR_CONFLICT);
    (void)mpack_tree_destroy(&t);
    u8 kind = 0;
    size_t len = 0;
    int r = 1;
    while (r == 1)
        r = cl_recv(f, &kind, &len, 2000);
    EXPECT_EQ(r, -1); /* closed, not a timeout */
    EXPECT_LT(de_time_now_ns() - t0, 2000000000ull);
#endif
    cl_close(&f->cl);
    f->cl = live;
    EXPECT_EQ(ping_once(f, 1000), 1);
}

#endif
