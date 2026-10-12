/*
 * test_budget.c: per-category budgets, residency accounting, LRU eviction, resident bundles,
 * de_asset_flush_unused, EVICTED events and de_asset_get_stats (DENG-144, SPEC §11.8).
 *
 * Most tests run on a scripted backend (below): it records submissions and completes them when
 * the test says so, with an object from the fixture's tracking allocator and a chosen resident
 * size, so every state is observable between two updates. budget.pipeline_reload runs the real
 * load pipeline on files written into a scratch directory of the build tree (mounted as bud:/).
 * Tests that check time-based behaviour (the over-budget warning rate, the I/O rate) call
 * de__budget_update with their own clock.
 */
#include "de_test.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "asset/asset_internal.h"
#include "asset/backend_internal.h"
#include "asset/budget_internal.h"
#include "asset/load_internal.h"
#include "asset/manifest_internal.h"
#include "asset/resolve_internal.h"
#include "asset/slots_internal.h"
#include "platform/de_atomic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KIB ((u64)1024)
#define MIB (KIB * 1024u)
#define SEC ((u64)1000000000u)

#define BUD_MAX_QUEUE  1024u
#define BUD_MAX_EVENTS 256u
#define BUD_MAX_SIZES  16u

/* ---- Fixture: system + scripted backend ------------------------------------------------------ */

typedef struct bud_obj {
    u64 id;
    u32 magic;
    u32 pad_;
} bud_obj;

#define BUD_MAGIC 0xB0D6E7u

typedef struct bud_fx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_asset_system *s;
    /* Backend. */
    u32 queue[BUD_MAX_QUEUE]; /**< submitted slots, not completed or cancelled */
    u32 queued;
    u32 submits, cancels;
    u64 size_default;                       /**< resident bytes of a completed asset */
    const char *size_prefix[BUD_MAX_SIZES]; /**< assets whose path starts with it ... */
    u64 size_bytes[BUD_MAX_SIZES];          /**< ... get these bytes */
    u32 size_count;
    /* Objects. */
    u32 unloads;
    u32 live_objects;
    /* Events (emit_fn). */
    de_asset_event events[BUD_MAX_EVENTS];
    u32 event_count;
} bud_fx;

static bud_fx *g_fx; /* the running test's fixture, for emit_fn */

static void bk_submit(void *user, de_asset_system *s, u32 slot, de__load_priority priority) {
    (void)s;
    (void)priority;
    bud_fx *f = (bud_fx *)user;
    ASSERT_LT(f->queued, BUD_MAX_QUEUE);
    f->queue[f->queued++] = slot;
    f->submits++;
}

static void bk_cancel(void *user, de_asset_system *s, u32 slot) {
    (void)s;
    bud_fx *f = (bud_fx *)user;
    for (u32 i = 0; i < f->queued; ++i)
        if (f->queue[i] == slot) {
            f->queue[i] = f->queue[--f->queued];
            f->cancels++;
            return;
        }
}

static void bk_reprioritize(void *user, de_asset_system *s, u32 slot, de__load_priority p) {
    (void)user;
    (void)s;
    (void)slot;
    (void)p;
}

static void bud_unload(void *object, void *user) {
    bud_fx *f = (bud_fx *)user;
    bud_obj *o = (bud_obj *)object;
    EXPECT_EQ(o->magic, BUD_MAGIC);
    o->magic = 0;
    DE_FREE(&f->alloc, o, sizeof *o);
    f->unloads++;
    f->live_objects--;
}

static void bud_emit(de_asset_system *s, const de_asset_event *ev) {
    (void)s;
    bud_fx *f = g_fx;
    ASSERT_LT(f->event_count, BUD_MAX_EVENTS);
    f->events[f->event_count++] = *ev;
}

/* Registers the typed-access type names with their categories (no loaders: the backend is
 * scripted). */
static void register_types(bud_fx *f) {
    static const struct {
        const char *name, *ext;
        de_asset_category cat;
    } types[] = {{"blob", ".blob", DE_ASSET_CAT_CPU},
                 {"texture", ".tex", DE_ASSET_CAT_GPU_TEXTURE},
                 {"mesh", ".mesh", DE_ASSET_CAT_GPU_MESH},
                 {"sound", ".snd", DE_ASSET_CAT_AUDIO},
                 {"weird", ".weird", (de_asset_category)77}};
    for (u32 i = 0; i < (u32)(sizeof types / sizeof types[0]); ++i) {
        const char *ext[1];
        ext[0] = types[i].ext;
        de_asset_type_desc d = {0};
        d.name = types[i].name;
        d.extensions = ext;
        d.extension_count = 1;
        d.unload = bud_unload;
        d.user = f;
        d.category = types[i].cat;
        ASSERT_TRUE(DE_HANDLE_VALID(de_asset_type_register(f->s, &d)));
    }
}

static void bud_fx_setup(bud_fx *f) {
    g_fx = f;
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_ASSET);
    ASSERT_EQ(de_asset_system_create(&(de_asset_system_desc){.allocator = &f->alloc}, &f->s),
              DE_OK);
    de__load_backend be = {
        .submit = bk_submit, .cancel = bk_cancel, .reprioritize = bk_reprioritize, .user = f};
    de__asset_set_backend(f->s, &be);
    f->size_default = 256 * KIB;
    register_types(f);
}

static void bud_fx_teardown(bud_fx *f) {
    de_asset_system_destroy(f->s);
    f->s = NULL;
    EXPECT_EQ(f->live_objects, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).bytes_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0u);
    g_fx = NULL;
}

/* ---- Helpers --------------------------------------------------------------------------------- */

static const char *g_text;

