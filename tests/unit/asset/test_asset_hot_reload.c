/*
 * test_asset_hot_reload.c: asset hot reload (DENG-214, SPEC §11.9): a source edit reloads the
 * asset behind its handle with one RELOADED event, dependents get dependency_reloaded, a broken
 * source keeps the old data and its error, extra sources reload their owner, a manifest edit
 * reloads only what changed, old objects are unloaded after reload_retire_frames, assets a pack
 * serves are skipped, and one test runs the real file watcher.
 *
 * The tests write their files into a scratch directory of the build tree, mounted as hr:/, one
 * subdirectory per test (CTest runs the tests as parallel processes). Except for `real_watcher`,
 * they turn the OS watcher off (de__asset_reload_set_watching) and call the watch-change entry
 * point de__asset_reload_on_change with the paths, so nothing depends on FSEvents latency. Events
 * are captured with the system's `emit_fn` (no ECS world), as the DENG-144 tests do.
 */
#include "de_test.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_math.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_task.h>

#include "asset/asset_internal.h"
#include "asset/hot_reload_internal.h"
#include "asset/load_internal.h"
#include "asset/slots_internal.h"
#include "platform/de_atomic.h"

#if DE_HR_WITH_PACK
#include "pack_test_util.h" /* the tools-only pack writer (DENG-211) */
#endif

#include <stdio.h>
#include <string.h>

#define WAIT_NS ((u64)30 * 1000000000u)

/* ---- The `hr` test type ------------------------------------------------------------------------
 *
 * The object is an hr_obj followed by a copy of the bytes. A payload that starts with "FAIL"
 * fails with DE_ERR_PARSE. The string-array setting `deps` declares dependencies. Its
 * dependency_reloaded counts calls on the object and globally.
 */

typedef struct hr_obj {
    u64 size;
    u32 dep_reloads; /* dependency_reloaded calls on this object */
    u32 pad_;
    i64 level; /* the `level` setting, -1 when missing */
} hr_obj;

typedef struct hr_counters {
    const de_allocator *alloc;
    de_atomic_u32 loads, unloads;
    u32 dep_calls;
    de_asset_id last_dep;
} hr_counters;

#if DE__ASSET_RELOAD_ENABLED
static const char *hr_bytes(const hr_obj *o) {
    return (const char *)(o + 1);
}
#endif

static de_result hr_load(de_asset_load_ctx *ctx) {
    hr_counters *c = (hr_counters *)de_asset_load_ctx_user(ctx);
    u64 n = 0;
    const char *p = (const char *)de_asset_load_ctx_data(ctx, &n);
    const char *const *items = NULL;
    u32 k = de_asset_load_ctx_settings_string_array(ctx, "deps", &items);
    for (u32 i = 0; i < k; ++i)
        if (de_asset_load_ctx_add_dependency(ctx, de_asset_id_from(items[i])) != DE_OK)
            return DE_ERR_OUT_OF_MEMORY;
    if (n >= 4 && memcmp(p, "FAIL", 4) == 0)
        return de_asset_load_ctx_fail(ctx, DE_ERR_PARSE, "requested failure");
    hr_obj *o = (hr_obj *)DE_ALLOC(c->alloc, sizeof(hr_obj) + (size_t)n + 1u, 16, DE_MEM_ASSET);
    if (o == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "hr: out of memory");
    memset(o, 0, sizeof *o);
    o->size = n;
    o->level = de_asset_load_ctx_settings_int(ctx, "level", -1);
    memcpy(o + 1, p, (size_t)n);
    ((char *)(o + 1))[n] = '\0';
    de_asset_load_ctx_set_object(ctx, o, sizeof(hr_obj) + n);
    (void)de_atomic_fetch_add_u32(&c->loads, 1, DE_ATOMIC_ACQ_REL);
    return DE_OK;
}

static void hr_unload(void *object, void *user) {
    hr_counters *c = (hr_counters *)user;
    hr_obj *o = (hr_obj *)object;
    DE_FREE(c->alloc, o, sizeof(hr_obj) + (size_t)o->size + 1u);
    (void)de_atomic_fetch_add_u32(&c->unloads, 1, DE_ATOMIC_ACQ_REL);
}

static void hr_dep_reloaded(void *object, de_asset_id dep, void *user) {
    hr_counters *c = (hr_counters *)user;
    ((hr_obj *)object)->dep_reloads++;
    c->dep_calls++;
    c->last_dep = dep;
}

#if DE_HAS_COOKER
/* The `hri` importer: the source, then "|" and the file named by its first line after "inc:"
 * (read through de_asset_import_read_file, so it is an extra source). */
static de_result hri_import(de_asset_import_ctx *ctx) {
    u64 n = 0;
    const char *src = (const char *)de_asset_import_source(ctx, &n);
    de_result r = de_asset_import_write(ctx, src, n);
    if (r != DE_OK)
        return r;
    if (n > 4 && memcmp(src, "inc:", 4) == 0) {
        char name[64];
        u64 len = 0;
        while (4u + len < n && src[4u + len] != '\n' && len < sizeof name - 1u) {
            name[len] = src[4u + len];
            ++len;
        }
        name[len] = '\0';
        const void *data = NULL;
        u64 size = 0;
        r = de_asset_import_read_file(ctx, name, &data, &size);
        if (r != DE_OK)
            return de_asset_import_fail(ctx, r, "cannot read the include \"%s\"", name);
        if (size >= 4 && memcmp(data, "FAIL", 4) == 0)
            return de_asset_import_fail(ctx, DE_ERR_PARSE, "the include asks to fail");
        if ((r = de_asset_import_write(ctx, "|", 1)) != DE_OK)
            return r;
        return de_asset_import_write(ctx, data, size);
    }
    return DE_OK;
}
#endif

/* ---- Fixture ------------------------------------------------------------------------------- */

