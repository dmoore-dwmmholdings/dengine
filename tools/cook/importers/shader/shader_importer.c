/*
 * shader_importer.c: the `shader` importer (DENG-289). See shader_importer.h for the settings and
 * src/gpu/shader/shader_format.h for the output.
 *
 * For each target (SPIR-V, DXIL, metallib, in that order) and each permutation the source is
 * compiled once through the Slang shim (slang_compile.h), all entry points at a time. A SPIR-V
 * compile also runs when SPIR-V is not an output target: the bindless flags and the mesh output
 * limits are read from the SPIR-V binary (decorations and execution modes), which is the same for
 * every target because the source is.
 *
 * Determinism (SPEC §11.5): the output depends only on the source, the files read through the
 * import context, the settings, the embedded modules and the Slang version (all in the cache key)
 * and on which targets the host can compile (a skipped target is logged). No path, time or
 * pointer goes into the bytes; records are written in (target, permutation, entry) order.
 */
#include "cook/importers/shader/shader_importer.h"

#include "cook/importers/shader/slang_compile.h"
#include "gpu/shader/shader_format.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h> /* DE_FS_PATH_MAX */

#include <slang-tag-version.h>

#include <stdio.h>
#include <string.h>

/* The embedded shaders/common/ modules (generated: shader_common_embed.c), bindless first. */
extern const de__slang_module de__shader_common_modules[];
extern const u32 de__shader_common_module_count;

const char *const de_shader_import_extensions[DE_SHADER_IMPORT_EXTENSION_COUNT] = {".slang"};

#define MAX_ENTRIES      64u
#define MAX_PERMUTATIONS 256u
#define MAX_DEFINES      32u
#define ENTRY_NAME_MAX   128u
#define COUNT_OF(a)      (sizeof(a) / sizeof((a)[0]))
#define ALIGN_UP(x, a)   (((x) + ((a) - 1u)) & ~(size_t)((a) - 1u))

u32 de_shader_importer_version(void) {
    u64 h = de_hash64("de-shader-importer", 18, DE_SHADER_IMPORTER_BASE_VERSION);
    h = de_hash64(SLANG_TAG_VERSION, strlen(SLANG_TAG_VERSION), h);
    for (u32 i = 0; i < de__shader_common_module_count; ++i) {
        const de__slang_module *m = &de__shader_common_modules[i];
        h = de_hash64(m->name, strlen(m->name), h);
        h = de_hash64(m->source, m->size, h);
    }
    u32 v = (u32)(h ^ (h >> 32));
    return v != 0 ? v : 1u;
}

static de_log_category shader_log(void) {
    return de_log_register_category("shader", DE_LOG_INFO);
}

/* ---- Settings -------------------------------------------------------------------------------- */

typedef struct entry_spec {
    char name[ENTRY_NAME_MAX];
    u32 stage; /* de__shader_stage; NONE = from the attribute */
} entry_spec;

static bool stage_from_name(const char *s, u32 *out) {
    static const struct {
        const char *name;
        u32 stage;
    } k[] = {{"vertex", DE__SHADER_STAGE_VERTEX},
             {"pixel", DE__SHADER_STAGE_PIXEL},
             {"fragment", DE__SHADER_STAGE_PIXEL},
             {"compute", DE__SHADER_STAGE_COMPUTE},
             {"mesh", DE__SHADER_STAGE_MESH},
             {"amplification", DE__SHADER_STAGE_AMPLIFICATION},
             {"task", DE__SHADER_STAGE_AMPLIFICATION},
             {"raygen", DE__SHADER_STAGE_RAYGEN},
             {"raygeneration", DE__SHADER_STAGE_RAYGEN},
             {"miss", DE__SHADER_STAGE_MISS},
             {"closesthit", DE__SHADER_STAGE_CLOSEST_HIT},
             {"anyhit", DE__SHADER_STAGE_ANY_HIT}};
    for (u32 i = 0; i < COUNT_OF(k); ++i) {
        if (strcmp(s, k[i].name) == 0) {
            *out = k[i].stage;
            return true;
        }
    }
    return false;
}

