/*
 * world_cells.c: `de-cook world` (DENG-271), the world cook. See world_cells.h.
 *
 * The manifest directory is mounted read-only as `wcook:/`, the output directory writable as
 * `wcout:/`; both are unmounted before the command returns. Scene sources are parsed with the
 * core TOML parser; the cell files are written from the parsed values, keys sorted, floats in
 * their shortest round-trip form (de__ser_format_f64), so the bytes depend only on the input.
 * A root with DeWorldCellPin { cx, cy } goes, with its children, to that cell instead of the one
 * that holds its DeTransform position (DENG-831); it needs no transform then.
 *
 * Far proxies (DENG-830): for each cell, wc_proxy_item lists every placed entity's
 * DeMeshRenderer mesh (with its world transform made relative to the cell origin and its
 * material overrides' base colours), and world_proxy.c bakes the list into one glTF.
 *
 * Terrain tiles (DENG-857): wc_terrain_tiles writes each placed DeTerrain's 3 x 3 tile blobs
 * and lists the cells each tile overlaps; those cells join the cell list and their bundles depend
 * on the tile bundles. The terrain entity itself stays in its cell unchanged.
 *
 * Known gap: an entity reference (a component field that holds another entity's scene ID) that
 * crosses a cell boundary is written unchanged; the cell's scene import then reports it as a
 * reference outside the file. Keep cross-referencing entities under one root.
 */
#include "world_cells.h"
#include "world_proxy.h"

#include "asset/manifest_internal.h"
#include "asset/resolve_internal.h"
#include "core/de_strbuf.h"
#include "core/de_toml_internal.h"
#include "reflect/ser_internal.h"
#include "world/streaming/streaming_internal.h"

#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include <math.h>
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
    /* far proxies: material base colours read so far (DENG-830) */
    struct wc_matc *mats;
    u32 mat_count, mat_cap;
    /* terrain tiles (DENG-857), sorted by terrain, ty, tx */
    struct wc_tile *tiles;
    u32 tile_count, tile_cap;
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

/* DeWorldCellPin { cx, cy } (DENG-831): 1 with the pinned cell, 0 without a pin, -1 when the
 * pin is malformed (not integers, or outside the cell grid). */
static int wc_pin(const toml_datum_t *pin, i32 *cx, i32 *cy) {
    if (pin == NULL)
        return 0;
    f64 v[2];
    if (pin->type != TOML_TABLE || !wc_num(wc_get(pin, "cx"), &v[0]) ||
        !wc_num(wc_get(pin, "cy"), &v[1]))
        return -1;
    for (int i = 0; i < 2; ++i)
        if (v[i] != floor(v[i]) || v[i] < -(f64)DE__CELL_COORD_MAX ||
            v[i] > (f64)DE__CELL_COORD_MAX)
            return -1;
    *cx = (i32)v[0];
    *cy = (i32)v[1];
    return 1;
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
            i32 pcx, pcy;
            const int pin = wc_pin(wc_get(e->tab, "DeWorldCellPin"), &pcx, &pcy);
            if (pin > 0) {
                e->cx = pcx;
                e->cy = pcy;
                e->placed = true;
                continue;
            }
            if (pin < 0)
                wc_msg(run, "error", e->source, e->line,
                       "root 0x%016llx: DeWorldCellPin needs integer cx and cy within +/-%d",
                       (unsigned long long)e->id, (int)DE__CELL_COORD_MAX);
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

/* ---- Far proxies (DENG-830) ---------------------------------------------------------------- */

/* A 3-vector or quaternion as an array or a table of `names`; `n` values. */
static bool wc_vec(const toml_datum_t *v, const char *const *names, int n, f64 *out) {
    if (v == NULL)
        return false;
    if (v->type == TOML_ARRAY) {
        if (v->u.arr.size != n)
            return false;
        for (int i = 0; i < n; ++i)
            if (!wc_num(&v->u.arr.elem[i], &out[i]))
                return false;
        return true;
    }
    if (v->type == TOML_TABLE) {
        for (int i = 0; i < n; ++i)
            if (!wc_num(wc_get(v, names[i]), &out[i]))
                return false;
        return true;
    }
    return false;
}

/* The local DeTransform of `tab` as a row-major 3x4 matrix (identity parts where missing). */
static void wc_local(const toml_datum_t *tab, f64 m[12]) {
    static const char *const k_xyz[3] = {"x", "y", "z"};
    static const char *const k_xyzw[4] = {"x", "y", "z", "w"};
    f64 p[3] = {0, 0, 0}, q[4] = {0, 0, 0, 1}, sc[3] = {1, 1, 1};
    const toml_datum_t *tf = wc_get(tab, "DeTransform");
    if (tf != NULL && tf->type == TOML_TABLE) {
        (void)wc_position(tf, p);
        if (!wc_vec(wc_get(tf, "rotation"), k_xyzw, 4, q)) {
            q[0] = q[1] = q[2] = 0.0;
            q[3] = 1.0;
        }
        const toml_datum_t *s = wc_get(tf, "scale");
        f64 u;
        if (wc_num(s, &u))
            sc[0] = sc[1] = sc[2] = u;
        else if (!wc_vec(s, k_xyz, 3, sc))
            sc[0] = sc[1] = sc[2] = 1.0;
    }
    f64 len = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(len > 0.0)) {
        q[0] = q[1] = q[2] = 0.0;
        q[3] = len = 1.0;
    }
    const f64 x = q[0] / len, y = q[1] / len, z = q[2] / len, w = q[3] / len;
    const f64 r[9] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w),
                      2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                      2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y)};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j)
            m[i * 4 + j] = r[i * 3 + j] * sc[j];
        m[i * 4 + 3] = p[i];
    }
}

