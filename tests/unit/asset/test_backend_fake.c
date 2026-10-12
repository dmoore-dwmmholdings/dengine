/*
 * test_backend_fake.c: the scripted fake load backend (fake_backend.h) and the tests of the
 * backend seam itself (DENG-141): what reaches the backend, how completions and progress from
 * other threads are applied, and what happens without a backend.
 */
#include "de_test.h"
#include "fake_backend.h"
#include "load_test_util.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "asset/asset_internal.h"
#include "asset/backend_internal.h"
#include "asset/load_internal.h"
#include "asset/manifest_internal.h"
#include "asset/resolve_internal.h"
#include "asset/slots_internal.h"
#include "platform/de_atomic.h"

#include <stdio.h>
#include <string.h>

/* ---- The fake backend ---------------------------------------------------------------------- */

static void fake_submit(void *user, de_asset_system *s, u32 slot, de__load_priority priority) {
    fake_backend *f = (fake_backend *)user;
    ASSERT_LT(f->count, FAKE_MAX_ITEMS);
    const de__slot *sl = de__slot_get(s, slot);
    ASSERT_TRUE(sl != NULL);
    /* The seam's promise: never submitted twice without a completion or cancel in between. */
    for (u32 i = 0; i < f->count; ++i)
        if (f->items[i].slot == slot)
            EXPECT_TRUE(f->items[i].done || f->items[i].cancelled);
    fake_item *it = &f->items[f->count++];
    memset(it, 0, sizeof *it);
    it->slot = slot;
    it->id = sl->id;
    it->priority = priority;
    f->submits++;
}

static fake_item *fake_open_item(fake_backend *f, u32 slot) {
    for (u32 i = f->count; i-- > 0;)
        if (f->items[i].slot == slot && !f->items[i].done && !f->items[i].cancelled)
            return &f->items[i];
    return NULL;
}

static void fake_cancel(void *user, de_asset_system *s, u32 slot) {
    (void)s;
    fake_backend *f = (fake_backend *)user;
    fake_item *it = NULL;
    for (u32 i = f->count; i-- > 0 && it == NULL;)
        if (f->items[i].slot == slot)
            it = &f->items[i];
    /* Only submitted, not cancelled work is cancelled. It may already be done: a completion
     * queued but not applied yet races with the cancel (the seam allows that). */
    EXPECT_TRUE(it != NULL && !it->cancelled);
    if (it != NULL && !it->done)
        it->cancelled = true;
    f->cancels++;
}

static void fake_reprioritize(void *user, de_asset_system *s, u32 slot,
                              de__load_priority priority) {
    (void)s;
    fake_backend *f = (fake_backend *)user;
    fake_item *it = fake_open_item(f, slot);
    EXPECT_TRUE(it != NULL);
    if (it != NULL)
        it->priority = priority;
    f->reprioritizes++;
}

void fake_backend_install(fake_backend *f, de_asset_system *s) {
    memset(f, 0, sizeof *f);
    f->s = s;
    f->asset_bytes = 1000;
    de__load_backend b = {
        .submit = fake_submit, .cancel = fake_cancel, .reprioritize = fake_reprioritize, .user = f};
    de__asset_set_backend(s, &b);
}

void fake_backend_fail(fake_backend *f, de_asset_id id, de_result code) {
    ASSERT_LT(f->fail_count, FAKE_MAX_FAILS);
    f->fail_ids[f->fail_count] = id;
    f->fail_codes[f->fail_count] = code;
    f->fail_count++;
}

fake_item *fake_backend_find(fake_backend *f, de_asset_id id) {
    for (u32 i = f->count; i-- > 0;)
        if (f->items[i].id.hash == id.hash)
            return &f->items[i];
    return NULL;
}

u32 fake_backend_outstanding(const fake_backend *f) {
    u32 n = 0;
    for (u32 i = 0; i < f->count; ++i)
        n += !f->items[i].done && !f->items[i].cancelled;
    return n;
}

void fake_backend_progress(fake_backend *f, u32 num, u32 den) {
    for (u32 i = 0; i < f->count; ++i) {
        const fake_item *it = &f->items[i];
        if (!it->done && !it->cancelled)
            de__slot_progress(f->s, it->slot, f->asset_bytes * num / den, f->asset_bytes);
    }
}

