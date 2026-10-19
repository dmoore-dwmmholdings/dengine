/*
 * manifest.c: assets.toml parser (SPEC §11.2, §17.3 locale overrides; DENG-138).
 *
 * One pass per file, files in visit order (root, then includes depth-first in list order):
 *   1. read the file through the reader callback, parse it with tomlc17 (through core's entry
 *      point de__toml_parse, never toml_parse / toml_set_option: the hooks are process-wide),
 *   2. walk the top-level table in document order: [project], [defaults.<type>], [assets],
 *      [bundles.<name>]; copy everything into the manifest arena and record issues,
 *   3. follow `[project] include`, relative to this file, while this file's tree is still alive.
 * Then a finalize step sorts every settings list by key and overlays `defaults.<type>` under each
 * asset's own keys (the asset wins).
 *
 * Errors in the input never stop the walk: each becomes an issue, and the walk goes on with the
 * next key. Only an allocation failure aborts (every function below returns false then, and the
 * whole manifest is released).
 *
 * Memory: the manifest and everything it points to come from one chunk arena (mf_arena) on the
 * parse allocator. Temporaries (hash maps, the key-path buffer, sort scratch, file bytes, tomlc17
 * trees) use the parse allocator directly and are released before de__manifest_parse returns.
 * The hash maps are only probed, never iterated, so no output order depends on them.
 */

#include "asset/manifest_internal.h"

#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "core/de_array.h"
#include "core/de_hashmap.h"
#include "core/de_sort.h"
#include "core/de_strbuf.h"
#include "core/de_toml_internal.h"

#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- Chunk arena ------------------------------------------------------------------------- */

/* Chunks come from the backing allocator, so a tracking allocator sees all manifest memory.
 * free is a no-op; realloc grows the most recent block in place when it fits. */

typedef struct mf_chunk {
    struct mf_chunk *next;
    size_t cap;  /* bytes after the header */
    size_t used; /* bytes used after the header */
    size_t alloc_size;
} mf_chunk;

enum { MF_CHUNK_HDR = 32, MF_CHUNK_DEFAULT = 16 * 1024 };
DE_STATIC_ASSERT(sizeof(mf_chunk) <= MF_CHUNK_HDR, "mf_chunk header too large");

typedef struct mf_arena {
    const de_allocator *backing;
    mf_chunk *head;
    u8 *last;         /* most recent block, for in-place realloc */
    size_t last_size; /* its size */
} mf_arena;

static u8 *mf_chunk_data(mf_chunk *c) {
    return (u8 *)c + MF_CHUNK_HDR;
}

static void *mf_arena_alloc(void *user, size_t size, size_t align, u32 tag) {
    (void)tag;
    mf_arena *a = (mf_arena *)user;
    if (align == 0)
        align = 1;
    mf_chunk *c = a->head;
    if (c != NULL) {
        uintptr_t base = (uintptr_t)mf_chunk_data(c);
        uintptr_t at = (base + c->used + (align - 1)) & ~(uintptr_t)(align - 1);
        size_t off = (size_t)(at - base);
        if (off <= c->cap && size <= c->cap - off) {
            c->used = off + size;
            a->last = (u8 *)at;
            a->last_size = size;
            return (void *)at;
        }
    }
    if (size > SIZE_MAX / 2 - align - MF_CHUNK_HDR) {
        DE_FAIL(DE_ERR_OUT_OF_MEMORY, "manifest arena: %zu bytes is too large", size);
        return NULL;
    }
    size_t cap = size + align > MF_CHUNK_DEFAULT ? size + align : MF_CHUNK_DEFAULT;
    size_t total = cap + MF_CHUNK_HDR;
    mf_chunk *n = (mf_chunk *)DE_ALLOC(a->backing, total, 16, DE_MEM_ASSET);
    if (n == NULL)
        return NULL;
    n->next = a->head;
    n->cap = cap;
    n->used = 0;
    n->alloc_size = total;
    a->head = n;
    return mf_arena_alloc(user, size, align, tag);
}

static void *mf_arena_realloc(void *user, void *ptr, size_t old_size, size_t new_size, size_t align,
                              u32 tag) {
    mf_arena *a = (mf_arena *)user;
    if (ptr == NULL)
        return mf_arena_alloc(user, new_size, align, tag);
    if ((u8 *)ptr == a->last && a->head != NULL) {
        mf_chunk *c = a->head;
        size_t off = (size_t)((u8 *)ptr - mf_chunk_data(c));
        if (new_size <= c->cap - off) {
            c->used = off + new_size;
            a->last_size = new_size;
            return ptr;
        }
    }
    void *n = mf_arena_alloc(user, new_size, align, tag);
    if (n == NULL)
        return NULL;
    memcpy(n, ptr, old_size < new_size ? old_size : new_size);
    return n;
}

static void mf_arena_free(void *user, void *ptr, size_t size) {
    (void)user;
    (void)ptr;
    (void)size;
}

static void mf_arena_release(mf_arena *a) {
    if (a == NULL)
        return;
    const de_allocator *backing = a->backing;
    mf_chunk *c = a->head;
    while (c != NULL) {
        mf_chunk *next = c->next;
        DE_FREE(backing, c, c->alloc_size);
        c = next;
    }
    DE_FREE(backing, a, sizeof *a);
}

/* ---- Parser state ------------------------------------------------------------------------ */

/* Build-time defaults of one type: settings in file order, and the file each key came from (for
 * the duplicate-key error across files). Both arrays live in the arena. */
typedef struct mf_defaults_b {
    const char *type;
    de_array kvs;   /* de__manifest_kv */
    de_array files; /* const char * */
} mf_defaults_b;

enum { MF_KEYPATH_DEPTH = 8 };

typedef struct mf_parser {
    const de_allocator *tmp; /* parse allocator: temporaries */
    mf_arena *arena;
    de_allocator aa; /* allocator on `arena` */
    de__manifest *m;
    de__manifest_read_fn read;
    void *read_user;

    de_array files;    /* const char *, arena */
    de_array defaults; /* mf_defaults_b, arena */
    de_array assets;   /* de__manifest_asset, arena; settings = own keys until finalize */
    de_array bundles;  /* de__manifest_bundle, arena */
    de_array issues;   /* de__manifest_issue, arena */

    de_hashmap asset_ids;    /* de_hash64_str(id) -> u32 index into assets, tmp */
    de_hashmap bundle_names; /* de_hash64_str(name) -> u32 index into bundles, tmp */

    u32 stack[DE__MANIFEST_MAX_INCLUDE_DEPTH + 1]; /* file indices of the include chain */
    u32 depth;                                     /* entries in stack */

    const char *cur_file; /* file being walked (arena string) */
    de_strbuf keypath;    /* key path being walked, tmp */
    size_t kp_len[MF_KEYPATH_DEPTH];
    u32 kp_depth;
} mf_parser;

