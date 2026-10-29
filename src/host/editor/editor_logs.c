/* editor_logs.c: log records, the console and stats over IPC (DENG-630).
 *
 * A log sink copies every record into a ring (the log thread calls it; a mutex guards the ring),
 * and the per-frame tick sends the records each log subscription wants. console.exec runs the
 * line with de_command_exec and answers with what that thread logged meanwhile. */
#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS /* snprintf */
#endif
#include "host/editor/editor_internal.h"

#include <dengine/de_asset.h>
#include <dengine/de_debug.h>
#include <dengine/de_platform.h>
#include <dengine/de_render.h>

#include "core/command_internal.h"
#include "ecs/ecs_internal.h"
#include "host/host_internal.h"

#include <stdio.h>
#include <string.h>

#define ED_LOG_RING 2048u
#define ED_LOG_MSG  480u
#define ED_LOG_SUBS 8u
#define ED_STAT_SUBS 8u

typedef struct ed_log_rec {
    u64 seq;
    u64 time_ns;
    u64 frame;
    u32 thread;
    u32 line;
    de_log_level level;
    char category[32];
    char file[96];
    char message[ED_LOG_MSG];
} ed_log_rec;

typedef struct ed_log_sub {
    u32 id; /* 0: free */
    de_log_level min;
    char categories[16][32];
    u32 category_count;
    u64 next; /* the next ring sequence to send */
} ed_log_sub;

typedef struct ed_stat_sub {
    u32 id;
    u32 every;
    u32 groups; /* bits: 1 frame, 2 render, 4 ecs, 8 assets */
} ed_stat_sub;

typedef struct de__ed_logs {
    de_mutex lock;
    bool sink;
    ed_log_rec ring[ED_LOG_RING];
    u64 written; /* records written so far (ring index = seq % size) */
    u64 frame;   /* the frame the main thread last saw, stamped on records */
    u64 start_ns;
    ed_log_sub subs[ED_LOG_SUBS];
    ed_stat_sub stats[ED_STAT_SUBS];
    u32 next_id;
    u64 last_frame_ns;
    f64 frame_ms;
} de__ed_logs;

static de__ed_logs g_logs;

static void sink(const de_log_record *rec, void *user) {
    (void)user;
    de_mutex_lock(&g_logs.lock);
    ed_log_rec *r = &g_logs.ring[g_logs.written % ED_LOG_RING];
    r->seq = g_logs.written++;
    r->time_ns = rec->timestamp_ns;
    r->frame = g_logs.frame;
    r->thread = rec->thread_id;
    r->line = rec->line;
    r->level = rec->level;
    snprintf(r->category, sizeof r->category, "%s", de_log_category_name(rec->category));
    snprintf(r->file, sizeof r->file, "%s", rec->file != NULL ? rec->file : "");
    const size_t n = rec->message.len < ED_LOG_MSG - 1 ? rec->message.len : ED_LOG_MSG - 1;
    memcpy(r->message, rec->message.ptr, n);
    r->message[n] = '\0';
    de_mutex_unlock(&g_logs.lock);
}

static bool level_parse(de_ep_str s, de_log_level *out) {
    static const char *const names[] = {"trace", "debug", "info", "warn", "error", "fatal"};
    for (u32 i = 0; i < 6; ++i)
        if (de_ep_str_eq(s, names[i])) {
            *out = (de_log_level)i;
            return true;
        }
    return false;
}

static const char *level_name(de_log_level l) {
    static const char *const names[] = {"trace", "debug", "info", "warn", "error", "fatal"};
    return (u32)l < 6 ? names[l] : "info";
}

/* ---------- log.subscribe ---------- */

static void on_log_subscribe(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                             mpack_writer_t *w) {
    (void)ed;
    DE__ED_PARAMS(log_subscribe, de_ep_log_subscribe_params, q);
    de_log_level min = DE_LOG_INFO;
    if (q.min_level.len > 0 && !level_parse(q.min_level, &min)) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "unknown level %.*s",
                     (int)q.min_level.len, q.min_level.ptr);
        return;
    }
    ed_log_sub *s = NULL;
    for (u32 i = 0; i < ED_LOG_SUBS && s == NULL; ++i)
        if (g_logs.subs[i].id == 0)
            s = &g_logs.subs[i];
    if (s == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_BUSY, "too many log subscriptions");
        return;
    }
    memset(s, 0, sizeof *s);
    s->id = 0x10000u + ++g_logs.next_id;
    s->min = min;
    for (u32 i = 0; q.has_categories && i < q.categories.count && s->category_count < 16; ++i)
        snprintf(s->categories[s->category_count++], 32, "%.*s", (int)q.categories.items[i].len,
                 q.categories.items[i].ptr);
    /* Start with what the ring still holds, so the IDE sees the startup log. */
    de_mutex_lock(&g_logs.lock);
    s->next = g_logs.written > ED_LOG_RING ? g_logs.written - ED_LOG_RING : 0;
    de_mutex_unlock(&g_logs.lock);
    de_ep_subscribe_result r = {s->id};
    de_ep_write_response_log_subscribe(w, req->id, &r);
}