static void fake_finish(fake_backend *f, fake_item *it) {
    it->done = true;
    for (u32 k = 0; k < f->fail_count; ++k) {
        if (f->fail_ids[k].hash == it->id.hash) {
            const char *path = de_asset_id_path(f->s, it->id);
            de_result code = DE_FAIL(f->fail_codes[k], "fake backend: cannot load \"%s\"",
                                     path != NULL ? path : "?");
            de__slot_complete(f->s, it->slot, code, 0);
            return;
        }
    }
    de__slot_progress(f->s, it->slot, f->asset_bytes, f->asset_bytes);
    de__slot_complete(f->s, it->slot, DE_OK, f->asset_bytes);
}

u32 fake_backend_complete(fake_backend *f, u32 max) {
    u32 done = 0;
    for (u32 p = 0; p < DE__LOAD_PRIORITY_COUNT && done < max; ++p) {
        for (u32 i = 0; i < f->count && done < max; ++i) {
            fake_item *it = &f->items[i];
            if (it->done || it->cancelled || (u32)it->priority != p)
                continue;
            fake_finish(f, it);
            done++;
        }
    }
    return done;
}

bool fake_backend_complete_id(fake_backend *f, de_asset_id id) {
    for (u32 i = 0; i < f->count; ++i) {
        fake_item *it = &f->items[i];
        if (it->id.hash == id.hash && !it->done && !it->cancelled) {
            fake_finish(f, it);
            return true;
        }
    }
    return false;
}

/* ---- Shared test helpers (load_test_util.h) ------------------------------------------------- */

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

static void install(de_asset_system *s, de__manifest *m) {
    ASSERT_EQ(m->error_count, 0u);
    de__resolved *r = NULL;
    ASSERT_EQ(de__manifest_resolve(m, &(de__resolve_desc){.allocator = s->alloc}, &r), DE_OK);
    ASSERT_EQ(r->error_count, 0u);
    de__resolved_destroy(s->resolved);
    de__manifest_destroy(s->manifest);
    s->manifest = m;
    s->resolved = r;
}

void load_test_install_text(de_asset_system *s, const char *text) {
    g_text = text;
    de__manifest *m = NULL;
    de__manifest_parse_desc d = {
        .path = "mem:/assets.toml", .allocator = s->alloc, .read = one_file_read};
    ASSERT_EQ(de__manifest_parse(&d, &m), DE_OK);
    install(s, m);
}

static void mount_once(void) {
    static bool mounted;
    if (mounted)
        return;
    ASSERT_EQ(de_fs_mount("ldata", DE_ASSET_TEST_DATA_DIR, DE_MOUNT_READ_ONLY, 0), DE_OK);
    ASSERT_EQ(de_fs_mount("lex", DE_ASSET_EXAMPLES_DIR, DE_MOUNT_READ_ONLY, 0), DE_OK);
    mounted = true;
}

/* lex:/ (examples/), falling back to ldata:/example/ for the includes the repo does not have. */
static de_result example_read(void *user, const char *vpath, const de_allocator *a, void **data,
                              u64 *size) {
    (void)user;
    de_result r = de_file_read_all(vpath, a, data, size);
    if (r == DE_ERR_NOT_FOUND && strncmp(vpath, "lex:/", 5) == 0) {
        char alt[DE_FS_PATH_MAX];
        snprintf(alt, sizeof alt, "ldata:/example/%s", vpath + 5);
        r = de_file_read_all(alt, a, data, size);
    }
    return r;
}

void load_test_install_example(de_asset_system *s) {
    mount_once();
    de__manifest *m = NULL;
    de__manifest_parse_desc d = {
        .path = "lex:/assets.toml", .allocator = s->alloc, .read = example_read};
    ASSERT_EQ(de__manifest_parse(&d, &m), DE_OK);
    install(s, m);
}

void load_fx_setup(load_fx *f) {
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_ASSET);
    ASSERT_EQ(de_asset_system_create(&(de_asset_system_desc){.allocator = &f->alloc}, &f->s),
              DE_OK);
    fake_backend_install(&f->fake, f->s);
}

void load_fx_teardown(load_fx *f) {
    de_asset_system_destroy(f->s);
    f->s = NULL;
    /* Requests, per-slot load state, queues: all gone. */
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).bytes_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0u);
}

u32 load_test_handle_refs(de_asset_system *s, de_asset_id id) {
    u32 h = de__slot_find(s, id);
    const de__slot *sl = h != 0 ? de__slot_get(s, h) : NULL;
    u32 refs = 0;
    if (sl != NULL)
        de__slot_refs(sl, &refs, NULL);
    return refs;
}

