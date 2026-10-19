/*
 * import_ctx.c: the importer-facing API of de_asset.h (de_asset_import_*) and the import context
 * the cooker hands to a type's `import` (DENG-209, SPEC §11.5, §11.6).
 *
 * The context collects the importer's outputs (bytes, dependencies, sub-assets, extra source
 * files) in growable arrays from the cooker's allocator; cooker.c packs them into the cook
 * result. The scratch allocator (de_asset_import_allocator) keeps its blocks on a list so the
 * cooker frees whatever the importer left when the import returns.
 *
 * The cooker's lifecycle functions (de__import_ctx_*) live in this file on purpose: whenever a
 * program links the cooker, this object file comes with it, so the public de_asset_import_*
 * functions are there for importers in game code (the Dev host exports every symbol it has).
 */

#include "cooker_internal.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The streaming state (XXH3_state_t) is declared for static linking only; de_tp_xxhash is a
 * static library of the same version. */
#define XXH_STATIC_LINKING_ONLY
#include <xxhash.h>

/* ---- Scratch allocator --------------------------------------------------------------------- */

struct de__scratch_block {
    de__scratch_block *prev, *next;
    void *base;   /* what the cooker's allocator returned */
    size_t total; /* its size */
};

static size_t align_up(size_t v, size_t a) {
    return (v + (a - 1u)) & ~(a - 1u);
}

static void *scratch_alloc(void *user, size_t size, size_t align, u32 tag) {
    de_asset_import_ctx *ctx = (de_asset_import_ctx *)user;
    if (align < DE_ALIGNOF(de__scratch_block))
        align = DE_ALIGNOF(de__scratch_block);
    size_t head = sizeof(de__scratch_block);
    if (size > SIZE_MAX - head - align) {
        (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY, "import scratch: %zu bytes", size);
        return NULL;
    }
    size_t total = head + align + size;
    u8 *base = (u8 *)DE_ALLOC(ctx->c->alloc, total, DE_ALIGNOF(de__scratch_block), tag);
    if (base == NULL)
        return NULL;
    u8 *p = (u8 *)align_up((size_t)(base + head), align);
    de__scratch_block *b = (de__scratch_block *)(void *)(p - head);
    b->base = base;
    b->total = total;
    b->prev = NULL;
    b->next = ctx->scratch_head;
    if (ctx->scratch_head != NULL)
        ctx->scratch_head->prev = b;
    ctx->scratch_head = b;
    return p;
}

static void scratch_free(void *user, void *ptr, size_t size) {
    (void)size;
    if (ptr == NULL)
        return;
    de_asset_import_ctx *ctx = (de_asset_import_ctx *)user;
    de__scratch_block *b = (de__scratch_block *)(void *)((u8 *)ptr - sizeof(de__scratch_block));
    if (b->prev != NULL)
        b->prev->next = b->next;
    else
        ctx->scratch_head = b->next;
    if (b->next != NULL)
        b->next->prev = b->prev;
    DE_FREE(ctx->c->alloc, b->base, b->total);
}

static void *scratch_realloc(void *user, void *ptr, size_t old_size, size_t new_size, size_t align,
                             u32 tag) {
    void *p = scratch_alloc(user, new_size, align, tag);
    if (p == NULL || ptr == NULL)
        return p;
    memcpy(p, ptr, old_size < new_size ? old_size : new_size);
    scratch_free(user, ptr, old_size);
    return p;
}

void de__import_ctx_scratch_release(de_asset_import_ctx *ctx) {
    while (ctx->scratch_head != NULL)
        scratch_free(ctx, (u8 *)ctx->scratch_head + sizeof(de__scratch_block), 0);
}

/* ---- Lifecycle (cooker.c) ------------------------------------------------------------------ */

void de__import_ctx_init(de_asset_import_ctx *ctx, de_cooker *c, const de__cooker_type *type) {
    memset(ctx, 0, sizeof *ctx);
    ctx->c = c;
    ctx->type = type;
    ctx->scratch.alloc = scratch_alloc;
    ctx->scratch.realloc = scratch_realloc;
    ctx->scratch.free = scratch_free;
    ctx->scratch.user = ctx;
    ctx->fail_code = DE_OK;
}

