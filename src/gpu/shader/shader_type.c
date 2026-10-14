/*
 * shader_type.c: the runtime `shader` asset type (SPEC §11.6, §12.2; DENG-289). See
 * shader_internal.h and the public de_shader.h (DENG-409). The loader checks the cooked blob
 * (shader_format.h) and keeps a copy; the accessors find a record by (target, permutation, entry)
 * and hand out pointers into the copy.
 *
 * DENG-409: in builds with the cooker (DE_HAS_COOKER: Debug and Dev with the tools) a type
 * registered without an importer takes the cooker library's built-in shader importer
 * (de_cooker_builtin_importer, tools/cook/lib/de_cooker.h), so SOURCE mode cooks `.slang` sources
 * without the caller naming tools code. layers.toml allows gpu/shader -> cook for this.
 */
#include "gpu/shader/shader_internal.h"

#include "asset/slots_internal.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_gpu.h>
#include <dengine/de_memory.h>
#include <dengine/de_shader.h>

#include <string.h>

#if DE_HAS_COOKER
#include "cook/lib/de_cooker.h"
#endif

/* The public stage values are the cooked format's (de_shader.h, shader_format.h). */
DE_STATIC_ASSERT((int)DE_SHADER_STAGE_VERTEX == DE__SHADER_STAGE_VERTEX &&
                     (int)DE_SHADER_STAGE_PIXEL == DE__SHADER_STAGE_PIXEL &&
                     (int)DE_SHADER_STAGE_COMPUTE == DE__SHADER_STAGE_COMPUTE &&
                     (int)DE_SHADER_STAGE_MESH == DE__SHADER_STAGE_MESH &&
                     (int)DE_SHADER_STAGE_AMPLIFICATION == DE__SHADER_STAGE_AMPLIFICATION,
                 "de_shader_stage matches de__shader_stage");

/* ---- Format view ----------------------------------------------------------------------------- */

static u32 rd32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

de__shader_record de__shader_view_record(const de__shader_view *v, u32 i) {
    de__shader_record r;
    const u8 *p = v->records + (size_t)i * sizeof(de__shader_record);
    u32 *w = (u32 *)&r;
    for (u32 k = 0; k < sizeof r / 4u; ++k)
        w[k] = rd32(p + 4u * k);
    return r;
}

de__shader_entry de__shader_view_entry(const de__shader_view *v, u32 i, const char **name) {
    de__shader_entry e;
    const u8 *p = v->entries + (size_t)i * sizeof(de__shader_entry);
    e.name = rd32(p);
    e.stage = rd32(p + 4);
    if (name != NULL)
        *name = v->strings + e.name;
    return e;
}

const char *de__shader_view_permutation(const de__shader_view *v, u32 i) {
    return v->strings + rd32(v->permutations + (size_t)i * sizeof(de__shader_permutation));
}

static u64 record_key(const de__shader_record *r) {
    return ((u64)r->target << 48) | ((u64)r->permutation << 24) | (u64)r->entry;
}

i32 de__shader_view_find(const de__shader_view *v, u32 target, u32 permutation, u32 entry) {
    /* Records are sorted by (target, permutation, entry): binary search. */
    u64 want = ((u64)target << 48) | ((u64)permutation << 24) | (u64)entry;
    u32 lo = 0, hi = v->header.record_count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2u;
        de__shader_record r = de__shader_view_record(v, mid);
        u64 k = record_key(&r);
        if (k == want)
            return (i32)mid;
        if (k < want)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return -1;
}

/* The string at `off` of the table: inside it (the table ends with a NUL, so it terminates). */
static bool string_ok(const de__shader_view *v, u32 off) {
    return off < v->header.strings_size;
}