static de_result text_read(void *user, const char *vpath, const de_allocator *a, void **data,
                           u64 *size) {
    (void)user;
    (void)vpath;
    size_t n = strlen(g_text);
    char *b = (char *)DE_ALLOC(a, n + 1, 1, DE_MEM_GENERAL);
    if (b == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "text_read");
    memcpy(b, g_text, n + 1);
    *data = b;
    *size = n;
    return DE_OK;
}

/* Parses `text` as `vpath` and makes it the active manifest. */
static void install_manifest_at(de_asset_system *s, const char *vpath, const char *text) {
    g_text = text;
    de__manifest *m = NULL;
    de__manifest_parse_desc d = {.path = vpath, .allocator = s->alloc, .read = text_read};
    ASSERT_EQ(de__manifest_parse(&d, &m), DE_OK);
    ASSERT_EQ(m->error_count, 0u);
    de__resolved *r = NULL;
    ASSERT_EQ(de__manifest_resolve(m, &(de__resolve_desc){.allocator = s->alloc}, &r), DE_OK);
    ASSERT_EQ(r->error_count, 0u);
    de__resolved_destroy(s->resolved);
    de__manifest_destroy(s->manifest);
    s->manifest = m;
    s->resolved = r;
}

static void install_manifest(de_asset_system *s, const char *text) {
    install_manifest_at(s, "mem:/assets.toml", text);
}

/* A manifest of `count` assets "<prefix><i>" of `type` (appended to `buf`). */
static void add_assets(char *buf, size_t cap, const char *prefix, u32 count, const char *type,
                       const char *ext) {
    size_t len = strlen(buf);
    for (u32 i = 0; i < count; ++i) {
        int w = snprintf(buf + len, cap - len, "\"%s%u\" = { type = \"%s\", source = \"%u%s\" }\n",
                         prefix, i, type, i, ext);
        ASSERT_TRUE(w > 0 && (size_t)w < cap - len);
        len += (size_t)w;
    }
}

static de_asset_id idn(const char *prefix, u32 i) {
    char path[64];
    snprintf(path, sizeof path, "%s%u", prefix, i);
    return de_asset_id_from(path);
}

static u64 size_for(bud_fx *f, de_asset_id id) {
    const char *path = de_asset_id_path(f->s, id);
    for (u32 i = 0; path != NULL && i < f->size_count; ++i)
        if (strncmp(path, f->size_prefix[i], strlen(f->size_prefix[i])) == 0)
            return f->size_bytes[i];
    return f->size_default;
}

static void set_size(bud_fx *f, const char *prefix, u64 bytes) {
    ASSERT_LT(f->size_count, BUD_MAX_SIZES);
    f->size_prefix[f->size_count] = prefix;
    f->size_bytes[f->size_count] = bytes;
    f->size_count++;
}

/* Completes every outstanding submission with an object (the result applies at the next update).
 * Reports the bytes as read first, as a backend does. */
static void complete_all(bud_fx *f) {
    for (u32 i = 0; i < f->queued; ++i) {
        u32 slot = f->queue[i];
        de__slot *sl = de__slot_get(f->s, slot);
        ASSERT_TRUE(sl != NULL);
        bud_obj *o =
            (bud_obj *)DE_ALLOC(&f->alloc, sizeof(bud_obj), DE_ALIGNOF(bud_obj), DE_MEM_ASSET);
        ASSERT_TRUE(o != NULL);
        o->id = sl->id.hash;
        o->magic = BUD_MAGIC;
        f->live_objects++;
        de__slot_set_object(sl, o);
        u64 bytes = size_for(f, sl->id);
        de__slot_progress(f->s, slot, bytes, bytes);
        de__slot_complete(f->s, slot, DE_OK, bytes);
    }
    f->queued = 0;
}

static void update(bud_fx *f) {
    de_asset_system_update(f->s, NULL);
}

/* Loads `ids` as one request, completes it and returns the READY request. */
static de_load_handle load_ready(bud_fx *f, const de_asset_id *ids, u32 count) {
    de_load_handle h = de_asset_load_many(f->s, ids, count, DE_LOAD_DEFAULT);
    EXPECT_TRUE(DE_HANDLE_VALID(h));
    complete_all(f);
    update(f);
    EXPECT_EQ(de_asset_status(f->s, h, NULL), DE_LOAD_READY);
    return h;
}

static de_load_handle load_one(bud_fx *f, de_asset_id id) {
    return load_ready(f, &id, 1);
}

/* de_asset_get_stats(...).bytes_resident[i] through a local (MSVC C4223 on indexing an rvalue's
 * array). */
static u64 stat_bytes(bud_fx *f, u32 index) {
    de_asset_stats st = de_asset_get_stats(f->s);
    return st.bytes_resident[index];
}

static de_load_state state_of(bud_fx *f, de_asset_id id) {
    return de_asset_status_of(f->s, id);
}

static u32 refs_of(bud_fx *f, de_asset_id id) {
    const de__slot *sl = de__slot_get(f->s, de__slot_find(f->s, id));
    return sl != NULL ? (u32)de__slot_ref_total(sl) : 0u;
}

static void sort_ids(de_asset_id *ids, u32 n) {
    for (u32 i = 1; i < n; ++i)
        for (u32 k = i; k > 0 && ids[k - 1].hash > ids[k].hash; --k) {
            de_asset_id t = ids[k];
            ids[k] = ids[k - 1];
            ids[k - 1] = t;
        }
}

/* ---- budget.lru_order ------------------------------------------------------------------------ */

/* Ten 256 KiB CPU assets released over three frames (and one touched by typed access later),
 * then a 1 MiB budget: the update evicts the six least recently used (frame, then ID), and four
 * stay (= 1 MiB). */
