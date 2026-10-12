/*
 * test_pipeline.c: the asset load pipeline (DENG-142, SPEC §11.4): real files through the
 * process I/O threads, zstd payloads, loaders on workers and on the main thread, dependencies,
 * priorities, cancellation, and a 10k-asset stress run (run it under TSan).
 *
 * Files are written at run time into a scratch directory of the build tree, mounted as pipe:/;
 * the manifest is parsed from memory as pipe:/assets.toml, so sources resolve to pipe:/<source>.
 * data/pipeline/ (mounted as pdata:/) holds the checked-in zstd payload fixture.
 */
#include "de_alloc_guard.h"
#include "de_test.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_math.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_task.h>

#include "asset/asset_internal.h"
#include "asset/load_internal.h"
#include "asset/manifest_internal.h"
#include "asset/pipeline_internal.h"
#include "asset/resolve_internal.h"
#include "asset/slots_internal.h"
#include "platform/de_atomic.h"

#include <zstd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test hook of the I/O system (src/platform/async_io.c): called on the I/O thread after each
 * chunk of a read. */
void de__io_test_set_chunk_hook(void (*fn)(de_io_request *req, u64 done));

#define WAIT_NS ((u64)60 * 1000000000u)

/* ---- The `pt` test type ----------------------------------------------------------------------
 *
 * Payload: an optional first line "deps:<id>,<id>,...", then anything. A payload that starts
 * with "FAIL" fails with DE_ERR_PARSE. A string-array setting `deps` adds dependencies too. The
 * object is a pt_obj followed by a copy of the payload, allocated from the fixture's allocator.
 */

typedef struct pt_counters {
    const de_allocator *alloc;
    de_atomic_u32 loads, unloads, seq;
    de_atomic_u32 gate_closed, gate_reached;
    de_atomic_u64 gate_id; /* asset whose loader waits while gate_closed */
} pt_counters;

typedef struct pt_obj {
    u64 size;
    u32 seq;
    u32 thread;
    i64 level;
    f64 ratio;
    bool flag;
    char name[16];
    u32 tag_count;
    u32 pad_;
} pt_obj;

static const u8 *pt_bytes(const pt_obj *o) {
    return (const u8 *)(o + 1);
}

static de_result pt_load(de_asset_load_ctx *ctx) {
    pt_counters *c = (pt_counters *)de_asset_load_ctx_user(ctx);
    u64 n = 0;
    const char *p = (const char *)de_asset_load_ctx_data(ctx, &n);
    de_asset_id id = de_asset_load_ctx_id(ctx);
    if (id.hash == de_atomic_load_u64(&c->gate_id, DE_ATOMIC_ACQUIRE)) {
        de_atomic_store_u32(&c->gate_reached, 1, DE_ATOMIC_RELEASE);
        while (de_atomic_load_u32(&c->gate_closed, DE_ATOMIC_ACQUIRE) != 0)
            de_sleep_ns(100000u);
    }
    u32 seq = de_atomic_fetch_add_u32(&c->seq, 1, DE_ATOMIC_ACQ_REL);

    const char *const *items = NULL;
    u32 k = de_asset_load_ctx_settings_string_array(ctx, "deps", &items);
    for (u32 i = 0; i < k; ++i)
        if (de_asset_load_ctx_add_dependency(ctx, de_asset_id_from(items[i])) != DE_OK)
            return DE_ERR_OUT_OF_MEMORY;
    if (n >= 5 && memcmp(p, "deps:", 5) == 0) {
        u64 i = 5;
        while (i < n && p[i] != '\n') {
            char name[128];
            u32 len = 0;
            while (i < n && p[i] != ',' && p[i] != '\n' && len < sizeof name - 1u)
                name[len++] = p[i++];
            name[len] = '\0';
            if (i < n && p[i] == ',')
                ++i;
            if (de_asset_load_ctx_add_dependency(ctx, de_asset_id_from(name)) != DE_OK)
                return DE_ERR_OUT_OF_MEMORY;
        }
    }
    if (n >= 4 && memcmp(p, "FAIL", 4) == 0)
        return de_asset_load_ctx_fail(ctx, DE_ERR_PARSE, "requested failure");

    pt_obj *o = (pt_obj *)DE_ALLOC(c->alloc, sizeof(pt_obj) + (size_t)n, 16, DE_MEM_ASSET);
    if (o == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "pt: out of memory");
    memset(o, 0, sizeof *o);
    o->size = n;
    o->seq = seq;
    o->thread = de_thread_current_id();
    o->level = de_asset_load_ctx_settings_int(ctx, "level", -1);
    o->ratio = de_asset_load_ctx_settings_float(ctx, "ratio", -1.0);
    o->flag = de_asset_load_ctx_settings_bool(ctx, "flag", false);
    snprintf(o->name, sizeof o->name, "%s", de_asset_load_ctx_settings_string(ctx, "name", "-"));
    o->tag_count = de_asset_load_ctx_settings_string_array(ctx, "tags", NULL);
    if (n != 0)
        memcpy(o + 1, p, (size_t)n);
    de_asset_load_ctx_set_object(ctx, o, sizeof(pt_obj) + n);
    (void)de_atomic_fetch_add_u32(&c->loads, 1, DE_ATOMIC_ACQ_REL);
    return DE_OK;
}

static void pt_unload(void *object, void *user) {
    pt_counters *c = (pt_counters *)user;
    pt_obj *o = (pt_obj *)object;
    DE_FREE(c->alloc, o, sizeof(pt_obj) + (size_t)o->size);
    (void)de_atomic_fetch_add_u32(&c->unloads, 1, DE_ATOMIC_ACQ_REL);
}

static de_result pt_import(de_asset_import_ctx *ctx) {
#if DE_HAS_COOKER /* DENG-209: the in-process cooker runs it; its failure fails the load */
    return de_asset_import_fail(ctx, DE_ERR_PARSE, "the pti importer refuses");
#else
    (void)ctx;
    return DE_OK;
#endif
}

/* ---- Fixture ------------------------------------------------------------------------------- */

typedef struct pipe_fx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_task_system *tasks;
    de_asset_system *s;
    pt_counters c;
    u32 main_thread;
} pipe_fx;

static void mount_once(void) {
    static bool mounted;
    if (mounted)
        return;
    ASSERT_EQ(de_fs_mount("pipe", DE_ASSET_PIPELINE_TMP_DIR, DE_MOUNT_WRITABLE, 0), DE_OK);
    ASSERT_EQ(de_fs_mount("pdata", DE_ASSET_TEST_DATA_DIR "/pipeline", DE_MOUNT_READ_ONLY, 0),
              DE_OK);
    mounted = true;
}

static void register_types(pipe_fx *f) {
    static const char *const exts[] = {".pt"};
    de_asset_type_desc d = {0};
    d.name = "pt";
    d.extensions = exts;
    d.extension_count = 1;
    d.load = pt_load;
    d.unload = pt_unload;
    d.user = &f->c;
    ASSERT_TRUE(DE_HANDLE_VALID(de_asset_type_register(f->s, &d)));
    static const char *const exts_m[] = {".ptm"};
    d.name = "ptm";
    d.extensions = exts_m;
    d.main_thread_load = true;
    ASSERT_TRUE(DE_HANDLE_VALID(de_asset_type_register(f->s, &d)));
    static const char *const exts_i[] = {".pti"};
    d.name = "pti";
    d.extensions = exts_i;
    d.main_thread_load = false;
    d.import = pt_import;
    ASSERT_TRUE(DE_HANDLE_VALID(de_asset_type_register(f->s, &d)));
}

