/*
 * engine_shaders.c: loads the renderer's engine shaders (DENG-421); engine_shaders.h describes the
 * order (placeholder, Dev in-process cook through the cache, embedded build-time cook). The table
 * is generated from shaders/engine_shaders.cmake by embed_engine_shaders.cmake.
 *
 * The in-process cook uses exactly the inputs of tools/engine_shaders/ (asset ID
 * "engine/shaders/<name>", the source path relative to shaders/, the permutations, the host
 * platform's default targets), so its `.decache` key is the build-time cook's.
 */
#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS /* Installed distribution resource overrides. */
#endif
#include "render/shaders/engine_shaders.h"

#include "gpu/shader/shader_internal.h"

#if DE_HAS_COOKER
#include "asset/manifest_internal.h"
#include "cook/lib/de_cooker.h"
#include "cook/lib/decache.h"
#endif

#include <dengine/de_debug.h>
#include <dengine/de_platform.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Permutations of one engine shader at most. */
#define DE__ESH_MAX_PERMUTATIONS 32u

/* Test hooks (main thread, before the loads they affect). */
#if DE_HAS_COOKER
static bool g_no_source_cook;
static const char *g_cache_override;
#endif

void de__engine_shaders_set_source_cook(bool on) {
#if DE_HAS_COOKER
    g_no_source_cook = !on;
#else
    (void)on;
#endif
}

void de__engine_shaders_set_cache_dir(const char *native_dir) {
#if DE_HAS_COOKER
    g_cache_override = native_dir;
#else
    (void)native_dir;
#endif
}

const de__engine_shader_info *de__engine_shader_find(const char *name) {
    if (name == NULL)
        return NULL;
    for (u32 i = 0; i < de__engine_shader_count; ++i)
        if (strcmp(de__engine_shader_table[i].name, name) == 0)
            return &de__engine_shader_table[i];
    return NULL;
}

const char *de__engine_shader_origin_name(de__engine_shader_origin o) {
    switch (o) {
    case DE__ENGINE_SHADER_PLACEHOLDER:
        return "placeholder";
    case DE__ENGINE_SHADER_COOKED:
        return "cooked";
    case DE__ENGINE_SHADER_CACHED:
        return "cached";
    case DE__ENGINE_SHADER_EMBEDDED:
        return "embedded";
    case DE__ENGINE_SHADER_NONE:
        break;
    }
    return "none";
}

static void set_why(char *why, size_t cap, const char *fmt, const char *a, const char *b) {
    if (why != NULL && cap > 0)
        snprintf(why, cap, fmt, a, b);
}

/* The record of (target, permutation, entry name) in a checked view, or false. */
static bool find_record(const de__shader_view *v, u32 target, u32 perm, const char *entry,
                        de__shader_record *out) {
    for (u32 e = 0; e < v->header.entry_count; ++e) {
        const char *name = NULL;
        (void)de__shader_view_entry(v, e, &name);
        if (name == NULL || strcmp(name, entry) != 0)
            continue;
        const i32 ri = de__shader_view_find(v, target, perm, e);
        if (ri < 0)
            return false;
        *out = de__shader_view_record(v, (u32)ri);
        return true;
    }
    return false;
}

/* Fills `out` from a cooked blob: copies the entries' code into one block of `alloc` when `copy`
 * (the blob is about to go), else points into it (static embedded data). */
static de_result take_entries(const void *blob, u64 size, de__shader_target target, u32 perm,
                              const char *const *entries, u32 count, bool copy,
                              const de_allocator *alloc, de__engine_shader_code *out, char *why,
                              size_t cap) {
    de__shader_view v;
    if (de__shader_view_init(blob, size, &v) != DE_OK) {
        set_why(why, cap, "%s%s", "the cooked blob is damaged", "");
        return DE_ERR_PARSE;
    }
    de__shader_record rec[DE__ENGINE_SHADER_MAX_ENTRIES];
    size_t total = 0;
    for (u32 i = 0; i < count; ++i) {
        if (!find_record(&v, (u32)target, perm, entries[i], &rec[i])) {
            set_why(why, cap, "no entry point \"%s\" (or not for this backend's target%s)",
                    entries[i], ", or no such permutation");
            return DE_ERR_NOT_FOUND;
        }
        total += (size_t)rec[i].code_size;
    }
    u8 *dst = NULL;
    if (copy) {
        dst = (u8 *)DE_ALLOC(alloc, total, 16, DE_MEM_RENDER);
        if (dst == NULL)
            return DE_ERR_OUT_OF_MEMORY;
        out->block = dst;
        out->block_size = total;
        out->alloc = alloc;
    }
    size_t off = 0;
    out->count = count;
    for (u32 i = 0; i < count; ++i) {
        const u8 *code = v.base + rec[i].code_offset;
        if (copy) {
            memcpy(dst + off, code, (size_t)rec[i].code_size);
            code = dst + off;
            off += (size_t)rec[i].code_size;
        }
        de__engine_shader_entry *e = &out->entries[i];
        e->blob = (de_gpu_shader_blob){.code = code, .size = rec[i].code_size, .entry = entries[i]};
        e->stage = rec[i].stage;
        memcpy(e->threadgroup, rec[i].threadgroup, sizeof e->threadgroup);
        e->push_constant_bytes = rec[i].push_constant_bytes;
        if (e->push_constant_bytes > out->push_constant_bytes)
            out->push_constant_bytes = e->push_constant_bytes;
    }
    return DE_OK;
}