TEST_F(budget, lru_order, bud_fx) {
    char text[4096] = "[assets]\n";
    add_assets(text, sizeof text, "cpu/a", 10, "blob", ".blob");
    install_manifest(f->s, text);
    f->s->emit_fn = bud_emit;

    de_load_handle h[10];
    for (u32 i = 0; i < 10; ++i)
        h[i] = load_one(f, idn("cpu/a", i));
    de_asset_stats st = de_asset_get_stats(f->s);
    EXPECT_EQ(st.bytes_resident[DE_ASSET_BYTES_CPU], 10 * 256 * KIB);
    EXPECT_EQ(st.assets_resident, 10u);

    /* Frame A: a5 and a2; frame B: a9; frame C: the rest. */
    de_asset_release(f->s, h[5]);
    de_asset_release(f->s, h[2]);
    update(f);
    de_asset_release(f->s, h[9]);
    update(f);
    static const u32 rest[] = {8, 0, 7, 1, 6, 3, 4};
    for (u32 i = 0; i < (u32)(sizeof rest / sizeof rest[0]); ++i)
        de_asset_release(f->s, h[rest[i]]);
    update(f);
    /* Frame D: typed access touches a2, so it is the most recently used now. */
    EXPECT_TRUE(DE_HANDLE_VALID(de_asset_blob(f->s, idn("cpu/a", 2))));
    update(f);
    EXPECT_EQ(f->unloads, 0u); /* no budget yet */

    /* Expected: a5 (A; a2 moved on), a9 (B), then frame C in ID order, then a2. */
    de_asset_id order[10];
    u32 n = 0;
    order[n++] = idn("cpu/a", 5);
    order[n++] = idn("cpu/a", 9);
    de_asset_id group[sizeof rest / sizeof rest[0]];
    for (u32 i = 0; i < (u32)(sizeof rest / sizeof rest[0]); ++i)
        group[i] = idn("cpu/a", rest[i]);
    sort_ids(group, (u32)(sizeof rest / sizeof rest[0]));
    for (u32 i = 0; i < (u32)(sizeof rest / sizeof rest[0]); ++i)
        order[n++] = group[i];
    order[n++] = idn("cpu/a", 2);

    de_asset_set_budgets(f->s, &(de_asset_budgets){.cpu_bytes = 1 * MIB});
    update(f);
    ASSERT_EQ(f->event_count, 6u);
    for (u32 i = 0; i < 6; ++i) {
        EXPECT_EQ(f->events[i].kind, DE_ASSET_EV_EVICTED);
        EXPECT_EQ(f->events[i].id.hash, order[i].hash);
        EXPECT_EQ(f->events[i].state, DE_LOAD_INVALID);
        EXPECT_EQ(f->events[i].handle.id, 0u);
        EXPECT_EQ(state_of(f, order[i]), DE_LOAD_INVALID);
    }
    for (u32 i = 6; i < 10; ++i)
        EXPECT_EQ(state_of(f, order[i]), DE_LOAD_READY);
    EXPECT_EQ(f->unloads, 6u);
    st = de_asset_get_stats(f->s);
    EXPECT_LE(st.bytes_resident[DE_ASSET_BYTES_CPU], 1 * MIB);
    EXPECT_EQ(st.bytes_resident[DE_ASSET_BYTES_CPU], 1 * MIB);
    EXPECT_EQ(st.assets_resident, 4u);

    /* At the budget, not over it: nothing more goes. */
    update(f);
    EXPECT_EQ(f->event_count, 6u);
    EXPECT_EQ(f->s->budget->over_budget_warns, 0u);
}

/* ---- budget.never_evicts_referenced ---------------------------------------------------------- */

TEST_F(budget, never_evicts_referenced, bud_fx) {
    char text[2048] = "[assets]\n";
    add_assets(text, sizeof text, "cpu/r", 6, "blob", ".blob");
    install_manifest(f->s, text);

    de_load_handle h[6];
    for (u32 i = 0; i < 6; ++i)
        h[i] = load_one(f, idn("cpu/r", i));
    de_asset_set_budgets(f->s, &(de_asset_budgets){.cpu_bytes = 1 * MIB});

    /* 1.5 MiB resident, all referenced: nothing is evicted, and the warning is rate-limited to
     * once per second (own clock, after every real-time update of this test). */
    u64 t0 = de_time_now_ns() + 100 * SEC;
    de__budget_update(f->s, t0);
    de__budget_update(f->s, t0 + SEC / 2);
    de__budget_update(f->s, t0 + SEC - 1);
    EXPECT_EQ(f->s->budget->over_budget_warns, 1u);
    de__budget_update(f->s, t0 + SEC);
    EXPECT_EQ(f->s->budget->over_budget_warns, 2u);
    de__budget_update(f->s, t0 + SEC + SEC / 2);
    EXPECT_EQ(f->s->budget->over_budget_warns, 2u);
    de__budget_update(f->s, t0 + 2 * SEC + 1);
    EXPECT_EQ(f->s->budget->over_budget_warns, 3u);
    EXPECT_EQ(f->unloads, 0u);
    EXPECT_EQ(stat_bytes(f, DE_ASSET_BYTES_CPU), 6 * 256 * KIB);
    for (u32 i = 0; i < 6; ++i)
        EXPECT_EQ(state_of(f, idn("cpu/r", i)), DE_LOAD_READY);

    /* r1 and r4 released: exactly those two go, and the rest stay referenced. */
    de_asset_release(f->s, h[1]);
    de_asset_release(f->s, h[4]);
    de__budget_update(f->s, t0 + 3 * SEC);
    EXPECT_EQ(f->unloads, 2u);
    EXPECT_EQ(state_of(f, idn("cpu/r", 1)), DE_LOAD_INVALID);
    EXPECT_EQ(state_of(f, idn("cpu/r", 4)), DE_LOAD_INVALID);
    EXPECT_EQ(stat_bytes(f, DE_ASSET_BYTES_CPU), 1 * MIB);

    /* A listed candidate that is referenced again before the update is not evicted. */
    de_asset_release(f->s, h[0]);
    de__budget_update(f->s, t0 + 4 * SEC); /* r0 is listed; at the budget, nothing goes */
    EXPECT_EQ(f->unloads, 2u);
    h[0] = de_asset_load(f->s, idn("cpu/r", 0), DE_LOAD_DEFAULT); /* READY at once */
    EXPECT_EQ(de_asset_status(f->s, h[0], NULL), DE_LOAD_READY);
    de_asset_set_budgets(f->s, &(de_asset_budgets){.cpu_bytes = 1});
    u32 warns = f->s->budget->over_budget_warns;
    de__budget_update(f->s, t0 + 6 * SEC);
    EXPECT_EQ(f->unloads, 2u);
    EXPECT_EQ(state_of(f, idn("cpu/r", 0)), DE_LOAD_READY);
    EXPECT_EQ(f->s->budget->over_budget_warns, warns + 1u);

    for (u32 i = 0; i < 6; ++i)
        de_asset_release(f->s, h[i]); /* stale handles of r1, r4 are ignored */
}