/* ---- Strings and key paths --------------------------------------------------------------- */

static const char *mf_strndup(mf_parser *p, const char *s, size_t len) {
    char *d = (char *)DE_ALLOC(&p->aa, len + 1, 1, DE_MEM_ASSET);
    if (d == NULL)
        return NULL;
    if (len != 0)
        memcpy(d, s, len);
    d[len] = '\0';
    return d;
}

static const char *mf_strdup(mf_parser *p, const char *s) {
    return mf_strndup(p, s, strlen(s));
}

static bool mf_is_bare_key(const char *k, size_t len) {
    if (len == 0)
        return false;
    for (size_t i = 0; i < len; ++i) {
        char c = k[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '-';
        if (!ok)
            return false;
    }
    return true;
}

/* Appends one key segment, quoted as TOML would need it (`assets."ui/logo".type`). */
static bool mf_kp_push(mf_parser *p, const char *key, size_t len) {
    DE_ASSERT(p->kp_depth < MF_KEYPATH_DEPTH);
    p->kp_len[p->kp_depth++] = p->keypath.len;
    de_strbuf *b = &p->keypath;
    if (b->len != 0 && de_strbuf_append_char(b, '.') != DE_OK)
        return false;
    if (mf_is_bare_key(key, len))
        return de_strbuf_append(b, (de_str){key, len}) == DE_OK;
    if (de_strbuf_append_char(b, '"') != DE_OK)
        return false;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)key[i];
        de_result r;
        if (c == '"' || c == '\\')
            r = de_strbuf_appendf(b, "\\%c", (char)c);
        else if (c < 0x20 || c == 0x7f)
            r = de_strbuf_appendf(b, "\\u%04X", (unsigned)c);
        else
            r = de_strbuf_append_char(b, (char)c);
        if (r != DE_OK)
            return false;
    }
    return de_strbuf_append_char(b, '"') == DE_OK;
}

static void mf_kp_pop(mf_parser *p) {
    DE_ASSERT(p->kp_depth > 0);
    size_t len = p->kp_len[--p->kp_depth];
    p->keypath.len = len;
    if (p->keypath.ptr != NULL)
        p->keypath.ptr[len] = '\0';
}

static const char *mf_kp_cstr(const mf_parser *p) {
    return p->keypath.ptr != NULL ? p->keypath.ptr : "";
}

/* ---- Issues ------------------------------------------------------------------------------ */

static bool mf_issue_at(mf_parser *p, const char *file, const char *key_path,
                        de_asset_issue_severity sev, de_result code, int line, const char *fmt,
                        va_list args) DE_PRINTF_FMT(7, 0);

static bool mf_issue_at(mf_parser *p, const char *file, const char *key_path,
                        de_asset_issue_severity sev, de_result code, int line, const char *fmt,
                        va_list args) {
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (n < 0)
        n = 0;
    char *msg = (char *)DE_ALLOC(&p->aa, (size_t)n + 1, 1, DE_MEM_ASSET);
    if (msg == NULL)
        return false;
    vsnprintf(msg, (size_t)n + 1, fmt, args);
    de__manifest_issue is = {
        .severity = sev,
        .code = code,
        .file = file,
        .key_path = mf_strdup(p, key_path),
        .line = line > 0 ? (u32)line : 0u,
        .message = msg,
    };
    if (is.key_path == NULL || de_array_push(&p->issues, &is) != DE_OK)
        return false;
    if (sev == DE_ASSET_ISSUE_ERROR)
        p->m->error_count++;
    else if (sev == DE_ASSET_ISSUE_WARNING)
        p->m->warning_count++;
    else
        p->m->info_count++;
    return true;
}

/* An issue in the current file at the current key path. */
static bool mf_issue(mf_parser *p, de_asset_issue_severity sev, int line, const char *fmt, ...)
    DE_PRINTF_FMT(4, 5);
static bool mf_issue(mf_parser *p, de_asset_issue_severity sev, int line, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    bool ok = mf_issue_at(p, p->cur_file, mf_kp_cstr(p), sev, DE_ERR_PARSE, line, fmt, args);
    va_end(args);
    return ok;
}

static bool mf_issue_code(mf_parser *p, const char *file, const char *key_path, de_result code,
                          int line, const char *fmt, ...) DE_PRINTF_FMT(6, 7);
static bool mf_issue_code(mf_parser *p, const char *file, const char *key_path, de_result code,
                          int line, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    bool ok = mf_issue_at(p, file, key_path, DE_ASSET_ISSUE_ERROR, code, line, fmt, args);
    va_end(args);
    return ok;
}

#define MF_ERROR(p, line, ...)   mf_issue((p), DE_ASSET_ISSUE_ERROR, (line), __VA_ARGS__)
#define MF_WARNING(p, line, ...) mf_issue((p), DE_ASSET_ISSUE_WARNING, (line), __VA_ARGS__)
#define MF_INFO(p, line, ...)    mf_issue((p), DE_ASSET_ISSUE_INFO, (line), __VA_ARGS__)

/* ---- TOML helpers ------------------------------------------------------------------------ */

static const char *mf_type_name(toml_type_t t) {
    switch (t) {
    case TOML_STRING:
        return "a string";
    case TOML_INT64:
        return "an integer";
    case TOML_FP64:
        return "a float";
    case TOML_BOOLEAN:
        return "a boolean";
    case TOML_DATE:
    case TOML_TIME:
    case TOML_DATETIME:
    case TOML_DATETIMETZ:
        return "a date or time";
    case TOML_ARRAY:
        return "an array";
    case TOML_TABLE:
        return "a table";
    default:
        return "an unknown value";
    }
}

/* A line for a datum: its own, else (for a table without one) its first child's. */
static int mf_line(const toml_datum_t *d) {
    if (d->lineno > 0)
        return d->lineno;
    if (d->type == TOML_TABLE && d->u.tab.size > 0)
        return mf_line(&d->u.tab.value[0]);
    if (d->type == TOML_ARRAY && d->u.arr.size > 0)
        return mf_line(&d->u.arr.elem[0]);
    return 0;
}

static bool mf_key_is(const toml_datum_t *tab, int32_t i, const char *name) {
    size_t n = strlen(name);
    return (size_t)tab->u.tab.len[i] == n && memcmp(tab->u.tab.key[i], name, n) == 0;
}

/* Copies an array of strings. `*ok_out` false (and an error issue) if an element is not a
 * string. Returns false only on OOM. */
