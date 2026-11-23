/*
 * world_cells.c: `de-cook world` (DENG-271), the world cook. See world_cells.h.
 *
 * The manifest directory is mounted read-only as `wcook:/`, the output directory writable as
 * `wcout:/`; both are unmounted before the command returns. Scene sources are parsed with the
 * core TOML parser; the cell files are written from the parsed values, keys sorted, floats in
 * their shortest round-trip form (de__ser_format_f64), so the bytes depend only on the input.
 *
 * Known gap: an entity reference (a component field that holds another entity's scene ID) that
 * crosses a cell boundary is written unchanged; the cell's scene import then reports it as a
 * reference outside the file. Keep cross-referencing entities under one root.
 */
#include "world_cells.h"

#include "asset/manifest_internal.h"
#include "asset/resolve_internal.h"
#include "core/de_strbuf.h"
#include "core/de_toml_internal.h"
#include "reflect/ser_internal.h"
#include "world/streaming/streaming_internal.h"

#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WC_IN_ROOT    "wcook"
#define WC_IN_PREFIX  "wcook:/"
#define WC_OUT_ROOT   "wcout"
#define WC_OUT_PREFIX "wcout:/"

typedef struct wc_ent {
    u64 id;
    u64 parent_id; /* 0: a root */
    const toml_datum_t *tab;
    const char *source; /* scene source path, for messages */
    int line;
    u32 root;      /* index of its root in the sorted array */
    i32 cx, cy;    /* its root's cell */
    bool placed;   /* false: left out (no DeTransform on the root) */
} wc_ent;

typedef struct wc_run {
    const de_cook_io *io;
    const de_allocator *a;
    char dir[DE_FS_PATH_MAX];    /* manifest directory, native */
    char prefix[DE_FS_PATH_MAX]; /* the directory as given, with its separator, for messages */
    char root[DE_FS_PATH_MAX];   /* the manifest's vpath under wcook:/ */
    const char *out_arg;         /* --out, or NULL */
    const char *world_name;
    bool in_mounted, out_mounted;
    char out_native[DE_FS_PATH_MAX];
    u32 errors, warnings;
    /* scenes */
    toml_result_t *docs;
    u32 doc_count, doc_cap;
    wc_ent *ents;
    u32 ent_count, ent_cap;
} wc_run;

/* ---- Output helpers ------------------------------------------------------------------------ */

static void wc_put(const wc_run *run, de_cook_stream s, const char *text) {
    run->io->write(run->io->user, s, text, strlen(text));
}

static void wc_msg(wc_run *run, const char *kind, const char *file, int line, const char *fmt,
                   ...) DE_PRINTF_FMT(5, 6);
static void wc_msg(wc_run *run, const char *kind, const char *file, int line, const char *fmt,
                   ...) {
    char msg[768];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    char head[DE_FS_PATH_MAX + 64];
    const char *shown = file != NULL ? file : "";
    if (strncmp(shown, WC_IN_PREFIX, sizeof WC_IN_PREFIX - 1) == 0)
        shown += sizeof WC_IN_PREFIX - 1;
    (void)snprintf(head, sizeof head, "%s%s:%d: %s: ", run->prefix, shown, line > 0 ? line : 0,
                   kind);
    wc_put(run, DE_COOK_STDOUT, head);
    wc_put(run, DE_COOK_STDOUT, msg);
    wc_put(run, DE_COOK_STDOUT, "\n");
    if (kind[0] == 'e')
        run->errors++;
    else
        run->warnings++;
}

static int wc_fatal(wc_run *run, const char *what, const char *arg) {
    wc_put(run, DE_COOK_STDERR, "de-cook world: ");
    wc_put(run, DE_COOK_STDERR, what);
    if (arg != NULL) {
        wc_put(run, DE_COOK_STDERR, " ");
        wc_put(run, DE_COOK_STDERR, arg);
    }
    wc_put(run, DE_COOK_STDERR, "\n");
    return DE_COOK_EXIT_USAGE;
}

static int wc_usage(wc_run *run) {
    wc_put(run, DE_COOK_STDERR,
           "usage: de-cook world <assets.toml> --world <name> [--out <dir>]\n"
           "  Splits the scenes of [worlds.<name>] into 256 m cells and writes\n"
           "  cell_<x>_<y>.scene.toml and cells.toml (default: <asset_root>/<cells_dir>).\n");
    return DE_COOK_EXIT_USAGE;
}

/* ---- Arguments and mounts ------------------------------------------------------------------ */