/* a = a * b (3x4 affine). */
static void wc_mul(f64 a[12], const f64 b[12]) {
    f64 o[12];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j)
            o[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] +
                           a[i * 4 + 2] * b[2 * 4 + j];
        o[i * 4 + 3] += a[i * 4 + 3];
    }
    memcpy(a, o, sizeof o);
}

/* The world transform of `e`: its local DeTransform times its parents'. */
static void wc_world(const wc_run *run, const wc_ent *e, f64 m[12]) {
    wc_local(e->tab, m);
    u32 cur = (u32)(e - run->ents), steps = 0;
    while (run->ents[cur].parent_id != 0 && steps++ <= run->ent_count) {
        u32 p = wc_find(run, run->ents[cur].parent_id);
        if (p == UINT32_MAX)
            break;
        f64 pm[12];
        wc_local(run->ents[p].tab, pm);
        wc_mul(pm, m);
        memcpy(m, pm, sizeof pm);
        cur = p;
    }
}

typedef struct wc_matc {
    u64 hash;
    bool ok;
    f32 rgb[3];
} wc_matc;

/* The base colour of material asset `id` (its source TOML's `base_color`); false when it is not
 * a material with one. Cached per cook. */
static bool wc_material_color(wc_run *run, const de__manifest *m, const char *id, f32 rgb[3]) {
    u64 h = 1469598103934665603ull;
    for (const char *c = id; *c; ++c)
        h = (h ^ (u8)*c) * 1099511628211ull;
    for (u32 i = 0; i < run->mat_count; ++i)
        if (run->mats[i].hash == h) {
            memcpy(rgb, run->mats[i].rgb, sizeof(f32) * 3u);
            return run->mats[i].ok;
        }
    wc_matc e = {h, false, {1.0f, 1.0f, 1.0f}};
    const de__manifest_asset *as = de__manifest_find_asset(m, id);
    char raw[DE_FS_PATH_MAX], vpath[DE_FS_PATH_MAX];
    const char *ar = m->asset_root != NULL ? m->asset_root : "";
    void *data = NULL;
    u64 size = 0;
    if (as != NULL && strcmp(as->type, "material") == 0 && as->source != NULL &&
        snprintf(raw, sizeof raw, "%s%s%s%s", WC_IN_PREFIX, ar, ar[0] ? "/" : "", as->source) <
            (int)sizeof raw &&
        de_path_normalize(raw, vpath, sizeof vpath) == DE_OK &&
        de_file_read_all(vpath, run->a, &data, &size) == DE_OK) {
        if (size <= (u64)INT32_MAX) {
            toml_result_t tr = de__toml_parse(run->a, (const char *)data, (int)size);
            if (tr.ok) {
                const toml_datum_t *bc = wc_get(&tr.toptab, "base_color");
                if (bc != NULL && bc->type == TOML_ARRAY &&
                    (bc->u.arr.size == 3 || bc->u.arr.size == 4)) {
                    e.ok = true;
                    for (int c = 0; c < 3; ++c) {
                        f64 v;
                        if (wc_num(&bc->u.arr.elem[c], &v))
                            e.rgb[c] = (f32)v;
                        else
                            e.ok = false;
                    }
                }
            }
            if (tr.__internal != NULL)
                de__toml_free(tr);
        }
        DE_FREE(run->a, data, size + 1);
    }
    if (run->mat_count == run->mat_cap) {
        u32 cap = run->mat_cap ? run->mat_cap * 2u : 16u;
        wc_matc *nm = (wc_matc *)wc_alloc(run, sizeof *nm * cap);
        if (nm != NULL) {
            if (run->mat_count > 0) {
                memcpy(nm, run->mats, sizeof *nm * run->mat_count);
                DE_FREE(run->a, run->mats, sizeof *nm * run->mat_cap);
            }
            run->mats = nm;
            run->mat_cap = cap;
        }
    }
    if (run->mat_count < run->mat_cap)
        run->mats[run->mat_count++] = e;
    memcpy(rgb, e.rgb, sizeof(f32) * 3u);
    return e.ok;
}

