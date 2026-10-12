/*
 * test_asset_events.c: asset ECS events and the DeAssetSystemRef resource (DENG-143, SPEC §11.3,
 * §7.1).
 *
 * Each test has a world without a task system, an asset system on a tracking allocator and a
 * small scripted backend (submissions are recorded; the test completes them with
 * de__slot_complete). A "frame" here is what the host does: de_asset_system_update, then the
 * schedules First (where de.asset_events sends the queued events) and Update.
 */
#include "de_alloc_guard.h"
#include "de_test.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_ecs.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_reflect.h>

#include "asset/asset_internal.h"
#include "asset/backend_internal.h"
#include "asset/slots_internal.h"

#include <string.h>

#ifndef DE_ASSET_TEST_DATA_DIR
#error "DE_ASSET_TEST_DATA_DIR must be defined by tests/unit/asset/CMakeLists.txt"
#endif

#define EV_MAX 64u

/* ---- Fixture -------------------------------------------------------------------------------- */

typedef struct ev_fx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_type_registry *types;
    de_world *w;
    de_asset_system *s;
    u32 submitted[EV_MAX]; /* slot handles, in submit order */
    u32 submits, cancels;
    de_event_reader reader; /* the test's own reader (outside any system) */
    de_asset_event got[EV_MAX];
    u32 got_count;
} ev_fx;

static void fake_submit(void *user, de_asset_system *s, u32 slot, de__load_priority p) {
    (void)s;
    (void)p;
    ev_fx *f = (ev_fx *)user;
    if (f->submits < EV_MAX)
        f->submitted[f->submits] = slot;
    f->submits++;
}

static void fake_cancel(void *user, de_asset_system *s, u32 slot) {
    (void)s;
    (void)slot;
    ((ev_fx *)user)->cancels++;
}

static void fake_reprioritize(void *user, de_asset_system *s, u32 slot, de__load_priority p) {
    (void)user;
    (void)s;
    (void)slot;
    (void)p;
}

static void mount_once(void) {
    static bool mounted;
    if (mounted)
        return;
    ASSERT_EQ(de_fs_mount("evdata", DE_ASSET_TEST_DATA_DIR "/events", DE_MOUNT_READ_ONLY, 0),
              DE_OK);
    mounted = true;
}

static void ev_fx_setup(ev_fx *f) {
    mount_once();
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_ASSET);
    ASSERT_EQ(de_type_registry_create(NULL, &f->types), DE_OK);
    de_world_desc wd;
    memset(&wd, 0, sizeof wd);
    wd.types = f->types;
    ASSERT_EQ(de_world_create(&wd, &f->w), DE_OK);
    ASSERT_EQ(de_asset_system_create(&(de_asset_system_desc){.allocator = &f->alloc}, &f->s),
              DE_OK);
    de__load_backend b = {fake_submit, fake_cancel, fake_reprioritize, f, NULL};
    de__asset_set_backend(f->s, &b);
    ASSERT_EQ(de_asset_manifest_load(f->s, "evdata:/assets.toml"), DE_OK);
}

static void ev_fx_teardown(ev_fx *f) {
    if (f->s != NULL)
        de_asset_system_destroy(f->s);
    f->s = NULL;
    if (f->w != NULL)
        de_world_destroy(f->w);
    if (f->types != NULL)
        de_type_registry_destroy(f->types);
    /* The event queue, the events state and everything else: gone. */
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).bytes_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0u);
}

static de_asset_id id_of(const char *path) {
    return de_asset_id_from(path);
}

static u32 slot_of(ev_fx *f, const char *path) {
    return de__slot_find(f->s, id_of(path));
}

static void complete(ev_fx *f, const char *path, de_result r) {
    u32 slot = slot_of(f, path);
    ASSERT_NE(slot, 0u);
    if (r != DE_OK)
        (void)DE_FAIL(r, "asset \"%s\": scripted failure", path);
    de__slot_complete(f->s, slot, r, 100);
}

/* Attaches the system to the world (first update) and creates the test's reader. */
static void attach(ev_fx *f) {
    de_asset_system_update(f->s, f->w);
    ASSERT_TRUE(DE_HANDLE_VALID(de_asset_event_type(f->s)));
    f->reader = de_event_reader_create(f->w, de_asset_event_type(f->s));
}

