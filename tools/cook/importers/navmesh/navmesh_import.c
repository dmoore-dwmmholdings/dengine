/*
 * navmesh_import.c: the cooker glue of the `navmesh` importer (DENG-76). The work is
 * de__nav_cook_source (src/gameplay/nav/nav_cook.c, library de_nav_build).
 */
#include "cook/importers/navmesh/navmesh_import.h"

#include "gameplay/nav/nav_build.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>

#include <string.h>

static const char *const k_extensions[] = {".navmesh"};

de_result de_navmesh_import(de_asset_import_ctx *ctx) {
    u64 size = 0;
    const void *src = de_asset_import_source(ctx, &size);
    const de_allocator *a = de_asset_import_allocator(ctx);
    u8 *out = NULL;
    u64 out_size = 0;
    char why[256];
    de_result r = de__nav_cook_source(a, (const char *)src, size, &out, &out_size, why, sizeof why);
    if (r != DE_OK)
        return de_asset_import_fail(ctx, r, "%s", why[0] != '\0' ? why : "navmesh: cook failed");
    r = de_asset_import_write(ctx, out, out_size);
    DE_FREE(a, out, (size_t)out_size);
    if (r != DE_OK)
        return de_asset_import_fail(ctx, r, "navmesh: writing the cooked navmesh");
    return DE_OK;
}

de_asset_type_desc de_navmesh_import_type_desc(void) {
    de_asset_type_desc t;
    memset(&t, 0, sizeof t);
    t.name = "navmesh";
    t.extensions = k_extensions;
    t.extension_count = 1;
    t.importer_version = DE_NAVMESH_IMPORTER_VERSION;
    t.import = de_navmesh_import;
    return t;
}
