/*
 * test_slots.c: asset slots, typed handles with generations, refcounts and the eviction-candidate
 * list (DENG-140, SPEC §11.1, §11.8).
 *
 * Manifests are parsed from memory and installed as the system's active manifest, the way
 * de_asset_manifest_load does after a successful parse.
 */
#include "de_test.h"
#include "test_bytes_type.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "asset/asset_internal.h"
#include "asset/manifest_internal.h"
#include "asset/resolve_internal.h"
#include "asset/slots_internal.h"
#include "platform/de_atomic.h"

#include <stdlib.h>
#include <string.h>

/* ---- Helpers ----------------------------------------------------------------------------- */

static const char *g_text;

static de_result one_file_read(void *user, const char *vpath, const de_allocator *a, void **data,
                               u64 *size) {
    (void)user;
    (void)vpath;
    size_t n = strlen(g_text);
    char *b = (char *)DE_ALLOC(a, n + 1, 1, DE_MEM_GENERAL);
    if (b == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "one_file_read");
    memcpy(b, g_text, n + 1);
    *data = b;
    *size = n;
    return DE_OK;
}

/* Parses and resolves `text` as mem:/assets.toml and makes it the active manifest of `s`, the
 * way de_asset_manifest_load does after a clean parse. */
static void install_manifest(de_asset_system *s, const char *text) {
    g_text = text;
    de__manifest *m = NULL;
    de__manifest_parse_desc d = {
        .path = "mem:/assets.toml", .allocator = s->alloc, .read = one_file_read};
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

static const char k_manifest[] =
    "[assets]\n"
    "\"data/a\"      = { type = \"test_bytes\", source = \"a.bytes\" }\n"
    "\"data/b\"      = { type = \"test_bytes\", source = \"b.bytes\" }\n"
    "\"music/title\" = { type = \"sound\",   source = \"title.ogg\" }\n"
    "\"ui/logo\"     = { type = \"texture\", source = \"logo.png\" }\n"
    "\"misc/blob\"   = { type = \"blob\",    source = \"x.bin\" }\n";

static de_asset_id id_of(const char *path) {
    return de_asset_id_from(path);
}

/* splitmix64: well-spread test IDs. */
static u64 mix64(u64 x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    x ^= x >> 31;
    return x != 0 ? x : 1u;
}

typedef struct sys_fx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_asset_system *s;
    test_bytes_counters counters;
} sys_fx;

static void sys_fx_setup(sys_fx *f) {
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_ASSET);
    ASSERT_EQ(de_asset_system_create(&(de_asset_system_desc){.allocator = &f->alloc}, &f->s),
              DE_OK);
}

static void sys_fx_teardown(sys_fx *f) {
    de_asset_system_destroy(f->s);
    f->s = NULL;
    /* Everything the slots and the registry allocated is gone. */
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).bytes_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0u);
}

static u32 create_slot(de_asset_system *s, u64 id) {
    u32 h = 0;
    ASSERT_EQ(de__slot_create(s, (de_asset_id){id}, de_hash64_str(TEST_BYTES_TYPE), &h), DE_OK);
    ASSERT_NE(h, 0u);
    return h;
}

/* ---- Handles and generations ---------------------------------------------------------------- */

TEST(slots, handle_encoding) {
    u32 h = de__slot_handle_make(DE__SLOT_MAX - 1u, DE__SLOT_GEN_MAX);
    EXPECT_EQ(de__slot_handle_index(h), DE__SLOT_MAX - 1u);
    EXPECT_EQ(de__slot_handle_generation(h), DE__SLOT_GEN_MAX);
    EXPECT_EQ(h, 0xFFFFFFFFu);
    h = de__slot_handle_make(0, 1);
    EXPECT_NE(h, 0u); /* index 0 with the lowest generation is still a valid id */
    EXPECT_EQ(de__slot_handle_index(h), 0u);
    EXPECT_EQ(de__slot_handle_generation(h), 1u);
}

