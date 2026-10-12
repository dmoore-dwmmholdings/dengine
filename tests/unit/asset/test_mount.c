/*
 * test_mount.c: the pack mount stack and the pack read path of the load pipeline (DENG-212,
 * SPEC §11.7): priority override, unmount (also with reads in flight), PACKED-mode bundles from
 * the pack bundle tables, SOURCE-mode override, merged chunk-run reads, dependencies from the TOC,
 * and a 100-asset byte-exact roundtrip through the real pipeline (I/O threads, frame workers).
 *
 * Packs are written with the DENG-211 writer into a scratch directory of the build tree, mounted
 * as mnt:/. Every test ends with no live allocation of the asset system (tracking allocator).
 */
#include "de_test.h"
#include "pack_test_util.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_task.h>

#include "asset/asset_internal.h"
#include "asset/budget_internal.h"
#include "asset/dpak/dpak_reader.h"
#include "asset/manifest_internal.h"
#include "asset/mount_internal.h"
#include "asset/pipeline_internal.h"
#include "asset/resolve_internal.h"
#include "asset/slots_internal.h"
#include "dpak_writer.h"
#include "platform/de_atomic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test hook of the I/O system (src/platform/async_io.c): called on the I/O thread after each
 * chunk of a read. */
void de__io_test_set_chunk_hook(void (*fn)(de_io_request *req, u64 done));

#define WAIT_NS ((u64)60 * 1000000000u)

/* Waits block in the OS (a timed wait on a semaphore nobody posts), never spin: see
 * test_pipeline.c and "Decision: Asset loads never wait on below-normal threads". */
static de_sem g_nap;

static void nap_ns(u64 ns) {
    (void)de_sem_wait_timeout(&g_nap, ns);
}

/* ---- The pack test types ---------------------------------------------------------------------
 * pk_big, pk_noise, pk_rec: the object is a pk_obj followed by a copy of the bytes. pk_imp has an
 * importer that always fails, so a READY pk_imp asset proves the pack served it, not the cooker.
 */

typedef struct pk_obj {
    u64 size;
    i64 level; /* the manifest setting `level`, or -1 */
} pk_obj;

typedef struct pk_counters {
    const de_allocator *alloc;
    de_atomic_u32 loads, unloads, imports;
} pk_counters;

static de_result pk_load(de_asset_load_ctx *ctx) {
    pk_counters *c = (pk_counters *)de_asset_load_ctx_user(ctx);
    u64 n = 0;
    const void *p = de_asset_load_ctx_data(ctx, &n);
    pk_obj *o = (pk_obj *)DE_ALLOC(c->alloc, sizeof(pk_obj) + (size_t)n, 16, DE_MEM_ASSET);
    if (o == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "pk: out of memory");
    o->size = n;
    o->level = de_asset_load_ctx_settings_int(ctx, "level", -1);
    if (n != 0)
        memcpy(o + 1, p, (size_t)n);
    de_asset_load_ctx_set_object(ctx, o, sizeof(pk_obj) + n);
    (void)de_atomic_fetch_add_u32(&c->loads, 1, DE_ATOMIC_ACQ_REL);
    return DE_OK;
}

static void pk_unload(void *object, void *user) {
    pk_counters *c = (pk_counters *)user;
    pk_obj *o = (pk_obj *)object;
    DE_FREE(c->alloc, o, sizeof(pk_obj) + (size_t)o->size);
    (void)de_atomic_fetch_add_u32(&c->unloads, 1, DE_ATOMIC_ACQ_REL);
}

static de_result pk_import(de_asset_import_ctx *ctx) {
#if DE_HAS_COOKER
    return de_asset_import_fail(ctx, DE_ERR_PARSE, "the pk_imp importer refuses");
#else
    (void)ctx;
    return DE_OK;
#endif
}

/* ---- Fixture ------------------------------------------------------------------------------- */

typedef struct mnt_fx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_task_system *tasks;
    de_asset_system *s;
    pk_counters c;
} mnt_fx;

static void mount_once(void) {
    static bool mounted;
    if (mounted)
        return;
    ASSERT_EQ(de_sem_init(&g_nap, 0), DE_OK); /* never posted, never destroyed */
    ASSERT_EQ(de_fs_mount("mnt", DE_MOUNT_TEST_TMP_DIR, DE_MOUNT_WRITABLE, 0), DE_OK);
    mounted = true;
}

static void register_types(mnt_fx *f) {
    static const char *const k_names[] = {"pk_big", "pk_noise", "pk_rec", "pk_imp"};
    for (u32 i = 0; i < 4; ++i) {
        de_asset_type_desc d = {0};
        d.name = k_names[i];
        d.load = pk_load;
        d.unload = pk_unload;
        d.user = &f->c;
        if (i == 3)
            d.import = pk_import;
        ASSERT_TRUE(DE_HANDLE_VALID(de_asset_type_register(f->s, &d)));
    }
}

/* (Re)creates the asset system in `mode`, with the fixture's task system. */
static void fx_system(mnt_fx *f, de_asset_mode mode) {
    if (f->s != NULL)
        de_asset_system_destroy(f->s);
    f->s = NULL;
    de_asset_system_desc d = {0};
    d.allocator = &f->alloc;
    d.tasks = f->tasks;
    d.io_threads = 2;
    d.mode = mode;
    ASSERT_EQ(de_asset_system_create(&d, &f->s), DE_OK);
    register_types(f);
}

