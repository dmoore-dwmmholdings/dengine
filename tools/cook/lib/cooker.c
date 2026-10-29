/*
 * cooker.c: the cooker (DENG-209, SPEC §11.5): create and destroy, and de_cooker_cook, which
 * reads one asset's source, runs its type's importer (twice with verify_determinism) and packs
 * the importer's outputs into a de_cook_result.
 *
 * Paths: the asset root and the source are virtual paths (Decision: Engine file access uses
 * virtual paths). Everything the result names is relative to the asset root, so no absolute path
 * reaches cooked data or the cache key.
 */

#include "cooker_internal.h"
#include "decache.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "platform/de_atomic.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- Platforms ------------------------------------------------------------------------------- */

de_cook_platform de_cook_platform_host(void) {
#if defined(_WIN32)
    return DE_COOK_PLATFORM_WINDOWS;
#elif defined(__APPLE__)
    return DE_COOK_PLATFORM_MACOS;
#else
    return DE_COOK_PLATFORM_LINUX;
#endif
}

const char *de_cook_platform_name(de_cook_platform p) {
    switch (p) {
    case DE_COOK_PLATFORM_HOST:
        return "host";
    case DE_COOK_PLATFORM_WINDOWS:
        return "windows";
    case DE_COOK_PLATFORM_LINUX:
        return "linux";
    case DE_COOK_PLATFORM_MACOS:
        return "macos";
    }
    return "unknown";
}

/* ---- Lifetime -------------------------------------------------------------------------------- */

de_result de_cooker_create(const de_cooker_desc *desc, de_cooker **out) {
    if (out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_cooker_create: out is NULL");
    *out = NULL;
    de_cooker_desc d = desc != NULL ? *desc : (de_cooker_desc){0};
    if ((u32)d.platform > (u32)DE_COOK_PLATFORM_MACOS)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_cooker_create: unknown platform %u",
                       (u32)d.platform);
    if ((u32)d.verify_determinism > (u32)DE_COOK_VERIFY_OFF)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_cooker_create: bad verify_determinism %u",
                       (u32)d.verify_determinism);
    const de_allocator *a = d.allocator != NULL ? d.allocator : de_heap_allocator();
    de_cooker *c = DE_NEW(a, de_cooker, DE_MEM_ASSET);
    if (c == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_cooker_create: out of memory");
    memset(c, 0, sizeof *c);
    c->alloc = a;
    c->tasks = d.tasks;
    c->platform = d.platform != DE_COOK_PLATFORM_HOST ? d.platform : de_cook_platform_host();
#if defined(DE_DEBUG)
    c->verify = d.verify_determinism != DE_COOK_VERIFY_OFF;
#else
    c->verify = d.verify_determinism == DE_COOK_VERIFY_ON;
#endif
    de_atomic_init_u32(&c->type_count, 0);
    c->types = (de__cooker_type *)DE_ALLOC(a, sizeof(de__cooker_type) * DE__COOKER_MAX_TYPES,
                                           DE_ALIGNOF(de__cooker_type), DE_MEM_ASSET);
    if (c->types == NULL) {
        DE_FREE(a, c, sizeof *c);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_cooker_create: out of memory");
    }
    if (d.cache_dir != NULL) {
        c->cache_dir_size = strlen(d.cache_dir) + 1u;
        c->cache_dir = (char *)DE_ALLOC(a, c->cache_dir_size, 1, DE_MEM_ASSET);
        if (c->cache_dir == NULL) {
            DE_FREE(a, c->types, sizeof(de__cooker_type) * DE__COOKER_MAX_TYPES);
            DE_FREE(a, c, sizeof *c);
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_cooker_create: out of memory");
        }
        memcpy(c->cache_dir, d.cache_dir, c->cache_dir_size);
    }
    /* DENG-210: the cook cache (opened on first use). */
    if (de__decache_create(c) != DE_OK) {
        de_cooker_destroy(c);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_cooker_create: out of memory");
    }
    *out = c;
    return DE_OK;
}

