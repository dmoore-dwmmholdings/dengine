/*
 * asset_stubs.c: stand-ins for every de_asset.h function while the host has no asset system
 * (DE_HAS_ASSETS 0; SPEC §11; DENG-95). DENG-143 sets DE_HAS_ASSETS to 1 in
 * src/host/CMakeLists.txt, creates de_engine.assets in the engine and deletes this file.
 *
 * NOT part of de_host or of the aggregate `dengine` library: the real asset module (de_asset)
 * defines the same symbols. A game that calls the asset API before the host creates an asset
 * system links the INTERFACE target `de_asset_stubs` instead, which compiles this file into the
 * game's own binary:
 *   - library form: with the game's hidden visibility, so the calls bind inside the game library
 *     and the host's exported de_asset_* are never used;
 *   - static form: as an object of the game's static library, which the linker reads before the
 *     `dengine` archive, so no de_asset object is ever pulled in.
 * examples/CMakeLists.txt (minimal_game) and tests/unit/host (headless.*) link it.
 *
 * Behaviour: loading always "succeeds at once". de_asset_manifest_load returns DE_OK without
 * reading anything; de_asset_load_bundle / _load / _load_many return a valid handle;
 * de_asset_status (and de_asset_wait) report DE_LOAD_READY with a zeroed de_load_info whose
 * progress is 1.0 (the header's rule for a final request); de_asset_release and the other
 * setters do nothing. Everything else returns an invalid or empty value: a {0} ID or handle,
 * NULL, 0, DE_LOAD_INVALID, or DE_ERR_UNSUPPORTED from de_asset_system_create and
 * de_asset_pack_mount. No state, no allocation; the system pointer is ignored (it is NULL).
 */
#include <dengine/de_asset.h>

#if defined(DE_HAS_ASSETS) && DE_HAS_ASSETS
#error "asset_stubs.c is for DE_HAS_ASSETS 0 only: the asset module provides these functions"
#endif

#include <string.h>

/* The one handle the stubs give out: valid (non-zero), never stale. */
#define STUB_LOAD_HANDLE 1u

static de_load_handle stub_load_handle(void) {
    de_load_handle h = {STUB_LOAD_HANDLE};
    return h;
}

/* ---- IDs ---- */

de_asset_id de_asset_id_from(const char *path) {
    (void)path;
    de_asset_id id = {0};
    return id;
}

const char *de_asset_id_path(const de_asset_system *s, de_asset_id id) {
    (void)s;
    (void)id;
    return NULL;
}

/* ---- System ---- */

de_result de_asset_system_create(const de_asset_system_desc *desc, de_asset_system **out) {
    (void)desc;
    if (out != NULL)
        *out = NULL;
    return DE_FAIL(DE_ERR_UNSUPPORTED, "de_asset_system_create: no asset system (DE_HAS_ASSETS 0)");
}

void de_asset_system_destroy(de_asset_system *s) {
    (void)s;
}

void de_asset_system_update(de_asset_system *s, de_world *world) {
    (void)s;
    (void)world;
}

/* ---- Manifest ---- */

de_result de_asset_manifest_load(de_asset_system *s, const char *vpath) {
    (void)s;
    (void)vpath;
    return DE_OK;
}

const de_asset_manifest_issue *de_asset_manifest_issues(const de_asset_system *s, u32 *count) {
    (void)s;
    if (count != NULL)
        *count = 0;
    return NULL;
}

de_result de_asset_pack_mount(de_asset_system *s, const char *vpath, i32 priority) {
    (void)s;
    (void)priority;
    return DE_FAIL(DE_ERR_UNSUPPORTED, "de_asset_pack_mount(%s): no asset system (DE_HAS_ASSETS 0)",
                   vpath != NULL ? vpath : "NULL");
}

void de_asset_pack_unmount(de_asset_system *s, const char *vpath) {
    (void)s;
    (void)vpath;
}

void de_asset_set_locale(de_asset_system *s, const char *locale) {
    (void)s;
    (void)locale;
}

/* ---- Loading ---- */

de_load_handle de_asset_load_bundle(de_asset_system *s, const char *bundle, u32 flags) {
    (void)s;
    (void)bundle;
    (void)flags;
    return stub_load_handle();
}

de_load_handle de_asset_load(de_asset_system *s, de_asset_id id, u32 flags) {
    (void)s;
    (void)id;
    (void)flags;
    return stub_load_handle();
}

de_load_handle de_asset_load_many(de_asset_system *s, const de_asset_id *ids, u32 count,
                                  u32 flags) {
    (void)s;
    (void)ids;
    (void)count;
    (void)flags;
    return stub_load_handle();
}

void de_asset_cancel(de_asset_system *s, de_load_handle h) {
    (void)s;
    (void)h;
}

void de_asset_release(de_asset_system *s, de_load_handle h) {
    (void)s;
    (void)h;
}

/* ---- Status ---- */

de_load_state de_asset_status(de_asset_system *s, de_load_handle h, de_load_info *info) {
    (void)s;
    if (info != NULL) {
        memset(info, 0, sizeof *info);
        if (DE_HANDLE_VALID(h))
            info->progress = 1.0f;
    }
    return DE_HANDLE_VALID(h) ? DE_LOAD_READY : DE_LOAD_INVALID;
}