static int wc_args(wc_run *run, int argc, const char *const *argv, const char **manifest) {
    *manifest = NULL;
    for (int i = 0; i < argc; ++i) {
        const char *s = argv[i];
        if (strcmp(s, "--world") == 0 && i + 1 < argc) {
            run->world_name = argv[++i];
        } else if (strcmp(s, "--out") == 0 && i + 1 < argc) {
            run->out_arg = argv[++i];
        } else if (strcmp(s, "--help") == 0 || strcmp(s, "-h") == 0) {
            return wc_usage(run);
        } else if (s[0] == '-' && s[1] != '\0') {
            wc_put(run, DE_COOK_STDERR, "de-cook world: unknown or incomplete option ");
            wc_put(run, DE_COOK_STDERR, s);
            wc_put(run, DE_COOK_STDERR, "\n");
            return wc_usage(run);
        } else if (*manifest == NULL) {
            *manifest = s;
        } else {
            return wc_usage(run);
        }
    }
    if (*manifest == NULL || run->world_name == NULL)
        return wc_usage(run);
    return -1;
}

/* Splits the manifest argument into its directory and file name (like `--check`). */
static int wc_split(wc_run *run, const char *arg) {
    size_t len = strlen(arg);
    if (len == 0 || len >= sizeof run->dir)
        return wc_fatal(run, "manifest path is empty or too long", NULL);
    size_t sep = len;
    for (size_t i = len; i > 0; --i)
        if (arg[i - 1] == '/' || arg[i - 1] == '\\') {
            sep = i - 1;
            break;
        }
    const char *name = arg;
    if (sep == len) {
        (void)snprintf(run->dir, sizeof run->dir, ".");
        run->prefix[0] = '\0';
    } else {
        size_t dl = sep;
        if (sep == 0 || (sep == 2 && arg[1] == ':'))
            dl = sep + 1;
        memcpy(run->dir, arg, dl);
        run->dir[dl] = '\0';
        memcpy(run->prefix, arg, sep + 1);
        run->prefix[sep + 1] = '\0';
        name = arg + sep + 1;
    }
    char raw[DE_FS_PATH_MAX];
    int n = snprintf(raw, sizeof raw, "%s%s", WC_IN_PREFIX, name);
    if (n < 0 || (size_t)n >= sizeof raw ||
        de_path_normalize(raw, run->root, sizeof run->root) != DE_OK)
        return wc_fatal(run, "cannot use this file name as a manifest:", name);
    return -1;
}

/* ---- Small helpers ------------------------------------------------------------------------- */

static void *wc_alloc(wc_run *run, size_t bytes) {
    return DE_ALLOC(run->a, bytes ? bytes : 1u, 16u, DE_MEM_SCRATCH);
}

static const toml_datum_t *wc_get(const toml_datum_t *t, const char *key) {
    if (t == NULL || t->type != TOML_TABLE)
        return NULL;
    for (int32_t i = 0; i < t->u.tab.size; ++i)
        if (strcmp(t->u.tab.key[i], key) == 0)
            return &t->u.tab.value[i];
    return NULL;
}

