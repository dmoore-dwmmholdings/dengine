#include "host/editor/editor_scene_load.h"
#include "asset/asset_internal.h"
#include "asset/slots_internal.h"
#include "asset/resolve_internal.h"
#include "core/de_toml_internal.h"
#include "reflect/ser_internal.h"
#include <limits.h>
#include <string.h>

static const toml_datum_t *value(const toml_datum_t *t, const char *key) {
    if (!t || t->type != TOML_TABLE)
        return NULL;
    for (int32_t i = 0; i < t->u.tab.size; ++i)
        if (!strcmp(t->u.tab.key[i], key))
            return &t->u.tab.value[i];
    return NULL;
}
static de_result reference(de__scene_assets *p, const toml_datum_t *v, const char *type) {
    u64 id = 0;
    const toml_datum_t *path = NULL, *idv = NULL;
    if (v->type == TOML_STRING) {
        if (!de__ser_parse_hex64(v->u.str.ptr, (size_t)v->u.str.len, &id))
            path = v;
    } else if (v->type == TOML_TABLE) {
        idv = value(v, "id");
        path = value(v, "path");
        if (!idv && !path)
            return DE_FAIL(DE_ERR_PARSE, "scene asset needs id or path");
        if (idv) {
            if (idv->type == TOML_INT64 && idv->u.int64 >= 0)
                id = (u64)idv->u.int64;
            else if (idv->type != TOML_STRING ||
                     !de__ser_parse_hex64(idv->u.str.ptr, (size_t)idv->u.str.len, &id))
                return DE_FAIL(DE_ERR_PARSE, "scene asset id is not an unsigned identity");
        }
    } else
        return DE_FAIL(DE_ERR_PARSE, "scene asset must be an identity or path");
    de_asset_id parent = {0};
    if (path) {
        if (path->type != TOML_STRING || path->u.str.len <= 0 ||
            memchr(path->u.str.ptr, 0, (size_t)path->u.str.len))
            return DE_FAIL(DE_ERR_PARSE, "scene asset path is invalid");
        const u64 named = de_hash64(path->u.str.ptr, (size_t)path->u.str.len, 0);
        if (idv && id != named)
            return DE_FAIL(DE_ERR_PARSE, "scene asset id/path disagree");
        id = named;
        const char *hash = memchr(path->u.str.ptr, '#', (size_t)path->u.str.len);
        if (hash) {
            if (hash == path->u.str.ptr || hash + 1 == path->u.str.ptr + path->u.str.len)
                return DE_FAIL(DE_ERR_PARSE, "scene generated path has no parent or child");
            parent.hash = de_hash64(path->u.str.ptr, (size_t)(hash - path->u.str.ptr), 0);
        }
    }
    if (!id)
        return DE_OK;
    for (u32 i = 0; i < p->count; ++i)
        if (p->refs[i].id.hash == id &&
            ((!type && !p->refs[i].type) ||
             (type && p->refs[i].type && !strcmp(type, p->refs[i].type)))) {
            if (parent.hash)
                p->refs[i].parent = parent;
            return DE_OK;
        }
    const de_allocator *a = p->allocator ? p->allocator : de_heap_allocator();
    if (p->count == p->cap) {
        const u32 cap = p->cap ? p->cap * 2 : 32;
        if (cap <= p->cap || (u64)cap * sizeof(de__scene_ref) > SIZE_MAX)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "scene reference list overflow");
        de__scene_ref *grown = DE_ALLOC(a, sizeof(*grown) * cap, 8, DE_MEM_GENERAL);
        if (!grown)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "scene reference list allocation");
        if (p->refs) {
            memcpy(grown, p->refs, sizeof(*grown) * p->count);
            DE_FREE(a, p->refs, sizeof(*grown) * p->cap);
        }
        p->refs = grown;
        p->cap = cap;
    }
    de__scene_ref ref = {.id = {id}, .parent = parent};
    if (type) {
        ref.type_bytes = strlen(type) + 1;
        ref.type = DE_ALLOC(a, ref.type_bytes, 1, DE_MEM_GENERAL);
        if (!ref.type)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "scene reference type allocation");
        memcpy(ref.type, type, ref.type_bytes);
    }
    p->refs[p->count++] = ref;
    return DE_OK;
}
static de_result fields(const de_type_registry *types, de_type_id type, const toml_datum_t *table,
                        de__scene_assets *out, u32 depth);