TEST_F(slots, generation_stale, sys_fx) {
    de_asset_system *s = f->s;
    /* Fill up the free list past the reuse threshold so the next create reuses index 0. */
    enum { N = 66 };
    u32 h[N];
    for (u32 i = 0; i < N; ++i)
        h[i] = create_slot(s, mix64(i));
    EXPECT_EQ(de__slot_live_count(s), (u32)N);
    for (u32 i = 0; i < N; ++i)
        ASSERT_EQ(de__slot_free(s, h[i]), DE_OK);
    EXPECT_EQ(de__slot_live_count(s), 0u);
    for (u32 i = 0; i < N; ++i) {
        EXPECT_TRUE(de__slot_get(s, h[i]) == NULL); /* stale */
        EXPECT_EQ(de__slot_find(s, (de_asset_id){mix64(i)}), 0u);
    }

    u32 again = create_slot(s, 0xABCDEFull);
    EXPECT_EQ(de__slot_handle_index(again), de__slot_handle_index(h[0])); /* FIFO reuse */
    EXPECT_NE(de__slot_handle_generation(again), de__slot_handle_generation(h[0]));
    EXPECT_NE(again, h[0]);
    EXPECT_TRUE(de__slot_get(s, h[0]) == NULL); /* the old handle resolves to nothing */
    EXPECT_TRUE(de__asset_object(s, h[0]) == NULL);
    de__slot *sl = de__slot_get(s, again);
    ASSERT_TRUE(sl != NULL);
    EXPECT_EQ(sl->id.hash, 0xABCDEFull);
    EXPECT_EQ(de__slot_find(s, (de_asset_id){0xABCDEFull}), again);

    /* Stale and invalid handles everywhere. */
    EXPECT_TRUE(de__slot_get(s, 0) == NULL);
    EXPECT_TRUE(de__slot_get(s, de__slot_handle_make(5000, 1)) == NULL); /* no page there */
    EXPECT_TRUE(de__slot_get(NULL, again) == NULL);
    EXPECT_EQ(de__slot_free(s, h[0]), DE_ERR_INVALID_ARGUMENT);
    EXPECT_FALSE(de__slot_ref_handle(s, h[0]));
    EXPECT_FALSE(de__slot_unref_handle(s, h[0]));
}

TEST_F(slots, generation_wraps_never_zero, sys_fx) {
    de_asset_system *s = f->s;
    /* 65 free slots: each create takes the oldest free index, each free puts it last, so the
     * slots cycle and index 0 comes back every 65 creates. */
    enum { N = 65 };
    u32 h[N];
    for (u32 i = 0; i < N; ++i)
        h[i] = create_slot(s, mix64(i));
    for (u32 i = 0; i < N; ++i)
        ASSERT_EQ(de__slot_free(s, h[i]), DE_OK);
    u32 first_gen = de__slot_handle_generation(h[0]);
    u32 seen_wrap = 0, prev_gen = first_gen;
    for (u32 round = 0; round < DE__SLOT_GEN_MAX + 2u; ++round) {
        for (u32 i = 0; i < N; ++i) {
            u32 nh = 0;
            ASSERT_EQ(de__slot_create(s, (de_asset_id){mix64(1000 + i)}, 1, &nh), DE_OK);
            ASSERT_NE(nh, 0u);
            ASSERT_NE(de__slot_handle_generation(nh), 0u);
            if (de__slot_handle_index(nh) == 0) {
                u32 g = de__slot_handle_generation(nh);
                if (g < prev_gen) {
                    EXPECT_EQ(prev_gen, DE__SLOT_GEN_MAX);
                    EXPECT_EQ(g, 1u);
                    seen_wrap++;
                }
                prev_gen = g;
            }
            ASSERT_EQ(de__slot_free(s, nh), DE_OK);
        }
    }
    EXPECT_EQ(seen_wrap, 1u);
}

TEST_F(slots, create_duplicate_and_free_rules, sys_fx) {
    de_asset_system *s = f->s;
    u32 h = create_slot(s, 42);
    u32 dup = 7;
    de_error_clear();
    EXPECT_EQ(de__slot_create(s, (de_asset_id){42}, 1, &dup), DE_ERR_ALREADY_EXISTS);
    EXPECT_EQ(dup, h);
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_ALREADY_EXISTS);
    EXPECT_EQ(de__slot_create(s, (de_asset_id){0}, 1, &dup), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(dup, 0u);
    EXPECT_EQ(de__slot_create(s, (de_asset_id){43}, 1, NULL), DE_ERR_INVALID_ARGUMENT);

    /* A referenced slot cannot be freed. */
    EXPECT_TRUE(de__slot_ref_asset(s, h));
    EXPECT_EQ(de__slot_free(s, h), DE_ERR_BUSY);
    EXPECT_TRUE(de__slot_unref_asset(s, h)); /* last ref */
    EXPECT_EQ(de__slot_free(s, h), DE_OK);
    /* It was on the candidate list when freed; the drain skips it. */
    EXPECT_EQ(de__slot_eviction_drain(s, NULL, NULL), 0u);
}