/* A scene ID: "0x" + 1..16 hex digits, or a positive integer (as the scene importer reads). */
static u64 wc_scene_id_str(const char *s) {
    u64 v = 0;
    size_t len = strlen(s);
    if (len > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        return de__ser_parse_hex64(s, len, &v) ? v : 0u;
    if (len == 0 || len > 20)
        return 0;
    for (size_t i = 0; i < len; ++i) {
        if (s[i] < '0' || s[i] > '9')
            return 0;
        u64 d = (u64)(s[i] - '0');
        if (v > (UINT64_MAX - d) / 10u)
            return 0;
        v = v * 10u + d;
    }
    return v;
}

static u64 wc_scene_id(const toml_datum_t *v) {
    if (v->type == TOML_INT64)
        return v->u.int64 > 0 ? (u64)v->u.int64 : 0u;
    if (v->type == TOML_STRING)
        return wc_scene_id_str(v->u.str.ptr);
    return 0;
}

static bool wc_num(const toml_datum_t *v, f64 *out) {
    if (v == NULL)
        return false;
    if (v->type == TOML_FP64) {
        *out = v->u.fp64;
        return true;
    }
    if (v->type == TOML_INT64) {
        *out = (f64)v->u.int64;
        return true;
    }
    return false;
}

/* DeTransform.position as [x, y, z] or { x, y, z }. */
static bool wc_position(const toml_datum_t *tf, f64 p[3]) {
    const toml_datum_t *pos = wc_get(tf, "position");
    if (pos == NULL)
        return false;
    if (pos->type == TOML_ARRAY) {
        if (pos->u.arr.size != 3)
            return false;
        for (int i = 0; i < 3; ++i)
            if (!wc_num(&pos->u.arr.elem[i], &p[i]))
                return false;
        return true;
    }
    if (pos->type == TOML_TABLE)
        return wc_num(wc_get(pos, "x"), &p[0]) && wc_num(wc_get(pos, "y"), &p[1]) &&
               wc_num(wc_get(pos, "z"), &p[2]);
    return false;
}

/* ---- Reading the scenes -------------------------------------------------------------------- */

static bool wc_push_ent(wc_run *run, const wc_ent *e) {
    if (run->ent_count == run->ent_cap) {
        u32 cap = run->ent_cap ? run->ent_cap * 2u : 64u;
        wc_ent *n = (wc_ent *)wc_alloc(run, sizeof *n * cap);
        if (n == NULL)
            return false;
        if (run->ents != NULL) {
            memcpy(n, run->ents, sizeof *n * run->ent_count);
            DE_FREE(run->a, run->ents, sizeof *n * run->ent_cap);
        }
        run->ents = n;
        run->ent_cap = cap;
    }
    run->ents[run->ent_count++] = *e;
    return true;
}

static bool wc_entity(wc_run *run, const char *src, const toml_datum_t *t, const char *key) {
    wc_ent e;
    memset(&e, 0, sizeof e);
    e.tab = t;
    e.source = src;
    e.line = t->lineno;
    if (key != NULL) {
        e.id = wc_scene_id_str(key);
        if (e.id == 0) {
            wc_msg(run, "error", src, t->lineno, "entity key `%.64s` is not a scene ID", key);
            return true;
        }
    }
    for (int32_t k = 0; k < t->u.tab.size; ++k) {
        const char *name = t->u.tab.key[k];
        const toml_datum_t *v = &t->u.tab.value[k];
        if (strcmp(name, "id") == 0) {
            if (key != NULL) {
                wc_msg(run, "error", src, v->lineno, "a keyed entity table has no `id`");
                return true;
            }
            e.id = wc_scene_id(v);
        } else if (strcmp(name, "parent") == 0) {
            e.parent_id = wc_scene_id(v);
            if (e.parent_id == 0) {
                wc_msg(run, "error", src, v->lineno, "`parent` must be a scene ID");
                return true;
            }
        }
    }
    if (e.id == 0) {
        wc_msg(run, "error", src, t->lineno, "entity without a valid `id`");
        return true;
    }
    return wc_push_ent(run, &e);
}

static bool wc_read_scene(wc_run *run, const de__manifest *m, const de__manifest_asset *as,
                          u32 doc) {
    char vpath[DE_FS_PATH_MAX], raw[DE_FS_PATH_MAX];
    const char *ar = m->asset_root != NULL ? m->asset_root : "";
    int n = snprintf(raw, sizeof raw, "%s%s%s%s", WC_IN_PREFIX, ar, ar[0] ? "/" : "", as->source);
    if (n < 0 || (size_t)n >= sizeof raw || de_path_normalize(raw, vpath, sizeof vpath) != DE_OK) {
        wc_msg(run, "error", as->file, (int)as->line, "bad scene source path `%s`", as->source);
        return true;
    }
    void *data = NULL;
    u64 size = 0;
    de_result r = de_file_read_all(vpath, run->a, &data, &size);
    if (r != DE_OK) {
        wc_msg(run, "error", as->file, (int)as->line, "cannot read scene `%s`: %s", as->source,
               de_result_str(r));
        return true;
    }
    if (size > (u64)INT32_MAX) {
        DE_FREE(run->a, data, size + 1);
        wc_msg(run, "error", vpath, 0, "scene file is too large");
        return true;
    }
    run->docs[doc] = de__toml_parse(run->a, (const char *)data, (int)size);
    DE_FREE(run->a, data, size + 1);
    const toml_result_t *tr = &run->docs[doc];
    if (!tr->ok) {
        wc_msg(run, "error", vpath, 0, "TOML: %s", tr->errmsg);
        return true;
    }
    for (int32_t k = 0; k < tr->toptab.u.tab.size; ++k) {
        const char *key = tr->toptab.u.tab.key[k];
        const toml_datum_t *v = &tr->toptab.u.tab.value[k];
        if (strcmp(key, "prefab") == 0 && v->type == TOML_TABLE) {
            if (v->u.tab.size > 0)
                wc_msg(run, "warning", vpath, v->lineno,
                       "the editor [prefab] table is not copied into the cells");
            continue;
        }
        if (strcmp(key, "entity") != 0) {
            wc_msg(run, "warning", vpath, v->lineno, "top-level key `%s` is not `entity`; skipped",
                   key);
            continue;
        }
        if (v->type == TOML_TABLE) {
            for (int32_t i = 0; i < v->u.tab.size; ++i) {
                if (v->u.tab.value[i].type != TOML_TABLE) {
                    wc_msg(run, "error", vpath, v->u.tab.value[i].lineno,
                           "`entity.%s` is not a table", v->u.tab.key[i]);
                    continue;
                }
                if (!wc_entity(run, vpath, &v->u.tab.value[i], v->u.tab.key[i]))
                    return false;
            }
        } else if (v->type == TOML_ARRAY) {
            for (int32_t i = 0; i < v->u.arr.size; ++i) {
                if (v->u.arr.elem[i].type != TOML_TABLE) {
                    wc_msg(run, "error", vpath, v->lineno, "`entity` must hold tables");
                    continue;
                }
                if (!wc_entity(run, vpath, &v->u.arr.elem[i], NULL))
                    return false;
            }
        } else {
            wc_msg(run, "error", vpath, v->lineno,
                   "`entity` must be [entity.0x...] tables or [[entity]]");
        }
    }
    return true;
}

static int wc_ent_cmp(const void *pa, const void *pb) {
    const wc_ent *a = (const wc_ent *)pa, *b = (const wc_ent *)pb;
    return a->id < b->id ? -1 : (a->id > b->id ? 1 : 0);
}

static u32 wc_find(const wc_run *run, u64 id) {
    u32 lo = 0, hi = run->ent_count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2u;
        if (run->ents[mid].id < id)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < run->ent_count && run->ents[lo].id == id ? lo : UINT32_MAX;
}

/* Sorts by ID, finds every entity's root and gives each root its cell. */
static void wc_place(wc_run *run) {
    if (run->ent_count > 1)
        qsort(run->ents, run->ent_count, sizeof *run->ents, wc_ent_cmp);
    for (u32 i = 1; i < run->ent_count; ++i)
        if (run->ents[i].id == run->ents[i - 1].id)
            wc_msg(run, "error", run->ents[i].source, run->ents[i].line,
                   "scene ID 0x%016llx is also used in %s (line %d)",
                   (unsigned long long)run->ents[i].id, run->ents[i - 1].source,
                   run->ents[i - 1].line);
    for (u32 i = 0; i < run->ent_count; ++i) {
        wc_ent *e = &run->ents[i];
        u32 cur = i, steps = 0;
        bool ok = true;
        while (run->ents[cur].parent_id != 0) {
            u32 p = wc_find(run, run->ents[cur].parent_id);
            if (p == UINT32_MAX) {
                wc_msg(run, "error", run->ents[cur].source, run->ents[cur].line,
                       "parent 0x%016llx is not an entity of the world",
                       (unsigned long long)run->ents[cur].parent_id);
                ok = false;
                break;
            }
            if (++steps > run->ent_count) {
                wc_msg(run, "error", e->source, e->line, "the parent chain of 0x%016llx is a cycle",
                       (unsigned long long)e->id);
                ok = false;
                break;
            }
            cur = p;
        }
        e->root = cur;
        if (!ok)
            continue;
        if (cur == i) {
            f64 pos[3];
            const toml_datum_t *tf = wc_get(e->tab, "DeTransform");
            if (tf == NULL || tf->type != TOML_TABLE || !wc_position(tf, pos)) {
                wc_msg(run, "warning", e->source, e->line,
                       "root 0x%016llx has no DeTransform position; left out of the cells",
                       (unsigned long long)e->id);
                continue;
            }
            e->cx = de__cell_coord(pos[0]);
            e->cy = de__cell_coord(pos[1]);
            e->placed = true;
        }
    }
    /* Children take their root's cell (roots are final after the loop above). */
    for (u32 i = 0; i < run->ent_count; ++i) {
        wc_ent *e = &run->ents[i];
        if (e->root != i && e->root < run->ent_count && run->ents[e->root].parent_id == 0) {
            e->cx = run->ents[e->root].cx;
            e->cy = run->ents[e->root].cy;
            e->placed = run->ents[e->root].placed;
        }
    }
}

/* ---- Writing TOML -------------------------------------------------------------------------- */

static bool wc_bare_key(const char *k) {
    if (k[0] == '\0')
        return false;
    for (const char *c = k; *c; ++c)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') ||
              *c == '_' || *c == '-'))
            return false;
    return true;
}

