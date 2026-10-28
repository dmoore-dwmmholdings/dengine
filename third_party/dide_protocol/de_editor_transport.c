/* The engine side of the editor IPC transport. See de_editor_transport.h. */
#include "de_editor_transport.h"

#include <string.h>

static void de_ep__put_u32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v & 0xffu);
    p[1] = (unsigned char)((v >> 8) & 0xffu);
    p[2] = (unsigned char)((v >> 16) & 0xffu);
    p[3] = (unsigned char)((v >> 24) & 0xffu);
}

static uint32_t de_ep__get_u32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <sddl.h>

/* The SDDL granting the current user alone: "D:P(A;;GA;;;<sid>)". */
static bool de_ep__sddl(char *out, size_t cap) {
    HANDLE token = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    union {
        TOKEN_USER user;
        unsigned char bytes[256];
    } info;
    DWORD len = 0;
    BOOL ok = GetTokenInformation(token, TokenUser, &info, (DWORD)sizeof(info), &len);
    CloseHandle(token);
    if (!ok)
        return false;
    char *sid = NULL;
    if (!ConvertSidToStringSidA(info.user.User.Sid, &sid))
        return false;
    static const char prefix[] = "D:P(A;;GA;;;";
    size_t n = strlen(sid);
    bool fits = sizeof(prefix) - 1 + n + 2 <= cap;
    if (fits) {
        memcpy(out, prefix, sizeof(prefix) - 1);
        memcpy(out + sizeof(prefix) - 1, sid, n);
        out[sizeof(prefix) - 1 + n] = ')';
        out[sizeof(prefix) + n] = '\0';
    }
    LocalFree(sid);
    return fits;
}

de_ep_io de_ep_listen(de_ep_listener *l, const char *endpoint) {
    memset(l, 0, sizeof(*l));
    char sddl[256];
    if (!de_ep__sddl(sddl, sizeof(sddl)))
        return DE_EP_IO_ERROR;
    PSECURITY_DESCRIPTOR sd = NULL;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(sddl, SDDL_REVISION_1, &sd, NULL))
        return DE_EP_IO_ERROR;
    SECURITY_ATTRIBUTES sa;
    sa.nLength = (DWORD)sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;
    HANDLE pipe = CreateNamedPipeA(endpoint,
                                   PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                   1, 65536, 65536, 0, &sa);
    LocalFree(sd);
    if (pipe == INVALID_HANDLE_VALUE)
        return DE_EP_IO_ERROR;
    HANDLE event = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (event == NULL) {
        CloseHandle(pipe);
        return DE_EP_IO_ERROR;
    }
    l->pipe = pipe;
    l->event = event;
    return DE_EP_IO_OK;
}

static DWORD de_ep__wait_ms(int timeout_ms) {
    return timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms;
}

de_ep_io de_ep_accept(de_ep_listener *l, de_ep_conn *c, uint32_t max_frame, int timeout_ms) {
    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    ov.hEvent = l->event;
    ResetEvent(l->event);
    if (!ConnectNamedPipe(l->pipe, &ov)) {
        DWORD e = GetLastError();
        if (e == ERROR_IO_PENDING) {
            DWORD w = WaitForSingleObject(l->event, de_ep__wait_ms(timeout_ms));
            DWORD n = 0;
            if (w != WAIT_OBJECT_0) {
                CancelIoEx(l->pipe, &ov);
                GetOverlappedResult(l->pipe, &ov, &n, TRUE);
                return w == WAIT_TIMEOUT ? DE_EP_IO_TIMEOUT : DE_EP_IO_ERROR;
            }
            if (!GetOverlappedResult(l->pipe, &ov, &n, FALSE))
                return DE_EP_IO_ERROR;
        } else if (e != ERROR_PIPE_CONNECTED) {
            return DE_EP_IO_ERROR;
        }
    }
    c->listener = l;
    c->max_frame = max_frame;
    return DE_EP_IO_OK;
}

/* All of `n` bytes, read or written. */
static de_ep_io de_ep__io(de_ep_conn *c, bool write, void *p, size_t n) {
    HANDLE pipe = c->listener->pipe;
    unsigned char *b = (unsigned char *)p;
    while (n > 0) {
        OVERLAPPED ov;
        memset(&ov, 0, sizeof(ov));
        ov.hEvent = c->listener->event;
        ResetEvent(ov.hEvent);
        DWORD chunk = n > 0x40000000u ? 0x40000000u : (DWORD)n, done = 0;
        BOOL ok = write ? WriteFile(pipe, b, chunk, NULL, &ov) : ReadFile(pipe, b, chunk, NULL, &ov);
        if (!ok && GetLastError() != ERROR_IO_PENDING)
            return GetLastError() == ERROR_BROKEN_PIPE || GetLastError() == ERROR_NO_DATA ? DE_EP_IO_CLOSED
                                                                                          : DE_EP_IO_ERROR;
        if (!GetOverlappedResult(pipe, &ov, &done, TRUE)) {
            DWORD e = GetLastError();
            return e == ERROR_BROKEN_PIPE || e == ERROR_PIPE_NOT_CONNECTED ? DE_EP_IO_CLOSED : DE_EP_IO_ERROR;
        }
        if (done == 0)
            return DE_EP_IO_CLOSED;
        b += done;
        n -= done;
    }
    return DE_EP_IO_OK;
}