static bool mf_string_array(mf_parser *p, const toml_datum_t *d, const char *const **items,
                            u32 *count, bool *ok_out) {
    *items = NULL;
    *count = 0;
    *ok_out = false;
    if (d->type != TOML_ARRAY)
        return MF_ERROR(p, mf_line(d), "expected an array of strings, found %s",
                        mf_type_name(d->type));
    u32 n = (u32)d->u.arr.size;
    const char **out = NULL;
    if (n != 0) {
        out = (const char **)DE_ALLOC(&p->aa, sizeof(char *) * n, DE_ALIGNOF(const char *),
                                      DE_MEM_ASSET);
        if (out == NULL)
            return false;
    }
    for (u32 i = 0; i < n; ++i) {
        const toml_datum_t *e = &d->u.arr.elem[i];
        if (e->type != TOML_STRING)
            return MF_ERROR(p, mf_line(e), "element %u: expected a string, found %s", i,
                            mf_type_name(e->type));
        out[i] = mf_strndup(p, e->u.str.ptr, (size_t)e->u.str.len);
        if (out[i] == NULL)
            return false;
    }
    *items = out;
    *count = n;
    *ok_out = true;
    return true;
}

/* Converts a settings value. `*ok_out` false (and an error issue) for a type settings cannot
 * hold. Returns false only on OOM. */
static bool mf_value_at(mf_parser *p, const toml_datum_t *d, de__manifest_value *out, bool *ok_out,
                        u32 depth) {
    *ok_out = true;
    memset(out, 0, sizeof *out);
    switch (d->type) {
    case TOML_BOOLEAN:
        out->type = DE__MANIFEST_BOOL;
        out->u.b = d->u.boolean;
        return true;
    case TOML_INT64:
        out->type = DE__MANIFEST_INT;
        out->u.i = (i64)d->u.int64;
        return true;
    case TOML_FP64:
        out->type = DE__MANIFEST_FLOAT;
        out->u.f = d->u.fp64;
        return true;
    case TOML_STRING:
        out->type = DE__MANIFEST_STRING;
        out->u.s = mf_strndup(p, d->u.str.ptr, (size_t)d->u.str.len);
        return out->u.s != NULL;
    case TOML_ARRAY: {
        if (depth >= 16) {
            *ok_out = false;
            return MF_ERROR(p, mf_line(d), "settings array nesting exceeds 16 levels");
        }
        bool strings = true;
        u32 count = (u32)d->u.arr.size;
        for (u32 i = 0; i < count; ++i)
            strings &= d->u.arr.elem[i].type == TOML_STRING;
        if (strings) {
            out->type = DE__MANIFEST_STRING_ARRAY;
            return mf_string_array(p, d, &out->u.arr.items, &out->u.arr.count, ok_out);
        }
        out->type = DE__MANIFEST_ARRAY;
        de__manifest_value *values =
            DE_ALLOC(&p->aa, sizeof *values * count, DE_ALIGNOF(de__manifest_value), DE_MEM_ASSET);
        if (!values)
            return false;
        out->u.values.items = values;
        out->u.values.count = count;
        for (u32 i = 0; i < count; ++i) {
            bool valid;
            if (!mf_value_at(p, &d->u.arr.elem[i], &values[i], &valid, depth + 1))
                return false;
            *ok_out &= valid;
        }
        return true;
    }
    default:
        *ok_out = false;
        return MF_ERROR(p, mf_line(d),
                        "settings hold booleans, integers, floats, strings and arrays; found %s",
                        mf_type_name(d->type));
    }
}

static bool mf_value(mf_parser *p, const toml_datum_t *d, de__manifest_value *out, bool *ok_out) {
    return mf_value_at(p, d, out, ok_out, 0);
}

static bool mf_is_variants(const toml_datum_t *tab, int32_t i) {
    return mf_key_is(tab, i, "variants");
}

static bool mf_note_variants(mf_parser *p, const toml_datum_t *v) {
    return MF_INFO(p, mf_line(v),
                   "`variants` is reserved for platform/quality variants and ignored in v1 "
                   "(DENG-2 Q19)");
}

/* ---- [project] --------------------------------------------------------------------------- */

static bool mf_project(mf_parser *p, const toml_datum_t *t, const toml_datum_t **include_out) {
    if (t->type != TOML_TABLE)
        return MF_ERROR(p, mf_line(t), "expected a table, found %s", mf_type_name(t->type));
    bool is_root = p->depth == 1;
    for (int32_t i = 0; i < t->u.tab.size; ++i) {
        const toml_datum_t *v = &t->u.tab.value[i];
        if (!mf_kp_push(p, t->u.tab.key[i], (size_t)t->u.tab.len[i]))
            return false;
        bool ok = true;
        if (mf_key_is(t, i, "asset_root") || mf_key_is(t, i, "version")) {
            bool root_key = mf_key_is(t, i, "asset_root");
            if (!is_root) {
                ok = MF_WARNING(p, mf_line(v),
                                "only the root manifest sets project.%s; ignored in an included "
                                "file",
                                root_key ? "asset_root" : "version");
            } else if (root_key) {
                if (v->type != TOML_STRING) {
                    ok = MF_ERROR(p, mf_line(v), "expected a string, found %s",
                                  mf_type_name(v->type));
                } else {
                    p->m->asset_root = mf_strndup(p, v->u.str.ptr, (size_t)v->u.str.len);
                    ok = p->m->asset_root != NULL;
                }
            } else {
                if (v->type != TOML_INT64) {
                    ok = MF_ERROR(p, mf_line(v), "expected an integer, found %s",
                                  mf_type_name(v->type));
                } else {
                    p->m->version = (i64)v->u.int64;
                    p->m->has_version = true;
                }
            }
        } else if (mf_key_is(t, i, "include")) {
            if (v->type != TOML_ARRAY)
                ok = MF_ERROR(p, mf_line(v), "expected an array of strings, found %s",
                              mf_type_name(v->type));
            else
                *include_out = v;
        } else if (mf_is_variants(t, i)) {
            ok = mf_note_variants(p, v);
        } else {
            ok = MF_WARNING(p, mf_line(v), "unknown key in [project]; ignored");
        }
        mf_kp_pop(p);
        if (!ok)
            return false;
    }
    return true;
}

/* ---- [defaults.<type>] ------------------------------------------------------------------- */

static mf_defaults_b *mf_defaults_find(mf_parser *p, const char *type, size_t len) {
    for (u32 i = 0; i < p->defaults.count; ++i) {
        mf_defaults_b *d = &DE_ARRAY_AT(&p->defaults, mf_defaults_b, i);
        if (strlen(d->type) == len && memcmp(d->type, type, len) == 0)
            return d;
    }
    return NULL;
}

