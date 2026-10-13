/* bindings_io.c: the user bindings file (de_bindings_save / de_bindings_load, DENG-86).
 *
 * The file holds only overrides: per map and action, the bindings that differ from the action's
 * defaults (rebind.c), by binding index. Bindings use the field names and the name tables of
 * action-map files (action_toml.c): one source key with its name, then composite, modifier,
 * scale, deadzone, invert, curve, exponent and points. An unbound slot is `unbound = true`. A
 * code without a name (a key the name table lacks) is written as its number.
 *
 *     version = 1
 *     [maps.gameplay.actions.jump]
 *     bindings = [
 *         { index = 0, key = "f" },
 *     ]
 *
 * Writing goes through de_file_write_all_atomic, so a crash leaves the old or the new file.
 * Reading parses through core's tomlc17 entry point (never toml_parse or toml_set_option here)
 * and walks the tree twice, like action_toml.c: pass 1 checks every value and changes nothing,
 * pass 2 resets every action to its defaults and applies the file. A bad file therefore changes
 * no binding. Entries that point at something the game no longer has (a map, an action, a
 * binding index, a name; an action that is not rebindable; a binding that no longer fits the
 * action's type) are skipped with a warning, so a game update does not throw away the player's
 * other bindings. */
#include "action_internal.h"

#include "core/de_strbuf.h"
#include "core/de_toml_internal.h"

#include <dengine/de_memory.h>

#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The file format version this code writes and reads. */
#define BIO_VERSION 1

/* ---------- Save ---------- */

static bool bio_bare_key(const char *s) {
    if (*s == '\0')
        return false;
    for (; *s != '\0'; ++s) {
        const char c = *s;
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok)
            return false;
    }
    return true;
}

/* Whether `s` is valid UTF-8 (TOML files must be; a name that is not cannot be written). */
static bool bio_utf8(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    while (*p != 0) {
        u32 n = 0, cp = 0;
        if (*p < 0x80) {
            ++p;
            continue;
        } else if ((*p & 0xE0) == 0xC0) {
            n = 1;
            cp = *p & 0x1Fu;
        } else if ((*p & 0xF0) == 0xE0) {
            n = 2;
            cp = *p & 0x0Fu;
        } else if ((*p & 0xF8) == 0xF0) {
            n = 3;
            cp = *p & 0x07u;
        } else {
            return false;
        }
        ++p;
        for (u32 i = 0; i < n; ++i, ++p) {
            if ((*p & 0xC0) != 0x80)
                return false;
            cp = (cp << 6) | (*p & 0x3Fu);
        }
        const u32 min = n == 1 ? 0x80u : n == 2 ? 0x800u : 0x10000u;
        if (cp < min || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu))
            return false;
    }
    return true;
}

/* A TOML key: bare when it can be, else a basic string with escapes. */
static de_result bio_key(de_strbuf *sb, const char *s) {
    if (bio_bare_key(s))
        return de_strbuf_append_cstr(sb, s);
    DE_TRY(de_strbuf_append_char(sb, '"'));
    for (const unsigned char *p = (const unsigned char *)s; *p != 0; ++p) {
        if (*p == '"' || *p == '\\') {
            DE_TRY(de_strbuf_append_char(sb, '\\'));
            DE_TRY(de_strbuf_append_char(sb, (char)*p));
        } else if (*p < 0x20 || *p == 0x7F) {
            DE_TRY(de_strbuf_appendf(sb, "\\u%04X", (unsigned)*p));
        } else {
            DE_TRY(de_strbuf_append_char(sb, (char)*p));
        }
    }
    return de_strbuf_append_char(sb, '"');
}

/* The shortest decimal that reads back to the same f32 (TOML integer or float syntax). */
static de_result bio_f32(de_strbuf *sb, f32 v) {
    char buf[32];
    for (int prec = 6; prec <= 9; ++prec) {
        snprintf(buf, sizeof buf, "%.*g", prec, (double)v);
        if (strtof(buf, NULL) == v)
            break;
    }
    return de_strbuf_append_cstr(sb, buf);
}

