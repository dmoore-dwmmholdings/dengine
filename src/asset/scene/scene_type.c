/*
 * scene_type.c: the built-in `scene` and `prefab` asset types (SPEC §9.2, §11.6; DENG-215):
 * the check of the cooked format (de__scene_view_parse) and the runtime loader.
 * The tool-side TOML importer is in tools/cook/importers/scene/ and attaches
 * through the builtin importer bridge only in Debug/Dev builds.
 *
 * The importer reads `[[entity]]` tables (`id`, `parent`, one sub-table per component) and reads
 * each component table with the S04 text serializer into an object of its reflected type
 * (de_asset_system_desc.types), then writes the object with the S04 binary serializer
 * (self-describing, so a newer type can still read it and run `migrate`). The output is the
 * format of scene_format.h. Determinism: entities are sorted by scene ID, components by type
 * name, asset references by ID; nothing depends on the TOML key order, a hash map, a pointer or
 * the thread.
 */

#include "asset/scene/scene_format.h"

#include "asset/asset_internal.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_reflect.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Format check ---------------------------------------------------------------------------- */

static de_result view_fail(char *err, size_t cap, de_result code, const char *fmt, ...)
    DE_PRINTF_FMT(4, 5);
static de_result view_fail(char *err, size_t cap, de_result code, const char *fmt, ...) {
    if (err != NULL && cap != 0) {
        va_list ap;
        va_start(ap, fmt);
        (void)vsnprintf(err, cap, fmt, ap);
        va_end(ap);
    }
    return code;
}