static void free_str(const de_allocator *a, char *s) {
    if (s != NULL)
        DE_FREE(a, s, strlen(s) + 1u);
}

void de__import_ctx_release(de_asset_import_ctx *ctx) {
    const de_allocator *a = ctx->c->alloc;
    de__import_ctx_scratch_release(ctx);
    if (ctx->out != NULL)
        DE_FREE(a, ctx->out, (size_t)ctx->out_cap);
    if (ctx->deps != NULL)
        DE_FREE(a, ctx->deps, sizeof(de_asset_id) * ctx->dep_cap);
    for (u32 i = 0; i < ctx->sub_count; ++i) {
        de__import_sub *s = &ctx->subs[i];
        free_str(a, s->name);
        free_str(a, s->type);
        if (s->data != NULL)
            DE_FREE(a, s->data, (size_t)s->size);
    }
    if (ctx->subs != NULL)
        DE_FREE(a, ctx->subs, sizeof(de__import_sub) * ctx->sub_cap);
    for (u32 i = 0; i < ctx->extra_count; ++i)
        free_str(a, ctx->extra[i]);
    if (ctx->extra != NULL)
        DE_FREE(a, ctx->extra, sizeof(char *) * ctx->extra_cap);
    ctx->out = NULL;
    ctx->deps = NULL;
    ctx->subs = NULL;
    ctx->extra = NULL;
    ctx->out_size = ctx->out_cap = 0;
    ctx->dep_count = ctx->dep_cap = ctx->sub_count = ctx->sub_cap = 0;
    ctx->extra_count = ctx->extra_cap = 0;
}

static void ctx_vfail(de_asset_import_ctx *ctx, de_result code, const char *fmt, va_list ap) {
    if (ctx->failed)
        return;
    ctx->failed = true;
    ctx->fail_code = code != DE_OK ? code : DE_ERR_INTERNAL;
    vsnprintf(ctx->msg, sizeof ctx->msg, fmt, ap);
}

void de__import_ctx_fail(de_asset_import_ctx *ctx, de_result code, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    ctx_vfail(ctx, code, fmt, ap);
    va_end(ap);
}

/* An allocation failure of an output is a failure of the cook, even if the importer ignores the
 * result; the error context is set for the importer. */
static de_result ctx_oom(de_asset_import_ctx *ctx, const char *what) {
    de__import_ctx_fail(ctx, DE_ERR_OUT_OF_MEMORY, "out of memory (%s)", what);
    return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "import: out of memory (%s)", what);
}

/* Grows `*arr` (of `elem` bytes each, capacity `*cap`) to hold one more than `count`. */
static bool grow(const de_allocator *a, void **arr, u32 *cap, u32 count, size_t elem,
                 size_t align) {
    if (count < *cap)
        return true;
    u32 ncap = *cap != 0 ? *cap * 2u : 8u;
    if (ncap < *cap)
        return false;
    void *p = DE_REALLOC(a, *arr, elem * *cap, elem * ncap, align, DE_MEM_ASSET);
    if (p == NULL)
        return false;
    *arr = p;
    *cap = ncap;
    return true;
}

static char *dup_str(const de_allocator *a, const char *s) {
    size_t n = strlen(s) + 1u;
    char *d = (char *)DE_ALLOC(a, n, 1, DE_MEM_ASSET);
    if (d != NULL)
        memcpy(d, s, n);
    return d;
}

static void hash_u64(XXH3_state_t *st, u64 v) {
    u8 b[8];
    for (u32 i = 0; i < 8; ++i)
        b[i] = (u8)(v >> (8u * i));
    (void)XXH3_64bits_update(st, b, sizeof b);
}

static void hash_str(XXH3_state_t *st, const char *s) {
    size_t n = strlen(s);
    hash_u64(st, (u64)n);
    (void)XXH3_64bits_update(st, s, n);
}

