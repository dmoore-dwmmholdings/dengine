/* editor_ipc.c: the editor service's IPC thread and its two queues (DENG-784). See editor_ipc.h. */
#include "host/editor/editor_ipc.h"
#include "host/editor/editor_crash.h"

#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "de_editor_transport.h"
#include "platform/de_atomic.h"

#include <string.h>

/* The thread waits this long in recv while connected: the most a frame's reply waits to be sent.
 * A small read is answered as soon as it arrives (recv returns on data). */
#define IPC_RECV_TIMEOUT_MS   1u
#define IPC_ACCEPT_TIMEOUT_MS 2u
#define IPC_FULL_WAIT_NS      1000000ull
#define IPC_SMALL_CAP         (64u << 10) /* hello and small-read arena and reply */
#define IPC_SLACK             (1u << 20)  /* queue room beyond the largest record */
/* IPC: "A second `hello` while a connection is live is refused with `conflict`." */
#define IPC_RIVAL_WAIT_NS 5000000000ull

/* ---------- A byte queue of records (one producer, one consumer, under the ipc mutex) ----------
 */

enum { REC_REQUEST = 1, REC_CONNECTED, REC_DROPPED, REC_SEND, REC_DROP };

typedef struct ipc_rec {
    u32 size; /**< the whole record, 16-byte multiple */
    u32 epoch;
    u8 type, kind;
    bool crash_armed;
    u8 pad;
    u16 major, minor;
    u64 len;
    u64 seq; /**< SEND: the next notification sequence number after this frame */
    const char *why;
} ipc_rec;

#define REC_HEAD (((sizeof(ipc_rec)) + 15u) & ~(size_t)15u)

typedef struct ipc_ring {
    unsigned char *base;
    size_t cap;
    size_t head, tail, wrap_end;
    bool wrapped; /* data is [head, wrap_end) then [0, tail) */
    u32 count;
} ipc_ring;

static size_t rec_size(size_t len) {
    return (REC_HEAD + len + 15u) & ~(size_t)15u;
}

static ipc_rec *ring_alloc(ipc_ring *r, size_t size) {
    if (r->count == 0) {
        r->head = r->tail = 0;
        r->wrapped = false;
    }
    unsigned char *p = NULL;
    if (!r->wrapped) {
        if (r->cap - r->tail >= size) {
            p = r->base + r->tail;
            r->tail += size;
        } else if (r->head >= size) {
            r->wrap_end = r->tail;
            r->wrapped = true;
            p = r->base;
            r->tail = size;
        }
    } else if (r->head - r->tail >= size) {
        p = r->base + r->tail;
        r->tail += size;
    }
    if (p == NULL)
        return NULL;
    ++r->count;
    ipc_rec *rec = (ipc_rec *)p;
    memset(rec, 0, sizeof *rec);
    rec->size = (u32)size;
    return rec;
}

static ipc_rec *ring_front(const ipc_ring *r) {
    return r->count ? (ipc_rec *)(r->base + r->head) : NULL;
}

static void ring_pop(ipc_ring *r) {
    if (r->count == 0)
        return;
    r->head += ((const ipc_rec *)(r->base + r->head))->size;
    if (--r->count == 0) {
        r->head = r->tail = 0;
        r->wrapped = false;
    } else if (r->wrapped && r->head == r->wrap_end) {
        r->head = 0;
        r->wrapped = false;
    }
}

/* ---------- The service ---------- */

struct de__ed_ipc {
    de__ed_ipc_desc desc;

    de_mutex mu;
    de_cond in_room, out_room;
    ipc_ring in, out;         /* thread -> frame, frame -> thread */
    de__ed_snapshot snapshot; /* guarded by mu */

    de_thread *thread;
    de_atomic_u32 stop, linked;

    /* Owned by the thread. */
    de_ep_listener listener;
    de_ep_conn conn, rival;
    bool rival_open, authed;
    const char *broken; /* the link failed or the frame dropped it: close at the next step */
    u64 rival_since_ns;
    u32 epoch;
    u64 next_seq;
    char *recv;
    unsigned char *small_arena;
    char *small_out;
};

static const de_allocator *ipc_al(void) {
    return de_heap_allocator();
}

