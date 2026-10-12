/*
 * test_load_api.c: the load API (de_asset_load / _load_many / _load_bundle, cancel, release,
 * status, status_of, status_list, wait) driven through the fake backend (DENG-141, SPEC §11.3).
 *
 * The fake completes nothing by itself: each test reports progress and completes slots, then
 * calls de_asset_system_update, which applies the queued completions.
 */
#include "de_alloc_guard.h"
#include "de_test.h"
#include "fake_backend.h"
#include "load_test_util.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_ecs.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_reflect.h>

#include "asset/asset_internal.h"
#include "asset/load_internal.h"
#include "asset/resolve_internal.h"
#include "asset/slots_internal.h"

#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#if defined(DE_DEBUG) || defined(DE_DEV)
#define CHECKS_ON 1
#else
#define CHECKS_ON 0
#endif

static de_asset_id id_of(const char *path) {
    return de_asset_id_from(path);
}

static de_load_state status(de_asset_system *s, de_load_handle h, de_load_info *info) {
    return de_asset_status(s, h, info);
}

static void update(load_fx *f) {
    de_asset_system_update(f->s, NULL);
}

static void count_eviction(de_asset_system *s, u32 handle, void *user) {
    (void)s;
    (void)handle;
    ++*(u32 *)user;
}

/* ---- load.bundle_ready ---------------------------------------------------------------------- */

/* Adds the members of `b` to `ids` (no duplicates). */
static void add_members(const de__resolved_bundle *b, de_asset_id *ids, u32 *n, u32 cap) {
    for (u32 i = 0; i < b->member_count; ++i) {
        bool dup = false;
        for (u32 k = 0; k < *n && !dup; ++k)
            dup = ids[k].hash == b->members[i].hash;
        if (!dup) {
            ASSERT_LT(*n, cap);
            ids[(*n)++] = b->members[i];
        }
    }
}

TEST_F(load, bundle_ready, load_fx) {
    load_test_install_example(f->s);
    const de__resolved *res = f->s->resolved;
    const de__resolved_bundle *level = de__resolved_find_bundle(res, "level_01");
    const de__resolved_bundle *common = de__resolved_find_bundle(res, "common");
    ASSERT_TRUE(level != NULL && common != NULL);
    de_asset_id expect[64];
    u32 n = 0;
    add_members(common, expect, &n, 64);
    add_members(level, expect, &n, 64);
    ASSERT_EQ(n, level->member_count + common->member_count); /* disjoint in the example */
    ASSERT_GT(common->member_count, 0u);

    de_load_handle h = de_asset_load_bundle(f->s, "level_01", DE_LOAD_DEFAULT);
    ASSERT_TRUE(DE_HANDLE_VALID(h));
    de_load_info info;
    EXPECT_EQ((int)status(f->s, h, &info), (int)DE_LOAD_PENDING);
    /* The member count includes `common`, which level_01 depends on. */
    EXPECT_EQ(info.assets_total, n);
    EXPECT_EQ(info.assets_ready, 0u);
    EXPECT_FLOAT_EQ(info.progress, 0.0, 1e-6);
    EXPECT_EQ(f->fake.submits, n);
    /* No flag: the bundle's manifest priority ("high") for every asset, common's included. */
    for (u32 i = 0; i < f->fake.count; ++i)
        EXPECT_EQ((int)f->fake.items[i].priority, (int)DE__LOAD_PRIORITY_HIGH);
    for (u32 i = 0; i < n; ++i) {
        EXPECT_EQ(load_test_handle_refs(f->s, expect[i]), 1u);
        EXPECT_EQ((int)de_asset_status_of(f->s, expect[i]), (int)DE_LOAD_PENDING);
    }

    fake_backend_progress(&f->fake, 1, 2);
    update(f);
    EXPECT_EQ((int)status(f->s, h, &info), (int)DE_LOAD_LOADING);
    EXPECT_FLOAT_EQ(info.progress, 0.5, 1e-4);
    EXPECT_EQ(info.bytes_total, (u64)n * 1000u);
    EXPECT_EQ(info.bytes_done, (u64)n * 500u);
    EXPECT_EQ((int)de_asset_status_of(f->s, expect[0]), (int)DE_LOAD_LOADING);

    EXPECT_EQ(fake_backend_complete(&f->fake, n - 1u), n - 1u);
    update(f);
    EXPECT_EQ((int)status(f->s, h, &info), (int)DE_LOAD_LOADING);
    EXPECT_EQ(info.assets_ready, n - 1u);
    EXPECT_TRUE(info.progress < 1.0f);

    EXPECT_EQ(fake_backend_complete(&f->fake, 100), 1u);
    update(f);
    EXPECT_EQ((int)status(f->s, h, &info), (int)DE_LOAD_READY);
    EXPECT_EQ(info.assets_total, n);
    EXPECT_EQ(info.assets_ready, n);
    EXPECT_EQ(info.assets_failed, 0u);
    EXPECT_FLOAT_EQ(info.progress, 1.0, 1e-9);
    EXPECT_EQ(info.first_failed.hash, 0u);
    EXPECT_EQ((int)info.first_error, (int)DE_OK);

    de_asset_status_entry list[64];
    EXPECT_EQ(de_asset_status_list(f->s, h, list, 64), n);
    for (u32 i = 0; i < n; ++i) {
        EXPECT_EQ((int)list[i].state, (int)DE_LOAD_READY);
        EXPECT_EQ(list[i].bytes, 1000u);
        EXPECT_EQ((int)de_asset_status_of(f->s, list[i].id), (int)DE_LOAD_READY);
    }
    /* Dependencies' members come first. */
    for (u32 i = 0; i < common->member_count; ++i)
        EXPECT_EQ(list[i].id.hash, common->members[i].hash);
    EXPECT_EQ(de_asset_status_list(f->s, h, NULL, 0), n);
    EXPECT_EQ(de_asset_status_list(f->s, h, list, 2), n);

    de_asset_release(f->s, h);
    for (u32 i = 0; i < n; ++i) {
        EXPECT_EQ(load_test_handle_refs(f->s, expect[i]), 0u);
        /* Released, not evicted: the data stays until DENG-144 evicts it. */
        EXPECT_EQ((int)de_asset_status_of(f->s, expect[i]), (int)DE_LOAD_READY);
    }
}