void de_cooker_destroy(de_cooker *c) {
    if (c == NULL)
        return;
    const de_allocator *a = c->alloc;
    de__decache_destroy(c); /* DENG-210: writes the cache index */
    if (c->cache_dir != NULL)
        DE_FREE(a, c->cache_dir, c->cache_dir_size);
    DE_FREE(a, c->types, sizeof(de__cooker_type) * DE__COOKER_MAX_TYPES);
    DE_FREE(a, c, sizeof *c);
}

de_result de_cooker_set_reader(de_cooker *c, de_cook_read_fn read, void *user) {
    if (!c || c->cache_dir)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "snapshot readers cannot use the cook cache");
    c->read = read;
    c->read_user = user;
    return DE_OK;
}
de_result de_cooker_set_texture_decode_limit(de_cooker *c, u32 mib) {
    if (!c || mib > (1u << 20))
        return DE_ERR_INVALID_ARGUMENT;
    c->texture_decode_mib = mib;
    return DE_OK;
}

de_cook_platform de_cooker_platform(const de_cooker *c) {
    return c != NULL ? c->platform : de_cook_platform_host();
}

bool de_cooker_verifies(const de_cooker *c) {
    return c != NULL && c->verify;
}

const char *de_cooker_cache_dir(const de_cooker *c) {
    return c != NULL ? c->cache_dir : NULL;
}

/* ---- One import ------------------------------------------------------------------------------ */

/* Runs the importer (or the pass-through copy for a type without one) on a prepared context.
 * DE_OK, or the failure recorded in ctx->fail_code and ctx->msg. Also used for nested imports
 * (import_subcook.c, DENG-101). */
de_result de__cooker_run_import(de_asset_import_ctx *ctx) {
    if (ctx->type->import == NULL) /* no importer: the cooked bytes are the source bytes */
        return de_asset_import_write(ctx, ctx->source, ctx->source_size);
    de_error_clear(); /* a stale error of this thread must not become the importer's message */
    de_result r = ctx->type->import(ctx);
    de__import_ctx_scratch_release(ctx);
    if (r != DE_OK && !ctx->failed) {
        const de_error *e = de_error_last();
        if (e != NULL && e->result == r) {
            /* The import ctx functions name the asset already; keep the message once. */
            char prefix[DE__COOKER_MSG_MAX];
            snprintf(prefix, sizeof prefix, "asset \"%s\": ", ctx->id_str);
            size_t pl = strlen(prefix);
            const char *m = strncmp(e->message, prefix, pl) == 0 ? e->message + pl : e->message;
            de__import_ctx_fail(ctx, r, "%s", m);
        } else {
            de__import_ctx_fail(ctx, r, "the \"%s\" importer failed: %s", ctx->type->name,
                                de_result_str(r));
        }
    }
    return ctx->failed ? ctx->fail_code : DE_OK;
}

/* ---- Result ---------------------------------------------------------------------------------- */

static size_t align_up(size_t v, size_t a) {
    return (v + (a - 1u)) & ~(a - 1u);
}