static bool wc_str(de_strbuf *b, const char *s, size_t len) {
    bool ok = de_strbuf_append_char(b, '"') == DE_OK;
    for (size_t i = 0; ok && i < len; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\')
            ok = de_strbuf_append_char(b, '\\') == DE_OK && de_strbuf_append_char(b, (char)c) == DE_OK;
        else if (c == '\n')
            ok = de_strbuf_append_cstr(b, "\\n") == DE_OK;
        else if (c == '\t')
            ok = de_strbuf_append_cstr(b, "\\t") == DE_OK;
        else if (c < 0x20 || c == 0x7f)
            ok = de_strbuf_appendf(b, "\\u%04x", (unsigned)c) == DE_OK;
        else
            ok = de_strbuf_append_char(b, (char)c) == DE_OK;
    }
    return ok && de_strbuf_append_char(b, '"') == DE_OK;
}

static bool wc_key(de_strbuf *b, const char *k) {
    if (wc_bare_key(k))
        return de_strbuf_append_cstr(b, k) == DE_OK;
    return wc_str(b, k, strlen(k));
}

static bool wc_f64(de_strbuf *b, f64 v) {
    char buf[DE__SER_NUM_CAP];
    (void)de__ser_format_f64(v, buf);
    return de_strbuf_append_cstr(b, buf) == DE_OK;
}