/* One host frame: the asset update, then First (de.asset_events sends) and Update. */
static void frame(ev_fx *f) {
    de_asset_system_update(f->s, f->w);
    ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_FIRST, 0.0f, 0.0f), DE_OK);
    ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_UPDATE, 0.0f, 0.0f), DE_OK);
}

/* Reads every unread event with the test's reader into f->got. */
static void drain(ev_fx *f) {
    f->got_count = 0;
    const de_asset_event *ev;
    while ((ev = (const de_asset_event *)de_event_next(f->w, &f->reader)) != NULL) {
        ASSERT_LT(f->got_count, EV_MAX);
        f->got[f->got_count++] = *ev;
    }
}

static void expect_asset_ev(const de_asset_event *ev, de_asset_event_kind kind, const char *path,
                            de_result error) {
    EXPECT_EQ(ev->kind, kind);
    EXPECT_EQ(ev->id.hash, id_of(path).hash);
    EXPECT_EQ(ev->handle.id, 0u);
    EXPECT_EQ(ev->state, kind == DE_ASSET_EV_LOADED ? DE_LOAD_READY : DE_LOAD_FAILED);
    EXPECT_EQ(ev->error, error);
}

static void expect_done_ev(const de_asset_event *ev, de_load_handle h, de_load_state state,
                           de_result error) {
    EXPECT_EQ(ev->kind, DE_ASSET_EV_REQUEST_DONE);
    EXPECT_EQ(ev->handle.id, h.id);
    EXPECT_EQ(ev->id.hash, 0u);
    EXPECT_EQ(ev->state, state);
    EXPECT_EQ(ev->error, error);
}

/* ---- events.loaded_failed ------------------------------------------------------------------- */

TEST_F(events, loaded_failed, ev_fx) {
    attach(f);
    de_load_handle h = de_asset_load_bundle(f->s, "abc", DE_LOAD_DEFAULT);
    ASSERT_TRUE(DE_HANDLE_VALID(h));
    EXPECT_EQ(f->submits, 3u);

    /* Completion order c, b (fails), a: the per-asset events follow it, then REQUEST_DONE. */
    complete(f, "ev/c", DE_OK);
    complete(f, "ev/b", DE_ERR_IO);
    complete(f, "ev/a", DE_OK);
    frame(f);
    drain(f);
    ASSERT_EQ(f->got_count, 4u);
    expect_asset_ev(&f->got[0], DE_ASSET_EV_LOADED, "ev/c", DE_OK);
    expect_asset_ev(&f->got[1], DE_ASSET_EV_FAILED, "ev/b", DE_ERR_IO);
    expect_asset_ev(&f->got[2], DE_ASSET_EV_LOADED, "ev/a", DE_OK);
    expect_done_ev(&f->got[3], h, DE_LOAD_PARTIAL, DE_ERR_IO);

    /* Exactly once: the next frames send nothing more. */
    frame(f);
    frame(f);
    drain(f);
    EXPECT_EQ(f->got_count, 0u);

    /* A second request for READY assets sends no per-asset event, only its REQUEST_DONE. */
    de_load_handle h2 = de_asset_load(f->s, id_of("ev/a"), DE_LOAD_DEFAULT);
    frame(f);
    drain(f);
    ASSERT_EQ(f->got_count, 1u);
    expect_done_ev(&f->got[0], h2, DE_LOAD_READY, DE_OK);
    de_asset_release(f->s, h2);
    de_asset_release(f->s, h);
}

/* ---- events.request_done -------------------------------------------------------------------- */