static bool mf_defaults_type(mf_parser *p, const char *type, size_t type_len,
                             const toml_datum_t *t) {
    if (t->type != TOML_TABLE)
        return MF_ERROR(p, mf_line(t), "expected a table of settings, found %s",
                        mf_type_name(t->type));
    mf_defaults_b *d = mf_defaults_find(p, type, type_len);
    if (d == NULL) {
        mf_defaults_b nd = {0};
        nd.type = mf_strndup(p, type, type_len);
        if (nd.type == NULL)
            return false;
        DE_ARRAY_INIT_TAG(&nd.kvs, de__manifest_kv, &p->aa, DE_MEM_ASSET);
        DE_ARRAY_INIT_TAG(&nd.files, const char *, &p->aa, DE_MEM_ASSET);
        if (de_array_push(&p->defaults, &nd) != DE_OK)
            return false;
        d = &DE_ARRAY_LAST(&p->defaults, mf_defaults_b);
    }
    for (int32_t i = 0; i < t->u.tab.size; ++i) {
        const toml_datum_t *v = &t->u.tab.value[i];
        const char *key = t->u.tab.key[i];
        size_t key_len = (size_t)t->u.tab.len[i];
        if (!mf_kp_push(p, key, key_len))
            return false;
        bool ok = true;
        if (mf_key_is(t, i, "type") || mf_key_is(t, i, "source") || mf_key_is(t, i, "locale")) {
            ok = MF_WARNING(p, mf_line(v),
                            "`%s` is not a setting and cannot have a default; "
                            "ignored",
                            key);
        } else if (mf_is_variants(t, i)) {
            ok = mf_note_variants(p, v);
        } else {
            /* TOML refuses a key twice in one file, so an existing key came from another one. */
            const char *earlier = NULL;
            for (u32 k = 0; k < d->kvs.count; ++k) {
                const de__manifest_kv *kv = &DE_ARRAY_AT(&d->kvs, de__manifest_kv, k);
                if (strlen(kv->key) == key_len && memcmp(kv->key, key, key_len) == 0) {
                    earlier = DE_ARRAY_AT(&d->files, const char *, k);
                    break;
                }
            }
            if (earlier != NULL) {
                ok = MF_ERROR(p, mf_line(v), "defaults.%s.%s is already set in %s", d->type, key,
                              earlier);
            } else {
                de__manifest_kv kv = {0};
                bool valid;
                ok = mf_value(p, v, &kv.value, &valid);
                if (ok && valid) {
                    kv.key = mf_strndup(p, key, key_len);
                    ok = kv.key != NULL && de_array_push(&d->kvs, &kv) == DE_OK &&
                         de_array_push(&d->files, &p->cur_file) == DE_OK;
                }
            }
        }
        mf_kp_pop(p);
        if (!ok)
            return false;
    }
    return true;
}

static bool mf_defaults(mf_parser *p, const toml_datum_t *t) {
    if (t->type != TOML_TABLE)
        return MF_ERROR(p, mf_line(t), "expected a table of [defaults.<type>] tables, found %s",
                        mf_type_name(t->type));
    for (int32_t i = 0; i < t->u.tab.size; ++i) {
        if (!mf_kp_push(p, t->u.tab.key[i], (size_t)t->u.tab.len[i]))
            return false;
        bool ok = mf_defaults_type(p, t->u.tab.key[i], (size_t)t->u.tab.len[i], &t->u.tab.value[i]);
        mf_kp_pop(p);
        if (!ok)
            return false;
    }
    return true;
}

/* ---- [assets] ---------------------------------------------------------------------------- */

static bool mf_locales(mf_parser *p, const toml_datum_t *t, de__manifest_asset *a) {
    if (t->type != TOML_TABLE)
        return MF_ERROR(p, mf_line(t), "expected a table of locale = \"source\", found %s",
                        mf_type_name(t->type));
    u32 n = (u32)t->u.tab.size;
    if (n == 0)
        return true;
    de__manifest_locale *out = (de__manifest_locale *)DE_ALLOC(
        &p->aa, sizeof(de__manifest_locale) * n, DE_ALIGNOF(de__manifest_locale), DE_MEM_ASSET);
    if (out == NULL)
        return false;
    u32 count = 0;
    for (int32_t i = 0; i < t->u.tab.size; ++i) {
        const toml_datum_t *v = &t->u.tab.value[i];
        if (!mf_kp_push(p, t->u.tab.key[i], (size_t)t->u.tab.len[i]))
            return false;
        bool ok = true;
        if (t->u.tab.len[i] == 0) {
            ok = MF_ERROR(p, mf_line(v), "empty locale name");
        } else if (v->type != TOML_STRING) {
            ok = MF_ERROR(p, mf_line(v), "expected a source path string, found %s",
                          mf_type_name(v->type));
        } else {
            de__manifest_locale *l = &out[count];
            l->locale = mf_strndup(p, t->u.tab.key[i], (size_t)t->u.tab.len[i]);
            l->source = mf_strndup(p, v->u.str.ptr, (size_t)v->u.str.len);
            ok = l->locale != NULL && l->source != NULL;
            if (ok)
                ++count;
        }
        mf_kp_pop(p);
        if (!ok)
            return false;
    }
    a->locales = out;
    a->locale_count = count;
    return true;
}