/* (Re)creates the asset system with `io_threads` and, if `with_tasks`, the fixture's tasks. */
static void fx_system(pipe_fx *f, u32 io_threads, bool with_tasks) {
    if (f->s != NULL)
        de_asset_system_destroy(f->s);
    f->s = NULL;
    de_asset_system_desc d = {0};
    d.allocator = &f->alloc;
    d.tasks = with_tasks ? f->tasks : NULL;
    d.io_threads = io_threads;
    ASSERT_EQ(de_asset_system_create(&d, &f->s), DE_OK);
    register_types(f);
}

static void pipe_fx_setup(pipe_fx *f) {
    mount_once();
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_ASSET);
    f->c.alloc = &f->alloc;
    de_atomic_init_u32(&f->c.loads, 0);
    de_atomic_init_u32(&f->c.unloads, 0);
    de_atomic_init_u32(&f->c.seq, 0);
    de_atomic_init_u32(&f->c.gate_closed, 0);
    de_atomic_init_u32(&f->c.gate_reached, 0);
    de_atomic_init_u64(&f->c.gate_id, 0);
    f->main_thread = de_thread_current_id();
    de_fp_env_init_thread();
    de_task_system_desc td = {.worker_count = 4, .background_worker_count = 2};
    ASSERT_EQ(de_task_system_create(&td, &f->tasks), DE_OK);
    fx_system(f, 2, true);
}

static de_atomic_u32 g_open_hold, g_open_reached; /* see hold_open */

static void pipe_fx_teardown(pipe_fx *f) {
    de__io_test_set_chunk_hook(NULL);
    de_atomic_store_u32(&g_open_hold, 0, DE_ATOMIC_RELEASE); /* a failed test cannot hang destroy */
    de_atomic_store_u32(&f->c.gate_closed, 0, DE_ATOMIC_RELEASE);
    de_asset_system_destroy(f->s);
    f->s = NULL;
    de_task_system_destroy(f->tasks);
    f->tasks = NULL;
    /* Every job, buffer, decoder, object and request is gone. */
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).bytes_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0u);
}

/* ---- Helpers -------------------------------------------------------------------------------- */

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