static bool is_ident(const char *s, size_t n) {
    if (n == 0)
        return false;
    for (size_t i = 0; i < n; ++i) {
        char c = s[i];
        bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        if (!alpha && !(i > 0 && c >= '0' && c <= '9'))
            return false;
    }
    return true;
}

static de_result parse_entries(de_asset_import_ctx *ctx, entry_spec *out, u32 *count) {
    const char *const *items = NULL;
    u32 n = de_asset_import_setting_string_array(ctx, "entry_points", &items);
    *count = 0;
    if (n > MAX_ENTRIES)
        return de_asset_import_fail(ctx, DE_ERR_INVALID_ARGUMENT,
                                    "`entry_points` lists %u entry points; at most %u", n,
                                    MAX_ENTRIES);
    for (u32 i = 0; i < n; ++i) {
        const char *item = items[i];
        const char *colon = strchr(item, ':');
        size_t len = colon != NULL ? (size_t)(colon - item) : strlen(item);
        entry_spec *e = &out[i];
        if (!is_ident(item, len) || len >= ENTRY_NAME_MAX)
            return de_asset_import_fail(ctx, DE_ERR_INVALID_ARGUMENT,
                                        "`entry_points`: \"%s\" is not \"name\" or \"name:stage\"",
                                        item);
        memcpy(e->name, item, len);
        e->name[len] = '\0';
        e->stage = DE__SHADER_STAGE_NONE;
        if (colon != NULL && !stage_from_name(colon + 1, &e->stage))
            return de_asset_import_fail(ctx, DE_ERR_INVALID_ARGUMENT,
                                        "`entry_points`: unknown stage \"%s\" in \"%s\" (vertex, "
                                        "pixel, compute, mesh or amplification)",
                                        colon + 1, item);
        for (u32 k = 0; k < i; ++k)
            if (strcmp(out[k].name, e->name) == 0)
                return de_asset_import_fail(ctx, DE_ERR_INVALID_ARGUMENT,
                                            "`entry_points` lists \"%s\" twice", e->name);
    }
    *count = n;
    return DE_OK;
}

/* Splits one permutation "A=1;B" into defines (pointers into `buf`). */
static de_result split_defines(de_asset_import_ctx *ctx, const char *perm, char *buf, size_t cap,
                               const char **defs, u32 *count) {
    *count = 0;
    size_t n = strlen(perm);
    if (n >= cap)
        return de_asset_import_fail(ctx, DE_ERR_INVALID_ARGUMENT,
                                    "`permutations`: \"%.40s...\" is too long", perm);
    memcpy(buf, perm, n + 1u);
    char *at = buf;
    while (*at != '\0') {
        char *end = strchr(at, ';');
        if (end != NULL)
            *end = '\0';
        /* Trim spaces. */
        while (*at == ' ')
            ++at;
        size_t len = strlen(at);
        while (len > 0 && at[len - 1u] == ' ')
            at[--len] = '\0';
        if (len != 0) {
            const char *eq = strchr(at, '=');
            size_t name_len = eq != NULL ? (size_t)(eq - at) : len;
            if (!is_ident(at, name_len))
                return de_asset_import_fail(ctx, DE_ERR_INVALID_ARGUMENT,
                                            "`permutations`: \"%s\" in \"%s\" is not NAME or "
                                            "NAME=VALUE",
                                            at, perm);
            if (strncmp(at, "DE_SHADER_TARGET_", 17) == 0)
                return de_asset_import_fail(ctx, DE_ERR_INVALID_ARGUMENT,
                                            "`permutations`: \"%s\" is set by the importer", at);
            if (*count == MAX_DEFINES)
                return de_asset_import_fail(ctx, DE_ERR_INVALID_ARGUMENT,
                                            "`permutations`: more than %u defines in \"%s\"",
                                            MAX_DEFINES, perm);
            defs[(*count)++] = at;
        }
        if (end == NULL)
            break;
        at = end + 1;
    }
    return DE_OK;
}