static void mnt_fx_setup(mnt_fx *f) {
    mount_once();
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_ASSET);
    f->c.alloc = &f->alloc;
    de_atomic_init_u32(&f->c.loads, 0);
    de_atomic_init_u32(&f->c.unloads, 0);
    de_atomic_init_u32(&f->c.imports, 0);
    de_fp_env_init_thread();
    de_task_system_desc td = {.worker_count = 4, .background_worker_count = 2};
    ASSERT_EQ(de_task_system_create(&td, &f->tasks), DE_OK);
    fx_system(f, DE_ASSET_MODE_PACKED);
}

static de_atomic_u32 g_gate_closed, g_gate_reached;

static void mnt_fx_teardown(mnt_fx *f) {
    de__io_test_set_chunk_hook(NULL);
    de_atomic_store_u32(&g_gate_closed, 0, DE_ATOMIC_RELEASE); /* a failed test cannot hang */
    de_asset_system_destroy(f->s);
    f->s = NULL;
    de_task_system_destroy(f->tasks);
    f->tasks = NULL;
    /* Every pack, job, buffer, decoder, object and request is gone. */
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).bytes_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0u);
}

/* ---- Helpers -------------------------------------------------------------------------------- */

static de_asset_id id_of(const char *path) {
    return de_asset_id_from(path);
}

/* Updates until `h` is final (blocking in the OS between updates). */
static de_load_state wait_load(mnt_fx *f, de_load_handle h) {
    u64 end = de_time_now_ns() + WAIT_NS;
    de_asset_system_update(f->s, NULL);
    de_load_state st = de_asset_status(f->s, h, NULL);
    while ((st == DE_LOAD_PENDING || st == DE_LOAD_LOADING) && de_time_now_ns() < end) {
        nap_ns(100000u);
        de_asset_system_update(f->s, NULL);
        st = de_asset_status(f->s, h, NULL);
    }
    return st;
}

/* Updates until no pipeline job is left. */
static void drain(mnt_fx *f) {
    u64 end = de_time_now_ns() + WAIT_NS;
    de__pipeline_stats st;
    de__pipeline_get_stats(f->s, &st);
    while (st.live_jobs != 0 && de_time_now_ns() < end) {
        de_asset_system_update(f->s, NULL);
        nap_ns(100000u);
        de__pipeline_get_stats(f->s, &st);
    }
    EXPECT_EQ(st.live_jobs, 0u);
}

static const pk_obj *obj_of(mnt_fx *f, const char *path) {
    u32 h = de__slot_find(f->s, id_of(path));
    return h != 0 ? (const pk_obj *)de__asset_object(f->s, h) : NULL;
}

/* The asset's object holds exactly `size` bytes equal to `data`. */
static void expect_bytes(mnt_fx *f, const char *path, const void *data, size_t size) {
    const pk_obj *o = obj_of(f, path);
    ASSERT_TRUE(o != NULL);
    ASSERT_EQ(o->size, (u64)size);
    if (size != 0)
        EXPECT_MEMEQ(o + 1, data, size);
}

/* Loads one asset and waits; returns the state. The request is released. */
static de_load_state load_one(mnt_fx *f, const char *path) {
    de_load_handle h = de_asset_load(f->s, id_of(path), 0);
    de_load_state st = wait_load(f, h);
    de_asset_release(f->s, h);
    return st;
}

/* Releases nothing new, but evicts every unreferenced asset, so the next load reads again. */
static void evict_all(mnt_fx *f) {
    de_asset_flush_unused(f->s);
    de_asset_system_update(f->s, NULL);
}

static de__mount_stats mstats(mnt_fx *f) {
    de__mount_stats st;
    de__mount_get_stats(f->s, &st);
    return st;
}

/* A one-asset pack at `vpath`. */
static void one_asset_pack(const char *vpath, const char *path, const char *type, const void *data,
                           size_t size, u32 platform) {
    pt_asset a;
    memset(&a, 0, sizeof a);
    snprintf(a.path, sizeof a.path, "%s", path);
    a.type = type;
    a.data = (u8 *)data;
    a.size = size;
    dpak_writer_desc wd = {.zstd_level = 3, .platform = platform};
    pt_pack_file(&wd, &a, 1, vpath);
}

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

