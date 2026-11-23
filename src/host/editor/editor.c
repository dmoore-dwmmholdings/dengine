/* editor.c: the editor service's handshake, dispatch and frame side (DENG-623). The connection
 * itself runs on the IPC thread (editor_ipc.c, DENG-784). */
#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS /* getenv, vsnprintf */
#endif
#include "host/editor/editor_internal.h"
#include "host/editor/editor_crash.h"
#include "host/editor/editor_ipc.h"

#include <dengine/de_gpu.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "host/host_internal.h"
#if DE_HAS_RENDER
#include "render/loaders/render_assets.h"
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* DENG-555: the largest frame either way, and hello's limits.frame_size. The IDE uses the value it
 * is sent for its reads and its writes, so it must hold for both: requests up to it are read
 * whole and no response is larger. Below the protocol default (64 MiB), which IPC "Limits" lets
 * an engine lower ("so they can change in a minor version"). */
#define ED_FRAME_CAP          (32u << 20)
#define ED_IN_CAP             ED_FRAME_CAP
#define ED_OUT_CAP            (ED_FRAME_CAP - 1u) /* a payload; the frame adds its kind byte */
#define ED_NOTE_CAP           (8u << 20)
_Static_assert(ED_NOTE_CAP <= ED_FRAME_CAP, "a notification must fit in a frame");
_Static_assert(ED_FRAME_CAP <= DE_EP_LIMIT_FRAME_SIZE, "no frame over the protocol default");
#define ED_ARENA_CAP          (8u << 20)
#define ED_BLOB_PART          (1u << 20)
#define ED_REQUESTS_PER_FRAME 256u
#define ED_DEFAULT_GRACE_S    30u

static const char *arg_value(const de__host *h, const char *opt) {
    const size_t n = strlen(opt);
    for (int i = 1; i < h->desc.argc && h->desc.argv != NULL; ++i) {
        const char *a = h->desc.argv[i];
        if (a == NULL || strncmp(a, opt, n) != 0)
            continue;
        if (a[n] == '=')
            return a + n + 1;
        if (a[n] == '\0' && i + 1 < h->desc.argc)
            return h->desc.argv[i + 1];
    }
    return NULL;
}

/* ---------- Helpers for the areas ---------- */

void de__ed_on(de__editor *ed, de_ep_method m, de__ed_handler fn) {
    if ((u32)m < DE_EP_METHOD_COUNT)
        ed->handlers[m] = fn;
}

void de__ed_on_tick(de__editor *ed, de__ed_tick_fn fn) {
    if (ed->tick_count < DE__ED_MAX_TICKS)
        ed->ticks[ed->tick_count++] = fn;
}

void de__ed_on_reset(de__editor *ed, de__ed_reset_fn fn) {
    if (ed->reset_count < DE__ED_MAX_TICKS)
        ed->resets[ed->reset_count++] = fn;
}

void de__ed_add_capability(de__editor *ed, const char *cap) {
    if (ed->cap_count < DE__ED_MAX_CAPS)
        ed->caps[ed->cap_count++] = cap;
}

void de__ed_error(mpack_writer_t *w, u64 id, de_ep_error code, const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (code != DE_EP_ERR_BUSY) {
        de_ep_write_error(w, id, code, msg);
        return;
    }
    /* IPC: "A request over a limit gets `busy` with `data: { retryAfterMs }`." */
    mpack_start_map(w, 2);
    mpack_write_cstr(w, "id");
    mpack_write_u64(w, id);
    mpack_write_cstr(w, "error");
    mpack_start_map(w, 3);
    mpack_write_cstr(w, "code");
    mpack_write_i32(w, (int32_t)code);
    mpack_write_cstr(w, "message");
    mpack_write_cstr(w, msg);
    mpack_write_cstr(w, "data");
    mpack_start_map(w, 1);
    mpack_write_cstr(w, "retryAfterMs");
    mpack_write_u32(w, DE__ED_RETRY_AFTER_MS);
    mpack_finish_map(w);
    mpack_finish_map(w);
    mpack_finish_map(w);
}

void de__ed_defer(de__editor *ed) {
    ed->deferred = true;
}