de_load_state de_asset_status_of(de_asset_system *s, de_asset_id id) {
    (void)s;
    (void)id;
    return DE_LOAD_INVALID;
}

de_load_state de_asset_wait(de_asset_system *s, de_load_handle h, u64 timeout_ns) {
    (void)timeout_ns;
    return de_asset_status(s, h, NULL);
}

u32 de_asset_status_list(de_asset_system *s, de_load_handle h, de_asset_status_entry *out,
                         u32 max) {
    (void)s;
    (void)h;
    (void)out;
    (void)max;
    return 0;
}

/* ---- Typed access ---- */

de_texture de_asset_texture(de_asset_system *s, de_asset_id id) {
    (void)s;
    (void)id;
    de_texture h = {0};
    return h;
}

de_mesh de_asset_mesh(de_asset_system *s, de_asset_id id) {
    (void)s;
    (void)id;
    de_mesh h = {0};
    return h;
}

de_model de_asset_model(de_asset_system *s, de_asset_id id) {
    (void)s;
    (void)id;
    de_model h = {0};
    return h;
}

de_material de_asset_material(de_asset_system *s, de_asset_id id) {
    (void)s;
    (void)id;
    de_material h = {0};
    return h;
}

de_sound de_asset_sound(de_asset_system *s, de_asset_id id) {
    (void)s;
    (void)id;
    de_sound h = {0};
    return h;
}

de_font de_asset_font(de_asset_system *s, de_asset_id id) {
    (void)s;
    (void)id;
    de_font h = {0};
    return h;
}

de_prefab de_asset_prefab(de_asset_system *s, de_asset_id id) {
    (void)s;
    (void)id;
    de_prefab h = {0};
    return h;
}

de_blob de_asset_blob(de_asset_system *s, de_asset_id id) {
    (void)s;
    (void)id;
    de_blob h = {0};
    return h;
}

const void *de_blob_data(de_asset_system *s, de_blob b, u64 *size) {
    (void)s;
    (void)b;
    if (size != NULL)
        *size = 0;
    return NULL;
}

/* ---- Custom types, streaming, stats ---- */

de_asset_type de_asset_type_register(de_asset_system *s, const de_asset_type_desc *desc) {
    (void)s;
    (void)desc;
    de_asset_type t = {0};
    return t;
}

void de_asset_set_budgets(de_asset_system *s, const de_asset_budgets *b) {
    (void)s;
    (void)b;
}

void de_asset_flush_unused(de_asset_system *s) {
    (void)s;
}

void de_asset_set_streaming_sources(de_asset_system *s, const de_streaming_source *src, u32 count) {
    (void)s;
    (void)src;
    (void)count;
}

de_asset_stats de_asset_get_stats(const de_asset_system *s) {
    (void)s;
    de_asset_stats st;
    memset(&st, 0, sizeof st);
    return st;
}

/* ---- Type loader context (DENG-142). No load ever runs here, so no loader gets a context; the
 * stubs exist so that a game's own loader still links. ---- */

const void *de_asset_load_ctx_data(const de_asset_load_ctx *ctx, u64 *size) {
    (void)ctx;
    if (size != NULL)
        *size = 0;
    return NULL;
}

de_asset_id de_asset_load_ctx_id(const de_asset_load_ctx *ctx) {
    (void)ctx;
    de_asset_id id = {0};
    return id;
}

void *de_asset_load_ctx_user(const de_asset_load_ctx *ctx) {
    (void)ctx;
    return NULL;
}

bool de_asset_load_ctx_settings_has(const de_asset_load_ctx *ctx, const char *key) {
    (void)ctx;
    (void)key;
    return false;
}

bool de_asset_load_ctx_settings_bool(const de_asset_load_ctx *ctx, const char *key, bool fallback) {
    (void)ctx;
    (void)key;
    return fallback;
}

i64 de_asset_load_ctx_settings_int(const de_asset_load_ctx *ctx, const char *key, i64 fallback) {
    (void)ctx;
    (void)key;
    return fallback;
}

f64 de_asset_load_ctx_settings_float(const de_asset_load_ctx *ctx, const char *key, f64 fallback) {
    (void)ctx;
    (void)key;
    return fallback;
}

const char *de_asset_load_ctx_settings_string(const de_asset_load_ctx *ctx, const char *key,
                                              const char *fallback) {
    (void)ctx;
    (void)key;
    return fallback;
}

u32 de_asset_load_ctx_settings_string_array(const de_asset_load_ctx *ctx, const char *key,
                                            const char *const **items) {
    (void)ctx;
    (void)key;
    if (items != NULL)
        *items = NULL;
    return 0;
}

void de_asset_load_ctx_set_object(de_asset_load_ctx *ctx, void *object, u64 bytes) {
    (void)ctx;
    (void)object;
    (void)bytes;
}

de_result de_asset_load_ctx_add_dependency(de_asset_load_ctx *ctx, de_asset_id id) {
    (void)ctx;
    (void)id;
    return DE_FAIL(DE_ERR_UNSUPPORTED, "de_asset_load_ctx_add_dependency: no asset system");
}

de_result de_asset_load_ctx_fail(de_asset_load_ctx *ctx, de_result result, const char *fmt, ...) {
    (void)ctx;
    (void)fmt;
    return result == DE_OK ? DE_ERR_INTERNAL : result;
}