/* ---- Capacity ------------------------------------------------------------------------------ */

TEST(slots, capacity) {
    de_asset_system *s = NULL;
    ASSERT_EQ(de_asset_system_create(NULL, &s), DE_OK);
    u32 *handles = (u32 *)malloc(sizeof(u32) * DE__SLOT_MAX);
    u8 *seen = (u8 *)calloc(DE__SLOT_MAX / 8u, 1);
    ASSERT_TRUE(handles != NULL && seen != NULL);

    /* 2^20 (1M) slots: every index is used once, never more than 2^20 - 1. */
    for (u32 i = 0; i < DE__SLOT_MAX; ++i) {
        u32 h = 0;
        de_result r = de__slot_create(s, (de_asset_id){mix64(i)}, 1, &h);
        if (r != DE_OK) {
            FAIL("slot create failed before 2^20 slots");
        }
        u32 index = de__slot_handle_index(h);
        ASSERT_LE(index, DE__SLOT_MAX - 1u);
        ASSERT_EQ(seen[index >> 3] & (1u << (index & 7u)), 0);
        seen[index >> 3] = (u8)(seen[index >> 3] | (1u << (index & 7u)));
        handles[i] = h;
    }
    EXPECT_EQ(de__slot_live_count(s), DE__SLOT_MAX);

    /* Round trip: ID -> handle -> slot -> ID. */
    for (u32 i = 0; i < DE__SLOT_MAX; ++i) {
        u64 id = mix64(i);
        if (de__slot_find(s, (de_asset_id){id}) != handles[i])
            FAIL("de__slot_find gave another handle");
        const de__slot *sl = de__slot_get(s, handles[i]);
        if (sl == NULL || sl->id.hash != id)
            FAIL("de__slot_get gave another slot");
    }

    /* One more: invalid handle and DE_ERR_OUT_OF_MEMORY. */
    u32 h = 123;
    de_error_clear();
    EXPECT_EQ(de__slot_create(s, (de_asset_id){0x5EED}, 1, &h), DE_ERR_OUT_OF_MEMORY);
    EXPECT_EQ(h, 0u);
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_OUT_OF_MEMORY);
    EXPECT_EQ(de__slot_find(s, (de_asset_id){0x5EED}), 0u); /* not half-inserted */

    /* With every fresh index used, a freed slot is reused at once. */
    ASSERT_EQ(de__slot_free(s, handles[777]), DE_OK);
    ASSERT_EQ(de__slot_create(s, (de_asset_id){0x5EED}, 1, &h), DE_OK);
    EXPECT_EQ(de__slot_handle_index(h), de__slot_handle_index(handles[777]));
    EXPECT_TRUE(de__slot_get(s, handles[777]) == NULL);

    free(seen);
    free(handles);
    de_asset_system_destroy(s);
}

/* ---- Typed access ---------------------------------------------------------------------------- */

