/* cvar_toml.c: config.toml load and save for the cvar registry (SPEC §5.8).
 *
 * Load parses with the vendored tomlc17 (de_tp_tomlc17) and walks the tree in document order:
 * every non-table value is one cvar, named by its dotted key path, and is set through the
 * registry as a startup value (or kept pending). Save writes the SAVED cvars itself; the format
 * is simple enough that the writer needs no library.
 *
 * tomlc17 takes its allocator through process-global hooks without a user pointer (toml_set_option,
 * see the Trama page "Vendoring"). The hooks below put a 16-byte header in front of every block
 * that records the de_allocator and the size, so each block goes back to the allocator it came
 * from. Which allocator a parse uses is a thread-local set around the parse and the free.
 *
 * Paths are virtual (`user:/config.toml`, DENG-247): load reads the whole file with
 * de_file_read_all, save replaces it with de_file_write_all_atomic (temporary file + rename), so a
 * crash during a save never leaves a half-written config.
 */

#include <dengine/de_base.h>
#include <dengine/de_debug.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "core/de_compiler.h"
#include "core/de_cvar_internal.h"
#include "core/de_strbuf.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <tomlc17.h>

/* ---- tomlc17 allocation hooks ------------------------------------------------------------ */

enum { TOML_HDR = 16 }; /* header bytes; keeps tomlc17's blocks 16-byte aligned */

typedef struct toml_block_hdr {
    const de_allocator *a;
    size_t size; /* bytes after the header */
} toml_block_hdr;
DE_STATIC_ASSERT(sizeof(toml_block_hdr) <= TOML_HDR, "tomlc17 block header too large");

/* The allocator for tomlc17 blocks allocated on this thread. NULL: the cvar registry's. */
static DE_THREAD_LOCAL const de_allocator *t_toml_alloc;

static void *toml_hook_realloc(void *ptr, size_t size) {
    if (ptr == NULL) {
        const de_allocator *a = t_toml_alloc != NULL ? t_toml_alloc : de__cvar_allocator();
        u8 *raw = (u8 *)DE_ALLOC(a, size + TOML_HDR, TOML_HDR, DE_MEM_GENERAL);
        if (raw == NULL)
            return NULL;
        toml_block_hdr *h = (toml_block_hdr *)(void *)raw;
        h->a = a;
        h->size = size;
        return raw + TOML_HDR;
    }
    u8 *raw = (u8 *)ptr - TOML_HDR;
    toml_block_hdr *h = (toml_block_hdr *)(void *)raw;
    const de_allocator *a = h->a;
    u8 *grown =
        (u8 *)DE_REALLOC(a, raw, h->size + TOML_HDR, size + TOML_HDR, TOML_HDR, DE_MEM_GENERAL);
    if (grown == NULL)
        return NULL;
    ((toml_block_hdr *)(void *)grown)->size = size;
    return grown + TOML_HDR;
}

static void toml_hook_free(void *ptr) {
    if (ptr == NULL)
        return;
    u8 *raw = (u8 *)ptr - TOML_HDR;
    const toml_block_hdr *h = (const toml_block_hdr *)(void *)raw;
    DE_FREE(h->a, raw, h->size + TOML_HDR);
}

/* Installs the hooks once. Guarded by the registry lock; the cvar code is the only tomlc17 user
 * in the engine so far, so no other thread parses while they change. */
static bool g_toml_hooks_installed; /* registry lock */

static void toml_hooks_install(void) {
    de__cvar_lock();
    if (!g_toml_hooks_installed) {
        toml_option_t opt = toml_default_option();
        opt.check_utf8 = true;
        opt.mem_realloc = toml_hook_realloc;
        opt.mem_free = toml_hook_free;
        toml_set_option(opt);
        g_toml_hooks_installed = true;
    }
    de__cvar_unlock();
}

/* ---- Load -------------------------------------------------------------------------------- */