/* ---- load.partial / load.failed ---------------------------------------------------------------
 */

TEST_F(load, partial, load_fx) {
    load_test_install_text(f->s, k_load_manifest);
    fake_backend_fail(&f->fake, id_of("data/b"), DE_ERR_IO);
    const de_asset_id ids[] = {id_of("data/a"), id_of("data/b"), id_of("data/c")};
    de_load_handle h = de_asset_load_many(f->s, ids, 3, 0);
    fake_backend_complete(&f->fake, 100);
    update(f);
    de_load_info info;
    EXPECT_EQ((int)status(f->s, h, &info), (int)DE_LOAD_PARTIAL);
    EXPECT_EQ(info.assets_total, 3u);
    EXPECT_EQ(info.assets_ready, 2u);
    EXPECT_EQ(info.assets_failed, 1u);
    EXPECT_EQ(info.first_failed.hash, id_of("data/b").hash);
    EXPECT_EQ((int)info.first_error, (int)DE_ERR_IO);
    EXPECT_STREQ(info.first_error_msg, "fake backend: cannot load \"data/b\"");
    EXPECT_FLOAT_EQ(info.progress, 1.0, 1e-9);
    EXPECT_EQ((int)de_asset_status_of(f->s, id_of("data/b")), (int)DE_LOAD_FAILED);
    de_asset_status_entry list[3];
    EXPECT_EQ(de_asset_status_list(f->s, h, list, 3), 3u);
    EXPECT_EQ((int)list[1].state, (int)DE_LOAD_FAILED);
    EXPECT_EQ((int)list[0].state, (int)DE_LOAD_READY);
    de_asset_release(f->s, h);
}

TEST_F(load, failed, load_fx) {
    load_test_install_text(f->s, k_load_manifest);
    const char *const paths[] = {"data/a", "data/b", "data/c", "data/d", "data/e"};
    for (u32 i = 0; i < 5; ++i)
        fake_backend_fail(&f->fake, id_of(paths[i]), DE_ERR_PARSE);
    de_load_handle h = de_asset_load_bundle(f->s, "data", 0);
    fake_backend_complete(&f->fake, 100);
    update(f);
    de_load_info info;
    EXPECT_EQ((int)status(f->s, h, &info), (int)DE_LOAD_FAILED);
    EXPECT_EQ(info.assets_ready, 0u);
    EXPECT_EQ(info.assets_failed, 5u);
    EXPECT_EQ((int)info.first_error, (int)DE_ERR_PARSE);
    /* The first failure in request order: the bundle's first member. */
    de_asset_status_entry first;
    EXPECT_EQ(de_asset_status_list(f->s, h, &first, 1), 5u);
    EXPECT_EQ(info.first_failed.hash, first.id.hash);
    de_asset_release(f->s, h);

    /* Request order, not completion order: c fails after a, and c comes first. */
    h = de_asset_load_many(f->s, (de_asset_id[]){id_of("data/c"), id_of("data/a")}, 2, 0);
    EXPECT_TRUE(fake_backend_complete_id(&f->fake, id_of("data/a")));
    update(f);
    EXPECT_EQ((int)status(f->s, h, &info), (int)DE_LOAD_LOADING);
    EXPECT_EQ(info.first_failed.hash, id_of("data/a").hash);
    EXPECT_TRUE(fake_backend_complete_id(&f->fake, id_of("data/c")));
    update(f);
    EXPECT_EQ((int)status(f->s, h, &info), (int)DE_LOAD_FAILED);
    EXPECT_EQ(info.first_failed.hash, id_of("data/c").hash);
    EXPECT_STREQ(info.first_error_msg, "fake backend: cannot load \"data/c\"");
    de_asset_release(f->s, h);
}

/* ---- load.stop_on_error -----------------------------------------------------------------------
 */