#define HR_MAX_EVENTS 64

static de_asset_event g_ev[HR_MAX_EVENTS];
static u32 g_ev_count;

static void capture(de_asset_system *s, const de_asset_event *ev) {
    (void)s;
    if (g_ev_count < HR_MAX_EVENTS)
        g_ev[g_ev_count] = *ev;
    g_ev_count++;
}

/* RELOADED events for `id` since the last reset (`id` 0: any). */
static u32 reloaded(de_asset_id id) {
    u32 n = 0;
    for (u32 i = 0; i < g_ev_count && i < HR_MAX_EVENTS; ++i)
        if (g_ev[i].kind == DE_ASSET_EV_RELOADED && (id.hash == 0 || g_ev[i].id.hash == id.hash))
            n++;
    return n;
}

typedef struct hr_fx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_task_system *tasks;
    de_asset_system *s;
    hr_counters c;
    char dir[64]; /* hr:/<test> */
} hr_fx;

static de_sem g_nap;

#if DE__ASSET_RELOAD_ENABLED
static void nap_ns(u64 ns) {
    (void)de_sem_wait_timeout(&g_nap, ns);
}
#endif

static void mount_once(void) {
    static bool mounted;
    if (mounted)
        return;
    ASSERT_EQ(de_sem_init(&g_nap, 0), DE_OK); /* never posted: a blocking nap */
    ASSERT_EQ(de_fs_mount("hr", DE_ASSET_HOT_RELOAD_TMP_DIR, DE_MOUNT_WRITABLE, 0), DE_OK);
    mounted = true;
}

static void hr_fx_setup(hr_fx *f) {
    mount_once();
    memset(f, 0, sizeof *f);
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_ASSET);
    f->c.alloc = &f->alloc;
    de_atomic_init_u32(&f->c.loads, 0);
    de_atomic_init_u32(&f->c.unloads, 0);
    g_ev_count = 0;
    de_fp_env_init_thread();
}

static void hr_fx_teardown(hr_fx *f) {
    de_asset_system_destroy(f->s);
    f->s = NULL;
    if (f->tasks != NULL)
        de_task_system_destroy(f->tasks);
    f->tasks = NULL;
    /* Every job, object, retired object, record and watch is gone. */
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).bytes_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0u);
}

static void register_types(hr_fx *f) {
    static const char *const exts[] = {".hr"};
    de_asset_type_desc d = {0};
    d.name = "hr";
    d.extensions = exts;
    d.extension_count = 1;
    d.load = hr_load;
    d.unload = hr_unload;
    d.dependency_reloaded = hr_dep_reloaded;
    d.user = &f->c;
    ASSERT_TRUE(DE_HANDLE_VALID(de_asset_type_register(f->s, &d)));
#if DE_HAS_COOKER
    static const char *const exts_i[] = {".hri"};
    d.name = "hri";
    d.extensions = exts_i;
    d.import = hri_import;
    ASSERT_TRUE(DE_HANDLE_VALID(de_asset_type_register(f->s, &d)));
#endif
}

#if DE__ASSET_RELOAD_ENABLED
/* The test's directory hr:/<name> (emptied of what an earlier run left: files are rewritten). */
static void fx_system(hr_fx *f, const char *name, bool with_tasks, u32 retire_frames,
                      bool watching) {
    snprintf(f->dir, sizeof f->dir, "hr:/%s", name);
    (void)de_dir_create(f->dir);
    if (with_tasks) {
        de_task_system_desc td = {.worker_count = 2, .background_worker_count = 1};
        ASSERT_EQ(de_task_system_create(&td, &f->tasks), DE_OK);
    }
    char cache[96];
    snprintf(cache, sizeof cache, "%s/decache", f->dir);
    de_asset_system_desc d = {0};
    d.allocator = &f->alloc;
    d.tasks = f->tasks;
    d.io_threads = 1;
    d.cache_dir = cache;
    d.hot_reload = true;
    d.reload_retire_frames = retire_frames;
    ASSERT_EQ(de_asset_system_create(&d, &f->s), DE_OK);
    de__asset_reload_set_watching(f->s, watching);
    f->s->emit_fn = capture;
    register_types(f);
}
#endif

static void put(hr_fx *f, const char *name, const char *text) {
    char path[160];
    snprintf(path, sizeof path, "%s/%s", f->dir, name);
    ASSERT_EQ(de_file_write_all_atomic(path, text, strlen(text)), DE_OK);
}

static void manifest(hr_fx *f, const char *text) {
    put(f, "assets.toml", text);
    char path[160];
    snprintf(path, sizeof path, "%s/assets.toml", f->dir);
    ASSERT_EQ(de_asset_manifest_load(f->s, path), DE_OK);
}

/* The watch-change entry point for the test file `name`. */
static void changed(hr_fx *f, const char *name) {
    char path[160];
    snprintf(path, sizeof path, "%s/%s", f->dir, name);
    de__asset_reload_on_change(f->s, path, DE_WATCH_MODIFIED, false);
}

static de_asset_id id_of(const char *path) {
    return de_asset_id_from(path);
}

#if DE__ASSET_RELOAD_ENABLED
static u32 handle_of(hr_fx *f, const char *path) {
    return de__asset_handle_of(f->s, id_of(path), NULL);
}

static const hr_obj *obj_of(hr_fx *f, const char *path) {
    return (const hr_obj *)de__asset_object(f->s, handle_of(f, path));
}

static u32 version_of(hr_fx *f, const char *path) {
    const de__slot *sl = de__slot_get(f->s, handle_of(f, path));
    return sl != NULL ? sl->version : 0u;
}
#endif

static de_load_handle load_all(hr_fx *f, const char *const *paths, u32 n) {
    de_asset_id ids[8];
    for (u32 i = 0; i < n; ++i)
        ids[i] = id_of(paths[i]);
    de_load_handle h = de_asset_load_many(f->s, ids, n, 0);
    EXPECT_EQ((int)de_asset_wait(f->s, h, WAIT_NS), (int)DE_LOAD_READY);
    return h;
}