static const de__shader_target k_target_of[DE__SLANG_TARGET_COUNT_] = {
    DE__SHADER_TARGET_SPIRV, DE__SHADER_TARGET_DXIL, DE__SHADER_TARGET_METALLIB,
    DE__SHADER_TARGET_NONE};

/* The output targets, as a bit set of de__slang_target. */
static de_result parse_targets(de_asset_import_ctx *ctx, u32 *mask) {
    const char *const *items = NULL;
    u32 n = de_asset_import_setting_string_array(ctx, "targets", &items);
    *mask = 0;
    if (!de_asset_import_setting_has(ctx, "targets")) {
        switch (de_asset_import_platform(ctx)) {
        case DE_COOK_PLATFORM_WINDOWS:
            *mask = (1u << DE__SLANG_DXIL) | (1u << DE__SLANG_SPIRV);
            break;
        case DE_COOK_PLATFORM_MACOS:
            *mask = (1u << DE__SLANG_METALLIB) | (1u << DE__SLANG_SPIRV);
            break;
        default:
            *mask = 1u << DE__SLANG_SPIRV;
            break;
        }
        return DE_OK;
    }
    for (u32 i = 0; i < n; ++i) {
        if (strcmp(items[i], "spirv") == 0)
            *mask |= 1u << DE__SLANG_SPIRV;
        else if (strcmp(items[i], "dxil") == 0)
            *mask |= 1u << DE__SLANG_DXIL;
        else if (strcmp(items[i], "metallib") == 0)
            *mask |= 1u << DE__SLANG_METALLIB;
        else
            return de_asset_import_fail(ctx, DE_ERR_INVALID_ARGUMENT,
                                        "`targets`: unknown target \"%s\" (spirv, dxil, metallib)",
                                        items[i]);
    }
    if (*mask == 0)
        return de_asset_import_fail(ctx, DE_ERR_INVALID_ARGUMENT, "`targets` is empty");
    return DE_OK;
}

/* ---- Imports: Slang's paths to de_asset_import_read_file ------------------------------------- */

typedef struct read_ctx {
    de_asset_import_ctx *ctx;
    char source_dir[DE_FS_PATH_MAX]; /* the source's directory below the asset root ("" = root) */
} read_ctx;

/* Splits a '/' path into normalised segments ("." dropped, ".." pops). False when it is absolute,
 * has a drive or scheme, or climbs above its start. */
static bool split_path(const char *path, char *buf, size_t cap, const char **segs, u32 *count,
                       u32 max) {
    *count = 0;
    size_t n = strlen(path);
    if (n == 0 || n >= cap || path[0] == '/' || strchr(path, ':') != NULL ||
        strchr(path, '\\') != NULL)
        return false;
    memcpy(buf, path, n + 1u);
    char *at = buf;
    for (;;) {
        char *end = strchr(at, '/');
        if (end != NULL)
            *end = '\0';
        if (strcmp(at, "..") == 0) {
            if (*count == 0)
                return false;
            --*count;
        } else if (at[0] != '\0' && strcmp(at, ".") != 0) {
            if (*count == max)
                return false;
            segs[(*count)++] = at;
        }
        if (end == NULL)
            break;
        at = end + 1;
    }
    return true;
}

#define MAX_SEGS 64u