typedef struct wc_pitem {
    de__wproxy_item it;
    char vpath[DE_FS_PATH_MAX];
    char mesh[160];
} wc_pitem;

static void wc_proxy_warn(void *user, const de__wproxy_item *it, const char *msg) {
    wc_msg((wc_run *)user, "warning", it->file, it->line, "%s", msg);
}

/* The proxy item of entity `e` in its cell's frame; false when it draws no model mesh (a
 * reason is warned when it names one the bake cannot use). */
static bool wc_proxy_item(wc_run *run, const de__manifest *m, const wc_ent *e, wc_pitem *pi) {
    const toml_datum_t *mr = wc_get(e->tab, "DeMeshRenderer");
    const toml_datum_t *mesh = mr != NULL && mr->type == TOML_TABLE ? wc_get(mr, "mesh") : NULL;
    if (mesh == NULL || mesh->type != TOML_STRING || mesh->u.str.ptr[0] == '\0')
        return false;
    const char *ref = mesh->u.str.ptr;
    const char *hash = strchr(ref, '#');
    char model[160];
    size_t mlen = hash != NULL ? (size_t)(hash - ref) : strlen(ref);
    if (mlen >= sizeof model || (hash != NULL && strlen(hash + 1) >= sizeof pi->mesh)) {
        wc_msg(run, "warning", e->source, e->line, "far proxy: mesh `%.64s...` is too long", ref);
        return false;
    }
    memcpy(model, ref, mlen);
    model[mlen] = '\0';
    const de__manifest_asset *as = de__manifest_find_asset(m, model);
    if (as == NULL || strcmp(as->type, "model") != 0 || as->source == NULL) {
        wc_msg(run, "warning", e->source, e->line,
               "far proxy: mesh `%s` is not a sub-asset of a model asset; left out", ref);
        return false;
    }
    char raw[DE_FS_PATH_MAX];
    const char *ar = m->asset_root != NULL ? m->asset_root : "";
    int n = snprintf(raw, sizeof raw, "%s%s%s%s", WC_IN_PREFIX, ar, ar[0] ? "/" : "", as->source);
    if (n < 0 || (size_t)n >= sizeof raw ||
        de_path_normalize(raw, pi->vpath, sizeof pi->vpath) != DE_OK) {
        wc_msg(run, "warning", e->source, e->line, "far proxy: bad source of `%s`", model);
        return false;
    }
    (void)snprintf(pi->mesh, sizeof pi->mesh, "%s", hash != NULL ? hash + 1 : "");
    pi->it.model_vpath = pi->vpath;
    pi->it.mesh = pi->mesh;
    pi->it.scale = 1.0;
    const de__manifest_kv *kv = de__manifest_find_setting(as->settings, as->setting_count, "scale");
    if (kv != NULL && kv->value.type == DE__MANIFEST_FLOAT)
        pi->it.scale = kv->value.u.f;
    else if (kv != NULL && kv->value.type == DE__MANIFEST_INT)
        pi->it.scale = (f64)kv->value.u.i;
    pi->it.file = e->source;
    pi->it.line = e->line;
    pi->it.color_mask = 0;
    const toml_datum_t *mats = wc_get(mr, "materials");
    if (mats != NULL && mats->type == TOML_ARRAY)
        for (int32_t k = 0; k < mats->u.arr.size && k < 8; ++k) {
            const toml_datum_t *mv = &mats->u.arr.elem[k];
            if (mv->type == TOML_STRING && mv->u.str.ptr[0] != '\0' &&
                wc_material_color(run, m, mv->u.str.ptr, pi->it.colors[k]))
                pi->it.color_mask |= 1u << k;
        }
    /* Up the parent chain; the root's translation is relative to the cell origin. */
    wc_world(run, e, pi->it.m);
    pi->it.m[3] -= (f64)e->cx * DE__CELL_SIZE;
    pi->it.m[7] -= (f64)e->cy * DE__CELL_SIZE;
    return true;
}