const char k_load_manifest[] = "[assets]\n"
                               "\"data/a\" = { type = \"test_bytes\", source = \"a.bytes\" }\n"
                               "\"data/b\" = { type = \"test_bytes\", source = \"b.bytes\" }\n"
                               "\"data/c\" = { type = \"test_bytes\", source = \"c.bytes\" }\n"
                               "\"data/d\" = { type = \"test_bytes\", source = \"d.bytes\" }\n"
                               "\"data/e\" = { type = \"test_bytes\", source = \"e.bytes\" }\n"
                               "\"more/x\" = { type = \"test_bytes\", source = \"x.bytes\" }\n"
                               "\"more/y\" = { type = \"test_bytes\", source = \"y.bytes\" }\n"
                               "[bundles.data]\n"
                               "assets = [\"data/*\"]\n"
                               "[bundles.more]\n"
                               "assets = [\"more/*\"]\n"
                               "depends = [\"data\"]\n"
                               "priority = \"low\"\n"
                               "[bundles.empty]\n"
                               "assets = []\n";

/* ---- backend.* ------------------------------------------------------------------------------ */

static de_asset_id id_of(const char *path) {
    return de_asset_id_from(path);
}

TEST_F(backend, fake_records_calls, load_fx) {
    load_test_install_text(f->s, k_load_manifest);
    de_load_handle h = de_asset_load(f->s, id_of("data/a"), DE_LOAD_PRIORITY_HIGH);
    ASSERT_TRUE(DE_HANDLE_VALID(h));
    EXPECT_EQ(f->fake.submits, 1u);
    fake_item *it = fake_backend_find(&f->fake, id_of("data/a"));
    ASSERT_TRUE(it != NULL);
    EXPECT_EQ((int)it->priority, (int)DE__LOAD_PRIORITY_HIGH);
    EXPECT_EQ(it->slot, de__slot_find(f->s, id_of("data/a")));
    EXPECT_EQ(fake_backend_outstanding(&f->fake), 1u);
    EXPECT_EQ(fake_backend_complete(&f->fake, 10), 1u);
    EXPECT_EQ(fake_backend_outstanding(&f->fake), 0u);
    /* Queued, not applied: still PENDING until the update. */
    EXPECT_EQ((int)de_asset_status(f->s, h, NULL), (int)DE_LOAD_PENDING);
    de_asset_system_update(f->s, NULL);
    EXPECT_EQ((int)de_asset_status(f->s, h, NULL), (int)DE_LOAD_READY);
    /* A second request for a READY asset submits nothing. */
    de_load_handle h2 = de_asset_load(f->s, id_of("data/a"), 0);
    EXPECT_EQ((int)de_asset_status(f->s, h2, NULL), (int)DE_LOAD_READY);
    EXPECT_EQ(f->fake.submits, 1u);
    de_asset_release(f->s, h);
    de_asset_release(f->s, h2);
}

TEST_F(backend, complete_priority_order, load_fx) {
    load_test_install_text(f->s, k_load_manifest);
    de_load_handle lo = de_asset_load(f->s, id_of("data/a"), DE_LOAD_PRIORITY_BACKGROUND);
    de_load_handle no = de_asset_load(f->s, id_of("data/b"), 0);
    de_load_handle cr = de_asset_load(f->s, id_of("data/c"), DE_LOAD_PRIORITY_CRITICAL);
    EXPECT_EQ(fake_backend_complete(&f->fake, 1), 1u);
    EXPECT_TRUE(fake_backend_find(&f->fake, id_of("data/c"))->done);
    EXPECT_FALSE(fake_backend_find(&f->fake, id_of("data/b"))->done);
    EXPECT_EQ(fake_backend_complete(&f->fake, 1), 1u);
    EXPECT_TRUE(fake_backend_find(&f->fake, id_of("data/b"))->done);
    EXPECT_FALSE(fake_backend_find(&f->fake, id_of("data/a"))->done);
    de_asset_release(f->s, lo);
    de_asset_release(f->s, no);
    de_asset_release(f->s, cr);
}

TEST_F(backend, no_backend_fails_unsupported, load_fx) {
    load_test_install_text(f->s, k_load_manifest);
    de__asset_set_backend(f->s, NULL);
    EXPECT_TRUE(de__asset_backend(f->s) == NULL);
    de_load_handle h = de_asset_load(f->s, id_of("data/a"), 0);
    ASSERT_TRUE(DE_HANDLE_VALID(h));
    de_load_info info;
    /* Fails at once: nothing would ever complete it. */
    EXPECT_EQ((int)de_asset_status(f->s, h, &info), (int)DE_LOAD_FAILED);
    EXPECT_EQ((int)info.first_error, (int)DE_ERR_UNSUPPORTED);
    EXPECT_TRUE(strstr(info.first_error_msg, "no asset load backend") != NULL);
    EXPECT_EQ((int)de_asset_status_of(f->s, id_of("data/a")), (int)DE_LOAD_FAILED);
    de_asset_release(f->s, h);
    /* With a backend again, a new request retries the failed asset. */
    fake_backend_install(&f->fake, f->s);
    h = de_asset_load(f->s, id_of("data/a"), 0);
    EXPECT_EQ(f->fake.submits, 1u);
    EXPECT_EQ((int)de_asset_status(f->s, h, NULL), (int)DE_LOAD_PENDING);
    de_asset_release(f->s, h);
}