/* Key order of a table: indices sorted by key bytes (insertion sort; tables are small). */
static u32 *wc_key_order(wc_run *run, const toml_datum_t *t) {
    u32 n = (u32)t->u.tab.size;
    u32 *o = (u32 *)wc_alloc(run, sizeof *o * (n ? n : 1u));
    if (o == NULL)
        return NULL;
    for (u32 i = 0; i < n; ++i) {
        u32 j = i;
        while (j > 0 && strcmp(t->u.tab.key[o[j - 1]], t->u.tab.key[i]) > 0) {
            o[j] = o[j - 1];
            --j;
        }
        o[j] = i;
    }
    return o;
}

static void wc_free_order(wc_run *run, u32 *o, const toml_datum_t *t) {
    DE_FREE(run->a, o, sizeof *o * (t->u.tab.size ? (u32)t->u.tab.size : 1u));
}

static bool wc_value(wc_run *run, de_strbuf *b, const toml_datum_t *v, const char *src);

static bool wc_inline_table(wc_run *run, de_strbuf *b, const toml_datum_t *t, const char *src) {
    if (t->u.tab.size == 0)
        return de_strbuf_append_cstr(b, "{}") == DE_OK;
    u32 *o = wc_key_order(run, t);
    if (o == NULL)
        return false;
    bool ok = de_strbuf_append_cstr(b, "{ ") == DE_OK;
    for (int32_t i = 0; ok && i < t->u.tab.size; ++i) {
        if (i > 0)
            ok = de_strbuf_append_cstr(b, ", ") == DE_OK;
        ok = ok && wc_key(b, t->u.tab.key[o[i]]) && de_strbuf_append_cstr(b, " = ") == DE_OK &&
             wc_value(run, b, &t->u.tab.value[o[i]], src);
    }
    wc_free_order(run, o, t);
    return ok && de_strbuf_append_cstr(b, " }") == DE_OK;
}

static bool wc_value(wc_run *run, de_strbuf *b, const toml_datum_t *v, const char *src) {
    switch (v->type) {
    case TOML_STRING:
        return wc_str(b, v->u.str.ptr, (size_t)v->u.str.len);
    case TOML_INT64:
        return de_strbuf_appendf(b, "%lld", (long long)v->u.int64) == DE_OK;
    case TOML_FP64:
        return wc_f64(b, v->u.fp64);
    case TOML_BOOLEAN:
        return de_strbuf_append_cstr(b, v->u.boolean ? "true" : "false") == DE_OK;
    case TOML_ARRAY: {
        bool ok = de_strbuf_append_char(b, '[') == DE_OK;
        for (int32_t i = 0; ok && i < v->u.arr.size; ++i) {
            if (i > 0)
                ok = de_strbuf_append_cstr(b, ", ") == DE_OK;
            ok = ok && wc_value(run, b, &v->u.arr.elem[i], src);
        }
        return ok && de_strbuf_append_char(b, ']') == DE_OK;
    }
    case TOML_TABLE:
        return wc_inline_table(run, b, v, src);
    default:
        wc_msg(run, "error", src, v->lineno, "dates and times are not supported in world scenes");
        return de_strbuf_append_cstr(b, "\"\"") == DE_OK;
    }
}