static void *ipc_payload(ipc_rec *r) {
    return (unsigned char *)r + REC_HEAD;
}

static void flush_out(de__ed_ipc *ipc);

/* Thread: queues an event for the frame. Waits while the queue is full, sending what the frame
 * queued meanwhile so that neither side blocks the other. */
static bool post(de__ed_ipc *ipc, u8 type, const char *why, u16 major, u16 minor, bool armed,
                 const void *bytes, size_t len) {
    const size_t size = rec_size(len);
    if (size > ipc->in.cap)
        return false;
    for (;;) {
        de_mutex_lock(&ipc->mu);
        ipc_rec *r = ring_alloc(&ipc->in, size);
        if (r != NULL) {
            r->type = type;
            r->epoch = ipc->epoch;
            r->why = why;
            r->major = major;
            r->minor = minor;
            r->crash_armed = armed;
            r->len = len;
            if (len)
                memcpy(ipc_payload(r), bytes, len);
            de_mutex_unlock(&ipc->mu);
            return true;
        }
        de_mutex_unlock(&ipc->mu);
        if (de_atomic_load_u32(&ipc->stop, DE_ATOMIC_ACQUIRE))
            return false;
        flush_out(ipc);
        de_mutex_lock(&ipc->mu);
        if (ipc->in.count != 0)
            (void)de_cond_wait_timeout(&ipc->in_room, &ipc->mu, IPC_FULL_WAIT_NS);
        de_mutex_unlock(&ipc->mu);
    }
}

static void close_link(de__ed_ipc *ipc, const char *why) {
    if (de__ed_crash_disarm())
        de_ep_disconnect(&ipc->conn);
    ipc->authed = false;
    ipc->broken = NULL;
    de_atomic_store_u32(&ipc->linked, 0, DE_ATOMIC_RELEASE);
    (void)post(ipc, REC_DROPPED, why, 0, 0, false, NULL, 0);
}

static bool send_now(de__ed_ipc *ipc, u8 kind, const void *bytes, size_t n) {
    if (de__ed_crash_send(&ipc->conn, kind, bytes, n, ipc->next_seq) == DE_EP_IO_OK)
        return true;
    ipc->broken = "send failed";
    return false;
}

/* Thread: sends what the frame queued, in order. A failure only marks the link broken; the loop
 * closes it, so a send inside post() never posts. */
static void flush_out(de__ed_ipc *ipc) {
    for (;;) {
        de_mutex_lock(&ipc->mu);
        ipc_rec *r = ring_front(&ipc->out);
        de_mutex_unlock(&ipc->mu);
        if (r == NULL)
            return;
        const bool live = de_atomic_load_u32(&ipc->linked, DE_ATOMIC_RELAXED) && ipc->authed &&
                          ipc->broken == NULL && r->epoch == ipc->epoch;
        if (r->seq > ipc->next_seq)
            ipc->next_seq = r->seq;
        if (live && r->type == REC_DROP)
            ipc->broken = "closed by the engine";
        else if (live && r->type == REC_SEND)
            (void)send_now(ipc, r->kind, ipc_payload(r), (size_t)r->len);
        de_mutex_lock(&ipc->mu);
        ring_pop(&ipc->out);
        de_cond_signal(&ipc->out_room);
        de_mutex_unlock(&ipc->mu);
    }
}

static void read_snapshot(de__ed_ipc *ipc, de__ed_snapshot *s) {
    de_mutex_lock(&ipc->mu);
    *s = ipc->snapshot;
    de_mutex_unlock(&ipc->mu);
    if (s->next_seq > ipc->next_seq)
        ipc->next_seq = s->next_seq;
}

static void rival_close(de__ed_ipc *ipc) {
    if (ipc->rival_open)
        de_ep_disconnect(&ipc->rival);
    ipc->rival_open = false;
}

/* A second client while one is live: answered `conflict` for its first request, then closed.
 * POSIX only: a Windows pipe has one instance, so a second client cannot connect at all. */