static de_ep_io de_ep__readable(de_ep_conn *c, int timeout_ms) {
    ULONGLONG start = GetTickCount64();
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(c->listener->pipe, NULL, 0, NULL, &avail, NULL))
            return DE_EP_IO_CLOSED;
        if (avail > 0)
            return DE_EP_IO_OK;
        if (timeout_ms >= 0 && GetTickCount64() - start >= (ULONGLONG)timeout_ms)
            return DE_EP_IO_TIMEOUT;
        Sleep(1);
    }
}

void de_ep_disconnect(de_ep_conn *c) {
    if (c->listener != NULL) {
        /* DisconnectNamedPipe drops what the IDE hasn't read yet (a last
         * error response): wait until it has. */
        FlushFileBuffers(c->listener->pipe);
        DisconnectNamedPipe(c->listener->pipe);
    }
    c->listener = NULL;
}

void de_ep_listener_close(de_ep_listener *l) {
    if (l->pipe != NULL)
        CloseHandle(l->pipe);
    if (l->event != NULL)
        CloseHandle(l->event);
    l->pipe = NULL;
    l->event = NULL;
}

#else /* POSIX */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

de_ep_io de_ep_listen(de_ep_listener *l, const char *endpoint) {
    memset(l, 0, sizeof(*l));
    l->fd = -1;
    size_t n = strlen(endpoint);
    struct sockaddr_un addr;
    /* sun_path is 104 bytes on macOS, 108 on Linux: the smaller bound. */
    if (n >= sizeof(l->path) || n >= sizeof(addr.sun_path))
        return DE_EP_IO_ERROR;
    memcpy(l->path, endpoint, n + 1);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return DE_EP_IO_ERROR;
    /* Not inherited by processes the engine starts: they would keep the
     * connection open and hide its end from the IDE. */
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, endpoint, n + 1);
    unlink(endpoint);
    if (bind(fd, (struct sockaddr *)&addr, (socklen_t)sizeof(addr)) != 0 || chmod(endpoint, 0600) != 0 ||
        listen(fd, 1) != 0) {
        close(fd);
        return DE_EP_IO_ERROR;
    }
    l->fd = fd;
    return DE_EP_IO_OK;
}

static de_ep_io de_ep__poll(int fd, int timeout_ms) {
    struct pollfd p;
    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    int r;
    do {
        r = poll(&p, 1, timeout_ms < 0 ? -1 : timeout_ms);
    } while (r < 0 && errno == EINTR);
    if (r < 0)
        return DE_EP_IO_ERROR;
    return r == 0 ? DE_EP_IO_TIMEOUT : DE_EP_IO_OK;
}

de_ep_io de_ep_accept(de_ep_listener *l, de_ep_conn *c, uint32_t max_frame, int timeout_ms) {
    de_ep_io r = de_ep__poll(l->fd, timeout_ms);
    if (r != DE_EP_IO_OK)
        return r;
    int fd = accept(l->fd, NULL, NULL);
    if (fd < 0)
        return DE_EP_IO_ERROR;
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, (socklen_t)sizeof(one));
#endif
    c->fd = fd;
    c->max_frame = max_frame;
    return DE_EP_IO_OK;
}

static de_ep_io de_ep__io(de_ep_conn *c, bool write_, void *p, size_t n) {
    unsigned char *b = (unsigned char *)p;
    while (n > 0) {
#ifdef MSG_NOSIGNAL
        ssize_t r = write_ ? send(c->fd, b, n, MSG_NOSIGNAL) : recv(c->fd, b, n, 0);
#else
        ssize_t r = write_ ? send(c->fd, b, n, 0) : recv(c->fd, b, n, 0);
#endif
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0)
            return errno == EPIPE || errno == ECONNRESET ? DE_EP_IO_CLOSED : DE_EP_IO_ERROR;
        if (r == 0)
            return DE_EP_IO_CLOSED;
        b += r;
        n -= (size_t)r;
    }
    return DE_EP_IO_OK;
}

static de_ep_io de_ep__readable(de_ep_conn *c, int timeout_ms) {
    return de_ep__poll(c->fd, timeout_ms);
}

void de_ep_disconnect(de_ep_conn *c) {
    if (c->fd >= 0)
        close(c->fd);
    c->fd = -1;
}

void de_ep_listener_close(de_ep_listener *l) {
    if (l->fd >= 0) {
        close(l->fd);
        unlink(l->path);
    }
    l->fd = -1;
}

#endif

de_ep_io de_ep_recv(de_ep_conn *c, void *buf, size_t cap, uint8_t *kind, size_t *len, int timeout_ms) {
    de_ep_io r = de_ep__readable(c, timeout_ms);
    if (r != DE_EP_IO_OK)
        return r;
    unsigned char head[5];
    r = de_ep__io(c, false, head, sizeof(head));
    if (r != DE_EP_IO_OK)
        return r;
    uint32_t n = de_ep__get_u32(head);
    if (n == 0 || n > c->max_frame || (size_t)(n - 1) > cap)
        return DE_EP_IO_TOO_BIG;
    *kind = head[4];
    *len = (size_t)(n - 1);
    return *len == 0 ? DE_EP_IO_OK : de_ep__io(c, false, buf, *len);
}

de_ep_io de_ep_send(de_ep_conn *c, uint8_t kind, const void *payload, size_t len) {
    if (len + 1 > (size_t)c->max_frame)
        return DE_EP_IO_TOO_BIG;
    unsigned char head[5];
    de_ep__put_u32(head, (uint32_t)(len + 1));
    head[4] = kind;
    de_ep_io r = de_ep__io(c, true, head, sizeof(head));
    if (r != DE_EP_IO_OK || len == 0)
        return r;
    return de_ep__io(c, true, (void *)(uintptr_t)payload, len);
}
