/*
 * load_test_util.h: shared fixture and helpers of the load API tests (DENG-141). Implemented in
 * test_backend_fake.c.
 */
#ifndef DE_TEST_LOAD_TEST_UTIL_H
#define DE_TEST_LOAD_TEST_UTIL_H

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>

#include "fake_backend.h"

/** An asset system on a tracking allocator, with the fake backend installed. Teardown checks
 * that destroy leaves zero live bytes. */
typedef struct load_fx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_asset_system *s;
    fake_backend fake;
} load_fx;

void load_fx_setup(load_fx *f);
void load_fx_teardown(load_fx *f);

/** Parses and resolves `text` (one in-memory file) and makes it the active manifest of `s`. */
void load_test_install_text(de_asset_system *s, const char *text);
/** Makes examples/assets.toml (with the stub includes of data/example/) the active manifest. */
void load_test_install_example(de_asset_system *s);
/** The load-handle refs on the slot of `id` (0 without a slot). */
u32 load_test_handle_refs(de_asset_system *s, de_asset_id id);

/** Assets data/a..e and more/x, y; bundles `data` (every data/ asset), `more` (every more/
 * asset, depends on data, priority low) and `empty`. */
extern const char k_load_manifest[];

#endif /* DE_TEST_LOAD_TEST_UTIL_H */