/* ---- budget.resident_bundle ------------------------------------------------------------------ */

TEST_F(budget, resident_bundle, bud_fx) {
    install_manifest(f->s, "[assets]\n"
                           "\"core/c0\" = { type = \"blob\", source = \"c0.blob\" }\n"
                           "\"core/c1\" = { type = \"blob\", source = \"c1.blob\" }\n"
                           "\"lvl/l0\" = { type = \"blob\", source = \"l0.blob\" }\n"
                           "\"lvl/l1\" = { type = \"blob\", source = \"l1.blob\" }\n"
                           "\"lvl/l2\" = { type = \"texture\", source = \"l2.tex\" }\n"
                           "[bundles.core]\n"
                           "assets = [\"core/*\"]\n"
                           "resident = true\n"
                           "[bundles.level]\n"
                           "assets = [\"lvl/*\"]\n"
                           "depends = [\"core\"]\n");
    de_load_handle h = de_asset_load_bundle(f->s, "level", DE_LOAD_DEFAULT);
    ASSERT_TRUE(DE_HANDLE_VALID(h));
    /* Pinned while the request is alive... */
    EXPECT_TRUE(de__budget_is_resident(f->s, de__slot_find(f->s, de_asset_id_from("core/c0"))));
    EXPECT_TRUE(de__budget_is_resident(f->s, de__slot_find(f->s, de_asset_id_from("core/c1"))));
    EXPECT_FALSE(de__budget_is_resident(f->s, de__slot_find(f->s, de_asset_id_from("lvl/l0"))));
    complete_all(f);
    update(f);
    ASSERT_EQ(de_asset_status(f->s, h, NULL), DE_LOAD_READY);

    /* ... and after release. */
    de_asset_release(f->s, h);
    update(f);
    de_asset_flush_unused(f->s);
    EXPECT_EQ(state_of(f, de_asset_id_from("core/c0")), DE_LOAD_READY);
    EXPECT_EQ(state_of(f, de_asset_id_from("core/c1")), DE_LOAD_READY);
    EXPECT_EQ(state_of(f, de_asset_id_from("lvl/l0")), DE_LOAD_INVALID);
    EXPECT_EQ(state_of(f, de_asset_id_from("lvl/l1")), DE_LOAD_INVALID);
    EXPECT_EQ(state_of(f, de_asset_id_from("lvl/l2")), DE_LOAD_INVALID);
    EXPECT_EQ(f->unloads, 3u);
    EXPECT_EQ(de_asset_get_stats(f->s).assets_resident, 2u);

    /* A budget does not move them either: over budget, no candidate, a warning. */
    de_asset_set_budgets(f->s, &(de_asset_budgets){.cpu_bytes = 1});
    update(f);
    EXPECT_EQ(state_of(f, de_asset_id_from("core/c0")), DE_LOAD_READY);
    EXPECT_EQ(f->unloads, 3u);
    EXPECT_EQ(f->s->budget->over_budget_warns, 1u);

    /* Loading a pinned asset on its own keeps it pinned. */
    de_load_handle h2 = de_asset_load(f->s, de_asset_id_from("core/c1"), DE_LOAD_DEFAULT);
    de_asset_release(f->s, h2);
    de_asset_flush_unused(f->s);
    EXPECT_EQ(state_of(f, de_asset_id_from("core/c1")), DE_LOAD_READY);
}

/* ---- budget.flush_unused --------------------------------------------------------------------- */

