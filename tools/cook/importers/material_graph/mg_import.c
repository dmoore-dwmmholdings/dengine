/*
 * mg_import.c: the `material_graph` importer (mg_import.h; DENG-770).
 */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* getenv, fopen */
#endif
#include "cook/importers/material_graph/mg_import.h"

#include "cook/importers/material_graph/material_graph.h"
#include "cook/importers/shader/shader_cook.h"
#include "cook/importers/shader/shader_importer.h"
#include "gpu/shader/shader_format.h"
#include "platform/de_atomic.h"
#include "render/formats/de_cooked_material.h"
#include "render/formats/de_cooked_material_graph.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

extern const de__slang_module de__shader_common_modules[];
extern const u32 de__shader_common_module_count;

#ifndef DE_COOK_ENGINE_SHADER_ROOT
#define DE_COOK_ENGINE_SHADER_ROOT "shaders"
#endif

#define MG_MAX_FILE    (64u * 1024u * 1024u)
#define MG_MAX_READS   1024u
#define MG_ENTRY       "cs_resolve_model"
#define MG_MODULE_PATH "material_graph.slang"

static de_log_category mg_log(void) {
    return de_log_register_category("material_graph", DE_LOG_INFO);
}

const char *de_mg_engine_shader_root(void) {
    const char *env = getenv("DE_ENGINE_SHADER_ROOT"); /* as the renderer and the editor */
    return env != NULL && env[0] != '\0' ? env : DE_COOK_ENGINE_SHADER_ROOT;
}

/* ---- Engine shader files (native paths below the engine shader root) ------------------------ */

/* Reads a whole native file into malloc'd memory (NUL appended). */
static de_result read_native(const char *path, char **data, u64 *size) {
    *data = NULL;
    *size = 0;
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return DE_ERR_NOT_FOUND;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return DE_ERR_IO;
    }
    long n = ftell(f);
    if (n < 0 || (unsigned long)n > MG_MAX_FILE || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return DE_ERR_IO;
    }
    char *b = (char *)malloc((size_t)n + 1u);
    if (b == NULL) {
        fclose(f);
        return DE_ERR_OUT_OF_MEMORY;
    }
    size_t got = fread(b, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) {
        free(b);
        return DE_ERR_IO;
    }
    b[n] = '\0';
    *data = b;
    *size = (u64)n;
    return DE_OK;
}

/* A path Slang asks for: relative, '/' separated, no "..", drive or backslash (as the editor). */
static bool safe_rel(const char *p) {
    if (p == NULL || p[0] == '\0' || p[0] == '/' || strstr(p, "..") != NULL ||
        strchr(p, '\\') != NULL || strchr(p, ':') != NULL)
        return false;
    return true;
}

typedef struct mg_reads {
    char *data[MG_MAX_READS];
    u32 count;
} mg_reads;

static de_result engine_read(void *user, const char *path, const void **data, u64 *size) {
    mg_reads *rd = (mg_reads *)user;
    if (!safe_rel(path))
        return DE_ERR_NOT_FOUND;
    if (rd->count == MG_MAX_READS)
        return DE_ERR_OUT_OF_MEMORY;
    char full[2048];
    int n = snprintf(full, sizeof full, "%s/%s", de_mg_engine_shader_root(), path);
    if (n < 0 || (size_t)n >= sizeof full)
        return DE_ERR_NOT_FOUND;
    char *b = NULL;
    DE_TRY(read_native(full, &b, size));
    rd->data[rd->count++] = b;
    *data = b;
    return DE_OK;
}

static void reads_free(mg_reads *rd) {
    for (u32 i = 0; i < rd->count; ++i)
        free(rd->data[i]);
    rd->count = 0;
}

/* ---- Importer version: hashes the engine shader tree ---------------------------------------- */

typedef struct name_list {
    char **items;
    u32 count, cap;
} name_list;

static bool names_add(name_list *l, const char *name) {
    if (l->count == l->cap) {
        u32 cap = l->cap != 0 ? l->cap * 2u : 32u;
        char **n = (char **)realloc(l->items, sizeof(char *) * cap);
        if (n == NULL)
            return false;
        l->items = n;
        l->cap = cap;
    }
    size_t len = strlen(name) + 1u;
    char *c = (char *)malloc(len);
    if (c == NULL)
        return false;
    memcpy(c, name, len);
    l->items[l->count++] = c;
    return true;
}

