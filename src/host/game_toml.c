/*
 * game_toml.c: the game.toml project file (SPEC §10, DENG-2 Q9) into de__game_config.
 *
 * Parsing goes through core's single tomlc17 entry point (core/de_toml_internal.h); never
 * toml_parse / toml_set_option here, the allocator hooks are process-wide.
 *
 * The file is checked completely before anything happens outside the config: every known key
 * (type, enum string, range), then every [cvars] value type. Only then are the cvars applied,
 * so a file that fails to parse sets no cvar.
 */
#include "host/engine_internal.h"

#include <dengine/de_debug.h>

#include "core/de_toml_internal.h"

#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Longest key path in messages and cvar names ("cvars." + a cvar name, with room to spare). */
#define GT_PATH_MAX 512
/* Deepest [cvars] nesting walked; a cvar name has at most DE_CVAR_NAME_MAX_PARTS parts. */
#define GT_CVAR_DEPTH_MAX 16

typedef struct gt_parser {
    const char *source;
    de__game_config *cfg;
    de_log_category log;
} gt_parser;

static const char *const k_window_modes[] = {"windowed", "borderless", "fullscreen"};
static const char *const k_window_surfaces[] = {"auto", "none", "metal", "vulkan"};
static const char *const k_timing_modes[] = {"fixed", "variable"};

void de__game_config_defaults(de__game_config *out) {
    if (out == NULL)
        return;
    memset(out, 0, sizeof *out);
    memcpy(out->name, DE__GAME_DEFAULT_NAME, sizeof DE__GAME_DEFAULT_NAME);
    out->window_width = DE__GAME_DEFAULT_WIDTH;
    out->window_height = DE__GAME_DEFAULT_HEIGHT;
    out->window_mode = DE_WINDOW_WINDOWED;
    out->window_high_dpi = true;
    out->window_surface_auto = true;
    out->timing.mode = DE_TIMING_FIXED;
    out->timing.fixed_hz = DE_TIMING_DEFAULT_FIXED_HZ;
    out->timing.max_catchup_steps = DE_TIMING_DEFAULT_MAX_CATCHUP_STEPS;
    out->timing.time_scale = DE_TIMING_DEFAULT_TIME_SCALE;
    out->timing.frame_cap_hz = DE_TIMING_DEFAULT_FRAME_CAP_HZ;
    out->task_workers = 0;
    out->frame_arena_mib = DE__GAME_DEFAULT_FRAME_ARENA_MIB;
    out->asset_io_threads = DE__GAME_DEFAULT_IO_THREADS; /* DENG-143 */
    out->asset_hot_reload = true;
}

/* ---- Messages ---------------------------------------------------------------------------- */