void de__ed_respond_begin(de__editor *ed, mpack_writer_t *w) {
    mpack_writer_init(w, ed->out, ed->out_cap);
}

u64 de__ed_note_begin(de__editor *ed, mpack_writer_t *w) {
    mpack_writer_init(w, ed->note, ed->note_cap);
    return ++ed->seq;
}

static void ed_snapshot(de__editor *ed, de__ed_snapshot *s);

/* Queues the frame to the IPC thread. The snapshot goes first, so a small read sent after this
 * reply arrives sees the state the reply describes. A failed send reaches the frame as a
 * dropped connection. */
bool de__ed_send(de__editor *ed, mpack_writer_t *w, u8 kind) {
    const char *buf = w->buffer;
    const size_t n = mpack_writer_buffer_used(w);
    const mpack_error_t err = mpack_writer_destroy(w);
    if (err != mpack_ok) {
        DE_WARN(ed->log, "editor: a message did not fit (%s)", mpack_error_to_string(err));
        return ed->connected;
    }
    if (!ed->connected || ed->ipc == NULL)
        return false;
    de__ed_snapshot s;
    ed_snapshot(ed, &s);
    return de__ed_ipc_send(ed->ipc, ed->epoch, kind, buf, n, &s);
}

u64 de__ed_send_blob(de__editor *ed, const void *bytes, size_t n) {
    if (!ed->connected)
        return 0;
    const u64 id = ++ed->blob_next;
    const unsigned char *p = (const unsigned char *)bytes;
    u32 part = 0;
    size_t off = 0;
    do {
        const size_t chunk = n - off > ED_BLOB_PART ? ED_BLOB_PART : n - off;
        mpack_writer_t w;
        mpack_writer_init(&w, ed->note, ed->note_cap);
        mpack_build_map(&w);
        mpack_write_cstr(&w, "blobId");
        mpack_write_u64(&w, id);
        mpack_write_cstr(&w, "part");
        mpack_write_u32(&w, part++);
        mpack_write_cstr(&w, "last");
        mpack_write_bool(&w, off + chunk >= n);
        mpack_write_cstr(&w, "bytes");
        mpack_write_bin(&w, (const char *)p + off, (u32)chunk);
        mpack_complete_map(&w);
        if (!de__ed_send(ed, &w, DE_EP_KIND_BLOB))
            return 0;
        off += chunk;
    } while (off < n);
    return id;
}

u64 de__ed_frame(const de__editor *ed) {
    return ed->h->loop_frames;
}

void *de__ed_alloc(de_ep_arena *a, size_t size) {
    size_t at = (a->used + 15u) & ~(size_t)15u;
    if (at + size > a->size)
        return NULL;
    a->used = at + size;
    return a->base + at;
}

char *de__ed_cstr(de_ep_arena *a, de_ep_str s) {
    char *c = (char *)de__ed_alloc(a, (size_t)s.len + 1);
    if (c == NULL)
        return NULL;
    memcpy(c, s.ptr, s.len);
    c[s.len] = '\0';
    return c;
}

void de__ed_raw_begin(de_ep_arena *a, mpack_writer_t *w) {
    const size_t at = (a->used + 15u) & ~(size_t)15u;
    const size_t room = at < a->size ? a->size - at : 0;
    mpack_writer_init(w, (char *)a->base + at, room);
}

de_ep_raw de__ed_raw_end(de_ep_arena *a, mpack_writer_t *w) {
    de_ep_raw r = {0};
    const char *start = w->buffer;
    const size_t n = mpack_writer_buffer_used(w);
    if (mpack_writer_destroy(w) != mpack_ok)
        return r;
    a->used = (size_t)((const unsigned char *)start - a->base) + n;
    r.ptr = start;
    r.len = n;
    return r;
}

de_ep_raw de__ed_raw_str(de_ep_arena *a, const char *s) {
    mpack_writer_t w;
    de__ed_raw_begin(a, &w);
    mpack_write_cstr(&w, s != NULL ? s : "");
    return de__ed_raw_end(a, &w);
}

de_ep_raw de__ed_raw_f64(de_ep_arena *a, f64 v) {
    mpack_writer_t w;
    de__ed_raw_begin(a, &w);
    mpack_write_double(&w, v);
    return de__ed_raw_end(a, &w);
}

