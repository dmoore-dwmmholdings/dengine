/* editor_extra.c: GPU capture, replays and the network simulator over IPC (DENG-633).
 *
 * Replays (.dereplay): a world snapshot, then per frame the raw input events the game saw and
 * the world checksum. Playing one restores the snapshot and feeds the recorded events back
 * through the input module's injection queue, one frame ahead, reporting replay.tick with the
 * recorded checksum next to the live one. Ticks are frames. Game state outside the world is not
 * in the snapshot, so a game that keeps state elsewhere drifts.
 *
 * GPU capture needs RenderDoc's or PIX's in-app API, which is not vendored: capture.request
 * answers unsupported and the capture capabilities are not offered. */
#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS /* fopen, snprintf */
#endif
#include "host/editor/editor_internal.h"

#include <dengine/de_memory.h>
#include <dengine/de_net.h>
#include <dengine/de_platform.h>

#include "host/host_internal.h"
#include "input/input_internal.h"

#include <stdio.h>
#include <string.h>

#define ED_REPLAY_MAGIC 0x50524544u /* "DERP" */
#define ED_REPLAY_VERSION 1u
#define ED_CK_HISTORY 4096u

typedef struct rp_frame {
    u64 checksum;
    u32 first, count; /* events */
} rp_frame;

typedef struct de__ed_extra {
    /* Recording or the loaded replay. */
    bool recording, playing;
    de_world_snapshot snap;
    bool has_snap;
    rp_frame *frames;
    u32 frame_count, frame_cap;
    de__input_event *events;
    u32 event_count, event_cap;
    u32 tick;      /* playback position */
    u32 seek_to;   /* fast-forward target (0: none) */
    f32 speed;
    f32 saved_scale;
    /* Local world checksums, for net.checksums. */
    u64 ck_tick[ED_CK_HISTORY], ck_value[ED_CK_HISTORY];
    u64 ck_count;
} de__ed_extra;

static de__ed_extra g_x;

static bool grow(void **p, u32 *cap, u32 need, size_t elem) {
    if (need <= *cap)
        return true;
    const de_allocator *al = de_heap_allocator();
    u32 ncap = *cap ? *cap * 2 : 1024;
    while (ncap < need)
        ncap *= 2;
    void *n = DE_ALLOC(al, elem * ncap, 16, DE_MEM_GENERAL);
    if (n == NULL)
        return false;
    if (*p != NULL) {
        memcpy(n, *p, elem * *cap);
        DE_FREE(al, *p, elem * *cap);
    }
    *p = n;
    *cap = ncap;
    return true;
}

static void replay_clear(void) {
    const de_allocator *al = de_heap_allocator();
    if (g_x.has_snap)
        de_world_snapshot_free(al, &g_x.snap);
    if (g_x.frames != NULL)
        DE_FREE(al, g_x.frames, sizeof(rp_frame) * g_x.frame_cap);
    if (g_x.events != NULL)
        DE_FREE(al, g_x.events, sizeof(de__input_event) * g_x.event_cap);
    g_x.has_snap = false;
    g_x.frames = NULL;
    g_x.events = NULL;
    g_x.frame_count = g_x.frame_cap = g_x.event_count = g_x.event_cap = 0;
    g_x.recording = g_x.playing = false;
    g_x.tick = g_x.seek_to = 0;
}

static void empty(mpack_writer_t *w, u64 id) {
    de_ep_write_result_begin(w, id);
    mpack_build_map(w);
    mpack_complete_map(w);
    de_ep_write_result_end(w);
}

static void set_scale(de__editor *ed, f32 scale) {
    de_timing t = de_engine_get_timing(ed->e);
    t.time_scale = scale;
    de_engine_set_timing(ed->e, &t);
}

/* ---------- capture ---------- */

static void on_capture(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    (void)ed;
    DE__ED_PARAMS(capture_request, de_ep_capture_params, q);
    de__ed_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "%.*s capture is not available in this build",
                 (int)q.backend.len, q.backend.ptr);
}