/* One component table. For a root's DeTransform, `origin` (non-NULL) is subtracted from x/y. */
static bool wc_component(wc_run *run, de_strbuf *b, const char *hex, const char *name,
                         const toml_datum_t *t, const f64 *origin, const char *src) {
    bool ok = de_strbuf_appendf(b, "[entity.%s.", hex) == DE_OK && wc_key(b, name) &&
              de_strbuf_append_cstr(b, "]\n") == DE_OK;
    u32 *o = wc_key_order(run, t);
    if (o == NULL)
        return false;
    for (int32_t i = 0; ok && i < t->u.tab.size; ++i) {
        const char *k = t->u.tab.key[o[i]];
        const toml_datum_t *v = &t->u.tab.value[o[i]];
        ok = wc_key(b, k) && de_strbuf_append_cstr(b, " = ") == DE_OK;
        if (ok && origin != NULL && strcmp(k, "position") == 0) {
            f64 p[3];
            (void)wc_position(t, p);
            p[0] -= origin[0];
            p[1] -= origin[1];
            if (v->type == TOML_TABLE)
                ok = de_strbuf_append_cstr(b, "{ x = ") == DE_OK && wc_f64(b, p[0]) &&
                     de_strbuf_append_cstr(b, ", y = ") == DE_OK && wc_f64(b, p[1]) &&
                     de_strbuf_append_cstr(b, ", z = ") == DE_OK && wc_f64(b, p[2]) &&
                     de_strbuf_append_cstr(b, " }") == DE_OK;
            else
                ok = de_strbuf_append_char(b, '[') == DE_OK && wc_f64(b, p[0]) &&
                     de_strbuf_append_cstr(b, ", ") == DE_OK && wc_f64(b, p[1]) &&
                     de_strbuf_append_cstr(b, ", ") == DE_OK && wc_f64(b, p[2]) &&
                     de_strbuf_append_char(b, ']') == DE_OK;
        } else {
            ok = ok && wc_value(run, b, v, src);
        }
        ok = ok && de_strbuf_append_char(b, '\n') == DE_OK;
    }
    wc_free_order(run, o, t);
    return ok;
}

static bool wc_entity_out(wc_run *run, de_strbuf *b, const wc_ent *e) {
    char hex[DE__SER_NUM_CAP], phex[DE__SER_NUM_CAP];
    (void)de__ser_format_hex64(e->id, hex);
    bool ok = de_strbuf_appendf(b, "\n[entity.%s]\n", hex) == DE_OK;
    if (ok && e->parent_id != 0) {
        (void)de__ser_format_hex64(e->parent_id, phex);
        ok = de_strbuf_appendf(b, "parent = \"%s\"\n", phex) == DE_OK;
    }
    u32 *o = wc_key_order(run, e->tab);
    if (o == NULL)
        return false;
    /* Plain values first (they belong to [entity.0x..]), then the component tables. */
    for (int32_t i = 0; ok && i < e->tab->u.tab.size; ++i) {
        const char *k = e->tab->u.tab.key[o[i]];
        const toml_datum_t *v = &e->tab->u.tab.value[o[i]];
        if (v->type == TOML_TABLE || strcmp(k, "id") == 0 || strcmp(k, "parent") == 0)
            continue;
        ok = wc_key(b, k) && de_strbuf_append_cstr(b, " = ") == DE_OK &&
             wc_value(run, b, v, e->source) && de_strbuf_append_char(b, '\n') == DE_OK;
    }
    f64 origin[2] = {(f64)e->cx * DE__CELL_SIZE, (f64)e->cy * DE__CELL_SIZE};
    for (int32_t i = 0; ok && i < e->tab->u.tab.size; ++i) {
        const char *k = e->tab->u.tab.key[o[i]];
        const toml_datum_t *v = &e->tab->u.tab.value[o[i]];
        if (v->type != TOML_TABLE)
            continue;
        bool rel = e->parent_id == 0 && strcmp(k, "DeTransform") == 0;
        ok = wc_component(run, b, hex, k, v, rel ? origin : NULL, e->source);
    }
    wc_free_order(run, o, e->tab);
    return ok;
}

static bool wc_write(const char *rel, const de_strbuf *b) {
    char vpath[DE_FS_PATH_MAX];
    int n = snprintf(vpath, sizeof vpath, "%s%s", WC_OUT_PREFIX, rel);
    if (n < 0 || (size_t)n >= sizeof vpath)
        return false;
    return de_file_write_all_atomic(vpath, b->ptr != NULL ? b->ptr : "", (u64)b->len) == DE_OK;
}

/* ---- The command --------------------------------------------------------------------------- */