de_ep_raw de__ed_raw_bool(de_ep_arena *a, bool v) {
    mpack_writer_t w;
    de__ed_raw_begin(a, &w);
    mpack_write_bool(&w, v);
    return de__ed_raw_end(a, &w);
}

de_ep_raw de__ed_raw_range(de_ep_arena *a, f64 lo, f64 hi) {
    mpack_writer_t w;
    de__ed_raw_begin(a, &w);
    mpack_start_array(&w, 2);
    mpack_write_double(&w, lo);
    mpack_write_double(&w, hi);
    mpack_finish_array(&w);
    return de__ed_raw_end(a, &w);
}

bool de__ed_raw_map_entry(mpack_reader_t *r, de_ep_kv_raw *out) {
    memset(out, 0, sizeof *out);
    if (r->buffer || r->fill || r->skip) {
        mpack_reader_flag_error(r, mpack_error_unsupported);
        return false;
    }
    const u32 length = mpack_expect_str(r);
    const char *key = length ? mpack_read_bytes_inplace(r, length) : "";
    mpack_done_str(r);
    /* This pinned MPack reader exposes its contiguous cursor. reader_remaining()
     * is only valid after closing every container, including our containing map. */
    const char *begin = r->data;
    mpack_discard(r);
    const char *end = r->data;
    if (mpack_reader_error(r) != mpack_ok || !begin || !end || end <= begin)
        return false;
    out->key = (de_ep_str){key, length};
    out->value = (de_ep_raw){begin, (size_t)(end - begin)};
    return true;
}

/* ---------- Connection ---------- */

/* The connection ended: drop the state tied to it. */
static void ed_reset(de__editor *ed, const char *why) {
    if (!ed->connected)
        return;
    DE_INFO(ed->log, "editor: IDE disconnected (%s)", why);
    ed->connected = ed->authed = false;
    ed->proto_major = ed->proto_minor = 0;
    ed->idle_since_ns = de_time_now_ns();
    for (u32 i = 0; i < ed->reset_count; ++i)
        ed->resets[i](ed);
}

/* The frame ends the connection: the IPC thread closes it after what is already queued. */
static void ed_drop(de__editor *ed, const char *why) {
    if (!ed->connected)
        return;
    if (ed->ipc != NULL)
        de__ed_ipc_drop(ed->ipc, ed->epoch);
    ed_reset(ed, why);
}

static void ed_build_info(de__editor *ed, de_ep_build_info *b) {
    static char version[32];
    snprintf(version, sizeof version, "%d.%d.%d", DE_VERSION_MAJOR, DE_VERSION_MINOR,
             DE_VERSION_PATCH);
    /* A game generation changes only after a committed library swap. It identifies this
     * process's loaded game instance, not a source-content hash or executable build hash. */
    static char game_build[512];
    if (ed->h->game.lib) {
        snprintf(game_build, sizeof game_build, "%s#game%u", ed->h->config.name,
                 ed->h->game.generation);
        b->id = de_ep_cstr(game_build);
    } else
        b->id = de_ep_cstr(ed->h->config.name);
    b->engine_version = de_ep_cstr(version);
    b->gpu_backend = de_ep_cstr("none");
#if DE_HAS_GPU
    if (ed->e->gpu != NULL)
        b->gpu_backend = de_ep_cstr(de_gpu_backend_name(de_gpu_get_caps(ed->e->gpu)->backend));
#endif
    b->config = de_ep_cstr("dev");
}

void de__ed_session_shutdown(void);
void de__ed_logs_shutdown(void);
void de__ed_views_shutdown(de__editor *ed);
void de__ed_session_build_info(de__editor *ed, de_ep_build_info *b);
void de__ed_session_build_info(de__editor *ed, de_ep_build_info *b) {
    ed_build_info(ed, b);
}

static void copy_str(char *dst, size_t cap, de_ep_str v) {
    const size_t n = v.len < cap - 1 ? v.len : cap - 1;
    memcpy(dst, v.ptr, n);
    dst[n] = '\0';
}