u64 de__import_ctx_output_hash(const de_asset_import_ctx *ctx) {
    XXH3_state_t st;
    XXH3_INITSTATE(&st);
    (void)XXH3_64bits_reset(&st);
    hash_u64(&st, ctx->out_size);
    if (ctx->out_size != 0)
        (void)XXH3_64bits_update(&st, ctx->out, (size_t)ctx->out_size);
    hash_u64(&st, ctx->dep_count);
    for (u32 i = 0; i < ctx->dep_count; ++i)
        hash_u64(&st, ctx->deps[i].hash);
    hash_u64(&st, ctx->sub_count);
    for (u32 i = 0; i < ctx->sub_count; ++i) {
        const de__import_sub *s = &ctx->subs[i];
        hash_str(&st, s->name);
        hash_str(&st, s->type);
        hash_u64(&st, s->size);
        if (s->size != 0)
            (void)XXH3_64bits_update(&st, s->data, (size_t)s->size);
    }
    hash_u64(&st, ctx->extra_count);
    for (u32 i = 0; i < ctx->extra_count; ++i)
        hash_str(&st, ctx->extra[i]);
    return (u64)XXH3_64bits_digest(&st);
}

bool de__cook_path_below(const char *root, const char *path, const char **rel) {
    size_t n = strlen(root);
    if (strncmp(root, path, n) != 0)
        return false;
    if (n >= 2 && root[n - 1] == '/' && root[n - 2] == ':') { /* "dev:/": the whole root */
        *rel = path + n;
        return true;
    }
    if (path[n] == '\0') {
        *rel = path + n;
        return true;
    }
    if (path[n] != '/')
        return false;
    *rel = path + n + 1;
    return true;
}

/* ---- Inputs -------------------------------------------------------------------------------- */

const void *de_asset_import_source(const de_asset_import_ctx *ctx, u64 *size) {
    if (size != NULL)
        *size = ctx != NULL ? ctx->source_size : 0u;
    if (ctx == NULL || ctx->source == NULL)
        return "";
    return ctx->source;
}

const char *de_asset_import_source_path(const de_asset_import_ctx *ctx) {
    return ctx != NULL && ctx->source_rel != NULL ? ctx->source_rel : "";
}

de_asset_id de_asset_import_id(const de_asset_import_ctx *ctx) {
    return ctx != NULL ? ctx->id : (de_asset_id){0};
}

de_cook_platform de_asset_import_platform(const de_asset_import_ctx *ctx) {
    return ctx != NULL ? ctx->c->platform : de_cook_platform_host();
}

void *de_asset_import_user(const de_asset_import_ctx *ctx) {
    return ctx != NULL && ctx->type != NULL ? ctx->type->user : NULL;
}

/* Settings are short lists: a linear search does not depend on their order. */
static const de__manifest_kv *ctx_setting(const de_asset_import_ctx *ctx, const char *key) {
    if (ctx == NULL || key == NULL)
        return NULL;
    for (u32 i = 0; i < ctx->setting_count; ++i)
        if (ctx->settings[i].key != NULL && strcmp(ctx->settings[i].key, key) == 0)
            return &ctx->settings[i];
    return NULL;
}

const de_asset_setting_value *de_asset_import_setting_value(const de_asset_import_ctx *ctx,
                                                            const char *key) {
    const de__manifest_kv *kv = ctx_setting(ctx, key);
    return kv ? &kv->value : NULL;
}

bool de_asset_import_setting_has(const de_asset_import_ctx *ctx, const char *key) {
    return ctx_setting(ctx, key) != NULL;
}

bool de_asset_import_setting_bool(const de_asset_import_ctx *ctx, const char *key, bool fallback) {
    const de__manifest_kv *kv = ctx_setting(ctx, key);
    return kv != NULL && kv->value.type == DE__MANIFEST_BOOL ? kv->value.u.b : fallback;
}

i64 de_asset_import_setting_int(const de_asset_import_ctx *ctx, const char *key, i64 fallback) {
    const de__manifest_kv *kv = ctx_setting(ctx, key);
    return kv != NULL && kv->value.type == DE__MANIFEST_INT ? kv->value.u.i : fallback;
}

f64 de_asset_import_setting_float(const de_asset_import_ctx *ctx, const char *key, f64 fallback) {
    const de__manifest_kv *kv = ctx_setting(ctx, key);
    if (kv == NULL)
        return fallback;
    if (kv->value.type == DE__MANIFEST_FLOAT)
        return kv->value.u.f;
    if (kv->value.type == DE__MANIFEST_INT)
        return (f64)kv->value.u.i;
    return fallback;
}