/* Converts a leaf value to the text de_cvar_set would get. false for arrays and dates. */
static bool datum_text(const toml_datum_t *d, char *buf, size_t cap, const char **out) {
    switch (d->type) {
    case TOML_STRING:
        *out = d->u.str.ptr;
        return true;
    case TOML_INT64:
        snprintf(buf, cap, "%lld", (long long)d->u.int64);
        *out = buf;
        return true;
    case TOML_FP64: {
        f64 v = d->u.fp64;
        if (v != v)
            snprintf(buf, cap, "nan");
        else if (v > 1.7976931348623157e308)
            snprintf(buf, cap, "inf");
        else if (v < -1.7976931348623157e308)
            snprintf(buf, cap, "-inf");
        else
            snprintf(buf, cap, "%.17g", v);
        for (char *p = buf; *p != '\0'; ++p)
            if (*p == ',')
                *p = '.'; /* a C locale with a decimal comma */
        *out = buf;
        return true;
    }
    case TOML_BOOLEAN:
        *out = d->u.boolean ? "true" : "false";
        return true;
    default:
        return false;
    }
}

typedef struct load_ctx {
    const char *source; /* for messages */
    de__cvar_batch batch;
    char path[DE_CVAR_NAME_MAX + 2]; /* dotted key path being walked */
} load_ctx;

static void load_table(load_ctx *c, const toml_datum_t *tab, size_t path_len) {
    for (int32_t i = 0; i < tab->u.tab.size; ++i) {
        const char *key = tab->u.tab.key[i];
        size_t key_len = (size_t)tab->u.tab.len[i];
        const toml_datum_t *v = &tab->u.tab.value[i];
        size_t len = path_len + (path_len != 0 ? 1u : 0u) + key_len;
        if (len > DE_CVAR_NAME_MAX) {
            de__cvar_batch_note(
                &c->batch, DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                                   "%s: line %d: the key path %.*s.%.*s is longer than %d bytes",
                                   c->source, v->lineno, (int)path_len, c->path,
                                   key_len > 64 ? 64 : (int)key_len, key, (int)DE_CVAR_NAME_MAX));
            continue;
        }
        size_t at = path_len;
        if (path_len != 0)
            c->path[at++] = '.';
        memcpy(c->path + at, key, key_len);
        c->path[len] = '\0';
        if (v->type == TOML_TABLE) {
            load_table(c, v, len);
        } else {
            char buf[64];
            const char *text;
            if (!datum_text(v, buf, sizeof buf, &text)) {
                de__cvar_batch_note(&c->batch,
                                    DE_FAIL(DE_ERR_PARSE,
                                            "%s: line %d: %s: arrays and dates are not cvar values",
                                            c->source, v->lineno, c->path));
            } else {
                de__cvar_batch_note(
                    &c->batch,
                    de__cvar_set_by_name(c->path, len, text, DE__CVAR_SRC_STARTUP, c->source));
            }
        }
        c->path[path_len] = '\0';
    }
}

static de_result load_text(const char *text, size_t len, const char *source) {
    if (text == NULL && len != 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s: text is NULL", source);
    if (len > (size_t)INT_MAX)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s: %zu bytes is too large", source, len);
    toml_hooks_install();
    const de_allocator *a = de__cvar_allocator();
    t_toml_alloc = a;
    toml_result_t res = toml_parse(text != NULL ? text : "", (int)len);
    t_toml_alloc = NULL;
    de_result r = DE_OK;
    if (!res.ok) {
        r = DE_FAIL(DE_ERR_PARSE, "%s: %s", source, res.errmsg);
    } else {
        load_ctx *c =
            (load_ctx *)DE_ALLOC(a, sizeof(load_ctx), DE_ALIGNOF(load_ctx), DE_MEM_GENERAL);
        if (c == NULL) {
            r = DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "%s: no memory", source);
        } else {
            memset(c, 0, sizeof *c);
            c->source = source;
            load_table(c, &res.toptab, 0);
            r = de__cvar_batch_result(&c->batch);
            DE_FREE(a, c, sizeof(load_ctx));
        }
    }
    t_toml_alloc = a;
    toml_free(res);
    t_toml_alloc = NULL;
    return r;
}