TEST_F(events, request_done, ev_fx) {
    attach(f);
    de_load_handle ha = de_asset_load(f->s, id_of("ev/a"), DE_LOAD_DEFAULT);
    de_load_handle hb = de_asset_load(f->s, id_of("ev/b"), DE_LOAD_DEFAULT);
    /* Final at once (between updates): their events come first, in call order. */
    de_load_handle he = de_asset_load_bundle(f->s, "empty", DE_LOAD_DEFAULT);
    de_load_handle hx = de_asset_load_bundle(f->s, "no_such_bundle", DE_LOAD_DEFAULT);
    de_load_handle hc = de_asset_load(f->s, id_of("ev/c"), DE_LOAD_DEFAULT);
    de_asset_cancel(f->s, hc);
    /* A request released before it is final sends nothing: its handle is stale. */
    de_load_handle hd = de_asset_load(f->s, id_of("ev/d"), DE_LOAD_DEFAULT);
    de_asset_release(f->s, hd);

    /* b completes before a, but REQUEST_DONE follows the request index order (a first). */
    complete(f, "ev/b", DE_OK);
    complete(f, "ev/a", DE_OK);
    frame(f);
    drain(f);
    ASSERT_EQ(f->got_count, 7u);
    expect_done_ev(&f->got[0], he, DE_LOAD_READY, DE_OK);
    expect_done_ev(&f->got[1], hx, DE_LOAD_FAILED, DE_ERR_NOT_FOUND);
    expect_done_ev(&f->got[2], hc, DE_LOAD_CANCELLED, DE_OK);
    expect_asset_ev(&f->got[3], DE_ASSET_EV_LOADED, "ev/b", DE_OK);
    expect_asset_ev(&f->got[4], DE_ASSET_EV_LOADED, "ev/a", DE_OK);
    expect_done_ev(&f->got[5], ha, DE_LOAD_READY, DE_OK);
    expect_done_ev(&f->got[6], hb, DE_LOAD_READY, DE_OK);
    EXPECT_EQ(f->cancels, 2u); /* c and d */

    /* Releasing a final request sends nothing either. */
    de_asset_release(f->s, ha);
    de_asset_release(f->s, hb);
    de_asset_release(f->s, he);
    de_asset_release(f->s, hx);
    de_asset_release(f->s, hc);
    frame(f);
    drain(f);
    EXPECT_EQ(f->got_count, 0u);
}

/* ---- events.no_events_flag ------------------------------------------------------------------ */

TEST_F(events, no_events_flag, ev_fx) {
    attach(f);
    /* Only a NO_EVENTS request waits on a: no event at all. */
    de_load_handle h1 = de_asset_load(f->s, id_of("ev/a"), DE_LOAD_NO_EVENTS);
    complete(f, "ev/a", DE_OK);
    /* A NO_EVENTS request fails on b: no FAILED, no REQUEST_DONE. */
    de_load_handle h2 = de_asset_load(f->s, id_of("ev/b"), DE_LOAD_NO_EVENTS);
    complete(f, "ev/b", DE_ERR_PARSE);
    frame(f);
    EXPECT_EQ(de_asset_status(f->s, h1, NULL), DE_LOAD_READY);
    EXPECT_EQ(de_asset_status(f->s, h2, NULL), DE_LOAD_FAILED);
    drain(f);
    EXPECT_EQ(f->got_count, 0u);

    /* c is wanted by a NO_EVENTS request and by one without the flag: one LOADED (the asset has
     * an interested request that wants events), and REQUEST_DONE for the second request only. */
    de_load_handle h3 = de_asset_load(f->s, id_of("ev/c"), DE_LOAD_NO_EVENTS);
    de_load_handle h4 = de_asset_load(f->s, id_of("ev/c"), DE_LOAD_DEFAULT);
    EXPECT_EQ(f->submits, 3u);
    complete(f, "ev/c", DE_OK);
    frame(f);
    drain(f);
    ASSERT_EQ(f->got_count, 2u);
    expect_asset_ev(&f->got[0], DE_ASSET_EV_LOADED, "ev/c", DE_OK);
    expect_done_ev(&f->got[1], h4, DE_LOAD_READY, DE_OK);

    /* d: the request that wanted events is cancelled before d finishes; the NO_EVENTS one keeps
     * waiting, so d's load sends nothing. */
    de_load_handle h5 = de_asset_load(f->s, id_of("ev/d"), DE_LOAD_NO_EVENTS);
    de_load_handle h6 = de_asset_load(f->s, id_of("ev/d"), DE_LOAD_DEFAULT);
    de_asset_cancel(f->s, h6);
    complete(f, "ev/d", DE_OK);
    frame(f);
    drain(f);
    ASSERT_EQ(f->got_count, 1u);
    expect_done_ev(&f->got[0], h6, DE_LOAD_CANCELLED, DE_OK);
    EXPECT_EQ(de_asset_status(f->s, h5, NULL), DE_LOAD_READY);

    de_load_handle hs[] = {h1, h2, h3, h4, h5, h6};
    for (u32 i = 0; i < 6; ++i)
        de_asset_release(f->s, hs[i]);
}

/* ---- events.two_frame_lifetime -------------------------------------------------------------- */

typedef struct ev_reader_sys {
    de_event_type type;
    de_event_reader reader;
    bool read_now;        /* this frame's run reads (else it skips) */
    u32 seen[EV_MAX / 4]; /* events read per frame */
    u32 frame;
} ev_reader_sys;