TEST_F(load, stop_on_error, load_fx) {
    load_test_install_text(f->s, k_load_manifest);
    fake_backend_fail(&f->fake, id_of("data/b"), DE_ERR_IO);
    const de_asset_id ids[] = {id_of("data/a"), id_of("data/b"), id_of("data/c"), id_of("data/d")};
    de_load_handle h = de_asset_load_many(f->s, ids, 4, DE_LOAD_STOP_ON_ERROR);
    EXPECT_EQ(f->fake.submits, 4u);
    EXPECT_TRUE(fake_backend_complete_id(&f->fake, id_of("data/a")));
    update(f);
    EXPECT_EQ((int)status(f->s, h, NULL), (int)DE_LOAD_LOADING);
    EXPECT_TRUE(fake_backend_complete_id(&f->fake, id_of("data/b")));
    update(f);
    de_load_info info;
    /* One ready, one failed, the rest cancelled: PARTIAL. */
    EXPECT_EQ((int)status(f->s, h, &info), (int)DE_LOAD_PARTIAL);
    EXPECT_EQ(info.assets_ready, 1u);
    EXPECT_EQ(info.assets_failed, 1u);
    EXPECT_EQ(info.first_failed.hash, id_of("data/b").hash);
    EXPECT_EQ(f->fake.cancels, 2u);
    EXPECT_TRUE(fake_backend_find(&f->fake, id_of("data/c"))->cancelled);
    EXPECT_TRUE(fake_backend_find(&f->fake, id_of("data/d"))->cancelled);
    EXPECT_EQ(fake_backend_outstanding(&f->fake), 0u);
    EXPECT_EQ((int)de_asset_status_of(f->s, id_of("data/c")), (int)DE_LOAD_CANCELLED);
    update(f);
    EXPECT_EQ(f->fake.submits, 4u); /* nothing more was submitted */
    EXPECT_EQ((int)status(f->s, h, NULL), (int)DE_LOAD_PARTIAL);
    de_asset_release(f->s, h);

    /* The first asset fails: FAILED, everything else cancelled. */
    fake_backend_fail(&f->fake, id_of("more/x"), DE_ERR_IO);
    h = de_asset_load_many(f->s, (de_asset_id[]){id_of("more/x"), id_of("more/y")}, 2,
                           DE_LOAD_STOP_ON_ERROR);
    u32 submits = f->fake.submits;
    EXPECT_TRUE(fake_backend_complete_id(&f->fake, id_of("more/x")));
    update(f);
    EXPECT_EQ((int)status(f->s, h, &info), (int)DE_LOAD_FAILED);
    EXPECT_EQ(info.assets_ready, 0u);
    EXPECT_EQ(info.first_failed.hash, id_of("more/x").hash);
    EXPECT_EQ(fake_backend_outstanding(&f->fake), 0u);
    EXPECT_EQ(f->fake.submits, submits);
    de_asset_release(f->s, h);

    /* A shared asset stays in flight for the request that still waits on it. */
    fake_backend_install(&f->fake, f->s);
    fake_backend_fail(&f->fake, id_of("data/c"), DE_ERR_IO);
    de_load_handle hs = de_asset_load_many(f->s, (de_asset_id[]){id_of("data/c"), id_of("data/e")},
                                           2, DE_LOAD_STOP_ON_ERROR);
    de_load_handle other = de_asset_load(f->s, id_of("data/e"), 0);
    EXPECT_EQ(f->fake.submits, 2u); /* c was cancelled above: submitted again */
    EXPECT_TRUE(fake_backend_complete_id(&f->fake, id_of("data/c")));
    update(f);
    EXPECT_EQ((int)status(f->s, hs, NULL), (int)DE_LOAD_FAILED);
    EXPECT_FALSE(fake_backend_find(&f->fake, id_of("data/e"))->cancelled);
    EXPECT_EQ(f->fake.cancels, 0u);
    fake_backend_complete(&f->fake, 100);
    update(f);
    EXPECT_EQ((int)status(f->s, other, NULL), (int)DE_LOAD_READY);
    EXPECT_EQ((int)status(f->s, hs, NULL), (int)DE_LOAD_FAILED); /* final states stay */
    de_asset_release(f->s, hs);
    de_asset_release(f->s, other);
}

/* ---- load.shared_asset_refcount ---------------------------------------------------------------
 */

TEST_F(load, shared_asset_refcount, load_fx) {
    load_test_install_text(f->s, k_load_manifest);
    de_load_handle h1 =
        de_asset_load_many(f->s, (de_asset_id[]){id_of("data/a"), id_of("data/b")}, 2, 0);
    de_load_handle h2 = de_asset_load(f->s, id_of("data/b"), 0);
    EXPECT_EQ(f->fake.submits, 2u); /* b once */
    EXPECT_EQ(load_test_handle_refs(f->s, id_of("data/b")), 2u);
    EXPECT_EQ(load_test_handle_refs(f->s, id_of("data/a")), 1u);
    fake_backend_complete(&f->fake, 100);
    update(f);
    EXPECT_EQ((int)status(f->s, h1, NULL), (int)DE_LOAD_READY);
    EXPECT_EQ((int)status(f->s, h2, NULL), (int)DE_LOAD_READY);

    de_asset_release(f->s, h1);
    EXPECT_EQ((int)status(f->s, h2, NULL), (int)DE_LOAD_READY);
    EXPECT_EQ((int)de_asset_status_of(f->s, id_of("data/b")), (int)DE_LOAD_READY);
    EXPECT_EQ(load_test_handle_refs(f->s, id_of("data/b")), 1u);
    EXPECT_EQ(load_test_handle_refs(f->s, id_of("data/a")), 0u);
    de_asset_release(f->s, h2);
    EXPECT_EQ(load_test_handle_refs(f->s, id_of("data/b")), 0u);
    /* Both reached zero refs: both are eviction candidates (DENG-144), once each. */
    u32 calls = 0;
    EXPECT_EQ(de__slot_eviction_drain(f->s, count_eviction, &calls), 2u);
    EXPECT_EQ(calls, 2u);
    EXPECT_EQ(f->fake.cancels, 0u);
}

/* ---- load.cancel ------------------------------------------------------------------------------
 */