static void names_free(name_list *l) {
    for (u32 i = 0; i < l->count; ++i)
        free(l->items[i]);
    free(l->items);
    memset(l, 0, sizeof *l);
}

static int cmp_names(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* The entries of native directory `dir` (no "." or ".."), dirs with a trailing '/'. */
static bool list_dir(const char *dir, name_list *out) {
#if defined(_WIN32)
    char pat[2048];
    if (snprintf(pat, sizeof pat, "%s\\*", dir) >= (int)sizeof pat)
        return false;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;
        char n[512];
        snprintf(n, sizeof n, "%s%s", fd.cFileName,
                 (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? "/" : "");
        if (!names_add(out, n)) {
            FindClose(h);
            return false;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return true;
#else
    DIR *d = opendir(dir);
    if (d == NULL)
        return false;
    struct dirent *e;
    bool ok = true;
    while (ok && (e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char full[2048], n[512];
        if (snprintf(full, sizeof full, "%s/%s", dir, e->d_name) >= (int)sizeof full)
            continue;
        struct stat st;
        if (stat(full, &st) != 0)
            continue;
        snprintf(n, sizeof n, "%s%s", e->d_name, S_ISDIR(st.st_mode) ? "/" : "");
        ok = names_add(out, n);
    }
    closedir(d);
    return ok;
#endif
}

/* Hashes every .slang file below `dir` (relative path `rel`), in sorted order. */
static u64 hash_tree(const char *dir, const char *rel, u64 h, u32 depth) {
    if (depth > 16u)
        return h;
    name_list l = {0};
    if (!list_dir(dir, &l)) {
        names_free(&l);
        return h;
    }
    qsort(l.items, l.count, sizeof(char *), cmp_names);
    for (u32 i = 0; i < l.count; ++i) {
        const char *n = l.items[i];
        size_t len = strlen(n);
        char full[2048], sub[1024];
        if (n[len - 1] == '/') {
            snprintf(full, sizeof full, "%s/%.*s", dir, (int)(len - 1u), n);
            snprintf(sub, sizeof sub, "%s%s", rel, n);
            h = hash_tree(full, sub, h, depth + 1u);
            continue;
        }
        if (len < 6u || strcmp(n + len - 6u, ".slang") != 0)
            continue;
        snprintf(full, sizeof full, "%s/%s", dir, n);
        snprintf(sub, sizeof sub, "%s%s", rel, n);
        char *b = NULL;
        u64 size = 0;
        if (read_native(full, &b, &size) != DE_OK)
            continue;
        h = de_hash64(sub, strlen(sub), h);
        h = de_hash64(b, size, h);
        free(b);
    }
    names_free(&l);
    return h;
}

static de_atomic_u32 g_version;

u32 de_mg_importer_version(void) {
    u32 v = de_atomic_load_u32(&g_version, DE_ATOMIC_ACQUIRE);
    if (v != 0)
        return v;
    u64 h = de_hash64("de-material-graph-importer", 26, DE_MG_IMPORTER_BASE_VERSION);
    u32 sv = de_shader_importer_version();
    h = de_hash64(&sv, sizeof sv, h);
    h = hash_tree(de_mg_engine_shader_root(), "", h, 0);
    v = (u32)(h ^ (h >> 32));
    if (v == 0)
        v = 1u;
    de_atomic_store_u32(&g_version, v, DE_ATOMIC_RELEASE);
    return v;
}

de_asset_type_desc de_mg_import_type_desc(void) {
    de_asset_type_desc t;
    memset(&t, 0, sizeof t);
    t.name = "material_graph";
    t.importer_version = de_mg_importer_version();
    t.import = de_mg_import;
    return t;
}

/* ---- Settings -------------------------------------------------------------------------------- */

static const char *const k_builtin_models[] = {"standard", "unlit", "sprite", "terrain", "water"};

static bool model_name_ok(const char *s) {
    size_t n = strlen(s);
    if (n == 0 || n >= DE_COOKED_MATERIAL_GRAPH_NAME_SIZE)
        return false;
    for (size_t i = 0; i < n; ++i) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-'))
            return false;
    }
    return true;
}

/* The model name: the setting, or the file name up to its first '.'. */
static de_result model_name(de_asset_import_ctx *ctx, const char *path, char *out) {
    const char *set = de_asset_import_setting_string(ctx, "model", NULL);
    char buf[256];
    if (set != NULL) {
        snprintf(buf, sizeof buf, "%s", set);
    } else {
        const char *base = strrchr(path, '/');
        base = base != NULL ? base + 1 : path;
        size_t n = strcspn(base, ".");
        if (n >= sizeof buf)
            n = sizeof buf - 1u;
        memcpy(buf, base, n);
        buf[n] = '\0';
    }
    if (!model_name_ok(buf))
        return de_asset_import_fail(
            ctx, DE_ERR_INVALID_ARGUMENT,
            "%s: model name \"%s\" must be 1 to %u bytes of letters, digits, '_' and '-'%s", path,
            buf, DE_COOKED_MATERIAL_GRAPH_NAME_SIZE - 1u,
            set == NULL ? " (it comes from the file name: set `model`)" : "");
    for (u32 i = 0; i < (u32)(sizeof k_builtin_models / sizeof k_builtin_models[0]); ++i)
        if (strcmp(buf, k_builtin_models[i]) == 0)
            return de_asset_import_fail(ctx, DE_ERR_INVALID_ARGUMENT,
                                        "%s: model name \"%s\" is a built-in material model", path,
                                        buf);
    memset(out, 0, DE_COOKED_MATERIAL_GRAPH_NAME_SIZE);
    memcpy(out, buf, strlen(buf));
    return DE_OK;
}

/* ---- Errors ---------------------------------------------------------------------------------- */

/* Fails with every error diagnostic of `g` from `first` on ("node.pin: message; ..."). */
static de_result fail_diags(de_asset_import_ctx *ctx, const char *path, const de__mg_result *g,
                            u32 first, de_result rc, const char *what) {
    char msg[2048];
    size_t at = 0;
    u32 shown = 0;
    msg[0] = '\0';
    for (u32 i = first; i < g->diag_count && at < sizeof msg - 1u; ++i) {
        const de__mg_diag *d = &g->diags[i];
        if (d->severity != DE__MG_ERROR)
            continue;
        int n = snprintf(msg + at, sizeof msg - at, "%snode \"%s\"%s%s: %s", shown ? "; " : "",
                         d->node[0] ? d->node : "(graph)", d->pin[0] ? " pin " : "", d->pin,
                         d->message);
        if (n < 0)
            break;
        at += (size_t)n < sizeof msg - at ? (size_t)n : sizeof msg - at - 1u;
        ++shown;
    }
    return de_asset_import_fail(ctx, rc, "%s: %s: %s", path, what,
                                shown ? msg : "no diagnostic (see the log)");
}

static void log_warnings(const char *path, const de__mg_result *g, u32 first) {
    for (u32 i = first; i < g->diag_count; ++i)
        if (g->diags[i].severity == DE__MG_WARNING)
            DE_WARN(mg_log(), "%s: node \"%s\"%s%s: %s", path, g->diags[i].node,
                    g->diags[i].pin[0] ? " pin " : "", g->diags[i].pin, g->diags[i].message);
}

/* ---- Output ---------------------------------------------------------------------------------- */

static de_result write_dmgr(de_asset_import_ctx *ctx, const char *model, const de__mg_result *g,
                            const u8 *shader, size_t shader_size) {
    const de_allocator *a = de_asset_import_allocator(ctx);
    const size_t params_end = DE_COOKED_MATERIAL_GRAPH_HEADER_SIZE +
                              (size_t)g->param_count * DE_COOKED_MATERIAL_GRAPH_PARAM_SIZE;
    const size_t shader_at = (params_end + 15u) & ~(size_t)15u;
    const size_t total = shader_at + shader_size;
    if (total > 0xffffffffu)
        return de_asset_import_fail(ctx, DE_ERR_INVALID_ARGUMENT, "the cooked graph is too large");
    u8 *b = (u8 *)DE_ALLOC(a, total, 16, DE_MEM_ASSET);
    if (b == NULL)
        return de_asset_import_fail(ctx, DE_ERR_OUT_OF_MEMORY, "out of memory");
    memset(b, 0, total);
    memcpy(b, DE_COOKED_MATERIAL_GRAPH_MAGIC, 4);
    de_cooked_put_u32(b + 4, DE_COOKED_MATERIAL_GRAPH_VERSION);
    de_cooked_put_u32(b + 8, (u32)total);
    de_cooked_put_u32(b + 12, g->features);
    de_cooked_put_u32(b + 16, g->param_count);
    de_cooked_put_u32(b + 20, (u32)shader_at);
    de_cooked_put_u32(b + 24, (u32)shader_size);
    memcpy(b + 32, model, DE_COOKED_MATERIAL_GRAPH_NAME_SIZE);
    for (u32 i = 0; i < g->param_count; ++i) {
        const de__mg_param *p = &g->params[i];
        u8 *r = b + DE_COOKED_MATERIAL_GRAPH_HEADER_SIZE +
                (size_t)i * DE_COOKED_MATERIAL_GRAPH_PARAM_SIZE;
        de_cooked_put_u32(r, (u32)p->kind);
        de_cooked_put_u32(r + 4, p->kind == DE__MG_PARAM_TEXTURE ? 0u : p->row);
        de_cooked_put_u32(r + 8, p->kind == DE__MG_PARAM_TEXTURE ? 0u : p->component);
        de_cooked_put_u32(r + 12, p->kind == DE__MG_PARAM_TEXTURE ? p->slot : 0u);
        for (u32 k = 0; k < 3u; ++k)
            de_cooked_put_f32(r + 16 + 4u * k, p->value[k]);
        de_cooked_put_f32(r + 28, p->min);
        de_cooked_put_f32(r + 32, p->max);
        /* names: NUL padded (the generator's arrays may hold bytes after the NUL) */
        size_t nl = strnlen(p->name, sizeof p->name - 1u),
               dl = strnlen(p->node, sizeof p->node - 1u);
        memcpy(r + 40, p->name, nl);
        memcpy(r + 104, p->node, dl);
    }
    memcpy(b + shader_at, shader, shader_size);
    de_result r = de_asset_import_write(ctx, b, total);
    DE_FREE(a, b, total);
    if (r != DE_OK)
        return de_asset_import_fail(ctx, r, "writing the cooked material graph");
    return DE_OK;
}

typedef struct tex_bind {
    const char *asset; /* points into the settings */
    size_t asset_len;
} tex_bind;

/* The default material (DMAT v2): defaults at their row positions, the model's hash, textures. */
static de_result write_material(de_asset_import_ctx *ctx, const char *path, const char *model,
                                const de__mg_result *g) {
    de_cooked_material_params params = de_cooked_material_params_default();
    f32 f[DE_COOKED_MATERIAL_PARAM_FLOATS];
    memcpy(f, &params, sizeof f);
    tex_bind tex[DE_COOKED_TEX_COUNT];
    memset(tex, 0, sizeof tex);
    for (u32 i = 0; i < g->param_count; ++i) {
        const de__mg_param *p = &g->params[i];
        if (p->kind == DE__MG_PARAM_TEXTURE)
            continue;
        const u32 n = p->kind == DE__MG_PARAM_COLOR ? 3u : 1u;
        for (u32 k = 0; k < n; ++k) {
            const i32 at = de_cooked_material_graph_param_float(p->row, p->component + k);
            if (at < 0)
                return de_asset_import_fail(ctx, DE_ERR_INTERNAL,
                                            "%s: parameter \"%s\" at row %u component %u", path,
                                            p->name, p->row, p->component + k);
            f[at] = p->value[k];
        }
    }

    /* textures = ["param=asset/id", ...] */
    const char *const *items = NULL;
    const u32 count = de_asset_import_setting_string_array(ctx, "textures", &items);
    for (u32 i = 0; i < count; ++i) {
        const char *eq = strchr(items[i], '=');
        if (eq == NULL || eq == items[i] || eq[1] == '\0')
            return de_asset_import_fail(ctx, DE_ERR_INVALID_ARGUMENT,
                                        "%s: `textures`: \"%s\" is not \"<param>=<asset id>\"",
                                        path, items[i]);
        const size_t nlen = (size_t)(eq - items[i]);
        const de__mg_param *p = NULL;
        for (u32 k = 0; k < g->param_count; ++k)
            if (g->params[k].kind == DE__MG_PARAM_TEXTURE && strlen(g->params[k].name) == nlen &&
                memcmp(g->params[k].name, items[i], nlen) == 0)
                p = &g->params[k];
        if (p == NULL)
            return de_asset_import_fail(ctx, DE_ERR_INVALID_ARGUMENT,
                                        "%s: `textures`: the graph has no texture parameter "
                                        "\"%.*s\"",
                                        path, (int)nlen, items[i]);
        if (tex[p->slot].asset != NULL)
            return de_asset_import_fail(ctx, DE_ERR_INVALID_ARGUMENT,
                                        "%s: `textures`: \"%s\" is bound twice", path, p->name);
        tex[p->slot].asset = eq + 1;
        tex[p->slot].asset_len = strlen(eq + 1);
    }

    u32 tex_count = 0;
    for (u32 s = 0; s < DE_COOKED_TEX_COUNT; ++s) {
        if (tex[s].asset == NULL)
            continue;
        ++tex_count;
        de_result r = de_asset_import_add_dependency(ctx, de_asset_id_from(tex[s].asset));
        if (r != DE_OK)
            return de_asset_import_fail(ctx, r, "%s: texture \"%s\"", path, tex[s].asset);
    }

    u32 alpha = DE_COOKED_ALPHA_OPAQUE;
    const char *am = de_asset_import_setting_string(ctx, "alpha_mode", "opaque");
    if (strcmp(am, "mask") == 0)
        alpha = DE_COOKED_ALPHA_MASK;
    else if (strcmp(am, "blend") == 0)
        alpha = DE_COOKED_ALPHA_BLEND;
    else if (strcmp(am, "opaque") != 0)
        return de_asset_import_fail(ctx, DE_ERR_INVALID_ARGUMENT,
                                    "%s: `alpha_mode` \"%s\" (opaque, mask or blend)", path, am);
    const u32 flags =
        de_asset_import_setting_bool(ctx, "double_sided", false) ? DE_COOKED_MAT_DOUBLE_SIDED : 0u;

    const u32 total = DE_COOKED_MATERIAL_HEADER_SIZE + DE_COOKED_MATERIAL_PARAMS_SIZE +
                      tex_count * DE_COOKED_MATERIAL_TEX_SIZE + DE_COOKED_MATERIAL_TRAILER_SIZE;
    u8 b[DE_COOKED_MATERIAL_HEADER_SIZE + DE_COOKED_MATERIAL_PARAMS_SIZE +
         DE_COOKED_TEX_COUNT * DE_COOKED_MATERIAL_TEX_SIZE + DE_COOKED_MATERIAL_TRAILER_SIZE];
    memset(b, 0, sizeof b);
    memcpy(b, DE_COOKED_MATERIAL_MAGIC, 4);
    de_cooked_put_u32(b + 4, DE_COOKED_MATERIAL_VERSION);
    de_cooked_put_u32(b + 8, flags);
    de_cooked_put_u32(b + 12, alpha);
    de_cooked_put_u32(b + 16, DE_COOKED_MATERIAL_PARAMS_SIZE);
    de_cooked_put_u32(b + 20, tex_count);
    de_cooked_put_u32(b + 24, total);
    de_cooked_put_u32(b + 28, de_cooked_material_model_hash(model));
    for (u32 i = 0; i < DE_COOKED_MATERIAL_PARAM_FLOATS; ++i)
        de_cooked_put_f32(b + DE_COOKED_MATERIAL_HEADER_SIZE + 4u * i, f[i]);
    u8 *t = b + DE_COOKED_MATERIAL_HEADER_SIZE + DE_COOKED_MATERIAL_PARAMS_SIZE;
    for (u32 s = 0; s < DE_COOKED_TEX_COUNT; ++s) {
        if (tex[s].asset == NULL)
            continue;
        de_cooked_put_u32(t, s);
        de_cooked_put_u32(t + 4, 0u);
        de_cooked_put_u64(t + 8, de_asset_id_from(tex[s].asset).hash);
        t += DE_COOKED_MATERIAL_TEX_SIZE;
    }
    /* The trailer (DENG-774): the graph asset itself, loaded with the material. */
    de_cooked_put_u64(t, de_asset_import_id(ctx).hash);
    de_result r =
        de_asset_import_add_subasset(ctx, DE_COOKED_MATERIAL_GRAPH_MATERIAL, "material", b, total);
    if (r != DE_OK)
        return de_asset_import_fail(ctx, r, "%s: the default material", path);
    return DE_OK;
}

/* ---- The import ------------------------------------------------------------------------------ */

de_result de_mg_import(de_asset_import_ctx *ctx) {
    const char *path = de_asset_import_source_path(ctx);
    u64 size = 0;
    const char *src = (const char *)de_asset_import_source(ctx, &size);
    char model[DE_COOKED_MATERIAL_GRAPH_NAME_SIZE];
    DE_TRY(model_name(ctx, path, model));

    de__mg_result g;
    memset(&g, 0, sizeof g);
    de_result r = de__mg_generate(&(de__mg_desc){.graph = src,
                                                 .graph_size = size,
                                                 .name = model,
                                                 .allocator = de_asset_import_allocator(ctx)},
                                  &g);
    if (r != DE_OK) {
        r = fail_diags(ctx, path, &g, 0, r == DE_ERR_PARSE ? DE_ERR_PARSE : r,
                       "the material graph has errors");
        de__mg_result_free(&g);
        return r;
    }
    log_warnings(path, &g, 0);
    if (g.param_count > DE_COOKED_MATERIAL_GRAPH_MAX_PARAMS) {
        r = de_asset_import_fail(ctx, DE_ERR_INVALID_ARGUMENT, "%s: %u parameters; at most %u",
                                 path, g.param_count, DE_COOKED_MATERIAL_GRAPH_MAX_PARAMS);
        de__mg_result_free(&g);
        return r;
    }

    /* SPIR-V once through de__mg_compile, so Slang errors name their nodes. */
    mg_reads *rd = (mg_reads *)calloc(1, sizeof *rd);
    if (rd == NULL) {
        de__mg_result_free(&g);
        return de_asset_import_fail(ctx, DE_ERR_OUT_OF_MEMORY, "out of memory");
    }
    char why[512];
    if (!de__slang_available(why, sizeof why)) {
        r = de_asset_import_fail(ctx, DE_ERR_UNSUPPORTED, "%s", why);
    } else {
        const u32 before = g.diag_count;
        de__slang_compile_result cr;
        memset(&cr, 0, sizeof cr);
        r = de__mg_compile(&g,
                           &(de__mg_compile_desc){.target = DE__SLANG_SPIRV,
                                                  .path = MG_MODULE_PATH,
                                                  .read = engine_read,
                                                  .read_user = rd,
                                                  .modules = de__shader_common_modules,
                                                  .module_count = de__shader_common_module_count,
                                                  .preload_count = 1},
                           &cr);
        if (r != DE_OK)
            r = fail_diags(ctx, path, &g, before, r,
                           cr.diagnostics[0] != '\0'
                               ? "the generated shader does not compile"
                               : "the generated shader does not compile (engine shader root?)");
        de__slang_compile_result_free(&cr);
        reads_free(rd);
    }

    u8 *shader = NULL;
    size_t shader_size = 0;
    if (r == DE_OK)
        r = de__shader_cook(&(de__shader_cook_desc){.ctx = ctx,
                                                    .path = MG_MODULE_PATH,
                                                    .source = g.source,
                                                    .source_size = g.source_size,
                                                    .entry = MG_ENTRY,
                                                    .stage = DE__SHADER_STAGE_COMPUTE,
                                                    .read = engine_read,
                                                    .read_user = rd},
                            &shader, &shader_size);
    reads_free(rd);
    free(rd);
    if (r == DE_OK)
        r = write_dmgr(ctx, model, &g, shader, shader_size);
    if (r == DE_OK)
        r = write_material(ctx, path, model, &g);
    if (shader != NULL)
        DE_FREE(de_asset_import_allocator(ctx), shader, shader_size);
    de__mg_result_free(&g);
    return r;
}