static bool mf_asset(mf_parser *p, const char *id, size_t id_len, const toml_datum_t *t) {
    int line = mf_line(t);
    if (id_len == 0)
        return MF_ERROR(p, line, "empty asset ID");
    if (t->type != TOML_TABLE)
        return MF_ERROR(p, line, "an asset is a table with `type` and `source`; found %s",
                        mf_type_name(t->type));

    const char *id_copy = mf_strndup(p, id, id_len);
    if (id_copy == NULL)
        return false;
    u64 h = de_hash64_str(id_copy);
    const u32 *prev = DE_HASHMAP_GET(&p->asset_ids, u32, h, &h);
    if (prev != NULL) {
        const de__manifest_asset *first = &DE_ARRAY_AT(&p->assets, de__manifest_asset, *prev);
        if (strcmp(first->id, id_copy) == 0) {
            if (first->file == p->cur_file)
                return MF_ERROR(p, line, "asset \"%s\" is declared twice in %s (line %u)", id_copy,
                                first->file, first->line);
            return MF_ERROR(p, line, "duplicate asset \"%s\": declared in %s (line %u) and in %s",
                            id_copy, first->file, first->line, p->cur_file);
        }
        /* A 64-bit hash collision of two different IDs: not a duplicate. DENG-139 reports
         * colliding asset IDs; here the second one is simply not indexed. */
    }

    de__manifest_asset a = {0};
    a.id = id_copy;
    a.file = p->cur_file;
    a.line = line > 0 ? (u32)line : 0u;
    de_array own;
    DE_ARRAY_INIT_TAG(&own, de__manifest_kv, &p->aa, DE_MEM_ASSET);
    bool broken = false;
    bool has_type = false, has_source = false;

    for (int32_t i = 0; i < t->u.tab.size; ++i) {
        const toml_datum_t *v = &t->u.tab.value[i];
        const char *key = t->u.tab.key[i];
        size_t key_len = (size_t)t->u.tab.len[i];
        if (!mf_kp_push(p, key, key_len))
            return false;
        bool ok = true;
        if (mf_key_is(t, i, "type") || mf_key_is(t, i, "source")) {
            bool is_type = mf_key_is(t, i, "type");
            if (v->type != TOML_STRING || v->u.str.len == 0) {
                ok = MF_ERROR(p, mf_line(v), "`%s` must be a non-empty string; found %s",
                              is_type ? "type" : "source",
                              v->type == TOML_STRING ? "an empty string" : mf_type_name(v->type));
                broken = true;
            } else {
                const char *s = mf_strndup(p, v->u.str.ptr, (size_t)v->u.str.len);
                ok = s != NULL;
                if (is_type) {
                    a.type = s;
                    has_type = true;
                } else {
                    a.source = s;
                    has_source = true;
                }
            }
        } else if (mf_key_is(t, i, "locale")) {
            u32 before = p->m->error_count;
            ok = mf_locales(p, v, &a);
            broken |= p->m->error_count != before;
        } else if (mf_is_variants(t, i)) {
            ok = mf_note_variants(p, v);
        } else {
            de__manifest_kv kv = {0};
            bool valid;
            ok = mf_value(p, v, &kv.value, &valid);
            if (ok && valid) {
                kv.key = mf_strndup(p, key, key_len);
                ok = kv.key != NULL && de_array_push(&own, &kv) == DE_OK;
            }
            broken |= !valid;
        }
        mf_kp_pop(p);
        if (!ok)
            return false;
    }

    if (!has_type && !broken && !MF_ERROR(p, line, "asset \"%s\" has no `type`", id_copy))
        return false;
    if (!has_source && !broken && !MF_ERROR(p, line, "asset \"%s\" has no `source`", id_copy))
        return false;
    if (!has_type || !has_source || broken)
        return true; /* reported; not added */

    a.settings = (const de__manifest_kv *)own.data;
    a.setting_count = own.count;
    u32 index = p->assets.count;
    if (de_array_push(&p->assets, &a) != DE_OK)
        return false;
    if (prev == NULL && de_hashmap_put(&p->asset_ids, h, &h, &index) != DE_OK)
        return false;
    return true;
}

static bool mf_assets(mf_parser *p, const toml_datum_t *t) {
    if (t->type != TOML_TABLE)
        return MF_ERROR(p, mf_line(t), "expected a table of assets, found %s",
                        mf_type_name(t->type));
    for (int32_t i = 0; i < t->u.tab.size; ++i) {
        if (!mf_kp_push(p, t->u.tab.key[i], (size_t)t->u.tab.len[i]))
            return false;
        bool ok = mf_asset(p, t->u.tab.key[i], (size_t)t->u.tab.len[i], &t->u.tab.value[i]);
        mf_kp_pop(p);
        if (!ok)
            return false;
    }
    return true;
}

/* ---- [bundles.<name>] -------------------------------------------------------------------- */

static const char *const k_priority_names[] = {"critical", "high", "normal", "low", "background"};
#define MF_PRIORITY_COUNT ((u32)(sizeof k_priority_names / sizeof k_priority_names[0]))

const char *de__manifest_priority_name(de__manifest_priority pr) {
    u32 i = (u32)pr;
    return i < MF_PRIORITY_COUNT ? k_priority_names[i] : "normal";
}

static bool mf_bundle(mf_parser *p, const char *name, size_t name_len, const toml_datum_t *t) {
    int line = mf_line(t);
    if (name_len == 0)
        return MF_ERROR(p, line, "empty bundle name");
    if (t->type != TOML_TABLE)
        return MF_ERROR(p, line, "a bundle is a table; found %s", mf_type_name(t->type));
    const char *name_copy = mf_strndup(p, name, name_len);
    if (name_copy == NULL)
        return false;
    u64 h = de_hash64_str(name_copy);
    const u32 *prev = DE_HASHMAP_GET(&p->bundle_names, u32, h, &h);
    if (prev != NULL) {
        const de__manifest_bundle *first = &DE_ARRAY_AT(&p->bundles, de__manifest_bundle, *prev);
        if (strcmp(first->name, name_copy) == 0)
            return MF_ERROR(p, line, "duplicate bundle \"%s\": declared in %s (line %u) and in %s",
                            name_copy, first->file, first->line, p->cur_file);
    }

    de__manifest_bundle b = {0};
    b.name = name_copy;
    b.file = p->cur_file;
    b.line = line > 0 ? (u32)line : 0u;
    b.priority = DE__MANIFEST_PRIORITY_NORMAL;
    bool broken = false;
    for (int32_t i = 0; i < t->u.tab.size; ++i) {
        const toml_datum_t *v = &t->u.tab.value[i];
        if (!mf_kp_push(p, t->u.tab.key[i], (size_t)t->u.tab.len[i]))
            return false;
        bool ok = true, valid = true;
        if (mf_key_is(t, i, "assets")) {
            ok = mf_string_array(p, v, &b.assets, &b.asset_count, &valid);
        } else if (mf_key_is(t, i, "depends")) {
            ok = mf_string_array(p, v, &b.depends, &b.depends_count, &valid);
        } else if (mf_key_is(t, i, "resident")) {
            if (v->type != TOML_BOOLEAN) {
                valid = false;
                ok = MF_ERROR(p, mf_line(v), "expected a boolean, found %s", mf_type_name(v->type));
            } else {
                b.resident = v->u.boolean;
            }
        } else if (mf_key_is(t, i, "priority")) {
            valid = false;
            if (v->type == TOML_STRING) {
                for (u32 k = 0; k < MF_PRIORITY_COUNT; ++k) {
                    if (strcmp(v->u.str.ptr, k_priority_names[k]) == 0) {
                        b.priority = (de__manifest_priority)k;
                        valid = true;
                    }
                }
            }
            if (!valid)
                ok = MF_ERROR(p, mf_line(v),
                              "priority is one of \"critical\", \"high\", \"normal\", \"low\", "
                              "\"background\"; found %s%s%s",
                              v->type == TOML_STRING ? "\"" : "",
                              v->type == TOML_STRING ? v->u.str.ptr : mf_type_name(v->type),
                              v->type == TOML_STRING ? "\"" : "");
        } else if (mf_is_variants(t, i)) {
            ok = mf_note_variants(p, v);
        } else {
            ok = MF_WARNING(p, mf_line(v), "unknown key in bundle \"%s\"; ignored", name_copy);
        }
        broken |= !valid;
        mf_kp_pop(p);
        if (!ok)
            return false;
    }
    if (broken)
        return true; /* reported; not added */
    u32 index = p->bundles.count;
    if (de_array_push(&p->bundles, &b) != DE_OK)
        return false;
    if (prev == NULL && de_hashmap_put(&p->bundle_names, h, &h, &index) != DE_OK)
        return false;
    return true;
}

