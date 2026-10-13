/*
 * blob_type.c: the built-in `blob` asset type and de_blob_data (SPEC §11.1; DENG-215), and the
 * registration of every built-in type at de_asset_system_create (de__register_builtin_types).
 *
 * A blob is raw bytes. It has no `import` callback: the cooker copies a type without an importer
 * unchanged (de-cook, and packs), and in SOURCE mode the pipeline reads the source file directly,
 * so a blob is never cooked in-process and never fills the `.decache` with a second copy. The
 * asset setting `compress = true|false` matters only to the pack writer. The loader keeps a copy
 * of the bytes (the load context's bytes live only during `load`).
 */

#include "asset/asset_internal.h"
#include "asset/scene/scene_format.h"
#include "asset/slots_internal.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>

#include <string.h>

/* One allocation: this header, then the bytes and a NUL. */
typedef struct de__blob {
    u64 size;
    size_t block_size;
    const de_allocator *alloc;
} de__blob;

static u8 *blob_bytes(de__blob *b) {
    return (u8 *)(b + 1);
}

static de_result blob_load(de_asset_load_ctx *ctx) {
    de_asset_system *s = (de_asset_system *)de_asset_load_ctx_user(ctx);
    u64 n = 0;
    const void *src = de_asset_load_ctx_data(ctx, &n);
    if (n > (u64)SIZE_MAX - sizeof(de__blob) - 1u)
        return de_asset_load_ctx_fail(ctx, DE_ERR_OUT_OF_MEMORY, "blob of %llu bytes is too large",
                                      (unsigned long long)n);
    size_t block = sizeof(de__blob) + (size_t)n + 1u;
    de__blob *b = (de__blob *)DE_ALLOC(s->alloc, block, DE_ALIGNOF(de__blob), DE_MEM_ASSET);
    if (b == NULL)
        return de_asset_load_ctx_fail(ctx, DE_ERR_OUT_OF_MEMORY, "no memory for %llu bytes",
                                      (unsigned long long)n);
    b->size = n;
    b->block_size = block;
    b->alloc = s->alloc;
    if (n != 0)
        memcpy(blob_bytes(b), src, (size_t)n);
    blob_bytes(b)[n] = 0;
    de_asset_load_ctx_set_object(ctx, b, n);
    return DE_OK;
}

static void blob_unload(void *object, void *user) {
    (void)user;
    de__blob *b = (de__blob *)object;
    if (b != NULL)
        DE_FREE(b->alloc, b, b->block_size);
}

de_result de__blob_type_register(de_asset_system *s) {
    de_asset_type_desc d;
    memset(&d, 0, sizeof d);
    d.name = "blob";
    d.importer_version = 1;
    d.load = blob_load;
    d.unload = blob_unload;
    d.user = s;
    d.category = DE_ASSET_CAT_CPU;
    if (!DE_HANDLE_VALID(de_asset_type_register(s, &d))) {
        const de_error *e = de_error_last();
        return e != NULL && e->result != DE_OK ? e->result : DE_ERR_INTERNAL;
    }
    return DE_OK;
}

const void *de_blob_data(de_asset_system *s, de_blob b, u64 *size) {
    if (size != NULL)
        *size = 0;
    if (s == NULL || b.id == 0)
        return NULL;
    const de__slot *sl = de__slot_get(s, b.id);
    if (sl == NULL || sl->type_hash != de_hash64_str("blob"))
        return NULL;
    de__blob *o = (de__blob *)de__slot_object(sl);
    if (o == NULL)
        return NULL;
    if (size != NULL)
        *size = o->size;
    return blob_bytes(o);
}

de_result de__register_builtin_types(de_asset_system *s) {
    DE_TRY(de__blob_type_register(s));
    return de__scene_types_register(s);
}