de_result de__scene_view_parse(const void *data, u64 size, de__scene_view *out, char *err,
                               size_t err_cap) {
    memset(out, 0, sizeof *out);
    const u8 *b = (const u8 *)data;
    if (b == NULL || size < DE__SCENE_HEADER_SIZE || memcmp(b, DE__SCENE_MAGIC, 4) != 0)
        return view_fail(err, err_cap, DE_ERR_PARSE,
                         "not a cooked scene (no \"%s\" header; a scene source loads only "
                         "through the cooker, in Debug and Dev builds, or from a pack)",
                         DE__SCENE_MAGIC);
    u32 version = de__scene_get_u32(b + 4);
    if (version != DE__SCENE_VERSION)
        return view_fail(err, err_cap, DE_ERR_VERSION_MISMATCH,
                         "scene format version %u, this build reads %u (cook it again)",
                         (unsigned)version, (unsigned)DE__SCENE_VERSION);
    u32 kind = de__scene_get_u32(b + 8);
    de__scene_view v;
    memset(&v, 0, sizeof v);
    v.bytes = b;
    v.size = size;
    v.type_count = de__scene_get_u32(b + 12);
    v.entity_count = de__scene_get_u32(b + 16);
    v.component_count = de__scene_get_u32(b + 20);
    v.asset_count = de__scene_get_u32(b + 24);
    v.string_bytes = de__scene_get_u32(b + 28);
    v.data_bytes = de__scene_get_u64(b + 32);
    v.root_count = de__scene_get_u32(b + 40);
    if (kind > (u32)DE__SCENE_KIND_PREFAB || de__scene_get_u32(b + 44) != 0)
        return view_fail(err, err_cap, DE_ERR_PARSE, "bad scene header (kind %u)", (unsigned)kind);
    v.kind = (de__scene_kind)kind;
    if (v.type_count > DE__SCENE_MAX_COUNT || v.entity_count > DE__SCENE_MAX_COUNT ||
        v.component_count > DE__SCENE_MAX_COUNT || v.asset_count > DE__SCENE_MAX_COUNT)
        return view_fail(err, err_cap, DE_ERR_PARSE, "scene table counts out of range");
    /* Every count is below 2^24, so these sums cannot overflow a u64. */
    u64 off = DE__SCENE_HEADER_SIZE;
    u64 types_off = off;
    off += (u64)v.type_count * DE__SCENE_TYPE_SIZE;
    u64 ent_off = off;
    off += (u64)v.entity_count * DE__SCENE_ENTITY_SIZE;
    u64 comp_off = off;
    off += (u64)v.component_count * DE__SCENE_COMP_SIZE;
    u64 asset_off = off;
    off += (u64)v.asset_count * DE__SCENE_ASSET_SIZE;
    u64 str_off = off;
    off += v.string_bytes;
    u64 data_off = off;
    if (v.data_bytes > size || off > size || size - off != v.data_bytes)
        return view_fail(err, err_cap, DE_ERR_PARSE,
                         "scene size %llu does not match its tables (%llu + %llu data bytes)",
                         (unsigned long long)size, (unsigned long long)off,
                         (unsigned long long)v.data_bytes);
    v.types = b + types_off;
    v.entities = b + ent_off;
    v.components = b + comp_off;
    v.assets = b + asset_off;
    v.strings = b + str_off;
    v.data = b + data_off;

    /* Types: names inside the strings, NUL-terminated, sorted and unique. */
    const char *prev = NULL;
    u32 prev_len = 0;
    for (u32 i = 0; i < v.type_count; ++i) {
        const u8 *p = v.types + (size_t)i * DE__SCENE_TYPE_SIZE;
        u32 noff = de__scene_get_u32(p), nlen = de__scene_get_u32(p + 4);
        if (de__scene_get_u32(p + 12) != 0 || nlen == 0 || nlen > DE__SCENE_MAX_NAME ||
            (u64)noff + nlen >= v.string_bytes || v.strings[noff + nlen] != 0 ||
            memchr(v.strings + noff, 0, nlen) != NULL)
            return view_fail(err, err_cap, DE_ERR_PARSE, "scene type %u has a bad name",
                             (unsigned)i);
        const char *name = (const char *)v.strings + noff;
        if (prev != NULL) {
            u32 m = prev_len < nlen ? prev_len : nlen;
            int c = memcmp(prev, name, m);
            if (c > 0 || (c == 0 && prev_len >= nlen))
                return view_fail(err, err_cap, DE_ERR_PARSE,
                                 "scene types are not sorted and unique at %u", (unsigned)i);
        }
        prev = name;
        prev_len = nlen;
    }

    /* Entities: sorted unique IDs, contiguous component ranges, valid parents. */
    u32 next_comp = 0, roots = 0;
    u64 prev_id = 0;
    for (u32 i = 0; i < v.entity_count; ++i) {
        de__scene_entity_rec e = de__scene_entity_at(&v, i);
        const u8 *p = v.entities + (size_t)i * DE__SCENE_ENTITY_SIZE;
        if (e.scene_id == 0 || (i != 0 && e.scene_id <= prev_id) || de__scene_get_u32(p + 20) != 0)
            return view_fail(err, err_cap, DE_ERR_PARSE,
                             "scene entity %u: scene IDs are not sorted, unique and non-zero",
                             (unsigned)i);
        prev_id = e.scene_id;
        if (e.parent == DE__SCENE_NO_PARENT)
            roots++;
        else if (e.parent >= v.entity_count || e.parent == i)
            return view_fail(err, err_cap, DE_ERR_PARSE, "scene entity %u: bad parent %u",
                             (unsigned)i, (unsigned)e.parent);
        if (e.first_component != next_comp || e.component_count > v.component_count - next_comp)
            return view_fail(err, err_cap, DE_ERR_PARSE, "scene entity %u: bad component range",
                             (unsigned)i);
        next_comp += e.component_count;
        /* Components of the entity: type indices ascending (each type once). Read the type index
         * only: de__scene_comp_at also forms the data pointer, and the data offsets are checked
         * below (an unchecked offset overflows the pointer; DENG-403,
         * regress_comp_offset_overflow.dscn). */
        for (u32 k = 0; k < e.component_count; ++k) {
            const u8 *cp = v.components + (size_t)(e.first_component + k) * DE__SCENE_COMP_SIZE;
            u32 type_index = de__scene_get_u32(cp);
            if (type_index >= v.type_count ||
                (k != 0 && type_index <= de__scene_get_u32(cp - DE__SCENE_COMP_SIZE)))
                return view_fail(err, err_cap, DE_ERR_PARSE, "scene entity %u: bad component types",
                                 (unsigned)i);
        }
    }
    if (next_comp != v.component_count || roots != v.root_count)
        return view_fail(err, err_cap, DE_ERR_PARSE,
                         "scene component or root counts do not match the entities");
    if (v.kind == DE__SCENE_KIND_PREFAB && v.root_count != 1)
        return view_fail(err, err_cap, DE_ERR_PARSE, "a prefab has %u roots, not 1",
                         (unsigned)v.root_count);
    /* No parent cycle: walking up from any entity reaches a root within entity_count steps. */
    for (u32 i = 0; i < v.entity_count; ++i) {
        u32 at = i, steps = 0;
        while (at != DE__SCENE_NO_PARENT) {
            if (++steps > v.entity_count)
                return view_fail(err, err_cap, DE_ERR_PARSE, "scene entity %u is in a parent cycle",
                                 (unsigned)i);
            at = de__scene_get_u32(v.entities + (size_t)at * DE__SCENE_ENTITY_SIZE + 8);
        }
    }

    /* Component data: contiguous, in record order, exactly the data section. */
    u64 doff = 0;
    for (u32 i = 0; i < v.component_count; ++i) {
        const u8 *p = v.components + (size_t)i * DE__SCENE_COMP_SIZE;
        if (de__scene_get_u64(p + 8) != doff)
            return view_fail(err, err_cap, DE_ERR_PARSE, "scene component %u: bad data offset",
                             (unsigned)i);
        doff += de__scene_get_u32(p + 4);
    }
    if (doff != v.data_bytes)
        return view_fail(err, err_cap, DE_ERR_PARSE,
                         "scene component data does not fill %llu "
                         "bytes",
                         (unsigned long long)v.data_bytes);

    /* Asset references: sorted, unique, non-zero. */
    u64 prev_asset = 0;
    for (u32 i = 0; i < v.asset_count; ++i) {
        u64 id = de__scene_asset_at(&v, i);
        if (id == 0 || (i != 0 && id <= prev_asset))
            return view_fail(err, err_cap, DE_ERR_PARSE,
                             "scene asset references are not sorted, unique and non-zero");
        prev_asset = id;
    }
    *out = v;
    return DE_OK;
}