static bool sub_wants(const ed_log_sub *s, const ed_log_rec *r) {
    if (r->level < s->min)
        return false;
    if (s->category_count == 0)
        return true;
    for (u32 i = 0; i < s->category_count; ++i)
        if (strcmp(s->categories[i], r->category) == 0)
            return true;
    return false;
}

static void send_logs(de__editor *ed) {
    for (u32 i = 0; i < ED_LOG_SUBS; ++i) {
        ed_log_sub *s = &g_logs.subs[i];
        if (s->id == 0)
            continue;
        for (u32 budget = 0; budget < 256; ++budget) {
            ed_log_rec r;
            de_mutex_lock(&g_logs.lock);
            if (s->next + ED_LOG_RING < g_logs.written)
                s->next = g_logs.written - ED_LOG_RING; /* the ring wrapped: skip ahead */
            const bool have = s->next < g_logs.written;
            if (have)
                r = g_logs.ring[s->next % ED_LOG_RING];
            de_mutex_unlock(&g_logs.lock);
            if (!have)
                break;
            ++s->next;
            if (!sub_wants(s, &r))
                continue;
            de_ep_log_record d;
            memset(&d, 0, sizeof d);
            d.time = (double)(r.time_ns - g_logs.start_ns) * 1e-9;
            d.frame = r.frame;
            d.level = de_ep_cstr(level_name(r.level));
            d.category = de_ep_cstr(r.category);
            char thread[16];
            snprintf(thread, sizeof thread, "%u", r.thread);
            d.thread = de_ep_cstr(thread);
            d.message = de_ep_cstr(r.message);
            if (r.file[0]) {
                d.has_file = true;
                d.file = de_ep_cstr(r.file);
                d.has_line = true;
                d.line = r.line;
            }
            mpack_writer_t nw;
            const u64 seq = de__ed_note_begin(ed, &nw);
            de_ep_write_notify_log_record(&nw, seq, &d);
            if (!de__ed_send(ed, &nw, DE_EP_KIND_NOTIFICATION))
                return;
        }
    }
}

/* ---------- console ---------- */

static void on_exec(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    (void)ed;
    DE__ED_PARAMS(console_exec, de_ep_console_exec_params, q);
    char *line = de__ed_cstr(a, q.line);
    if (line == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_BUSY, "line too long");
        return;
    }
    de_log_flush();
    de_mutex_lock(&g_logs.lock);
    const u64 from = g_logs.written;
    de_mutex_unlock(&g_logs.lock);
    const de_result r = de_command_exec(line);
    const de_error *err = r != DE_OK ? de_error_last() : NULL;
    char errmsg[256] = "";
    if (err != NULL)
        snprintf(errmsg, sizeof errmsg, "%s", err->message);
    de_error_clear();
    de_log_flush();
    /* What this thread logged during the command. */
    const u32 me = de_thread_current_id();
    char *out = (char *)de__ed_alloc(a, 64u * 1024u);
    size_t n = 0;
    if (out != NULL) {
        out[0] = '\0';
        de_mutex_lock(&g_logs.lock);
        for (u64 s = from; s < g_logs.written; ++s) {
            const ed_log_rec *rec = &g_logs.ring[s % ED_LOG_RING];
            if (rec->thread != me || s + ED_LOG_RING < g_logs.written)
                continue;
            const int k = snprintf(out + n, 64u * 1024u - n, "%s\n", rec->message);
            if (k < 0 || n + (size_t)k >= 64u * 1024u)
                break;
            n += (size_t)k;
        }
        de_mutex_unlock(&g_logs.lock);
        if (errmsg[0] && n + strlen(errmsg) + 2 < 64u * 1024u)
            n += (size_t)snprintf(out + n, 64u * 1024u - n, "%s\n", errmsg);
    }
    de_ep_console_output o;
    o.output = (de_ep_str){out ? out : "", (u32)n};
    o.ok = r == DE_OK;
    de_ep_write_response_console_exec(w, req->id, &o);
}