static de_result field(const de_type_registry *types, const de_field_info *f, const toml_datum_t *v,
                       de__scene_assets *out, u32 depth) {
    if (f->kind == DE_FK_ASSET)
        return reference(out, v, f->attrs.nested_type);
    if (f->kind == DE_FK_STRUCT)
        return fields(types, de_type_find(types, f->attrs.nested_type), v, out, depth + 1);
    return DE_OK;
}
static de_result fields(const de_type_registry *types, de_type_id type, const toml_datum_t *table,
                        de__scene_assets *out, u32 depth) {
    if (depth > 64 || table->type != TOML_TABLE)
        return DE_FAIL(DE_ERR_PARSE, "scene component nesting is invalid");
    u32 count = 0;
    const de_field_info *fs = de_type_fields(types, type, &count);
    for (u32 i = 0; i < count; ++i) {
        const toml_datum_t *v = value(table, fs[i].name);
        if (!v)
            continue;
        if (fs[i].kind == DE_FK_FIXED_ARRAY) {
            const de_field_info *element = de_type_array_element(types, type, i);
            if (!element || v->type != TOML_ARRAY || (u32)v->u.arr.size > fs[i].attrs.array_count)
                return DE_FAIL(DE_ERR_PARSE, "scene fixed array is invalid");
            for (int32_t k = 0; k < v->u.arr.size; ++k)
                DE_TRY(field(types, element, &v->u.arr.elem[k], out, depth));
        } else
            DE_TRY(field(types, &fs[i], v, out, depth));
    }
    return DE_OK;
}

void de__scene_assets_drop(de__scene_assets *p) {
    if (p->assets) {
        de_asset_release(p->assets, p->discovery);
        de_asset_release(p->assets, p->exact);
    }
    const de_allocator *a = p->allocator ? p->allocator : de_heap_allocator();
    for (u32 i = 0; i < p->count; ++i)
        if (p->refs[i].type)
            DE_FREE(a, p->refs[i].type, p->refs[i].type_bytes);
    if (p->refs)
        DE_FREE(a, p->refs, sizeof(*p->refs) * p->cap);
    memset(p, 0, sizeof *p);
}