/* `path` (relative to the asset root) as a path relative to the source's directory. */
static bool relative_to_source(const read_ctx *rc, const char *path, char *out, size_t cap) {
    char pbuf[DE_FS_PATH_MAX], dbuf[DE_FS_PATH_MAX];
    const char *ps[MAX_SEGS], *ds[MAX_SEGS];
    u32 pn = 0, dn = 0;
    if (!split_path(path, pbuf, sizeof pbuf, ps, &pn, MAX_SEGS) || pn == 0)
        return false;
    if (rc->source_dir[0] != '\0' &&
        !split_path(rc->source_dir, dbuf, sizeof dbuf, ds, &dn, MAX_SEGS))
        return false;
    u32 common = 0;
    while (common < dn && common + 1u < pn && strcmp(ps[common], ds[common]) == 0)
        ++common;
    size_t at = 0;
    out[0] = '\0';
    for (u32 i = common; i < dn; ++i) {
        int w = snprintf(out + at, cap - at, "../");
        if (w < 0 || (size_t)w >= cap - at)
            return false;
        at += (size_t)w;
    }
    for (u32 i = common; i < pn; ++i) {
        int w = snprintf(out + at, cap - at, "%s%s", ps[i], i + 1u < pn ? "/" : "");
        if (w < 0 || (size_t)w >= cap - at)
            return false;
        at += (size_t)w;
    }
    return true;
}

static de_result read_import(void *user, const char *path, const void **data, u64 *size) {
    read_ctx *rc = (read_ctx *)user;
    char rel[DE_FS_PATH_MAX];
    if (!relative_to_source(rc, path, rel, sizeof rel))
        return DE_ERR_NOT_FOUND;
    de_result r = de_asset_import_read_file(rc->ctx, rel, data, size);
    if (r == DE_ERR_INVALID_ARGUMENT)
        return DE_ERR_NOT_FOUND; /* leaves the asset root: not a file of this project */
    return r;
}

/* ---- SPIR-V scan: bindless flags and mesh limits --------------------------------------------- */

#define SPV_OP_EXECUTION_MODE      16u
#define SPV_OP_DECORATE            71u
#define SPV_DEC_BINDING            33u
#define SPV_DEC_DESCRIPTOR_SET     34u
#define SPV_MODE_OUTPUT_VERTICES   26u
#define SPV_MODE_OUTPUT_PRIMITIVES 5270u
#define SPV_MAX_DECOS              256u

static u32 rd32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void spirv_scan(const void *code, u64 size, u32 *bindless_flags, u32 *mesh_limits) {
    *bindless_flags = 0;
    *mesh_limits = 0;
    const u8 *p = (const u8 *)code;
    if (size < 20 || (size & 3u) != 0 || rd32(p) != 0x07230203u)
        return;
    u32 set_id[SPV_MAX_DECOS], bind_id[SPV_MAX_DECOS], bind_val[SPV_MAX_DECOS];
    u32 set_n = 0, bind_n = 0, verts = 0, prims = 0;
    u64 at = 20;
    while (at + 4u <= size) {
        u32 w0 = rd32(p + at);
        u32 words = w0 >> 16, op = w0 & 0xffffu;
        if (words == 0 || at + (u64)words * 4u > size)
            break;
        const u8 *ins = p + at;
        if (op == SPV_OP_DECORATE && words >= 4) {
            u32 id = rd32(ins + 4), deco = rd32(ins + 8), val = rd32(ins + 12);
            if (deco == SPV_DEC_DESCRIPTOR_SET && val == 0 && set_n < SPV_MAX_DECOS)
                set_id[set_n++] = id;
            else if (deco == SPV_DEC_BINDING && bind_n < SPV_MAX_DECOS) {
                bind_id[bind_n] = id;
                bind_val[bind_n++] = val;
            }
        } else if (op == SPV_OP_EXECUTION_MODE && words >= 4) {
            u32 mode = rd32(ins + 8), val = rd32(ins + 12);
            if (mode == SPV_MODE_OUTPUT_VERTICES)
                verts = val;
            else if (mode == SPV_MODE_OUTPUT_PRIMITIVES)
                prims = val;
        }
        at += (u64)words * 4u;
    }
    /* Set 0 bindings 0-3 are the bindless tables (shaders/common/bindless.slang); Slang drops
     * the ones an entry point does not use. */
    static const u32 k_flag_of_binding[4] = {DE__SHADER_USES_TEXTURES,
                                             DE__SHADER_USES_STORAGE_TEXTURES,
                                             DE__SHADER_USES_SAMPLERS, DE__SHADER_USES_BUFFERS};
    for (u32 i = 0; i < bind_n; ++i) {
        if (bind_val[i] >= 4u)
            continue;
        for (u32 k = 0; k < set_n; ++k)
            if (set_id[k] == bind_id[i])
                *bindless_flags |= k_flag_of_binding[bind_val[i]];
    }
    if (verts > 0xffffu)
        verts = 0xffffu;
    if (prims > 0xffffu)
        prims = 0xffffu;
    *mesh_limits = verts | (prims << 16);
}

