/* The engine side of the editor IPC transport (DIDE spec v2, "API: Editor
 * IPC" → Transport and security). DEngine's editor service listens on the
 * endpoint the IDE passes as `--editor-service <endpoint>`; the IDE
 * connects, and its first frame must be `hello` with the token from
 * `DE_EDITOR_TOKEN` (check it with de_ep_token_eq).
 *
 * - Windows: a named pipe the current user alone can open (a DACL with
 *   only their SID), remote clients rejected, one instance.
 * - macOS: a Unix socket, mode 0600, in the IDE's 0700 folder.
 *
 * Frames are `u32 length (LE) | u8 kind | payload`, the length counting the
 * kind byte and the payload. Nothing here allocates; the caller owns the
 * structs and buffers. Meant for one service thread: recv polls with a
 * timeout, so the thread can also send notifications.
 */
#ifndef DE_EDITOR_TRANSPORT_H
#define DE_EDITOR_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum de_ep_io {
    DE_EP_IO_OK = 0,
    DE_EP_IO_TIMEOUT = 1,  /* nothing arrived in time */
    DE_EP_IO_CLOSED = -1,  /* the IDE went away */
    DE_EP_IO_ERROR = -2,   /* an OS error */
    DE_EP_IO_TOO_BIG = -3, /* a frame over the buffer or the limit: close the connection */
} de_ep_io;

typedef struct de_ep_listener {
#ifdef _WIN32
    void *pipe;  /* HANDLE */
    void *event; /* HANDLE, for overlapped I/O */
#else
    int fd;
    char path[108];
#endif
} de_ep_listener;

typedef struct de_ep_conn {
#ifdef _WIN32
    de_ep_listener *listener; /* the pipe instance is the connection */
#else
    int fd;
#endif
    uint32_t max_frame;
} de_ep_conn;

/* Starts listening. */
de_ep_io de_ep_listen(de_ep_listener *l, const char *endpoint);
/* Waits up to timeout_ms (negative: forever) for the IDE. */
de_ep_io de_ep_accept(de_ep_listener *l, de_ep_conn *c, uint32_t max_frame, int timeout_ms);
/* One frame into buf; TIMEOUT when none starts within timeout_ms. */
de_ep_io de_ep_recv(de_ep_conn *c, void *buf, size_t cap, uint8_t *kind, size_t *len, int timeout_ms);
de_ep_io de_ep_send(de_ep_conn *c, uint8_t kind, const void *payload, size_t len);
/* Ends the connection; the listener can accept again (a reconnect). */
void de_ep_disconnect(de_ep_conn *c);
void de_ep_listener_close(de_ep_listener *l);

#ifdef __cplusplus
}
#endif

#endif /* DE_EDITOR_TRANSPORT_H */