static int wc_cook(wc_run *run, const de__manifest *m, const de__manifest_world *w) {
    char cells_dir[256];
    if (w->cells_dir != NULL && w->cells_dir[0] != '\0')
        (void)snprintf(cells_dir, sizeof cells_dir, "%s", w->cells_dir);
    else
        (void)snprintf(cells_dir, sizeof cells_dir, "worlds/%s", w->name);

    run->docs = (toml_result_t *)wc_alloc(run, sizeof *run->docs * (w->scene_count + 1u));
    if (run->docs == NULL)
        return wc_fatal(run, "out of memory", NULL);
    memset(run->docs, 0, sizeof *run->docs * (w->scene_count + 1u));
    run->doc_cap = w->scene_count;
    run->doc_count = w->scene_count;
    for (u32 i = 0; i < w->scene_count; ++i) {
        const de__manifest_asset *as = de__manifest_find_asset(m, w->scenes[i]);
        if (as == NULL)
            continue; /* resolve already reported it */
        if (!wc_read_scene(run, m, as, i))
            return wc_fatal(run, "out of memory", NULL);
    }
    wc_place(run);
    if (run->errors > 0)
        return DE_COOK_EXIT_ERRORS;

    /* Output directory. */
    if (run->out_arg != NULL) {
        if (de_fs_mount(WC_OUT_ROOT, run->out_arg, DE_MOUNT_WRITABLE, 0) != DE_OK)
            return wc_fatal(run, "cannot open the output directory", run->out_arg);
        (void)snprintf(run->out_native, sizeof run->out_native, "%s", run->out_arg);
        run->out_mounted = true;
    } else {
        if (de_fs_mount(WC_OUT_ROOT, run->dir, DE_MOUNT_WRITABLE, 0) != DE_OK)
            return wc_fatal(run, "cannot open the manifest directory", run->dir);
        (void)snprintf(run->out_native, sizeof run->out_native, "%s", run->dir);
        run->out_mounted = true;
    }
    char sub[DE_FS_PATH_MAX] = "";
    if (run->out_arg == NULL) {
        const char *ar = m->asset_root != NULL ? m->asset_root : "";
        (void)snprintf(sub, sizeof sub, "%s%s%s/", ar, ar[0] ? "/" : "", cells_dir);
        char vdir[DE_FS_PATH_MAX];
        (void)snprintf(vdir, sizeof vdir, "%s%s%s%s", WC_OUT_PREFIX, ar, ar[0] ? "/" : "",
                       cells_dir);
        de_result dr = de_dir_create(vdir);
        if (dr != DE_OK) {
            wc_put(run, DE_COOK_STDERR, de_result_str(dr));
            wc_put(run, DE_COOK_STDERR, ": ");
            return wc_fatal(run, "cannot create the cells directory", vdir);
        }
    }

    de_strbuf idx, cell;
    de_strbuf_init(&idx, run->a);
    de_strbuf_init(&cell, run->a);
    bool ok = de_strbuf_appendf(&idx,
                                "# Generated by `de-cook world` (DENG-271) for world \"%s\". "
                                "Do not edit;\n# cook the world again instead.\n\n[assets]\n",
                                w->name) == DE_OK;
    de_strbuf bundles;
    de_strbuf_init(&bundles, run->a);
    /* Cells in (cx, cy) order: collect the placed entities' cells, sort, walk. */
    de__cell_key *cells = (de__cell_key *)wc_alloc(run, sizeof *cells * (run->ent_count + 1u));
    u32 cell_count = 0;
    ok = ok && cells != NULL;
    for (u32 i = 0; ok && i < run->ent_count; ++i) {
        if (!run->ents[i].placed || run->ents[i].parent_id != 0)
            continue;
        cells[cell_count].dist2 = 0.0;
        cells[cell_count].cx = run->ents[i].cx;
        cells[cell_count].cy = run->ents[i].cy;
        cell_count++;
    }
    if (ok)
        de__cell_keys_sort(cells, cell_count);
    u32 written = 0;
    for (u32 c = 0; ok && c < cell_count; ++c) {
        if (c > 0 && cells[c].cx == cells[c - 1].cx && cells[c].cy == cells[c - 1].cy)
            continue;
        i32 cx = cells[c].cx, cy = cells[c].cy;
        char leaf[64], id[160], file[DE_FS_PATH_MAX];
        de__cell_leaf(leaf, sizeof leaf, cx, cy);
        de__cell_name(id, sizeof id, w->name, cx, cy);
        de_strbuf_clear(&cell);
        ok = de_strbuf_appendf(&cell, "# %s: cell (%d, %d) of world \"%s\" (de-cook world)\n", id,
                               (int)cx, (int)cy, w->name) == DE_OK;
        for (u32 i = 0; ok && i < run->ent_count; ++i) {
            const wc_ent *e = &run->ents[i];
            if (e->placed && e->cx == cx && e->cy == cy)
                ok = wc_entity_out(run, &cell, e);
        }
        (void)snprintf(file, sizeof file, "%s%s.scene.toml", sub, leaf);
        if (ok && !wc_write(file, &cell)) {
            wc_msg(run, "error", NULL, 0, "cannot write %s", file);
            ok = false;
        }
        ok = ok &&
             wc_str(&idx, id, strlen(id)) &&
             de_strbuf_appendf(&idx, " = { type = \"scene\", source = \"%s/%s.scene.toml\" }\n",
                               cells_dir, leaf) == DE_OK;
        ok = ok && de_strbuf_append_cstr(&bundles, "\n[bundles.") == DE_OK &&
             wc_str(&bundles, id, strlen(id)) &&
             de_strbuf_append_cstr(&bundles, "]\nassets = [") == DE_OK &&
             wc_str(&bundles, id, strlen(id)) && de_strbuf_append_cstr(&bundles, "]\n") == DE_OK;
        written++;
    }
    ok = ok && de_strbuf_append(&idx, de_strbuf_view(&bundles)) == DE_OK;
    if (ok) {
        char file[DE_FS_PATH_MAX];
        (void)snprintf(file, sizeof file, "%scells.toml", sub);
        if (!wc_write(file, &idx)) {
            wc_msg(run, "error", NULL, 0, "cannot write %s", file);
            ok = false;
        }
    }
    if (cells != NULL)
        DE_FREE(run->a, cells, sizeof *cells * (run->ent_count + 1u));
    de_strbuf_free(&bundles);
    de_strbuf_free(&cell);
    de_strbuf_free(&idx);
    if (!ok && run->errors == 0)
        return wc_fatal(run, "out of memory", NULL);
    if (run->errors > 0)
        return DE_COOK_EXIT_ERRORS;
    char line[256];
    (void)snprintf(line, sizeof line,
                   "de-cook world: \"%s\": %u entities in %u cells, %u warnings\n", w->name,
                   (unsigned)run->ent_count, (unsigned)written, (unsigned)run->warnings);
    wc_put(run, DE_COOK_STDOUT, line);
    return DE_COOK_EXIT_OK;
}