de_result de_cvar_load_from_memory(const char *text, size_t len) {
    return load_text(text, len, "config (memory)");
}

de_result de_cvar_load(const char *vpath) {
    if (vpath == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_cvar_load: path is NULL");
    const de_allocator *a = de__cvar_allocator();
    void *data = NULL;
    u64 size = 0;
    de_result r = de_file_read_all(vpath, a, &data, &size);
    if (r != DE_OK)
        return DE_FAIL_WRAP(r, "config %s: cannot read it", vpath);
    r = load_text((const char *)data, (size_t)size, vpath);
    DE_FREE(a, data, (size_t)size + 1);
    return r;
}

/* ---- Save -------------------------------------------------------------------------------- */

/* Shortest text that reads back as exactly `v`, in TOML float syntax. */
static void format_float(f64 v, char *buf, size_t cap) {
    if (v > 1.7976931348623157e308) {
        snprintf(buf, cap, "inf");
        return;
    }
    if (v < -1.7976931348623157e308) {
        snprintf(buf, cap, "-inf");
        return;
    }
    for (int prec = 1; prec <= 17; ++prec) {
        snprintf(buf, cap, "%.*g", prec, v);
        for (char *p = buf; *p != '\0'; ++p)
            if (*p == ',')
                *p = '.';
        f64 back;
        if (de_str_to_f64(de_str_from_cstr(buf), &back) == DE_OK && back == v &&
            signbit(back) == signbit(v)) /* keep the sign of zero */
            break;
    }
    if (strpbrk(buf, ".eEn") == NULL) { /* "2" is an integer in TOML: make it "2.0" */
        size_t n = strlen(buf);
        if (n + 3 <= cap)
            memcpy(buf + n, ".0", 3);
    }
}

/* Appends `s` as a TOML basic string. */
static de_result append_toml_string(de_strbuf *b, const char *s) {
    DE_TRY(de_strbuf_append_char(b, '"'));
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; ++p) {
        unsigned char ch = *p;
        switch (ch) {
        case '"':
            DE_TRY(de_strbuf_append_cstr(b, "\\\""));
            break;
        case '\\':
            DE_TRY(de_strbuf_append_cstr(b, "\\\\"));
            break;
        case '\b':
            DE_TRY(de_strbuf_append_cstr(b, "\\b"));
            break;
        case '\t':
            DE_TRY(de_strbuf_append_cstr(b, "\\t"));
            break;
        case '\n':
            DE_TRY(de_strbuf_append_cstr(b, "\\n"));
            break;
        case '\f':
            DE_TRY(de_strbuf_append_cstr(b, "\\f"));
            break;
        case '\r':
            DE_TRY(de_strbuf_append_cstr(b, "\\r"));
            break;
        default:
            if (ch < 0x20 || ch == 0x7f)
                DE_TRY(de_strbuf_appendf(b, "\\u%04X", (unsigned)ch));
            else
                DE_TRY(de_strbuf_append_char(b, (char)ch));
        }
    }
    return de_strbuf_append_char(b, '"');
}

/* Appends the help text as comment lines. */
static de_result append_help(de_strbuf *b, const char *help) {
    while (*help != '\0') {
        const char *nl = strchr(help, '\n');
        size_t n = nl != NULL ? (size_t)(nl - help) : strlen(help);
        DE_TRY(de_strbuf_append_cstr(b, "# "));
        DE_TRY(de_strbuf_append(b, (de_str){help, n}));
        DE_TRY(de_strbuf_append_char(b, '\n'));
        help += n;
        if (*help == '\n')
            ++help;
    }
    return DE_OK;
}