/* ---------- replay ---------- */

static void on_record_start(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                            mpack_writer_t *w) {
    (void)a;
    replay_clear();
    if (de_world_snapshot_take(ed->e->world, de_heap_allocator(), &g_x.snap) != DE_OK) {
        de_error_clear();
        de__ed_error(w, req->id, DE_EP_ERR_INTERNAL, "could not snapshot the world");
        return;
    }
    g_x.has_snap = true;
    g_x.recording = true;
    empty(w, req->id);
}

static bool write_all(FILE *f, const void *p, size_t n) {
    return n == 0 || fwrite(p, 1, n, f) == n;
}

static void on_record_stop(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                           mpack_writer_t *w) {
    (void)ed;
    if (!g_x.recording) {
        de__ed_error(w, req->id, DE_EP_ERR_CONFLICT, "not recording");
        return;
    }
    g_x.recording = false;
    char vpath[128], path[DE_FS_PATH_MAX];
    snprintf(vpath, sizeof vpath, "user:/replays/replay-%llu.dereplay",
             (unsigned long long)(de_time_now_ns() / 1000000ull));
    (void)de_dir_create("user:/replays");
    de_error_clear();
    if (de_fs_resolve(vpath, path, sizeof path) != DE_OK) {
        de_error_clear();
        de__ed_error(w, req->id, DE_EP_ERR_INTERNAL, "no user folder for replays");
        return;
    }
    FILE *f = fopen(path, "wb");
    const u32 hdr[4] = {ED_REPLAY_MAGIC, ED_REPLAY_VERSION, g_x.frame_count, g_x.event_count};
    const u64 snap_size = g_x.snap.size;
    const bool ok = f != NULL && write_all(f, hdr, sizeof hdr) && write_all(f, &snap_size, 8) &&
                    write_all(f, &g_x.snap.tick, 8) && write_all(f, g_x.snap.data, (size_t)snap_size) &&
                    write_all(f, g_x.frames, sizeof(rp_frame) * g_x.frame_count) &&
                    write_all(f, g_x.events, sizeof(de__input_event) * g_x.event_count);
    if (f != NULL)
        fclose(f);
    if (!ok) {
        de__ed_error(w, req->id, DE_EP_ERR_INTERNAL, "could not write %s", path);
        return;
    }
    de_ep_path_result r;
    r.path = de_ep_cstr(de__ed_cstr(a, de_ep_cstr(path)));
    de_ep_write_response_replay_record_stop(w, req->id, &r);
}

static bool read_all(FILE *f, void *p, size_t n) {
    return n == 0 || fread(p, 1, n, f) == n;
}

static void on_play(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    DE__ED_PARAMS(replay_play, de_ep_replay_play_params, q);
    char *path = de__ed_cstr(a, q.path);
    FILE *f = path != NULL ? fopen(path, "rb") : NULL;
    if (f == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "%s", path ? path : "?");
        return;
    }
    replay_clear();
    u32 hdr[4];
    u64 snap_size = 0;
    bool ok = read_all(f, hdr, sizeof hdr) && hdr[0] == ED_REPLAY_MAGIC && hdr[1] == ED_REPLAY_VERSION &&
              read_all(f, &snap_size, 8) && read_all(f, &g_x.snap.tick, 8);
    const de_allocator *al = de_heap_allocator();
    if (ok) {
        g_x.snap.data = DE_ALLOC(al, (size_t)snap_size, 16, DE_MEM_GENERAL);
        g_x.snap.size = snap_size;
        ok = g_x.snap.data != NULL && read_all(f, g_x.snap.data, (size_t)snap_size);
        g_x.has_snap = g_x.snap.data != NULL;
    }
    ok = ok && grow((void **)&g_x.frames, &g_x.frame_cap, hdr[2] + 1, sizeof(rp_frame)) &&
         grow((void **)&g_x.events, &g_x.event_cap, hdr[3] + 1, sizeof(de__input_event)) &&
         read_all(f, g_x.frames, sizeof(rp_frame) * hdr[2]) &&
         read_all(f, g_x.events, sizeof(de__input_event) * hdr[3]);
    fclose(f);
    if (!ok) {
        replay_clear();
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "%s is not a replay this engine reads", path);
        return;
    }
    g_x.frame_count = hdr[2];
    g_x.event_count = hdr[3];
    if (de_world_snapshot_restore(ed->e->world, &g_x.snap) != DE_OK) {
        de_error_clear();
        replay_clear();
        de__ed_error(w, req->id, DE_EP_ERR_CONFLICT, "the replay's world does not fit this game");
        return;
    }
    de__ed_world_resync(ed);
    g_x.playing = true;
    g_x.tick = 0;
    g_x.saved_scale = de_engine_get_timing(ed->e).time_scale;
    g_x.speed = q.speed;
    set_scale(ed, q.speed);
    empty(w, req->id);
}