/* Parses `text` as mnt:/assets.toml and makes it the active manifest. */
static void install_manifest(de_asset_system *s, const char *text) {
    g_text = text;
    de__manifest *m = NULL;
    de__manifest_parse_desc d = {
        .path = "mnt:/assets.toml", .allocator = s->alloc, .read = text_read};
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

/* I/O gate: an I/O thread that finishes a chunk of a read waits while the gate is closed. */
static void gate_hook(de_io_request *req, u64 done) {
    (void)req;
    (void)done;
    if (de_atomic_load_u32(&g_gate_closed, DE_ATOMIC_ACQUIRE) == 0)
        return;
    de_atomic_store_u32(&g_gate_reached, 1, DE_ATOMIC_RELEASE);
    while (de_atomic_load_u32(&g_gate_closed, DE_ATOMIC_ACQUIRE) != 0)
        nap_ns(200000u);
}

static void wait_gate(mnt_fx *f) {
    u64 end = de_time_now_ns() + WAIT_NS;
    while (de_atomic_load_u32(&g_gate_reached, DE_ATOMIC_ACQUIRE) == 0 && de_time_now_ns() < end) {
        de_asset_system_update(f->s, NULL);
        nap_ns(100000u);
    }
    ASSERT_EQ(de_atomic_load_u32(&g_gate_reached, DE_ATOMIC_ACQUIRE), 1u);
}

/* ---- mount.roundtrip_100 --------------------------------------------------------------------- */

TEST_F(mount, roundtrip_100, mnt_fx) {
    static pt_asset a[100];
    pt_make_100(a);
    /* A stripped pack (no names): the asset system needs only IDs and types. */
    dpak_writer_desc wd = {.zstd_level = 3, .use_dictionaries = true};
    pt_pack_file(&wd, a, 100, "mnt:/r100.dpak");
    ASSERT_EQ(de_asset_pack_mount(f->s, "mnt:/r100.dpak", 0), DE_OK);

    de_asset_id ids[100];
    for (u32 i = 0; i < 100; ++i)
        ids[i] = id_of(a[i].path);
    de_load_handle h = de_asset_load_many(f->s, ids, 100, 0);
    EXPECT_EQ((int)wait_load(f, h), (int)DE_LOAD_READY);
    for (u32 i = 0; i < 100; ++i) {
        expect_bytes(f, a[i].path, a[i].data, a[i].size);
        const de__slot *sl = de__slot_get(f->s, de__slot_find(f->s, ids[i]));
        ASSERT_TRUE(sl != NULL);
        EXPECT_EQ(sl->dep_count, a[i].dep_count); /* the TOC's dependencies */
        EXPECT_EQ(sl->type_hash, de_hash64_str(a[i].type));
    }
    dpak_reader *r = NULL;
    ASSERT_EQ(dpak_open("mnt:/r100.dpak", NULL, &r), DE_OK);
    u32 chunks = dpak_get_header(r)->sections[DPAK_SECTION_CHUNKS].count;
    dpak_close(r);
    de__mount_stats ms = mstats(f);
    EXPECT_EQ(ms.jobs, 100u);
    EXPECT_EQ(ms.chunks, (u64)chunks);
    EXPECT_EQ(ms.mounted, 1u);
    de_asset_release(f->s, h);
    pt_asset_free(a, 100);
}

/* ---- mount.priority_override --------------------------------------------------------------- */

TEST_F(mount, priority_override, mnt_fx) {
    u8 *base_x = pt_make_bytes(PT_TEXT, 1, 5000);
    u8 *patch_x = pt_make_bytes(PT_TEXT, 2, 7000);
    u8 *tie_x = pt_make_bytes(PT_TEXT, 3, 300);
    u8 *only = pt_make_bytes(PT_NOISE, 4, 2000);
    pt_asset base[2];
    memset(base, 0, sizeof base);
    snprintf(base[0].path, sizeof base[0].path, "shared/x");
    base[0].type = "pk_big";
    base[0].data = base_x;
    base[0].size = 5000;
    snprintf(base[1].path, sizeof base[1].path, "base/only");
    base[1].type = "pk_noise";
    base[1].data = only;
    base[1].size = 2000;
    pt_pack_file(&(dpak_writer_desc){.zstd_level = 3}, base, 2, "mnt:/base.dpak");
    one_asset_pack("mnt:/patch.dpak", "shared/x", "pk_big", patch_x, 7000, DPAK_PLATFORM_ANY);
    one_asset_pack("mnt:/tie.dpak", "shared/x", "pk_big", tie_x, 300, DPAK_PLATFORM_ANY);

    /* The patch is mounted first, at a higher priority: mount order does not matter. */
    ASSERT_EQ(de_asset_pack_mount(f->s, "mnt:/patch.dpak", 10), DE_OK);
    ASSERT_EQ(de_asset_pack_mount(f->s, "mnt:/base.dpak", 0), DE_OK);
    EXPECT_EQ((int)load_one(f, "shared/x"), (int)DE_LOAD_READY);
    expect_bytes(f, "shared/x", patch_x, 7000);
    EXPECT_EQ((int)load_one(f, "base/only"), (int)DE_LOAD_READY);
    expect_bytes(f, "base/only", only, 2000);
    de__mount_entry e;
    ASSERT_TRUE(de__mount_lookup(f->s, id_of("shared/x"), &e));
    EXPECT_STREQ(e.pack->vpath, "mnt:/patch.dpak");

    /* Equal priority: the pack mounted later wins. */
    ASSERT_EQ(de_asset_pack_mount(f->s, "mnt:/tie.dpak", 10), DE_OK);
    ASSERT_TRUE(de__mount_lookup(f->s, id_of("shared/x"), &e));
    EXPECT_STREQ(e.pack->vpath, "mnt:/tie.dpak");
    evict_all(f);
    EXPECT_EQ((int)load_one(f, "shared/x"), (int)DE_LOAD_READY);
    expect_bytes(f, "shared/x", tie_x, 300);
    EXPECT_EQ(mstats(f).mounted, 3u);

    free(base_x);
    free(patch_x);
    free(tie_x);
    free(only);
}

/* ---- mount.unmount_fallback ---------------------------------------------------------------- */

TEST_F(mount, unmount_fallback, mnt_fx) {
    u8 *base_x = pt_make_bytes(PT_TEXT, 11, 4000);
    u8 *patch_x = pt_make_bytes(PT_TEXT, 12, 6000);
    one_asset_pack("mnt:/ub.dpak", "shared/x", "pk_big", base_x, 4000, DPAK_PLATFORM_ANY);
    one_asset_pack("mnt:/up.dpak", "shared/x", "pk_big", patch_x, 6000, DPAK_PLATFORM_ANY);
    ASSERT_EQ(de_asset_pack_mount(f->s, "mnt:/ub.dpak", 0), DE_OK);
    ASSERT_EQ(de_asset_pack_mount(f->s, "mnt:/up.dpak", 1), DE_OK);

    de_load_handle h = de_asset_load(f->s, id_of("shared/x"), 0);
    EXPECT_EQ((int)wait_load(f, h), (int)DE_LOAD_READY);
    expect_bytes(f, "shared/x", patch_x, 6000);

    /* Unmounted: the loaded asset keeps its data; nothing reads the pack, so it closes now. */
    de_asset_pack_unmount(f->s, "mnt:/up.dpak");
    EXPECT_EQ(mstats(f).mounted, 1u);
    EXPECT_EQ(mstats(f).retired, 0u);
    expect_bytes(f, "shared/x", patch_x, 6000);
    de_asset_release(f->s, h);

    /* A new load of the ID resolves to the next pack. */
    evict_all(f);
    EXPECT_TRUE(obj_of(f, "shared/x") == NULL);
    EXPECT_EQ((int)load_one(f, "shared/x"), (int)DE_LOAD_READY);
    expect_bytes(f, "shared/x", base_x, 4000);

    /* With no pack left, PACKED mode has nowhere to load it from. */
    de_asset_pack_unmount(f->s, "mnt:/ub.dpak");
    de_asset_pack_unmount(f->s, "mnt:/ub.dpak"); /* not mounted: a warning, nothing else */
    evict_all(f);
    h = de_asset_load(f->s, id_of("shared/x"), 0);
    de_load_info info;
    EXPECT_EQ((int)wait_load(f, h), (int)DE_LOAD_FAILED);
    EXPECT_EQ((int)de_asset_status(f->s, h, &info), (int)DE_LOAD_FAILED);
    EXPECT_EQ((int)info.first_error, (int)DE_ERR_NOT_FOUND);
    EXPECT_TRUE(strstr(info.first_error_msg, "no mounted pack") != NULL);
    de_asset_release(f->s, h);
    EXPECT_EQ(mstats(f).mounted, 0u);

    free(base_x);
    free(patch_x);
}

/* ---- mount.unmount_in_flight ----------------------------------------------------------------- */

TEST_F(mount, unmount_in_flight, mnt_fx) {
    const size_t n = (size_t)3 << 20;
    u8 *base_x = pt_make_bytes(PT_NOISE, 21, n);
    u8 *patch_x = pt_make_bytes(PT_NOISE, 22, n);
    one_asset_pack("mnt:/fb.dpak", "shared/big", "pk_noise", base_x, n, DPAK_PLATFORM_ANY);
    one_asset_pack("mnt:/fp.dpak", "shared/big", "pk_noise", patch_x, n, DPAK_PLATFORM_ANY);
    ASSERT_EQ(de_asset_pack_mount(f->s, "mnt:/fb.dpak", 0), DE_OK);
    ASSERT_EQ(de_asset_pack_mount(f->s, "mnt:/fp.dpak", 1), DE_OK);

    /* Hold the read of the patch pack on its I/O thread, then unmount the pack under it. */
    de_atomic_store_u32(&g_gate_reached, 0, DE_ATOMIC_RELEASE);
    de_atomic_store_u32(&g_gate_closed, 1, DE_ATOMIC_RELEASE);
    de__io_test_set_chunk_hook(gate_hook);
    de_load_handle h = de_asset_load(f->s, id_of("shared/big"), 0);
    wait_gate(f);
    de_asset_pack_unmount(f->s, "mnt:/fp.dpak");
    EXPECT_EQ(mstats(f).mounted, 1u);
    EXPECT_EQ(mstats(f).retired, 1u); /* deferred: the read still uses it */
    de__mount_entry e;
    ASSERT_TRUE(de__mount_lookup(f->s, id_of("shared/big"), &e));
    EXPECT_STREQ(e.pack->vpath, "mnt:/fb.dpak");

    de_atomic_store_u32(&g_gate_closed, 0, DE_ATOMIC_RELEASE);
    EXPECT_EQ((int)wait_load(f, h), (int)DE_LOAD_READY);
    expect_bytes(f, "shared/big", patch_x, n); /* the read that was in flight finished */
    drain(f);
    EXPECT_EQ(mstats(f).retired, 0u); /* closed with its last job */
    de_asset_release(f->s, h);

    evict_all(f);
    EXPECT_EQ((int)load_one(f, "shared/big"), (int)DE_LOAD_READY);
    expect_bytes(f, "shared/big", base_x, n);
    free(base_x);
    free(patch_x);
}

/* ---- mount.cancel_in_flight ------------------------------------------------------------------ */

TEST_F(mount, cancel_in_flight, mnt_fx) {
    const size_t n = (size_t)9 << 20; /* three runs */
    u8 *x = pt_make_bytes(PT_NOISE, 31, n);
    one_asset_pack("mnt:/c.dpak", "c/big", "pk_noise", x, n, DPAK_PLATFORM_ANY);
    ASSERT_EQ(de_asset_pack_mount(f->s, "mnt:/c.dpak", 0), DE_OK);
    de_atomic_store_u32(&g_gate_reached, 0, DE_ATOMIC_RELEASE);
    de_atomic_store_u32(&g_gate_closed, 1, DE_ATOMIC_RELEASE);
    de__io_test_set_chunk_hook(gate_hook);
    de_load_handle h = de_asset_load(f->s, id_of("c/big"), 0);
    wait_gate(f);
    de_asset_release(f->s, h); /* cancels: every run is cancelled */
    de_atomic_store_u32(&g_gate_closed, 0, DE_ATOMIC_RELEASE);
    drain(f);
    EXPECT_EQ(de_atomic_load_u32(&f->c.loads, DE_ATOMIC_ACQUIRE), 0u);
    de__pipeline_stats ps;
    de__pipeline_get_stats(f->s, &ps);
    EXPECT_GE(ps.io_cancelled, 1u);
    EXPECT_EQ(ps.discarded, 1u);

    /* The pack is not held by the cancelled job any more, and a new load works. */
    EXPECT_EQ((int)load_one(f, "c/big"), (int)DE_LOAD_READY);
    expect_bytes(f, "c/big", x, n);
    de_asset_pack_unmount(f->s, "mnt:/c.dpak");
    EXPECT_EQ(mstats(f).retired, 0u);
    free(x);
}

/* ---- mount.packed_bundle_table -------------------------------------------------------------- */

TEST_F(mount, packed_bundle_table, mnt_fx) {
    static pt_asset a[100];
    pt_make_100(a);
    /* Base pack: assets 0..5; bundles common = {3} (resident), level_01 = {4, 5} + common,
     * cyc_a <-> cyc_b (a cycle). Patch pack: level_01 = {0} + common. */
    dpak_writer *w = pt_writer(&(dpak_writer_desc){.zstd_level = 3}, a, 6);
    u64 common[] = {id_of(a[3].path).hash};
    u64 level[] = {id_of(a[4].path).hash, id_of(a[5].path).hash};
    u64 ca[] = {id_of(a[1].path).hash}, cb[] = {id_of(a[2].path).hash};
    const char *const dep_common[] = {"common"};
    const char *const dep_a[] = {"cyc_a"}, *const dep_b[] = {"cyc_b"};
    ASSERT_EQ(dpak_writer_add_bundle(w, "common", common, 1, NULL, 0, true, DPAK_PRIORITY_NORMAL),
              DE_OK);
    ASSERT_EQ(
        dpak_writer_add_bundle(w, "level_01", level, 2, dep_common, 1, false, DPAK_PRIORITY_HIGH),
        DE_OK);
    ASSERT_EQ(dpak_writer_add_bundle(w, "cyc_a", ca, 1, dep_b, 1, false, DPAK_PRIORITY_NORMAL),
              DE_OK);
    ASSERT_EQ(dpak_writer_add_bundle(w, "cyc_b", cb, 1, dep_a, 1, false, DPAK_PRIORITY_NORMAL),
              DE_OK);
    const char *const dep_gone[] = {"no_such_bundle"};
    ASSERT_EQ(
        dpak_writer_add_bundle(w, "dangling", cb, 1, dep_gone, 1, false, DPAK_PRIORITY_NORMAL),
        DE_OK);
    ASSERT_EQ(dpak_writer_finish(w, "mnt:/bb.dpak"), DE_OK);
    dpak_writer_destroy(w);
    ASSERT_EQ(de_asset_pack_mount(f->s, "mnt:/bb.dpak", 0), DE_OK);
    EXPECT_TRUE(f->s->manifest == NULL); /* PACKED mode: no assets.toml */

    de_load_handle h = de_asset_load_bundle(f->s, "level_01", 0);
    EXPECT_EQ((int)wait_load(f, h), (int)DE_LOAD_READY);
    de_asset_status_entry list[8];
    ASSERT_EQ(de_asset_status_list(f->s, h, list, 8), 3u);
    EXPECT_EQ(list[0].id.hash, common[0]); /* the depended-on bundle first */
    /* Then the bundle's own members, in the pack's (sorted) order. */
    EXPECT_TRUE((list[1].id.hash == level[0] && list[2].id.hash == level[1]) ||
                (list[1].id.hash == level[1] && list[2].id.hash == level[0]));
    for (u32 i = 3; i <= 5; ++i)
        expect_bytes(f, a[i].path, a[i].data, a[i].size);
    EXPECT_TRUE(de__budget_is_resident(f->s, de__slot_find(f->s, id_of(a[3].path))));
    EXPECT_FALSE(de__budget_is_resident(f->s, de__slot_find(f->s, id_of(a[4].path))));
    de_asset_release(f->s, h);

    /* A cycle of depends ends; a depend in no pack is skipped. */
    h = de_asset_load_bundle(f->s, "cyc_a", 0);
    EXPECT_EQ((int)wait_load(f, h), (int)DE_LOAD_READY);
    EXPECT_EQ(de_asset_status_list(f->s, h, NULL, 0), 2u);
    de_asset_release(f->s, h);
    h = de_asset_load_bundle(f->s, "dangling", 0);
    EXPECT_EQ((int)wait_load(f, h), (int)DE_LOAD_READY);
    EXPECT_EQ(de_asset_status_list(f->s, h, NULL, 0), 1u);
    de_asset_release(f->s, h);

    /* A higher pack's bundle of the same name replaces the whole definition. */
    w = pt_writer(&(dpak_writer_desc){.zstd_level = 3}, a, 1);
    u64 patched[] = {id_of(a[0].path).hash};
    ASSERT_EQ(
        dpak_writer_add_bundle(w, "level_01", patched, 1, dep_common, 1, false, DPAK_PRIORITY_LOW),
        DE_OK);
    ASSERT_EQ(dpak_writer_finish(w, "mnt:/bp.dpak"), DE_OK);
    dpak_writer_destroy(w);
    ASSERT_EQ(de_asset_pack_mount(f->s, "mnt:/bp.dpak", 5), DE_OK);
    h = de_asset_load_bundle(f->s, "level_01", 0);
    EXPECT_EQ((int)wait_load(f, h), (int)DE_LOAD_READY);
    ASSERT_EQ(de_asset_status_list(f->s, h, list, 8), 2u);
    EXPECT_EQ(list[0].id.hash, common[0]); /* "common" still comes from the base pack */
    EXPECT_EQ(list[1].id.hash, patched[0]);
    expect_bytes(f, a[0].path, a[0].data, a[0].size);
    de_asset_release(f->s, h);

    /* A bundle no pack (and no manifest) defines. */
    h = de_asset_load_bundle(f->s, "nope", 0);
    de_load_info info;
    EXPECT_EQ((int)de_asset_status(f->s, h, &info), (int)DE_LOAD_FAILED);
    EXPECT_EQ((int)info.first_error, (int)DE_ERR_NOT_FOUND);
    EXPECT_STREQ(info.first_error_msg,
                 "bundle \"nope\" is not in the asset manifest or a mounted pack");
    de_asset_release(f->s, h);
    pt_asset_free(a, 100);
}

/* ---- mount.source_mode_override ------------------------------------------------------------- */

TEST_F(mount, source_mode_override, mnt_fx) {
    fx_system(f, DE_ASSET_MODE_SOURCE);
    ASSERT_EQ(de_file_write_all_atomic("mnt:/x.pk", "SOURCE-X", 8), DE_OK);
    ASSERT_EQ(de_file_write_all_atomic("mnt:/y.pk", "SOURCE-Y", 8), DE_OK);
    ASSERT_EQ(de_file_write_all_atomic("mnt:/imp.pk", "SOURCE-IMP", 10), DE_OK);
    install_manifest(f->s, "[assets]\n"
                           "\"a/x\" = { type = \"pk_big\", source = \"x.pk\", level = 7 }\n"
                           "\"a/y\" = { type = \"pk_big\", source = \"y.pk\" }\n"
                           "\"a/imp\" = { type = \"pk_imp\", source = \"imp.pk\" }\n"
                           "\"a/typo\" = { type = \"pk_big\", source = \"y.pk\" }\n");
    /* Without a pack, the sources load (and the importer type cooks, and fails). */
    EXPECT_EQ((int)load_one(f, "a/x"), (int)DE_LOAD_READY);
    expect_bytes(f, "a/x", "SOURCE-X", 8);

    pt_asset p[3];
    memset(p, 0, sizeof p);
    snprintf(p[0].path, sizeof p[0].path, "a/x");
    p[0].type = "pk_big";
    p[0].data = (u8 *)"PACK-X";
    p[0].size = 6;
    snprintf(p[1].path, sizeof p[1].path, "a/imp");
    p[1].type = "pk_imp";
    p[1].data = (u8 *)"PACK-IMP";
    p[1].size = 8;
    snprintf(p[2].path, sizeof p[2].path, "a/typo");
    p[2].type = "pk_rec"; /* not the manifest's type */
    p[2].data = (u8 *)"PACK-TYPO";
    p[2].size = 9;
    pt_pack_file(&(dpak_writer_desc){.zstd_level = 3}, p, 3, "mnt:/src_patch.dpak");
    ASSERT_EQ(de_asset_pack_mount(f->s, "mnt:/src_patch.dpak", 0), DE_OK);
    evict_all(f);

    const de_asset_id ids[] = {id_of("a/x"), id_of("a/y"), id_of("a/imp")};
    de_load_handle h = de_asset_load_many(f->s, ids, 3, 0);
    EXPECT_EQ((int)wait_load(f, h), (int)DE_LOAD_READY);
    expect_bytes(f, "a/x", "PACK-X", 6);     /* the pack overrides the source */
    expect_bytes(f, "a/y", "SOURCE-Y", 8);   /* not in the pack: the source */
    expect_bytes(f, "a/imp", "PACK-IMP", 8); /* cooked bytes from the pack: no cook */
    EXPECT_EQ(obj_of(f, "a/x")->level, 7);   /* the manifest's settings still reach the loader */
    de_asset_release(f->s, h);

    /* The pack holds the ID as another type than the manifest says: refused. */
    de_load_handle t = de_asset_load(f->s, id_of("a/typo"), 0);
    de_load_info info;
    EXPECT_EQ((int)wait_load(f, t), (int)DE_LOAD_FAILED);
    EXPECT_EQ((int)de_asset_status(f->s, t, &info), (int)DE_LOAD_FAILED);
    EXPECT_EQ((int)info.first_error, (int)DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(strstr(info.first_error_msg, "another type") != NULL);
    de_asset_release(f->s, t);

    /* SOURCE mode: an ID only a pack has loads too (typed access finds it in the pack). */
    EXPECT_EQ(mstats(f).mounted, 1u);
}

/* ---- mount.merged_reads --------------------------------------------------------------------- */

/* The runs the read path makes for entry `e` (the same rule as pack_stage.c). */
static u32 expected_runs(const dpak_reader *r, const dpak_toc_entry *e) {
    u32 n = 0;
    u64 bytes = 0;
    for (u32 k = 0; k < e->chunk_count; ++k) {
        dpak_chunk_entry c;
        dpak_chunk(r, e->first_chunk + k, &c);
        if (n == 0 || bytes + c.stored_size > DE__PACK_RUN_MAX_BYTES) {
            n++;
            bytes = 0;
        }
        bytes += c.stored_size;
    }
    return n;
}

TEST_F(mount, merged_reads, mnt_fx) {
    const size_t n = (size_t)10 << 20;
    pt_asset a[2];
    memset(a, 0, sizeof a);
    snprintf(a[0].path, sizeof a[0].path, "big/ten_text");
    a[0].type = "pk_big";
    a[0].size = n;
    a[0].data = pt_make_bytes(PT_TEXT, 41, n);
    snprintf(a[1].path, sizeof a[1].path, "big/ten_noise");
    a[1].type = "pk_noise";
    a[1].size = n;
    a[1].data = pt_make_bytes(PT_NOISE, 42, n); /* every chunk raw: read straight to raw */
    pt_pack_file(&(dpak_writer_desc){.zstd_level = 1}, a, 2, "mnt:/ten.dpak");

    dpak_reader *r = NULL;
    ASSERT_EQ(dpak_open("mnt:/ten.dpak", NULL, &r), DE_OK);
    u32 want_runs = 0, chunks = 0;
    for (u32 i = 0; i < 2; ++i) {
        dpak_toc_entry e;
        dpak_entry(r, dpak_find(r, id_of(a[i].path).hash), &e);
        EXPECT_EQ(e.chunk_count, 160u);
        want_runs += expected_runs(r, &e);
        chunks += e.chunk_count;
    }
    dpak_close(r);
    EXPECT_LE(want_runs, 6u); /* <= 4 MiB per read, not one read per 64 KiB chunk */

    ASSERT_EQ(de_asset_pack_mount(f->s, "mnt:/ten.dpak", 0), DE_OK);
    const de_asset_id ids[] = {id_of(a[0].path), id_of(a[1].path)};
    de_load_handle h = de_asset_load_many(f->s, ids, 2, 0);
    EXPECT_EQ((int)wait_load(f, h), (int)DE_LOAD_READY);
    expect_bytes(f, a[0].path, a[0].data, n);
    expect_bytes(f, a[1].path, a[1].data, n);
    de__mount_stats ms = mstats(f);
    EXPECT_EQ(ms.runs, (u64)want_runs); /* the I/O request counter */
    EXPECT_EQ(ms.chunks, (u64)chunks);
    de__pipeline_stats ps;
    de__pipeline_get_stats(f->s, &ps);
    EXPECT_EQ(ps.reads, (u64)want_runs);
    de_load_info info;
    (void)de_asset_status(f->s, h, &info);
    EXPECT_EQ(info.bytes_done, info.bytes_total); /* byte progress over the stored sizes */
    EXPECT_GT(info.bytes_total, (u64)n);
    de_asset_release(f->s, h);
    pt_asset_free(a, 2);
}

/* ---- mount.mount_errors --------------------------------------------------------------------- */

TEST_F(mount, mount_errors, mnt_fx) {
    u8 *x = pt_make_bytes(PT_TEXT, 51, 1000);
    one_asset_pack("mnt:/e.dpak", "e/x", "pk_big", x, 1000, DPAK_PLATFORM_ANY);
    ASSERT_EQ(de_asset_pack_mount(f->s, "mnt:/e.dpak", 0), DE_OK);
    EXPECT_EQ(de_asset_pack_mount(f->s, "mnt:/e.dpak", 3), DE_ERR_ALREADY_EXISTS);
    EXPECT_EQ(de_asset_pack_mount(f->s, "mnt:/sub/../e.dpak", 3), DE_ERR_ALREADY_EXISTS);
    EXPECT_EQ(de_asset_pack_mount(f->s, NULL, 0), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_asset_pack_mount(NULL, "mnt:/e.dpak", 0), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_asset_pack_mount(f->s, "mnt:/missing.dpak", 0), DE_ERR_NOT_FOUND);

    u32 host = dpak_platform_host();
    u32 other = host == DPAK_PLATFORM_WINDOWS ? DPAK_PLATFORM_LINUX : DPAK_PLATFORM_WINDOWS;
    one_asset_pack("mnt:/other.dpak", "e/y", "pk_big", x, 1000, other);
    EXPECT_EQ(de_asset_pack_mount(f->s, "mnt:/other.dpak", 0), DE_ERR_UNSUPPORTED);

    void *data = NULL;
    u64 size = 0;
    ASSERT_EQ(de_file_read_all("mnt:/e.dpak", NULL, &data, &size), DE_OK);
    u8 *p = (u8 *)data;
    dpak_store_u32(p + 4, DPAK_VERSION + 1u);
    ASSERT_EQ(de_file_write_all_atomic("mnt:/v2.dpak", p, size), DE_OK);
    EXPECT_EQ(de_asset_pack_mount(f->s, "mnt:/v2.dpak", 0), DE_ERR_VERSION_MISMATCH);
    dpak_store_u32(p + 4, DPAK_VERSION);
    ASSERT_EQ(de_file_write_all_atomic("mnt:/cut.dpak", p, size - 10u), DE_OK);
    EXPECT_EQ(de_asset_pack_mount(f->s, "mnt:/cut.dpak", 0), DE_ERR_PARSE);
    DE_FREE(de_heap_allocator(), data, (size_t)size + 1u);

    /* The failures left the stack as it was. */
    EXPECT_EQ(mstats(f).mounted, 1u);
    EXPECT_EQ((int)load_one(f, "e/x"), (int)DE_LOAD_READY);
    expect_bytes(f, "e/x", x, 1000);
    de_asset_pack_unmount(f->s, "mnt:/nothing.dpak");
    de_asset_pack_unmount(NULL, "mnt:/e.dpak");
    free(x);
}

/* ---- mount.deps_from_toc, typed_access ------------------------------------------------------ */

TEST_F(mount, deps_from_toc, mnt_fx) {
    pt_asset a[3];
    memset(a, 0, sizeof a);
    snprintf(a[0].path, sizeof a[0].path, "p/parent");
    snprintf(a[1].path, sizeof a[1].path, "p/child");
    snprintf(a[2].path, sizeof a[2].path, "p/grandchild");
    for (u32 i = 0; i < 3; ++i) {
        a[i].type = "pk_big";
        a[i].size = 100u + i;
        a[i].data = pt_make_bytes(PT_TEXT, 60u + i, a[i].size);
    }
    a[0].deps[a[0].dep_count++] = id_of("p/child").hash;
    a[1].deps[a[1].dep_count++] = id_of("p/grandchild").hash;
    pt_pack_file(&(dpak_writer_desc){.zstd_level = 3}, a, 3, "mnt:/deps.dpak");
    ASSERT_EQ(de_asset_pack_mount(f->s, "mnt:/deps.dpak", 0), DE_OK);

    de_load_handle h = de_asset_load(f->s, id_of("p/parent"), 0);
    EXPECT_EQ((int)wait_load(f, h), (int)DE_LOAD_READY);
    EXPECT_EQ((int)de_asset_status_of(f->s, id_of("p/child")), (int)DE_LOAD_READY);
    EXPECT_EQ((int)de_asset_status_of(f->s, id_of("p/grandchild")), (int)DE_LOAD_READY);
    const de__slot *sl = de__slot_get(f->s, de__slot_find(f->s, id_of("p/parent")));
    ASSERT_TRUE(sl != NULL);
    ASSERT_EQ(sl->dep_count, 1u);
    EXPECT_EQ(sl->deps[0].hash, id_of("p/child").hash);
    const de__slot *cs = de__slot_get(f->s, de__slot_find(f->s, id_of("p/child")));
    u32 asset_refs = 0;
    de__slot_refs(cs, NULL, &asset_refs);
    EXPECT_EQ(asset_refs, 1u); /* held by its parent */
    de_asset_release(f->s, h);
    pt_asset_free(a, 3);
}

TEST_F(mount, typed_access, mnt_fx) {
    u8 *x = pt_make_bytes(PT_TEXT, 71, 64);
    one_asset_pack("mnt:/t.dpak", "t/x", "pk_big", x, 64, DPAK_PLATFORM_ANY);
    EXPECT_EQ(de__asset_handle_of(f->s, id_of("t/x"), NULL), 0u); /* not mounted yet */
    ASSERT_EQ(de_asset_pack_mount(f->s, "mnt:/t.dpak", 0), DE_OK);
    u32 hv = de__asset_handle_of(f->s, id_of("t/x"), "pk_big");
    EXPECT_NE(hv, 0u);
    EXPECT_EQ(de__asset_handle_of(f->s, id_of("t/x"), NULL), hv);
    EXPECT_FALSE(DE_HANDLE_VALID(de_asset_blob(f->s, id_of("t/x")))); /* another type */
    EXPECT_FALSE(DE_HANDLE_VALID(de_asset_blob(f->s, id_of("t/nope"))));
    free(x);
}