/* Appends "<key> = <value>\n" for `e`, with its help above it. Caller holds the lock. */
static de_result append_entry(de_strbuf *b, const de__cvar_entry *e, const char *key) {
    DE_TRY(append_help(b, e->help));
    DE_TRY(de_strbuf_appendf(b, "%s = ", key));
    u64 bits = de_atomic_load_u64(&e->bits, DE_ATOMIC_RELAXED);
    switch (e->type) {
    case DE_CVAR_BOOL:
        DE_TRY(de_strbuf_append_cstr(b, bits != 0 ? "true" : "false"));
        break;
    case DE_CVAR_INT:
        DE_TRY(de_strbuf_appendf(b, "%lld", (long long)(i64)bits));
        break;
    case DE_CVAR_FLOAT: {
        f64 v;
        memcpy(&v, &bits, sizeof v);
        char buf[48];
        format_float(v, buf, sizeof buf);
        DE_TRY(de_strbuf_append_cstr(b, buf));
        break;
    }
    case DE_CVAR_STRING:
        DE_TRY(append_toml_string(b, (const char *)de_atomic_load_ptr(&e->str, DE_ATOMIC_RELAXED)));
        break;
    case DE_CVAR_ENUM:
        DE_TRY(append_toml_string(b, bits < e->enum_count ? e->enum_values[bits] : ""));
        break;
    case DE_CVAR_TYPE_COUNT_:
        break;
    }
    return de_strbuf_append_char(b, '\n');
}

/* Length of the table part of a dotted name ("r.shadows" of "r.shadows.quality"); 0 if none. */
static size_t table_len(const de__cvar_entry *e) {
    const char *dot = strrchr(e->name, '.');
    return dot != NULL ? (size_t)(dot - e->name) : 0;
}

static bool saved(const de__cvar_entry *e) {
    return (e->flags & DE_CVAR_SAVED) != 0;
}

/* Writes the SAVED cvars into `b`. Caller holds the lock. O(n²) in the saved cvars, which keeps
 * the order rule simple (tables in order of their first cvar) without a map. */
static de_result write_config_locked(de_strbuf *b) {
    DE_TRY(de_strbuf_append_cstr(
        b, "# DEngine config, written by de_cvar_save. Keys are cvar names; a [table] holds the\n"
           "# cvars whose names start with its name.\n"));
    u32 count = de__cvar_count_locked();
    bool first_top = true;
    for (u32 i = 0; i < count; ++i) {
        const de__cvar_entry *e = de__cvar_at_locked(i);
        if (!saved(e) || table_len(e) != 0)
            continue;
        if (first_top)
            DE_TRY(de_strbuf_append_char(b, '\n'));
        first_top = false;
        DE_TRY(append_entry(b, e, e->name));
    }
    for (u32 i = 0; i < count; ++i) {
        const de__cvar_entry *e = de__cvar_at_locked(i);
        size_t tl = table_len(e);
        if (!saved(e) || tl == 0)
            continue;
        bool seen = false; /* this table was written with an earlier cvar */
        for (u32 j = 0; j < i && !seen; ++j) {
            const de__cvar_entry *o = de__cvar_at_locked(j);
            seen = saved(o) && table_len(o) == tl && memcmp(o->name, e->name, tl) == 0;
        }
        if (seen)
            continue;
        DE_TRY(de_strbuf_appendf(b, "\n[%.*s]\n", (int)tl, e->name));
        for (u32 j = i; j < count; ++j) {
            const de__cvar_entry *o = de__cvar_at_locked(j);
            if (saved(o) && table_len(o) == tl && memcmp(o->name, e->name, tl) == 0)
                DE_TRY(append_entry(b, o, o->name + tl + 1));
        }
    }
    return DE_OK;
}

de_result de_cvar_save(const char *vpath) {
    if (vpath == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_cvar_save: path is NULL");
    de_strbuf b;
    de_strbuf_init(&b, de__cvar_allocator());
    de__cvar_lock();
    de_result r = write_config_locked(&b);
    de__cvar_unlock();
    if (r != DE_OK) {
        de_strbuf_free(&b);
        return DE_FAIL_WRAP(r, "config %s: cannot build the text", vpath);
    }
    r = de_file_write_all_atomic(vpath, de_strbuf_cstr(&b), b.len);
    if (r != DE_OK)
        r = DE_FAIL_WRAP(r, "config %s: cannot write it", vpath);
    de_strbuf_free(&b);
    return r;
}