TEST_F(budget, flush_unused, bud_fx) {
    char text[2048] = "[assets]\n";
    add_assets(text, sizeof text, "cpu/f", 6, "blob", ".blob");
    install_manifest(f->s, text);

    de_load_handle h[5];
    for (u32 i = 0; i < 5; ++i)
        h[i] = load_one(f, idn("cpu/f", i));
    /* f5: a slot made by typed access, never loaded: nothing to evict, its handle stays. */
    de_blob never = de_asset_blob(f->s, idn("cpu/f", 5));
    ASSERT_TRUE(DE_HANDLE_VALID(never));

    de_asset_release(f->s, h[0]);
    de_asset_release(f->s, h[2]);
    de_asset_release(f->s, h[4]);
    /* No update in between: the flush takes the candidates the update has not seen yet. */
    de_asset_flush_unused(f->s);
    EXPECT_EQ(f->unloads, 3u);
    EXPECT_EQ(state_of(f, idn("cpu/f", 0)), DE_LOAD_INVALID);
    EXPECT_EQ(state_of(f, idn("cpu/f", 1)), DE_LOAD_READY);
    EXPECT_EQ(state_of(f, idn("cpu/f", 2)), DE_LOAD_INVALID);
    EXPECT_EQ(state_of(f, idn("cpu/f", 3)), DE_LOAD_READY);
    EXPECT_EQ(state_of(f, idn("cpu/f", 4)), DE_LOAD_INVALID);
    EXPECT_TRUE(de__slot_get(f->s, never.id) != NULL);
    de_asset_stats st = de_asset_get_stats(f->s);
    EXPECT_EQ(st.bytes_resident[DE_ASSET_BYTES_CPU], 2 * 256 * KIB);
    EXPECT_EQ(st.assets_resident, 2u);

    /* A second flush, and an update, have nothing left to do. */
    de_asset_flush_unused(f->s);
    update(f);
    EXPECT_EQ(f->unloads, 3u);

    de_asset_release(f->s, h[1]);
    de_asset_release(f->s, h[3]);
    update(f); /* now listed */
    de_asset_flush_unused(f->s);
    EXPECT_EQ(f->unloads, 5u);
    EXPECT_EQ(de_asset_get_stats(f->s).assets_resident, 0u);
    EXPECT_EQ(stat_bytes(f, DE_ASSET_BYTES_CPU), 0u);

    de_asset_flush_unused(NULL); /* ignored */
}

/* ---- budget.reload_after_evict --------------------------------------------------------------- */

TEST_F(budget, reload_after_evict, bud_fx) {
    install_manifest(f->s, "[assets]\n"
                           "\"tex/t0\" = { type = \"texture\", source = \"t0.tex\" }\n");
    de_asset_id id = de_asset_id_from("tex/t0");
    de_texture t = de_asset_texture(f->s, id); /* before anything is loaded */
    ASSERT_TRUE(DE_HANDLE_VALID(t));

    de_load_handle h = load_one(f, id);
    const bud_obj *o = (const bud_obj *)de__asset_object(f->s, t.id);
    ASSERT_TRUE(o != NULL);
    EXPECT_EQ(o->id, id.hash);
    EXPECT_EQ(f->submits, 1u);
    de_asset_release(f->s, h);
    de_asset_flush_unused(f->s);
    EXPECT_EQ(state_of(f, id), DE_LOAD_INVALID);
    EXPECT_TRUE(de__asset_object(f->s, t.id) == NULL);
    EXPECT_TRUE(de__slot_get(f->s, t.id) != NULL); /* still valid: same slot, same generation */
    EXPECT_EQ(de_asset_texture(f->s, id).id, t.id);
    EXPECT_EQ(stat_bytes(f, DE_ASSET_BYTES_GPU_TEXTURE), 0u);

    /* The next load submits it again; the old handle sees the new data. */
    h = de_asset_load(f->s, id, DE_LOAD_DEFAULT);
    EXPECT_EQ(de_asset_status(f->s, h, NULL), DE_LOAD_PENDING);
    EXPECT_EQ(f->submits, 2u);
    complete_all(f);
    update(f);
    EXPECT_EQ(de_asset_status(f->s, h, NULL), DE_LOAD_READY);
    o = (const bud_obj *)de__asset_object(f->s, t.id);
    ASSERT_TRUE(o != NULL);
    EXPECT_EQ(o->id, id.hash);
    EXPECT_EQ(stat_bytes(f, DE_ASSET_BYTES_GPU_TEXTURE), 256 * KIB);
    de_asset_release(f->s, h);
}

/* ---- budget.eviction_cap_per_update ---------------------------------------------------------- */

TEST_F(budget, eviction_cap_per_update, bud_fx) {
    char text[16384] = "[assets]\n";
    add_assets(text, sizeof text, "cpu/k", 100, "blob", ".blob");
    install_manifest(f->s, text);
    f->size_default = KIB;
    de_asset_id ids[100];
    for (u32 i = 0; i < 100; ++i)
        ids[i] = idn("cpu/k", i);
    de_load_handle h = load_ready(f, ids, 100);
    de_asset_release(f->s, h);
    update(f);
    EXPECT_EQ(de_asset_get_stats(f->s).assets_resident, 100u);

    de_asset_set_budgets(f->s, &(de_asset_budgets){.cpu_bytes = 1});
    update(f);
    EXPECT_EQ(f->unloads, DE__BUDGET_EVICT_CAP);
    EXPECT_EQ(de_asset_get_stats(f->s).assets_resident, 100u - DE__BUDGET_EVICT_CAP);
    update(f);
    EXPECT_EQ(f->unloads, 100u);
    EXPECT_EQ(stat_bytes(f, DE_ASSET_BYTES_CPU), 0u);
    update(f);
    EXPECT_EQ(f->unloads, 100u);
    EXPECT_EQ(f->s->budget->over_budget_warns, 0u); /* never over budget without candidates */
}

/* ---- budget.evicted_event_capture ------------------------------------------------------------ */

