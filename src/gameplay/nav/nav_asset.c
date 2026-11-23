/*
 * nav_asset.c: the `navmesh` asset type (DENG-76). The cooked bytes are a DNAV container
 * (nav_build.h), written by the cooker's `navmesh` importer (tools/cook/importers/navmesh/). A
 * load checks the container and keeps a copy; de_nav_add_navmesh adds its tiles to a nav.
 */
#include "gameplay/nav/nav_internal.h"

#include "asset/slots_internal.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_gameplay.h>
#include <dengine/de_memory.h>

#include <string.h>

#ifndef DE_HAS_COOKER
#define DE_HAS_COOKER 0
#endif
#if DE_HAS_COOKER
#include "cook/lib/de_cooker.h"
#endif

/** A loaded navmesh: this header, padded to 16 bytes, then the container bytes. */
typedef struct de__nav_asset {
    const de_allocator *alloc;
    size_t bytes; /* the whole block */
    u64 size;     /* the container */
} de__nav_asset;

#define DE__NAV_ASSET_HEADER ((sizeof(de__nav_asset) + 15u) & ~(size_t)15u)

static de_result navmesh_load(de_asset_load_ctx *ctx) {
    const de_allocator *a = de_heap_allocator();
    u64 size = 0;
    const void *data = de_asset_load_ctx_data(ctx, &size);
    de__nav_container_info info;
    const char *why = NULL;
    if (data == NULL || !de__nav_container_read((const u8 *)data, size, &info, &why))
        return de_asset_load_ctx_fail(ctx, DE_ERR_PARSE, "%s", why != NULL ? why : "navmesh");
    if (size > (u64)SIZE_MAX - DE__NAV_ASSET_HEADER)
        return de_asset_load_ctx_fail(ctx, DE_ERR_OUT_OF_MEMORY, "navmesh: too large");
    const size_t bytes = DE__NAV_ASSET_HEADER + (size_t)size;
    de__nav_asset *o = (de__nav_asset *)DE_ALLOC(a, bytes, 16, DE_MEM_ASSET);
    if (o == NULL)
        return de_asset_load_ctx_fail(ctx, DE_ERR_OUT_OF_MEMORY, "navmesh: out of memory");
    o->alloc = a;
    o->bytes = bytes;
    o->size = size;
    memcpy((u8 *)o + DE__NAV_ASSET_HEADER, data, (size_t)size);
    de_asset_load_ctx_set_object(ctx, o, bytes);
    return DE_OK;
}

static void navmesh_unload(void *object, void *user) {
    (void)user;
    de__nav_asset *o = (de__nav_asset *)object;
    if (o != NULL)
        DE_FREE(o->alloc, o, o->bytes);
}

static const char *const k_navmesh_ext[] = {".navmesh"};

static de_result register_asset_type(de_asset_system *assets) {
    if (DE_HANDLE_VALID(de__asset_type_find(assets, "navmesh")))
        return DE_OK;
    de_asset_type_desc td;
    memset(&td, 0, sizeof td);
    td.name = "navmesh";
    td.extensions = k_navmesh_ext;
    td.extension_count = 1;
    td.importer_version = 1;
#if DE_HAS_COOKER
    de_asset_type_desc builtin;
    if (de_cooker_builtin_importer("navmesh", &builtin)) {
        td.import = builtin.import;
        td.importer_version = builtin.importer_version;
    }
#endif
    td.load = navmesh_load;
    td.unload = navmesh_unload;
    td.user = assets;
    td.category = DE_ASSET_CAT_CPU;
    if (!DE_HANDLE_VALID(de_asset_type_register(assets, &td)))
        return DE_FAIL_WRAP(de_error_last() != NULL ? de_error_last()->result : DE_ERR_INTERNAL,
                            "navmesh asset type");
    return DE_OK;
}

de_result de_nav_use_assets(de_nav *n, de_asset_system *assets) {
    if (n == NULL || assets == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_nav_use_assets: NULL argument");
    DE_TRY(register_asset_type(assets));
    n->assets = assets;
    return DE_OK;
}

de_navmesh de_asset_navmesh(de_asset_system *s, de_asset_id id) {
    return (de_navmesh){de__asset_handle_of(s, id, "navmesh")};
}

const u8 *de__nav_asset_bytes(de_asset_system *s, de_navmesh m, u64 *size) {
    if (s == NULL || !DE_HANDLE_VALID(m))
        return NULL;
    const de__slot *sl = de__slot_get(s, m.id);
    if (sl == NULL || sl->state != DE_LOAD_READY || sl->type_hash != de_hash64_str("navmesh"))
        return NULL;
    const de__nav_asset *o = (const de__nav_asset *)de__slot_object(sl);
    if (o == NULL)
        return NULL;
    *size = o->size;
    return (const u8 *)o + DE__NAV_ASSET_HEADER;
}