/* Bakes cell (cx, cy)'s proxy into `<sub><leaf>.proxy.gltf` and lists it; no mesh, no proxy. */
static bool wc_proxy_cell(wc_run *run, const de__manifest *m, const de__manifest_world *w,
                          de__wproxy *bk, wc_pitem *items, i32 cx, i32 cy, const char *sub,
                          const char *cells_dir, const char *leaf, de_strbuf *idx,
                          de_strbuf *bundles, de_strbuf *gltf) {
    u32 n = 0;
    for (u32 i = 0; i < run->ent_count; ++i) {
        const wc_ent *e = &run->ents[i];
        if (e->placed && e->cx == cx && e->cy == cy && wc_proxy_item(run, m, e, &items[n]))
            n++;
    }
    if (n == 0)
        return true;
    de__wproxy_item *list = (de__wproxy_item *)wc_alloc(run, sizeof *list * n);
    if (list == NULL)
        return false;
    for (u32 i = 0; i < n; ++i)
        list[i] = items[i].it;
    u32 tris = 0;
    de_result r = de__wproxy_bake(bk, list, n, w->proxy_triangle_ratio, gltf, &tris);
    DE_FREE(run->a, list, sizeof *list * n);
    if (r != DE_OK)
        return false;
    if (tris == 0)
        return true;
    char pleaf[96], pid[192], file[DE_FS_PATH_MAX];
    (void)snprintf(pleaf, sizeof pleaf, "%s.proxy.gltf", leaf);
    de__cell_proxy_name(pid, sizeof pid, w->name, cx, cy);
    (void)snprintf(file, sizeof file, "%s%s", sub, pleaf);
    if (!wc_write(file, gltf)) {
        wc_msg(run, "error", NULL, 0, "cannot write %s", file);
        return false;
    }
    return wc_str(idx, pid, strlen(pid)) &&
           de_strbuf_appendf(idx,
                             " = { type = \"model\", source = \"%s/%s\", generate_lods = false }\n",
                             cells_dir, pleaf) == DE_OK &&
           de_strbuf_append_cstr(bundles, "\n[bundles.") == DE_OK &&
           wc_str(bundles, pid, strlen(pid)) &&
           de_strbuf_append_cstr(bundles, "]\nassets = [") == DE_OK &&
           wc_str(bundles, pid, strlen(pid)) && de_strbuf_append_cstr(bundles, "]\n") == DE_OK;
}

/* ---- Terrain tiles (DENG-857) -------------------------------------------------------------- */