static bool mf_bundles(mf_parser *p, const toml_datum_t *t) {
    if (t->type != TOML_TABLE)
        return MF_ERROR(p, mf_line(t), "expected a table of [bundles.<name>] tables, found %s",
                        mf_type_name(t->type));
    for (int32_t i = 0; i < t->u.tab.size; ++i) {
        if (!mf_kp_push(p, t->u.tab.key[i], (size_t)t->u.tab.len[i]))
            return false;
        bool ok = mf_bundle(p, t->u.tab.key[i], (size_t)t->u.tab.len[i], &t->u.tab.value[i]);
        mf_kp_pop(p);
        if (!ok)
            return false;
    }
    return true;
}

/* ---- Files and includes ------------------------------------------------------------------ */

/* The line in a tomlc17 error message ("(line 12) ..."), or 0. */
static int mf_errmsg_line(const char *msg) {
    const char *at = strstr(msg, "line ");
    if (at == NULL)
        return 0;
    at += 5;
    int line = 0;
    while (*at >= '0' && *at <= '9' && line < 100000000)
        line = line * 10 + (*at++ - '0');
    return line;
}

/* The tomlc17 message without its "(line N) " prefix (the line is reported separately). */
static const char *mf_errmsg_text(const char *msg) {
    if (msg[0] == '(') {
        const char *close = strchr(msg, ')');
        if (close != NULL && strncmp(msg, "(line ", 6) == 0) {
            close++;
            while (*close == ' ')
                close++;
            return close;
        }
    }
    return msg;
}

static bool mf_load_file(mf_parser *p, const char *vpath, const char *from_file,
                         const char *from_key, int from_line);

static bool mf_includes(mf_parser *p, const toml_datum_t *arr) {
    const char *file = p->cur_file;
    for (int32_t i = 0; i < arr->u.arr.size; ++i) {
        const toml_datum_t *e = &arr->u.arr.elem[i];
        int line = mf_line(e);
        /* key path of the element, for messages: project.include[i] */
        char key[64];
        snprintf(key, sizeof key, "project.include[%d]", (int)i);
        if (e->type != TOML_STRING) {
            if (!mf_issue_code(p, file, key, DE_ERR_PARSE, line,
                               "expected a relative path string, found %s", mf_type_name(e->type)))
                return false;
            continue;
        }
        /* Join with the directory of the including file, then normalise; the VFS rules refuse
         * a `..` that leaves the root, a `:` (so no other root), backslashes and the like. */
        const char *slash = strrchr(file, '/');
        size_t dir_len = slash != NULL ? (size_t)(slash - file) + 1 : 0;
        size_t inc_len = (size_t)e->u.str.len;
        char joined[DE_FS_PATH_MAX];
        char norm[DE_FS_PATH_MAX];
        bool fits = dir_len + inc_len < sizeof joined;
        de_result r = DE_ERR_INVALID_ARGUMENT;
        if (fits && inc_len != 0 && e->u.str.ptr[0] != '/' &&
            memchr(e->u.str.ptr, '\0', inc_len) == NULL) {
            memcpy(joined, file, dir_len);
            memcpy(joined + dir_len, e->u.str.ptr, inc_len);
            joined[dir_len + inc_len] = '\0';
            r = de_path_normalize(joined, norm, sizeof norm);
        }
        if (r != DE_OK) {
            if (!mf_issue_code(p, file, key, DE_ERR_PARSE, line,
                               "invalid include \"%s\": includes are relative paths that stay "
                               "inside the root of the including file",
                               e->u.str.ptr))
                return false;
            continue;
        }
        /* Cycle: the file is on the current include chain. Duplicate: read before elsewhere. */
        const char *cycle = NULL, *dup = NULL;
        for (u32 k = 0; k < p->depth; ++k) {
            const char *f = DE_ARRAY_AT(&p->files, const char *, p->stack[k]);
            if (strcmp(f, norm) == 0)
                cycle = f;
        }
        for (u32 k = 0; cycle == NULL && k < p->files.count; ++k) {
            const char *f = DE_ARRAY_AT(&p->files, const char *, k);
            if (strcmp(f, norm) == 0)
                dup = f;
        }
        bool ok = true;
        if (cycle != NULL) {
            de_strbuf chain;
            de_strbuf_init(&chain, p->tmp);
            for (u32 k = 0; k < p->depth && ok; ++k)
                ok = de_strbuf_appendf(&chain, "%s -> ",
                                       DE_ARRAY_AT(&p->files, const char *, p->stack[k])) == DE_OK;
            ok = ok && de_strbuf_append_cstr(&chain, norm) == DE_OK;
            ok = ok &&
                 mf_issue_code(p, file, key, DE_ERR_PARSE, line, "include cycle: %s", chain.ptr);
            de_strbuf_free(&chain);
        } else if (dup != NULL) {
            ok = mf_issue_code(p, file, key, DE_ERR_PARSE, line,
                               "%s is included more than once (each file may be included once)",
                               norm);
        } else if (p->depth > DE__MANIFEST_MAX_INCLUDE_DEPTH) {
            ok =
                mf_issue_code(p, file, key, DE_ERR_PARSE, line,
                              "includes nest deeper than %u files", DE__MANIFEST_MAX_INCLUDE_DEPTH);
        } else if (p->files.count >= DE__MANIFEST_MAX_FILES) {
            ok = mf_issue_code(p, file, key, DE_ERR_PARSE, line, "a manifest is at most %u files",
                               DE__MANIFEST_MAX_FILES);
        } else {
            ok = mf_load_file(p, norm, file, key, line);
            p->cur_file = file; /* back to the includer */
        }
        if (!ok)
            return false;
    }
    return true;
}