static void on_seek(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    DE__ED_PARAMS(replay_seek, de_ep_replay_seek_params, q);
    if (!g_x.playing || !g_x.has_snap) {
        de__ed_error(w, req->id, DE_EP_ERR_CONFLICT, "no replay is playing");
        return;
    }
    /* From the snapshot again, fast, to the tick. */
    if (de_world_snapshot_restore(ed->e->world, &g_x.snap) != DE_OK)
        de_error_clear();
    de__ed_world_resync(ed);
    g_x.tick = 0;
    g_x.seek_to = (u32)(q.tick < g_x.frame_count ? q.tick : g_x.frame_count);
    set_scale(ed, 8.0f);
    empty(w, req->id);
}

static void on_speed(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    DE__ED_PARAMS(replay_set_speed, de_ep_replay_speed_params, q);
    if (q.speed < 0 || q.speed > 8) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "speed is 0 to 8");
        return;
    }
    g_x.speed = q.speed;
    if (g_x.seek_to == 0)
        set_scale(ed, q.speed);
    empty(w, req->id);
}

/* ---------- net ---------- */

static void on_net_sim(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    DE__ED_PARAMS(net_sim, de_ep_net_sim_params, q);
    /* The net module has no session layer yet (de_net_session and de_net_set_simulation are
     * declared, not built): nothing to simulate on. */
    (void)ed;
    de__ed_error(w, req->id, DE_EP_ERR_UNSUPPORTED,
                 "no network session (latency %.0f ms, loss %.0f%% not applied)", q.latency_ms, q.loss_pct);
}

static void on_net_checksums(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                             mpack_writer_t *w) {
    (void)ed;
    DE__ED_PARAMS(net_checksums, de_ep_net_checksums_params, q);
    de_ep_net_checksums r;
    memset(&r, 0, sizeof r);
    r.checksums.items = (de_ep_peer_checksum *)de__ed_alloc(a, sizeof(de_ep_peer_checksum) * ED_CK_HISTORY);
    const u64 first = g_x.ck_count > ED_CK_HISTORY ? g_x.ck_count - ED_CK_HISTORY : 0;
    for (u64 i = first; i < g_x.ck_count && r.checksums.items != NULL; ++i) {
        const u32 k = (u32)(i % ED_CK_HISTORY);
        if (g_x.ck_tick[k] < q.from_tick || g_x.ck_tick[k] > q.to_tick)
            continue;
        r.checksums.items[r.checksums.count++] = (de_ep_peer_checksum){0, g_x.ck_tick[k], g_x.ck_value[k]};
    }
    de_ep_write_response_net_checksums(w, req->id, &r);
}

/* ---------- per frame ---------- */