TEST_F(budget, evicted_event_capture, bud_fx) {
    char text[2048] = "[assets]\n";
    add_assets(text, sizeof text, "cpu/e", 6, "blob", ".blob");
    install_manifest(f->s, text);
    de_asset_id ids[6];
    for (u32 i = 0; i < 6; ++i)
        ids[i] = idn("cpu/e", i);

    f->s->emit_fn = bud_emit;
    de_asset_release(f->s, load_ready(f, ids, 3));
    de_asset_flush_unused(f->s);
    ASSERT_EQ(f->event_count, 3u);
    de_asset_id want[3] = {ids[0], ids[1], ids[2]};
    sort_ids(want, 3); /* one frame: ID order */
    for (u32 i = 0; i < 3; ++i) {
        EXPECT_EQ(f->events[i].kind, DE_ASSET_EV_EVICTED);
        EXPECT_EQ(f->events[i].id.hash, want[i].hash);
        EXPECT_EQ(f->events[i].state, DE_LOAD_INVALID);
        EXPECT_EQ(f->events[i].error, DE_OK);
    }
    de_asset_flush_unused(f->s);
    EXPECT_EQ(f->event_count, 3u); /* once per eviction */

    /* No emitter: eviction works the same, nothing is sent. */
    f->s->emit_fn = NULL;
    de_asset_release(f->s, load_ready(f, ids + 3, 3));
    de_asset_flush_unused(f->s);
    EXPECT_EQ(f->event_count, 3u);
    EXPECT_EQ(f->unloads, 6u);
    for (u32 i = 3; i < 6; ++i)
        EXPECT_EQ(state_of(f, ids[i]), DE_LOAD_INVALID);
}

/* ---- budget.stats_bytes_per_category --------------------------------------------------------- */

TEST_F(budget, stats_bytes_per_category, bud_fx) {
    char text[4096] = "[assets]\n";
    add_assets(text, sizeof text, "tex/t", 3, "texture", ".tex");
    add_assets(text, sizeof text, "mesh/m", 2, "mesh", ".mesh");
    add_assets(text, sizeof text, "snd/s", 1, "sound", ".snd");
    add_assets(text, sizeof text, "cpu/b", 4, "blob", ".blob");
    add_assets(text, sizeof text, "odd/w", 1, "weird", ".weird");
    install_manifest(f->s, text);
    set_size(f, "tex/", 100);
    set_size(f, "mesh/", 200);
    set_size(f, "snd/", 300);
    set_size(f, "cpu/", 50);
    set_size(f, "odd/", 7);

    de_asset_stats st = de_asset_get_stats(NULL);
    EXPECT_EQ(st.assets_resident, 0u);
    de_asset_id ids[11];
    u32 n = 0;
    for (u32 i = 0; i < 3; ++i)
        ids[n++] = idn("tex/t", i);
    for (u32 i = 0; i < 2; ++i)
        ids[n++] = idn("mesh/m", i);
    ids[n++] = idn("snd/s", 0);
    for (u32 i = 0; i < 4; ++i)
        ids[n++] = idn("cpu/b", i);
    ids[n++] = idn("odd/w", 0);

    de_load_handle h = de_asset_load_many(f->s, ids, n, DE_LOAD_DEFAULT);
    st = de_asset_get_stats(f->s);
    EXPECT_EQ(st.loads_in_flight, 11u);
    EXPECT_EQ(st.assets_resident, 0u);
    EXPECT_EQ(st.uploads_in_flight, 0u);
    complete_all(f);
    update(f);
    ASSERT_EQ(de_asset_status(f->s, h, NULL), DE_LOAD_READY);
    st = de_asset_get_stats(f->s);
    EXPECT_EQ(st.loads_in_flight, 0u);
    EXPECT_EQ(st.assets_resident, 11u);
    EXPECT_EQ(st.bytes_resident[DE_ASSET_BYTES_GPU_TEXTURE], 300u);
    EXPECT_EQ(st.bytes_resident[DE_ASSET_BYTES_GPU_MESH], 400u);
    EXPECT_EQ(st.bytes_resident[DE_ASSET_BYTES_AUDIO], 300u);
    EXPECT_EQ(st.bytes_resident[DE_ASSET_BYTES_CPU], 207u); /* 4 x 50 + the out-of-range type */

    /* A texture budget evicts textures only. */
    de_asset_release(f->s, h);
    update(f);
    de_asset_set_budgets(f->s, &(de_asset_budgets){.gpu_texture_bytes = 150});
    update(f);
    st = de_asset_get_stats(f->s);
    EXPECT_EQ(st.bytes_resident[DE_ASSET_BYTES_GPU_TEXTURE], 100u);
    EXPECT_EQ(st.bytes_resident[DE_ASSET_BYTES_GPU_MESH], 400u);
    EXPECT_EQ(st.bytes_resident[DE_ASSET_BYTES_AUDIO], 300u);
    EXPECT_EQ(st.bytes_resident[DE_ASSET_BYTES_CPU], 207u);
    EXPECT_EQ(st.assets_resident, 9u);
    EXPECT_EQ(f->unloads, 2u);
    /* NULL budgets: unlimited again. */
    de_asset_set_budgets(f->s, NULL);
    update(f);
    EXPECT_EQ(f->unloads, 2u);

    /* The I/O rate: 11 loads reported 1207 bytes read so far. With an own clock: bytes since the
     * oldest sample of the last second, per second. */
    u64 t0 = de_time_now_ns() + 100 * SEC;
    de__budget_update(f->s, t0);
    de__budget_add_io_bytes(f->s, 4000);
    de__budget_update(f->s, t0 + SEC / 2);
    EXPECT_EQ(de_asset_get_stats(f->s).io_bytes_per_sec, 8000u);
    de__budget_add_io_bytes(f->s, 1000);
    de__budget_update(f->s, t0 + SEC);
    EXPECT_EQ(de_asset_get_stats(f->s).io_bytes_per_sec, 5000u);
    de__budget_update(f->s, t0 + 3 * SEC); /* nothing read for over a second */
    EXPECT_EQ(de_asset_get_stats(f->s).io_bytes_per_sec, 0u);
    EXPECT_EQ(de_atomic_load_u64(&f->s->budget->io_bytes, DE_ATOMIC_RELAXED), 1207u + 5000u);
}