static de__asset_reload_stats rstats(hr_fx *f) {
    de__asset_reload_stats st;
    de__asset_reload_get_stats(f->s, &st);
    return st;
}

#if DE__ASSET_RELOAD_ENABLED
/* Updates until the reloads started so far are all swapped or failed, blocking in the OS between
 * updates (a finished job wakes the block). Returns the updates it took; 0 on timeout. */
static u32 pump(hr_fx *f) {
    const u64 t0 = de_time_now_ns();
    for (u32 frames = 1;; ++frames) {
        de__asset_wake_reset(f->s);
        de_asset_system_update(f->s, NULL);
        de__asset_reload_stats st = rstats(f);
        if (st.swapped + st.failed + st.superseded + st.discarded >= st.started)
            return frames;
        if (de_time_now_ns() - t0 > WAIT_NS)
            return 0;
        (void)de__asset_wake_block(f->s, 10000000u);
    }
}
#endif

/* ---- hot_reload.source_edit --------------------------------------------------------------------
 *
 * The first update after the change is seen starts the reload; once the job is finished, the
 * next update swaps it in: exactly one RELOADED, within two updates. The handle taken before the
 * edit resolves to the new data, the version is bumped, the other asset is untouched. */

TEST_F(hot_reload, source_edit, hr_fx) {
#if !DE__ASSET_RELOAD_ENABLED
    SKIP("hot reload is compiled in Debug and Dev builds only");
#else
    fx_system(f, "source_edit", false, 0, false);
    put(f, "a.hr", "old A");
    put(f, "b.hr", "old B");
    manifest(f, "[assets]\n"
                "\"a\" = { type = \"hr\", source = \"a.hr\" }\n"
                "\"b\" = { type = \"hr\", source = \"b.hr\" }\n");
    const char *const paths[] = {"a", "b"};
    de_load_handle h = load_all(f, paths, 2);
    const u32 ha = handle_of(f, "a");
    const hr_obj *before = obj_of(f, "a");
    ASSERT_TRUE(before != NULL);
    EXPECT_STREQ(hr_bytes(before), "old A");
    const u32 va = version_of(f, "a"), vb = version_of(f, "b");
    const hr_obj *b_before = obj_of(f, "b");
    g_ev_count = 0;

    put(f, "a.hr", "new A, longer");
    changed(f, "a.hr");
    changed(f, "a.hr"); /* the same path twice in a frame: queued once */

    /* Update 1: seen, started. */
    de__asset_wake_reset(f->s);
    de_asset_system_update(f->s, NULL);
    de__asset_reload_stats st = rstats(f);
    EXPECT_EQ(st.queued, 1u);
    EXPECT_EQ(st.started, 1u);
    u32 frames = 1;
    if (st.swapped == 0) {
        /* The read finishes on the I/O thread; its finish wakes this block. */
        EXPECT_TRUE(de__asset_wake_block(f->s, WAIT_NS));
        de_asset_system_update(f->s, NULL);
        frames = 2;
    }
    EXPECT_LE(frames, 2u);
    EXPECT_EQ(reloaded(id_of("a")), 1u);
    EXPECT_EQ(reloaded((de_asset_id){0}), 1u);
    for (u32 i = 0; i < g_ev_count; ++i) {
        if (g_ev[i].kind != DE_ASSET_EV_RELOADED)
            continue;
        EXPECT_EQ((int)g_ev[i].state, (int)DE_LOAD_READY);
        EXPECT_EQ((int)g_ev[i].error, (int)DE_OK);
    }

    /* The same handle, new data, a new version; b untouched. */
    EXPECT_EQ(handle_of(f, "a"), ha);
    const hr_obj *after = obj_of(f, "a");
    ASSERT_TRUE(after != NULL);
    EXPECT_STREQ(hr_bytes(after), "new A, longer");
    EXPECT_EQ(version_of(f, "a"), va + 1u);
    EXPECT_EQ(version_of(f, "b"), vb);
    EXPECT_TRUE(obj_of(f, "b") == b_before);
    EXPECT_EQ((int)de_asset_status_of(f->s, id_of("a")), (int)DE_LOAD_READY);
    EXPECT_EQ((int)de_asset_status(f->s, h, NULL), (int)DE_LOAD_READY);
    char msg[64];
    EXPECT_EQ((int)de_asset_last_error(f->s, id_of("a"), msg, sizeof msg), (int)DE_OK);
    EXPECT_STREQ(msg, "");

    /* Nothing more happens without a change. */
    for (u32 i = 0; i < 4; ++i)
        de_asset_system_update(f->s, NULL);
    EXPECT_EQ(reloaded((de_asset_id){0}), 1u);
    EXPECT_EQ(rstats(f).started, 1u);
    de_asset_release(f->s, h);
#endif
}

/* ---- hot_reload.dependents_notified ------------------------------------------------------------
 *
 * `mat` depends on `tex` (and `other` on nothing): each reload of `tex` calls mat's
 * dependency_reloaded once, after the swap (the new object is in place when it runs). A reload
 * of `mat` itself does not call it. Runs with a task system (loads on frame workers). */

#if DE__ASSET_RELOAD_ENABLED
static hr_fx *g_dep_fx;
static const hr_obj *g_seen_tex; /* tex's object when mat's callback ran */

static void dep_check(void *object, de_asset_id dep, void *user) {
    hr_dep_reloaded(object, dep, user);
    g_seen_tex = obj_of(g_dep_fx, "tex");
}
#endif