/* ---- The import ------------------------------------------------------------------------------ */

/* One compile: a target and a permutation, all entry points. */
typedef struct compile_out {
    de__slang_target target;
    u32 permutation;
    bool spirv; /* the result is the permutation's SPIR-V compile (import_state.spirv) */
    de__slang_compile_result r;
} compile_out;

typedef struct import_state {
    de_asset_import_ctx *ctx;
    const char *path;
    const char *source;
    u64 source_size;
    read_ctx rc;
    entry_spec entries[MAX_ENTRIES];
    u32 entry_count;
    const char *const *perms;
    u32 perm_count;
    compile_out *outs; /* (target, permutation) compiles, in output order */
    u32 out_count, out_cap;
    compile_out *spirv; /* per permutation: the SPIR-V compile used for the scan */
} import_state;

static de_result compile_one(import_state *st, de__slang_target target, u32 perm,
                             de__slang_compile_result *r) {
    char buf[1024];
    const char *defs[MAX_DEFINES];
    u32 def_count = 0;
    DE_TRY(split_defines(st->ctx, st->perms[perm], buf, sizeof buf, defs, &def_count));
    de__slang_entry ents[MAX_ENTRIES];
    for (u32 i = 0; i < st->entry_count; ++i) {
        ents[i].name = st->entries[i].name;
        ents[i].stage = st->entries[i].stage;
    }
    de__slang_compile_desc d;
    memset(&d, 0, sizeof d);
    d.target = target;
    d.path = st->path;
    d.source = st->source;
    d.source_size = st->source_size;
    d.defines = defs;
    d.define_count = def_count;
    d.modules = de__shader_common_modules;
    d.module_count = de__shader_common_module_count;
    d.preload_count = 1; /* bindless: the Metal heap takes buffer index 0 in every program */
    d.read = read_import;
    d.read_user = &st->rc;
    d.entries = st->entry_count != 0 ? ents : NULL;
    d.entry_count = st->entry_count;
    d.allocator = de_asset_import_allocator(st->ctx);
    de_result rc = de__slang_compile(&d, r);
    if (rc != DE_OK)
        return de_asset_import_fail(st->ctx, rc, "%s (%s%s%s): %s", de__slang_target_name(target),
                                    st->perms[perm][0] != '\0' ? "permutation \"" : "no defines",
                                    st->perms[perm], st->perms[perm][0] != '\0' ? "\"" : "",
                                    r->diagnostics);
    if (r->diagnostics[0] != '\0')
        DE_WARN(shader_log(), "shader \"%s\" (%s): %s", st->path, de__slang_target_name(target),
                r->diagnostics);
    /* The first compile names the entry points when the settings do not. */
    if (st->entry_count == 0) {
        if (r->entry_count > MAX_ENTRIES)
            return de_asset_import_fail(st->ctx, DE_ERR_INVALID_ARGUMENT,
                                        "%u entry points; at most %u", r->entry_count, MAX_ENTRIES);
        for (u32 i = 0; i < r->entry_count; ++i) {
            snprintf(st->entries[i].name, ENTRY_NAME_MAX, "%s", r->entry_names[i]);
            st->entries[i].stage = r->entries[i].stage;
        }
        st->entry_count = r->entry_count;
    }
    if (r->entry_count != st->entry_count)
        return de_asset_import_fail(st->ctx, DE_ERR_INTERNAL, "entry point count changed");
    for (u32 i = 0; i < r->entry_count; ++i) {
        if (r->entries[i].push_constant_bytes > DE__SHADER_MAX_PUSH_CONSTANT_BYTES)
            return de_asset_import_fail(
                st->ctx, DE_ERR_INVALID_ARGUMENT,
                "entry point \"%s\" uses %u bytes of push constants on %s; the limit is %u "
                "(SPEC §12.1): move the rest into a buffer",
                st->entries[i].name, r->entries[i].push_constant_bytes,
                de__slang_target_name(target), DE__SHADER_MAX_PUSH_CONSTANT_BYTES);
    }
    return DE_OK;
}