/* Packs the context's outputs into one block owned by `out`. */
static de_result build_result(de_cooker *c, const de_asset_import_ctx *ctx, u64 settings_hash,
                              u64 output_hash, de_cook_result *out) {
    size_t off_data = 0;
    size_t size = off_data + (size_t)ctx->out_size;
    size_t off_deps = align_up(size, DE_ALIGNOF(de_asset_id));
    size = off_deps + sizeof(de_asset_id) * ctx->dep_count;
    size_t off_subs = align_up(size, DE_ALIGNOF(de_cook_subasset));
    size = off_subs + sizeof(de_cook_subasset) * ctx->sub_count;
    size_t off_extra = align_up(size, DE_ALIGNOF(const char *));
    size = off_extra + sizeof(const char *) * ctx->extra_count;
    size_t off_sub_data = align_up(size, 16);
    size = off_sub_data;
    for (u32 i = 0; i < ctx->sub_count; ++i)
        size = align_up(size + (size_t)ctx->subs[i].size, 16);
    size_t off_str = size;
    size += strlen(ctx->source_rel) + 1u;
    for (u32 i = 0; i < ctx->sub_count; ++i)
        size += strlen(ctx->subs[i].name) + 1u + strlen(ctx->subs[i].type) + 1u;
    for (u32 i = 0; i < ctx->extra_count; ++i)
        size += strlen(ctx->extra[i]) + 1u;

    u8 *block = (u8 *)DE_ALLOC(c->alloc, size, 16, DE_MEM_ASSET);
    if (block == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "asset \"%s\": out of memory for the cook result",
                       ctx->id_str);
    char *str = (char *)(block + off_str);

    if (ctx->out_size != 0)
        memcpy(block + off_data, ctx->out, (size_t)ctx->out_size);
    de_asset_id *deps = (de_asset_id *)(void *)(block + off_deps);
    for (u32 i = 0; i < ctx->dep_count; ++i)
        deps[i] = ctx->deps[i];
    de_cook_subasset *subs = (de_cook_subasset *)(void *)(block + off_subs);
    const char **extra = (const char **)(void *)(block + off_extra);
    u8 *sub_data = block + off_sub_data;

    size_t n = strlen(ctx->source_rel) + 1u;
    memcpy(str, ctx->source_rel, n);
    out->source = str;
    str += n;
    for (u32 i = 0; i < ctx->sub_count; ++i) {
        const de__import_sub *s = &ctx->subs[i];
        de_cook_subasset *d = &subs[i];
        n = strlen(s->name) + 1u;
        memcpy(str, s->name, n);
        d->name = str;
        str += n;
        n = strlen(s->type) + 1u;
        memcpy(str, s->type, n);
        d->type = str;
        str += n;
        d->id = s->id;
        d->size = s->size;
        if (s->size != 0) {
            memcpy(sub_data, s->data, (size_t)s->size);
            d->data = sub_data;
            sub_data = block + align_up((size_t)(sub_data - block) + (size_t)s->size, 16);
        } else {
            d->data = "";
        }
    }
    for (u32 i = 0; i < ctx->extra_count; ++i) {
        n = strlen(ctx->extra[i]) + 1u;
        memcpy(str, ctx->extra[i], n);
        extra[i] = str;
        str += n;
    }

    out->data = ctx->out_size != 0 ? (const void *)(block + off_data) : (const void *)"";
    out->size = ctx->out_size;
    out->dependencies = ctx->dep_count != 0 ? deps : NULL;
    out->dependency_count = ctx->dep_count;
    out->subassets = ctx->sub_count != 0 ? subs : NULL;
    out->subasset_count = ctx->sub_count;
    out->extra_sources = ctx->extra_count != 0 ? extra : NULL;
    out->extra_source_count = ctx->extra_count;
    out->settings_hash = settings_hash;
    out->importer_version = ctx->type->importer_version;
    out->platform = c->platform;
    out->output_hash = output_hash;
    out->alloc_ = c->alloc;
    out->block_ = block;
    out->block_size_ = size;
    return DE_OK;
}

void de_cook_result_free(de_cook_result *r) {
    if (r == NULL)
        return;
    if (r->block_ != NULL && r->alloc_ != NULL)
        DE_FREE(r->alloc_, r->block_, r->block_size_);
    memset(r, 0, sizeof *r);
}

const de_cook_subasset *de_cook_result_subasset(const de_cook_result *r, const char *name) {
    if (r == NULL || name == NULL)
        return NULL;
    for (u32 i = 0; i < r->subasset_count; ++i)
        if (strcmp(r->subassets[i].name, name) == 0)
            return &r->subassets[i];
    return NULL;
}

/* ---- Cook ------------------------------------------------------------------------------------ */