TEST_F(hot_reload, dependents_notified, hr_fx) {
#if !DE__ASSET_RELOAD_ENABLED
    SKIP("hot reload is compiled in Debug and Dev builds only");
#else
    fx_system(f, "dependents", true, 0, false);
    g_dep_fx = f;
    /* A second type whose callback also records what tex's slot holds at that moment. */
    static const char *const exts[] = {".hrm"};
    de_asset_type_desc d = {0};
    d.name = "hrm";
    d.extensions = exts;
    d.extension_count = 1;
    d.load = hr_load;
    d.unload = hr_unload;
    d.dependency_reloaded = dep_check;
    d.user = &f->c;
    ASSERT_TRUE(DE_HANDLE_VALID(de_asset_type_register(f->s, &d)));
    put(f, "tex.hr", "tex v1");
    put(f, "mat.hr", "mat v1");
    put(f, "other.hr", "other");
    manifest(f, "[assets]\n"
                "\"tex\" = { type = \"hr\", source = \"tex.hr\" }\n"
                "\"mat\" = { type = \"hrm\", source = \"mat.hr\", deps = [\"tex\"] }\n"
                "\"other\" = { type = \"hr\", source = \"other.hr\" }\n");
    const char *const paths[] = {"mat", "other"};
    de_load_handle h = load_all(f, paths, 2);
    ASSERT_EQ((int)de_asset_status_of(f->s, id_of("tex")), (int)DE_LOAD_READY);
    const hr_obj *mat = obj_of(f, "mat");
    ASSERT_TRUE(mat != NULL);
    EXPECT_EQ(mat->dep_reloads, 0u);

    for (u32 round = 1; round <= 2; ++round) {
        char text[32];
        snprintf(text, sizeof text, "tex v%u", round + 1u);
        put(f, "tex.hr", text);
        changed(f, "tex.hr");
        EXPECT_NE(pump(f), 0u);
        EXPECT_EQ(reloaded(id_of("tex")), round);
        EXPECT_EQ(mat->dep_reloads, round); /* once per reload */
        EXPECT_EQ(f->c.dep_calls, round);
        EXPECT_EQ(f->c.last_dep.hash, id_of("tex").hash);
        EXPECT_TRUE(g_seen_tex == obj_of(f, "tex")); /* after the swap */
        EXPECT_STREQ(hr_bytes(g_seen_tex), text);
    }
    EXPECT_EQ(rstats(f).dependents, 2u);

    /* mat reloads: a new object, no callback (nothing depends on mat). */
    put(f, "mat.hr", "mat v2");
    changed(f, "mat.hr");
    EXPECT_NE(pump(f), 0u);
    EXPECT_EQ(reloaded(id_of("mat")), 1u);
    EXPECT_EQ(f->c.dep_calls, 2u);
    EXPECT_STREQ(hr_bytes(obj_of(f, "mat")), "mat v2");
    /* The reloaded mat still holds its dependency: tex is referenced by an asset ref. */
    u32 handle_refs = 0, asset_refs = 0;
    de__slot_refs(de__slot_get(f->s, handle_of(f, "tex")), &handle_refs, &asset_refs);
    EXPECT_GE(asset_refs, 1u);
    de_asset_release(f->s, h);
#endif
}

/* ---- hot_reload.failure_keeps_old --------------------------------------------------------------
 *
 * A source that makes the loader fail: the old object stays, an Error is logged, no event, and
 * de_asset_last_error has the message. Fixing it reloads and clears the error. */

#if DE__ASSET_RELOAD_ENABLED
static u32 g_errors;
static char g_error_text[512];

static void error_sink(const de_log_record *rec, void *user) {
    (void)user;
    if (rec->level < DE_LOG_ERROR || strcmp(de_log_category_name(rec->category), "asset") != 0)
        return;
    g_errors++;
    snprintf(g_error_text, sizeof g_error_text, "%.*s", (int)rec->message.len, rec->message.ptr);
}
#endif

TEST_F(hot_reload, failure_keeps_old, hr_fx) {
#if !DE__ASSET_RELOAD_ENABLED
    SKIP("hot reload is compiled in Debug and Dev builds only");
#else
    de_log_desc ld;
    memset(&ld, 0, sizeof ld);
    ld.no_console_sink = true;
    ASSERT_EQ(de_log_init(&ld), DE_OK);
    de_log_add_sink(error_sink, NULL);
    g_errors = 0;

    fx_system(f, "failure", false, 0, false);
    put(f, "a.hr", "good v1");
    manifest(f, "[assets]\n\"a\" = { type = \"hr\", source = \"a.hr\" }\n");
    const char *const paths[] = {"a"};
    de_load_handle h = load_all(f, paths, 1);
    const hr_obj *v1 = obj_of(f, "a");
    const u32 ver = version_of(f, "a");
    g_ev_count = 0;

    put(f, "a.hr", "FAIL now");
    changed(f, "a.hr");
    EXPECT_NE(pump(f), 0u);
    EXPECT_EQ(rstats(f).failed, 1u);
    EXPECT_EQ(g_ev_count, 0u); /* a failed reload sends no event */
    EXPECT_TRUE(obj_of(f, "a") == v1);
    EXPECT_STREQ(hr_bytes(obj_of(f, "a")), "good v1");
    EXPECT_EQ(version_of(f, "a"), ver);
    EXPECT_EQ((int)de_asset_status_of(f->s, id_of("a")), (int)DE_LOAD_READY);
    char msg[256];
    EXPECT_EQ((int)de_asset_last_error(f->s, id_of("a"), msg, sizeof msg), (int)DE_ERR_PARSE);
    EXPECT_TRUE(strstr(msg, "asset \"a\"") != NULL);
    EXPECT_TRUE(strstr(msg, "requested failure") != NULL);
    de_log_flush();
    EXPECT_GE(g_errors, 1u);
    EXPECT_TRUE(strstr(g_error_text, "requested failure") != NULL);
    /* A short buffer is cut and NUL-terminated. */
    char tiny[8];
    EXPECT_EQ((int)de_asset_last_error(f->s, id_of("a"), tiny, sizeof tiny), (int)DE_ERR_PARSE);
    EXPECT_EQ(strlen(tiny), sizeof tiny - 1u);

    /* Fixed: reloads, clears the error. */
    put(f, "a.hr", "good v2");
    changed(f, "a.hr");
    EXPECT_NE(pump(f), 0u);
    EXPECT_EQ(reloaded(id_of("a")), 1u);
    EXPECT_STREQ(hr_bytes(obj_of(f, "a")), "good v2");
    EXPECT_EQ((int)de_asset_last_error(f->s, id_of("a"), msg, sizeof msg), (int)DE_OK);
    EXPECT_STREQ(msg, "");

    /* de_asset_last_error also reports a load failure, and nothing for unknown IDs. */
    put(f, "bad.hr", "FAIL at load");
    manifest(f, "[assets]\n\"a\" = { type = \"hr\", source = \"a.hr\" }\n"
                "\"bad\" = { type = \"hr\", source = \"bad.hr\" }\n");
    de_load_handle hb = de_asset_load(f->s, id_of("bad"), 0);
    EXPECT_EQ((int)de_asset_wait(f->s, hb, WAIT_NS), (int)DE_LOAD_FAILED);
    EXPECT_EQ((int)de_asset_last_error(f->s, id_of("bad"), msg, sizeof msg), (int)DE_ERR_PARSE);
    EXPECT_TRUE(strstr(msg, "requested failure") != NULL);
    EXPECT_EQ((int)de_asset_last_error(f->s, id_of("nothing"), msg, sizeof msg), (int)DE_OK);
    EXPECT_EQ((int)de_asset_last_error(NULL, id_of("a"), NULL, 0), (int)DE_ERR_INVALID_ARGUMENT);
    de_asset_release(f->s, hb);
    de_asset_release(f->s, h);

    de_asset_system_destroy(f->s);
    f->s = NULL;
    de_log_flush();
    de_log_shutdown();
#endif
}