static de_result view_init(const void *data, u64 size, de__shader_view *out) {
    if (data == NULL || size < sizeof(de__shader_header))
        return DE_FAIL(DE_ERR_PARSE, "cooked shader: %llu bytes is too short",
                       (unsigned long long)size);
    const u8 *p = (const u8 *)data;
    de__shader_header h;
    h.magic = rd32(p);
    h.version = rd32(p + 4);
    h.entry_count = rd32(p + 8);
    h.permutation_count = rd32(p + 12);
    h.record_count = rd32(p + 16);
    h.strings_offset = rd32(p + 20);
    h.strings_size = rd32(p + 24);
    h.reserved = rd32(p + 28);
    if (h.magic != DE__SHADER_MAGIC)
        return DE_FAIL(DE_ERR_PARSE, "cooked shader: bad magic (not a cooked shader)");
    if (h.version != DE__SHADER_VERSION)
        return DE_FAIL(DE_ERR_VERSION_MISMATCH,
                       "cooked shader: format version %u, this build reads %u (cook it again)",
                       h.version, DE__SHADER_VERSION);
    if (h.entry_count == 0 || h.permutation_count == 0 || h.entry_count > 0xffffffu ||
        h.permutation_count > 0xffffu || h.reserved != 0)
        return DE_FAIL(DE_ERR_PARSE, "cooked shader: bad header");
    u64 tables = sizeof(de__shader_header) + (u64)h.entry_count * sizeof(de__shader_entry) +
                 (u64)h.permutation_count * sizeof(de__shader_permutation) +
                 (u64)h.record_count * sizeof(de__shader_record);
    if (tables > size || h.strings_offset != tables || h.strings_size == 0 ||
        (u64)h.strings_offset + h.strings_size > size || p[h.strings_offset + h.strings_size - 1u])
        return DE_FAIL(DE_ERR_PARSE, "cooked shader: tables out of bounds");
    out->base = p;
    out->size = size;
    out->header = h;
    out->entries = p + sizeof(de__shader_header);
    out->permutations = out->entries + (size_t)h.entry_count * sizeof(de__shader_entry);
    out->records = out->permutations + (size_t)h.permutation_count * sizeof(de__shader_permutation);
    out->strings = (const char *)p + h.strings_offset;

    for (u32 i = 0; i < h.entry_count; ++i) {
        de__shader_entry e = de__shader_view_entry(out, i, NULL);
        if (!string_ok(out, e.name) || e.stage >= DE__SHADER_STAGE_COUNT_)
            return DE_FAIL(DE_ERR_PARSE, "cooked shader: bad entry %u", i);
    }
    for (u32 i = 0; i < h.permutation_count; ++i)
        if (!string_ok(out, rd32(out->permutations + (size_t)i * sizeof(de__shader_permutation))))
            return DE_FAIL(DE_ERR_PARSE, "cooked shader: bad permutation %u", i);
    u64 prev = 0;
    for (u32 i = 0; i < h.record_count; ++i) {
        de__shader_record r = de__shader_view_record(out, i);
        u64 k = record_key(&r);
        if (r.target == DE__SHADER_TARGET_NONE || r.target >= DE__SHADER_TARGET_COUNT_ ||
            r.permutation >= h.permutation_count || r.entry >= h.entry_count ||
            r.stage >= DE__SHADER_STAGE_COUNT_ || (r.code_offset % DE__SHADER_CODE_ALIGN) != 0 ||
            r.code_offset < (u64)h.strings_offset + h.strings_size ||
            (u64)r.code_offset + r.code_size > size ||
            r.push_constant_bytes > DE__SHADER_MAX_PUSH_CONSTANT_BYTES || (i > 0 && k <= prev))
            return DE_FAIL(DE_ERR_PARSE, "cooked shader: bad record %u", i);
        prev = k;
    }
    return DE_OK;
}

de_result de__shader_view_init(const void *data, u64 size, de__shader_view *out) {
    memset(out, 0, sizeof *out);
    de_result r = view_init(data, size, out);
    if (r != DE_OK)
        memset(out, 0, sizeof *out); /* DENG-409: a refused blob leaves no half-checked view */
    return r;
}

/* ---- The loaded object ----------------------------------------------------------------------- */

/* One allocation: this header, then the blob (16-byte aligned, as its code offsets assume). */
typedef struct de__shader_obj {
    const de_allocator *alloc;
    size_t block_size;
    de__shader_view view;
} de__shader_obj;