const char *de_asset_import_setting_string(const de_asset_import_ctx *ctx, const char *key,
                                           const char *fallback) {
    const de__manifest_kv *kv = ctx_setting(ctx, key);
    return kv != NULL && kv->value.type == DE__MANIFEST_STRING ? kv->value.u.s : fallback;
}

u32 de_asset_import_setting_string_array(const de_asset_import_ctx *ctx, const char *key,
                                         const char *const **items) {
    const de__manifest_kv *kv = ctx_setting(ctx, key);
    bool ok = kv != NULL && kv->value.type == DE__MANIFEST_STRING_ARRAY;
    if (items != NULL)
        *items = ok ? kv->value.u.arr.items : NULL;
    return ok ? kv->value.u.arr.count : 0u;
}

/* ---- Outputs ------------------------------------------------------------------------------- */

de_result de_asset_import_read_file(de_asset_import_ctx *ctx, const char *rel_path,
                                    const void **data, u64 *size) {
    if (data != NULL)
        *data = NULL;
    if (size != NULL)
        *size = 0;
    if (ctx == NULL || rel_path == NULL || data == NULL || size == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_asset_import_read_file: NULL argument");
    if (rel_path[0] == '\0' || rel_path[0] == '/' || strchr(rel_path, ':') != NULL ||
        strchr(rel_path, '\\') != NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "asset \"%s\": \"%s\" is not a relative path (write it relative to the "
                       "source file, with '/')",
                       ctx->id_str, rel_path);
    char joined[DE_FS_PATH_MAX];
    char norm[DE_FS_PATH_MAX];
    int n = snprintf(joined, sizeof joined, "%s/%s", ctx->source_dir, rel_path);
    const char *rel = NULL;
    if (n < 0 || (size_t)n >= sizeof joined ||
        de_path_normalize(joined, norm, sizeof norm) != DE_OK ||
        !de__cook_path_below(ctx->asset_root, norm, &rel) || rel[0] == '\0')
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "asset \"%s\": \"%s\" (relative to \"%s\") leaves the asset root",
                       ctx->id_str, rel_path, ctx->source_rel);

    /* Recorded before the read: a file that is missing now still decides the output if it
     * appears later. */
    bool known = false;
    for (u32 i = 0; i < ctx->extra_count && !known; ++i)
        known = strcmp(ctx->extra[i], rel) == 0;
    if (!known) {
        const de_allocator *a = ctx->c->alloc;
        if (!grow(a, (void **)&ctx->extra, &ctx->extra_cap, ctx->extra_count, sizeof(char *),
                  DE_ALIGNOF(char *)))
            return ctx_oom(ctx, "extra source list");
        char *copy = dup_str(a, rel);
        if (copy == NULL)
            return ctx_oom(ctx, "extra source path");
        ctx->extra[ctx->extra_count++] = copy;
    }

    void *bytes = NULL;
    u64 len = 0;
    de_result r = de_file_read_all(norm, &ctx->scratch, &bytes, &len);
    if (r != DE_OK) {
        const de_error *e = de_error_last();
        return DE_FAIL(r, "asset \"%s\": cannot read \"%s\": %s", ctx->id_str, rel,
                       e != NULL && e->result == r ? e->message : de_result_str(r));
    }
    *data = bytes;
    *size = len;
    return DE_OK;
}

de_result de_asset_import_write(de_asset_import_ctx *ctx, const void *data, u64 size) {
    if (ctx == NULL || (data == NULL && size != 0))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_asset_import_write: NULL argument");
    if (size == 0)
        return DE_OK;
    if (size > (u64)SIZE_MAX / 2u - ctx->out_size)
        return ctx_oom(ctx, "cooked bytes");
    u64 need = ctx->out_size + size;
    if (need > ctx->out_cap) {
        u64 ncap = ctx->out_cap != 0 ? ctx->out_cap : 256u;
        while (ncap < need)
            ncap *= 2u;
        void *p = DE_REALLOC(ctx->c->alloc, ctx->out, (size_t)ctx->out_cap, (size_t)ncap, 16,
                             DE_MEM_ASSET);
        if (p == NULL)
            return ctx_oom(ctx, "cooked bytes");
        ctx->out = (u8 *)p;
        ctx->out_cap = ncap;
    }
    memcpy(ctx->out + ctx->out_size, data, (size_t)size);
    ctx->out_size = need;
    return DE_OK;
}