/* ---- hot_reload.extra_source -------------------------------------------------------------------
 *
 * An importer reads a second file through de_asset_import_read_file: editing that file reloads
 * the owning asset (the cooker's extra sources are in the reverse index). A broken include
 * fails the reload (importer failure) and keeps the old data; fixing it reloads. */

TEST_F(hot_reload, extra_source, hr_fx) {
#if !DE__ASSET_RELOAD_ENABLED || !DE_HAS_COOKER
    SKIP("needs hot reload and the in-process cooker (Debug and Dev with tools)");
#else
    fx_system(f, "extra_source", true, 0, false);
    put(f, "inc.txt", "include v1");
    put(f, "main.hri", "inc:inc.txt\n");
    put(f, "plain.hr", "plain");
    manifest(f, "[assets]\n"
                "\"m\" = { type = \"hri\", source = \"main.hri\" }\n"
                "\"p\" = { type = \"hr\", source = \"plain.hr\" }\n");
    const char *const paths[] = {"m", "p"};
    de_load_handle h = load_all(f, paths, 2);
    EXPECT_STREQ(hr_bytes(obj_of(f, "m")), "inc:inc.txt\n|include v1");
    g_ev_count = 0;

    put(f, "inc.txt", "include v2");
    changed(f, "inc.txt");
    EXPECT_NE(pump(f), 0u);
    EXPECT_EQ(rstats(f).started, 1u); /* only the owner */
    EXPECT_EQ(reloaded(id_of("m")), 1u);
    EXPECT_EQ(reloaded(id_of("p")), 0u);
    EXPECT_STREQ(hr_bytes(obj_of(f, "m")), "inc:inc.txt\n|include v2");

    /* The importer fails: old data, the importer's message. */
    put(f, "inc.txt", "FAIL include");
    changed(f, "inc.txt");
    EXPECT_NE(pump(f), 0u);
    EXPECT_EQ(rstats(f).failed, 1u);
    EXPECT_STREQ(hr_bytes(obj_of(f, "m")), "inc:inc.txt\n|include v2");
    char msg[256];
    EXPECT_EQ((int)de_asset_last_error(f->s, id_of("m"), msg, sizeof msg), (int)DE_ERR_PARSE);
    EXPECT_TRUE(strstr(msg, "the include asks to fail") != NULL);

    /* Fixed (the include is still indexed from the last good cook). */
    put(f, "inc.txt", "include v3");
    changed(f, "inc.txt");
    EXPECT_NE(pump(f), 0u);
    EXPECT_EQ(reloaded(id_of("m")), 2u);
    EXPECT_STREQ(hr_bytes(obj_of(f, "m")), "inc:inc.txt\n|include v3");
    EXPECT_EQ((int)de_asset_last_error(f->s, id_of("m"), msg, sizeof msg), (int)DE_OK);

    /* The main source of a cooked asset is indexed too. */
    put(f, "inc2.txt", "second");
    put(f, "main.hri", "inc:inc2.txt\n");
    changed(f, "main.hri");
    EXPECT_NE(pump(f), 0u);
    EXPECT_EQ(reloaded(id_of("m")), 3u);
    EXPECT_STREQ(hr_bytes(obj_of(f, "m")), "inc:inc2.txt\n|second");
    /* The index follows the new cook: inc.txt no longer reloads it, inc2.txt does. */
    changed(f, "inc.txt");
    de_asset_system_update(f->s, NULL);
    EXPECT_EQ(rstats(f).started, 4u);
    changed(f, "inc2.txt");
    EXPECT_NE(pump(f), 0u);
    EXPECT_EQ(rstats(f).started, 5u);
    de_asset_release(f->s, h);
#endif
}

/* ---- hot_reload.manifest_edit ------------------------------------------------------------------
 *
 * A manifest edit that changes one asset's setting reloads only that asset (with the new
 * setting); an added asset is loadable without a restart; a removed one keeps its data; a type
 * change is refused with an error. */