static void rival_pump(de__ed_ipc *ipc) {
#ifdef _WIN32
    (void)ipc;
#else
    if (!ipc->rival_open) {
        if (de_ep_accept(&ipc->listener, &ipc->rival, (u32)ipc->desc.max_frame, 0) != DE_EP_IO_OK)
            return;
        ipc->rival_open = true;
        ipc->rival_since_ns = de_time_now_ns();
    }
    u8 kind = 0;
    size_t len = 0;
    const de_ep_io io = de_ep_recv(&ipc->rival, ipc->recv, ipc->desc.max_frame, &kind, &len, 0);
    if (io == DE_EP_IO_TIMEOUT) {
        if (de_time_now_ns() - ipc->rival_since_ns > IPC_RIVAL_WAIT_NS)
            rival_close(ipc);
        return;
    }
    de_ep_request req;
    if (io == DE_EP_IO_OK && kind == DE_EP_KIND_REQUEST &&
        de_ep_read_request(ipc->recv, len, &req)) {
        mpack_writer_t w;
        mpack_writer_init(&w, ipc->small_out, IPC_SMALL_CAP);
        de_ep_write_error(&w, req.id, DE_EP_ERR_CONFLICT, "another IDE is connected");
        const size_t used = mpack_writer_buffer_used(&w);
        if (mpack_writer_destroy(&w) == mpack_ok)
            (void)de_ep_send(&ipc->rival, DE_EP_KIND_RESPONSE, ipc->small_out, used);
    }
    rival_close(ipc);
#endif
}

/* Thread: hello on a new link. */
static void on_hello(de__ed_ipc *ipc, const de_ep_request *req) {
    de__ed_snapshot s;
    read_snapshot(ipc, &s);
    de_ep_arena a = {ipc->small_arena, IPC_SMALL_CAP, 0};
    mpack_writer_t w;
    mpack_writer_init(&w, ipc->small_out, IPC_SMALL_CAP);
    u16 major = 0, minor = 0;
    const bool ok = ipc->desc.hello(ipc->desc.user, &s, ipc->epoch, req, &a, &w, &major, &minor);
    const size_t used = mpack_writer_buffer_used(&w);
    const bool written = mpack_writer_destroy(&w) == mpack_ok && used > 0;
    if (written && !send_now(ipc, DE_EP_KIND_RESPONSE, ipc->small_out, used))
        return; /* the loop closes the broken link */
    if (!ok || !written) {
        ipc->broken = "handshake refused";
        return;
    }
    ipc->authed = true;
    const bool armed = de__ed_crash_arm(&ipc->conn, ipc->next_seq);
    (void)post(ipc, REC_CONNECTED, NULL, major, minor, armed, NULL, 0);
}

/* Thread: one received frame on an authenticated link. */
static void on_request(de__ed_ipc *ipc, const de_ep_request *req, size_t len) {
    de__ed_snapshot s;
    read_snapshot(ipc, &s);
    de_ep_arena a = {ipc->small_arena, IPC_SMALL_CAP, 0};
    mpack_writer_t w;
    mpack_writer_init(&w, ipc->small_out, IPC_SMALL_CAP);
    if (ipc->desc.fast(ipc->desc.user, &s, req, &a, &w)) {
        const size_t used = mpack_writer_buffer_used(&w);
        if (mpack_writer_destroy(&w) == mpack_ok) {
            (void)send_now(ipc, DE_EP_KIND_RESPONSE, ipc->small_out, used);
            return;
        }
        /* Too large for the small buffer: let the frame answer it. */
    } else {
        (void)mpack_writer_destroy(&w);
    }
    (void)post(ipc, REC_REQUEST, NULL, 0, 0, false, ipc->recv, len);
}