/* The frame-boundary copy for the IPC thread's small reads (DENG-784). */
static void ed_snapshot(de__editor *ed, de__ed_snapshot *s) {
    de_ep_build_info b;
    memset(&b, 0, sizeof b);
    ed_build_info(ed, &b);
    copy_str(s->build_id, sizeof s->build_id, b.id);
    copy_str(s->engine_version, sizeof s->engine_version, b.engine_version);
    copy_str(s->gpu_backend, sizeof s->gpu_backend, b.gpu_backend);
    copy_str(s->config, sizeof s->config, b.config);
    s->mode = ed->mode;
    s->frame = de__ed_frame(ed);
    s->next_seq = ed->seq + 1;
}

static void snapshot_build(const de__ed_snapshot *s, de_ep_build_info *b) {
    b->id = de_ep_cstr(s->build_id);
    b->engine_version = de_ep_cstr(s->engine_version);
    b->gpu_backend = de_ep_cstr(s->gpu_backend);
    b->config = de_ep_cstr(s->config);
}

/* Capabilities are fixed once the service starts, so the IPC thread may read them. */
static void hello_result(const de__editor *ed, const de__ed_snapshot *s, u32 epoch,
                         const char *agreed, de_ep_str *caps, de_ep_hello_result *r) {
    for (u32 i = 0; i < ed->cap_count; ++i)
        caps[i] = de_ep_cstr(ed->caps[i]);
    memset(r, 0, sizeof *r);
    r->protocol = de_ep_cstr(agreed);
    r->capabilities.items = caps;
    r->capabilities.count = ed->cap_count;
    snapshot_build(s, &r->build);
    r->limits.requests_in_flight = DE_EP_LIMIT_REQUESTS_IN_FLIGHT;
    r->limits.subscriptions = DE_EP_LIMIT_SUBSCRIPTIONS;
    r->limits.query_page_size = DE_EP_LIMIT_QUERY_PAGE_SIZE;
    r->limits.open_transactions = DE_EP_LIMIT_OPEN_TRANSACTIONS;
    r->limits.frame_size = ED_FRAME_CAP; /* what the engine really reads and writes */
    r->epoch = epoch;
    r->has_schema = true;
    r->schema = de_ep_cstr(DE_EP_SCHEMA_HASH);
    /* Required from protocol 1.9: the native version the schema hash belongs to. */
    r->has_server_protocol = true;
    r->server_protocol = de_ep_cstr(DE_EP_PROTOCOL_VERSION);
}

/* The hello result for the current state (frame thread). The strings live until the next call. */
void de__ed_hello_result(de__editor *ed, const char *agreed, de_ep_str *caps,
                         de_ep_hello_result *r) {
    static de__ed_snapshot s;
    ed_snapshot(ed, &s);
    hello_result(ed, &s, ed->epoch, agreed, caps, r);
}

/* The first request: hello, on the IPC thread. False closes the connection. Reads only the
 * token and capabilities (fixed after start) and the snapshot. */
static bool ed_hello(void *user, const de__ed_snapshot *s, u32 epoch, const de_ep_request *req,
                     de_ep_arena *a, mpack_writer_t *w, u16 *major_out, u16 *minor_out) {
    const de__editor *ed = (const de__editor *)user;
    de_ep_hello_params p;
    memset(&p, 0, sizeof p);
    if (de_ep_method_parse(req->method) != DE_EP_METHOD_HELLO ||
        !de_ep_read_params_hello(req, a, &p))
        return false; /* anything but hello first closes the connection */
    if (!de_ep_token_eq(p.token, de_ep_cstr(ed->token))) {
        de_ep_write_error(w, req->id, DE_EP_ERR_UNAUTHORIZED, "bad session token");
        return false;
    }
    if (p.has_schema && de_ep_str_eq(p.protocol, DE_EP_PROTOCOL_VERSION) &&
        !de_ep_str_eq(p.schema, DE_EP_SCHEMA_HASH)) {
        de_ep_write_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "same version, different schema");
        return false;
    }
    char agreed[16];
    if (!de_ep_negotiate(de_ep_cstr(DE_EP_PROTOCOL_VERSION), p.protocol, agreed)) {
        de_ep_write_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "unsupported protocol version");
        return false;
    }
    de_ep_str caps[DE__ED_MAX_CAPS];
    de_ep_hello_result r;
    hello_result(ed, s, epoch, agreed, caps, &r);
    de_ep_write_response_hello(w, req->id, &r);
    unsigned major = 0, minor = 0;
    if (sscanf(agreed, "%u.%u", &major, &minor) != 2 || major > 0xffffu || minor > 0xffffu)
        major = minor = 0;
    *major_out = (u16)major; /* DENG-785: gates the 1.15 methods */
    *minor_out = (u16)minor;
    return true;
}