static de_result push_out(import_state *st, de__slang_target target, u32 perm, compile_out **out) {
    const de_allocator *a = de_asset_import_allocator(st->ctx);
    if (st->out_count == st->out_cap) {
        u32 ncap = st->out_cap != 0 ? st->out_cap * 2u : 8u;
        compile_out *n = (compile_out *)DE_REALLOC(a, st->outs, sizeof(compile_out) * st->out_cap,
                                                   sizeof(compile_out) * ncap,
                                                   DE_ALIGNOF(compile_out), DE_MEM_ASSET);
        if (n == NULL)
            return de_asset_import_fail(st->ctx, DE_ERR_OUT_OF_MEMORY, "out of memory");
        st->outs = n;
        st->out_cap = ncap;
    }
    compile_out *o = &st->outs[st->out_count++];
    memset(o, 0, sizeof *o);
    o->target = target;
    o->permutation = perm;
    *out = o;
    return DE_OK;
}

/* Little-endian writer into one buffer. */
typedef struct writer {
    u8 *p;
    size_t at, cap;
} writer;

static void w32(writer *w, u32 v) {
    u8 *d = w->p + w->at;
    d[0] = (u8)v;
    d[1] = (u8)(v >> 8);
    d[2] = (u8)(v >> 16);
    d[3] = (u8)(v >> 24);
    w->at += 4;
}