#define DE__SHADER_OBJ_DATA_OFFSET                                                                 \
    ((sizeof(de__shader_obj) + DE__SHADER_CODE_ALIGN - 1u) & ~(size_t)(DE__SHADER_CODE_ALIGN - 1u))

#define DE__SHADER_MAX_CALLBACKS 64u

struct de__shader_types {
    const de_allocator *alloc;
    struct {
        de__shader_reload_fn fn;
        void *user;
    } cb[DE__SHADER_MAX_CALLBACKS];
    u32 cb_count;
};

/* Loads into one block from `alloc`. */
static de_result shader_load_into(de_asset_load_ctx *ctx, const de_allocator *alloc) {
    u64 n = 0;
    const void *src = de_asset_load_ctx_data(ctx, &n);
    de__shader_view check;
    de_result r = de__shader_view_init(src, n, &check);
    if (r != DE_OK) {
        const de_error *e = de_error_last();
        return de_asset_load_ctx_fail(
            ctx, r, "%s", e != NULL && e->result == r ? e->message : "not a cooked shader");
    }
    if (n > (u64)SIZE_MAX - DE__SHADER_OBJ_DATA_OFFSET)
        return de_asset_load_ctx_fail(ctx, DE_ERR_OUT_OF_MEMORY, "cooked shader too large");
    size_t block = DE__SHADER_OBJ_DATA_OFFSET + (size_t)n;
    de__shader_obj *o =
        (de__shader_obj *)DE_ALLOC(alloc, block, DE__SHADER_CODE_ALIGN, DE_MEM_ASSET);
    if (o == NULL)
        return de_asset_load_ctx_fail(ctx, DE_ERR_OUT_OF_MEMORY,
                                      "no memory for a %llu-byte cooked shader",
                                      (unsigned long long)n);
    memset(o, 0, sizeof *o);
    u8 *copy = (u8 *)o + DE__SHADER_OBJ_DATA_OFFSET;
    memcpy(copy, src, (size_t)n);
    o->alloc = alloc;
    o->block_size = block;
    (void)de__shader_view_init(copy, n, &o->view); /* checked above: the same bytes */
    de_asset_load_ctx_set_object(ctx, o, n);
    return DE_OK;
}

/* de__shader_type_register: `user` is the registration state. */
static de_result shader_load(de_asset_load_ctx *ctx) {
    const de__shader_types *t = (const de__shader_types *)de_asset_load_ctx_user(ctx);
    return shader_load_into(ctx, t->alloc);
}

/* de_shader_register_type: `user` is the allocator itself (no state to free). */
static de_result shader_load_plain(de_asset_load_ctx *ctx) {
    return shader_load_into(ctx, (const de_allocator *)de_asset_load_ctx_user(ctx));
}

static void shader_unload(void *object, void *user) {
    (void)user;
    de__shader_obj *o = (de__shader_obj *)object;
    if (o != NULL)
        DE_FREE(o->alloc, o, o->block_size);
}

static void shader_reloaded(void *object, de_asset_id id, void *user) {
    (void)object;
    de__shader_types *t = (de__shader_types *)user;
    for (u32 i = 0; i < t->cb_count; ++i)
        t->cb[i].fn(id, t->cb[i].user);
}

/* Registers the type with `load` and `user`. The importer is `import`, or in builds with the
 * cooker the built-in one when `import` is NULL. Returns the result; the error context is set. */