/* A source or modifier name: "<name>", or the number when the code has no name. */
static de_result bio_code(de_strbuf *sb, de_binding_source src, u32 code) {
    const char *name = de__binding_code_name(src, code);
    if (name != NULL && name[0] != '\0')
        return de_strbuf_appendf(sb, "\"%s\"", name);
    return de_strbuf_appendf(sb, "%u", (unsigned)code);
}

static de_result bio_binding(de_strbuf *sb, de_action_type type, u32 index, const de_binding *b) {
    DE_TRY(de_strbuf_appendf(sb, "    { index = %u, ", (unsigned)index));
    if (de_binding_is_unbound(b)) {
        DE_TRY(de_strbuf_append_cstr(sb, "unbound = true"));
    } else {
        DE_TRY(de_strbuf_appendf(sb, "%s = ", de__binding_source_name(b->source)));
        DE_TRY(bio_code(sb, b->source, b->code));
    }
    const char *comp =
        type != DE_ACTION_BUTTON ? de__composite_name(type, b->composite_slot) : NULL;
    if (comp != NULL)
        DE_TRY(de_strbuf_appendf(sb, ", composite = \"%s\"", comp));
    if (de_binding_is_unbound(b))
        return de_strbuf_append_cstr(sb, " },\n");
    if (b->modifier_key != 0) {
        DE_TRY(de_strbuf_append_cstr(sb, ", modifier = "));
        DE_TRY(bio_code(sb, DE_BIND_KEY, b->modifier_key));
    }
    if (b->scale != 0.0f) {
        DE_TRY(de_strbuf_append_cstr(sb, ", scale = "));
        DE_TRY(bio_f32(sb, b->scale));
    }
    if (b->deadzone != 0.0f) {
        DE_TRY(de_strbuf_append_cstr(sb, ", deadzone = "));
        DE_TRY(bio_f32(sb, b->deadzone));
    }
    if (b->invert)
        DE_TRY(de_strbuf_append_cstr(sb, ", invert = true"));
    if (b->curve.type == DE_CURVE_POWER) {
        DE_TRY(de_strbuf_append_cstr(sb, ", curve = \"power\", exponent = "));
        DE_TRY(bio_f32(sb, b->curve.exponent));
    } else if (b->curve.type == DE_CURVE_CUSTOM) {
        DE_TRY(de_strbuf_append_cstr(sb, ", curve = \"custom\", points = ["));
        for (u32 i = 0; i < b->curve.point_count && i < DE_CURVE_MAX_POINTS; ++i) {
            DE_TRY(de_strbuf_append_cstr(sb, i == 0 ? "[" : ", ["));
            DE_TRY(bio_f32(sb, b->curve.points[i].x));
            DE_TRY(de_strbuf_append_cstr(sb, ", "));
            DE_TRY(bio_f32(sb, b->curve.points[i].y));
            DE_TRY(de_strbuf_append_char(sb, ']'));
        }
        DE_TRY(de_strbuf_append_char(sb, ']'));
    }
    return de_strbuf_append_cstr(sb, " },\n");
}