/* Parses `text` as pipe:/assets.toml and makes it the active manifest. */
static void install_manifest(de_asset_system *s, const char *text) {
    g_text = text;
    de__manifest *m = NULL;
    de__manifest_parse_desc d = {
        .path = "pipe:/assets.toml", .allocator = s->alloc, .read = text_read};
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

static void put_file(const char *name, const void *data, size_t size) {
    char path[256];
    snprintf(path, sizeof path, "pipe:/%s", name);
    ASSERT_EQ(de_file_write_all_atomic(path, data, size), DE_OK);
}

static void put_text(const char *name, const char *text) {
    put_file(name, text, strlen(text));
}

static de_asset_id id_of(const char *path) {
    return de_asset_id_from(path);
}

static de_load_state state_of(pipe_fx *f, const char *path) {
    return de_asset_status_of(f->s, id_of(path));
}

static u32 slot_of(pipe_fx *f, const char *path) {
    return de__slot_find(f->s, id_of(path));
}

static const pt_obj *obj_of(pipe_fx *f, const char *path) {
    u32 h = slot_of(f, path);
    return h != 0 ? (const pt_obj *)de__asset_object(f->s, h) : NULL;
}

static u32 asset_refs(pipe_fx *f, const char *path) {
    const de__slot *sl = de__slot_get(f->s, slot_of(f, path));
    u32 refs = 0;
    if (sl != NULL)
        de__slot_refs(sl, NULL, &refs);
    return refs;
}

static de__pipeline_stats stats(pipe_fx *f) {
    de__pipeline_stats st;
    de__pipeline_get_stats(f->s, &st);
    return st;
}

/* Updates until no job is left (cancelled jobs come back). */
static void drain(pipe_fx *f) {
    u64 end = de_time_now_ns() + WAIT_NS;
    while (stats(f).live_jobs != 0 && de_time_now_ns() < end) {
        de_asset_system_update(f->s, NULL);
        de_sleep_ns(100000u);
    }
    EXPECT_EQ(stats(f).live_jobs, 0u);
}

/* Updates until `counter` is at least `n`. */
static void wait_count(pipe_fx *f, de_atomic_u32 *counter, u32 n) {
    u64 end = de_time_now_ns() + WAIT_NS;
    while (de_atomic_load_u32(counter, DE_ATOMIC_ACQUIRE) < n && de_time_now_ns() < end) {
        de_asset_system_update(f->s, NULL);
        de_sleep_ns(100000u);
    }
    ASSERT_GE(de_atomic_load_u32(counter, DE_ATOMIC_ACQUIRE), n);
}

/* Updates until `flag` is non-zero. */
static void wait_flag(pipe_fx *f, de_atomic_u32 *flag) {
    wait_count(f, flag, 1);
}

/* Holds the OPEN stage: counts the opens that arrive (g_open_reached), then waits while
 * g_open_hold is set. A held job keeps its place in the OPEN/READ window (2 x io_threads), so
 * every later job stays QUEUED, and it reports no progress yet (DENG-380). */
static void hold_open(void) {
    (void)de_atomic_fetch_add_u32(&g_open_reached, 1, DE_ATOMIC_ACQ_REL);
    while (de_atomic_load_u32(&g_open_hold, DE_ATOMIC_ACQUIRE) != 0)
        de_sleep_ns(100000u);
}

/* A payload: header + body. */
static u8 *make_payload(de__payload_codec codec, const void *body, size_t body_size, u64 raw_size,
                        size_t *out_size) {
    u8 *p = (u8 *)malloc(DE__PAYLOAD_HEADER_SIZE + body_size);
    ASSERT_TRUE(p != NULL);
    de__payload_header_store(p, codec, raw_size, body_size);
    memcpy(p + DE__PAYLOAD_HEADER_SIZE, body, body_size);
    *out_size = DE__PAYLOAD_HEADER_SIZE + body_size;
    return p;
}

/* zstd with a checksum, so corrupted content is detected, not only corrupted structure. */
static void *zstd_frame(const void *src, size_t n, size_t *out) {
    size_t cap = ZSTD_compressBound(n);
    void *dst = malloc(cap);
    ZSTD_CCtx *cc = ZSTD_createCCtx();
    ASSERT_TRUE(dst != NULL && cc != NULL);
    ZSTD_CCtx_setParameter(cc, ZSTD_c_compressionLevel, 3);
    ZSTD_CCtx_setParameter(cc, ZSTD_c_checksumFlag, 1);
    size_t z = ZSTD_compress2(cc, dst, cap, src, n);
    ZSTD_freeCCtx(cc);
    ASSERT_FALSE(ZSTD_isError(z));
    *out = z;
    return dst;
}

/* ---- pipeline.single_asset ------------------------------------------------------------------ */

/* The default source behind hold_open. The OPEN stage reports the first progress (the size)
 * only after `open` returns, so a held load stays PENDING. Without the hold, a fast background
 * worker can make the request LOADING inside de_asset_load (DENG-380). */
static de_result gated_open(void *user, de_asset_system *s, const char *vpath, de_asset_id id,
                            de__source_span *out) {
    (void)user;
    hold_open();
    const de__pipeline_source *d = de__pipeline_default_source();
    return d->open(d->user, s, vpath, id, out);
}

TEST_F(pipeline, single_asset, pipe_fx) {
    put_text("one.bin", "hello world");
    put_text("one_ja.bin", "konnichiwa");
    put_text("empty.bin", "");
    static const char k_raw[] = "raw payload body";
    size_t raw_n = 0;
    u8 *raw = make_payload(DE__PAYLOAD_RAW, k_raw, sizeof k_raw - 1u, sizeof k_raw - 1u, &raw_n);
    put_file("raw.payload", raw, raw_n);
    free(raw);
    install_manifest(f->s,
                     "[assets]\n"
                     "\"one\" = { type = \"pt\", source = \"one.bin\", level = 3, ratio = 0.5, "
                     "flag = true, name = \"hi\", tags = [\"a\", \"b\"] }\n"
                     "\"loc\" = { type = \"pt\", source = \"one.bin\", locale = { ja = "
                     "\"one_ja.bin\" } }\n"
                     "\"empty\" = { type = \"pt\", source = \"empty.bin\" }\n"
                     "\"raw\" = { type = \"pt\", source = \"raw.payload\" }\n"
                     "\"missing\" = { type = \"pt\", source = \"nope.bin\" }\n"
                     "\"cooked\" = { type = \"pti\", source = \"one.bin\" }\n"
                     "\"untyped\" = { type = \"nobody\", source = \"one.bin\" }\n");
    de_asset_set_locale(f->s, "ja");
    de__pipeline_source src = *de__pipeline_default_source();
    src.open = gated_open;
    de__pipeline_set_source(f->s, &src);

    /* Submitted, no progress yet: PENDING, also across updates, until the open is let go. */
    de_atomic_store_u32(&g_open_hold, 1, DE_ATOMIC_RELEASE);
    de_load_handle h = de_asset_load(f->s, id_of("one"), 0);
    de_load_info info;
    EXPECT_EQ((int)de_asset_status(f->s, h, &info), (int)DE_LOAD_PENDING);
    EXPECT_EQ((int)state_of(f, "one"), (int)DE_LOAD_PENDING);
    de_asset_system_update(f->s, NULL);
    EXPECT_EQ((int)de_asset_status(f->s, h, &info), (int)DE_LOAD_PENDING);
    EXPECT_EQ((int)state_of(f, "one"), (int)DE_LOAD_PENDING);
    de_atomic_store_u32(&g_open_hold, 0, DE_ATOMIC_RELEASE);
    EXPECT_EQ((int)de_asset_wait(f->s, h, WAIT_NS), (int)DE_LOAD_READY);
    EXPECT_EQ((int)de_asset_status(f->s, h, &info), (int)DE_LOAD_READY);
    EXPECT_EQ(info.bytes_total, 11u); /* the file size, known after the size query */
    EXPECT_EQ(info.bytes_done, 11u);
    EXPECT_FLOAT_EQ(info.progress, 1.0f, 0.0f);
    const pt_obj *o = obj_of(f, "one");
    ASSERT_TRUE(o != NULL);
    EXPECT_EQ(o->size, 11u);
    EXPECT_MEMEQ(pt_bytes(o), "hello world", 11);
    EXPECT_NE(o->thread, f->main_thread); /* a frame worker loaded it */
    EXPECT_EQ(o->level, 3);
    EXPECT_FLOAT_EQ(o->ratio, 0.5, 0.0);
    EXPECT_TRUE(o->flag);
    EXPECT_STREQ(o->name, "hi");
    EXPECT_EQ(o->tag_count, 2u);
    const de__slot *sl = de__slot_get(f->s, slot_of(f, "one"));
    ASSERT_TRUE(sl != NULL);
    EXPECT_EQ(sl->bytes, sizeof(pt_obj) + 11u); /* the size the loader gave */
    EXPECT_EQ(sl->version, 1u);
    de_asset_release(f->s, h);

    /* Locale override, empty file, raw-codec payload. */
    const de_asset_id ok_ids[] = {id_of("loc"), id_of("empty"), id_of("raw")};
    h = de_asset_load_many(f->s, ok_ids, 3, 0);
    EXPECT_EQ((int)de_asset_wait(f->s, h, WAIT_NS), (int)DE_LOAD_READY);
    o = obj_of(f, "loc");
    ASSERT_TRUE(o != NULL);
    EXPECT_MEMEQ(pt_bytes(o), "konnichiwa", 10);
    o = obj_of(f, "empty");
    ASSERT_TRUE(o != NULL);
    EXPECT_EQ(o->size, 0u);
    o = obj_of(f, "raw");
    ASSERT_TRUE(o != NULL);
    EXPECT_EQ(o->size, sizeof k_raw - 1u);
    EXPECT_MEMEQ(pt_bytes(o), k_raw, sizeof k_raw - 1u);
    de_asset_release(f->s, h);

    /* Failures: each with its own code and message. */
    struct {
        const char *id;
        de_result code;
        const char *msg;
    } fails[] = {{"missing", DE_ERR_NOT_FOUND, "nope.bin"},
#if DE_HAS_COOKER
                 {"cooked", DE_ERR_PARSE, "the pti importer refuses"},
#else
                 {"cooked", DE_ERR_UNSUPPORTED, "cooker not available"},
#endif
                 {"untyped", DE_ERR_UNSUPPORTED, "no asset type \"nobody\""}};
    for (u32 i = 0; i < 3; ++i) {
        h = de_asset_load(f->s, id_of(fails[i].id), 0);
        EXPECT_EQ((int)de_asset_wait(f->s, h, WAIT_NS), (int)DE_LOAD_FAILED);
        (void)de_asset_status(f->s, h, &info);
        EXPECT_EQ((int)info.first_error, (int)fails[i].code);
        EXPECT_TRUE(strstr(info.first_error_msg, fails[i].msg) != NULL);
        if (strstr(info.first_error_msg, fails[i].msg) == NULL)
            printf("  %s: \"%s\"\n", fails[i].id, info.first_error_msg);
        de_asset_release(f->s, h);
    }
    EXPECT_EQ(stats(f).live_jobs, 0u);
}

/* ---- pipeline.dependency_chain -------------------------------------------------------------- */

TEST_F(pipeline, dependency_chain, pipe_fx) {
    put_text("root.bin", "deps:mid1,mid2\nroot");
    put_text("mid1.bin", "deps:leaf1\nmid1");
    put_text("mid2.bin", "deps:leaf2\nmid2");
    put_text("leaf1.bin", "leaf1");
    put_text("leaf2.bin", "leaf2");
    install_manifest(f->s, "[assets]\n"
                           "\"root\" = { type = \"pt\", source = \"root.bin\" }\n"
                           "\"mid1\" = { type = \"pt\", source = \"mid1.bin\" }\n"
                           "\"mid2\" = { type = \"pt\", source = \"mid2.bin\" }\n"
                           "\"leaf1\" = { type = \"pt\", source = \"leaf1.bin\" }\n"
                           "\"leaf2\" = { type = \"pt\", source = \"leaf2.bin\" }\n");
    de_load_handle h = de_asset_load(f->s, id_of("root"), DE_LOAD_PRIORITY_HIGH);
    u64 end = de_time_now_ns() + WAIT_NS;
    for (;;) {
        de_asset_system_update(f->s, NULL);
        bool root = state_of(f, "root") == DE_LOAD_READY;
        bool mid1 = state_of(f, "mid1") == DE_LOAD_READY;
        bool mid2 = state_of(f, "mid2") == DE_LOAD_READY;
        bool leaf1 = state_of(f, "leaf1") == DE_LOAD_READY;
        bool leaf2 = state_of(f, "leaf2") == DE_LOAD_READY;
        /* A parent is loaded (and waiting) before its children are even requested, so after
         * every update: parent READY <=> all its children READY. Never earlier, never later. */
        EXPECT_EQ(root, mid1 && mid2);
        EXPECT_EQ(mid1, leaf1);
        EXPECT_EQ(mid2, leaf2);
        EXPECT_EQ(de_asset_status(f->s, h, NULL) == DE_LOAD_READY, root);
        if (root || de_time_now_ns() > end)
            break;
        de_sleep_ns(50000u);
    }
    ASSERT_EQ((int)de_asset_status(f->s, h, NULL), (int)DE_LOAD_READY);
    /* Each child is held by its parent with one asset ref; the parent lists its dependencies. */
    EXPECT_EQ(asset_refs(f, "mid1"), 1u);
    EXPECT_EQ(asset_refs(f, "mid2"), 1u);
    EXPECT_EQ(asset_refs(f, "leaf1"), 1u);
    EXPECT_EQ(asset_refs(f, "root"), 0u);
    const de__slot *root = de__slot_get(f->s, slot_of(f, "root"));
    ASSERT_TRUE(root != NULL);
    ASSERT_EQ(root->dep_count, 2u);
    EXPECT_EQ(root->deps[0].hash, id_of("mid1").hash);
    EXPECT_EQ(root->deps[1].hash, id_of("mid2").hash);
    /* The internal dependency requests are released once their parent is published. */
    EXPECT_EQ(f->s->load->live_requests, 1u);
    de_asset_release(f->s, h);
    EXPECT_EQ(f->s->load->live_requests, 0u);
    EXPECT_EQ(stats(f).published, 5u);
}

/* ---- pipeline.failed_dependency ------------------------------------------------------------- */

TEST_F(pipeline, failed_dependency, pipe_fx) {
    put_text("froot.bin", "deps:fmid\nx");
    put_text("fmid.bin", "deps:fleaf\nx");
    put_text("fleaf.bin", "FAIL");
    put_text("fine.bin", "fine");
    put_text("uroot.bin", "deps:not/declared\nx");
    put_text("c1.bin", "deps:c2\nx");
    put_text("c2.bin", "deps:c1\nx");
    put_text("self.bin", "deps:self\nx");
    install_manifest(f->s, "[assets]\n"
                           "\"froot\" = { type = \"pt\", source = \"froot.bin\" }\n"
                           "\"fmid\" = { type = \"pt\", source = \"fmid.bin\" }\n"
                           "\"fleaf\" = { type = \"pt\", source = \"fleaf.bin\" }\n"
                           "\"fine\" = { type = \"pt\", source = \"fine.bin\" }\n"
                           "\"uroot\" = { type = \"pt\", source = \"uroot.bin\" }\n"
                           "\"c1\" = { type = \"pt\", source = \"c1.bin\" }\n"
                           "\"c2\" = { type = \"pt\", source = \"c2.bin\" }\n"
                           "\"self\" = { type = \"pt\", source = \"self.bin\" }\n");

    /* A failing leaf fails every ancestor, with the cause chain. */
    const de_asset_id ids[] = {id_of("froot"), id_of("fine")};
    de_load_handle h = de_asset_load_many(f->s, ids, 2, 0);
    EXPECT_EQ((int)de_asset_wait(f->s, h, WAIT_NS), (int)DE_LOAD_PARTIAL);
    de_load_info info;
    (void)de_asset_status(f->s, h, &info);
    EXPECT_EQ(info.first_failed.hash, id_of("froot").hash);
    EXPECT_EQ((int)info.first_error, (int)DE_ERR_PARSE); /* the leaf's code */
    EXPECT_TRUE(strstr(info.first_error_msg, "asset \"froot\": dependency \"fmid\" failed") !=
                NULL);
    EXPECT_TRUE(strstr(info.first_error_msg, "dependency \"fleaf\" failed") != NULL);
    EXPECT_TRUE(strstr(info.first_error_msg, "requested failure") != NULL);
    printf("  chain: %s\n", info.first_error_msg);
    EXPECT_EQ((int)state_of(f, "fmid"), (int)DE_LOAD_FAILED);
    EXPECT_EQ((int)state_of(f, "fleaf"), (int)DE_LOAD_FAILED);
    EXPECT_EQ((int)state_of(f, "fine"), (int)DE_LOAD_READY);
    /* froot's and fmid's objects were loaded, then given back. */
    EXPECT_EQ(de_atomic_load_u32(&f->c.unloads, DE_ATOMIC_ACQUIRE), 2u);
    EXPECT_TRUE(obj_of(f, "froot") == NULL && obj_of(f, "fmid") == NULL);
    /* No asset refs are left on failed dependencies. */
    EXPECT_EQ(asset_refs(f, "fmid"), 0u);
    EXPECT_EQ(asset_refs(f, "fleaf"), 0u);
    de_asset_release(f->s, h);

    /* Only the failing asset: FAILED. */
    h = de_asset_load(f->s, id_of("froot"), 0);
    EXPECT_EQ((int)de_asset_wait(f->s, h, WAIT_NS), (int)DE_LOAD_FAILED);
    de_asset_release(f->s, h);

    /* A dependency the manifest does not declare. */
    h = de_asset_load(f->s, id_of("uroot"), 0);
    EXPECT_EQ((int)de_asset_wait(f->s, h, WAIT_NS), (int)DE_LOAD_FAILED);
    (void)de_asset_status(f->s, h, &info);
    EXPECT_EQ((int)info.first_error, (int)DE_ERR_NOT_FOUND);
    EXPECT_TRUE(strstr(info.first_error_msg, "not in the asset manifest") != NULL);
    de_asset_release(f->s, h);

    /* Cycles found at run time: c1 -> c2 -> c1, and a self-dependency. */
    h = de_asset_load(f->s, id_of("c1"), 0);
    EXPECT_EQ((int)de_asset_wait(f->s, h, WAIT_NS), (int)DE_LOAD_FAILED);
    (void)de_asset_status(f->s, h, &info);
    EXPECT_TRUE(strstr(info.first_error_msg, "dependency cycle: ") != NULL);
    printf("  cycle: %s\n", info.first_error_msg);
    EXPECT_EQ((int)state_of(f, "c2"), (int)DE_LOAD_FAILED);
    de_asset_release(f->s, h);
    h = de_asset_load(f->s, id_of("self"), 0);
    EXPECT_EQ((int)de_asset_wait(f->s, h, WAIT_NS), (int)DE_LOAD_FAILED);
    (void)de_asset_status(f->s, h, &info);
    EXPECT_EQ((int)info.first_error, (int)DE_ERR_PARSE);
    EXPECT_TRUE(strstr(info.first_error_msg, "dependency cycle: self -> self") != NULL);
    de_asset_release(f->s, h);
    drain(f);
    EXPECT_EQ(f->s->load->live_requests, 0u);
}

/* ---- pipeline.zstd_payload ------------------------------------------------------------------ */

TEST_F(pipeline, zstd_payload, pipe_fx) {
    /* The checked-in fixture (format stability), copied next to the manifest. */
    void *fixture = NULL;
    u64 fixture_size = 0;
    ASSERT_EQ(de_file_read_all("pdata:/hello.zst.payload", NULL, &fixture, &fixture_size), DE_OK);
    put_file("hello.zst.payload", fixture, (size_t)fixture_size);
    DE_FREE(de_heap_allocator(), fixture, (size_t)fixture_size + 1u);

    /* A larger one made here: 1 MiB of compressible text. */
    size_t big_n = (size_t)1 << 20;
    char *big = (char *)malloc(big_n);
    ASSERT_TRUE(big != NULL);
    for (size_t i = 0; i < big_n; ++i)
        big[i] = (char)('a' + (char)((i * 7u + i / 1000u) % 26u));
    size_t z_n = 0;
    void *z = zstd_frame(big, big_n, &z_n);
    size_t pay_n = 0;
    u8 *pay = make_payload(DE__PAYLOAD_ZSTD, z, z_n, big_n, &pay_n);
    put_file("big.zst.payload", pay, pay_n);
    free(pay);
    free(z);
    EXPECT_LT(z_n, big_n / 4u);

    install_manifest(f->s, "[assets]\n"
                           "\"hello\" = { type = \"pt\", source = \"hello.zst.payload\" }\n"
                           "\"big\" = { type = \"pt\", source = \"big.zst.payload\" }\n");
    const de_asset_id ids[] = {id_of("hello"), id_of("big")};
    de_load_handle h = de_asset_load_many(f->s, ids, 2, 0);
    EXPECT_EQ((int)de_asset_wait(f->s, h, WAIT_NS), (int)DE_LOAD_READY);
    const pt_obj *o = obj_of(f, "hello");
    ASSERT_TRUE(o != NULL);
    char expect[1024];
    size_t n = 0;
    for (u32 i = 0; i < 24; ++i)
        n += (size_t)snprintf(expect + n, sizeof expect - n,
                              "DEngine zstd payload fixture (DENG-142). ");
    expect[n++] = '\n';
    ASSERT_EQ(o->size, (u64)n);
    EXPECT_MEMEQ(pt_bytes(o), expect, n);
    o = obj_of(f, "big");
    ASSERT_TRUE(o != NULL);
    ASSERT_EQ(o->size, (u64)big_n);
    EXPECT_MEMEQ(pt_bytes(o), big, big_n);
    EXPECT_EQ(stats(f).decoded, 2u);
    /* Progress counts the stored (compressed) bytes that were read. */
    de_load_info info;
    (void)de_asset_status(f->s, h, &info);
    EXPECT_EQ(info.bytes_total, fixture_size + (u64)pay_n);
    de_asset_release(f->s, h);
    free(big);
}

/* ---- pipeline.corrupt_zstd ------------------------------------------------------------------ */

TEST_F(pipeline, corrupt_zstd, pipe_fx) {
    size_t raw_n = 64u * 1024u;
    char *raw = (char *)malloc(raw_n);
    ASSERT_TRUE(raw != NULL);
    u32 x = 12345u;
    for (size_t i = 0; i < raw_n; ++i) {
        x = x * 1103515245u + 12345u;
        raw[i] = (char)('a' + (char)((x >> 16) % 8u));
    }
    size_t z_n = 0;
    u8 *z = (u8 *)zstd_frame(raw, raw_n, &z_n);
    size_t p_n = 0;

    /* Body bytes flipped (the frame checksum catches what the structure does not). */
    u8 *bad = (u8 *)malloc(z_n);
    memcpy(bad, z, z_n);
    for (size_t i = z_n / 3u; i < z_n / 3u + 16u && i < z_n; ++i)
        bad[i] ^= 0x5Au;
    u8 *pay = make_payload(DE__PAYLOAD_ZSTD, bad, z_n, raw_n, &p_n);
    put_file("flipped.payload", pay, p_n);
    free(pay);
    /* Not a zstd frame at all. */
    memcpy(bad, z, z_n);
    bad[0] ^= 0xFFu;
    pay = make_payload(DE__PAYLOAD_ZSTD, bad, z_n, raw_n, &p_n);
    put_file("magic.payload", pay, p_n);
    free(pay);
    /* The payload header disagrees with the frame. */
    pay = make_payload(DE__PAYLOAD_ZSTD, z, z_n, raw_n + 1u, &p_n);
    put_file("size.payload", pay, p_n);
    /* Truncated: stored_size no longer matches the file. */
    put_file("short.payload", pay, p_n - 10u);
    free(pay);
    /* Truncated frame body (header fixed up to match). */
    pay = make_payload(DE__PAYLOAD_ZSTD, z, z_n / 2u, raw_n, &p_n);
    put_file("half.payload", pay, p_n);
    free(pay);
    free(bad);
    free(z);
    free(raw);

    install_manifest(f->s, "[assets]\n"
                           "\"flipped\" = { type = \"pt\", source = \"flipped.payload\" }\n"
                           "\"magic\" = { type = \"pt\", source = \"magic.payload\" }\n"
                           "\"size\" = { type = \"pt\", source = \"size.payload\" }\n"
                           "\"short\" = { type = \"pt\", source = \"short.payload\" }\n"
                           "\"half\" = { type = \"pt\", source = \"half.payload\" }\n");
    const char *names[] = {"flipped", "magic", "size", "short", "half"};
    for (u32 i = 0; i < 5; ++i) {
        de_load_handle h = de_asset_load(f->s, id_of(names[i]), 0);
        EXPECT_EQ((int)de_asset_wait(f->s, h, WAIT_NS), (int)DE_LOAD_FAILED);
        de_load_info info;
        (void)de_asset_status(f->s, h, &info);
        EXPECT_EQ((int)info.first_error, (int)DE_ERR_PARSE);
        printf("  %s: %s\n", names[i], info.first_error_msg);
        de_asset_release(f->s, h);
    }
    EXPECT_EQ(de_atomic_load_u32(&f->c.loads, DE_ATOMIC_ACQUIRE), 0u); /* never reached `load` */
    EXPECT_EQ(stats(f).failed, 5u);
}

/* ---- pipeline.priority_order ---------------------------------------------------------------- */

static de_result slow_open(void *user, de_asset_system *s, const char *vpath, de_asset_id id,
                           de__source_span *out) {
    (void)user;
    hold_open();
    de_sleep_ns(1000000u); /* 1 ms per open: the queue drains slowly after the hold */
    const de__pipeline_source *d = de__pipeline_default_source();
    return d->open(d->user, s, vpath, id, out);
}

TEST_F(pipeline, priority_order, pipe_fx) {
    fx_system(f, 1, true); /* one I/O thread */
    de__pipeline_source src = *de__pipeline_default_source();
    src.open = slow_open;
    de__pipeline_set_source(f->s, &src);
    put_text("p.bin", "payload");
    char *text = (char *)malloc(16384);
    ASSERT_TRUE(text != NULL);
    size_t n = (size_t)snprintf(text, 16384, "[assets]\n");
    for (u32 i = 0; i < 100; ++i)
        n += (size_t)snprintf(text + n, 16384 - n,
                              "\"low/%u\" = { type = \"pt\", source = \"p.bin\" }\n", i);
    n +=
        (size_t)snprintf(text + n, 16384 - n, "\"crit\" = { type = \"pt\", source = \"p.bin\" }\n");
    install_manifest(f->s, text);
    free(text);

    de_asset_id low[100];
    char name[32];
    for (u32 i = 0; i < 100; ++i) {
        snprintf(name, sizeof name, "low/%u", i);
        low[i] = id_of(name);
    }
    /* The first LOW jobs fill the OPEN/READ window (2 x 1 I/O thread) and wait in `open`; the
     * other 98 stay QUEUED until the CRITICAL requests are in, however slow the runner is.
     * (Waiting for the first LOW load instead raced: on a slow ASan runner the main thread saw
     * it only after 99 LOW loads, with nothing left in the queue to overtake. DENG-380.) */
    const u32 in_flight = 2;
    de_atomic_store_u32(&g_open_reached, 0, DE_ATOMIC_RELAXED);
    de_atomic_store_u32(&g_open_hold, 1, DE_ATOMIC_RELEASE);
    de_load_handle hl = de_asset_load_many(f->s, low, 100, DE_LOAD_PRIORITY_LOW);
    wait_count(f, &g_open_reached, in_flight);
    EXPECT_EQ(de_atomic_load_u32(&g_open_reached, DE_ATOMIC_ACQUIRE), in_flight);
    u32 queued = 0;
    for (u32 i = 0; i < 100; ++i)
        queued += de__pipeline_debug_stage(f->s, de__slot_find(f->s, low[i])) == DE__PJOB_QUEUED;
    EXPECT_EQ(queued, 100u - in_flight);
    u32 started = de_atomic_load_u32(&f->c.seq, DE_ATOMIC_ACQUIRE);
    EXPECT_EQ(started, 0u);
    de_load_handle hc = de_asset_load(f->s, id_of("crit"), DE_LOAD_PRIORITY_CRITICAL);
    /* A LOW asset still queued, asked for again as CRITICAL: it moves up (reprioritize). */
    EXPECT_EQ(de__pipeline_debug_stage(f->s, slot_of(f, "low/99")), (u32)DE__PJOB_QUEUED);
    de_load_handle hr = de_asset_load(f->s, low[99], DE_LOAD_PRIORITY_CRITICAL);
    de_atomic_store_u32(&g_open_hold, 0, DE_ATOMIC_RELEASE);
    EXPECT_EQ((int)de_asset_wait(f->s, hc, WAIT_NS), (int)DE_LOAD_READY);
    EXPECT_EQ((int)de_asset_wait(f->s, hr, WAIT_NS), (int)DE_LOAD_READY);
    EXPECT_EQ((int)de_asset_wait(f->s, hl, WAIT_NS), (int)DE_LOAD_READY);

    u32 crit_seq = obj_of(f, "crit")->seq;
    u32 reprio_seq = obj_of(f, "low/99")->seq;
    u32 low_before = 0;
    for (u32 i = 0; i < 99; ++i) {
        snprintf(name, sizeof name, "low/%u", i);
        low_before += obj_of(f, name)->seq < crit_seq;
    }
    printf("  LOW loads started before CRITICAL was issued: %u; LOW loaded before it: %u of 99; "
           "CRITICAL seq %u, reprioritized LOW seq %u\n",
           started, low_before, crit_seq, reprio_seq);
    /* Only the LOW jobs held in the window, or taken from the queue after the CRITICAL ones and
     * finished while those were still between read and load, can finish first; the rest of the
     * queue waits. */
    EXPECT_LE(low_before, in_flight + 8u);
    EXPECT_LT(crit_seq, 30u);
    EXPECT_LT(reprio_seq, 30u);
    de_asset_release(f->s, hl);
    de_asset_release(f->s, hc);
    de_asset_release(f->s, hr);
}

/* ---- pipeline.cancel_io --------------------------------------------------------------------- */

static de_atomic_u32 g_hook_reached, g_hook_hold;

/* Holds every read of 2 MiB or more after its first chunk (smaller reads pass). */
static void hold_hook(de_io_request *req, u64 done) {
    (void)done;
    if (req->bytes < ((u64)2 << 20))
        return;
    de_atomic_store_u32(&g_hook_reached, 1, DE_ATOMIC_RELEASE);
    while (de_atomic_load_u32(&g_hook_hold, DE_ATOMIC_ACQUIRE) != 0)
        de_sleep_ns(100000u);
}

static void write_big(const char *name, size_t size) {
    u8 *b = (u8 *)calloc(size, 1);
    ASSERT_TRUE(b != NULL);
    put_file(name, b, size);
    free(b);
}

TEST_F(pipeline, cancel_io, pipe_fx) {
    fx_system(f, 1, true); /* one I/O thread: the second read waits in the I/O queue */
    write_big("big1.bin", (size_t)3 << 20);
    write_big("big2.bin", (size_t)3 << 20);
    install_manifest(f->s, "[assets]\n"
                           "\"big1\" = { type = \"pt\", source = \"big1.bin\" }\n"
                           "\"big2\" = { type = \"pt\", source = \"big2.bin\" }\n");
    de_atomic_store_u32(&g_hook_reached, 0, DE_ATOMIC_RELAXED);
    de_atomic_store_u32(&g_hook_hold, 1, DE_ATOMIC_RELEASE);
    de__io_test_set_chunk_hook(hold_hook);

    de_load_handle h1 = de_asset_load(f->s, id_of("big1"), DE_LOAD_PRIORITY_HIGH);
    wait_flag(f, &g_hook_reached); /* big1's read is past its first chunk, held */
    de_load_handle h2 = de_asset_load(f->s, id_of("big2"), 0);
    u64 end = de_time_now_ns() + WAIT_NS;
    while (de__pipeline_debug_stage(f->s, slot_of(f, "big2")) != DE__PJOB_READ &&
           de_time_now_ns() < end) {
        de_asset_system_update(f->s, NULL);
        de_sleep_ns(100000u);
    }
    EXPECT_EQ(de__pipeline_debug_stage(f->s, slot_of(f, "big1")), (u32)DE__PJOB_READ);
    EXPECT_EQ(de__pipeline_debug_stage(f->s, slot_of(f, "big2")), (u32)DE__PJOB_READ);

    /* big2's read is queued behind big1's: the cancel completes it at once. */
    de_asset_cancel(f->s, h2);
    EXPECT_EQ(stats(f).io_cancelled, 1u);
    /* big1's read is running: it stops at the next chunk boundary. */
    de_asset_cancel(f->s, h1);
    EXPECT_EQ((int)de_asset_status(f->s, h1, NULL), (int)DE_LOAD_CANCELLED);
    EXPECT_EQ((int)state_of(f, "big1"), (int)DE_LOAD_CANCELLED);
    de_atomic_store_u32(&g_hook_hold, 0, DE_ATOMIC_RELEASE);
    drain(f);
    de__io_test_set_chunk_hook(NULL);
    EXPECT_EQ(stats(f).io_cancelled, 2u);
    EXPECT_EQ(stats(f).discarded, 2u);
    EXPECT_EQ(de_atomic_load_u32(&f->c.loads, DE_ATOMIC_ACQUIRE), 0u);
    EXPECT_EQ((int)state_of(f, "big1"), (int)DE_LOAD_CANCELLED);
    EXPECT_EQ((int)state_of(f, "big2"), (int)DE_LOAD_CANCELLED);
    de_asset_release(f->s, h1);
    de_asset_release(f->s, h2);

    /* Loading them again works. */
    const de_asset_id ids[] = {id_of("big1"), id_of("big2")};
    de_load_handle h = de_asset_load_many(f->s, ids, 2, 0);
    EXPECT_EQ((int)de_asset_wait(f->s, h, WAIT_NS), (int)DE_LOAD_READY);
    EXPECT_EQ(obj_of(f, "big1")->size, (u64)3 << 20);
    de_asset_release(f->s, h);
}

/* ---- pipeline.cancel_load ------------------------------------------------------------------- */

TEST_F(pipeline, cancel_load, pipe_fx) {
    put_text("gated.bin", "gated");
    put_text("wroot.bin", "deps:wleaf\nwroot");
    put_text("wleaf.bin", "wleaf");
    install_manifest(f->s, "[assets]\n"
                           "\"gated\" = { type = \"pt\", source = \"gated.bin\" }\n"
                           "\"wroot\" = { type = \"pt\", source = \"wroot.bin\" }\n"
                           "\"wleaf\" = { type = \"pt\", source = \"wleaf.bin\" }\n");

    /* Cancelled while its loader runs: the loader finishes, its object is unloaded. */
    de_atomic_store_u64(&f->c.gate_id, id_of("gated").hash, DE_ATOMIC_RELEASE);
    de_atomic_store_u32(&f->c.gate_closed, 1, DE_ATOMIC_RELEASE);
    de_load_handle h = de_asset_load(f->s, id_of("gated"), 0);
    wait_flag(f, &f->c.gate_reached);
    EXPECT_EQ(de__pipeline_debug_stage(f->s, slot_of(f, "gated")), (u32)DE__PJOB_LOAD);
    de_asset_cancel(f->s, h);
    EXPECT_EQ((int)de_asset_status(f->s, h, NULL), (int)DE_LOAD_CANCELLED);
    de_atomic_store_u32(&f->c.gate_closed, 0, DE_ATOMIC_RELEASE);
    drain(f);
    EXPECT_EQ(de_atomic_load_u32(&f->c.loads, DE_ATOMIC_ACQUIRE), 1u);
    EXPECT_EQ(de_atomic_load_u32(&f->c.unloads, DE_ATOMIC_ACQUIRE), 1u);
    EXPECT_EQ((int)state_of(f, "gated"), (int)DE_LOAD_CANCELLED);
    EXPECT_TRUE(obj_of(f, "gated") == NULL);
    de_asset_release(f->s, h);
    /* Requested again: loads and publishes. */
    de_atomic_store_u64(&f->c.gate_id, 0, DE_ATOMIC_RELEASE);
    h = de_asset_load(f->s, id_of("gated"), 0);
    EXPECT_EQ((int)de_asset_wait(f->s, h, WAIT_NS), (int)DE_LOAD_READY);
    EXPECT_TRUE(obj_of(f, "gated") != NULL);
    de_asset_release(f->s, h);

    /* Cancelled while waiting for its dependency: both stop, both objects go. */
    de_atomic_store_u32(&f->c.gate_reached, 0, DE_ATOMIC_RELEASE);
    de_atomic_store_u64(&f->c.gate_id, id_of("wleaf").hash, DE_ATOMIC_RELEASE);
    de_atomic_store_u32(&f->c.gate_closed, 1, DE_ATOMIC_RELEASE);
    h = de_asset_load(f->s, id_of("wroot"), 0);
    wait_flag(f, &f->c.gate_reached); /* wleaf's loader runs, so wroot waits for it */
    EXPECT_EQ(de__pipeline_debug_stage(f->s, slot_of(f, "wroot")), (u32)DE__PJOB_WAIT_DEPS);
    EXPECT_EQ(asset_refs(f, "wleaf"), 1u);
    de_asset_cancel(f->s, h);
    EXPECT_EQ((int)state_of(f, "wroot"), (int)DE_LOAD_CANCELLED);
    de_asset_system_update(f->s, NULL); /* the waiting job is dropped; wleaf is cancelled */
    EXPECT_EQ((int)state_of(f, "wleaf"), (int)DE_LOAD_CANCELLED);
    de_atomic_store_u32(&f->c.gate_closed, 0, DE_ATOMIC_RELEASE);
    drain(f);
    EXPECT_EQ(de_atomic_load_u32(&f->c.unloads, DE_ATOMIC_ACQUIRE), 3u);
    EXPECT_EQ(asset_refs(f, "wleaf"), 0u);
    EXPECT_TRUE(obj_of(f, "wroot") == NULL && obj_of(f, "wleaf") == NULL);
    de_asset_release(f->s, h);
    EXPECT_EQ(f->s->load->live_requests, 0u);
}

/* ---- pipeline.main_thread_load -------------------------------------------------------------- */

TEST_F(pipeline, main_thread_load, pipe_fx) {
    put_text("m1.bin", "deps:w1\nmain");
    put_text("w1.bin", "worker");
    install_manifest(f->s, "[assets]\n"
                           "\"m1\" = { type = \"ptm\", source = \"m1.bin\" }\n"
                           "\"w1\" = { type = \"pt\", source = \"w1.bin\" }\n");
    de_load_handle h = de_asset_load(f->s, id_of("m1"), 0);
    EXPECT_EQ((int)de_asset_wait(f->s, h, WAIT_NS), (int)DE_LOAD_READY);
    EXPECT_EQ(obj_of(f, "m1")->thread, f->main_thread);
    EXPECT_NE(obj_of(f, "w1")->thread, f->main_thread);
    EXPECT_EQ(stats(f).main_loads, 1u);
    EXPECT_EQ(stats(f).loads, 2u);
    de_asset_release(f->s, h);
}

/* ---- pipeline.update_no_alloc --------------------------------------------------------------- */

TEST_F(pipeline, update_no_alloc, pipe_fx) {
    put_text("warm.bin", "deps:warm2\nwarm");
    put_text("warm2.bin", "warm2");
    put_text("slow.bin", "slow");
    write_big("held.bin", (size_t)2 << 20);
    install_manifest(f->s, "[assets]\n"
                           "\"warm\" = { type = \"pt\", source = \"warm.bin\" }\n"
                           "\"warm2\" = { type = \"pt\", source = \"warm2.bin\" }\n"
                           "\"slow\" = { type = \"pt\", source = \"slow.bin\" }\n"
                           "\"held\" = { type = \"pt\", source = \"held.bin\" }\n");
    /* Warm-up: a full load (queues, maps and the request table grow on first use). */
    de_load_handle hw = de_asset_load(f->s, id_of("warm"), 0);
    EXPECT_EQ((int)de_asset_wait(f->s, hw, WAIT_NS), (int)DE_LOAD_READY);

    /* A loader that does not finish, and a read held inside the I/O system. */
    de_atomic_store_u64(&f->c.gate_id, id_of("slow").hash, DE_ATOMIC_RELEASE);
    de_atomic_store_u32(&f->c.gate_closed, 1, DE_ATOMIC_RELEASE);
    de_atomic_store_u32(&g_hook_reached, 0, DE_ATOMIC_RELAXED);
    de_atomic_store_u32(&g_hook_hold, 1, DE_ATOMIC_RELEASE);
    de__io_test_set_chunk_hook(hold_hook);
    const de_asset_id ids[] = {id_of("slow"), id_of("held")};
    de_load_handle h = de_asset_load_many(f->s, ids, 2, 0);
    wait_flag(f, &f->c.gate_reached);
    wait_flag(f, &g_hook_reached);
    de_asset_system_update(f->s, NULL);

    de_alloc_guard g;
    memset(&g, 0, sizeof g);
    g.this_thread_only = true;
    u64 worst = 0;
    de_alloc_guard_begin(&g);
    for (u32 frame = 0; frame < 100; ++frame) {
        u64 t0 = de_time_now_ns();
        de_asset_system_update(f->s, NULL);
        u64 dt = de_time_now_ns() - t0;
        worst = dt > worst ? dt : worst;
        (void)de_asset_status(f->s, h, NULL);
    }
    de_alloc_guard_end(&g);
    DE_ALLOC_GUARD_ASSERT_ZERO(&g);
    printf("  slowest update while a read and a load are held: %llu ns\n",
           (unsigned long long)worst);
    EXPECT_LT(worst, (u64)20000000u); /* never waits for the held read or loader */
    de_load_state st = de_asset_status(f->s, h, NULL);
    EXPECT_TRUE(st == DE_LOAD_PENDING || st == DE_LOAD_LOADING);

    de_atomic_store_u32(&f->c.gate_closed, 0, DE_ATOMIC_RELEASE);
    de_atomic_store_u32(&g_hook_hold, 0, DE_ATOMIC_RELEASE);
    EXPECT_EQ((int)de_asset_wait(f->s, h, WAIT_NS), (int)DE_LOAD_READY);
    de__io_test_set_chunk_hook(NULL);
    de_asset_release(f->s, h);
    de_asset_release(f->s, hw);
}

/* ---- pipeline.no_task_system ---------------------------------------------------------------- */

TEST_F(pipeline, no_task_system, pipe_fx) {
    fx_system(f, 2, false); /* every stage runs inline on the I/O (or calling) thread */
    put_text("nroot.bin", "deps:nleaf\nnroot");
    put_text("nleaf.bin", "nleaf");
    static const char k_text[] = "inline zstd inline zstd inline zstd inline zstd";
    size_t z_n = 0;
    void *z = zstd_frame(k_text, sizeof k_text - 1u, &z_n);
    size_t p_n = 0;
    u8 *pay = make_payload(DE__PAYLOAD_ZSTD, z, z_n, sizeof k_text - 1u, &p_n);
    put_file("nz.payload", pay, p_n);
    free(pay);
    free(z);
    install_manifest(f->s, "[assets]\n"
                           "\"nroot\" = { type = \"pt\", source = \"nroot.bin\" }\n"
                           "\"nleaf\" = { type = \"pt\", source = \"nleaf.bin\" }\n"
                           "\"nz\" = { type = \"pt\", source = \"nz.payload\" }\n"
                           "\"nm\" = { type = \"ptm\", source = \"nleaf.bin\" }\n");
    const de_asset_id ids[] = {id_of("nroot"), id_of("nz"), id_of("nm")};
    de_load_handle h = de_asset_load_many(f->s, ids, 3, 0);
    EXPECT_EQ((int)de_asset_wait(f->s, h, WAIT_NS), (int)DE_LOAD_READY);
    EXPECT_MEMEQ(pt_bytes(obj_of(f, "nz")), k_text, sizeof k_text - 1u);
    EXPECT_EQ(obj_of(f, "nm")->thread, f->main_thread);
    EXPECT_EQ((int)state_of(f, "nleaf"), (int)DE_LOAD_READY);
    de_asset_release(f->s, h);
}

/* ---- pipeline.stress_10k -------------------------------------------------------------------- */

#define STRESS_ASSETS 10000u
#define STRESS_FILES  16u

TEST_F(pipeline, stress_10k, pipe_fx) {
    ASSERT_EQ(de_dir_create("pipe:/s"), DE_OK);
    u64 file_size[STRESS_FILES];
    for (u32 i = 0; i < STRESS_FILES; ++i) {
        char name[32], body[256];
        snprintf(name, sizeof name, "s/f%u.bin", i);
        u32 n = 32u + i * 11u;
        for (u32 k = 0; k < n; ++k)
            body[k] = (char)('A' + (char)((i + k) % 26u));
        put_file(name, body, n);
        file_size[i] = n;
    }
    size_t cap = (size_t)STRESS_ASSETS * 96u + 64u;
    char *text = (char *)malloc(cap);
    ASSERT_TRUE(text != NULL);
    size_t n = (size_t)snprintf(text, cap, "[assets]\n");
    for (u32 i = 0; i < STRESS_ASSETS; ++i) {
        if (i % 10u == 9u)
            n += (size_t)snprintf(text + n, cap - n,
                                  "\"s/%u\" = { type = \"pt\", source = \"s/f%u.bin\", deps = "
                                  "[\"s/%u\", \"s/%u\"] }\n",
                                  i, i % STRESS_FILES, i - 1u, i - 9u);
        else
            n += (size_t)snprintf(text + n, cap - n,
                                  "\"s/%u\" = { type = \"pt\", source = \"s/f%u.bin\" }\n", i,
                                  i % STRESS_FILES);
    }
    install_manifest(f->s, text);
    free(text);

    de_asset_id *ids = (de_asset_id *)malloc(sizeof(de_asset_id) * STRESS_ASSETS);
    ASSERT_TRUE(ids != NULL);
    for (u32 i = 0; i < STRESS_ASSETS; ++i) {
        char name[32];
        snprintf(name, sizeof name, "s/%u", i);
        ids[i] = id_of(name);
    }
    u64 t0 = de_time_now_ns();
    static const u32 k_flags[4] = {DE_LOAD_PRIORITY_LOW, 0, DE_LOAD_PRIORITY_HIGH,
                                   DE_LOAD_PRIORITY_BACKGROUND};
    de_load_handle h[4];
    u32 quarter = STRESS_ASSETS / 4u;
    for (u32 q = 0; q < 4; ++q)
        h[q] = de_asset_load_many(f->s, ids + q * quarter, quarter, k_flags[q]);
    /* A request over part of the third quarter, cancelled and released while loads run: those
     * assets keep loading for request 2. */
    de_load_handle hx =
        de_asset_load_many(f->s, ids + 2u * quarter, 1000, DE_LOAD_PRIORITY_CRITICAL);
    for (u32 i = 0; i < 3; ++i) {
        de_asset_system_update(f->s, NULL);
        de_sleep_ns(200000u);
    }
    de_asset_cancel(f->s, hx);
    de_asset_release(f->s, hx);
    for (u32 q = 0; q < 4; ++q)
        EXPECT_EQ((int)de_asset_wait(f->s, h[q], WAIT_NS * 4u), (int)DE_LOAD_READY);
    u64 t1 = de_time_now_ns();

    u32 bad = 0;
    for (u32 i = 0; i < STRESS_ASSETS; ++i) {
        u32 hs = de__slot_find(f->s, ids[i]);
        const pt_obj *o = hs != 0 ? (const pt_obj *)de__asset_object(f->s, hs) : NULL;
        bad += o == NULL || o->size != file_size[i % STRESS_FILES] ||
               pt_bytes(o)[0] != (u8)('A' + (char)((i % STRESS_FILES) % 26u));
    }
    EXPECT_EQ(bad, 0u);
    EXPECT_EQ(stats(f).published, (u64)STRESS_ASSETS);
    EXPECT_EQ(asset_refs(f, "s/8"), 1u); /* held by s/9 */
    EXPECT_EQ(asset_refs(f, "s/0"), 1u); /* held by s/9 */
    const de__slot *s9 = de__slot_get(f->s, slot_of(f, "s/9"));
    ASSERT_TRUE(s9 != NULL);
    EXPECT_EQ(s9->dep_count, 2u);
    printf("  %u assets: %.1f ms, %llu loads, %llu reads\n", STRESS_ASSETS, (double)(t1 - t0) / 1e6,
           (unsigned long long)stats(f).loads, (unsigned long long)stats(f).reads);
    for (u32 q = 0; q < 4; ++q)
        de_asset_release(f->s, h[q]);
    drain(f);
    EXPECT_EQ(f->s->load->live_requests, 0u);
    free(ids);
}
