/*
 * editor_ipc.h: the editor service's IPC thread (DENG-784). DIDE "API: Editor IPC": "The engine
 * runs the service on its own thread and never blocks a frame on IPC. Read requests are answered
 * from a copy made at the last frame boundary. Requests that change the world are queued and
 * applied at the next frame boundary."
 *
 * The service thread owns the listener and the connection: it accepts, receives, sends, refuses a
 * second IDE, does the hello handshake and arms the crash notification (editor_crash.h: normal
 * crash-gate operations are service-thread only). The transport is single-threaded by design
 * (on Windows one overlapped event serves accept, recv and send).
 *
 * Small reads (`ping`, `session.info`) are answered on the thread from a snapshot the frame
 * publishes. Every other request goes to the frame through the inbound queue, in order; the
 * frame's replies and notifications come back through the outbound queue, in order. A record is
 * tagged with the connection epoch, so nothing reaches a later connection.
 *
 * Include as "host/editor/editor_ipc.h". Internal to src/host/editor/.
 */
#ifndef DE_HOST_EDITOR_IPC_H
#define DE_HOST_EDITOR_IPC_H

#include <dengine/de_base.h>

#include "de_editor_protocol.h"

DE_EXTERN_C_BEGIN

/** The frame-boundary copy that the thread answers small reads from. */
typedef struct de__ed_snapshot {
    char build_id[512];
    char engine_version[32];
    char gpu_backend[32];
    char config[16];
    de_ep_world_mode mode;
    u64 frame;
    u64 next_seq; /**< the next notification sequence number (for the crash notification) */
} de__ed_snapshot;

typedef enum de__ed_event_type {
    DE__ED_EVENT_CONNECTED = 1, /**< an IDE finished hello */
    DE__ED_EVENT_REQUEST,       /**< a request for the frame */
    DE__ED_EVENT_DROPPED,       /**< the connection of `epoch` ended */
} de__ed_event_type;

/** One inbound event. `bytes` stay valid until de__ed_ipc_pop(). */
typedef struct de__ed_event {
    de__ed_event_type type;
    u32 epoch;
    u16 major, minor;  /**< CONNECTED: the agreed protocol version */
    bool crash_armed;  /**< CONNECTED: the crash notification is ready */
    const char *why;   /**< DROPPED: a static reason */
    const char *bytes; /**< REQUEST: the frame payload */
    size_t len;
} de__ed_event;

/** The hello handshake, on the service thread. Writes the reply (or nothing) into `w`; true
 * accepts the IDE. Reads only `s` and state that is fixed after start. */
typedef bool (*de__ed_ipc_hello_fn)(void *user, const de__ed_snapshot *s, u32 epoch,
                                    const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w,
                                    u16 *major, u16 *minor);
/** A small read, on the service thread: true when it wrote the reply into `w`; false queues the
 * request to the frame. Reads only `s` and state that is fixed after start. */
typedef bool (*de__ed_ipc_fast_fn)(void *user, const de__ed_snapshot *s, const de_ep_request *req,
                                   de_ep_arena *a, mpack_writer_t *w);

typedef struct de__ed_ipc_desc {
    const char *endpoint;
    size_t max_frame; /**< the largest frame received */
    size_t out_max;   /**< the largest payload sent */
    de__ed_ipc_hello_fn hello;
    de__ed_ipc_fast_fn fast;
    void *user;
    const de__ed_snapshot *initial; /**< the first snapshot */
} de__ed_ipc_desc;

typedef struct de__ed_ipc de__ed_ipc;

/** Listens on `endpoint` and starts the service thread. */
de_result de__ed_ipc_start(const de__ed_ipc_desc *desc, de__ed_ipc **out);
/** Sends what is queued, closes the connection, stops the thread and closes the listener. */
void de__ed_ipc_stop(de__ed_ipc *ipc);

/** Frame: replaces the snapshot. */
void de__ed_ipc_publish(de__ed_ipc *ipc, const de__ed_snapshot *s);
/** Frame: the next inbound event, or false. */
bool de__ed_ipc_peek(de__ed_ipc *ipc, de__ed_event *ev);
/** Frame: releases the event from de__ed_ipc_peek(). */
void de__ed_ipc_pop(de__ed_ipc *ipc);
/** Frame: queues one frame for connection `epoch`, publishing `s` first (NULL: keep). Waits
 * while the queue is full. False: the payload is larger than out_max. */
bool de__ed_ipc_send(de__ed_ipc *ipc, u32 epoch, u8 kind, const void *bytes, size_t n,
                     const de__ed_snapshot *s);
/** Frame: closes connection `epoch` after what is already queued for it. */
void de__ed_ipc_drop(de__ed_ipc *ipc, u32 epoch);
/** True while the thread has a connection (with or without hello). */
bool de__ed_ipc_linked(const de__ed_ipc *ipc);

DE_EXTERN_C_END

#endif /* DE_HOST_EDITOR_IPC_H */