de_result de_cooker_cook(de_cooker *c, const de_cook_input *in, de_cook_result *out) {
    if (out != NULL)
        memset(out, 0, sizeof *out);
    if (c == NULL || in == NULL || out == NULL || in->asset == NULL || in->asset_root == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_cooker_cook: NULL argument");
    const de__manifest_asset *a = in->asset;
    const char *src = in->source != NULL ? in->source : a->source;
    if (a->id == NULL || a->type == NULL || src == NULL || src[0] == '\0')
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_cooker_cook: the asset needs an id, a type and a source");
    de_asset_id id = de_asset_id_from(a->id);
    if (id.hash == 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_cooker_cook: invalid asset ID \"%s\"", a->id);
    const de__cooker_type *type = de__cooker_find_type(c, a->type);
    if (type == NULL)
        return DE_FAIL(DE_ERR_NOT_FOUND,
                       "asset \"%s\": no asset type \"%s\" is registered with the cooker", a->id,
                       a->type);

    /* The source: asset_root/src, normalised, and still below the root. */
    char root[DE_FS_PATH_MAX], joined[DE_FS_PATH_MAX], vpath[DE_FS_PATH_MAX];
    char dir[DE_FS_PATH_MAX];
    if (de_path_normalize(in->asset_root, root, sizeof root) != DE_OK)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "asset \"%s\": asset root \"%s\" is not a valid "
                       "virtual path",
                       a->id, in->asset_root);
    if (src[0] == '/' || strchr(src, ':') != NULL || strchr(src, '\\') != NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "asset \"%s\": source \"%s\" is not a path relative to the asset root",
                       a->id, src);
    int jn = snprintf(joined, sizeof joined, "%s/%s", root, src);
    const char *rel = NULL;
    if (jn < 0 || (size_t)jn >= sizeof joined ||
        de_path_normalize(joined, vpath, sizeof vpath) != DE_OK ||
        !de__cook_path_below(root, vpath, &rel) || rel[0] == '\0')
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "asset \"%s\": source \"%s\" is not a valid path below the asset root",
                       a->id, src);
    memcpy(dir, vpath, strlen(vpath) + 1u);
    char *slash = strrchr(dir, '/');
    if (slash != NULL && slash > dir && slash[-1] == ':')
        slash[1] = '\0'; /* a file at the top of the root: the directory is "<root>:/" */
    else if (slash != NULL)
        *slash = '\0';

    u64 settings_hash = de_cook_settings_hash(a->settings, a->setting_count);

    /* DENG-210, hook 1: a cache hit returns before the source is read. On a miss the lookup may
     * hand over the source bytes it read to hash them. */
    de__decache_probe probe;
    if (de__decache_lookup(c, &probe, root, rel, a->id, type->name, type->importer_version,
                           settings_hash, out) == DE_OK)
        return DE_OK;
    void *source = probe.source;
    u64 source_size = probe.source_size;
    de_result r = source != NULL ? DE_OK
                  : c->read      ? c->read(c->read_user, vpath, c->alloc, &source, &source_size)
                                 : de__cook_read_source(vpath, c->alloc, &source, &source_size);
    if (r != DE_OK) {
        const de_error *e = de_error_last();
        return DE_FAIL(r, "asset \"%s\": cannot read source \"%s\": %s", a->id, rel,
                       e != NULL && e->result == r ? e->message : de_result_str(r));
    }

    de_asset_import_ctx ctx[2];
    u32 runs = c->verify ? 2u : 1u;
    u64 hash[2] = {0, 0};
    for (u32 i = 0; i < runs; ++i) {
        de__import_ctx_init(&ctx[i], c, type);
        ctx[i].id = id;
        ctx[i].id_str = a->id;
        ctx[i].asset_root = root;
        ctx[i].source_rel = rel;
        ctx[i].source_dir = dir;
        ctx[i].source = source;
        ctx[i].source_size = source_size;
        ctx[i].settings = a->settings;
        ctx[i].setting_count = a->setting_count;
    }
    for (u32 i = 0; i < runs && r == DE_OK; ++i) {
        r = de__cooker_run_import(&ctx[i]);
        if (r != DE_OK) {
            r = DE_FAIL(r, "asset \"%s\": %s", a->id, ctx[i].msg);
            break;
        }
        hash[i] = de__import_ctx_output_hash(&ctx[i]);
    }
    if (r == DE_OK && runs == 2u && hash[0] != hash[1])
        r = DE_FAIL(DE_ERR_INTERNAL,
                    "asset \"%s\": the \"%s\" importer is not deterministic: two imports of the "
                    "same inputs gave different output (hashes %016llx and %016llx)",
                    a->id, type->name, (unsigned long long)hash[0], (unsigned long long)hash[1]);
    if (r == DE_OK)
        r = build_result(c, &ctx[0], settings_hash, hash[0], out);
    if (r == DE_OK) /* DENG-210, hook 2 */
        de__decache_store(c, &probe, source, source_size, out);
    for (u32 i = 0; i < runs; ++i)
        de__import_ctx_release(&ctx[i]);
    DE_FREE(c->alloc, source, (size_t)source_size + 1u);
    return r;
}