de_result de_asset_import_add_dependency(de_asset_import_ctx *ctx, de_asset_id id) {
    if (ctx == NULL || id.hash == 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_asset_import_add_dependency: NULL context or asset ID 0");
    for (u32 i = 0; i < ctx->dep_count; ++i)
        if (ctx->deps[i].hash == id.hash)
            return DE_OK;
    if (!grow(ctx->c->alloc, (void **)&ctx->deps, &ctx->dep_cap, ctx->dep_count,
              sizeof(de_asset_id), DE_ALIGNOF(de_asset_id)))
        return ctx_oom(ctx, "dependency list");
    ctx->deps[ctx->dep_count++] = id;
    return DE_OK;
}

de_result de_asset_import_add_subasset(de_asset_import_ctx *ctx, const char *name,
                                       const char *type_name, const void *data, u64 size) {
    if (ctx == NULL || name == NULL || type_name == NULL || (data == NULL && size != 0))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_asset_import_add_subasset: NULL argument");
    if (name[0] == '\0' || type_name[0] == '\0' || strchr(name, '#') != NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "asset \"%s\": sub-asset \"%s\" of type \"%s\": the name and the type "
                       "must not be empty, and the name has no '#'",
                       ctx->id_str, name, type_name);
    char full[DE_FS_PATH_MAX];
    int n = snprintf(full, sizeof full, "%s#%s", ctx->id_str, name);
    if (n < 0 || (size_t)n >= sizeof full)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "asset \"%s\": sub-asset name \"%s\" is too long",
                       ctx->id_str, name);
    de_asset_id id = de_asset_id_from(full);
    if (id.hash == 0) {
        const de_error *e = de_error_last();
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "asset \"%s\": sub-asset \"%s\": %s", ctx->id_str,
                       name, e != NULL ? e->message : "invalid asset ID");
    }
    for (u32 i = 0; i < ctx->sub_count; ++i)
        if (strcmp(ctx->subs[i].name, name) == 0)
            return DE_FAIL(DE_ERR_ALREADY_EXISTS, "asset \"%s\": sub-asset \"%s\" declared twice",
                           ctx->id_str, name);
    const de_allocator *a = ctx->c->alloc;
    if (!grow(a, (void **)&ctx->subs, &ctx->sub_cap, ctx->sub_count, sizeof(de__import_sub),
              DE_ALIGNOF(de__import_sub)))
        return ctx_oom(ctx, "sub-asset list");
    de__import_sub s = {0};
    s.id = id;
    s.size = size;
    s.name = dup_str(a, name);
    s.type = dup_str(a, type_name);
    if (size != 0) {
        s.data = (u8 *)DE_ALLOC(a, (size_t)size, 16, DE_MEM_ASSET);
        if (s.data != NULL)
            memcpy(s.data, data, (size_t)size);
    }
    if (s.name == NULL || s.type == NULL || (size != 0 && s.data == NULL)) {
        free_str(a, s.name);
        free_str(a, s.type);
        if (s.data != NULL)
            DE_FREE(a, s.data, (size_t)size);
        return ctx_oom(ctx, "sub-asset");
    }
    ctx->subs[ctx->sub_count++] = s;
    return DE_OK;
}

const de_allocator *de_asset_import_allocator(de_asset_import_ctx *ctx) {
    return ctx != NULL ? &ctx->scratch : de_allocator_null();
}

de_result de_asset_import_fail(de_asset_import_ctx *ctx, de_result result, const char *fmt, ...) {
    de_result code = result != DE_OK ? result : DE_ERR_INTERNAL;
    if (ctx == NULL || fmt == NULL)
        return code;
    va_list ap;
    va_start(ap, fmt);
    ctx_vfail(ctx, code, fmt, ap);
    va_end(ap);
    return code;
}

/* DENG-99: the cooker's task system, for importers that split one import into fixed chunks. */
de_task_system *de_cooker_import_tasks(const de_asset_import_ctx *ctx) {
    return ctx != NULL && ctx->c != NULL ? ctx->c->tasks : NULL;
}