/* The issues that stop a world cook: every error, except an include that cannot be read (the
 * generated cells.toml before the first cook). */
static void wc_report(wc_run *run, const de__manifest_issue *is, u32 n) {
    for (u32 i = 0; i < n; ++i) {
        if (is[i].severity != DE_ASSET_ISSUE_ERROR)
            continue;
        if (is[i].code == DE_ERR_NOT_FOUND && is[i].key_path != NULL && is[i].key_path[0] != '\0')
            continue;
        wc_msg(run, "error", is[i].file, (int)is[i].line, "%s%s%s%s", is[i].message,
               is[i].key_path != NULL && is[i].key_path[0] ? " [" : "",
               is[i].key_path != NULL ? is[i].key_path : "",
               is[i].key_path != NULL && is[i].key_path[0] ? "]" : "");
    }
}

int de_cook_world_main(int argc, const char *const *argv, const de_cook_io *io) {
    wc_run run;
    memset(&run, 0, sizeof run);
    run.io = io;
    run.a = de_heap_allocator();
    const char *manifest = NULL;
    int rc = wc_args(&run, argc, argv, &manifest);
    if (rc >= 0)
        return rc;
    rc = wc_split(&run, manifest);
    if (rc >= 0)
        return rc;
    if (de_fs_mount(WC_IN_ROOT, run.dir, DE_MOUNT_READ_ONLY, 0) != DE_OK)
        return wc_fatal(&run, "cannot open the manifest directory", run.dir);
    run.in_mounted = true;

    de__manifest *m = NULL;
    de__resolved *res = NULL;
    de__manifest_parse_desc pd = {.path = run.root, .allocator = run.a};
    if (de__manifest_parse(&pd, &m) != DE_OK) {
        rc = wc_fatal(&run, "cannot parse the manifest", manifest);
        goto done;
    }
    if (de__manifest_resolve(m, &(de__resolve_desc){.allocator = run.a}, &res) != DE_OK) {
        rc = wc_fatal(&run, "out of memory", NULL);
        goto done;
    }
    wc_report(&run, res->issues, res->issue_count);
    if (run.errors > 0) {
        rc = DE_COOK_EXIT_ERRORS;
        goto done;
    }
    const de__manifest_world *w = de__manifest_find_world(m, run.world_name);
    if (w == NULL) {
        rc = wc_fatal(&run, "the manifest declares no world", run.world_name);
        goto done;
    }
    rc = wc_cook(&run, m, w);

done:
    for (u32 i = 0; i < run.doc_count; ++i)
        if (run.docs[i].__internal != NULL)
            de__toml_free(run.docs[i]);
    if (run.docs != NULL)
        DE_FREE(run.a, run.docs, sizeof *run.docs * (run.doc_cap + 1u));
    if (run.ents != NULL)
        DE_FREE(run.a, run.ents, sizeof *run.ents * run.ent_cap);
    de__resolved_destroy(res);
    de__manifest_destroy(m);
    if (run.out_mounted)
        (void)de_fs_unmount(WC_OUT_ROOT, run.out_native);
    if (run.in_mounted)
        (void)de_fs_unmount(WC_IN_ROOT, run.dir);
    return rc;
}