TEST_F(load, cancel, load_fx) {
    load_test_install_text(f->s, k_load_manifest);
    de_load_handle h1 =
        de_asset_load_many(f->s, (de_asset_id[]){id_of("data/a"), id_of("data/b")}, 2, 0);
    de_load_handle h2 = de_asset_load(f->s, id_of("data/b"), 0);
    de_asset_cancel(f->s, h1);
    de_load_info info;
    EXPECT_EQ((int)status(f->s, h1, &info), (int)DE_LOAD_CANCELLED);
    EXPECT_FLOAT_EQ(info.progress, 1.0, 1e-9);
    /* Only a: b is still wanted by h2. */
    EXPECT_EQ(f->fake.cancels, 1u);
    EXPECT_TRUE(fake_backend_find(&f->fake, id_of("data/a"))->cancelled);
    EXPECT_FALSE(fake_backend_find(&f->fake, id_of("data/b"))->cancelled);
    EXPECT_EQ((int)de_asset_status_of(f->s, id_of("data/a")), (int)DE_LOAD_CANCELLED);
    EXPECT_EQ((int)de_asset_status_of(f->s, id_of("data/b")), (int)DE_LOAD_PENDING);
    /* Cancel keeps the refs until release. */
    EXPECT_EQ(load_test_handle_refs(f->s, id_of("data/a")), 1u);

    fake_backend_complete(&f->fake, 100);
    update(f);
    EXPECT_EQ((int)status(f->s, h2, NULL), (int)DE_LOAD_READY);
    EXPECT_EQ((int)status(f->s, h1, NULL), (int)DE_LOAD_CANCELLED);
    de_asset_cancel(f->s, h2); /* final: no effect */
    EXPECT_EQ((int)status(f->s, h2, NULL), (int)DE_LOAD_READY);
    EXPECT_EQ(f->fake.cancels, 1u);

    /* A new request resubmits the cancelled asset. */
    de_load_handle h3 = de_asset_load(f->s, id_of("data/a"), 0);
    EXPECT_EQ((int)de_asset_status_of(f->s, id_of("data/a")), (int)DE_LOAD_PENDING);
    EXPECT_EQ(f->fake.submits, 3u);
    /* Releasing a request that is not final cancels it first. */
    de_asset_release(f->s, h3);
    EXPECT_EQ(f->fake.cancels, 2u);
    de_asset_release(f->s, h1);
    de_asset_release(f->s, h2);
    EXPECT_EQ(load_test_handle_refs(f->s, id_of("data/a")), 0u);
    EXPECT_EQ(load_test_handle_refs(f->s, id_of("data/b")), 0u);
}

/* ---- load.release_stale -----------------------------------------------------------------------
 */

TEST_F(load, release_stale, load_fx) {
    load_test_install_text(f->s, k_load_manifest);
    de_load_handle h = de_asset_load(f->s, id_of("data/a"), 0);
    fake_backend_complete(&f->fake, 100);
    update(f);
    EXPECT_EQ((int)status(f->s, h, NULL), (int)DE_LOAD_READY);
    de_asset_release(f->s, h);

    de_load_info info;
    memset(&info, 0xAB, sizeof info);
    EXPECT_EQ((int)status(f->s, h, &info), (int)DE_LOAD_INVALID);
    EXPECT_EQ(info.assets_total, 0u);
    EXPECT_EQ(info.first_error_msg[0], '\0');
    de_asset_status_entry e;
    EXPECT_EQ(de_asset_status_list(f->s, h, &e, 1), 0u);
    EXPECT_EQ((int)de_asset_wait(f->s, h, 0), (int)DE_LOAD_INVALID);
    de_asset_cancel(f->s, h);  /* ignored */
    de_asset_release(f->s, h); /* ignored */
    EXPECT_EQ(load_test_handle_refs(f->s, id_of("data/a")), 0u);
    EXPECT_EQ((int)status(f->s, (de_load_handle){0}, NULL), (int)DE_LOAD_INVALID);
    EXPECT_EQ((int)status(NULL, h, NULL), (int)DE_LOAD_INVALID);

    /* Request indices are reused late and with a new generation: no new handle equals h. */
    de_load_handle hs[200];
    for (u32 i = 0; i < 200; ++i) {
        hs[i] = de_asset_load(f->s, id_of("data/a"), 0);
        EXPECT_NE(hs[i].id, h.id);
        de_asset_release(f->s, hs[i]);
        EXPECT_EQ((int)status(f->s, h, NULL), (int)DE_LOAD_INVALID);
    }
}

/* ---- load.priority_resolution -----------------------------------------------------------------
 */

static de__load_priority prio_of(load_fx *f, const char *path) {
    fake_item *it = fake_backend_find(&f->fake, id_of(path));
    ASSERT_TRUE(it != NULL);
    return it->priority;
}