static de_result write_blob(import_state *st) {
    de_asset_import_ctx *ctx = st->ctx;
    const de_allocator *a = de_asset_import_allocator(ctx);
    u32 record_count = st->out_count * st->entry_count;

    /* Strings: entry names, then permutations. */
    size_t strings = 0;
    for (u32 i = 0; i < st->entry_count; ++i)
        strings += strlen(st->entries[i].name) + 1u;
    for (u32 i = 0; i < st->perm_count; ++i)
        strings += strlen(st->perms[i]) + 1u;
    size_t strings_offset = sizeof(de__shader_header) + sizeof(de__shader_entry) * st->entry_count +
                            sizeof(de__shader_permutation) * st->perm_count +
                            sizeof(de__shader_record) * record_count;
    size_t code_at = ALIGN_UP(strings_offset + strings, DE__SHADER_CODE_ALIGN);
    size_t total = code_at;
    for (u32 o = 0; o < st->out_count; ++o) {
        const compile_out *c = &st->outs[o];
        const de__slang_compile_result *res = c->spirv ? &st->spirv[c->permutation].r : &c->r;
        for (u32 e = 0; e < st->entry_count; ++e)
            total = ALIGN_UP(total + (size_t)res->entries[e].code_size, DE__SHADER_CODE_ALIGN);
    }
    if (total > 0xffffffffu)
        return de_asset_import_fail(ctx, DE_ERR_INVALID_ARGUMENT,
                                    "the cooked shader is larger than 4 GiB");
    writer w = {(u8 *)DE_ALLOC(a, total, 16, DE_MEM_ASSET), 0, total};
    if (w.p == NULL)
        return de_asset_import_fail(ctx, DE_ERR_OUT_OF_MEMORY, "out of memory for %zu bytes",
                                    total);
    memset(w.p, 0, total); /* padding is zero */

    w32(&w, DE__SHADER_MAGIC);
    w32(&w, DE__SHADER_VERSION);
    w32(&w, st->entry_count);
    w32(&w, st->perm_count);
    w32(&w, record_count);
    w32(&w, (u32)strings_offset);
    w32(&w, (u32)strings);
    w32(&w, 0);

    u32 str_at = 0;
    for (u32 i = 0; i < st->entry_count; ++i) {
        w32(&w, str_at);
        w32(&w, st->spirv[0].r.entries[i].stage);
        str_at += (u32)strlen(st->entries[i].name) + 1u;
    }
    for (u32 i = 0; i < st->perm_count; ++i) {
        w32(&w, str_at);
        str_at += (u32)strlen(st->perms[i]) + 1u;
    }

    size_t code = code_at;
    for (u32 o = 0; o < st->out_count; ++o) {
        const compile_out *c = &st->outs[o];
        const compile_out *sv = &st->spirv[c->permutation];
        const de__slang_compile_result *res = c->spirv ? &sv->r : &c->r;
        for (u32 e = 0; e < st->entry_count; ++e) {
            const de__slang_entry_out *eo = &res->entries[e];
            u32 flags = 0, mesh = 0;
            spirv_scan(sv->r.entries[e].code, sv->r.entries[e].code_size, &flags, &mesh);
            w32(&w, (u32)k_target_of[c->target]);
            w32(&w, c->permutation);
            w32(&w, e);
            w32(&w, sv->r.entries[e].stage);
            w32(&w, (u32)code);
            w32(&w, (u32)eo->code_size);
            w32(&w, eo->threadgroup[0]);
            w32(&w, eo->threadgroup[1]);
            w32(&w, eo->threadgroup[2]);
            w32(&w, eo->push_constant_bytes);
            w32(&w, flags);
            w32(&w, sv->r.entries[e].stage == DE__SHADER_STAGE_MESH ? mesh : 0u);
            if (eo->code_size != 0)
                memcpy(w.p + code, eo->code, (size_t)eo->code_size);
            code = ALIGN_UP(code + (size_t)eo->code_size, DE__SHADER_CODE_ALIGN);
        }
    }
    for (u32 i = 0; i < st->entry_count; ++i) {
        size_t n = strlen(st->entries[i].name) + 1u;
        memcpy(w.p + w.at, st->entries[i].name, n);
        w.at += n;
    }
    for (u32 i = 0; i < st->perm_count; ++i) {
        size_t n = strlen(st->perms[i]) + 1u;
        memcpy(w.p + w.at, st->perms[i], n);
        w.at += n;
    }
    de_result r = de_asset_import_write(ctx, w.p, total);
    DE_FREE(a, w.p, total);
    return r;
}

static void state_free(import_state *st) {
    const de_allocator *a = de_asset_import_allocator(st->ctx);
    for (u32 i = 0; i < st->out_count; ++i)
        de__slang_compile_result_free(&st->outs[i].r);
    if (st->outs != NULL)
        DE_FREE(a, st->outs, sizeof(compile_out) * st->out_cap);
    if (st->spirv != NULL) {
        for (u32 i = 0; i < st->perm_count; ++i)
            de__slang_compile_result_free(&st->spirv[i].r);
        DE_FREE(a, st->spirv, sizeof(compile_out) * st->perm_count);
    }
}