static void tick(de__editor *ed) {
    const u64 frame = de__ed_frame(ed);
    const u64 ck = de_world_checksum(ed->e->world, DE_COMP_SNAPSHOT | DE_COMP_SERIALIZE | DE_COMP_REPLICATE);
    const u32 k = (u32)(g_x.ck_count % ED_CK_HISTORY);
    g_x.ck_tick[k] = frame;
    g_x.ck_value[k] = ck;
    ++g_x.ck_count;

    if (g_x.recording && ed->e->input != NULL) {
        /* This frame's events (begin_frame ran before the service's hook). */
        const de_input *in = ed->e->input;
        const u32 n = in->frame_event_count;
        if (grow((void **)&g_x.frames, &g_x.frame_cap, g_x.frame_count + 1, sizeof(rp_frame)) &&
            grow((void **)&g_x.events, &g_x.event_cap, g_x.event_count + n, sizeof(de__input_event))) {
            memcpy(&g_x.events[g_x.event_count], in->frame_events, sizeof(de__input_event) * n);
            g_x.frames[g_x.frame_count++] = (rp_frame){ck, g_x.event_count, n};
            g_x.event_count += n;
        }
    }
    if (g_x.playing && ed->e->input != NULL) {
        const u32 t = g_x.tick;
        const bool done = t + 1 >= g_x.frame_count;
        /* The next frame's events: injection applies at the next begin_frame. */
        if (t + 1 < g_x.frame_count) {
            const rp_frame *nf = &g_x.frames[t + 1];
            for (u32 i = 0; i < nf->count; ++i)
                de__input_inject(ed->e->input, &g_x.events[nf->first + i]);
        }
        const bool quiet = g_x.seek_to != 0 && t < g_x.seek_to;
        if (!quiet && t < g_x.frame_count) {
            de_ep_replay_tick d;
            memset(&d, 0, sizeof d);
            d.tick = t;
            d.checksum = ck;
            d.has_expected = true;
            d.expected = g_x.frames[t].checksum;
            if (done) {
                d.has_done = true;
                d.done = true;
            }
            mpack_writer_t nw;
            const u64 seq = de__ed_note_begin(ed, &nw);
            de_ep_write_notify_replay_tick(&nw, seq, &d);
            de__ed_send(ed, &nw, DE_EP_KIND_NOTIFICATION);
        }
        if (g_x.seek_to != 0 && t + 1 >= g_x.seek_to) {
            g_x.seek_to = 0;
            set_scale(ed, g_x.speed);
        }
        g_x.tick = t + 1;
        if (done) {
            g_x.playing = false;
            set_scale(ed, g_x.saved_scale > 0 ? g_x.saved_scale : 1.0f);
        }
    }
}

static void reset(de__editor *ed) {
    if (g_x.playing)
        set_scale(ed, g_x.saved_scale > 0 ? g_x.saved_scale : 1.0f);
    replay_clear();
}

void de__ed_extra_register(de__editor *ed) {
    memset(&g_x, 0, sizeof g_x);
    ed->extra = (struct de__ed_extra *)&g_x;
    de__ed_add_capability(ed, "replay");
    de__ed_on(ed, DE_EP_METHOD_CAPTURE_REQUEST, on_capture);
    de__ed_on(ed, DE_EP_METHOD_REPLAY_RECORD_START, on_record_start);
    de__ed_on(ed, DE_EP_METHOD_REPLAY_RECORD_STOP, on_record_stop);
    de__ed_on(ed, DE_EP_METHOD_REPLAY_PLAY, on_play);
    de__ed_on(ed, DE_EP_METHOD_REPLAY_SEEK, on_seek);
    de__ed_on(ed, DE_EP_METHOD_REPLAY_SET_SPEED, on_speed);
    de__ed_on(ed, DE_EP_METHOD_NET_SIM, on_net_sim);
    de__ed_on(ed, DE_EP_METHOD_NET_CHECKSUMS, on_net_checksums);
    de__ed_on_tick(ed, tick);
    de__ed_on_reset(ed, reset);
}
