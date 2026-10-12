/*
 * test_bytes_type.h: the `test_bytes` asset type, a test-only fixture (DENG-140).
 *
 * No real engine asset type exists before S08 (`blob`) and S11+ (texture, mesh, ...), so the S07
 * tests register this one instead. It claims the extension `.bytes`; its `unload` counts calls
 * in the counter passed as `user`. Later S07 tests (load API, pipeline) reuse it and may fill in
 * `load` for their fake backend.
 */
#ifndef DE_TEST_BYTES_TYPE_H
#define DE_TEST_BYTES_TYPE_H

#include <dengine/de_asset.h>
#include <dengine/de_base.h>

#define TEST_BYTES_TYPE "test_bytes"

/** Counts test_bytes unloads; pass its address as `unload_counter`. */
typedef struct test_bytes_counters {
    u32 unloads;
    void *last_unloaded;
} test_bytes_counters;

static inline void test_bytes_unload(void *object, void *user) {
    test_bytes_counters *c = (test_bytes_counters *)user;
    if (c != NULL) {
        c->unloads++;
        c->last_unloaded = object;
    }
}

/** The desc of `test_bytes`. `counters` may be NULL. */
static inline de_asset_type_desc test_bytes_desc(test_bytes_counters *counters) {
    static const char *const exts[] = {".bytes"};
    de_asset_type_desc d = {0};
    d.name = TEST_BYTES_TYPE;
    d.extensions = exts;
    d.extension_count = 1;
    d.importer_version = 1;
    d.unload = test_bytes_unload;
    d.user = counters;
    return d;
}

/** Registers `test_bytes` on `s`. */
static inline de_asset_type test_bytes_register(de_asset_system *s, test_bytes_counters *counters) {
    de_asset_type_desc d = test_bytes_desc(counters);
    return de_asset_type_register(s, &d);
}

#endif /* DE_TEST_BYTES_TYPE_H */