static bool mf_walk(mf_parser *p, const toml_datum_t *top, const toml_datum_t **include_out) {
    for (int32_t i = 0; i < top->u.tab.size; ++i) {
        const toml_datum_t *v = &top->u.tab.value[i];
        if (!mf_kp_push(p, top->u.tab.key[i], (size_t)top->u.tab.len[i]))
            return false;
        bool ok;
        if (mf_key_is(top, i, "project"))
            ok = mf_project(p, v, include_out);
        else if (mf_key_is(top, i, "defaults"))
            ok = mf_defaults(p, v);
        else if (mf_key_is(top, i, "assets"))
            ok = mf_assets(p, v);
        else if (mf_key_is(top, i, "bundles"))
            ok = mf_bundles(p, v);
        else if (mf_is_variants(top, i))
            ok = mf_note_variants(p, v);
        else
            ok = MF_WARNING(p, mf_line(v),
                            "unknown top-level key (expected project, defaults, assets, "
                            "bundles); ignored");
        mf_kp_pop(p);
        if (!ok)
            return false;
    }
    return true;
}

/* Reads, parses and walks `vpath` (normalised), then its includes. Read and parse failures are
 * issues on the including key (`from_*`), or on the file itself for the root. */
static bool mf_load_file(mf_parser *p, const char *vpath, const char *from_file,
                         const char *from_key, int from_line) {
    const char *path = mf_strdup(p, vpath);
    if (path == NULL)
        return false;
    const char *issue_file = from_file != NULL ? from_file : path;
    const char *issue_key = from_key != NULL ? from_key : "";

    void *data = NULL;
    u64 size = 0;
    de_error_clear();
    de_result r = p->read(p->read_user, path, p->tmp, &data, &size);
    if (r == DE_ERR_OUT_OF_MEMORY)
        return false;
    if (r != DE_OK) {
        const de_error *e = de_error_last();
        return mf_issue_code(p, issue_file, issue_key, r, from_line, "cannot read %s: %s%s%s", path,
                             de_result_str(r), e != NULL ? ": " : "", e != NULL ? e->message : "");
    }
    u32 index = p->files.count;
    if (de_array_push(&p->files, &path) != DE_OK) {
        DE_FREE(p->tmp, data, (size_t)size + 1);
        return false;
    }
    if (size > (u64)INT_MAX) {
        DE_FREE(p->tmp, data, (size_t)size + 1);
        return mf_issue_code(p, path, "", DE_ERR_PARSE, 0, "file is too large (%llu bytes)",
                             (unsigned long long)size);
    }
    toml_result_t res = de__toml_parse(p->tmp, (const char *)data, (int)size);
    DE_FREE(p->tmp, data, (size_t)size + 1);
    if (!res.ok) {
        bool oom = strstr(res.errmsg, "out of memory") != NULL;
        bool ok = !oom && mf_issue_code(p, path, "", DE_ERR_PARSE, mf_errmsg_line(res.errmsg),
                                        "invalid TOML: %s", mf_errmsg_text(res.errmsg));
        de__toml_free(res);
        if (oom)
            DE_FAIL(DE_ERR_OUT_OF_MEMORY, "out of memory while parsing %s", path);
        return ok;
    }

    p->stack[p->depth++] = index;
    p->cur_file = path;
    const toml_datum_t *include = NULL;
    size_t kp_save = p->kp_depth;
    bool ok = mf_walk(p, &res.toptab, &include);
    DE_ASSERT(!ok || p->kp_depth == kp_save);
    (void)kp_save;
    if (ok && include != NULL)
        ok = mf_includes(p, include);
    p->depth--;
    de__toml_free(res);
    return ok;
}

/* ---- Finalize: sort settings, overlay defaults ------------------------------------------- */

static int mf_kv_cmp(const void *a, const void *b, void *user) {
    (void)user;
    return strcmp(((const de__manifest_kv *)a)->key, ((const de__manifest_kv *)b)->key);
}

static bool mf_sort_kvs(mf_parser *p, de__manifest_kv *kvs, u32 count) {
    if (count < 2)
        return true;
    return de_sort_stable(kvs, count, sizeof *kvs, mf_kv_cmp, NULL, p->tmp) == DE_OK;
}

static bool mf_finalize(mf_parser *p) {
    de__manifest *m = p->m;
    u32 nd = p->defaults.count;
    de__manifest_defaults *defs = NULL;
    if (nd != 0) {
        defs = (de__manifest_defaults *)DE_ALLOC(&p->aa, sizeof *defs * nd,
                                                 DE_ALIGNOF(de__manifest_defaults), DE_MEM_ASSET);
        if (defs == NULL)
            return false;
    }
    for (u32 i = 0; i < nd; ++i) {
        mf_defaults_b *d = &DE_ARRAY_AT(&p->defaults, mf_defaults_b, i);
        de__manifest_kv *kvs = (de__manifest_kv *)d->kvs.data;
        if (!mf_sort_kvs(p, kvs, d->kvs.count))
            return false;
        defs[i] = (de__manifest_defaults){d->type, kvs, d->kvs.count};
    }
    m->defaults = defs;
    m->defaults_count = nd;

    for (u32 i = 0; i < p->assets.count; ++i) {
        de__manifest_asset *a = &DE_ARRAY_AT(&p->assets, de__manifest_asset, i);
        de__manifest_kv *own = (de__manifest_kv *)(void *)a->settings;
        if (!mf_sort_kvs(p, own, a->setting_count))
            return false;
        const de__manifest_defaults *d = de__manifest_find_defaults(m, a->type);
        if (d == NULL || d->setting_count == 0)
            continue;
        u32 cap = a->setting_count + d->setting_count;
        de__manifest_kv *out = (de__manifest_kv *)DE_ALLOC(
            &p->aa, sizeof *out * cap, DE_ALIGNOF(de__manifest_kv), DE_MEM_ASSET);
        if (out == NULL)
            return false;
        /* Merge two sorted lists; on an equal key the asset's value wins. */
        u32 ia = 0, id = 0, n = 0;
        while (ia < a->setting_count || id < d->setting_count) {
            int c;
            if (ia == a->setting_count)
                c = 1;
            else if (id == d->setting_count)
                c = -1;
            else
                c = strcmp(own[ia].key, d->settings[id].key);
            if (c < 0) {
                out[n++] = own[ia++];
            } else if (c > 0) {
                out[n++] = d->settings[id++];
            } else {
                out[n++] = own[ia++];
                id++;
            }
        }
        a->settings = out;
        a->setting_count = n;
    }

    m->files = (const char *const *)p->files.data;
    m->file_count = p->files.count;
    m->assets = (const de__manifest_asset *)p->assets.data;
    m->asset_count = p->assets.count;
    m->bundles = (const de__manifest_bundle *)p->bundles.data;
    m->bundle_count = p->bundles.count;
    m->issues = (const de__manifest_issue *)p->issues.data;
    m->issue_count = p->issues.count;
    if (m->asset_root == NULL)
        m->asset_root = "";
    return true;
}

