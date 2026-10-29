/* editor.c: the editor service's connection, handshake and dispatch (DENG-623). */
#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS /* getenv, vsnprintf */
#endif
#include "host/editor/editor_internal.h"

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

#define ED_IN_CAP    (16u << 20)
#define ED_OUT_CAP   (32u << 20)
#define ED_NOTE_CAP  (8u << 20)
#define ED_ARENA_CAP (8u << 20)
#define ED_BLOB_PART (1u << 20)
#define ED_REQUESTS_PER_FRAME 256u
#define ED_DEFAULT_GRACE_S 30u

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
    de_ep_write_error(w, id, code, msg);
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

static void ed_drop(de__editor *ed, const char *why);

bool de__ed_send(de__editor *ed, mpack_writer_t *w, u8 kind) {
    const char *buf = w->buffer;
    const size_t n = mpack_writer_buffer_used(w);
    const mpack_error_t err = mpack_writer_destroy(w);
    if (err != mpack_ok) {
        DE_WARN(ed->log, "editor: a message did not fit (%s)", mpack_error_to_string(err));
        return ed->connected;
    }
    if (!ed->connected)
        return false;
    if (de_ep_send(&ed->conn, kind, buf, n) != DE_EP_IO_OK) {
        ed_drop(ed, "send failed");
        return false;
    }
    return true;
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

/* ---------- Connection ---------- */

static void ed_drop(de__editor *ed, const char *why) {
    if (!ed->connected)
        return;
    DE_INFO(ed->log, "editor: IDE disconnected (%s)", why);
    de_ep_disconnect(&ed->conn);
    ed->connected = ed->authed = false;
    ed->idle_since_ns = de_time_now_ns();
    for (u32 i = 0; i < ed->reset_count; ++i)
        ed->resets[i](ed);
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

/* The first request: hello. False closes the connection. */
static bool ed_hello(de__editor *ed, const de_ep_request *req, de_ep_arena *a) {
    mpack_writer_t w;
    mpack_writer_init(&w, ed->out, ed->out_cap);
    de_ep_hello_params p;
    memset(&p, 0, sizeof p);
    if (de_ep_method_parse(req->method) != DE_EP_METHOD_HELLO ||
        !de_ep_read_params_hello(req, a, &p)) {
        mpack_writer_destroy(&w);
        return false; /* anything but hello first closes the connection */
    }
    if (!de_ep_token_eq(p.token, de_ep_cstr(ed->token))) {
        de_ep_write_error(&w, req->id, DE_EP_ERR_UNAUTHORIZED, "bad session token");
        de__ed_send(ed, &w, DE_EP_KIND_RESPONSE);
        return false;
    }
    if (p.has_schema && de_ep_str_eq(p.protocol, DE_EP_PROTOCOL_VERSION) &&
        !de_ep_str_eq(p.schema, DE_EP_SCHEMA_HASH)) {
        de_ep_write_error(&w, req->id, DE_EP_ERR_UNSUPPORTED, "same version, different schema");
        de__ed_send(ed, &w, DE_EP_KIND_RESPONSE);
        return false;
    }
    char agreed[16];
    if (!de_ep_negotiate(de_ep_cstr(DE_EP_PROTOCOL_VERSION), p.protocol, agreed)) {
        de_ep_write_error(&w, req->id, DE_EP_ERR_UNSUPPORTED, "unsupported protocol version");
        de__ed_send(ed, &w, DE_EP_KIND_RESPONSE);
        return false;
    }
    de_ep_str caps[DE__ED_MAX_CAPS];
    for (u32 i = 0; i < ed->cap_count; ++i)
        caps[i] = de_ep_cstr(ed->caps[i]);
    de_ep_hello_result r;
    memset(&r, 0, sizeof r);
    r.protocol = de_ep_cstr(agreed);
    r.capabilities.items = caps;
    r.capabilities.count = ed->cap_count;
    ed_build_info(ed, &r.build);
    r.limits.requests_in_flight = DE_EP_LIMIT_REQUESTS_IN_FLIGHT;
    r.limits.subscriptions = DE_EP_LIMIT_SUBSCRIPTIONS;
    r.limits.query_page_size = DE_EP_LIMIT_QUERY_PAGE_SIZE;
    r.limits.open_transactions = DE_EP_LIMIT_OPEN_TRANSACTIONS;
    r.limits.frame_size = DE_EP_LIMIT_FRAME_SIZE;
    r.epoch = ed->epoch;
    r.has_schema = true;
    r.schema = de_ep_cstr(DE_EP_SCHEMA_HASH);
    de_ep_write_response_hello(&w, req->id, &r);
    if (!de__ed_send(ed, &w, DE_EP_KIND_RESPONSE))
        return false;
    unsigned major = 0, minor = 0;
    ed->reload_outcomes =
        sscanf(agreed, "%u.%u", &major, &minor) == 2 && (major > 1 || (major == 1 && minor >= 6));
    ed->authed = true;
    DE_INFO(ed->log, "editor: IDE connected (protocol %s, epoch %u)", agreed, ed->epoch);
    return true;
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
    } else if (ed->handlers[m] == NULL) {
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

static de_result ed_pump(de__host *h, void *user) {
    (void)h;
    de__editor *ed = (de__editor *)user;
#if DE_HAS_RENDER
    /* Reap cancelled workers even with no IDE connected. Frame polling never joins a live job. */
    de__ed_settings_pump(ed);
#endif
    if (!ed->connected) {
        const de_ep_io io = de_ep_accept(&ed->listener, &ed->conn, (u32)ed->in_cap, 0);
        if (io == DE_EP_IO_OK) {
            ed->connected = true;
            ed->authed = false;
            ++ed->epoch;
        } else if (de_time_now_ns() - ed->idle_since_ns > ed->grace_ns) {
            DE_INFO(ed->log, "editor: no IDE for %llu s; quitting",
                    (unsigned long long)(ed->grace_ns / 1000000000ull));
            de_engine_request_quit(ed->e);
            return DE_OK;
        }
    }
    for (u32 n = 0; ed->connected && n < ED_REQUESTS_PER_FRAME; ++n) {
        u8 kind = 0;
        size_t len = 0;
        const de_ep_io io = de_ep_recv(&ed->conn, ed->in, ed->in_cap, &kind, &len, 0);
        if (io == DE_EP_IO_TIMEOUT)
            break;
        if (io != DE_EP_IO_OK) {
            ed_drop(ed, io == DE_EP_IO_CLOSED ? "closed" : "read failed");
            break;
        }
        de_ep_request req;
        if (kind != DE_EP_KIND_REQUEST || !de_ep_read_request(ed->in, len, &req)) {
            ed_drop(ed, "not a request");
            break;
        }
        de_ep_arena a = {ed->arena, ed->arena_cap, 0};
        if (!ed->authed) {
            if (!ed_hello(ed, &req, &a))
                ed_drop(ed, "handshake refused");
            continue;
        }
        const bool goodbye = de_ep_method_parse(req.method) == DE_EP_METHOD_GOODBYE;
        ed_dispatch(ed, &req, &a);
        if (goodbye) {
            ed_drop(ed, "goodbye");
            de_engine_request_quit(ed->e);
            return DE_OK;
        }
    }
    if (ed->connected && ed->authed)
        for (u32 i = 0; i < ed->tick_count; ++i)
            ed->ticks[i](ed);
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
    ed->grace_ns = (u64)(grace != NULL ? strtoul(grace, NULL, 10) : ED_DEFAULT_GRACE_S) *
                   1000000000ull;
    ed->in_cap = ED_IN_CAP;
    ed->out_cap = ED_OUT_CAP;
    ed->note_cap = ED_NOTE_CAP;
    ed->arena_cap = ED_ARENA_CAP;
    ed->in = (char *)DE_ALLOC(al, ed->in_cap, 16, DE_MEM_GENERAL);
    ed->out = (char *)DE_ALLOC(al, ed->out_cap, 16, DE_MEM_GENERAL);
    ed->note = (char *)DE_ALLOC(al, ed->note_cap, 16, DE_MEM_GENERAL);
    ed->arena = (unsigned char *)DE_ALLOC(al, ed->arena_cap, 16, DE_MEM_GENERAL);
    h->editor = ed;
    if (ed->in == NULL || ed->out == NULL || ed->note == NULL || ed->arena == NULL) {
        de__editor_stop(h);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "editor service buffers");
    }
    if (de_ep_listen(&ed->listener, endpoint) != DE_EP_IO_OK) {
        de__editor_stop(h);
        return DE_FAIL(DE_ERR_IO, "editor service: cannot listen on %s", endpoint);
    }
    ed->mode = DE_EP_WORLD_MODE_EDIT;
    ed->idle_since_ns = de_time_now_ns();

    de__ed_session_register(ed);
    de__ed_reflect_register(ed);
    de__ed_world_register(ed);
    de__ed_edit_register(ed);
    de__ed_scene_register(ed);
    de__ed_assets_register(ed);
#if DE_HAS_RENDER
    de__ed_preview_register(ed);
    de__ed_settings_register(ed);
#endif
    de__ed_logs_register(ed);
    de__ed_views_register(ed);
    de__ed_debugdraw_register(ed);
    de__ed_extra_register(ed);

    de_result r = de__loop_add_frame_begin_hook(h, ed_pump, ed);
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
    de_ep_listener_close(&ed->listener);
    de__ed_session_shutdown();
    de__ed_logs_shutdown();
    de__ed_views_shutdown(ed);
#if DE_HAS_RENDER
    de__ed_preview_shutdown(ed);
    de__ed_settings_shutdown(ed);
#endif
    const de_allocator *al = de_heap_allocator();
    if (ed->in)
        DE_FREE(al, ed->in, ed->in_cap);
    if (ed->out)
        DE_FREE(al, ed->out, ed->out_cap);
    if (ed->note)
        DE_FREE(al, ed->note, ed->note_cap);
    if (ed->arena)
        DE_FREE(al, ed->arena, ed->arena_cap);
    DE_FREE(al, ed, sizeof *ed);
    h->editor = NULL;
}