TEST_F(slots, typed_access_wrong_type, sys_fx) {
    de_asset_system *s = f->s;
    install_manifest(s, k_manifest);
    de_asset_id title = id_of("music/title");

    de_error_clear();
    de_texture t = de_asset_texture(s, title); /* declared as `sound` */
    EXPECT_FALSE(DE_HANDLE_VALID(t));
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(strstr(de_error_last()->message, "music/title") != NULL);
    EXPECT_EQ(de__slot_live_count(s), 0u); /* no slot was created for the failed call */

    de_sound snd = de_asset_sound(s, title);
    EXPECT_TRUE(DE_HANDLE_VALID(snd));
    /* With the slot in place, the wrong type is still refused. */
    de_error_clear();
    EXPECT_FALSE(DE_HANDLE_VALID(de_asset_texture(s, title)));
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    EXPECT_FALSE(DE_HANDLE_VALID(de_asset_mesh(s, title)));
    EXPECT_FALSE(DE_HANDLE_VALID(de_asset_model(s, title)));
    EXPECT_FALSE(DE_HANDLE_VALID(de_asset_material(s, title)));
    EXPECT_FALSE(DE_HANDLE_VALID(de_asset_font(s, title)));
    EXPECT_FALSE(DE_HANDLE_VALID(de_asset_prefab(s, title)));
    EXPECT_FALSE(DE_HANDLE_VALID(de_asset_blob(s, title)));
    EXPECT_EQ(de__asset_handle_of(s, title, "test_bytes"), 0u);

    /* The right types work, and the generic form with no type accepts anything. */
    EXPECT_TRUE(DE_HANDLE_VALID(de_asset_texture(s, id_of("ui/logo"))));
    EXPECT_TRUE(DE_HANDLE_VALID(de_asset_blob(s, id_of("misc/blob"))));
    EXPECT_EQ(de__asset_handle_of(s, title, NULL), snd.id);
}

TEST_F(slots, lazy_create, sys_fx) {
    de_asset_system *s = f->s;
    de_asset_type tb = test_bytes_register(s, &f->counters);
    ASSERT_TRUE(DE_HANDLE_VALID(tb));

    /* No manifest: nothing is declared. */
    de_error_clear();
    EXPECT_EQ(de__asset_handle_of(s, id_of("data/a"), TEST_BYTES_TYPE), 0u);
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_NOT_FOUND);

    install_manifest(s, k_manifest);
    EXPECT_EQ(de__slot_live_count(s), 0u); /* no slot before first use */
    u32 a = de__asset_handle_of(s, id_of("data/a"), TEST_BYTES_TYPE);
    ASSERT_NE(a, 0u);
    EXPECT_EQ(de__slot_live_count(s), 1u);
    EXPECT_EQ(de__asset_handle_of(s, id_of("data/a"), TEST_BYTES_TYPE), a); /* same slot */
    EXPECT_EQ(de__slot_live_count(s), 1u);

    const de__slot *sl = de__slot_get(s, a);
    ASSERT_TRUE(sl != NULL);
    EXPECT_EQ(sl->id.hash, id_of("data/a").hash);
    EXPECT_EQ(sl->type.id, tb.id); /* registered before the slot */
    EXPECT_EQ(sl->state, DE_LOAD_INVALID);
    EXPECT_EQ(de__slot_ref_total(sl), 0u);       /* typed access takes no reference */
    EXPECT_TRUE(de__asset_object(s, a) == NULL); /* valid before anything is loaded */

    /* The slot of a type registered later resolves through its type name. */
    u32 snd = de_asset_sound(s, id_of("music/title")).id;
    ASSERT_NE(snd, 0u);
    EXPECT_FALSE(DE_HANDLE_VALID(de__slot_type(s, de__slot_get(s, snd))));
    static const char *const ogg[] = {"ogg"};
    de_asset_type st = de_asset_type_register(
        s, &(de_asset_type_desc){.name = "sound", .extensions = ogg, .extension_count = 1});
    ASSERT_TRUE(DE_HANDLE_VALID(st));
    EXPECT_EQ(de__slot_type(s, de__slot_get(s, snd)).id, st.id);

    /* Undeclared and invalid IDs. */
    de_error_clear();
    EXPECT_EQ(de__asset_handle_of(s, id_of("nope"), NULL), 0u);
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_NOT_FOUND);
    de_error_clear();
    EXPECT_FALSE(DE_HANDLE_VALID(de_asset_blob(s, (de_asset_id){0})));
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de__asset_handle_of(NULL, id_of("data/a"), NULL), 0u);

    /* A new manifest is indexed again; existing slots keep their handles. */
    install_manifest(s, "[assets]\n\"data/c\" = { type = \"test_bytes\", source = \"c.bytes\" }\n");
    EXPECT_NE(de__asset_handle_of(s, id_of("data/c"), TEST_BYTES_TYPE), 0u);
    EXPECT_EQ(de__asset_handle_of(s, id_of("data/a"), TEST_BYTES_TYPE), a);
    EXPECT_EQ(de__asset_handle_of(s, id_of("data/b"), TEST_BYTES_TYPE), 0u); /* gone */
}