static de_result register_type(de_asset_system *s, de_result (*import)(de_asset_import_ctx *ctx),
                               u32 importer_version, de_result (*load)(de_asset_load_ctx *ctx),
                               void *user,
                               void (*reloaded)(void *object, de_asset_id id, void *user)) {
    static const char *const exts[] = {".slang"};
    de_asset_type_desc td;
    memset(&td, 0, sizeof td);
    td.name = "shader";
    td.extensions = exts;
    td.extension_count = 1;
    td.import = import;
    td.importer_version = import != NULL ? importer_version : 1u;
#if DE_HAS_COOKER
    de_asset_type_desc builtin;
    if (import == NULL && de_cooker_builtin_importer("shader", &builtin)) {
        td.import = builtin.import;
        td.importer_version = builtin.importer_version;
    }
#endif
    td.load = load;
    td.unload = shader_unload;
    td.user = user;
    td.category = DE_ASSET_CAT_CPU;
    td.reloaded = reloaded;
    if (!DE_HANDLE_VALID(de_asset_type_register(s, &td))) {
        const de_error *e = de_error_last();
        return e != NULL && e->result != DE_OK ? e->result : DE_ERR_INTERNAL;
    }
    return DE_OK;
}

de_result de__shader_type_register(de_asset_system *s, const de__shader_type_desc *d,
                                   de__shader_types **out) {
    if (out != NULL)
        *out = NULL;
    if (s == NULL || d == NULL || out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__shader_type_register: NULL argument");
    const de_allocator *a = d->allocator != NULL ? d->allocator : de_heap_allocator();
    de__shader_types *t = DE_NEW(a, de__shader_types, DE_MEM_ASSET);
    if (t == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "no memory for the shader type");
    memset(t, 0, sizeof *t);
    t->alloc = a;

    de_result r = register_type(s, d->import, d->importer_version, shader_load, t, shader_reloaded);
    if (r != DE_OK) {
        DE_FREE(a, t, sizeof *t);
        return r;
    }
    *out = t;
    return DE_OK;
}

de_result de_shader_register_type(de_asset_system *assets, const de_allocator *allocator) {
    if (assets == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_shader_register_type: assets is NULL");
    const de_allocator *a = allocator != NULL ? allocator : de_heap_allocator();
    /* The loader needs only the allocator: it is the type's `user`, nothing to free later. */
    return register_type(assets, NULL, 0, shader_load_plain, (void *)a, NULL);
}

void de__shader_types_destroy(de__shader_types *t) {
    if (t != NULL)
        DE_FREE(t->alloc, t, sizeof *t);
}

de_result de__shader_on_reload(de__shader_types *t, de__shader_reload_fn fn, void *user) {
    if (t == NULL || fn == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__shader_on_reload: NULL argument");
    if (t->cb_count == DE__SHADER_MAX_CALLBACKS)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de__shader_on_reload: %u callbacks already",
                       DE__SHADER_MAX_CALLBACKS);
    t->cb[t->cb_count].fn = fn;
    t->cb[t->cb_count].user = user;
    t->cb_count++;
    return DE_OK;
}

void de__shader_on_reload_remove(de__shader_types *t, de__shader_reload_fn fn, void *user) {
    if (t == NULL)
        return;
    for (u32 i = 0; i < t->cb_count; ++i) {
        if (t->cb[i].fn == fn && t->cb[i].user == user) {
            memmove(&t->cb[i], &t->cb[i + 1u], sizeof t->cb[0] * (t->cb_count - i - 1u));
            t->cb_count--;
            return;
        }
    }
}

/* ---- Accessors ------------------------------------------------------------------------------- */

de__shader_target de__shader_target_for_backend(de_gpu_backend backend) {
    if (backend == DE_GPU_DEFAULT)
        backend = de_gpu_platform_default_backend();
    switch (backend) {
    case DE_GPU_D3D12:
        return DE__SHADER_TARGET_DXIL;
    case DE_GPU_METAL:
        return DE__SHADER_TARGET_METALLIB;
    case DE_GPU_VULKAN:
    case DE_GPU_NULL:
        return DE__SHADER_TARGET_SPIRV;
    default:
        return DE__SHADER_TARGET_NONE;
    }
}

/* The loaded object behind `shader`, or NULL with `*why` set. */
static const de__shader_obj *obj_of(de_asset_system *s, de_shader shader, de_result *why) {
    *why = DE_ERR_INVALID_ARGUMENT;
    if (s == NULL || shader.id == 0)
        return NULL;
    const de__slot *sl = de__slot_get(s, shader.id);
    if (sl == NULL || sl->type_hash != de_hash64_str("shader"))
        return NULL;
    const de__shader_obj *o = (const de__shader_obj *)de__slot_object(sl);
    if (o == NULL)
        *why = DE_ERR_BUSY;
    return o;
}

de_result de__shader_blob(de_asset_system *s, de_shader shader, de_gpu_backend backend,
                          u32 permutation, const char *entry, de_gpu_shader_blob *out,
                          de__shader_reflection *refl) {
    if (out != NULL)
        memset(out, 0, sizeof *out);
    if (refl != NULL)
        memset(refl, 0, sizeof *refl);
    if (out == NULL || entry == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__shader_blob: NULL argument");
    de_result why;
    const de__shader_obj *o = obj_of(s, shader, &why);
    if (o == NULL)
        return DE_FAIL(why, why == DE_ERR_BUSY ? "de__shader_blob: the shader is not loaded"
                                               : "de__shader_blob: not a shader handle");
    const de__shader_view *v = &o->view;
    u32 e = 0;
    const char *name = NULL;
    for (; e < v->header.entry_count; ++e) {
        (void)de__shader_view_entry(v, e, &name);
        if (strcmp(name, entry) == 0)
            break;
    }
    if (e == v->header.entry_count)
        return DE_FAIL(DE_ERR_NOT_FOUND, "de__shader_blob: no entry point \"%s\"", entry);
    if (permutation >= v->header.permutation_count)
        return DE_FAIL(DE_ERR_NOT_FOUND, "de__shader_blob: permutation %u of %u", permutation,
                       v->header.permutation_count);
    de__shader_target target = de__shader_target_for_backend(backend);
    i32 i = de__shader_view_find(v, (u32)target, permutation, e);
    if (i < 0)
        return DE_FAIL(DE_ERR_NOT_FOUND,
                       "de__shader_blob: \"%s\" was not cooked for %s (the cooking host could not "
                       "compile that target)",
                       entry, de_gpu_backend_name(backend));
    de__shader_record r = de__shader_view_record(v, (u32)i);
    out->code = v->base + r.code_offset;
    out->size = r.code_size;
    out->entry = name;
    if (refl != NULL) {
        refl->stage = r.stage;
        memcpy(refl->threadgroup, r.threadgroup, sizeof refl->threadgroup);
        refl->push_constant_bytes = r.push_constant_bytes;
        refl->bindless_flags = r.bindless_flags;
        refl->mesh_max_vertices = r.mesh_limits & 0xffffu;
        refl->mesh_max_primitives = r.mesh_limits >> 16;
    }
    return DE_OK;
}

u32 de__shader_permutation_count(de_asset_system *s, de_shader shader) {
    de_result why;
    const de__shader_obj *o = obj_of(s, shader, &why);
    return o != NULL ? o->view.header.permutation_count : 0u;
}

const char *de__shader_permutation_defines(de_asset_system *s, de_shader shader, u32 i) {
    de_result why;
    const de__shader_obj *o = obj_of(s, shader, &why);
    if (o == NULL || i >= o->view.header.permutation_count)
        return NULL;
    return de__shader_view_permutation(&o->view, i);
}

de_result de_shader_get_code(de_asset_system *assets, de_shader shader, de_gpu_backend backend,
                             u32 permutation, const char *entry, de_gpu_shader_blob *out,
                             de_shader_reflection *refl) {
    if (refl != NULL)
        memset(refl, 0, sizeof *refl);
    de__shader_reflection r;
    DE_TRY(de__shader_blob(assets, shader, backend, permutation, entry, out, &r));
    if (refl != NULL) {
        refl->stage = (de_shader_stage)r.stage;
        memcpy(refl->threadgroup, r.threadgroup, sizeof refl->threadgroup);
        refl->push_constant_bytes = r.push_constant_bytes;
        refl->mesh_max_vertices = r.mesh_max_vertices;
        refl->mesh_max_primitives = r.mesh_max_primitives;
    }
    return DE_OK;
}