static de_result import_run(import_state *st) {
    de_asset_import_ctx *ctx = st->ctx;
    u32 targets = 0;
    DE_TRY(parse_entries(ctx, st->entries, &st->entry_count));
    DE_TRY(parse_targets(ctx, &targets));

    static const char *const k_no_defines[1] = {""};
    st->perm_count = de_asset_import_setting_string_array(ctx, "permutations", &st->perms);
    if (st->perm_count == 0) {
        if (de_asset_import_setting_has(ctx, "permutations"))
            return de_asset_import_fail(ctx, DE_ERR_INVALID_ARGUMENT, "`permutations` is empty");
        st->perms = k_no_defines;
        st->perm_count = 1;
    }
    if (st->perm_count > MAX_PERMUTATIONS)
        return de_asset_import_fail(ctx, DE_ERR_INVALID_ARGUMENT, "%u permutations; at most %u",
                                    st->perm_count, MAX_PERMUTATIONS);

    /* SPIR-V of every permutation first: it names the entry points, and the scan reads it. */
    const de_allocator *a = de_asset_import_allocator(ctx);
    st->spirv = (compile_out *)DE_ALLOC(a, sizeof(compile_out) * st->perm_count,
                                        DE_ALIGNOF(compile_out), DE_MEM_ASSET);
    if (st->spirv == NULL)
        return de_asset_import_fail(ctx, DE_ERR_OUT_OF_MEMORY, "out of memory");
    memset(st->spirv, 0, sizeof(compile_out) * st->perm_count);
    if (!de__slang_target_supported(DE__SLANG_SPIRV))
        return de_asset_import_fail(ctx, DE_ERR_UNSUPPORTED,
                                    "this host's Slang cannot compile SPIR-V");
    for (u32 p = 0; p < st->perm_count; ++p) {
        st->spirv[p].target = DE__SLANG_SPIRV;
        st->spirv[p].permutation = p;
        DE_TRY(compile_one(st, DE__SLANG_SPIRV, p, &st->spirv[p].r));
    }

    static const de__slang_target k_order[] = {DE__SLANG_SPIRV, DE__SLANG_DXIL, DE__SLANG_METALLIB};
    for (u32 t = 0; t < COUNT_OF(k_order); ++t) {
        de__slang_target target = k_order[t];
        if ((targets & (1u << target)) == 0)
            continue;
        if (!de__slang_target_supported(target)) {
            DE_WARN(shader_log(),
                    "shader \"%s\": target %s skipped: this host cannot compile it (%s)", st->path,
                    de__slang_target_name(target),
                    target == DE__SLANG_DXIL       ? "no DXC (dxcompiler) found"
                    : target == DE__SLANG_METALLIB ? "no Metal toolchain: cook on macOS"
                                                   : "not supported");
            continue;
        }
        for (u32 p = 0; p < st->perm_count; ++p) {
            compile_out *o = NULL;
            DE_TRY(push_out(st, target, p, &o));
            if (target == DE__SLANG_SPIRV) {
                o->spirv = true; /* compiled above */
                continue;
            }
            DE_TRY(compile_one(st, target, p, &o->r));
        }
    }
    if (st->out_count == 0)
        return de_asset_import_fail(ctx, DE_ERR_UNSUPPORTED,
                                    "none of the requested targets can be compiled on this host");
    return write_blob(st);
}

de_result de_shader_import(de_asset_import_ctx *ctx) {
    char why[512];
    if (!de__slang_available(why, sizeof why))
        return de_asset_import_fail(ctx, DE_ERR_UNSUPPORTED, "%s", why);
    import_state *st =
        (import_state *)DE_ALLOC(de_asset_import_allocator(ctx), sizeof(import_state),
                                 DE_ALIGNOF(import_state), DE_MEM_ASSET);
    if (st == NULL)
        return de_asset_import_fail(ctx, DE_ERR_OUT_OF_MEMORY, "out of memory");
    memset(st, 0, sizeof *st);
    st->ctx = ctx;
    st->path = de_asset_import_source_path(ctx);
    st->source = (const char *)de_asset_import_source(ctx, &st->source_size);
    st->rc.ctx = ctx;
    const char *slash = strrchr(st->path, '/');
    size_t dir = slash != NULL ? (size_t)(slash - st->path) : 0;
    if (dir >= sizeof st->rc.source_dir)
        dir = sizeof st->rc.source_dir - 1u;
    memcpy(st->rc.source_dir, st->path, dir);
    st->rc.source_dir[dir] = '\0';
    de_result r = import_run(st);
    state_free(st);
    DE_FREE(de_asset_import_allocator(ctx), st, sizeof *st);
    return r;
}