#if DE_HAS_COOKER

/* Mounts `root` at `native` once per process (never unmounted); remounts when `native` changes. */
static bool mount_once(const char *root, const char *native, u32 flags, char *mounted, size_t cap) {
    if (strcmp(mounted, native) == 0)
        return true;
    if (mounted[0] != '\0')
        (void)de_fs_unmount(root, mounted);
    mounted[0] = '\0';
    if (de_fs_mount(root, native, flags, 0) != DE_OK)
        return false;
    snprintf(mounted, cap, "%s", native);
    return true;
}

/* Step 2: cooks the source in-process through the cache. */
static de_result source_cook(const de__engine_shader_info *info, de__shader_target target, u32 perm,
                             const char *const *entries, u32 count, const de_allocator *alloc,
                             de__engine_shader_code *out, de_log_category log, char *why,
                             size_t cap) {
    static char mounted_root[DE_FS_PATH_MAX], mounted_cache[DE_FS_PATH_MAX];
    de_asset_type_desc t;
    if (!de_cooker_builtin_importer("shader", &t)) {
        set_why(why, cap, "%s%s", "no shader importer in this build (no Slang)", "");
        return DE_ERR_UNSUPPORTED;
    }
    const char *root = getenv("DE_ENGINE_SHADER_ROOT");
    if (root == NULL || root[0] == '\0') root = DE_RENDER_ENGINE_SHADER_ROOT;
    const char *cache = g_cache_override != NULL ? g_cache_override : getenv("DE_ENGINE_SHADER_CACHE");
    if (cache == NULL || cache[0] == '\0') cache = DE_RENDER_ENGINE_SHADER_CACHE;
    if (!mount_once("deengshd", root, 0, mounted_root,
                    sizeof mounted_root)) {
        set_why(why, cap, "the shader sources are not at %s%s", root, "");
        return DE_ERR_NOT_FOUND;
    }
    const bool cached =
        mount_once("deengcache", cache, DE_MOUNT_WRITABLE, mounted_cache, sizeof mounted_cache);
    de_cooker *ck = NULL;
    if (de_cooker_create(&(de_cooker_desc){.allocator = alloc,
                                           .cache_dir = cached ? "deengcache:/" : NULL,
                                           .verify_determinism = DE_COOK_VERIFY_OFF},
                         &ck) != DE_OK ||
        de_cooker_register_type(ck, &t) != DE_OK) {
        de_cooker_destroy(ck);
        set_why(why, cap, "%s%s", "cannot create the cooker", "");
        return DE_ERR_INTERNAL;
    }
    /* The permutations, as the build-time cook gives them (',' -> ';'). */
    char perm_text[DE__ESH_MAX_PERMUTATIONS][256];
    const char *perm_items[DE__ESH_MAX_PERMUTATIONS];
    u32 perm_count = 0;
    for (const char *p = info->permutations; *p != '\0' && perm_count < DE__ESH_MAX_PERMUTATIONS;) {
        const char *end = strchr(p, '/');
        const size_t n = end != NULL ? (size_t)(end - p) : strlen(p);
        char *d = perm_text[perm_count];
        snprintf(d, sizeof perm_text[0], "%.*s", (int)n, p);
        for (char *c = d; *c != '\0'; ++c)
            if (*c == ',')
                *c = ';';
        perm_items[perm_count] = d;
        ++perm_count;
        p += n + (end != NULL ? 1u : 0u);
    }
    char id[160];
    snprintf(id, sizeof id, "engine/shaders/%s", info->name);
    de__manifest_kv kv;
    memset(&kv, 0, sizeof kv);
    kv.key = "permutations";
    kv.value.type = DE__MANIFEST_STRING_ARRAY;
    kv.value.u.arr.items = perm_items;
    kv.value.u.arr.count = perm_count;
    de__manifest_asset a;
    memset(&a, 0, sizeof a);
    a.id = id;
    a.type = "shader";
    a.source = info->source;
    a.file = "deengshd:/engine_shaders.cmake";
    a.settings = perm_count > 0 ? &kv : NULL;
    a.setting_count = perm_count > 0 ? 1u : 0u;
    de_cook_result res;
    memset(&res, 0, sizeof res);
    const de_cook_input in = {.asset = &a, .asset_root = "deengshd:/", .source = NULL};
    const u64 t0 = de_time_now_ns();
    de_result r = de_cooker_cook(ck, &in, &res);
    if (r != DE_OK) {
        const de_error *e = de_error_last();
        set_why(why, cap, "cooking it failed: %.300s%s", e != NULL ? e->message : "?", "");
    } else {
        de_cooker_cache_stats st;
        de_cooker_get_cache_stats(ck, &st);
        r = take_entries(res.data, res.size, target, perm, entries, count, true, alloc, out, why,
                         cap);
        if (r == DE_OK) {
            out->origin = st.hits > 0 ? DE__ENGINE_SHADER_CACHED : DE__ENGINE_SHADER_COOKED;
            DE_INFO(log, "engine shader \"%s\": %s (%.1f ms)", info->name,
                    st.hits > 0 ? "cache hit" : "cooked from source",
                    (double)(de_time_now_ns() - t0) / 1e6);
        }
        de_cook_result_free(&res);
    }
    de_cooker_destroy(ck);
    return r;
}