static const char *gt_type_name(toml_type_t t) {
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

/* DE_ERR_PARSE with "<source>:<line>: <path>: <message>". */
static de_result gt_fail(const gt_parser *p, const toml_datum_t *d, const char *path,
                         const char *fmt, ...) DE_PRINTF_FMT(4, 5);
static de_result gt_fail(const gt_parser *p, const toml_datum_t *d, const char *path,
                         const char *fmt, ...) {
    char msg[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof msg, fmt, args);
    va_end(args);
    if (d != NULL && d->lineno > 0)
        return DE_FAIL(DE_ERR_PARSE, "%s:%d: %s: %s", p->source, d->lineno, path, msg);
    return DE_FAIL(DE_ERR_PARSE, "%s: %s: %s", p->source, path, msg);
}

/* Writes "<prefix>.<key>" (or "<key>" for an empty prefix) into `buf`. False if it is cut. */
static bool gt_path(char *buf, size_t cap, const char *prefix, const char *key, int key_len) {
    int n = prefix[0] != '\0' ? snprintf(buf, cap, "%s.%.*s", prefix, key_len, key)
                              : snprintf(buf, cap, "%.*s", key_len, key);
    return n >= 0 && (size_t)n < cap;
}

static void gt_unknown(gt_parser *p, const toml_datum_t *d, const char *path) {
    p->cfg->unknown_keys++;
    if (d->lineno > 0)
        DE_WARN(p->log, "%s:%d: unknown key '%s' (ignored)", p->source, d->lineno, path);
    else
        DE_WARN(p->log, "%s: unknown key '%s' (ignored)", p->source, path);
}

static bool gt_key_is(const toml_datum_t *tab, int32_t i, const char *name) {
    size_t n = strlen(name);
    return (size_t)tab->u.tab.len[i] == n && memcmp(tab->u.tab.key[i], name, n) == 0;
}

/* ---- Typed values ------------------------------------------------------------------------ */

static de_result gt_string(const gt_parser *p, const toml_datum_t *v, const char *path, char *dst,
                           size_t cap) {
    if (v->type != TOML_STRING)
        return gt_fail(p, v, path, "expected a string, found %s", gt_type_name(v->type));
    size_t len = (size_t)v->u.str.len;
    if (len >= cap)
        return gt_fail(p, v, path, "longer than %zu bytes", cap - 1);
    memcpy(dst, v->u.str.ptr, len);
    dst[len] = '\0';
    return DE_OK;
}

static de_result gt_u32(const gt_parser *p, const toml_datum_t *v, const char *path, u32 min,
                        u32 max, u32 *dst) {
    if (v->type != TOML_INT64)
        return gt_fail(p, v, path, "expected an integer, found %s", gt_type_name(v->type));
    i64 x = v->u.int64;
    if (x < (i64)min || x > (i64)max)
        return gt_fail(p, v, path, "%" PRId64 " is out of range (%" PRIu32 " to %" PRIu32 ")", x,
                       min, max);
    *dst = (u32)x;
    return DE_OK;
}

static de_result gt_f32(const gt_parser *p, const toml_datum_t *v, const char *path, f64 min,
                        f64 max, f32 *dst) {
    f64 x;
    if (v->type == TOML_FP64)
        x = v->u.fp64;
    else if (v->type == TOML_INT64)
        x = (f64)v->u.int64;
    else
        return gt_fail(p, v, path, "expected a number, found %s", gt_type_name(v->type));
    /* `!(x >= min)` is also true for NaN. */
    if (!(x >= min) || !(x <= max))
        return gt_fail(p, v, path, "%g is out of range (%g to %g)", x, min, max);
    *dst = (f32)x;
    return DE_OK;
}

static de_result gt_bool(const gt_parser *p, const toml_datum_t *v, const char *path, bool *dst) {
    if (v->type != TOML_BOOLEAN)
        return gt_fail(p, v, path, "expected true or false, found %s", gt_type_name(v->type));
    *dst = v->u.boolean;
    return DE_OK;
}

static de_result gt_enum(const gt_parser *p, const toml_datum_t *v, const char *path,
                         const char *const *names, u32 count, u32 *dst) {
    if (v->type == TOML_STRING) {
        for (u32 i = 0; i < count; ++i) {
            if ((size_t)v->u.str.len == strlen(names[i]) &&
                memcmp(v->u.str.ptr, names[i], (size_t)v->u.str.len) == 0) {
                *dst = i;
                return DE_OK;
            }
        }
    }
    char expected[128] = "";
    size_t used = 0;
    for (u32 i = 0; i < count; ++i) {
        int n = snprintf(expected + used, sizeof expected - used, "%s\"%s\"",
                         i == 0 ? "" : (i + 1 == count ? " or " : ", "), names[i]);
        if (n < 0 || (size_t)n >= sizeof expected - used)
            break;
        used += (size_t)n;
    }
    if (v->type == TOML_STRING)
        return gt_fail(p, v, path, "expected %s, found \"%.*s\"", expected,
                       v->u.str.len > 64 ? 64 : v->u.str.len, v->u.str.ptr);
    return gt_fail(p, v, path, "expected %s, found %s", expected, gt_type_name(v->type));
}

/* ---- Sections ---------------------------------------------------------------------------- */

static de_result gt_game(gt_parser *p, const toml_datum_t *t) {
    de__game_config *c = p->cfg;
    for (int32_t i = 0; i < t->u.tab.size; ++i) {
        const toml_datum_t *v = &t->u.tab.value[i];
        char path[GT_PATH_MAX];
        if (!gt_path(path, sizeof path, "game", t->u.tab.key[i], t->u.tab.len[i]))
            return gt_fail(p, v, "game", "key path longer than %d bytes", GT_PATH_MAX - 1);
        de_result r = DE_OK;
        if (gt_key_is(t, i, "name"))
            r = gt_string(p, v, path, c->name, sizeof c->name);
        else if (gt_key_is(t, i, "library"))
            r = gt_string(p, v, path, c->library, sizeof c->library);
        else
            gt_unknown(p, v, path);
        if (r != DE_OK)
            return r;
    }
    return DE_OK;
}

static de_result gt_window(gt_parser *p, const toml_datum_t *t) {
    de__game_config *c = p->cfg;
    for (int32_t i = 0; i < t->u.tab.size; ++i) {
        const toml_datum_t *v = &t->u.tab.value[i];
        char path[GT_PATH_MAX];
        if (!gt_path(path, sizeof path, "window", t->u.tab.key[i], t->u.tab.len[i]))
            return gt_fail(p, v, "window", "key path longer than %d bytes", GT_PATH_MAX - 1);
        de_result r = DE_OK;
        if (gt_key_is(t, i, "title")) {
            r = gt_string(p, v, path, c->window_title, sizeof c->window_title);
        } else if (gt_key_is(t, i, "width")) {
            r = gt_u32(p, v, path, 1, DE__GAME_MAX_WINDOW_DIM, &c->window_width);
        } else if (gt_key_is(t, i, "height")) {
            r = gt_u32(p, v, path, 1, DE__GAME_MAX_WINDOW_DIM, &c->window_height);
        } else if (gt_key_is(t, i, "mode")) {
            u32 m = 0;
            r = gt_enum(p, v, path, k_window_modes, 3, &m);
            c->window_mode = m == 2 ? DE_WINDOW_FULLSCREEN
                                    : (m == 1 ? DE_WINDOW_BORDERLESS : DE_WINDOW_WINDOWED);
        } else if (gt_key_is(t, i, "high_dpi")) {
            r = gt_bool(p, v, path, &c->window_high_dpi);
        } else if (gt_key_is(t, i, "surface")) {
            u32 surface = 0;
            r = gt_enum(p, v, path, k_window_surfaces, 4, &surface);
            c->window_surface_auto = surface == 0;
            c->window_surface = surface == 2   ? DE_WINDOW_SURFACE_METAL
                                : surface == 3 ? DE_WINDOW_SURFACE_VULKAN
                                               : DE_WINDOW_SURFACE_NONE;
        } else {
            gt_unknown(p, v, path);
        }
        if (r != DE_OK)
            return r;
    }
    return DE_OK;
}

static de_result gt_timing(gt_parser *p, const toml_datum_t *t) {
    de_timing *tm = &p->cfg->timing;
    for (int32_t i = 0; i < t->u.tab.size; ++i) {
        const toml_datum_t *v = &t->u.tab.value[i];
        char path[GT_PATH_MAX];
        if (!gt_path(path, sizeof path, "timing", t->u.tab.key[i], t->u.tab.len[i]))
            return gt_fail(p, v, "timing", "key path longer than %d bytes", GT_PATH_MAX - 1);
        de_result r = DE_OK;
        if (gt_key_is(t, i, "mode")) {
            u32 m = 0;
            r = gt_enum(p, v, path, k_timing_modes, 2, &m);
            tm->mode = m == 1 ? DE_TIMING_VARIABLE : DE_TIMING_FIXED;
        } else if (gt_key_is(t, i, "fixed_hz")) {
            r = gt_u32(p, v, path, 1, DE__GAME_MAX_FIXED_HZ, &tm->fixed_hz);
        } else if (gt_key_is(t, i, "max_catchup_steps")) {
            r = gt_u32(p, v, path, 1, DE__GAME_MAX_CATCHUP_STEPS, &tm->max_catchup_steps);
        } else if (gt_key_is(t, i, "time_scale")) {
            r = gt_f32(p, v, path, 0.0, 1000.0, &tm->time_scale);
        } else if (gt_key_is(t, i, "frame_cap_hz")) {
            r = gt_u32(p, v, path, 0, DE__GAME_MAX_FRAME_CAP_HZ, &tm->frame_cap_hz);
        } else {
            gt_unknown(p, v, path);
        }
        if (r != DE_OK)
            return r;
    }
    return DE_OK;
}

static de_result gt_tasks(gt_parser *p, const toml_datum_t *t) {
    for (int32_t i = 0; i < t->u.tab.size; ++i) {
        const toml_datum_t *v = &t->u.tab.value[i];
        char path[GT_PATH_MAX];
        if (!gt_path(path, sizeof path, "tasks", t->u.tab.key[i], t->u.tab.len[i]))
            return gt_fail(p, v, "tasks", "key path longer than %d bytes", GT_PATH_MAX - 1);
        de_result r = DE_OK;
        if (gt_key_is(t, i, "workers"))
            r = gt_u32(p, v, path, 0, DE__GAME_MAX_WORKERS, &p->cfg->task_workers);
        else
            gt_unknown(p, v, path);
        if (r != DE_OK)
            return r;
    }
    return DE_OK;
}

static de_result gt_memory(gt_parser *p, const toml_datum_t *t) {
    for (int32_t i = 0; i < t->u.tab.size; ++i) {
        const toml_datum_t *v = &t->u.tab.value[i];
        char path[GT_PATH_MAX];
        if (!gt_path(path, sizeof path, "memory", t->u.tab.key[i], t->u.tab.len[i]))
            return gt_fail(p, v, "memory", "key path longer than %d bytes", GT_PATH_MAX - 1);
        de_result r = DE_OK;
        if (gt_key_is(t, i, "frame_arena_mib"))
            r = gt_u32(p, v, path, 1, DE__GAME_MAX_FRAME_ARENA_MIB, &p->cfg->frame_arena_mib);
        else
            gt_unknown(p, v, path);
        if (r != DE_OK)
            return r;
    }
    return DE_OK;
}

/* DENG-143: [assets], the asset system's settings (read by de__engine_create). */
static de_result gt_assets(gt_parser *p, const toml_datum_t *t) {
    for (int32_t i = 0; i < t->u.tab.size; ++i) {
        const toml_datum_t *v = &t->u.tab.value[i];
        char path[GT_PATH_MAX];
        if (!gt_path(path, sizeof path, "assets", t->u.tab.key[i], t->u.tab.len[i]))
            return gt_fail(p, v, "assets", "key path longer than %d bytes", GT_PATH_MAX - 1);
        de_result r = DE_OK;
        if (gt_key_is(t, i, "io_threads"))
            r = gt_u32(p, v, path, 1, DE__GAME_MAX_IO_THREADS, &p->cfg->asset_io_threads);
        else if (gt_key_is(t, i, "hot_reload"))
            r = gt_bool(p, v, path, &p->cfg->asset_hot_reload);
        else if (gt_key_is(t, i, "locale"))
            r = gt_string(p, v, path, p->cfg->asset_locale, sizeof p->cfg->asset_locale);
        else
            gt_unknown(p, v, path);
        if (r != DE_OK)
            return r;
    }
    return DE_OK;
}

/* ---- [cvars] ----------------------------------------------------------------------------- */

/* Pass 1: every leaf is a bool, integer, float or string. `path` holds "cvars...." */
static de_result gt_cvars_check(gt_parser *p, const toml_datum_t *t, const char *path, u32 depth) {
    if (depth > GT_CVAR_DEPTH_MAX)
        return gt_fail(p, t, path, "nested deeper than %d tables", GT_CVAR_DEPTH_MAX);
    for (int32_t i = 0; i < t->u.tab.size; ++i) {
        const toml_datum_t *v = &t->u.tab.value[i];
        char sub[GT_PATH_MAX];
        if (!gt_path(sub, sizeof sub, path, t->u.tab.key[i], t->u.tab.len[i]))
            return gt_fail(p, v, path, "key path longer than %d bytes", GT_PATH_MAX - 1);
        switch (v->type) {
        case TOML_TABLE: {
            de_result r = gt_cvars_check(p, v, sub, depth + 1);
            if (r != DE_OK)
                return r;
            break;
        }
        case TOML_BOOLEAN:
        case TOML_INT64:
        case TOML_FP64:
        case TOML_STRING:
            break;
        default:
            return gt_fail(p, v, sub,
                           "a cvar value must be a boolean, integer, float or string, "
                           "found %s",
                           gt_type_name(v->type));
        }
    }
    return DE_OK;
}

/* Pass 2: applies every leaf as a startup value, the way the command line does (so a cvar
 * nobody registered yet keeps it pending, and read-only cvars take it until the startup lock).
 * `name` is the dotted cvar name so far ("" at the top). */
static void gt_cvars_apply(gt_parser *p, const toml_datum_t *t, const char *name) {
    for (int32_t i = 0; i < t->u.tab.size; ++i) {
        const toml_datum_t *v = &t->u.tab.value[i];
        char sub[GT_PATH_MAX];
        /* Checked to fit in pass 1, where the prefix "cvars." was longer. */
        (void)gt_path(sub, sizeof sub, name, t->u.tab.key[i], t->u.tab.len[i]);
        if (v->type == TOML_TABLE) {
            gt_cvars_apply(p, v, sub);
            continue;
        }
        char text[64];
        /* de_cvar_apply_args takes char ** but never writes through it. A union reads the
         * string pointer as non-const without a qualifier-dropping cast. */
        union {
            const char *c;
            char *m;
        } value = {.c = text};
        switch (v->type) {
        case TOML_BOOLEAN:
            snprintf(text, sizeof text, "%s", v->u.boolean ? "true" : "false");
            break;
        case TOML_INT64:
            snprintf(text, sizeof text, "%" PRId64, v->u.int64);
            break;
        case TOML_FP64:
            snprintf(text, sizeof text, "%.17g", v->u.fp64);
            break;
        default: /* TOML_STRING (pass 1 refused the rest) */
            value.c = v->u.str.ptr;
            break;
        }
        char arg[GT_PATH_MAX + 2];
        snprintf(arg, sizeof arg, "+%s", sub);
        char program[] = "game.toml";
        char *argv[3] = {program, arg, value.m};
        if (de_cvar_apply_args(3, argv) == DE_OK) {
            p->cfg->cvars_applied++;
        } else {
            const de_error *err = de_error_last();
            DE_WARN(p->log, "%s:%d: cvars.%s: %s (ignored)", p->source, v->lineno, sub,
                    err != NULL ? err->message : "refused");
            de_error_clear();
        }
    }
}

/* ---- Entry points ------------------------------------------------------------------------ */

de_result de__game_config_parse(const char *text, size_t len, const char *source,
                                de__game_config *out) {
    if (out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__game_config_parse: out is NULL");
    de__game_config_defaults(out);
    if (text == NULL && len != 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__game_config_parse: text is NULL");
    if (len > (size_t)INT_MAX)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__game_config_parse: %zu bytes is too long",
                       len);

    de__game_config cfg;
    de__game_config_defaults(&cfg);
    gt_parser p = {source != NULL ? source : "game.toml", &cfg,
                   de_log_register_category("host", DE_LOG_INFO)};

    toml_result_t res = de__toml_parse(NULL, text != NULL ? text : "", (int)len);
    if (!res.ok) {
        de_result r = DE_FAIL(DE_ERR_PARSE, "%s: %s", p.source, res.errmsg);
        de__toml_free(res);
        return r;
    }

    const toml_datum_t *top = &res.toptab;
    const toml_datum_t *cvars = NULL;
    de_result r = DE_OK;
    for (int32_t i = 0; i < top->u.tab.size && r == DE_OK; ++i) {
        const toml_datum_t *v = &top->u.tab.value[i];
        char path[GT_PATH_MAX];
        if (!gt_path(path, sizeof path, "", top->u.tab.key[i], top->u.tab.len[i])) {
            r = gt_fail(&p, v, "(top level)", "key longer than %d bytes", GT_PATH_MAX - 1);
            break;
        }
        de_result (*section)(gt_parser *, const toml_datum_t *) = NULL;
        bool is_cvars = false;
        if (gt_key_is(top, i, "game"))
            section = gt_game;
        else if (gt_key_is(top, i, "window"))
            section = gt_window;
        else if (gt_key_is(top, i, "timing"))
            section = gt_timing;
        else if (gt_key_is(top, i, "tasks"))
            section = gt_tasks;
        else if (gt_key_is(top, i, "memory"))
            section = gt_memory;
        else if (gt_key_is(top, i, "assets")) /* DENG-143 */
            section = gt_assets;
        else if (gt_key_is(top, i, "cvars"))
            is_cvars = true;
        if (section == NULL && !is_cvars) {
            gt_unknown(&p, v, path);
            continue;
        }
        if (v->type != TOML_TABLE) {
            r = gt_fail(&p, v, path, "expected a table, found %s", gt_type_name(v->type));
            break;
        }
        if (is_cvars)
            cvars = v;
        else
            r = section(&p, v);
    }
    if (r == DE_OK && cvars != NULL)
        r = gt_cvars_check(&p, cvars, "cvars", 1);
    if (r == DE_OK && cvars != NULL)
        gt_cvars_apply(&p, cvars, "");
    de__toml_free(res);
    if (r != DE_OK)
        return r;
    *out = cfg;
    return DE_OK;
}

de_result de__game_config_load(const char *vpath, de__game_config *out) {
    if (out == NULL || vpath == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__game_config_load: NULL %s",
                       out == NULL ? "out" : "vpath");
    de__game_config_defaults(out);
    const de_allocator *a = de_heap_allocator();
    void *data = NULL;
    u64 size = 0;
    de_result r = de_file_read_all(vpath, a, &data, &size);
    if (r == DE_ERR_NOT_FOUND) {
        de_error_clear();
        DE_INFO(de_log_register_category("host", DE_LOG_INFO),
                "no project file at %s; using the default settings", vpath);
        return DE_OK;
    }
    if (r != DE_OK)
        return DE_FAIL_WRAP(r, "reading the project file %s", vpath);
    if (size > (u64)INT_MAX) {
        DE_FREE(a, data, (size_t)size + 1);
        return DE_FAIL(DE_ERR_PARSE, "%s: %" PRIu64 " bytes is too large", vpath, size);
    }
    r = de__game_config_parse((const char *)data, (size_t)size, vpath, out);
    DE_FREE(a, data, (size_t)size + 1);
    return r;
}