/* Tiles a terrain may overlap per axis before the cook refuses it (a 2 km tile at most). */
#define WC_TILE_CELLS_MAX 9

typedef struct wc_tile {
    u64 terrain;
    u32 tx, ty;
    i32 cx0, cy0, cx1, cy1; /* the cells it overlaps, inclusive */
} wc_tile;

typedef struct wc_terrain {
    f64 size, height;
    u16 heights[DE__TTILE_RES * DE__TTILE_RES];
    u8 holes[DE__TTILE_RES * DE__TTILE_RES];
    u8 splat[DE__TTILE_RES * DE__TTILE_RES * 4u];
} wc_terrain;

/* DeTerrain.<key>, an array of `n` integers in [0, max], into out16 or out8; absent keeps the
 * default. False (with an error) when it is malformed. */
static bool wc_terrain_ints(wc_run *run, const wc_ent *e, const toml_datum_t *dt, const char *key,
                            u32 n, u32 max, u16 *out16, u8 *out8) {
    const toml_datum_t *v = wc_get(dt, key);
    if (v == NULL)
        return true;
    if (v->type != TOML_ARRAY || v->u.arr.size != (int32_t)n) {
        wc_msg(run, "error", e->source, v->lineno, "DeTerrain.%s must be an array of %u integers",
               key, (unsigned)n);
        return false;
    }
    for (u32 i = 0; i < n; ++i) {
        const toml_datum_t *x = &v->u.arr.elem[i];
        if (x->type != TOML_INT64 || x->u.int64 < 0 || x->u.int64 > (i64)max) {
            wc_msg(run, "error", e->source, v->lineno,
                   "DeTerrain.%s[%u] must be an integer from 0 to %u", key, (unsigned)i,
                   (unsigned)max);
            return false;
        }
        if (out16 != NULL)
            out16[i] = (u16)x->u.int64;
        else
            out8[i] = (u8)x->u.int64;
    }
    return true;
}

/* Reads e's DeTerrain into t, defaults as construct_terrain (render/components.c). */
static bool wc_terrain_read(wc_run *run, const wc_ent *e, const toml_datum_t *dt, wc_terrain *t) {
    memset(t, 0, sizeof *t);
    t->size = 32.0;
    t->height = 32.0;
    for (u32 i = 0; i < DE__TTILE_RES * DE__TTILE_RES; ++i)
        t->splat[i * 4u] = 255;
    f64 v;
    const toml_datum_t *res = wc_get(dt, "resolution");
    if (res != NULL && (!wc_num(res, &v) || v != (f64)DE__TTILE_RES)) {
        wc_msg(run, "error", e->source, res->lineno, "DeTerrain.resolution must be %u",
               (unsigned)DE__TTILE_RES);
        return false;
    }
    if (wc_get(dt, "size") != NULL) {
        if (!wc_num(wc_get(dt, "size"), &t->size) || !(t->size > 0.0) || t->size > 1.0e6) {
            wc_msg(run, "error", e->source, e->line,
                   "DeTerrain.size of 0x%016llx must be a number above 0",
                   (unsigned long long)e->id);
            return false;
        }
    }
    if (wc_get(dt, "height") != NULL && !wc_num(wc_get(dt, "height"), &t->height)) {
        wc_msg(run, "error", e->source, e->line, "DeTerrain.height must be a number");
        return false;
    }
    const u32 n = DE__TTILE_RES * DE__TTILE_RES;
    return wc_terrain_ints(run, e, dt, "heights", n, 65535u, t->heights, NULL) &&
           wc_terrain_ints(run, e, dt, "holes", n, 255u, NULL, t->holes) &&
           wc_terrain_ints(run, e, dt, "splat", n * 4u, 255u, NULL, t->splat);
}

static void wc_le32(u8 *p, u32 v) {
    for (int i = 0; i < 4; ++i)
        p[i] = (u8)(v >> (8 * i));
}

static void wc_le64(u8 *p, u64 v) {
    wc_le32(p, (u32)v);
    wc_le32(p + 4, (u32)(v >> 32));
}

static void wc_lef64(u8 *p, f64 d) {
    u64 v;
    memcpy(&v, &d, sizeof v);
    wc_le64(p, v);
}