TEST_F(load, priority_resolution, load_fx) {
    load_test_install_text(f->s, k_load_manifest);
    /* Several bits: the most urgent wins. */
    de_load_handle h[6];
    h[0] = de_asset_load(f->s, id_of("data/a"), DE_LOAD_PRIORITY_CRITICAL | DE_LOAD_PRIORITY_LOW);
    h[1] = de_asset_load(f->s, id_of("data/b"),
                         DE_LOAD_PRIORITY_HIGH | DE_LOAD_PRIORITY_BACKGROUND | DE_LOAD_NO_EVENTS);
    h[2] = de_asset_load(f->s, id_of("data/c"), DE_LOAD_PRIORITY_LOW | DE_LOAD_PRIORITY_BACKGROUND);
    h[3] = de_asset_load(f->s, id_of("data/d"), DE_LOAD_PRIORITY_BACKGROUND);
    h[4] = de_asset_load(f->s, id_of("data/e"), DE_LOAD_DEFAULT);
    EXPECT_EQ((int)prio_of(f, "data/a"), (int)DE__LOAD_PRIORITY_CRITICAL);
    EXPECT_EQ((int)prio_of(f, "data/b"), (int)DE__LOAD_PRIORITY_HIGH);
    EXPECT_EQ((int)prio_of(f, "data/c"), (int)DE__LOAD_PRIORITY_LOW);
    EXPECT_EQ((int)prio_of(f, "data/d"), (int)DE__LOAD_PRIORITY_BACKGROUND);
    EXPECT_EQ((int)prio_of(f, "data/e"), (int)DE__LOAD_PRIORITY_NORMAL);
    /* DE_LOAD_NO_EVENTS is kept on the request (DENG-143 reads it). */
    EXPECT_TRUE((de__request_get(f->s, h[1])->flags & DE_LOAD_NO_EVENTS) != 0);
    for (u32 i = 0; i < 5; ++i)
        de_asset_release(f->s, h[i]);

    /* No flag on a bundle: the manifest's priority ("low" on `more`), dependencies included. */
    h[0] = de_asset_load_bundle(f->s, "more", 0);
    EXPECT_EQ((int)prio_of(f, "more/x"), (int)DE__LOAD_PRIORITY_LOW);
    EXPECT_EQ((int)prio_of(f, "data/a"), (int)DE__LOAD_PRIORITY_LOW);
    de_asset_release(f->s, h[0]);
    /* A flag overrides it. */
    h[0] = de_asset_load_bundle(f->s, "more", DE_LOAD_PRIORITY_HIGH);
    EXPECT_EQ((int)prio_of(f, "more/y"), (int)DE__LOAD_PRIORITY_HIGH);
    de_asset_release(f->s, h[0]);
    /* `data` has no priority: NORMAL. */
    h[0] = de_asset_load_bundle(f->s, "data", 0);
    EXPECT_EQ((int)prio_of(f, "data/e"), (int)DE__LOAD_PRIORITY_NORMAL);
    de_asset_release(f->s, h[0]);

    /* A shared slot takes the most urgent waiting request's priority, and drops back. */
    fake_backend_install(&f->fake, f->s);
    h[0] = de_asset_load(f->s, id_of("data/a"), DE_LOAD_PRIORITY_BACKGROUND);
    h[1] = de_asset_load(f->s, id_of("data/a"), DE_LOAD_PRIORITY_LOW);
    EXPECT_EQ((int)prio_of(f, "data/a"), (int)DE__LOAD_PRIORITY_LOW);
    h[2] = de_asset_load(f->s, id_of("data/a"), DE_LOAD_PRIORITY_CRITICAL);
    EXPECT_EQ((int)prio_of(f, "data/a"), (int)DE__LOAD_PRIORITY_CRITICAL);
    h[3] = de_asset_load(f->s, id_of("data/a"), DE_LOAD_PRIORITY_HIGH); /* less urgent: no call */
    EXPECT_EQ(f->fake.reprioritizes, 2u);
    EXPECT_EQ(f->fake.submits, 1u);
    de_asset_release(f->s, h[2]);
    EXPECT_EQ((int)prio_of(f, "data/a"), (int)DE__LOAD_PRIORITY_HIGH);
    de_asset_release(f->s, h[3]);
    de_asset_cancel(f->s, h[1]);
    EXPECT_EQ((int)prio_of(f, "data/a"), (int)DE__LOAD_PRIORITY_BACKGROUND);
    EXPECT_EQ(f->fake.reprioritizes, 5u); /* LOW, CRITICAL, HIGH, LOW, BACKGROUND */
    EXPECT_EQ(f->fake.cancels, 0u);
    de_asset_release(f->s, h[0]);
    EXPECT_EQ(f->fake.cancels, 1u);
    de_asset_release(f->s, h[1]);
}

/* ---- load.unknown_bundle ----------------------------------------------------------------------
 */