/* ---- budget.evict_drops_dependency_refs ------------------------------------------------------ */

/* An evicted parent drops its asset refs on its dependencies (as the pipeline stores them in
 * de__slot.deps), so they become candidates: in the next update, or at once in a flush. */
TEST_F(budget, evict_drops_dependency_refs, bud_fx) {
    install_manifest(f->s, "[assets]\n"
                           "\"dep/p\" = { type = \"blob\", source = \"p.blob\" }\n"
                           "\"dep/c\" = { type = \"blob\", source = \"c.blob\" }\n"
                           "\"dep/q\" = { type = \"blob\", source = \"q.blob\" }\n");
    de_asset_id p = de_asset_id_from("dep/p"), c = de_asset_id_from("dep/c"),
                q = de_asset_id_from("dep/q");
    /* Two rounds: eviction by budget in updates, then by flush. */
    for (u32 round = 0; round < 2; ++round) {
        de_load_handle hc = load_one(f, c);
        de_load_handle hp = load_one(f, p);
        de_load_handle hq = load_one(f, q);
        /* p depends on c: one asset -> asset ref, the ID in p's deps. */
        de__slot *sp = de__slot_get(f->s, de__slot_find(f->s, p));
        ASSERT_TRUE(sp != NULL && sp->deps == NULL);
        sp->deps = (de_asset_id *)DE_ALLOC(f->s->alloc, sizeof(de_asset_id),
                                           DE_ALIGNOF(de_asset_id), DE_MEM_ASSET);
        ASSERT_TRUE(sp->deps != NULL);
        sp->deps[0] = c;
        sp->dep_count = 1;
        ASSERT_TRUE(de__slot_ref_asset(f->s, de__slot_find(f->s, c)));
        de_asset_release(f->s, hc);
        de_asset_release(f->s, hp);
        update(f);
        EXPECT_EQ(refs_of(f, c), 1u); /* held by p only */
        u32 base = f->unloads;
        if (round == 0) {
            /* Room for one asset: p goes first (c is still referenced by it), then the list is
             * empty while still over budget (a warning): c becomes a candidate only when the
             * next update takes its refcount drop. */
            de_asset_set_budgets(f->s, &(de_asset_budgets){.cpu_bytes = 256 * KIB});
            update(f);
            EXPECT_EQ(f->unloads, base + 1u);
            EXPECT_EQ(state_of(f, p), DE_LOAD_INVALID);
            EXPECT_EQ(state_of(f, c), DE_LOAD_READY);
            EXPECT_EQ(refs_of(f, c), 0u);
            EXPECT_EQ(f->s->budget->over_budget_warns, 1u);
            /* Now c goes; q is referenced. */
            update(f);
            EXPECT_EQ(f->unloads, base + 2u);
            EXPECT_EQ(state_of(f, c), DE_LOAD_INVALID);
            EXPECT_EQ(state_of(f, q), DE_LOAD_READY);
            de_asset_set_budgets(f->s, NULL);
        } else {
            de_asset_flush_unused(f->s);
            EXPECT_EQ(f->unloads, base + 2u);
            EXPECT_EQ(state_of(f, p), DE_LOAD_INVALID);
            EXPECT_EQ(state_of(f, c), DE_LOAD_INVALID);
            EXPECT_EQ(state_of(f, q), DE_LOAD_READY);
        }
        de__slot *sp2 = de__slot_get(f->s, de__slot_find(f->s, p));
        EXPECT_TRUE(sp2->deps == NULL);
        EXPECT_EQ(sp2->dep_count, 0u);
        de_asset_release(f->s, hq);
        de_asset_flush_unused(f->s);
    }
}

/* ---- budget.publish_without_refs ------------------------------------------------------------- */

/* A load whose last request went away while it was in flight, but whose success still arrives,
 * is READY with no reference: a candidate at once. */
TEST_F(budget, publish_without_refs, bud_fx) {
    install_manifest(f->s, "[assets]\n"
                           "\"cpu/z\" = { type = \"blob\", source = \"z.blob\" }\n");
    de_asset_id id = de_asset_id_from("cpu/z");
    de_load_handle h = de_asset_load(f->s, id, DE_LOAD_DEFAULT);
    ASSERT_EQ(f->queued, 1u);
    /* The backend finishes, then the request goes before the completion is applied. */
    complete_all(f);
    de_asset_release(f->s, h);
    update(f);
    EXPECT_EQ(state_of(f, id), DE_LOAD_READY);
    EXPECT_EQ(refs_of(f, id), 0u);
    EXPECT_EQ(de_asset_get_stats(f->s).assets_resident, 1u);
    de_asset_set_budgets(f->s, &(de_asset_budgets){.cpu_bytes = 1});
    update(f);
    EXPECT_EQ(state_of(f, id), DE_LOAD_INVALID);
    EXPECT_EQ(f->unloads, 1u);
}

/* ---- budget.pipeline_reload ------------------------------------------------------------------ */

/* The real pipeline: a parent with a dependency (manifest setting `deps`), evicted by a flush
 * (both go), then loaded again through the same typed handle. */

typedef struct bt_obj {
    u64 size;
    const de_allocator *alloc;
} bt_obj;

typedef struct bt_counters {
    const de_allocator *alloc;
    de_atomic_u32 loads, unloads;
} bt_counters;