/* Writes tile (tx, ty) of terrain `e` as `<sub>terrain_<id>_<tx>_<ty>.tile`. */
static bool wc_tile_write(wc_run *run, const wc_ent *e, const wc_terrain *t, const f64 o[3],
                          u32 tx, u32 ty, const char *sub) {
    u32 x0, w, y0, h;
    de__ttile_span(tx, &x0, &w);
    de__ttile_span(ty, &y0, &h);
    const u64 bytes = de__ttile_bytes(w, h);
    u8 *b = (u8 *)wc_alloc(run, (size_t)bytes);
    if (b == NULL)
        return false;
    memset(b, 0, (size_t)bytes);
    memcpy(b, DE__TTILE_MAGIC, 4);
    wc_le32(b + 4, DE__TTILE_VERSION);
    wc_le64(b + 8, e->id);
    const u32 hdr[6] = {tx, ty, x0, y0, w, h};
    for (int i = 0; i < 6; ++i)
        wc_le32(b + 16 + 4 * i, hdr[i]);
    for (int i = 0; i < 3; ++i)
        wc_lef64(b + 40 + 8 * i, o[i]);
    wc_lef64(b + 64, t->size / (f64)(DE__TTILE_RES - 1u));
    wc_lef64(b + 72, t->height);
    u8 *hp = b + DE__TTILE_HEADER, *ho = hp + 2u * w * h, *sp = ho + w * h;
    for (u32 j = 0; j < h; ++j)
        for (u32 i = 0; i < w; ++i) {
            const u32 src = (y0 + j) * DE__TTILE_RES + x0 + i, dst = j * w + i;
            hp[2u * dst] = (u8)t->heights[src];
            hp[2u * dst + 1u] = (u8)(t->heights[src] >> 8);
            ho[dst] = t->holes[src];
            memcpy(sp + 4u * dst, t->splat + 4u * src, 4u);
        }
    char leaf[64], file[DE_FS_PATH_MAX], vpath[DE_FS_PATH_MAX];
    bool ok = de__ttile_leaf(leaf, sizeof leaf, e->id, tx, ty) &&
              snprintf(file, sizeof file, "%s%s.tile", sub, leaf) < (int)sizeof file &&
              snprintf(vpath, sizeof vpath, "%s%s", WC_OUT_PREFIX, file) < (int)sizeof vpath &&
              de_file_write_all_atomic(vpath, b, bytes) == DE_OK;
    DE_FREE(run->a, b, (size_t)bytes);
    if (!ok)
        wc_msg(run, "error", NULL, 0, "cannot write the tile %s", leaf);
    return ok;
}

static bool wc_push_tile(wc_run *run, const wc_tile *t) {
    if (run->tile_count == run->tile_cap) {
        u32 cap = run->tile_cap ? run->tile_cap * 2u : 16u;
        wc_tile *n = (wc_tile *)wc_alloc(run, sizeof *n * cap);
        if (n == NULL)
            return false;
        if (run->tiles != NULL) {
            memcpy(n, run->tiles, sizeof *n * run->tile_count);
            DE_FREE(run->a, run->tiles, sizeof *n * run->tile_cap);
        }
        run->tiles = n;
        run->tile_cap = cap;
    }
    run->tiles[run->tile_count++] = *t;
    return true;
}

/* Each placed entity with a DeTerrain: its 3 x 3 tiles, written and listed in run->tiles (in
 * entity ID order, then ty, tx). The grid's columns run along world +x and its rows along +y
 * from the entity's world position; its rotation and scale are not applied. Returns false when
 * out of memory or a write failed; a malformed terrain is an error and is left out. */