TEST_F(backend, error_message_from_context, load_fx) {
    load_test_install_text(f->s, k_load_manifest);
    const de_asset_id ids[] = {id_of("data/a"), id_of("data/b"), id_of("data/c"), id_of("data/d"),
                               id_of("data/e")};
    de_load_handle h = de_asset_load_many(f->s, ids, 5, 0);
    u32 a = de__slot_find(f->s, id_of("data/a"));
    u32 b = de__slot_find(f->s, id_of("data/b"));
    /* The error context matches the result: its message is kept. */
    (void)DE_FAIL(DE_ERR_IO, "disk on fire while reading a.bytes");
    de__slot_complete(f->s, a, DE_ERR_IO, 0);
    /* It does not match (DE_ERR_PARSE): a message is generated from the path and code. */
    (void)DE_FAIL(DE_ERR_IO, "unrelated");
    de__slot_complete(f->s, b, DE_ERR_PARSE, 0);
    fake_backend_find(&f->fake, id_of("data/a"))->done = true;
    fake_backend_find(&f->fake, id_of("data/b"))->done = true;
    fake_backend_complete(&f->fake, 100);
    de_asset_system_update(f->s, NULL);
    de_load_info info;
    EXPECT_EQ((int)de_asset_status(f->s, h, &info), (int)DE_LOAD_PARTIAL);
    EXPECT_EQ(info.first_failed.hash, id_of("data/a").hash);
    EXPECT_EQ((int)info.first_error, (int)DE_ERR_IO);
    EXPECT_STREQ(info.first_error_msg, "disk on fire while reading a.bytes");
    de_asset_release(f->s, h);

    /* Only data/b failed now (a is retried and succeeds): the generated message. */
    h = de_asset_load_many(f->s, (de_asset_id[]){id_of("data/b"), id_of("data/a")}, 2, 0);
    de__slot_complete(f->s, b, DE_ERR_PARSE, 0);
    fake_backend_find(&f->fake, id_of("data/b"))->done = true;
    fake_backend_complete(&f->fake, 100);
    de_asset_system_update(f->s, NULL);
    EXPECT_EQ((int)de_asset_status(f->s, h, &info), (int)DE_LOAD_PARTIAL);
    EXPECT_EQ(info.first_failed.hash, id_of("data/b").hash);
    EXPECT_EQ((int)info.first_error, (int)DE_ERR_PARSE);
    EXPECT_TRUE(strstr(info.first_error_msg, "\"data/b\"") != NULL);
    EXPECT_TRUE(strstr(info.first_error_msg, "DE_ERR_PARSE") != NULL);
    de_asset_release(f->s, h);
}

TEST_F(backend, late_completions, load_fx) {
    load_test_install_text(f->s, k_load_manifest);
    de_load_handle h =
        de_asset_load_many(f->s, (de_asset_id[]){id_of("data/a"), id_of("data/b")}, 2, 0);
    u32 a = de__slot_find(f->s, id_of("data/a"));
    u32 b = de__slot_find(f->s, id_of("data/b"));
    de_asset_cancel(f->s, h);
    EXPECT_EQ(f->fake.cancels, 2u);
    /* The work raced past the cancel: a failure is ignored, a success still loads the asset. */
    de__slot_complete(f->s, a, DE_ERR_IO, 0);
    de__slot_complete(f->s, b, DE_OK, 77);
    /* Stale and zero handles are ignored. */
    de__slot_complete(f->s, 0, DE_OK, 1);
    de__slot_complete(f->s, a ^ (1u << DE__SLOT_INDEX_BITS), DE_OK, 1);
    de__slot_progress(f->s, 0, 1, 1);
    de_asset_system_update(f->s, NULL);
    EXPECT_EQ((int)de_asset_status(f->s, h, NULL), (int)DE_LOAD_CANCELLED);
    EXPECT_EQ((int)de_asset_status_of(f->s, id_of("data/a")), (int)DE_LOAD_CANCELLED);
    EXPECT_EQ((int)de_asset_status_of(f->s, id_of("data/b")), (int)DE_LOAD_READY);
    EXPECT_EQ(de__slot_get(f->s, b)->bytes, 77u);
    EXPECT_EQ(de__slot_get(f->s, b)->version, 1u);
    de_asset_release(f->s, h);

    /* A failure queued before a cancel does not end the next submission of the slot. */
    h = de_asset_load(f->s, id_of("data/c"), 0);
    u32 c = de__slot_find(f->s, id_of("data/c"));
    de__slot_complete(f->s, c, DE_ERR_IO, 0);
    fake_backend_find(&f->fake, id_of("data/c"))->done = true;
    de_asset_release(f->s, h); /* cancels: the queued failure is dropped */
    h = de_asset_load(f->s, id_of("data/c"), 0);
    de_asset_system_update(f->s, NULL);
    EXPECT_EQ((int)de_asset_status(f->s, h, NULL), (int)DE_LOAD_PENDING);
    EXPECT_EQ(fake_backend_complete(&f->fake, 100), 1u);
    de_asset_system_update(f->s, NULL);
    EXPECT_EQ((int)de_asset_status(f->s, h, NULL), (int)DE_LOAD_READY);
    de_asset_release(f->s, h);
}