TEST_F(slots, validator_takes_type_names, sys_fx) {
    /* The registry's name set is what DENG-139's validator checks manifest types against. */
    de_asset_system *s = f->s;
    ASSERT_TRUE(DE_HANDLE_VALID(test_bytes_register(s, NULL)));
    install_manifest(s, k_manifest);
    u32 n = 0;
    const char *const *names = de__asset_type_names(s, &n);
    ASSERT_EQ(n, 1u);
    u32 before = s->resolved->error_count;
    de__resolved *r = s->resolved;
    ASSERT_EQ(de__manifest_validate(r, &(de__validate_desc){.known_types = names,
                                                            .known_type_count = n,
                                                            .skip_sources = true}),
              DE_OK);
    /* sound, texture and blob are not registered; the two test_bytes assets are fine. */
    EXPECT_EQ(r->error_count - before, 3u);
}

TEST_F(slots, object_and_unload, sys_fx) {
    de_asset_system *s = f->s;
    ASSERT_TRUE(DE_HANDLE_VALID(test_bytes_register(s, &f->counters)));
    install_manifest(s, k_manifest);
    u32 a = de__asset_handle_of(s, id_of("data/a"), TEST_BYTES_TYPE);
    u32 b = de__asset_handle_of(s, id_of("data/b"), TEST_BYTES_TYPE);
    ASSERT_NE(a, 0u);
    ASSERT_NE(b, 0u);
    static int obj_a, obj_b;
    de__slot_set_object(de__slot_get(s, a), &obj_a);
    de__slot_set_object(de__slot_get(s, b), &obj_b);
    EXPECT_TRUE(de__asset_object(s, a) == &obj_a);

    /* Dependencies are owned by the slot and freed with it. */
    de__slot *sa = de__slot_get(s, a);
    sa->deps = (de_asset_id *)DE_ALLOC(s->alloc, sizeof(de_asset_id) * 2, DE_ALIGNOF(de_asset_id),
                                       DE_MEM_ASSET);
    ASSERT_TRUE(sa->deps != NULL);
    sa->dep_count = 2;

    ASSERT_EQ(de__slot_free(s, a), DE_OK);
    EXPECT_EQ(f->counters.unloads, 1u);
    EXPECT_TRUE(f->counters.last_unloaded == &obj_a);
    EXPECT_TRUE(de__asset_object(s, a) == NULL);
    /* Destroying the system unloads what is still live (b); the teardown checks the memory. */
    de_asset_system_destroy(s);
    f->s = NULL;
    EXPECT_EQ(f->counters.unloads, 2u);
    EXPECT_TRUE(f->counters.last_unloaded == &obj_b);
}

TEST_F(slots, oom_sweep, sys_fx) {
    de_asset_system *s = f->s;
    install_manifest(s, k_manifest);
    /* Fail the n-th allocation of a first typed access (slot table, ID index, page, ID map) for
     * every n until it succeeds: each failure is DE_ERR_OUT_OF_MEMORY and leaves no slot. */
    u32 h = 0;
    for (i64 n = 0; n < 64 && h == 0; ++n) {
        de_tracking_allocator_fail_after(&f->track, n);
        de_error_clear();
        h = de__asset_handle_of(s, id_of("data/a"), TEST_BYTES_TYPE);
        de_tracking_allocator_fail_after(&f->track, -1);
        if (h == 0) {
            EXPECT_EQ(de_error_last() != NULL ? de_error_last()->result : DE_OK,
                      DE_ERR_OUT_OF_MEMORY);
            EXPECT_EQ(de__slot_live_count(s), 0u);
            EXPECT_EQ(de__slot_find(s, id_of("data/a")), 0u);
        }
    }
    ASSERT_NE(h, 0u);
    EXPECT_EQ(de__asset_handle_of(s, id_of("data/a"), TEST_BYTES_TYPE), h);
    EXPECT_EQ(de__slot_live_count(s), 1u);
}

/* ---- Refcounts and eviction candidates ------------------------------------------------------ */

typedef struct drain_log {
    u32 count;
    u32 handles[8];
    bool free_them;
} drain_log;