static i32 ipc_main(void *user) {
    de__ed_ipc *ipc = (de__ed_ipc *)user;
    while (!de_atomic_load_u32(&ipc->stop, DE_ATOMIC_ACQUIRE)) {
        if (de__ed_crash_frozen()) { /* a fatal report owns the connection */
            de_sleep_ns(IPC_FULL_WAIT_NS);
            continue;
        }
        flush_out(ipc);
        const bool linked = de_atomic_load_u32(&ipc->linked, DE_ATOMIC_RELAXED) != 0;
        if (linked && ipc->broken != NULL) {
            close_link(ipc, ipc->broken);
            continue;
        }
        if (!linked) {
            if (de_ep_accept(&ipc->listener, &ipc->conn, (u32)ipc->desc.max_frame,
                             IPC_ACCEPT_TIMEOUT_MS) == DE_EP_IO_OK) {
                ++ipc->epoch;
                ipc->authed = false;
                ipc->broken = NULL;
                de_atomic_store_u32(&ipc->linked, 1, DE_ATOMIC_RELEASE);
            }
            continue;
        }
        rival_pump(ipc);
        u8 kind = 0;
        size_t len = 0;
        const de_ep_io io = de_ep_recv(&ipc->conn, ipc->recv, ipc->desc.max_frame, &kind, &len,
                                       IPC_RECV_TIMEOUT_MS);
        if (io == DE_EP_IO_TIMEOUT)
            continue;
        if (io != DE_EP_IO_OK) {
            ipc->broken = io == DE_EP_IO_CLOSED ? "closed" : "read failed";
            continue;
        }
        de_ep_request req;
        if (kind != DE_EP_KIND_REQUEST || !de_ep_read_request(ipc->recv, len, &req)) {
            ipc->broken = "not a request";
            continue;
        }
        if (!ipc->authed)
            on_hello(ipc, &req);
        else
            on_request(ipc, &req, len);
    }
    /* Stopping: send what the frame queued (a goodbye reply), then close. */
    flush_out(ipc);
    if (de_atomic_load_u32(&ipc->linked, DE_ATOMIC_RELAXED)) {
        if (de__ed_crash_disarm())
            de_ep_disconnect(&ipc->conn);
        de_atomic_store_u32(&ipc->linked, 0, DE_ATOMIC_RELEASE);
    }
    rival_close(ipc);
    return 0;
}

/* ---------- The frame's side ---------- */

static void ipc_free(de__ed_ipc *ipc) {
    const de_allocator *al = ipc_al();
    if (ipc->in.base)
        DE_FREE(al, ipc->in.base, ipc->in.cap);
    if (ipc->out.base)
        DE_FREE(al, ipc->out.base, ipc->out.cap);
    if (ipc->recv)
        DE_FREE(al, ipc->recv, ipc->desc.max_frame);
    if (ipc->small_arena)
        DE_FREE(al, ipc->small_arena, IPC_SMALL_CAP);
    if (ipc->small_out)
        DE_FREE(al, ipc->small_out, IPC_SMALL_CAP);
    de_cond_destroy(&ipc->in_room);
    de_cond_destroy(&ipc->out_room);
    de_mutex_destroy(&ipc->mu);
    DE_FREE(al, ipc, sizeof *ipc);
}

de_result de__ed_ipc_start(const de__ed_ipc_desc *desc, de__ed_ipc **out) {
    *out = NULL;
    const de_allocator *al = ipc_al();
    de__ed_ipc *ipc = DE_NEW(al, de__ed_ipc, DE_MEM_GENERAL);
    if (ipc == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "editor IPC");
    memset(ipc, 0, sizeof *ipc);
    ipc->desc = *desc;
    de_mutex_init(&ipc->mu);
    de_cond_init(&ipc->in_room);
    de_cond_init(&ipc->out_room);
    ipc->in.cap = rec_size(desc->max_frame) + IPC_SLACK;
    ipc->out.cap = rec_size(desc->out_max) + IPC_SLACK;
    ipc->in.base = (unsigned char *)DE_ALLOC(al, ipc->in.cap, 16, DE_MEM_GENERAL);
    ipc->out.base = (unsigned char *)DE_ALLOC(al, ipc->out.cap, 16, DE_MEM_GENERAL);
    ipc->recv = (char *)DE_ALLOC(al, desc->max_frame, 16, DE_MEM_GENERAL);
    ipc->small_arena = (unsigned char *)DE_ALLOC(al, IPC_SMALL_CAP, 16, DE_MEM_GENERAL);
    ipc->small_out = (char *)DE_ALLOC(al, IPC_SMALL_CAP, 16, DE_MEM_GENERAL);
    if (!ipc->in.base || !ipc->out.base || !ipc->recv || !ipc->small_arena || !ipc->small_out) {
        ipc_free(ipc);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "editor IPC buffers");
    }
    if (desc->initial != NULL)
        ipc->snapshot = *desc->initial;
    ipc->next_seq = ipc->snapshot.next_seq ? ipc->snapshot.next_seq : 1;
    de_atomic_init_u32(&ipc->stop, 0);
    de_atomic_init_u32(&ipc->linked, 0);
    if (de_ep_listen(&ipc->listener, desc->endpoint) != DE_EP_IO_OK) {
        ipc_free(ipc);
        return DE_FAIL(DE_ERR_IO, "editor service: cannot listen on %s", desc->endpoint);
    }
    const de_result r =
        de_thread_create(ipc_main, ipc, "editor-ipc", DE_THREAD_NORMAL, 0, &ipc->thread);
    if (r != DE_OK) {
        if (de__ed_crash_disarm())
            de_ep_listener_close(&ipc->listener);
        ipc_free(ipc);
        return DE_FAIL_WRAP(r, "editor IPC thread");
    }
    *out = ipc;
    return DE_OK;
}