static void ev_reader_fn(de_system_ctx *ctx) {
    ev_reader_sys *r = (ev_reader_sys *)ctx->user;
    if (r->read_now) {
        while (de_event_next(ctx->world, &r->reader) != NULL)
            r->seen[r->frame]++;
    }
    r->frame++;
}

TEST_F(events, two_frame_lifetime, ev_fx) {
    attach(f);
    static ev_reader_sys late, fresh;
    memset(&late, 0, sizeof late);
    memset(&fresh, 0, sizeof fresh);
    de_event_type t = de_asset_event_type(f->s);
    late.type = fresh.type = t;
    late.reader = de_event_reader_create(f->w, t);

    de_system_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.name = "test.late_reader";
    sd.schedule = DE_SCHEDULE_UPDATE;
    sd.fn = ev_reader_fn;
    sd.user = &late;
    sd.flags = DE_SYSTEM_MAIN_THREAD;
    sd.reads_events = &t;
    sd.reads_event_count = 1;
    ASSERT_TRUE(DE_HANDLE_VALID(de_system_add(f->w, &sd)));

    de_load_handle h = de_asset_load(f->s, id_of("ev/a"), DE_LOAD_DEFAULT);
    complete(f, "ev/a", DE_OK);
    /* Frame 0: LOADED + REQUEST_DONE are sent in First; the late reader does not read yet. */
    frame(f);
    /* Frame 1: the events are still there (sent last frame). */
    late.read_now = true;
    frame(f);
    EXPECT_EQ(late.seen[0], 0u);
    EXPECT_EQ(late.seen[1], 2u);
    EXPECT_EQ(de_event_reader_missed(&late.reader), 0u);

    /* Frame 2: dropped. A reader created now sees nothing of them. */
    frame(f);
    fresh.reader = de_event_reader_create(f->w, t);
    EXPECT_TRUE(de_event_next(f->w, &fresh.reader) == NULL);
    EXPECT_EQ(late.seen[2], 0u);
    de_asset_release(f->s, h);
}

/* ---- events.null_emit_fn -------------------------------------------------------------------- */

TEST_F(events, null_emit_fn, ev_fx) {
    /* Never attached: updates without a world. Every transition happens, nothing is emitted. */
    EXPECT_TRUE(f->s->emit_fn == NULL);
    de_load_handle h = de_asset_load_bundle(f->s, "abc", DE_LOAD_DEFAULT);
    de_load_handle he = de_asset_load_bundle(f->s, "empty", DE_LOAD_DEFAULT);
    complete(f, "ev/a", DE_OK);
    complete(f, "ev/b", DE_ERR_IO);
    complete(f, "ev/c", DE_OK);
    de_asset_system_update(f->s, NULL);
    EXPECT_EQ(de_asset_status(f->s, h, NULL), DE_LOAD_PARTIAL);
    EXPECT_EQ(de_asset_status(f->s, he, NULL), DE_LOAD_READY);
    EXPECT_TRUE(f->s->emit_fn == NULL);
    EXPECT_TRUE(f->s->events == NULL);
    EXPECT_FALSE(DE_HANDLE_VALID(de_asset_event_type(f->s)));
    EXPECT_FALSE(DE_HANDLE_VALID(de_asset_resource(f->s)));

    /* Attached later: only what happens from then on is sent. */
    attach(f);
    frame(f);
    drain(f);
    EXPECT_EQ(f->got_count, 0u);
    de_load_handle hd = de_asset_load(f->s, id_of("ev/d"), DE_LOAD_DEFAULT);
    complete(f, "ev/d", DE_OK);
    frame(f);
    drain(f);
    ASSERT_EQ(f->got_count, 2u);
    expect_asset_ev(&f->got[0], DE_ASSET_EV_LOADED, "ev/d", DE_OK);
    expect_done_ev(&f->got[1], hd, DE_LOAD_READY, DE_OK);
    de_asset_release(f->s, h);
    de_asset_release(f->s, he);
    de_asset_release(f->s, hd);
}

/* ---- events.resource_ref -------------------------------------------------------------------- */

