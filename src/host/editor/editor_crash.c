/* DENG-701: preallocated, best-effort fatal notification. The ordinary transport can block
 * and split frames; never enter it from a crash callback. Skip an interrupted writer instead
 * of waiting for it. Once fatal ownership wins, neither handle nor packet may be reused. */
#include "host/editor/editor_crash.h"
#include "de_editor_protocol.h"
#include "platform/de_atomic.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/socket.h>
#endif

#define CRASH_REASON_CAP 512u
#define CRASH_TAIL_CAP   1536u
#define CRASH_PACKET_CAP 4096u
_Static_assert(CRASH_PACKET_CAP >= CRASH_REASON_CAP + CRASH_TAIL_CAP + 128u,
               "crash envelope must fit without a flush or allocation");

/* 0 idle, 1 normal owner, 2 fatal owner. All non-atomic fields belong to the CAS winner.
 * Lock-free atomic operations are a platform requirement in de_atomic.h. */
static de_atomic_u32 gate;
static bool registered, prepared, armed;
static u64 sequence;
static mpack_writer_t writer;
static char packet[CRASH_PACKET_CAP], reason[CRASH_REASON_CAP], tail[CRASH_TAIL_CAP];
#if defined(_WIN32)
static HANDLE handle;
static OVERLAPPED overlap; /* no stack storage may outlive an overlapped WriteFile */
#else
static int handle = -1;
static int fatal_flags;
#endif

static bool acquire(u32 owner) {
    u32 expected = 0;
    return de_atomic_cas_u32(&gate, &expected, owner, DE_ATOMIC_ACQ_REL, DE_ATOMIC_ACQUIRE);
}

static void release(void) {
    de_atomic_store_u32(&gate, 0, DE_ATOMIC_RELEASE);
}

static void unprepare(void) {
    armed = false;
    if (prepared) {
        (void)mpack_writer_destroy(&writer);
        prepared = false;
    }
}

de_result de__ed_crash_register(void) {
    if (registered)
        return DE_OK;
    const de_result r = de_crash_add_handler(de__ed_crash_report, NULL);
    if (r == DE_OK)
        registered = true;
    return r;
}

bool de__ed_crash_arm(const de_ep_conn *conn, u64 next_seq) {
    if (!acquire(1))
        return false;
    unprepare();
    /* Debug mpack tracking may allocate here, never on the fatal path. The fixed envelope
     * needs at most three tracking entries; its initial capacity is eight. */
    mpack_writer_init(&writer, packet + 5, sizeof packet - 5);
    prepared = true;
    armed = mpack_writer_error(&writer) == mpack_ok;
    sequence = next_seq;
#if defined(_WIN32)
    handle = conn->listener->pipe;
#else
    handle = conn->fd;
    const int flags = fcntl(handle, F_GETFL);
    armed = armed && flags >= 0;
    fatal_flags = flags | O_NONBLOCK;
#endif
    const bool ok = armed;
    release();
    return ok;
}

bool de__ed_crash_disarm(void) {
    if (!acquire(1))
        return false;
    unprepare();
    release();
    return true;
}

bool de__ed_crash_frozen(void) {
    return de_atomic_load_u32(&gate, DE_ATOMIC_ACQUIRE) == 2;
}

de_ep_io de__ed_crash_send(de_ep_conn *conn, u8 kind, const void *bytes, size_t n, u64 next_seq) {
    if (!acquire(1))
        return DE_EP_IO_CLOSED;
    const de_ep_io io = de_ep_send(conn, kind, bytes, n);
    sequence = next_seq;
    /* Never append a crash packet to an incomplete ordinary frame. */
    if (io != DE_EP_IO_OK)
        unprepare();
    release();
    return io;
}

/* Bounded UTF-8 copy, also sanitizing an already truncated/invalid panic or native path.
 * No strlen, locale, formatting, or allocator. Never read beyond the output budget. */