typedef struct complete_walk {
    de_ep_str prefix;
    de_ep_arena *a;
    de_ep_str *items;
    u32 count, cap;
} complete_walk;

static bool prefixed(const char *name, de_ep_str prefix) {
    return strlen(name) >= prefix.len && memcmp(name, prefix.ptr, prefix.len) == 0;
}

static void complete_cvar(de_cvar cv, void *user) {
    complete_walk *cw = (complete_walk *)user;
    const char *name = de_cvar_name(cv);
    if (cw->count < cw->cap && name != NULL && prefixed(name, cw->prefix))
        cw->items[cw->count++] = de_ep_cstr(name);
}

static void on_complete(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                        mpack_writer_t *w) {
    (void)ed;
    DE__ED_PARAMS(console_complete, de_ep_console_complete_params, q);
    complete_walk cw = {q.prefix, a, NULL, 0, 512};
    cw.items = (de_ep_str *)de__ed_alloc(a, sizeof(de_ep_str) * cw.cap);
    if (cw.items != NULL) {
        char name[128];
        for (u32 i = 0; cw.count < cw.cap && de__command_name_at(i, name, sizeof name); ++i)
            if (prefixed(name, q.prefix)) {
                char *copy = de__ed_cstr(a, de_ep_cstr(name));
                if (copy != NULL)
                    cw.items[cw.count++] = de_ep_cstr(copy);
            }
        de_cvar_foreach_flag(0, complete_cvar, &cw);
    }
    de_ep_completions c;
    c.items.items = cw.items;
    c.items.count = cw.count;
    de_ep_write_response_console_complete(w, req->id, &c);
}

/* ---------- stats ---------- */

static u32 group_bit(de_ep_str g) {
    if (de_ep_str_eq(g, "frame"))
        return 1;
    if (de_ep_str_eq(g, "render"))
        return 2;
    if (de_ep_str_eq(g, "ecs"))
        return 4;
    if (de_ep_str_eq(g, "assets"))
        return 8;
    return 0;
}

static void on_stats_subscribe(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                               mpack_writer_t *w) {
    (void)ed;
    DE__ED_PARAMS(stats_subscribe, de_ep_stats_subscribe_params, q);
    ed_stat_sub *s = NULL;
    for (u32 i = 0; i < ED_STAT_SUBS && s == NULL; ++i)
        if (g_logs.stats[i].id == 0)
            s = &g_logs.stats[i];
    if (s == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_BUSY, "too many stats subscriptions");
        return;
    }
    memset(s, 0, sizeof *s);
    s->id = 0x20000u + ++g_logs.next_id;
    s->every = q.rate_hz > 0 ? (u32)(60.0f / q.rate_hz) : 30u;
    if (s->every == 0)
        s->every = 1;
    for (u32 i = 0; i < q.groups.count; ++i)
        s->groups |= group_bit(q.groups.items[i]);
    if (s->groups == 0)
        s->groups = 0xF;
    de_ep_subscribe_result r = {s->id};
    de_ep_write_response_stats_subscribe(w, req->id, &r);
}