static void drain_cb(de_asset_system *s, u32 handle_id, void *user) {
    drain_log *log = (drain_log *)user;
    if (log->count < 8)
        log->handles[log->count] = handle_id;
    log->count++;
    if (log->free_them)
        EXPECT_EQ(de__slot_free(s, handle_id), DE_OK);
}

TEST_F(slots, refcount_counts_and_candidates, sys_fx) {
    de_asset_system *s = f->s;
    u32 h = create_slot(s, 1);
    const de__slot *sl = de__slot_get(s, h);
    EXPECT_TRUE(de__slot_ref_handle(s, h));
    EXPECT_TRUE(de__slot_ref_handle(s, h));
    EXPECT_TRUE(de__slot_ref_asset(s, h));
    u32 hr = 0, ar = 0;
    de__slot_refs(sl, &hr, &ar);
    EXPECT_EQ(hr, 2u);
    EXPECT_EQ(ar, 1u);
    EXPECT_EQ(de__slot_ref_total(sl), 3u);

    EXPECT_FALSE(de__slot_unref_handle(s, h));
    EXPECT_FALSE(de__slot_unref_handle(s, h)); /* the asset ref is left */
    EXPECT_EQ(de__slot_eviction_drain(s, drain_cb, &(drain_log){0}), 0u);
    EXPECT_TRUE(de__slot_unref_asset(s, h)); /* the last one */
    EXPECT_EQ(de__slot_ref_total(sl), 0u);

    /* A second zero crossing before the drain does not queue the slot twice. */
    EXPECT_TRUE(de__slot_ref_handle(s, h));
    EXPECT_TRUE(de__slot_unref_handle(s, h));
    drain_log log = {0};
    EXPECT_EQ(de__slot_eviction_drain(s, drain_cb, &log), 1u);
    EXPECT_EQ(log.count, 1u);
    EXPECT_EQ(log.handles[0], h);
    EXPECT_EQ(de__slot_eviction_drain(s, drain_cb, &log), 0u); /* the list is empty now */

    /* After a drain the slot can be queued again. Several slots, freed from the callback. */
    u32 h2 = create_slot(s, 2);
    EXPECT_TRUE(de__slot_ref_handle(s, h));
    EXPECT_TRUE(de__slot_ref_handle(s, h2));
    EXPECT_TRUE(de__slot_unref_handle(s, h));
    EXPECT_TRUE(de__slot_unref_handle(s, h2));
    drain_log log2 = {.free_them = true};
    EXPECT_EQ(de__slot_eviction_drain(s, drain_cb, &log2), 2u);
    EXPECT_EQ(log2.handles[0], h2); /* most recently queued first */
    EXPECT_EQ(log2.handles[1], h);
    EXPECT_EQ(de__slot_live_count(s), 0u);
    EXPECT_EQ(de__slot_eviction_drain(NULL, drain_cb, &log2), 0u);
}

enum { REF_THREADS = 8, REF_ITERS = 100000, REF_SLOTS = 4 };

typedef struct ref_worker {
    de_asset_system *s;
    const u32 *handles;
    u32 seed;
    de_atomic_u32 *done; /* incremented when the worker finishes */
} ref_worker;

/* Balanced ref/unref of handle and asset refs over several slots, in a per-thread order. */
static i32 ref_worker_main(void *user) {
    ref_worker *w = (ref_worker *)user;
    u32 x = w->seed;
    for (u32 i = 0; i < REF_ITERS; ++i) {
        x = x * 1664525u + 1013904223u;
        u32 h = w->handles[(x >> 16) % REF_SLOTS];
        if ((x >> 8) & 1u) {
            de__slot_ref_handle(w->s, h);
            de__slot_ref_asset(w->s, h);
            de__slot_unref_handle(w->s, h);
            de__slot_unref_asset(w->s, h);
        } else {
            de__slot_ref_asset(w->s, h);
            de__slot_unref_asset(w->s, h);
        }
    }
    de_atomic_fetch_add_u32(w->done, 1, DE_ATOMIC_RELEASE);
    return 0;
}

/* Runs the workers. With `drained_per_slot`, the calling thread drains the candidate list while
 * they run, and counts the calls per slot. */