/* Small reads on the IPC thread (DENG-784): `ping` and `session.info` from the snapshot. Every
 * other request goes to the frame. */
static bool ed_fast(void *user, const de__ed_snapshot *s, const de_ep_request *req, de_ep_arena *a,
                    mpack_writer_t *w) {
    (void)a;
    const de__editor *ed = (const de__editor *)user;
    const de_ep_method m = de_ep_method_parse(req->method);
    if ((m != DE_EP_METHOD_PING && m != DE_EP_METHOD_SESSION_INFO) || !de__ed_method_offered(ed, m))
        return false;
    if (m == DE_EP_METHOD_PING) {
        de_ep_empty e;
        memset(&e, 0, sizeof e);
        de_ep_write_response_ping(w, req->id, &e);
        return true;
    }
    de_ep_str caps[DE__ED_MAX_CAPS];
    for (u32 i = 0; i < ed->cap_count; ++i)
        caps[i] = de_ep_cstr(ed->caps[i]);
    de_ep_session_info info;
    memset(&info, 0, sizeof info);
    snapshot_build(s, &info.build);
    info.capabilities.items = caps;
    info.capabilities.count = ed->cap_count;
    info.mode = s->mode;
    info.frame = s->frame;
    de_ep_write_response_session_info(w, req->id, &info);
    return true;
}

bool de__ed_method_offered(const de__editor *ed, de_ep_method m) {
    if ((u32)m >= DE_EP_METHOD_COUNT || ed->handlers[m] == NULL)
        return false;
    const char *cap = de_ep_method_capability(m);
    if (cap == NULL || cap[0] == '\0')
        return true;
    for (u32 i = 0; i < ed->cap_count; ++i)
        if (strcmp(ed->caps[i], cap) == 0)
            return true;
    return false;
}

static void ed_dispatch(de__editor *ed, const de_ep_request *req, de_ep_arena *a) {
    const de_ep_method m = de_ep_method_parse(req->method);
    mpack_writer_t w;
    mpack_writer_init(&w, ed->out, ed->out_cap);
    if ((u32)m >= DE_EP_METHOD_COUNT) {
        de__ed_error(&w, req->id, DE_EP_ERR_INVALID_PARAMS, "unknown method %.*s",
                     (int)req->method.len, req->method.ptr);
    } else if (m == DE_EP_METHOD_HELLO) {
        de__ed_error(&w, req->id, DE_EP_ERR_CONFLICT, "already connected");
    } else if (!de__ed_method_offered(ed, m)) {
        de__ed_error(&w, req->id, DE_EP_ERR_UNSUPPORTED, "%s is not supported by this engine",
                     de_ep_method_name(m));
    } else {
        ed->deferred = false;
        ed->handlers[m](ed, req, a, &w);
        if (ed->deferred) {
            ed->deferred = false;
            mpack_writer_destroy(&w);
            return;
        }
    }
    if (mpack_writer_buffer_used(&w) == 0) {
        /* The handler wrote nothing: an empty result. */
        de_ep_write_result_begin(&w, req->id);
        mpack_build_map(&w);
        mpack_complete_map(&w);
        de_ep_write_result_end(&w);
    } else if (mpack_writer_error(&w) == mpack_error_too_big) {
        mpack_writer_destroy(&w);
        mpack_writer_init(&w, ed->out, ed->out_cap);
        de__ed_error(&w, req->id, DE_EP_ERR_BUSY, "the result is larger than the frame limit");
    }
    de__ed_send(ed, &w, DE_EP_KIND_RESPONSE);
}