/* Completions and progress from 8 threads while the main thread updates (TSan). */
#define CONC_THREADS 8u
#define CONC_ASSETS  64u

typedef struct conc_ctx {
    de_asset_system *s;
    u32 slots[CONC_ASSETS];
    u32 first, step;
} conc_ctx;

typedef struct conc_arg {
    conc_ctx *ctx;
    u32 first;
} conc_arg;

static i32 conc_thread(void *p) {
    conc_arg *a = (conc_arg *)p;
    for (u32 i = a->first; i < CONC_ASSETS; i += CONC_THREADS) {
        for (u32 k = 1; k <= 4; ++k)
            de__slot_progress(a->ctx->s, a->ctx->slots[i], 250u * k, 1000u);
        de__slot_complete(a->ctx->s, a->ctx->slots[i], DE_OK, 1000u);
    }
    return 0;
}

TEST_F(backend, concurrent_completions, load_fx) {
    /* A manifest of CONC_ASSETS assets in one bundle. */
    static char text[CONC_ASSETS * 64 + 128];
    size_t n = (size_t)snprintf(text, sizeof text, "[assets]\n");
    for (u32 i = 0; i < CONC_ASSETS; ++i)
        n += (size_t)snprintf(text + n, sizeof text - n,
                              "\"c/%02u\" = { type = \"test_bytes\", source = \"c.bytes\" }\n", i);
    snprintf(text + n, sizeof text - n, "[bundles.c]\nassets = [\"c/*\"]\n");
    load_test_install_text(f->s, text);

    de_load_handle h = de_asset_load_bundle(f->s, "c", 0);
    conc_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.s = f->s;
    for (u32 i = 0; i < CONC_ASSETS; ++i) {
        char path[16];
        snprintf(path, sizeof path, "c/%02u", i);
        ctx.slots[i] = de__slot_find(f->s, id_of(path));
        ASSERT_NE(ctx.slots[i], 0u);
    }
    conc_arg args[CONC_THREADS];
    de_thread *t[CONC_THREADS];
    for (u32 i = 0; i < CONC_THREADS; ++i) {
        args[i].ctx = &ctx;
        args[i].first = i;
        ASSERT_EQ(de_thread_create(conc_thread, &args[i], "conc", DE_THREAD_NORMAL, 0, &t[i]),
                  DE_OK);
    }
    f32 last = 0.0f;
    de_load_info info;
    while (de_asset_status(f->s, h, &info) < DE_LOAD_READY) {
        de_asset_system_update(f->s, NULL);
        EXPECT_TRUE(info.progress >= last);
        last = info.progress;
    }
    for (u32 i = 0; i < CONC_THREADS; ++i)
        (void)de_thread_join(t[i]);
    de_asset_system_update(f->s, NULL);
    EXPECT_EQ((int)de_asset_status(f->s, h, &info), (int)DE_LOAD_READY);
    EXPECT_EQ(info.assets_ready, CONC_ASSETS);
    EXPECT_EQ(info.bytes_done, (u64)CONC_ASSETS * 1000u);
    EXPECT_EQ(info.bytes_total, (u64)CONC_ASSETS * 1000u);
    /* The fake saw the submits but completed nothing itself. */
    for (u32 i = 0; i < f->fake.count; ++i)
        f->fake.items[i].done = true;
    de_asset_release(f->s, h);
}