static de_result bt_load(de_asset_load_ctx *ctx) {
    bt_counters *c = (bt_counters *)de_asset_load_ctx_user(ctx);
    u64 n = 0;
    (void)de_asset_load_ctx_data(ctx, &n);
    const char *const *deps = NULL;
    u32 k = de_asset_load_ctx_settings_string_array(ctx, "deps", &deps);
    for (u32 i = 0; i < k; ++i)
        if (de_asset_load_ctx_add_dependency(ctx, de_asset_id_from(deps[i])) != DE_OK)
            return DE_ERR_OUT_OF_MEMORY;
    bt_obj *o = (bt_obj *)DE_ALLOC(c->alloc, sizeof *o, DE_ALIGNOF(bt_obj), DE_MEM_ASSET);
    if (o == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "bt: out of memory");
    o->size = n;
    o->alloc = c->alloc;
    de_asset_load_ctx_set_object(ctx, o, 1000 + n);
    (void)de_atomic_fetch_add_u32(&c->loads, 1, DE_ATOMIC_ACQ_REL);
    return DE_OK;
}

static void bt_unload(void *object, void *user) {
    bt_counters *c = (bt_counters *)user;
    DE_FREE(c->alloc, object, sizeof(bt_obj));
    (void)de_atomic_fetch_add_u32(&c->unloads, 1, DE_ATOMIC_ACQ_REL);
}

typedef struct pipe_bud_fx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_asset_system *s;
    bt_counters c;
} pipe_bud_fx;

static void pipe_bud_fx_setup(pipe_bud_fx *f) {
    static bool mounted;
    if (!mounted) {
        ASSERT_EQ(de_fs_mount("bud", DE_ASSET_BUDGET_TMP_DIR, DE_MOUNT_WRITABLE, 0), DE_OK);
        mounted = true;
    }
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_ASSET);
    f->c.alloc = &f->alloc;
    de_atomic_init_u32(&f->c.loads, 0);
    de_atomic_init_u32(&f->c.unloads, 0);
    ASSERT_EQ(de_asset_system_create(
                  &(de_asset_system_desc){.allocator = &f->alloc, .io_threads = 1}, &f->s),
              DE_OK);
    static const char *const exts[] = {".bt"};
    de_asset_type_desc d = {0};
    d.name = "bt";
    d.extensions = exts;
    d.extension_count = 1;
    d.load = bt_load;
    d.unload = bt_unload;
    d.user = &f->c;
    d.category = DE_ASSET_CAT_AUDIO;
    ASSERT_TRUE(DE_HANDLE_VALID(de_asset_type_register(f->s, &d)));
}

static void pipe_bud_fx_teardown(pipe_bud_fx *f) {
    de_asset_system_destroy(f->s);
    f->s = NULL;
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).bytes_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0u);
}

TEST_F(budget, pipeline_reload, pipe_bud_fx) {
    ASSERT_EQ(de_file_write_all_atomic("bud:/p.bt", "parent", 6), DE_OK);
    ASSERT_EQ(de_file_write_all_atomic("bud:/c.bt", "child!!", 7), DE_OK);
    install_manifest_at(f->s, "bud:/assets.toml",
                        "[assets]\n"
                        "\"bt/p\" = { type = \"bt\", source = \"p.bt\", deps = [\"bt/c\"] }\n"
                        "\"bt/c\" = { type = \"bt\", source = \"c.bt\" }\n");
    de_asset_id p = de_asset_id_from("bt/p"), c = de_asset_id_from("bt/c");
    u32 tp = de__asset_handle_of(f->s, p, "bt");
    ASSERT_TRUE(tp != 0);

    for (u32 round = 0; round < 2; ++round) {
        de_load_handle h = de_asset_load(f->s, p, DE_LOAD_DEFAULT);
        ASSERT_EQ(de_asset_wait(f->s, h, (u64)60 * SEC), DE_LOAD_READY);
        EXPECT_EQ(de_asset_status_of(f->s, c), DE_LOAD_READY);
        const bt_obj *o = (const bt_obj *)de__asset_object(f->s, tp);
        ASSERT_TRUE(o != NULL);
        EXPECT_EQ(o->size, 6u);
        de_asset_stats st = de_asset_get_stats(f->s);
        EXPECT_EQ(st.bytes_resident[DE_ASSET_BYTES_AUDIO], 1006u + 1007u);
        EXPECT_EQ(st.assets_resident, 2u);
        EXPECT_EQ(de_atomic_load_u32(&f->c.loads, DE_ATOMIC_ACQUIRE), 2u * (round + 1u));

        de_asset_release(f->s, h);
        de_asset_system_update(f->s, NULL);
        de_asset_flush_unused(f->s); /* p, then c once p's ref on it is gone */
        EXPECT_EQ(de_asset_status_of(f->s, p), DE_LOAD_INVALID);
        EXPECT_EQ(de_asset_status_of(f->s, c), DE_LOAD_INVALID);
        EXPECT_EQ(de_atomic_load_u32(&f->c.unloads, DE_ATOMIC_ACQUIRE), 2u * (round + 1u));
        EXPECT_TRUE(de__asset_object(f->s, tp) == NULL);
        EXPECT_TRUE(de__slot_get(f->s, tp) != NULL);
        st = de_asset_get_stats(f->s);
        EXPECT_EQ(st.assets_resident, 0u);
        EXPECT_EQ(st.bytes_resident[DE_ASSET_BYTES_AUDIO], 0u);
    }
    /* Both reads were reported: 2 x (6 + 7) bytes. */
    EXPECT_EQ(de_atomic_load_u64(&f->s->budget->io_bytes, DE_ATOMIC_RELAXED), 26u);
}