/* The overrides of action `ai` (nothing when it has none). */
static de_result bio_action(de_strbuf *sb, const de_input *in, u32 ai) {
    const struct de__actions *s = in->actions;
    const de__action *act = &s->actions[ai];
    if (!act->rebindable)
        return DE_OK;
    de_binding defs[DE_ACTION_MAX_BINDINGS];
    const u32 dn = de__rebind_defaults(in, ai, defs, DE_ACTION_MAX_BINDINGS);
    bool header = false;
    for (u32 j = 0; j < act->binding_count && j < dn; ++j) {
        const de_binding *cur = &s->bindings[act->first_binding + j];
        if (de__binding_same(act->type, cur, &defs[j]))
            continue;
        if (!header) {
            const char *map = s->maps[act->map].name;
            if (!bio_utf8(map) || !bio_utf8(act->name))
                return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                               "de_bindings_save: the name of action '%s' or its map is not "
                               "UTF-8",
                               act->name);
            DE_TRY(de_strbuf_append_cstr(sb, "\n[maps."));
            DE_TRY(bio_key(sb, map));
            DE_TRY(de_strbuf_append_cstr(sb, ".actions."));
            DE_TRY(bio_key(sb, act->name));
            DE_TRY(de_strbuf_append_cstr(sb, "]\nbindings = [\n"));
            header = true;
        }
        DE_TRY(bio_binding(sb, act->type, j, cur));
    }
    return header ? de_strbuf_append_cstr(sb, "]\n") : DE_OK;
}

static de_result bio_write(de_strbuf *sb, const de_input *in) {
    DE_TRY(de_strbuf_append_cstr(sb, "# User bindings: the bindings that differ from the default "
                                     "action maps (de_bindings_save).\n"
                                     "# Each entry replaces binding `index` of an action.\n"));
    DE_TRY(de_strbuf_appendf(sb, "version = %d\n", BIO_VERSION));
    const struct de__actions *s = de__actions_state_const(in);
    if (s == NULL)
        return DE_OK;
    /* Maps in creation order, actions in declaration order: the same file for the same state. */
    for (u32 m = 0; m < s->map_count; ++m)
        for (u32 i = s->maps[m].first_action; i != DE__ACTION_NONE; i = s->actions[i].next_in_map)
            DE_TRY(bio_action(sb, in, i));
    return DE_OK;
}

de_result de_bindings_save(const de_input *in, const char *vpath) {
    if (in == NULL || vpath == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_bindings_save: NULL %s",
                       in == NULL ? "input state" : "path");
    de_strbuf sb;
    de_strbuf_init(&sb, &in->alloc);
    de_result r = bio_write(&sb, in);
    if (r == DE_OK) {
        const de_str text = de_strbuf_view(&sb);
        r = de_file_write_all_atomic(vpath, text.ptr != NULL ? text.ptr : "", (u64)text.len);
        if (r != DE_OK)
            r = DE_FAIL_WRAP(r, "de_bindings_save: writing %s", vpath);
    }
    de_strbuf_free(&sb);
    return r;
}

/* ---------- Load ---------- */

#define BIO_PATH_MAX 256

static const char *const k_curve_names[] = {"linear", "power", "custom"};

typedef struct bio_parser {
    de_input *in;
    const char *source;
    de_log_category log;
    bool apply; /* pass 2 */
    u32 warnings;
} bio_parser;

static bool bio_str_is(const char *s, size_t len, const char *name) {
    return strlen(name) == len && memcmp(s, name, len) == 0;
}

static bool bio_key_is(const toml_datum_t *tab, int32_t i, const char *name) {
    return bio_str_is(tab->u.tab.key[i], (size_t)tab->u.tab.len[i], name);
}

static const char *bio_type_name(toml_type_t t) {
    switch (t) {
    case TOML_STRING:
        return "a string";
    case TOML_INT64:
        return "an integer";
    case TOML_FP64:
        return "a float";
    case TOML_BOOLEAN:
        return "a boolean";
    case TOML_ARRAY:
        return "an array";
    case TOML_TABLE:
        return "a table";
    default:
        return "a date or time";
    }
}

/* DE_ERR_INVALID_ARGUMENT with "<source>:<line>: <path>: <message>". */
static de_result bio_fail(const bio_parser *p, const toml_datum_t *d, const char *path,
                          const char *fmt, ...) DE_PRINTF_FMT(4, 5);