static void ed_connected(de__editor *ed, const de__ed_event *ev) {
    ed_drop(ed, "replaced"); /* never happens: the thread reports the drop first */
    ed->connected = ed->authed = true;
    ed->epoch = ev->epoch;
    ed->proto_major = ev->major; /* DENG-785: gates the 1.15 methods */
    ed->proto_minor = ev->minor;
    ed->reload_outcomes = de__ed_protocol_at_least(ed, 1, 6);
    if (!ev->crash_armed)
        DE_WARN(ed->log, "editor: crash notification unavailable; IDE will use process exit");
    DE_INFO(ed->log, "editor: IDE connected (protocol %u.%u, epoch %u)", (unsigned)ev->major,
            (unsigned)ev->minor, ed->epoch);
}

/* Frame begin: applies what the IPC thread queued, at the frame boundary. Returns true when the
 * IDE said goodbye. */
static bool ed_drain(de__editor *ed) {
    de__ed_event ev;
    for (u32 n = 0; n < ED_REQUESTS_PER_FRAME && de__ed_ipc_peek(ed->ipc, &ev);) {
        const bool current = ed->connected && ev.epoch == ed->epoch;
        if (ev.type == DE__ED_EVENT_CONNECTED) {
            ed_connected(ed, &ev);
        } else if (ev.type == DE__ED_EVENT_DROPPED) {
            if (current)
                ed_reset(ed, ev.why);
            else
                ed->idle_since_ns = de_time_now_ns();
        } else if (current) {
            ++n;
            de_ep_request req;
            if (de_ep_read_request(ev.bytes, ev.len, &req)) {
                de_ep_arena a = {ed->arena, ed->arena_cap, 0};
                const bool goodbye = de_ep_method_parse(req.method) == DE_EP_METHOD_GOODBYE;
                ed_dispatch(ed, &req, &a);
                if (goodbye) {
                    de__ed_ipc_pop(ed->ipc);
                    ed_drop(ed, "goodbye");
                    de_engine_request_quit(ed->e);
                    return true;
                }
            }
        }
        de__ed_ipc_pop(ed->ipc);
    }
    return false;
}

static de_result ed_pump(de__host *h, void *user) {
    (void)h;
    de__editor *ed = (de__editor *)user;
    if (de__ed_crash_frozen())
        return DE_OK;
#if DE_HAS_RENDER
    /* Reap cancelled workers even with no IDE connected. Frame polling never joins a live job. */
    de__ed_settings_pump(ed);
#endif
    if (!ed_drain(ed)) {
        if (!ed->connected && !de__ed_ipc_linked(ed->ipc) &&
            de_time_now_ns() - ed->idle_since_ns > ed->grace_ns) {
            DE_INFO(ed->log, "editor: no IDE for %llu s; quitting",
                    (unsigned long long)(ed->grace_ns / 1000000000ull));
            de_engine_request_quit(ed->e);
            return DE_OK;
        }
        if (ed->connected && ed->authed)
            for (u32 i = 0; i < ed->tick_count; ++i)
                ed->ticks[i](ed);
    }
    de__ed_snapshot s;
    ed_snapshot(ed, &s);
    de__ed_ipc_publish(ed->ipc, &s); /* the frame boundary the small reads answer from */
    return DE_OK;
}

/* ---------- Lifecycle ---------- */

void de__editor_preinit(de__host *h) {
#if DE_HAS_RENDER
    if (arg_value(h, "--editor-service") != NULL)
        de__render_keep_cpu_meshes(true);
#else
    (void)h;
#endif
}