static bool wc_terrain_tiles(wc_run *run, const char *sub) {
    wc_terrain *t = (wc_terrain *)wc_alloc(run, sizeof *t);
    if (t == NULL)
        return false;
    bool ok = true;
    for (u32 i = 0; ok && i < run->ent_count; ++i) {
        const wc_ent *e = &run->ents[i];
        const toml_datum_t *dt = wc_get(e->tab, "DeTerrain");
        if (!e->placed || dt == NULL)
            continue;
        if (dt->type != TOML_TABLE) {
            wc_msg(run, "error", e->source, dt->lineno, "DeTerrain must be a table");
            continue;
        }
        if (!wc_terrain_read(run, e, dt, t))
            continue;
        f64 m[12];
        wc_world(run, e, m);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                if (fabs(m[r * 4 + c] - (r == c ? 1.0 : 0.0)) > 1e-6) {
                    wc_msg(run, "warning", e->source, e->line,
                           "terrain 0x%016llx: its rotation and scale are not applied to its "
                           "streaming tiles",
                           (unsigned long long)e->id);
                    r = c = 3;
                }
        const f64 o[3] = {m[3], m[7], m[11]};
        const f64 sp = t->size / (f64)(DE__TTILE_RES - 1u);
        for (u32 ty = 0; ok && ty < DE__TTILE_COUNT; ++ty)
            for (u32 tx = 0; ok && tx < DE__TTILE_COUNT; ++tx) {
                u32 x0, w, y0, h;
                de__ttile_span(tx, &x0, &w);
                de__ttile_span(ty, &y0, &h);
                wc_tile tl = {e->id, tx, ty, 0, 0, 0, 0};
                tl.cx0 = de__cell_coord(o[0] + (f64)x0 * sp);
                tl.cx1 = de__cell_coord(o[0] + (f64)(x0 + w - 1u) * sp);
                tl.cy0 = de__cell_coord(o[1] + (f64)y0 * sp);
                tl.cy1 = de__cell_coord(o[1] + (f64)(y0 + h - 1u) * sp);
                if (tl.cx1 - tl.cx0 >= WC_TILE_CELLS_MAX || tl.cy1 - tl.cy0 >= WC_TILE_CELLS_MAX) {
                    wc_msg(run, "error", e->source, e->line,
                           "terrain 0x%016llx: a tile spans more than %d cells on a side",
                           (unsigned long long)e->id, WC_TILE_CELLS_MAX);
                    tx = ty = DE__TTILE_COUNT;
                    break;
                }
                ok = wc_tile_write(run, e, t, o, tx, ty, sub) && wc_push_tile(run, &tl);
            }
    }
    DE_FREE(run->a, t, sizeof *t);
    return ok;
}

static bool wc_tile_in(const wc_tile *t, i32 cx, i32 cy) {
    return cx >= t->cx0 && cx <= t->cx1 && cy >= t->cy0 && cy <= t->cy1;
}

/* `depends = [...]` of cell (cx, cy): the tiles that overlap it, in run->tiles order. */
static bool wc_cell_depends(const wc_run *run, const char *world, i32 cx, i32 cy,
                            de_strbuf *bundles) {
    bool any = false;
    for (u32 i = 0; i < run->tile_count; ++i) {
        const wc_tile *t = &run->tiles[i];
        if (!wc_tile_in(t, cx, cy))
            continue;
        char id[192];
        if (!de__ttile_name(id, sizeof id, world, t->terrain, t->tx, t->ty) ||
            de_strbuf_append_cstr(bundles, any ? ", " : "depends = [") != DE_OK ||
            !wc_str(bundles, id, strlen(id)))
            return false;
        any = true;
    }
    return !any || de_strbuf_append_cstr(bundles, "]\n") == DE_OK;
}