static de_result bio_fail(const bio_parser *p, const toml_datum_t *d, const char *path,
                          const char *fmt, ...) {
    char msg[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof msg, fmt, args);
    va_end(args);
    if (d != NULL && d->lineno > 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s:%d: %s: %s", p->source, d->lineno, path, msg);
    return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s: %s: %s", p->source, path, msg);
}

/* A warning (pass 1 only; pass 2 sees the same entries again). */
static void bio_warn(bio_parser *p, const toml_datum_t *d, const char *path, const char *fmt, ...)
    DE_PRINTF_FMT(4, 5);
static void bio_warn(bio_parser *p, const toml_datum_t *d, const char *path, const char *fmt, ...) {
    if (p->apply)
        return;
    p->warnings++;
    char msg[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof msg, fmt, args);
    va_end(args);
    if (d != NULL && d->lineno > 0)
        DE_WARN(p->log, "%s:%d: %s: %s (skipped)", p->source, d->lineno, path, msg);
    else
        DE_WARN(p->log, "%s: %s: %s (skipped)", p->source, path, msg);
}

/* "<prefix>.<key>" into buf; false when it does not fit. */
static bool bio_path(char *buf, size_t cap, const char *prefix, const char *key, int key_len) {
    const size_t pl = strlen(prefix);
    const size_t kl = key_len > 0 ? (size_t)key_len : 0u;
    const size_t dot = pl != 0 ? 1u : 0u;
    if (pl + dot + kl + 1u > cap)
        return false;
    memcpy(buf, prefix, pl);
    if (dot != 0)
        buf[pl] = '.';
    memcpy(buf + pl + dot, key, kl);
    buf[pl + dot + kl] = '\0';
    return true;
}

static de_result bio_num(const bio_parser *p, const toml_datum_t *v, const char *path, f64 min,
                         f64 max, f32 *dst) {
    f64 x;
    if (v->type == TOML_FP64)
        x = v->u.fp64;
    else if (v->type == TOML_INT64)
        x = (f64)v->u.int64;
    else
        return bio_fail(p, v, path, "expected a number, found %s", bio_type_name(v->type));
    if (!(x >= min) || !(x <= max))
        return bio_fail(p, v, path, "%g is out of range (%g to %g)", x, min, max);
    *dst = (f32)x;
    return DE_OK;
}

static de_result bio_bool(const bio_parser *p, const toml_datum_t *v, const char *path, bool *dst) {
    if (v->type != TOML_BOOLEAN)
        return bio_fail(p, v, path, "expected true or false, found %s", bio_type_name(v->type));
    *dst = v->u.boolean;
    return DE_OK;
}

/* A code of source `src`: a name (unknown: `*known` false) or a number (must be valid). */
static de_result bio_code_value(const bio_parser *p, const toml_datum_t *v, const char *path,
                                de_binding_source src, u32 *code, bool *known) {
    *known = true;
    if (v->type == TOML_STRING) {
        if (!de__binding_code_from_name(src, v->u.str.ptr, (size_t)v->u.str.len, code))
            *known = false;
        return DE_OK;
    }
    if (v->type == TOML_INT64) {
        de_binding t;
        memset(&t, 0, sizeof t);
        t.source = src;
        t.code = v->u.int64 >= 0 && v->u.int64 <= (int64_t)UINT32_MAX ? (u32)v->u.int64 : ~0u;
        if (!de__binding_fits(DE_ACTION_BUTTON, &t))
            return bio_fail(p, v, path, "%" PRId64 " is not a %s code", v->u.int64,
                            de__binding_source_name(src));
        *code = t.code;
        return DE_OK;
    }
    return bio_fail(p, v, path, "expected a name, found %s", bio_type_name(v->type));
}

static de_result bio_points(const bio_parser *p, const toml_datum_t *v, const char *path,
                            de_response_curve *c) {
    if (v->type != TOML_ARRAY)
        return bio_fail(p, v, path, "expected an array of [x, y] pairs, found %s",
                        bio_type_name(v->type));
    if (v->u.arr.size < 2 || v->u.arr.size > DE_CURVE_MAX_POINTS)
        return bio_fail(p, v, path, "a custom curve needs 2 to %d points, found %d",
                        DE_CURVE_MAX_POINTS, (int)v->u.arr.size);
    for (int32_t i = 0; i < v->u.arr.size; ++i) {
        const toml_datum_t *pt = &v->u.arr.elem[i];
        if (pt->type != TOML_ARRAY || pt->u.arr.size != 2)
            return bio_fail(p, pt->lineno > 0 ? pt : v, path, "point %d is not an [x, y] pair",
                            (int)i);
        f32 xy[2] = {0.0f, 0.0f};
        for (int32_t k = 0; k < 2; ++k)
            DE_TRY(bio_num(p, &pt->u.arr.elem[k], path, -1.0e6, 1.0e6, &xy[k]));
        c->points[i].x = xy[0];
        c->points[i].y = xy[1];
    }
    c->point_count = (u32)v->u.arr.size;
    return DE_OK;
}

/* One entry { index = .., <source> = .., ... } of action `ai`. Pass 2 applies it. */
static de_result bio_entry(bio_parser *p, const toml_datum_t *t, const char *path, u32 ai) {
    if (t->type != TOML_TABLE)
        return bio_fail(p, t, path, "expected an inline table, found %s", bio_type_name(t->type));
    const struct de__actions *s = p->in->actions;
    const de__action *act = &s->actions[ai];
    de_binding b;
    memset(&b, 0, sizeof b);
    b.composite_slot = DE_COMPOSITE_NONE;
    bool has_index = false, has_source = false, unbound = false, has_composite = false;
    bool skip = false, has_exponent = false, has_points = false;
    const char *skip_why = "";
    i64 index = -1;
    const toml_datum_t *index_d = t;
    const toml_datum_t *curve_d = t;
    for (int32_t i = 0; i < t->u.tab.size; ++i) {
        const toml_datum_t *v = &t->u.tab.value[i];
        const char *key = t->u.tab.key[i];
        const size_t key_len = (size_t)t->u.tab.len[i];
        char kp[BIO_PATH_MAX];
        if (!bio_path(kp, sizeof kp, path, key, (int)key_len))
            return bio_fail(p, v, path, "key too long");
        de_binding_source src = DE_BIND_KEY;
        if (bio_key_is(t, i, "index")) {
            if (v->type != TOML_INT64)
                return bio_fail(p, v, kp, "expected an integer, found %s", bio_type_name(v->type));
            if (v->u.int64 < 0 || v->u.int64 >= DE_ACTION_MAX_BINDINGS)
                return bio_fail(p, v, kp, "%" PRId64 " is out of range (0 to %d)", v->u.int64,
                                DE_ACTION_MAX_BINDINGS - 1);
            index = (i64)v->u.int64;
            index_d = v;
            has_index = true;
        } else if (de__binding_source_from_name(key, key_len, &src)) {
            if (has_source || unbound)
                return bio_fail(p, v, kp, "a binding has exactly one source (found a second)");
            has_source = true;
            b.source = src;
            bool known = true;
            DE_TRY(bio_code_value(p, v, kp, src, &b.code, &known));
            if (!known) {
                skip = true;
                skip_why = "unknown name";
            }
        } else if (bio_key_is(t, i, "unbound")) {
            bool u = false;
            DE_TRY(bio_bool(p, v, kp, &u));
            if (!u)
                return bio_fail(p, v, kp, "only `unbound = true` is valid");
            if (has_source)
                return bio_fail(p, v, kp, "a binding has exactly one source (found a second)");
            unbound = true;
        } else if (bio_key_is(t, i, "composite")) {
            if (v->type != TOML_STRING)
                return bio_fail(p, v, kp, "expected a string, found %s", bio_type_name(v->type));
            has_composite = true;
            if (!de__composite_from_name(v->u.str.ptr, (size_t)v->u.str.len, act->type,
                                         &b.composite_slot)) {
                skip = true; /* the action's type changed since the file was written */
                skip_why = "the composite does not fit the action";
            }
        } else if (bio_key_is(t, i, "modifier")) {
            bool known = true;
            DE_TRY(bio_code_value(p, v, kp, DE_BIND_KEY, &b.modifier_key, &known));
            if (!known) {
                skip = true;
                skip_why = "unknown modifier key";
            }
        } else if (bio_key_is(t, i, "scale")) {
            DE_TRY(bio_num(p, v, kp, -1.0e6, 1.0e6, &b.scale));
        } else if (bio_key_is(t, i, "deadzone")) {
            DE_TRY(bio_num(p, v, kp, 0.0, 0.99999994, &b.deadzone));
        } else if (bio_key_is(t, i, "invert")) {
            DE_TRY(bio_bool(p, v, kp, &b.invert));
        } else if (bio_key_is(t, i, "curve")) {
            if (v->type != TOML_STRING)
                return bio_fail(p, v, kp, "expected a string, found %s", bio_type_name(v->type));
            bool found = false;
            for (u32 c = 0; c < 3; ++c) {
                if (bio_str_is(v->u.str.ptr, (size_t)v->u.str.len, k_curve_names[c])) {
                    b.curve.type = (de_curve_type)c;
                    found = true;
                }
            }
            if (!found)
                return bio_fail(p, v, kp, "unknown curve \"%.*s\"",
                                v->u.str.len > 64 ? 64 : v->u.str.len, v->u.str.ptr);
            curve_d = v;
        } else if (bio_key_is(t, i, "exponent")) {
            DE_TRY(bio_num(p, v, kp, 0.01, 100.0, &b.curve.exponent));
            has_exponent = true;
        } else if (bio_key_is(t, i, "points")) {
            DE_TRY(bio_points(p, v, kp, &b.curve));
            has_points = true;
        } else {
            bio_warn(p, v, kp, "unknown key");
        }
    }
    if (!has_index)
        return bio_fail(p, t, path, "no index");
    if (!has_source && !unbound)
        return bio_fail(p, t, path,
                        "no source (key, mouse_button, mouse_delta, mouse_wheel, pad_button, "
                        "pad_axis, gyro, or unbound = true)");
    if (b.curve.type == DE_CURVE_POWER && !has_exponent)
        b.curve.exponent = 2.0f;
    if (b.curve.type == DE_CURVE_CUSTOM && !has_points)
        return bio_fail(p, curve_d, path, "a custom curve needs points = [[x, y], ...]");
    if (b.curve.type != DE_CURVE_CUSTOM && has_points)
        return bio_fail(p, curve_d, path, "points need curve = \"custom\"");
    DE_TRY(de__curve_validate(&b.curve, path));

    if ((u64)index >= act->binding_count) {
        bio_warn(p, index_d, path, "action '%s' has %u bindings", act->name,
                 (unsigned)act->binding_count);
        return DE_OK;
    }
    const de_binding cur = s->bindings[act->first_binding + (u32)index];
    if (unbound) {
        b.source = DE_BIND_KEY;
        b.code = DE_BIND_UNBOUND_KEY;
        if (!has_composite)
            b.composite_slot = cur.composite_slot;
    }
    if (act->type == DE_ACTION_BUTTON)
        b.composite_slot = DE_COMPOSITE_NONE;
    if (!skip && !de__binding_fits(act->type, &b)) {
        skip = true;
        skip_why = "the binding does not fit the action";
    }
    if (skip) {
        bio_warn(p, t, path, "%s", skip_why);
        return DE_OK;
    }
    if (!p->apply)
        return DE_OK;
    return de__rebind_set(p->in, ai, (u32)index, &b);
}

/* [maps.<map>.actions.<action>] of existing action `ai`. */
static de_result bio_action_table(bio_parser *p, const toml_datum_t *t, const char *path, u32 ai) {
    for (int32_t i = 0; i < t->u.tab.size; ++i) {
        const toml_datum_t *v = &t->u.tab.value[i];
        char kp[BIO_PATH_MAX];
        if (!bio_path(kp, sizeof kp, path, t->u.tab.key[i], t->u.tab.len[i]))
            return bio_fail(p, v, path, "key too long");
        if (!bio_key_is(t, i, "bindings")) {
            bio_warn(p, v, kp, "unknown key");
            continue;
        }
        if (v->type != TOML_ARRAY)
            return bio_fail(p, v, kp, "expected an array of inline tables, found %s",
                            bio_type_name(v->type));
        for (int32_t k = 0; k < v->u.arr.size; ++k) {
            char bp[BIO_PATH_MAX + 16];
            const int bn = snprintf(bp, sizeof bp, "%s[%d]", kp, (int)k);
            if (bn < 0 || (size_t)bn >= sizeof bp)
                return bio_fail(p, v, kp, "key too long");
            const toml_datum_t *e = &v->u.arr.elem[k];
            DE_TRY(bio_entry(p, e->lineno > 0 ? e : v, bp, ai));
        }
    }
    return DE_OK;
}

/* Finds action `name` (`len` bytes) in map index `mi`. */
static u32 bio_find_action(const struct de__actions *s, u32 mi, const char *name, size_t len) {
    for (u32 i = s->maps[mi].first_action; i != DE__ACTION_NONE; i = s->actions[i].next_in_map)
        if (bio_str_is(name, len, s->actions[i].name))
            return i;
    return DE__ACTION_NONE;
}

static u32 bio_find_map(const struct de__actions *s, const char *name, size_t len) {
    for (u32 i = 0; s != NULL && i < s->map_count; ++i)
        if (bio_str_is(name, len, s->maps[i].name))
            return i;
    return DE__ACTION_NONE;
}

/* [maps.<map>] of existing map `mi`. */
static de_result bio_map(bio_parser *p, const toml_datum_t *t, const char *path, u32 mi) {
    const struct de__actions *s = p->in->actions;
    for (int32_t i = 0; i < t->u.tab.size; ++i) {
        const toml_datum_t *v = &t->u.tab.value[i];
        char kp[BIO_PATH_MAX];
        if (!bio_path(kp, sizeof kp, path, t->u.tab.key[i], t->u.tab.len[i]))
            return bio_fail(p, v, path, "key too long");
        if (!bio_key_is(t, i, "actions")) {
            bio_warn(p, v, kp, "unknown key");
            continue;
        }
        if (v->type != TOML_TABLE)
            return bio_fail(p, v, kp, "expected a table, found %s", bio_type_name(v->type));
        for (int32_t k = 0; k < v->u.tab.size; ++k) {
            const toml_datum_t *av = &v->u.tab.value[k];
            char ap[BIO_PATH_MAX];
            if (!bio_path(ap, sizeof ap, kp, v->u.tab.key[k], v->u.tab.len[k]))
                return bio_fail(p, av, kp, "key too long");
            if (av->type != TOML_TABLE)
                return bio_fail(p, av, ap, "expected a table, found %s", bio_type_name(av->type));
            const u32 ai = bio_find_action(s, mi, v->u.tab.key[k], (size_t)v->u.tab.len[k]);
            if (ai == DE__ACTION_NONE) {
                bio_warn(p, av, ap, "no such action");
                continue;
            }
            if (!s->actions[ai].rebindable) {
                bio_warn(p, av, ap, "the action is not rebindable");
                continue;
            }
            DE_TRY(bio_action_table(p, av, ap, ai));
        }
    }
    return DE_OK;
}

static de_result bio_top(bio_parser *p, const toml_datum_t *top) {
    for (int32_t i = 0; i < top->u.tab.size; ++i) {
        const toml_datum_t *v = &top->u.tab.value[i];
        char path[BIO_PATH_MAX];
        if (!bio_path(path, sizeof path, "", top->u.tab.key[i], top->u.tab.len[i]))
            return bio_fail(p, v, "(top level)", "key too long");
        if (bio_key_is(top, i, "version")) {
            if (v->type != TOML_INT64)
                return bio_fail(p, v, path, "expected an integer, found %s",
                                bio_type_name(v->type));
            if (v->u.int64 != BIO_VERSION)
                return bio_fail(p, v, path, "version %" PRId64 " is not supported (%d is)",
                                v->u.int64, BIO_VERSION);
            continue;
        }
        if (!bio_key_is(top, i, "maps")) {
            bio_warn(p, v, path, "unknown key");
            continue;
        }
        if (v->type != TOML_TABLE)
            return bio_fail(p, v, path, "expected a table, found %s", bio_type_name(v->type));
        for (int32_t k = 0; k < v->u.tab.size; ++k) {
            const toml_datum_t *mv = &v->u.tab.value[k];
            char mp[BIO_PATH_MAX];
            if (!bio_path(mp, sizeof mp, path, v->u.tab.key[k], v->u.tab.len[k]))
                return bio_fail(p, mv, path, "key too long");
            if (mv->type != TOML_TABLE)
                return bio_fail(p, mv, mp, "expected a table, found %s", bio_type_name(mv->type));
            const u32 mi = bio_find_map(de__actions_state_const(p->in), v->u.tab.key[k],
                                        (size_t)v->u.tab.len[k]);
            if (mi == DE__ACTION_NONE) {
                bio_warn(p, mv, mp, "no such map");
                continue;
            }
            DE_TRY(bio_map(p, mv, mp, mi));
        }
    }
    return DE_OK;
}

de_result de__bindings_parse(de_input *in, const char *text, size_t len, const char *source,
                             u32 *warnings) {
    if (warnings != NULL)
        *warnings = 0;
    if (in == NULL || (text == NULL && len != 0))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_bindings_load: NULL input or text");
    if (len > (size_t)INT_MAX)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_bindings_load: %zu bytes is too long", len);
    bio_parser p;
    memset(&p, 0, sizeof p);
    p.in = in;
    p.source = source != NULL ? source : "bindings.toml";
    p.log = in->log;

    toml_result_t res = de__toml_parse(&in->alloc, text != NULL ? text : "", (int)len);
    if (!res.ok) {
        const de_result r = DE_FAIL(DE_ERR_PARSE, "%s: %s", p.source, res.errmsg);
        de__toml_free(res);
        return r;
    }
    de_result r = bio_top(&p, &res.toptab);
    /* The rebinding state before any change, so running out of memory changes nothing. */
    if (r == DE_OK && in->actions != NULL && de__rebind_get_or_create(in) == NULL)
        r = de_error_last() != NULL ? de_error_last()->result : DE_ERR_OUT_OF_MEMORY;
    if (r == DE_OK) {
        /* The file replaces the user layer: defaults first, then its entries. */
        de_bindings_reset_all(in);
        p.apply = true;
        r = bio_top(&p, &res.toptab);
    }
    de__toml_free(res);
    if (warnings != NULL)
        *warnings = p.warnings;
    return r;
}

de_result de_bindings_load(de_input *in, const char *vpath) {
    if (in == NULL || vpath == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_bindings_load: NULL %s",
                       in == NULL ? "input state" : "path");
    void *data = NULL;
    u64 size = 0;
    const de_result rr = de_file_read_all(vpath, &in->alloc, &data, &size);
    if (rr != DE_OK)
        return DE_FAIL_WRAP(rr, "de_bindings_load: reading %s", vpath);
    de_result r;
    if (size > (u64)INT_MAX)
        r = DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s: %" PRIu64 " bytes is too large", vpath, size);
    else
        r = de__bindings_parse(in, (const char *)data, (size_t)size, vpath, NULL);
    DE_FREE(&in->alloc, data, (size_t)size + 1);
    return r;
}