de_result de__scene_assets_begin(de__editor *ed, const char *text, size_t len,
                                 de__scene_assets *out) {
    memset(out, 0, sizeof *out);
    out->assets = ed->e->assets;
    out->allocator = ed->e->allocator ? ed->e->allocator : de_heap_allocator();
    if (len > DE_EP_MAX_FRAME || len > INT_MAX)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "scene exceeds protocol text ceiling");
    toml_result_t tr = de__toml_parse(NULL, text, (int)len);
    if (!tr.ok) {
        de_result r = DE_FAIL(DE_ERR_PARSE, "%s", tr.errmsg);
        de__toml_free(tr);
        return r;
    }
    de_result r = DE_OK;
    const toml_datum_t *entities = value(&tr.toptab, "entity");
    if (entities && entities->type != TOML_ARRAY)
        r = DE_FAIL(DE_ERR_PARSE, "scene entity must be an array");
    for (int32_t i = 0; r == DE_OK && entities && i < entities->u.arr.size; ++i) {
        const toml_datum_t *e = &entities->u.arr.elem[i];
        if (e->type != TOML_TABLE) {
            r = DE_FAIL(DE_ERR_PARSE, "scene entity must be a table");
            break;
        }
        for (int32_t k = 0; r == DE_OK && k < e->u.tab.size; ++k) {
            const char *name = e->u.tab.key[k];
            if (e->u.tab.value[k].type != TOML_TABLE || !strcmp(name, "DeChildOf") ||
                !strcmp(name, "DeChildren") || !strcmp(name, "DeGlobalTransform"))
                continue;
            const de_component c = de__ed_component(ed, de_ep_cstr(name));
            de_type_id type;
            u32 size = 0;
            if (c.id && de__ed_component_type(ed, c, &type, &size))
                r = fields(ed->e->types, type, &e->u.tab.value[k], out, 0);
        }
    }
    de__toml_free(tr);
    if (r != DE_OK)
        return r;
    if (!out->count)
        return DE_OK;
    if (!out->assets)
        return DE_FAIL(DE_ERR_NOT_FOUND, "scene references assets but no asset system exists");
    de_asset_id *parents =
        DE_ALLOC(out->allocator, sizeof(*parents) * out->count, 8, DE_MEM_GENERAL);
    if (!parents)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "scene discovery allocation");
    u32 n = 0;
    for (u32 i = 0; i < out->count; ++i) {
        if (!out->refs[i].parent.hash)
            continue;
        /* Only an explicitly named, registered parent can establish generated children. */
        if (!out->assets->resolved ||
            !de__resolved_find(out->assets->resolved, out->refs[i].parent)) {
            r = DE_FAIL(DE_ERR_NOT_FOUND, "scene generated asset has no manifest parent");
            break;
        }
        parents[n++] = out->refs[i].parent;
    }
    if (r == DE_OK && n) {
        out->discovery = de_asset_load_many(out->assets, parents, n, DE_LOAD_DEFAULT);
        if (!DE_HANDLE_VALID(out->discovery))
            r = DE_ERR_OUT_OF_MEMORY;
    }
    DE_FREE(out->allocator, parents, sizeof(*parents) * out->count);
    return r;
}
static de_result ready(de_asset_system *s, de_load_handle h) {
    if (!DE_HANDLE_VALID(h))
        return DE_OK;
    de_load_info info;
    const de_load_state state = de_asset_status(s, h, &info);
    if (state == DE_LOAD_PENDING || state == DE_LOAD_LOADING)
        return DE_ERR_BUSY;
    if (state != DE_LOAD_READY)
        return DE_FAIL(DE_ERR_NOT_FOUND, "scene asset load failed: %s", info.first_error_msg);
    return DE_OK;
}
de_result de__scene_assets_poll(de__scene_assets *p) {
    if (!p->count)
        return DE_OK;
    DE_TRY(ready(p->assets, p->discovery));
    if (!p->children_started) {
        de_asset_id *ids = DE_ALLOC(p->allocator, sizeof(*ids) * p->count, 8, DE_MEM_GENERAL);
        if (!ids)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "scene exact-reference allocation");
        de_result r = DE_OK;
        for (u32 i = 0; i < p->count; ++i) {
            ids[i] = p->refs[i].id;
            if (!de__asset_handle_of(p->assets, ids[i], p->refs[i].type)) {
                r = DE_FAIL(DE_ERR_NOT_FOUND,
                            "scene asset %016llx is unknown or has the wrong type; unresolved "
                            "legacy generated IDs require an explicit path",
                            (unsigned long long)ids[i].hash);
                break;
            }
        }
        if (r == DE_OK) {
            p->exact = de_asset_load_many(p->assets, ids, p->count, DE_LOAD_DEFAULT);
            if (!DE_HANDLE_VALID(p->exact))
                r = DE_ERR_OUT_OF_MEMORY;
        }
        DE_FREE(p->allocator, ids, sizeof(*ids) * p->count);
        if (r != DE_OK)
            return r;
        p->children_started = true;
    }
    DE_TRY(ready(p->assets, p->exact));
    for (u32 i = 0; i < p->count; ++i) {
        const u32 h = de__asset_handle_of(p->assets, p->refs[i].id, p->refs[i].type);
        if (!h || !de__asset_object(p->assets, h))
            return DE_FAIL(DE_ERR_NOT_FOUND, "scene asset is not a ready typed object");
    }
    de_asset_release(p->assets, p->discovery);
    p->discovery.id = 0;
    return DE_OK;
}