void de__ed_ipc_stop(de__ed_ipc *ipc) {
    if (ipc == NULL)
        return;
    de_atomic_store_u32(&ipc->stop, 1, DE_ATOMIC_RELEASE);
    de_mutex_lock(&ipc->mu);
    de_cond_broadcast(&ipc->in_room);
    de_mutex_unlock(&ipc->mu);
    (void)de_thread_join(ipc->thread);
    if (de__ed_crash_disarm())
        de_ep_listener_close(&ipc->listener);
    ipc_free(ipc);
}

void de__ed_ipc_publish(de__ed_ipc *ipc, const de__ed_snapshot *s) {
    de_mutex_lock(&ipc->mu);
    ipc->snapshot = *s;
    de_mutex_unlock(&ipc->mu);
}

bool de__ed_ipc_peek(de__ed_ipc *ipc, de__ed_event *ev) {
    de_mutex_lock(&ipc->mu);
    ipc_rec *r = ring_front(&ipc->in);
    de_mutex_unlock(&ipc->mu);
    if (r == NULL)
        return false;
    memset(ev, 0, sizeof *ev);
    ev->type = r->type == REC_CONNECTED ? DE__ED_EVENT_CONNECTED
               : r->type == REC_DROPPED ? DE__ED_EVENT_DROPPED
                                        : DE__ED_EVENT_REQUEST;
    ev->epoch = r->epoch;
    ev->major = r->major;
    ev->minor = r->minor;
    ev->crash_armed = r->crash_armed;
    ev->why = r->why;
    ev->bytes = (const char *)ipc_payload(r);
    ev->len = (size_t)r->len;
    return true;
}

void de__ed_ipc_pop(de__ed_ipc *ipc) {
    de_mutex_lock(&ipc->mu);
    ring_pop(&ipc->in);
    de_cond_signal(&ipc->in_room);
    de_mutex_unlock(&ipc->mu);
}

static bool queue_out(de__ed_ipc *ipc, u8 type, u32 epoch, u8 kind, const void *bytes, size_t n,
                      const de__ed_snapshot *s) {
    const size_t size = rec_size(n);
    if (size > ipc->out.cap)
        return false;
    de_mutex_lock(&ipc->mu);
    if (s != NULL)
        ipc->snapshot = *s;
    ipc_rec *r;
    while ((r = ring_alloc(&ipc->out, size)) == NULL)
        (void)de_cond_wait_timeout(&ipc->out_room, &ipc->mu, IPC_FULL_WAIT_NS);
    r->type = type;
    r->epoch = epoch;
    r->kind = kind;
    r->len = n;
    r->seq = s != NULL ? s->next_seq : 0;
    if (n)
        memcpy(ipc_payload(r), bytes, n);
    de_mutex_unlock(&ipc->mu);
    return true;
}

bool de__ed_ipc_send(de__ed_ipc *ipc, u32 epoch, u8 kind, const void *bytes, size_t n,
                     const de__ed_snapshot *s) {
    return queue_out(ipc, REC_SEND, epoch, kind, bytes, n, s);
}

void de__ed_ipc_drop(de__ed_ipc *ipc, u32 epoch) {
    (void)queue_out(ipc, REC_DROP, epoch, 0, NULL, 0, NULL);
}

bool de__ed_ipc_linked(const de__ed_ipc *ipc) {
    return de_atomic_load_u32(&ipc->linked, DE_ATOMIC_ACQUIRE) != 0;
}