/* The tiles' blob assets and bundles. */
static bool wc_tiles_list(const wc_run *run, const char *world, const char *cells_dir,
                          de_strbuf *idx, de_strbuf *bundles) {
    for (u32 i = 0; i < run->tile_count; ++i) {
        const wc_tile *t = &run->tiles[i];
        char id[192], leaf[64];
        if (!de__ttile_name(id, sizeof id, world, t->terrain, t->tx, t->ty) ||
            !de__ttile_leaf(leaf, sizeof leaf, t->terrain, t->tx, t->ty) ||
            !wc_str(idx, id, strlen(id)) ||
            de_strbuf_appendf(idx, " = { type = \"blob\", source = \"%s/%s.tile\" }\n", cells_dir,
                              leaf) != DE_OK ||
            de_strbuf_append_cstr(bundles, "\n[bundles.") != DE_OK ||
            !wc_str(bundles, id, strlen(id)) ||
            de_strbuf_append_cstr(bundles, "]\nassets = [") != DE_OK ||
            !wc_str(bundles, id, strlen(id)) || de_strbuf_append_cstr(bundles, "]\n") != DE_OK)
            return false;
    }
    return true;
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
    /* Terrain tiles first (DENG-857): a cell that only a tile overlaps is a cell too. */
    ok = ok && wc_terrain_tiles(run, sub);
    u32 cell_cap = run->ent_count + 1u;
    for (u32 i = 0; i < run->tile_count; ++i)
        cell_cap += (u32)(run->tiles[i].cx1 - run->tiles[i].cx0 + 1) *
                    (u32)(run->tiles[i].cy1 - run->tiles[i].cy0 + 1);
    /* Cells in (cx, cy) order: collect the placed entities' and tiles' cells, sort, walk. */
    de__cell_key *cells = ok ? (de__cell_key *)wc_alloc(run, sizeof *cells * cell_cap) : NULL;
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
    for (u32 i = 0; ok && i < run->tile_count; ++i)
        for (i32 cy = run->tiles[i].cy0; cy <= run->tiles[i].cy1; ++cy)
            for (i32 cx = run->tiles[i].cx0; cx <= run->tiles[i].cx1; ++cx) {
                cells[cell_count].dist2 = 0.0;
                cells[cell_count].cx = cx;
                cells[cell_count].cy = cy;
                cell_count++;
            }
    if (ok)
        de__cell_keys_sort(cells, cell_count);
    u32 written = 0;
    de__wproxy *bk = NULL;
    wc_pitem *pitems = NULL;
    de_strbuf gltf;
    de_strbuf_init(&gltf, run->a);
    if (ok && w->proxy_enabled) {
        bk = de__wproxy_create(run->a, wc_proxy_warn, run);
        pitems = (wc_pitem *)wc_alloc(run, sizeof *pitems * (run->ent_count + 1u));
        ok = bk != NULL && pitems != NULL;
    }
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
             wc_str(&bundles, id, strlen(id)) && de_strbuf_append_cstr(&bundles, "]\n") == DE_OK &&
             wc_cell_depends(run, w->name, cx, cy, &bundles);
        if (ok && bk != NULL)
            ok = wc_proxy_cell(run, m, w, bk, pitems, cx, cy, sub, cells_dir, leaf, &idx,
                               &bundles, &gltf);
        written++;
    }
    ok = ok && wc_tiles_list(run, w->name, cells_dir, &idx, &bundles);
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
        DE_FREE(run->a, cells, sizeof *cells * cell_cap);
    if (run->tiles != NULL)
        DE_FREE(run->a, run->tiles, sizeof *run->tiles * run->tile_cap);
    run->tiles = NULL;
    const u32 tiles = run->tile_count;
    run->tile_count = run->tile_cap = 0;
    if (pitems != NULL)
        DE_FREE(run->a, pitems, sizeof *pitems * (run->ent_count + 1u));
    de__wproxy_destroy(bk);
    de_strbuf_free(&gltf);
    if (run->mats != NULL)
        DE_FREE(run->a, run->mats, sizeof *run->mats * run->mat_cap);
    run->mats = NULL;
    run->mat_count = run->mat_cap = 0;
    de_strbuf_free(&bundles);
    de_strbuf_free(&cell);
    de_strbuf_free(&idx);
    if (!ok && run->errors == 0)
        return wc_fatal(run, "out of memory", NULL);
    if (run->errors > 0)
        return DE_COOK_EXIT_ERRORS;
    char line[256];
    if (tiles > 0)
        (void)snprintf(line, sizeof line,
                       "de-cook world: \"%s\": %u entities in %u cells, %u terrain tiles, %u "
                       "warnings\n",
                       w->name, (unsigned)run->ent_count, (unsigned)written, (unsigned)tiles,
                       (unsigned)run->warnings);
    else
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