TEST_F(events, resource_ref, ev_fx) {
    EXPECT_FALSE(DE_HANDLE_VALID(de_asset_resource(f->s)));
    attach(f);
    de_resource_id rid = de_asset_resource(f->s);
    ASSERT_TRUE(DE_HANDLE_VALID(rid));
    const DeAssetSystemRef *ref = (const DeAssetSystemRef *)de_resource_get(f->w, rid);
    ASSERT_TRUE(ref != NULL);
    EXPECT_TRUE(ref->ptr == f->s);

    /* Found by type name too, the way a game finds it. */
    de_type_id ref_type = de_type_find(f->types, "DeAssetSystemRef");
    ASSERT_TRUE(DE_HANDLE_VALID(ref_type));
    EXPECT_EQ(de_resource_find(f->w, ref_type).id, rid.id);

    /* The payload type is reflected with its five fields, and its event type is the one the
     * world has for it. */
    de_type_id payload = de_type_find(f->types, "de_asset_event");
    ASSERT_TRUE(DE_HANDLE_VALID(payload));
    u32 n = 0;
    const de_field_info *fields = de_type_fields(f->types, payload, &n);
    ASSERT_EQ(n, 5u);
    const char *names[] = {"kind", "handle", "id", "state", "error"};
    for (u32 i = 0; i < 5; ++i)
        EXPECT_STREQ(fields[i].name, names[i]);
    EXPECT_EQ(de_event_register(f->w, payload).id, de_asset_event_type(f->s).id);

    /* Another update keeps everything (no second registration). */
    frame(f);
    EXPECT_EQ(de_asset_resource(f->s).id, rid.id);

    /* A second asset system cannot attach to the same world. */
    de_asset_system *other = NULL;
    ASSERT_EQ(de_asset_system_create(NULL, &other), DE_OK);
    de_asset_system_update(other, f->w);
    EXPECT_FALSE(DE_HANDLE_VALID(de_asset_event_type(other)));
    EXPECT_TRUE(other->emit_fn == NULL);
    ref = (const DeAssetSystemRef *)de_resource_get(f->w, rid);
    ASSERT_TRUE(ref != NULL);
    EXPECT_TRUE(ref->ptr == f->s);
    de_asset_system_destroy(other);

    /* Destroy detaches: the resource and the de.asset_events system leave the world. */
    de_asset_system_destroy(f->s);
    f->s = NULL;
    EXPECT_TRUE(de_resource_get(f->w, rid) == NULL);
    EXPECT_FALSE(DE_HANDLE_VALID(de_resource_find(f->w, ref_type)));
    EXPECT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_FIRST, 0.0f, 0.0f), DE_OK);
}

/* ---- events.update_no_alloc ----------------------------------------------------------------- */

TEST_F(events, update_no_alloc, ev_fx) {
    attach(f);
    /* Warm the queue and the event buffers with one load's events. */
    de_load_handle h = de_asset_load(f->s, id_of("ev/a"), DE_LOAD_DEFAULT);
    complete(f, "ev/a", DE_OK);
    frame(f);
    frame(f);
    frame(f);
    de_load_handle hb = de_asset_load(f->s, id_of("ev/b"), DE_LOAD_DEFAULT);

    /* Frames with no completion allocate nothing (the zero-alloc host test relies on it). */
    de_alloc_guard g;
    memset(&g, 0, sizeof g);
    g.this_thread_only = true;
    de_alloc_guard_begin(&g);
    for (u32 i = 0; i < 50; ++i)
        frame(f);
    de_alloc_guard_end(&g);
    DE_ALLOC_GUARD_ASSERT_ZERO(&g);
    de_asset_release(f->s, h);
    de_asset_release(f->s, hb);
}

/* ---- events.evicted_through_world ----------------------------------------------------------- */

TEST_F(events, evicted_through_world, ev_fx) {
    /* DENG-144's EVICTED events take the same path: queued, sent in First. */
    attach(f);
    de_load_handle h = de_asset_load(f->s, id_of("ev/a"), DE_LOAD_DEFAULT);
    complete(f, "ev/a", DE_OK);
    frame(f);
    drain(f);
    ASSERT_EQ(f->got_count, 2u); /* LOADED, REQUEST_DONE */
    de_asset_release(f->s, h);
    frame(f); /* the zero-ref asset becomes an eviction candidate */
    de_asset_flush_unused(f->s);
    frame(f);
    drain(f);
    ASSERT_EQ(f->got_count, 1u);
    EXPECT_EQ(f->got[0].kind, DE_ASSET_EV_EVICTED);
    EXPECT_EQ(f->got[0].id.hash, id_of("ev/a").hash);
}