de_result de__editor_start(de__host *h) {
    const char *endpoint = arg_value(h, "--editor-service");
    if (endpoint == NULL)
        return DE_OK;
    const char *token = getenv("DE_EDITOR_TOKEN");
    if (token == NULL || token[0] == '\0')
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "--editor-service needs DE_EDITOR_TOKEN");
    const de_result crash = de__ed_crash_register();
    if (crash != DE_OK)
        return DE_FAIL(crash, "editor service crash handler registration");
    const de_allocator *al = de_heap_allocator();
    de__editor *ed = DE_NEW(al, de__editor, DE_MEM_GENERAL);
    if (ed == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "editor service");
    memset(ed, 0, sizeof *ed);
    ed->h = h;
    ed->e = h->engine;
    ed->log = de_log_register_category("editor", DE_LOG_INFO);
    snprintf(ed->token, sizeof ed->token, "%s", token);
    const char *grace = arg_value(h, "--editor-grace-s");
    ed->grace_ns =
        (u64)(grace != NULL ? strtoul(grace, NULL, 10) : ED_DEFAULT_GRACE_S) * 1000000000ull;
    ed->out_cap = ED_OUT_CAP;
    ed->note_cap = ED_NOTE_CAP;
    ed->arena_cap = ED_ARENA_CAP;
    ed->out = (char *)DE_ALLOC(al, ed->out_cap, 16, DE_MEM_GENERAL);
    ed->note = (char *)DE_ALLOC(al, ed->note_cap, 16, DE_MEM_GENERAL);
    ed->arena = (unsigned char *)DE_ALLOC(al, ed->arena_cap, 16, DE_MEM_GENERAL);
    h->editor = ed;
    if (ed->out == NULL || ed->note == NULL || ed->arena == NULL) {
        de__editor_stop(h);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "editor service buffers");
    }
    ed->mode = DE_EP_WORLD_MODE_EDIT;
    ed->idle_since_ns = de_time_now_ns();

    de__ed_session_register(ed);
    de__ed_reflect_register(ed);
    de__ed_world_register(ed);
    de__ed_edit_register(ed);
    de__ed_scene_register(ed);
    de__ed_assets_register(ed);
    de__ed_animation_register(ed);
#if DE_HAS_RENDER
    de__ed_preview_register(ed);
    de__ed_settings_register(ed);
#endif
    de__ed_logs_register(ed);
    de__ed_views_register(ed);
    de__ed_debugdraw_register(ed);
    de__ed_extra_register(ed);
    de__ed_spline_register(ed);
#if DE_HAS_RENDER
    de__ed_water_register(ed); /* DeWater is a render component */
#endif
#if DE_ED_MATERIAL_GRAPH
    de__ed_material_graph_register(ed);
#endif
#if DE_HAS_UI
    de__ed_ui_register(ed); /* ui.layout needs the UI module */
#endif

    /* Areas have registered their handlers and capabilities: from here they are fixed, and the
     * IPC thread reads them. */
    de__ed_snapshot first;
    ed_snapshot(ed, &first);
    const de__ed_ipc_desc ipc = {.endpoint = endpoint,
                                 .max_frame = ED_IN_CAP,
                                 .out_max = ED_OUT_CAP > ED_NOTE_CAP ? ED_OUT_CAP : ED_NOTE_CAP,
                                 .hello = ed_hello,
                                 .fast = ed_fast,
                                 .user = ed,
                                 .initial = &first};
    de_result r = de__ed_ipc_start(&ipc, &ed->ipc);
    if (r != DE_OK) {
        de__editor_stop(h);
        return r;
    }
    r = de__loop_add_frame_begin_hook(h, ed_pump, ed);
    if (r != DE_OK) {
        de__editor_stop(h);
        return DE_FAIL_WRAP(r, "editor service hook");
    }
    DE_INFO(ed->log, "editor: listening on %s", endpoint);
    return DE_OK;
}

void de__editor_stop(de__host *h) {
    de__editor *ed = h->editor;
    if (ed == NULL)
        return;
    ed_drop(ed, "shutdown");
    de__ed_ipc_stop(ed->ipc); /* sends what is queued, closes the connection and the listener */
    ed->ipc = NULL;
    de__ed_session_shutdown();
    de__ed_logs_shutdown();
    de__ed_views_shutdown(ed);
    de__ed_animation_shutdown(ed);
#if DE_HAS_RENDER
    de__ed_preview_shutdown(ed);
    de__ed_settings_shutdown(ed);
#endif
    const de_allocator *al = de_heap_allocator();
    if (ed->out)
        DE_FREE(al, ed->out, ed->out_cap);
    if (ed->note)
        DE_FREE(al, ed->note, ed->note_cap);
    if (ed->arena)
        DE_FREE(al, ed->arena, ed->arena_cap);
    DE_FREE(al, ed, sizeof *ed);
    h->editor = NULL;
}