/* ---- Entry points ------------------------------------------------------------------------ */

static de_result mf_default_read(void *user, const char *vpath, const de_allocator *a, void **data,
                                 u64 *size) {
    (void)user;
    return de_file_read_all(vpath, a, data, size);
}

de_result de__manifest_parse(const de__manifest_parse_desc *d, de__manifest **out) {
    if (out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__manifest_parse: out is NULL");
    *out = NULL;
    if (d == NULL || d->path == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__manifest_parse: no manifest path");
    char root[DE_FS_PATH_MAX];
    if (de_path_normalize(d->path, root, sizeof root) != DE_OK)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "asset manifest: \"%s\" is not a virtual path (e.g. dev:/assets.toml)",
                       d->path);

    mf_parser p;
    memset(&p, 0, sizeof p);
    p.tmp = d->allocator != NULL ? d->allocator : de_heap_allocator();
    p.read = d->read != NULL ? d->read : mf_default_read;
    p.read_user = d->read_user;

    p.arena = DE_NEW(p.tmp, mf_arena, DE_MEM_ASSET);
    if (p.arena == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "asset manifest: out of memory");
    memset(p.arena, 0, sizeof *p.arena);
    p.arena->backing = p.tmp;
    p.aa = (de_allocator){mf_arena_alloc, mf_arena_realloc, mf_arena_free, p.arena};

    de_result result = DE_OK;
    p.m = DE_NEW(&p.aa, de__manifest, DE_MEM_ASSET);
    if (p.m == NULL) {
        mf_arena_release(p.arena);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "asset manifest: out of memory");
    }
    memset(p.m, 0, sizeof *p.m);
    p.m->arena_ = p.arena;

    DE_ARRAY_INIT_TAG(&p.files, const char *, &p.aa, DE_MEM_ASSET);
    DE_ARRAY_INIT_TAG(&p.defaults, mf_defaults_b, &p.aa, DE_MEM_ASSET);
    DE_ARRAY_INIT_TAG(&p.assets, de__manifest_asset, &p.aa, DE_MEM_ASSET);
    DE_ARRAY_INIT_TAG(&p.bundles, de__manifest_bundle, &p.aa, DE_MEM_ASSET);
    DE_ARRAY_INIT_TAG(&p.issues, de__manifest_issue, &p.aa, DE_MEM_ASSET);
    de_strbuf_init(&p.keypath, p.tmp);
    p.keypath.tag = DE_MEM_ASSET;
    de_hashmap_desc hd = {.allocator = p.tmp,
                          .key_size = sizeof(u64),
                          .value_size = sizeof(u32),
                          .tag = DE_MEM_ASSET};
    bool ok = de_hashmap_init(&p.asset_ids, &hd) == DE_OK &&
              de_hashmap_init(&p.bundle_names, &hd) == DE_OK;

    if (ok) {
        p.m->root = mf_strdup(&p, root);
        ok = p.m->root != NULL;
    }
    ok = ok && mf_load_file(&p, root, NULL, NULL, 0);
    ok = ok && mf_finalize(&p);
    if (!ok) {
        /* The failing allocator set the error context; wrap it. */
        result = DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "asset manifest %s: out of memory", root);
    }

    de_hashmap_free(&p.asset_ids);
    de_hashmap_free(&p.bundle_names);
    de_strbuf_free(&p.keypath);
    if (result != DE_OK) {
        mf_arena_release(p.arena);
        return result;
    }
    *out = p.m;
    return DE_OK;
}

void de__manifest_destroy(de__manifest *m) {
    if (m == NULL)
        return;
    mf_arena_release((mf_arena *)m->arena_);
}

/* ---- Lookups ----------------------------------------------------------------------------- */

const de__manifest_asset *de__manifest_find_asset(const de__manifest *m, const char *id) {
    if (m == NULL || id == NULL)
        return NULL;
    for (u32 i = 0; i < m->asset_count; ++i)
        if (strcmp(m->assets[i].id, id) == 0)
            return &m->assets[i];
    return NULL;
}

const de__manifest_bundle *de__manifest_find_bundle(const de__manifest *m, const char *name) {
    if (m == NULL || name == NULL)
        return NULL;
    for (u32 i = 0; i < m->bundle_count; ++i)
        if (strcmp(m->bundles[i].name, name) == 0)
            return &m->bundles[i];
    return NULL;
}

const de__manifest_defaults *de__manifest_find_defaults(const de__manifest *m, const char *type) {
    if (m == NULL || type == NULL)
        return NULL;
    for (u32 i = 0; i < m->defaults_count; ++i)
        if (strcmp(m->defaults[i].type, type) == 0)
            return &m->defaults[i];
    return NULL;
}

const de__manifest_kv *de__manifest_find_setting(const de__manifest_kv *settings, u32 count,
                                                 const char *key) {
    if (settings == NULL || key == NULL)
        return NULL;
    u32 lo = 0, hi = count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        int c = strcmp(settings[mid].key, key);
        if (c == 0)
            return &settings[mid];
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

/* Folds ASCII case, and `_` to `-` (both separate subtags: "pt_BR" is "pt-BR"). */
static char mf_lower(char c) {
    if (c == '_')
        return '-';
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Tag compare of `a` with the first `len` bytes of `b` (and `a` has len): ASCII case-insensitive,
 * `_` equal to `-`. */
static bool mf_tag_eq(const char *a, const char *b, size_t len) {
    if (strlen(a) != len)
        return false;
    for (size_t i = 0; i < len; ++i)
        if (mf_lower(a[i]) != mf_lower(b[i]))
            return false;
    return true;
}

const char *de__manifest_source_for(const de__manifest *m, const de__manifest_asset *asset,
                                    const char *locale) {
    (void)m;
    if (asset == NULL)
        return NULL;
    if (locale == NULL || locale[0] == '\0' || asset->locale_count == 0)
        return asset->source;
    size_t len = strlen(locale);
    while (len != 0) {
        for (u32 i = 0; i < asset->locale_count; ++i)
            if (mf_tag_eq(asset->locales[i].locale, locale, len))
                return asset->locales[i].source;
        /* Drop the last subtag: "zh-Hant-TW" -> "zh-Hant" -> "zh". */
        while (len != 0 && locale[len - 1] != '-' && locale[len - 1] != '_')
            --len;
        if (len != 0)
            --len; /* the separator */
    }
    return asset->source;
}