static void run_ref_threads(de_asset_system *s, const u32 *handles, u32 *drained_per_slot) {
    ref_worker w[REF_THREADS];
    de_thread *t[REF_THREADS];
    de_atomic_u32 done;
    de_atomic_init_u32(&done, 0);
    for (u32 i = 0; i < REF_THREADS; ++i) {
        w[i] = (ref_worker){.s = s, .handles = handles, .seed = 0x1234u + i * 7919u, .done = &done};
        ASSERT_EQ(de_thread_create(ref_worker_main, &w[i], "asset-ref", DE_THREAD_NORMAL, 0, &t[i]),
                  DE_OK);
    }
    if (drained_per_slot != NULL) {
        while (de_atomic_load_u32(&done, DE_ATOMIC_ACQUIRE) < REF_THREADS) {
            drain_log log = {0};
            de__slot_eviction_drain(s, drain_cb, &log);
            for (u32 i = 0; i < log.count && i < 8; ++i)
                for (u32 k = 0; k < REF_SLOTS; ++k)
                    if (log.handles[i] == handles[k])
                        drained_per_slot[k]++;
        }
    }
    for (u32 i = 0; i < REF_THREADS; ++i)
        EXPECT_EQ(de_thread_join(t[i]), 0);
}

TEST_F(slots, refcount_concurrent, sys_fx) {
    de_asset_system *s = f->s;
    u32 handles[REF_SLOTS];
    for (u32 i = 0; i < REF_SLOTS; ++i)
        handles[i] = create_slot(s, 100 + i);

    /* 1. Starting from zero refs, the counts cross zero many times: each slot still ends at
     * zero and is on the candidate list exactly once. */
    run_ref_threads(s, handles, NULL);
    for (u32 i = 0; i < REF_SLOTS; ++i)
        EXPECT_EQ(de__slot_ref_total(de__slot_get(s, handles[i])), 0u);
    drain_log log = {0};
    EXPECT_EQ(de__slot_eviction_drain(s, drain_cb, &log), (u32)REF_SLOTS);
    u32 seen = 0;
    for (u32 i = 0; i < log.count && i < 8; ++i)
        for (u32 k = 0; k < REF_SLOTS; ++k)
            if (log.handles[i] == handles[k])
                seen |= 1u << k;
    EXPECT_EQ(seen, (1u << REF_SLOTS) - 1u);

    /* 2. With the main thread holding a ref, the workers never reach zero; the main thread's
     * unref is the one zero crossing and queues each slot once. */
    for (u32 i = 0; i < REF_SLOTS; ++i)
        EXPECT_TRUE(de__slot_ref_handle(s, handles[i]));
    run_ref_threads(s, handles, NULL);
    EXPECT_EQ(de__slot_eviction_drain(s, drain_cb, &(drain_log){0}), 0u);
    for (u32 i = 0; i < REF_SLOTS; ++i) {
        EXPECT_EQ(de__slot_ref_total(de__slot_get(s, handles[i])), 1u);
        EXPECT_TRUE(de__slot_unref_handle(s, handles[i]));
    }
    drain_log log2 = {0};
    EXPECT_EQ(de__slot_eviction_drain(s, drain_cb, &log2), (u32)REF_SLOTS);

    /* 3. Draining while the workers push: the list never loses or duplicates a slot. Every slot
     * reaches zero at some point and is delivered; once the workers are done and the list is
     * drained, it stays empty. */
    u32 drained[REF_SLOTS] = {0};
    run_ref_threads(s, handles, drained);
    drain_log last = {0};
    de__slot_eviction_drain(s, drain_cb, &last);
    EXPECT_LE(last.count, (u32)REF_SLOTS); /* each slot at most once in one drain */
    for (u32 i = 0; i < last.count && i < 8; ++i)
        for (u32 k = 0; k < REF_SLOTS; ++k)
            if (last.handles[i] == handles[k])
                drained[k]++;
    for (u32 k = 0; k < REF_SLOTS; ++k) {
        EXPECT_GE(drained[k], 1u);
        EXPECT_EQ(de__slot_ref_total(de__slot_get(s, handles[k])), 0u);
    }
    EXPECT_EQ(de__slot_eviction_drain(s, drain_cb, &(drain_log){0}), 0u);
}