TEST_F(hot_reload, manifest_edit, hr_fx) {
#if !DE__ASSET_RELOAD_ENABLED
    SKIP("hot reload is compiled in Debug and Dev builds only");
#else
    fx_system(f, "manifest_edit", false, 0, false);
    put(f, "a.hr", "A");
    put(f, "b.hr", "B");
    put(f, "c.hr", "C");
    manifest(f, "[assets]\n"
                "\"a\" = { type = \"hr\", source = \"a.hr\", level = 1 }\n"
                "\"b\" = { type = \"hr\", source = \"b.hr\", level = 1 }\n");
    const char *const paths[] = {"a", "b"};
    de_load_handle h = load_all(f, paths, 2);
    EXPECT_EQ(obj_of(f, "a")->level, 1);
    const hr_obj *b_obj = obj_of(f, "b");
    g_ev_count = 0;

    put(f, "assets.toml",
        "[assets]\n"
        "\"a\" = { type = \"hr\", source = \"a.hr\", level = 2 }\n"
        "\"b\" = { type = \"hr\", source = \"b.hr\", level = 1 }\n"
        "\"c\" = { type = \"hr\", source = \"c.hr\" }\n");
    changed(f, "assets.toml");
    EXPECT_NE(pump(f), 0u);
    EXPECT_EQ(rstats(f).manifest_reloads, 1u);
    EXPECT_EQ(rstats(f).started, 1u);
    EXPECT_EQ(reloaded(id_of("a")), 1u);
    EXPECT_EQ(reloaded((de_asset_id){0}), 1u);
    EXPECT_EQ(obj_of(f, "a")->level, 2);
    EXPECT_TRUE(obj_of(f, "b") == b_obj);
    /* The added asset loads without a restart. */
    de_load_handle hc = de_asset_load(f->s, id_of("c"), 0);
    EXPECT_EQ((int)de_asset_wait(f->s, hc, WAIT_NS), (int)DE_LOAD_READY);
    EXPECT_STREQ(hr_bytes(obj_of(f, "c")), "C");

    /* b is removed: its data stays while referenced; nothing reloads. A source change of b no
     * longer reloads it. */
    put(f, "assets.toml",
        "[assets]\n"
        "\"a\" = { type = \"hr\", source = \"a.hr\", level = 2 }\n"
        "\"c\" = { type = \"hr\", source = \"c.hr\" }\n");
    changed(f, "assets.toml");
    EXPECT_NE(pump(f), 0u);
    EXPECT_EQ(rstats(f).started, 1u);
    EXPECT_TRUE(obj_of(f, "b") == b_obj);
    changed(f, "b.hr");
    de_asset_system_update(f->s, NULL);
    EXPECT_EQ(rstats(f).started, 1u);

    /* A broken manifest keeps the previous one. */
    put(f, "assets.toml", "[assets\n");
    changed(f, "assets.toml");
    de_asset_system_update(f->s, NULL);
    EXPECT_EQ(rstats(f).manifest_reloads, 2u);
    EXPECT_STREQ(hr_bytes(obj_of(f, "a")), "A");

    /* c's type changes: refused, the error is kept, the data stays. */
    put(f, "assets.toml",
        "[assets]\n"
        "\"a\" = { type = \"hr\", source = \"a.hr\", level = 2 }\n"
        "\"c\" = { type = \"other\", source = \"c.hr\" }\n");
    changed(f, "assets.toml");
    EXPECT_NE(pump(f), 0u);
    EXPECT_EQ(rstats(f).manifest_reloads, 3u);
    EXPECT_EQ(rstats(f).started, 1u);
    char msg[256];
    EXPECT_EQ((int)de_asset_last_error(f->s, id_of("c"), msg, sizeof msg),
              (int)DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(strstr(msg, "type changed") != NULL);
    EXPECT_STREQ(hr_bytes(obj_of(f, "c")), "C");
    EXPECT_EQ(reloaded((de_asset_id){0}), 1u);
    de_asset_release(f->s, hc);
    de_asset_release(f->s, h);
#endif
}

/* ---- hot_reload.deferred_unload ----------------------------------------------------------------
 *
 * The old object goes to `unload` exactly reload_retire_frames updates after the update that
 * swapped the new one in; the default is 3. */

#if DE__ASSET_RELOAD_ENABLED
static void deferred_case(hr_fx *f, u32 desc_frames, u32 expect_frames) {
    put(f, "a.hr", "v1");
    manifest(f, "[assets]\n\"a\" = { type = \"hr\", source = \"a.hr\" }\n");
    const char *const paths[] = {"a"};
    de_load_handle h = load_all(f, paths, 1);
    (void)desc_frames;
    put(f, "a.hr", "v2");
    changed(f, "a.hr");
    EXPECT_NE(pump(f), 0u); /* the last update swapped */
    ASSERT_EQ(rstats(f).swapped, 1u);
    const u32 unloads = de_atomic_load_u32(&f->c.unloads, DE_ATOMIC_ACQUIRE);
    for (u32 frame = 1; frame <= expect_frames + 2u; ++frame) {
        de_asset_system_update(f->s, NULL);
        u32 now = de_atomic_load_u32(&f->c.unloads, DE_ATOMIC_ACQUIRE) - unloads;
        EXPECT_EQ(now, frame >= expect_frames ? 1u : 0u);
        EXPECT_EQ(rstats(f).retired, frame >= expect_frames ? 1u : 0u);
    }
    EXPECT_STREQ(hr_bytes(obj_of(f, "a")), "v2");
    de_asset_release(f->s, h);
}
#endif

TEST_F(hot_reload, deferred_unload, hr_fx) {
#if !DE__ASSET_RELOAD_ENABLED
    SKIP("hot reload is compiled in Debug and Dev builds only");
#else
    fx_system(f, "deferred_unload", false, 5, false);
    deferred_case(f, 5, 5);
#endif
}

TEST_F(hot_reload, deferred_unload_default, hr_fx) {
#if !DE__ASSET_RELOAD_ENABLED
    SKIP("hot reload is compiled in Debug and Dev builds only");
#else
    fx_system(f, "deferred_default", false, 0, false);
    deferred_case(f, 0, DE__ASSET_RELOAD_RETIRE_DEFAULT);
    /* Destroy unloads a retired object at once: reload again and destroy before it retires. */
    put(f, "a.hr", "v3");
    changed(f, "a.hr");
    EXPECT_NE(pump(f), 0u);
    const u32 unloads = de_atomic_load_u32(&f->c.unloads, DE_ATOMIC_ACQUIRE);
    de_asset_system_destroy(f->s);
    f->s = NULL;
    EXPECT_EQ(de_atomic_load_u32(&f->c.unloads, DE_ATOMIC_ACQUIRE), unloads + 2u); /* old + live */
#endif
}

/* ---- hot_reload.pack_override_skipped ----------------------------------------------------------
 *
 * An asset loaded from its source, then overridden by a mounted pack: a change of its source
 * does not reload it (Info once), and nothing else changes. */

TEST_F(hot_reload, pack_override_skipped, hr_fx) {
#if !DE__ASSET_RELOAD_ENABLED || !DE_HR_WITH_PACK
    SKIP("needs hot reload and the pack writer (Debug and Dev with tools)");
#else
    fx_system(f, "pack_override", false, 0, false);
    put(f, "a.hr", "source A");
    put(f, "b.hr", "source B");
    manifest(f, "[assets]\n"
                "\"a\" = { type = \"hr\", source = \"a.hr\" }\n"
                "\"b\" = { type = \"hr\", source = \"b.hr\" }\n");
    const char *const paths[] = {"a", "b"};
    de_load_handle h = load_all(f, paths, 2);

    pt_asset p[1];
    memset(p, 0, sizeof p);
    snprintf(p[0].path, sizeof p[0].path, "a");
    p[0].type = "hr";
    p[0].data = (u8 *)"PACK A";
    p[0].size = 6;
    char pack[160];
    snprintf(pack, sizeof pack, "%s/patch.dpak", f->dir);
    pt_pack_file(&(dpak_writer_desc){.zstd_level = 3}, p, 1, pack);
    ASSERT_EQ(de_asset_pack_mount(f->s, pack, 0), DE_OK);
    g_ev_count = 0;

    put(f, "a.hr", "source A2");
    put(f, "b.hr", "source B2");
    for (u32 i = 0; i < 2; ++i) {
        changed(f, "a.hr");
        changed(f, "b.hr");
        EXPECT_NE(pump(f), 0u);
    }
    de__asset_reload_stats st = rstats(f);
    EXPECT_EQ(st.skipped_pack, 2u);
    EXPECT_EQ(st.started, 2u); /* b only, twice */
    EXPECT_EQ(reloaded(id_of("a")), 0u);
    EXPECT_EQ(reloaded(id_of("b")), 2u);
    EXPECT_STREQ(hr_bytes(obj_of(f, "a")), "source A");
    EXPECT_STREQ(hr_bytes(obj_of(f, "b")), "source B2");
    de_asset_release(f->s, h);
#endif
}

/* ---- hot_reload.superseded ---------------------------------------------------------------------
 *
 * A second change while the first reload is still in flight cancels it: one swap, of the newest
 * bytes. Without a task system the cook (an importer type) runs inline, so the job is finished
 * but not yet published when the second change arrives. */

TEST_F(hot_reload, superseded, hr_fx) {
#if !DE__ASSET_RELOAD_ENABLED || !DE_HAS_COOKER
    SKIP("needs hot reload and the in-process cooker (Debug and Dev with tools)");
#else
    fx_system(f, "superseded", false, 0, false);
    put(f, "inc.txt", "x");
    put(f, "m.hri", "first");
    manifest(f, "[assets]\n\"m\" = { type = \"hri\", source = \"m.hri\" }\n");
    const char *const paths[] = {"m"};
    de_load_handle h = load_all(f, paths, 1);
    g_ev_count = 0;
    /* Start a reload, then queue a second change before any update publishes the first: the
     * reload update of the second change cancels the first job, which the same update then
     * discards. */
    put(f, "m.hri", "second");
    changed(f, "m.hri");
    de__asset_reload_update(f->s); /* starts job 1 (inline cook and load) */
    put(f, "m.hri", "third");
    changed(f, "m.hri");
    EXPECT_NE(pump(f), 0u);
    de__asset_reload_stats st = rstats(f);
    EXPECT_EQ(st.started, 2u);
    EXPECT_EQ(st.superseded, 1u);
    EXPECT_EQ(st.swapped, 1u);
    EXPECT_EQ(reloaded(id_of("m")), 1u);
    EXPECT_STREQ(hr_bytes(obj_of(f, "m")), "third");
    de_asset_release(f->s, h);
#endif
}

/* ---- hot_reload.real_watcher -------------------------------------------------------------------
 *
 * The OS watcher (FSEvents, inotify, ReadDirectoryChangesW) sees an edit of a source and the
 * asset reloads, within a generous timeout. */

TEST_F(hot_reload, real_watcher, hr_fx) {
#if !DE__ASSET_RELOAD_ENABLED
    SKIP("hot reload is compiled in Debug and Dev builds only");
#else
    fx_system(f, "real_watcher", true, 0, true);
    put(f, "w.hr", "watch v1");
    manifest(f, "[assets]\n\"w\" = { type = \"hr\", source = \"w.hr\" }\n");
    const char *const paths[] = {"w"};
    de_load_handle h = load_all(f, paths, 1);
    de_asset_system_update(f->s, NULL); /* creates the watches */
    EXPECT_GE(rstats(f).watches, 1u);
    nap_ns(200000000u); /* let the OS watch settle (FSEvents latency) */
    g_ev_count = 0;
    put(f, "w.hr", "watch v2");
    const u64 t0 = de_time_now_ns();
    while (reloaded(id_of("w")) == 0 && de_time_now_ns() - t0 < (u64)20 * 1000000000u) {
        de_asset_system_update(f->s, NULL);
        nap_ns(5000000u);
    }
    EXPECT_GE(reloaded(id_of("w")), 1u);
    EXPECT_STREQ(hr_bytes(obj_of(f, "w")), "watch v2");
    de_asset_release(f->s, h);
#endif
}

/* ---- hot_reload.off ----------------------------------------------------------------------------
 *
 * Without desc.hot_reload nothing is watched or reloaded, and the entry point is a no-op. */

TEST_F(hot_reload, off, hr_fx) {
    snprintf(f->dir, sizeof f->dir, "hr:/off");
    (void)de_dir_create(f->dir);
    de_asset_system_desc d = {0};
    d.allocator = &f->alloc;
    ASSERT_EQ(de_asset_system_create(&d, &f->s), DE_OK);
    f->s->emit_fn = capture;
    register_types(f);
    EXPECT_TRUE(f->s->reload == NULL);
    put(f, "a.hr", "A");
    manifest(f, "[assets]\n\"a\" = { type = \"hr\", source = \"a.hr\" }\n");
    const char *const paths[] = {"a"};
    de_load_handle h = load_all(f, paths, 1);
    changed(f, "a.hr");
    de_asset_system_update(f->s, NULL);
    EXPECT_EQ(rstats(f).changes, 0u);
    EXPECT_EQ(reloaded((de_asset_id){0}), 0u);
    de_asset_release(f->s, h);
}

/* ---- hot_reload.own_reloaded -------------------------------------------------------------------
 *
 * DENG-289: a type's `reloaded` runs once per reload of its own asset, after the swap (its
 * `object` is the new live object), after the RELOADED event and before the dependents'
 * dependency_reloaded. Assets of other types and failed reloads do not call it; a type without
 * the callback (`hr`) behaves as before. */

#if DE__ASSET_RELOAD_ENABLED
static hr_fx *g_own_fx;
static u32 g_own_calls;
static de_asset_id g_own_id;
static bool g_own_was_live;      /* `object` was the slot's object when the callback ran */
static u32 g_own_events_seen;    /* RELOADED events captured when the callback ran */
static u32 g_own_dep_calls_seen; /* dependency_reloaded calls made when the callback ran */

static void own_reloaded(void *object, de_asset_id id, void *user) {
    hr_counters *c = (hr_counters *)user;
    g_own_calls++;
    g_own_id = id;
    g_own_was_live = object == (const void *)obj_of(g_own_fx, "self");
    g_own_events_seen = reloaded(id);
    g_own_dep_calls_seen = c->dep_calls;
}
#endif

TEST_F(hot_reload, own_reloaded, hr_fx) {
#if !DE__ASSET_RELOAD_ENABLED
    SKIP("hot reload is compiled in Debug and Dev builds only");
#else
    fx_system(f, "own_reloaded", true, 0, false);
    g_own_fx = f;
    g_own_calls = 0;
    static const char *const exts[] = {".hro"};
    de_asset_type_desc d = {0};
    d.name = "hro";
    d.extensions = exts;
    d.extension_count = 1;
    d.load = hr_load;
    d.unload = hr_unload;
    d.reloaded = own_reloaded;
    d.user = &f->c;
    ASSERT_TRUE(DE_HANDLE_VALID(de_asset_type_register(f->s, &d)));
    put(f, "self.hr", "self v1");
    put(f, "user.hr", "user v1");
    put(f, "plain.hr", "plain v1");
    manifest(f, "[assets]\n"
                "\"self\" = { type = \"hro\", source = \"self.hr\" }\n"
                "\"user\" = { type = \"hr\", source = \"user.hr\", deps = [\"self\"] }\n"
                "\"plain\" = { type = \"hr\", source = \"plain.hr\" }\n");
    const char *const paths[] = {"user", "plain"};
    de_load_handle h = load_all(f, paths, 2);
    ASSERT_EQ((int)de_asset_status_of(f->s, id_of("self")), (int)DE_LOAD_READY);
    EXPECT_EQ(g_own_calls, 0u); /* a first load is not a reload */

    for (u32 round = 1; round <= 2; ++round) {
        g_ev_count = 0;
        char text[32];
        snprintf(text, sizeof text, "self v%u", round + 1u);
        put(f, "self.hr", text);
        changed(f, "self.hr");
        EXPECT_NE(pump(f), 0u);
        EXPECT_EQ(g_own_calls, round);
        EXPECT_EQ(g_own_id.hash, id_of("self").hash);
        EXPECT_TRUE(g_own_was_live);
        EXPECT_EQ(g_own_events_seen, 1u);            /* after RELOADED */
        EXPECT_EQ(g_own_dep_calls_seen, round - 1u); /* before the dependents */
        EXPECT_EQ(f->c.dep_calls, round);
        EXPECT_STREQ(hr_bytes(obj_of(f, "self")), text);
    }

    /* Another type's reload, and a failed reload of `self`: no call. */
    put(f, "plain.hr", "plain v2");
    changed(f, "plain.hr");
    EXPECT_NE(pump(f), 0u);
    put(f, "self.hr", "FAIL now");
    changed(f, "self.hr");
    EXPECT_NE(pump(f), 0u);
    EXPECT_EQ(g_own_calls, 2u);
    EXPECT_STREQ(hr_bytes(obj_of(f, "self")), "self v3");
    de_asset_release(f->s, h);
#endif
}