TEST_F(load, unknown_bundle, load_fx) {
    /* No manifest at all. */
    de_load_handle h = de_asset_load_bundle(f->s, "level_01", 0);
    ASSERT_TRUE(DE_HANDLE_VALID(h));
    de_load_info info;
    EXPECT_EQ((int)status(f->s, h, &info), (int)DE_LOAD_FAILED);
    EXPECT_EQ((int)info.first_error, (int)DE_ERR_NOT_FOUND);
    de_asset_release(f->s, h);

    load_test_install_text(f->s, k_load_manifest);
    h = de_asset_load_bundle(f->s, "nope", DE_LOAD_PRIORITY_HIGH);
    ASSERT_TRUE(DE_HANDLE_VALID(h));
    EXPECT_EQ((int)status(f->s, h, &info), (int)DE_LOAD_FAILED);
    EXPECT_EQ(info.assets_total, 0u);
    EXPECT_EQ((int)info.first_error, (int)DE_ERR_NOT_FOUND);
    EXPECT_EQ(info.first_failed.hash, 0u);
    EXPECT_STREQ(info.first_error_msg, "bundle \"nope\" is not in the asset manifest");
    EXPECT_FLOAT_EQ(info.progress, 1.0, 1e-9);
    EXPECT_EQ((int)de_asset_wait(f->s, h, 0), (int)DE_LOAD_FAILED);
    de_asset_release(f->s, h);

    /* An unknown asset ID: a FAILED request (never an invalid handle). */
    de_asset_id unknown = id_of("not/declared");
    h = de_asset_load(f->s, unknown, 0);
    ASSERT_TRUE(DE_HANDLE_VALID(h));
    EXPECT_EQ((int)status(f->s, h, &info), (int)DE_LOAD_FAILED);
    EXPECT_EQ(info.assets_total, 1u);
    EXPECT_EQ(info.assets_failed, 1u);
    EXPECT_EQ(info.first_failed.hash, unknown.hash);
    EXPECT_EQ((int)info.first_error, (int)DE_ERR_NOT_FOUND);
    EXPECT_TRUE(strstr(info.first_error_msg, "not in the asset manifest") != NULL);
    EXPECT_EQ((int)de_asset_status_of(f->s, unknown), (int)DE_LOAD_INVALID);
    EXPECT_EQ(f->fake.submits, 0u);
    de_asset_release(f->s, h);
    h = de_asset_load(f->s, (de_asset_id){0}, 0);
    EXPECT_EQ((int)status(f->s, h, &info), (int)DE_LOAD_FAILED);
    EXPECT_EQ((int)info.first_error, (int)DE_ERR_INVALID_ARGUMENT);
    de_asset_release(f->s, h);

    /* Known and unknown together: PARTIAL once the known one loads. */
    h = de_asset_load_many(f->s, (de_asset_id[]){unknown, id_of("data/a")}, 2, 0);
    EXPECT_EQ((int)status(f->s, h, NULL), (int)DE_LOAD_LOADING);
    fake_backend_complete(&f->fake, 100);
    update(f);
    EXPECT_EQ((int)status(f->s, h, &info), (int)DE_LOAD_PARTIAL);
    EXPECT_EQ(info.first_failed.hash, unknown.hash);
    de_asset_release(f->s, h);

    /* An empty bundle and an empty list are READY at once. */
    h = de_asset_load_bundle(f->s, "empty", 0);
    EXPECT_EQ((int)status(f->s, h, &info), (int)DE_LOAD_READY);
    EXPECT_EQ(info.assets_total, 0u);
    EXPECT_FLOAT_EQ(info.progress, 1.0, 1e-9);
    de_asset_release(f->s, h);
    h = de_asset_load_many(f->s, NULL, 0, 0);
    EXPECT_EQ((int)status(f->s, h, NULL), (int)DE_LOAD_READY);
    de_asset_release(f->s, h);

    /* Bad arguments (not "unknown"): an invalid handle and the error context. */
    EXPECT_FALSE(DE_HANDLE_VALID(de_asset_load_bundle(f->s, NULL, 0)));
    EXPECT_EQ((int)de_error_last()->result, (int)DE_ERR_INVALID_ARGUMENT);
    EXPECT_FALSE(DE_HANDLE_VALID(de_asset_load_many(f->s, NULL, 3, 0)));
    EXPECT_FALSE(DE_HANDLE_VALID(de_asset_load(NULL, unknown, 0)));
}

/* ---- load.dedupe_and_order --------------------------------------------------------------------
 */

TEST_F(load, dedupe_and_order, load_fx) {
    load_test_install_text(f->s, k_load_manifest);
    const de_asset_id ids[] = {id_of("data/c"), id_of("data/a"), id_of("data/c"), id_of("data/b"),
                               id_of("data/a")};
    de_load_handle h = de_asset_load_many(f->s, ids, 5, 0);
    de_load_info info;
    status(f->s, h, &info);
    EXPECT_EQ(info.assets_total, 3u);
    EXPECT_EQ(load_test_handle_refs(f->s, id_of("data/c")), 1u);
    de_asset_status_entry list[3];
    EXPECT_EQ(de_asset_status_list(f->s, h, list, 3), 3u);
    EXPECT_EQ(list[0].id.hash, id_of("data/c").hash);
    EXPECT_EQ(list[1].id.hash, id_of("data/a").hash);
    EXPECT_EQ(list[2].id.hash, id_of("data/b").hash);
    de_asset_release(f->s, h);
    /* `more` = data's 5 members (first) + its own 2. */
    h = de_asset_load_bundle(f->s, "more", 0);
    status(f->s, h, &info);
    EXPECT_EQ(info.assets_total, 7u);
    de_asset_release(f->s, h);
}

/* ---- load.progress_monotonic ------------------------------------------------------------------
 */

TEST_F(load, progress_monotonic, load_fx) {
    load_test_install_text(f->s, k_load_manifest);
    const de_asset_id ids[] = {id_of("data/a"), id_of("data/b"), id_of("data/c"), id_of("data/d")};
    de_load_handle h = de_asset_load_many(f->s, ids, 4, 0);
    de_load_info info;
    f32 last = -1.0f;
    u64 last_total = 0;
#define STEP()                                                                                     \
    do {                                                                                           \
        update(f);                                                                                 \
        de_load_state st__ = status(f->s, h, &info);                                               \
        EXPECT_TRUE(info.progress >= last);                                                        \
        if (st__ < DE_LOAD_READY)                                                                  \
            EXPECT_TRUE(info.progress < 1.0f);                                                     \
        EXPECT_LE(info.bytes_done, info.bytes_total);                                              \
        last = info.progress;                                                                      \
    } while (0)
    STEP();
    EXPECT_FLOAT_EQ(info.progress, 0.0, 1e-9);
    fake_backend_progress(&f->fake, 1, 4);
    STEP();
    EXPECT_FLOAT_EQ(info.progress, 0.25, 1e-4);
    last_total = info.bytes_total;
    EXPECT_TRUE(fake_backend_complete_id(&f->fake, id_of("data/a")));
    STEP();
    /* The remaining assets turn out larger: bytes_total grows, the ratio falls, the reported
     * progress does not. */
    f->fake.asset_bytes = 10000;
    fake_backend_progress(&f->fake, 1, 10);
    STEP();
    EXPECT_GT(info.bytes_total, last_total);
    f32 held = info.progress;
    EXPECT_TRUE((f64)held > (f64)info.bytes_done / (f64)info.bytes_total);
    fake_backend_progress(&f->fake, 9, 10);
    STEP();
    EXPECT_TRUE(info.progress > held);
    /* Every byte read but not yet published: still below 1.0. */
    fake_backend_progress(&f->fake, 1, 1);
    STEP();
    EXPECT_EQ(info.bytes_done, info.bytes_total);
    EXPECT_EQ((int)status(f->s, h, NULL), (int)DE_LOAD_LOADING);
    fake_backend_complete(&f->fake, 100);
    STEP();
    EXPECT_EQ((int)status(f->s, h, NULL), (int)DE_LOAD_READY);
    EXPECT_FLOAT_EQ(info.progress, 1.0, 1e-9);
    EXPECT_EQ(info.bytes_total, 31000u);
#undef STEP
    de_asset_release(f->s, h);
}