static void send_stats(de__editor *ed) {
    const u64 frame = de__ed_frame(ed);
    for (u32 i = 0; i < ED_STAT_SUBS; ++i) {
        const ed_stat_sub *s = &g_logs.stats[i];
        if (s->id == 0 || frame % s->every != 0)
            continue;
        de_ep_kv_f64 kv[24];
        u32 n = 0;
        if (s->groups & 1) {
            kv[n++] = (de_ep_kv_f64){de_ep_cstr("frame.ms"), g_logs.frame_ms};
            kv[n++] = (de_ep_kv_f64){de_ep_cstr("frame.fps"), g_logs.frame_ms > 0 ? 1000.0 / g_logs.frame_ms : 0};
        }
#if DE_HAS_RENDER
        if ((s->groups & 2) && ed->e->renderer != NULL) {
            const de_render_stats *rs = de_renderer_stats(ed->e->renderer);
            kv[n++] = (de_ep_kv_f64){de_ep_cstr("render.gpu_ms"), rs->gpu_ms};
            kv[n++] = (de_ep_kv_f64){de_ep_cstr("render.cpu_record_ms"), rs->cpu_record_ms};
            kv[n++] = (de_ep_kv_f64){de_ep_cstr("render.passes"), rs->pass_count};
            kv[n++] = (de_ep_kv_f64){de_ep_cstr("render.instances"), rs->instances_visible};
            kv[n++] = (de_ep_kv_f64){de_ep_cstr("render.triangles"), (f64)rs->triangles_visible};
        }
#endif
        if (s->groups & 4) {
            const de_world *w = ed->e->world;
            u32 ents = 0;
            for (u32 k = 0; k < w->archetypes.count; ++k)
                ents += (((ecs_archetype *const *)w->archetypes.data)[k])->count;
            kv[n++] = (de_ep_kv_f64){de_ep_cstr("ecs.entities"), ents};
            kv[n++] = (de_ep_kv_f64){de_ep_cstr("ecs.archetypes"), w->archetypes.count};
            kv[n++] = (de_ep_kv_f64){de_ep_cstr("ecs.components"), w->components.count};
        }
        if ((s->groups & 8) && ed->e->assets != NULL) {
            const de_asset_stats as = de_asset_get_stats(ed->e->assets);
            kv[n++] = (de_ep_kv_f64){de_ep_cstr("assets.loads_in_flight"), as.loads_in_flight};
            kv[n++] = (de_ep_kv_f64){de_ep_cstr("assets.resident"), as.assets_resident};
            kv[n++] = (de_ep_kv_f64){de_ep_cstr("assets.gpu_texture_bytes"), (f64)as.bytes_resident[0]};
            kv[n++] = (de_ep_kv_f64){de_ep_cstr("assets.gpu_mesh_bytes"), (f64)as.bytes_resident[1]};
            kv[n++] = (de_ep_kv_f64){de_ep_cstr("assets.audio_bytes"), (f64)as.bytes_resident[2]};
            kv[n++] = (de_ep_kv_f64){de_ep_cstr("assets.cpu_bytes"), (f64)as.bytes_resident[3]};
        }
        de_ep_stats_sample d;
        d.frame = frame;
        d.values.items = kv;
        d.values.count = n;
        mpack_writer_t nw;
        const u64 seq = de__ed_note_begin(ed, &nw);
        de_ep_write_notify_stats_sample(&nw, seq, &d);
        de__ed_send(ed, &nw, DE_EP_KIND_NOTIFICATION);
    }
}

/* ---------- unsubscribe for our ids, the tick ---------- */

/** Ends a log or stats subscription; false when `id` is not one. */
bool de__ed_logs_unsubscribe(u32 id);
bool de__ed_logs_unsubscribe(u32 id) {
    for (u32 i = 0; i < ED_LOG_SUBS; ++i)
        if (g_logs.subs[i].id == id && id != 0) {
            memset(&g_logs.subs[i], 0, sizeof g_logs.subs[i]);
            return true;
        }
    for (u32 i = 0; i < ED_STAT_SUBS; ++i)
        if (g_logs.stats[i].id == id && id != 0) {
            memset(&g_logs.stats[i], 0, sizeof g_logs.stats[i]);
            return true;
        }
    return false;
}

static void tick(de__editor *ed) {
    const u64 now = de_time_now_ns();
    if (g_logs.last_frame_ns != 0)
        g_logs.frame_ms = g_logs.frame_ms * 0.9 + (f64)(now - g_logs.last_frame_ns) * 1e-6 * 0.1;
    g_logs.last_frame_ns = now;
    g_logs.frame = de__ed_frame(ed);
    /* Game reload lifecycle is emitted at the actual attempt boundary (editor_session.c). */
    send_logs(ed);
    send_stats(ed);
}

static void reset(de__editor *ed) {
    (void)ed;
    memset(g_logs.subs, 0, sizeof g_logs.subs);
    memset(g_logs.stats, 0, sizeof g_logs.stats);
}

void de__ed_logs_register(de__editor *ed) {
    memset(&g_logs, 0, sizeof g_logs);
    ed->logs = (struct de__ed_logs *)&g_logs;
    de_mutex_init(&g_logs.lock);
    g_logs.start_ns = de_time_now_ns();
    de_log_add_sink(sink, NULL);
    g_logs.sink = true;
    de__ed_on(ed, DE_EP_METHOD_LOG_SUBSCRIBE, on_log_subscribe);
    de__ed_on(ed, DE_EP_METHOD_CONSOLE_EXEC, on_exec);
    de__ed_on(ed, DE_EP_METHOD_CONSOLE_COMPLETE, on_complete);
    de__ed_on(ed, DE_EP_METHOD_STATS_SUBSCRIBE, on_stats_subscribe);
    de__ed_on_tick(ed, tick);
    de__ed_on_reset(ed, reset);
}

/** Removes the log sink (service stop). */
void de__ed_logs_shutdown(void);
void de__ed_logs_shutdown(void) {
    if (g_logs.sink) {
        de_log_remove_sink(sink, NULL);
        g_logs.sink = false;
        de_mutex_destroy(&g_logs.lock);
    }
}