#endif /* DE_HAS_COOKER */

/* The null backend compiles nothing: any non-empty code will do. */
static const u32 k_placeholder_code[4] = {0x07230203u, 0x00010600u, 0u, 0u};

de_result de__engine_shader_load(de_gpu_device *dev, const de_allocator *alloc, const char *name,
                                 u32 permutation, const char *const *entries, u32 count,
                                 de__engine_shader_code *out, char *why, size_t why_cap) {
    if (out != NULL)
        memset(out, 0, sizeof *out);
    if (why != NULL && why_cap > 0)
        why[0] = '\0';
    if (dev == NULL || name == NULL || entries == NULL || out == NULL || count == 0 ||
        count > DE__ENGINE_SHADER_MAX_ENTRIES)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__engine_shader_load(\"%s\"): bad arguments",
                       name != NULL ? name : "");
    const de__engine_shader_info *info = de__engine_shader_find(name);
    if (info == NULL) {
        set_why(why, why_cap, "\"%s\" is not in shaders/engine_shaders.cmake%s", name, "");
        return DE_FAIL(DE_ERR_NOT_FOUND, "de__engine_shader_load: no engine shader \"%s\"", name);
    }
    const de_allocator *a = alloc != NULL ? alloc : de_heap_allocator();
    const de_gpu_backend backend = de_gpu_get_caps(dev)->backend;
    const de__shader_target target = de__shader_target_for_backend(backend);

    if (backend == DE_GPU_NULL) {
        /* Reflection from the embedded SPIR-V when there is one (the null device takes SPIR-V). */
        de__engine_shader_code refl;
        memset(&refl, 0, sizeof refl);
        if (info->blob != NULL)
            (void)take_entries(info->blob, info->blob_size, DE__SHADER_TARGET_SPIRV, permutation,
                               entries, count, false, a, &refl, NULL, 0);
        out->count = count;
        out->origin = DE__ENGINE_SHADER_PLACEHOLDER;
        out->push_constant_bytes = refl.push_constant_bytes;
        for (u32 i = 0; i < count; ++i) {
            out->entries[i] = refl.entries[i];
            out->entries[i].blob = (de_gpu_shader_blob){
                .code = k_placeholder_code, .size = sizeof k_placeholder_code, .entry = entries[i]};
        }
        return DE_OK;
    }

    char cook_why[400] = "";
    de_result r = DE_ERR_NOT_FOUND;
#if DE_HAS_COOKER
    if (!g_no_source_cook) {
        r = source_cook(info, target, permutation, entries, count, a, out,
                        de_log_register_category("render", DE_LOG_INFO), cook_why, sizeof cook_why);
        if (r == DE_OK)
            return DE_OK;
        memset(out, 0, sizeof *out);
    } else {
        snprintf(cook_why, sizeof cook_why, "the in-process cook is off");
    }
#else
    snprintf(cook_why, sizeof cook_why, "no cooker in this build");
#endif
    char emb_why[300] = "";
    if (info->blob == NULL) {
        snprintf(emb_why, sizeof emb_why,
                 "no build-time cook (a cross build, or no tools or Slang on the build host)");
        r = DE_ERR_NOT_FOUND;
    } else {
        r = take_entries(info->blob, info->blob_size, target, permutation, entries, count, false, a,
                         out, emb_why, sizeof emb_why);
        if (r == DE_OK) {
            out->origin = DE__ENGINE_SHADER_EMBEDDED;
            return DE_OK;
        }
        memset(out, 0, sizeof *out);
    }
    if (why != NULL && why_cap > 0)
        snprintf(why, why_cap, "%s for %s: in-process: %s; embedded: %s", name,
                 de_gpu_backend_name(backend), cook_why, emb_why);
    return DE_FAIL(r == DE_ERR_OUT_OF_MEMORY ? r : DE_ERR_NOT_FOUND,
                   "de__engine_shader_load(\"%s\") for %s: in-process: %s; embedded: %s", name,
                   de_gpu_backend_name(backend), cook_why, emb_why);
}

void de__engine_shader_code_free(de__engine_shader_code *c) {
    if (c == NULL)
        return;
    if (c->block != NULL && c->alloc != NULL)
        DE_FREE(c->alloc, c->block, c->block_size);
    memset(c, 0, sizeof *c);
}