/* ---- load.status_no_alloc ---------------------------------------------------------------------
 */

TEST_F(load, status_no_alloc, load_fx) {
    load_test_install_text(f->s, k_load_manifest);
    de_load_handle h = de_asset_load_bundle(f->s, "more", 0);
    /* Warm-up: each of the two completion queues grows on its first use. */
    fake_backend_complete(&f->fake, 1);
    update(f);
    fake_backend_complete(&f->fake, 1);
    update(f);
    de_load_info info;
    de_asset_status_entry list[8];
    (void)status(f->s, h, &info);

    de_alloc_guard g;
    memset(&g, 0, sizeof g);
    g.this_thread_only = true;
    de_alloc_guard_begin(&g);
    for (u32 frame = 0; frame < 4; ++frame) {
        fake_backend_progress(&f->fake, frame + 1u, 8);
        (void)fake_backend_complete(&f->fake, 1);
        update(f);
        (void)status(f->s, h, &info);
        (void)de_asset_status_list(f->s, h, list, 8);
        (void)de_asset_status_of(f->s, id_of("more/x"));
    }
    de_alloc_guard_end(&g);
    DE_ALLOC_GUARD_ASSERT_ZERO(&g);
    EXPECT_EQ((int)status(f->s, h, NULL), (int)DE_LOAD_LOADING);
    fake_backend_complete(&f->fake, 100);
    update(f);
    EXPECT_EQ((int)status(f->s, h, NULL), (int)DE_LOAD_READY);
    de_asset_release(f->s, h);
}

/* ---- load.wait_timeout ------------------------------------------------------------------------
 */

typedef struct late_ctx {
    de_asset_system *s;
    u32 slot;
    u64 delay_ns;
} late_ctx;

static i32 late_complete(void *p) {
    late_ctx *c = (late_ctx *)p;
    de_sleep_ns(c->delay_ns);
    de__slot_progress(c->s, c->slot, 500, 1000);
    de__slot_complete(c->s, c->slot, DE_OK, 1000);
    return 0;
}

TEST_F(wait, wait_timeout, load_fx) {
    load_test_install_text(f->s, k_load_manifest);
    de_load_handle h = de_asset_load(f->s, id_of("data/a"), 0);
    de_error_clear();
    u64 t0 = de_time_now_ns();
    EXPECT_EQ((int)de_asset_wait(f->s, h, 2000000u), (int)DE_LOAD_PENDING); /* 2 ms */
    EXPECT_GE(de_time_now_ns() - t0, 2000000u);
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_EQ((int)de_error_last()->result, (int)DE_ERR_TIMEOUT);
    /* Timeout 0 polls once. */
    EXPECT_EQ((int)de_asset_wait(f->s, h, 0), (int)DE_LOAD_PENDING);

    /* Another thread completes the load: the wait returns READY. */
    late_ctx c = {f->s, de__slot_find(f->s, id_of("data/a")), 5000000u};
    de_thread *t = NULL;
    ASSERT_EQ(de_thread_create(late_complete, &c, "late", DE_THREAD_NORMAL, 0, &t), DE_OK);
    EXPECT_EQ((int)de_asset_wait(f->s, h, 10ull * 1000000000ull), (int)DE_LOAD_READY);
    (void)de_thread_join(t);
    fake_backend_find(&f->fake, id_of("data/a"))->done = true;
    de_load_info info;
    EXPECT_EQ((int)status(f->s, h, &info), (int)DE_LOAD_READY);
    EXPECT_FLOAT_EQ(info.progress, 1.0, 1e-9);
    /* An already final request returns at once. */
    EXPECT_EQ((int)de_asset_wait(f->s, h, 0), (int)DE_LOAD_READY);
    de_asset_release(f->s, h);
}

/* ---- wait.wait_blocks (DENG-387) ----------------------------------------------------------------
 *
 * de_asset_wait blocks in the OS (Trama "Decision: A waiting thread blocks in the OS; it never
 * spins without a bound"). On a load that nobody completes it wakes only when its safety-net
 * slice (10 ms) runs out: about 20 rounds in 200 ms, where the old 0.1 ms polling with a
 * spinning de_sleep_ns ran about 2000. A completion from another thread ends a block at once
 * (a signalled wake-up): a block with a 5 s limit returns long before it. */

#define WAIT_SLICE_NS 10000000u /* DE__WAIT_SAFETY_NS in status.c */