/* ---- Loader ------------------------------------------------------------------------------- */

static de_result scene_load_kind(de_asset_load_ctx *ctx, de__scene_kind want) {
    de_asset_system *s = (de_asset_system *)de_asset_load_ctx_user(ctx);
    u64 n = 0;
    const void *src = de_asset_load_ctx_data(ctx, &n);
    char err[192];
    de__scene_view v;
    de_result r = de__scene_view_parse(src, n, &v, err, sizeof err);
    if (r != DE_OK)
        return de_asset_load_ctx_fail(ctx, r, "%s", err);
    if (v.kind != want)
        return de_asset_load_ctx_fail(ctx, DE_ERR_PARSE, "cooked as a %s, loaded as a %s",
                                      v.kind == DE__SCENE_KIND_PREFAB ? "prefab" : "scene",
                                      want == DE__SCENE_KIND_PREFAB ? "prefab" : "scene");
    /* The components' asset references load with the scene and gate READY (the cooker declared
     * them too; duplicates count once, and a pack-loaded scene has only this list). */
    for (u32 i = 0; i < v.asset_count; ++i)
        DE_TRY(de_asset_load_ctx_add_dependency(ctx, (de_asset_id){de__scene_asset_at(&v, i)}));
    if (n > (u64)SIZE_MAX - sizeof(de__scene_object))
        return de_asset_load_ctx_fail(ctx, DE_ERR_OUT_OF_MEMORY, "scene too large");
    size_t block = sizeof(de__scene_object) + (size_t)n;
    de__scene_object *o =
        (de__scene_object *)DE_ALLOC(s->alloc, block, DE_ALIGNOF(de__scene_object), DE_MEM_ASSET);
    if (o == NULL)
        return de_asset_load_ctx_fail(ctx, DE_ERR_OUT_OF_MEMORY, "no memory for %llu bytes",
                                      (unsigned long long)n);
    u8 *bytes = (u8 *)(o + 1);
    memcpy(bytes, src, (size_t)n);
    /* Parse the copy again: the view points into the bytes the object owns. */
    r = de__scene_view_parse(bytes, n, &o->view, NULL, 0);
    DE_ASSERT(r == DE_OK);
    (void)r;
    o->alloc = s->alloc;
    o->block_size = block;
    de_asset_load_ctx_set_object(ctx, o, n);
    return DE_OK;
}

static de_result scene_load(de_asset_load_ctx *ctx) {
    return scene_load_kind(ctx, DE__SCENE_KIND_SCENE);
}
static de_result prefab_load(de_asset_load_ctx *ctx) {
    return scene_load_kind(ctx, DE__SCENE_KIND_PREFAB);
}

static void scene_unload(void *object, void *user) {
    (void)user;
    de__scene_object *o = (de__scene_object *)object;
    if (o != NULL)
        DE_FREE(o->alloc, o, o->block_size);
}

/* ---- Importer (DE_HAS_COOKER) --------------------------------------------------------------- */

/* ---- Registration ------------------------------------------------------------------------- */

static de_result scene_register_one(de_asset_system *s, const char *name, bool prefab) {
    de_asset_type_desc d;
    memset(&d, 0, sizeof d);
    d.name = name;
    d.importer_version = DE__SCENE_IMPORTER_VER;
#if DE_HAS_COOKER
    (void)de__asset_builtin_importer(name, &d);
#endif
    d.load = prefab ? prefab_load : scene_load;
    d.unload = scene_unload;
    d.user = s;
    d.category = DE_ASSET_CAT_CPU;
    if (!DE_HANDLE_VALID(de_asset_type_register(s, &d))) {
        const de_error *e = de_error_last();
        return e != NULL && e->result != DE_OK ? e->result : DE_ERR_INTERNAL;
    }
    return DE_OK;
}

de_result de__scene_types_register(de_asset_system *s) {
    DE_TRY(scene_register_one(s, DE__SCENE_TYPE_NAME, false));
    return scene_register_one(s, DE__PREFAB_TYPE_NAME, true);
}
