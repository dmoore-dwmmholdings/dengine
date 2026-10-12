/*
 * fake_backend.h: a scripted de__load_backend for the load API tests (DENG-141). Implemented in
 * test_backend_fake.c, which also tests it.
 *
 * The fake records every call (submit, cancel, reprioritize) and completes nothing by itself:
 * the test decides when slots report progress, complete or fail, so every state of a request
 * can be observed between two de_asset_system_update calls.
 */
#ifndef DE_TEST_FAKE_BACKEND_H
#define DE_TEST_FAKE_BACKEND_H

#include <dengine/de_asset.h>
#include <dengine/de_base.h>

#include "asset/backend_internal.h"

#define FAKE_MAX_ITEMS 256u
#define FAKE_MAX_FAILS 16u

/** One submission. */
typedef struct fake_item {
    u32 slot;
    de_asset_id id;
    de__load_priority priority; /**< the latest (submit or reprioritize) */
    bool cancelled;
    bool done;
} fake_item;

typedef struct fake_backend {
    de_asset_system *s;
    fake_item items[FAKE_MAX_ITEMS]; /**< in submit order */
    u32 count;
    u32 submits, cancels, reprioritizes;
    /** Assets that fail when completed, and with which code. */
    de_asset_id fail_ids[FAKE_MAX_FAILS];
    de_result fail_codes[FAKE_MAX_FAILS];
    u32 fail_count;
    /** Bytes each asset "reads" (progress) and its resident size on success. */
    u64 asset_bytes;
} fake_backend;

/** Zeroes `f` and installs it as the backend of `s` (asset_bytes 1000). */
void fake_backend_install(fake_backend *f, de_asset_system *s);
/** Makes asset `id` fail with `code` when completed. */
void fake_backend_fail(fake_backend *f, de_asset_id id, de_result code);
/** The latest submission of `id`, or NULL. */
fake_item *fake_backend_find(fake_backend *f, de_asset_id id);
/** Outstanding submissions (not done, not cancelled). */
u32 fake_backend_outstanding(const fake_backend *f);
/** Reports `num / den` of asset_bytes read for every outstanding submission. */
void fake_backend_progress(fake_backend *f, u32 num, u32 den);
/**
 * Completes up to `max` outstanding submissions, most urgent priority first, then in submit
 * order (a failure for the assets fake_backend_fail named). Returns how many it completed. The
 * results are queued; they apply at the next de_asset_system_update.
 */
u32 fake_backend_complete(fake_backend *f, u32 max);
/** Completes the outstanding submission of `id` (false if there is none). */
bool fake_backend_complete_id(fake_backend *f, de_asset_id id);

#endif /* DE_TEST_FAKE_BACKEND_H */