static size_t append(char *dst, size_t cap, size_t at, const char *src) {
    if (src == NULL)
        return at;
    size_t i = 0;
    while (at + 1 < cap && src[i] != '\0') {
        const unsigned char c = (unsigned char)src[i];
        size_t n = c < 0x80                 ? 1
                   : c >= 0xc2 && c <= 0xdf ? 2
                   : c >= 0xe0 && c <= 0xef ? 3
                   : c >= 0xf0 && c <= 0xf4 ? 4
                                            : 0;
        if (n > cap - at - 1)
            break;
        bool valid = n != 0;
        for (size_t j = 1; j < n; ++j) {
            const unsigned char b = (unsigned char)src[i + j];
            if (b < 0x80 || b > 0xbf) {
                valid = false;
                break;
            }
            if (j == 1 && ((c == 0xe0 && b < 0xa0) || (c == 0xed && b > 0x9f) ||
                           (c == 0xf0 && b < 0x90) || (c == 0xf4 && b > 0x8f)))
                valid = false;
        }
        if (!valid) {
            dst[at++] = '?';
            ++i;
        } else {
            for (size_t j = 0; j < n; ++j)
                dst[at++] = src[i++];
        }
    }
    dst[at] = '\0';
    return at;
}

void de__ed_crash_report(const de_crash_context *ctx, void *user) {
    (void)user;
    /* One attempt, no retry, lock, wait, log, heap, service pointer, or normal transport. */
    if (!acquire(2))
        return;
    if (!armed)
        return;
    size_t rn =
        append(reason, sizeof reason, 0,
               ctx->panic_message && ctx->panic_message[0] ? ctx->panic_message : ctx->reason);
    if (rn == 0)
        rn = append(reason, sizeof reason, 0, "engine fatal error");
    size_t tn = append(tail, sizeof tail, 0, "Crash report: ");
    tn = append(tail, sizeof tail, tn, ctx->report_path);
    tn = append(tail, sizeof tail, tn, "\nThread: ");
    tn = append(tail, sizeof tail, tn, ctx->thread_name);
    tn = append(tail, sizeof tail, tn, "\nBuild: ");
    tn = append(tail, sizeof tail, tn, ctx->build_id);
    const de_ep_crashed crash = {
        .reason = {reason, (u32)rn}, .has_log_tail = true, .log_tail = {tail, (u32)tn}};
    de_ep_write_notify_session_crashed(&writer, sequence, &crash);
    if (mpack_writer_error(&writer) != mpack_ok)
        return;
    const size_t n = mpack_writer_buffer_used(&writer);
    const u32 length = (u32)n + 1;
    for (u32 i = 0; i < 4; ++i)
        packet[i] = (char)(length >> (8 * i));
    packet[4] = DE_EP_KIND_NOTIFICATION;
#if defined(_WIN32)
    /* Fatal-only NOWAIT: the ordinary overlapped transport remains unchanged. If the OS
     * queues this write, packet/overlap/handle stay alive until process death. Never wait. */
    DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
    if (SetNamedPipeHandleState(handle, &mode, NULL, NULL)) {
        (void)WriteFile(handle, packet, (DWORD)(n + 5), NULL, &overlap);
    }
#else
    /* The frozen owner may permanently set nonblocking mode: no ordinary writer can run
     * again. This also covers platforms where MSG_DONTWAIT alone does not bound a full
     * Unix-stream send. fcntl(F_SETFL) is POSIX async-signal-safe; never change a live
     * connection's mode outside terminal ownership. The prior flags were captured at arm. */
    if (fcntl(handle, F_SETFL, fatal_flags) < 0)
        return;
    int flags = MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    /* Accepted macOS sockets already have SO_NOSIGPIPE. A partial/full-buffer send is not
     * retried: the normal process-exit fallback still reports the fatal termination. */
    (void)send(handle, packet, n + 5, flags);
#endif
    /* No destroy: Debug mpack tracking and an optional overlapped write are still live. */
}