TEST_F(wait, wait_blocks, load_fx) {
    load_test_install_text(f->s, k_load_manifest);
    de_load_handle h = de_asset_load(f->s, id_of("data/a"), 0);

    /* Held: nobody completes the load. */
    const u64 hold_ns = 200000000u; /* 200 ms */
    de__asset_wait_stats a, b;
    de__asset_wait_get_stats(f->s, &a);
    u64 t0 = de_time_now_ns();
    EXPECT_EQ((int)de_asset_wait(f->s, h, hold_ns), (int)DE_LOAD_PENDING);
    u64 took = de_time_now_ns() - t0;
    de__asset_wait_get_stats(f->s, &b);
    EXPECT_GE(took, hold_ns);
    EXPECT_EQ(b.waits - a.waits, 1u);
    /* Each round but the last ends one blocking wait; each block lasts a whole slice (or the
     * rest of the timeout), so at most one round per slice, plus the first and the last. */
    u64 rounds = b.rounds - a.rounds;
    EXPECT_GE(rounds, 2u);
    EXPECT_LE(rounds, took / WAIT_SLICE_NS + 2u);
    EXPECT_EQ(b.blocks - a.blocks, rounds - 1u);
    EXPECT_EQ(b.signalled - a.signalled, 0u); /* nothing reported progress */
    EXPECT_EQ(b.timeouts - a.timeouts, rounds - 1u);
    if (rounds > took / WAIT_SLICE_NS + 2u)
        printf("  %llu rounds in %llu ns\n", (unsigned long long)rounds, (unsigned long long)took);

    /* Another thread completes the load after 30 ms: the completion ends a block that would
     * otherwise last 5 s (only a producer can end it early), and the wait applies it. */
    late_ctx c = {f->s, de__slot_find(f->s, id_of("data/a")), 30000000u};
    de_thread *t = NULL;
    de__asset_wait_get_stats(f->s, &a);
    t0 = de_time_now_ns();
    de__asset_wake_reset(f->s);
    ASSERT_EQ(de_thread_create(late_complete, &c, "late", DE_THREAD_NORMAL, 0, &t), DE_OK);
    const u64 long_ns = (u64)5 * 1000000000u;
    EXPECT_TRUE(de__asset_wake_block(f->s, long_ns));
    took = de_time_now_ns() - t0;
    EXPECT_LT(took, long_ns);
    EXPECT_EQ((int)de_asset_wait(f->s, h, 10ull * 1000000000ull), (int)DE_LOAD_READY);
    (void)de_thread_join(t);
    de__asset_wait_get_stats(f->s, &b);
    fake_backend_find(&f->fake, id_of("data/a"))->done = true;
    EXPECT_EQ(b.signalled - a.signalled, 1u);
    EXPECT_EQ(b.timeouts - a.timeouts, 0u);
    de_asset_release(f->s, h);
}

/* ---- wait.wait_asserts_in_schedule (death test) ------------------------------------------------
 */

/* The check panics (DE_ASSERT). The death test catches the panic with a panic hook that longjmps
 * out, on a fresh thread (a thread that left a panic by longjmp still counts as panicking), on a
 * world without a task system, so the system runs on that thread. */
static jmp_buf g_panic_jmp;
static int g_panic_calls;
static char g_panic_msg[512];

static void longjmp_hook(const de_panic_info *info, void *user) {
    (void)user;
    ++g_panic_calls;
    snprintf(g_panic_msg, sizeof g_panic_msg, "%s", info->message);
    longjmp(g_panic_jmp, 1);
}

typedef struct wait_sys_ctx {
    de_asset_system *s;
    de_load_handle h;
    de_world *w;
    int returned;
} wait_sys_ctx;

static void wait_system(de_system_ctx *ctx) {
    wait_sys_ctx *c = (wait_sys_ctx *)ctx->user;
    (void)de_asset_wait(c->s, c->h, 0);
    c->returned = 1;
}

static i32 run_update_thread(void *p) {
    wait_sys_ctx *c = (wait_sys_ctx *)p;
    if (setjmp(g_panic_jmp) == 0)
        (void)de_world_run_schedule(c->w, DE_SCHEDULE_UPDATE, 0.0f, 0.0f);
    return 0;
}

TEST_F(wait, wait_asserts_in_schedule, load_fx) {
    load_test_install_text(f->s, k_load_manifest);
    de_load_handle h = de_asset_load(f->s, id_of("data/a"), 0);
    de_type_registry *types = NULL;
    ASSERT_EQ(de_type_registry_create(NULL, &types), DE_OK);
    de_world *w = NULL;
    ASSERT_EQ(de_world_create(&(de_world_desc){.types = types}, &w), DE_OK);
    wait_sys_ctx c = {f->s, h, w, 0};
    de_system sys = de_system_add(
        w, &(de_system_desc){
               .name = "waits", .schedule = DE_SCHEDULE_UPDATE, .fn = wait_system, .user = &c});
    ASSERT_TRUE(DE_HANDLE_VALID(sys));

    /* Outside a schedule the same call is fine. */
    EXPECT_EQ((int)de_asset_wait(f->s, h, 0), (int)DE_LOAD_PENDING);

    const int before = g_panic_calls;
    g_panic_msg[0] = '\0';
    de_panic_set_hook(longjmp_hook, NULL);
    de_thread *t = NULL;
    ASSERT_EQ(de_thread_create(run_update_thread, &c, "wait-death", DE_THREAD_NORMAL, 0, &t),
              DE_OK);
    (void)de_thread_join(t);
    de_panic_set_hook(NULL, NULL);
#if CHECKS_ON
    EXPECT_EQ(g_panic_calls, before + 1);
    EXPECT_EQ(c.returned, 0);
    EXPECT_TRUE(strstr(g_panic_msg, "de_asset_wait called from inside an ECS system") != NULL);
#else
    /* Release builds compile the check out: the wait just polls. */
    EXPECT_EQ(g_panic_calls, before);
    EXPECT_EQ(c.returned, 1);
#endif
    /* The panicked world is left mid-schedule; it is only destroyed. */
    de_world_destroy(w);
    de_type_registry_destroy(types);
    de_asset_release(f->s, h);
}
