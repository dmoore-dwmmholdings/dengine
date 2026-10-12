/* action_toml.c: action-map files (de_action_maps_load, DENG-84) and the stable name tables of
 * binding sources, codes and composite slots (shared with DENG-86 save and DENG-88 glyphs).
 *
 * Parsing goes through core's tomlc17 entry point (core/de_toml_internal.h); never toml_parse or
 * toml_set_option here, the allocator hooks are process-wide.
 *
 * The file is read twice over the parsed tree: pass 1 checks every value and adds nothing; pass 2
 * adds the maps and actions in file order. So a bad file changes nothing. Bad values fail with
 * DE_ERR_INVALID_ARGUMENT and "<file>:<line>: <key path>: <what is wrong>"; unknown keys log a
 * warning in the same form and are skipped.
 *
 * DENG-85 appends one parser function for a top-level [contexts] table below and one dispatch
 * line in parse_top(). */
#include "action_internal.h"

#include "core/de_toml_internal.h"

#include <dengine/de_memory.h>

#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---------- Name tables ---------- */

static const char *const k_source_names[DE_BIND_SOURCE_COUNT_] = {
    [DE_BIND_KEY] = "key",
    [DE_BIND_MOUSE_BUTTON] = "mouse_button",
    [DE_BIND_MOUSE_DELTA] = "mouse_delta",
    [DE_BIND_MOUSE_WHEEL] = "mouse_wheel",
    [DE_BIND_PAD_BUTTON] = "pad_button",
    [DE_BIND_PAD_AXIS] = "pad_axis",
    [DE_BIND_GYRO] = "gyro",
};

static const char *const k_mouse_button_names[DE_MOUSE_BUTTON_COUNT_] = {
    [DE_MOUSE_LEFT] = "left", [DE_MOUSE_RIGHT] = "right", [DE_MOUSE_MIDDLE] = "middle",
    [DE_MOUSE_X1] = "x1",     [DE_MOUSE_X2] = "x2",
};

static const char *const k_pad_button_names[DE_PAD_BUTTON_COUNT_] = {
    [DE_PAD_SOUTH] = "south",
    [DE_PAD_EAST] = "east",
    [DE_PAD_WEST] = "west",
    [DE_PAD_NORTH] = "north",
    [DE_PAD_BACK] = "back",
    [DE_PAD_GUIDE] = "guide",
    [DE_PAD_START] = "start",
    [DE_PAD_LSTICK] = "lstick",
    [DE_PAD_RSTICK] = "rstick",
    [DE_PAD_LSHOULDER] = "lshoulder",
    [DE_PAD_RSHOULDER] = "rshoulder",
    [DE_PAD_DPAD_UP] = "dpad_up",
    [DE_PAD_DPAD_DOWN] = "dpad_down",
    [DE_PAD_DPAD_LEFT] = "dpad_left",
    [DE_PAD_DPAD_RIGHT] = "dpad_right",
    [DE_PAD_MISC1] = "misc1",
    [DE_PAD_PADDLE1] = "paddle1",
    [DE_PAD_PADDLE2] = "paddle2",
    [DE_PAD_PADDLE3] = "paddle3",
    [DE_PAD_PADDLE4] = "paddle4",
    [DE_PAD_TOUCHPAD] = "touchpad",
};

/* Pad axis codes 0..5 (de_pad_axis) and the two stick codes. */
static const char *const k_pad_axis_names[DE_BIND_RIGHT_STICK + 1u] = {
    [DE_AXIS_LEFT_X] = "left_x",
    [DE_AXIS_LEFT_Y] = "left_y",
    [DE_AXIS_RIGHT_X] = "right_x",
    [DE_AXIS_RIGHT_Y] = "right_y",
    [DE_AXIS_LEFT_TRIGGER] = "left_trigger",
    [DE_AXIS_RIGHT_TRIGGER] = "right_trigger",
    [DE_BIND_LEFT_STICK] = "left_stick",
    [DE_BIND_RIGHT_STICK] = "right_stick",
};

static const char *const k_axis_names[DE_BIND_AXIS_XY + 1u] = {
    [DE_BIND_AXIS_X] = "x",
    [DE_BIND_AXIS_Y] = "y",
    [DE_BIND_AXIS_Z] = "z",
    [DE_BIND_AXIS_XY] = "xy",
};

static const char *const k_composite_2d[4] = {"up", "down", "left", "right"};
static const char *const k_composite_1d[2] = {"positive", "negative"};

static bool str_is(const char *s, size_t len, const char *name) {
    return name != NULL && strlen(name) == len && memcmp(s, name, len) == 0;
}

const char *de__binding_source_name(de_binding_source s) {
    return (u32)s < (u32)DE_BIND_SOURCE_COUNT_ ? k_source_names[s] : NULL;
}

bool de__binding_source_from_name(const char *name, size_t len, de_binding_source *out) {
    for (u32 i = 0; i < (u32)DE_BIND_SOURCE_COUNT_; ++i) {
        if (str_is(name, len, k_source_names[i])) {
            if (out != NULL)
                *out = (de_binding_source)i;
            return true;
        }
    }
    return false;
}

/* The table of source `s` (NULL for keys, which use de_key_name). */
static const char *const *code_table(de_binding_source s, u32 *count) {
    switch (s) {
    case DE_BIND_MOUSE_BUTTON:
        *count = DE_MOUSE_BUTTON_COUNT_;
        return k_mouse_button_names;
    case DE_BIND_PAD_BUTTON:
        *count = DE_PAD_BUTTON_COUNT_;
        return k_pad_button_names;
    case DE_BIND_PAD_AXIS:
        *count = DE_BIND_RIGHT_STICK + 1u;
        return k_pad_axis_names;
    case DE_BIND_MOUSE_DELTA:
    case DE_BIND_MOUSE_WHEEL:
    case DE_BIND_GYRO:
        *count = DE_BIND_AXIS_XY + 1u;
        return k_axis_names;
    default:
        *count = 0;
        return NULL;
    }
}

const char *de__binding_code_name(de_binding_source s, u32 code) {
    if (s == DE_BIND_KEY)
        return code < (u32)DE_KEY_COUNT_ ? de_key_name((de_key)code) : NULL;
    u32 n = 0;
    const char *const *t = code_table(s, &n);
    if (t == NULL || code >= n)
        return NULL;
    /* Mouse delta and wheel have no z. */
    if ((s == DE_BIND_MOUSE_DELTA || s == DE_BIND_MOUSE_WHEEL) && code == DE_BIND_AXIS_Z)
        return NULL;
    return t[code];
}

bool de__binding_code_from_name(de_binding_source s, const char *name, size_t len, u32 *out) {
    if (name == NULL)
        return false;
    if (s == DE_BIND_KEY) {
        char buf[32];
        if (len == 0 || len >= sizeof buf)
            return false;
        memcpy(buf, name, len);
        buf[len] = '\0';
        const de_key k = de_key_from_name(buf);
        if (k == DE_KEY_UNKNOWN)
            return false;
        if (out != NULL)
            *out = (u32)k;
        return true;
    }
    u32 n = 0;
    const char *const *t = code_table(s, &n);
    for (u32 i = 0; t != NULL && i < n; ++i) {
        if (str_is(name, len, de__binding_code_name(s, i))) {
            if (out != NULL)
                *out = i;
            return true;
        }
    }
    return false;
}

const char *de__composite_name(de_action_type t, i8 slot) {
    if (t == DE_ACTION_AXIS2D && slot >= 0 && slot < 4)
        return k_composite_2d[slot];
    if (t == DE_ACTION_AXIS1D && slot >= 0 && slot < 2)
        return k_composite_1d[slot];
    return NULL;
}

bool de__composite_from_name(const char *name, size_t len, de_action_type t, i8 *out) {
    const char *const *tab = t == DE_ACTION_AXIS2D ? k_composite_2d : k_composite_1d;
    const u32 n = t == DE_ACTION_AXIS2D ? 4u : t == DE_ACTION_AXIS1D ? 2u : 0u;
    for (u32 i = 0; i < n; ++i) {
        if (str_is(name, len, tab[i])) {
            if (out != NULL)
                *out = (i8)i;
            return true;
        }
    }
    return false;
}

/* ---------- Parser ---------- */

#define AT_PATH_MAX 256

static const char *const k_type_names[] = {"button", "axis1d", "axis2d"};
static const char *const k_interaction_names[] = {"press", "release", "hold", "tap", "double_tap"};
static const char *const k_curve_names[] = {"linear", "power", "custom"};

typedef struct at_parser {
    de_input *in;
    const char *source;
    de_log_category log;
    bool apply; /* pass 2: add maps and actions */
    u32 unknown_keys;
} at_parser;

static const char *at_type_name(toml_type_t t) {
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
static de_result at_fail(const at_parser *p, const toml_datum_t *d, const char *path,
                         const char *fmt, ...) DE_PRINTF_FMT(4, 5);
static de_result at_fail(const at_parser *p, const toml_datum_t *d, const char *path,
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

static void at_unknown(at_parser *p, const toml_datum_t *d, const char *path) {
    if (p->apply)
        return; /* warned in pass 1 */
    p->unknown_keys++;
    if (d->lineno > 0)
        DE_WARN(p->log, "%s:%d: unknown key '%s' (ignored)", p->source, d->lineno, path);
    else
        DE_WARN(p->log, "%s: unknown key '%s' (ignored)", p->source, path);
}

/* "<prefix>.<key>" into buf; false when it does not fit. */
static bool at_path(char *buf, size_t cap, const char *prefix, const char *key, int key_len) {
    /* memcpy, not snprintf: GCC's -Wformat-truncation cannot tell that the length is checked. */
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

static bool at_key_is(const toml_datum_t *tab, int32_t i, const char *name) {
    return str_is(tab->u.tab.key[i], (size_t)tab->u.tab.len[i], name);
}

static de_result at_f32(const at_parser *p, const toml_datum_t *v, const char *path, f64 min,
                        f64 max, f32 *dst) {
    f64 x;
    if (v->type == TOML_FP64)
        x = v->u.fp64;
    else if (v->type == TOML_INT64)
        x = (f64)v->u.int64;
    else
        return at_fail(p, v, path, "expected a number, found %s", at_type_name(v->type));
    if (!(x >= min) || !(x <= max))
        return at_fail(p, v, path, "%g is out of range (%g to %g)", x, min, max);
    *dst = (f32)x;
    return DE_OK;
}

static de_result at_bool(const at_parser *p, const toml_datum_t *v, const char *path, bool *dst) {
    if (v->type != TOML_BOOLEAN)
        return at_fail(p, v, path, "expected true or false, found %s", at_type_name(v->type));
    *dst = v->u.boolean;
    return DE_OK;
}

static de_result at_enum(const at_parser *p, const toml_datum_t *v, const char *path,
                         const char *const *names, u32 count, u32 *dst) {
    if (v->type == TOML_STRING) {
        for (u32 i = 0; i < count; ++i) {
            if (str_is(v->u.str.ptr, (size_t)v->u.str.len, names[i])) {
                *dst = i;
                return DE_OK;
            }
        }
        return at_fail(p, v, path, "unknown value \"%.*s\"", v->u.str.len > 64 ? 64 : v->u.str.len,
                       v->u.str.ptr);
    }
    return at_fail(p, v, path, "expected a string, found %s", at_type_name(v->type));
}

static de_result at_curve_points(const at_parser *p, const toml_datum_t *v, const char *path,
                                 de_response_curve *c) {
    if (v->type != TOML_ARRAY)
        return at_fail(p, v, path, "expected an array of [x, y] pairs, found %s",
                       at_type_name(v->type));
    if (v->u.arr.size < 2 || v->u.arr.size > DE_CURVE_MAX_POINTS)
        return at_fail(p, v, path, "a custom curve needs 2 to %d points, found %d",
                       DE_CURVE_MAX_POINTS, (int)v->u.arr.size);
    for (int32_t i = 0; i < v->u.arr.size; ++i) {
        const toml_datum_t *pt = &v->u.arr.elem[i];
        if (pt->type != TOML_ARRAY || pt->u.arr.size != 2)
            return at_fail(p, pt->lineno > 0 ? pt : v, path, "point %d is not an [x, y] pair",
                           (int)i);
        f32 xy[2] = {0.0f, 0.0f};
        for (int32_t k = 0; k < 2; ++k)
            DE_TRY(at_f32(p, &pt->u.arr.elem[k], path, -1.0e6, 1.0e6, &xy[k]));
        c->points[i].x = xy[0];
        c->points[i].y = xy[1];
    }
    c->point_count = (u32)v->u.arr.size;
    return DE_OK;
}

/* One binding (an inline table) of an action of `type`. */
static de_result at_binding(at_parser *p, const toml_datum_t *t, const char *path,
                            de_action_type type, de_binding *b) {
    if (t->type != TOML_TABLE)
        return at_fail(p, t, path, "expected an inline table, found %s", at_type_name(t->type));
    memset(b, 0, sizeof *b);
    b->composite_slot = DE_COMPOSITE_NONE;
    bool has_source = false, has_exponent = false, has_points = false;
    const toml_datum_t *curve_d = NULL;
    const toml_datum_t *points_d = NULL;
    for (int32_t i = 0; i < t->u.tab.size; ++i) {
        const toml_datum_t *v = &t->u.tab.value[i];
        const char *key = t->u.tab.key[i];
        const size_t key_len = (size_t)t->u.tab.len[i];
        char kp[AT_PATH_MAX];
        if (!at_path(kp, sizeof kp, path, key, (int)key_len))
            return at_fail(p, v, path, "key too long");
        de_binding_source src = DE_BIND_KEY;
        if (de__binding_source_from_name(key, key_len, &src)) {
            if (has_source)
                return at_fail(p, v, kp, "a binding has exactly one source (found a second)");
            has_source = true;
            b->source = src;
            if (v->type != TOML_STRING)
                return at_fail(p, v, kp, "expected a name, found %s", at_type_name(v->type));
            if (!de__binding_code_from_name(src, v->u.str.ptr, (size_t)v->u.str.len, &b->code))
                return at_fail(p, v, kp, "unknown %s \"%.*s\"", k_source_names[src],
                               v->u.str.len > 64 ? 64 : v->u.str.len, v->u.str.ptr);
        } else if (at_key_is(t, i, "composite")) {
            if (v->type != TOML_STRING)
                return at_fail(p, v, kp, "expected a string, found %s", at_type_name(v->type));
            if (type == DE_ACTION_BUTTON)
                return at_fail(p, v, kp, "a button action has no composite");
            if (!de__composite_from_name(v->u.str.ptr, (size_t)v->u.str.len, type,
                                         &b->composite_slot))
                return at_fail(p, v, kp, "expected %s, found \"%.*s\"",
                               type == DE_ACTION_AXIS2D ? "\"up\", \"down\", \"left\" or \"right\""
                                                        : "\"positive\" or \"negative\"",
                               v->u.str.len > 64 ? 64 : v->u.str.len, v->u.str.ptr);
        } else if (at_key_is(t, i, "modifier")) {
            if (v->type != TOML_STRING)
                return at_fail(p, v, kp, "expected a key name, found %s", at_type_name(v->type));
            if (!de__binding_code_from_name(DE_BIND_KEY, v->u.str.ptr, (size_t)v->u.str.len,
                                            &b->modifier_key))
                return at_fail(p, v, kp, "unknown key \"%.*s\"",
                               v->u.str.len > 64 ? 64 : v->u.str.len, v->u.str.ptr);
        } else if (at_key_is(t, i, "scale")) {
            DE_TRY(at_f32(p, v, kp, -1.0e6, 1.0e6, &b->scale));
            if (b->scale == 0.0f)
                return at_fail(p, v, kp, "must not be 0");
        } else if (at_key_is(t, i, "deadzone")) {
            DE_TRY(at_f32(p, v, kp, 0.0, 0.99, &b->deadzone));
        } else if (at_key_is(t, i, "invert")) {
            DE_TRY(at_bool(p, v, kp, &b->invert));
        } else if (at_key_is(t, i, "curve")) {
            u32 ct = 0;
            DE_TRY(at_enum(p, v, kp, k_curve_names, 3, &ct));
            b->curve.type = (de_curve_type)ct;
            curve_d = v;
        } else if (at_key_is(t, i, "exponent")) {
            DE_TRY(at_f32(p, v, kp, 0.01, 100.0, &b->curve.exponent));
            has_exponent = true;
        } else if (at_key_is(t, i, "points")) {
            DE_TRY(at_curve_points(p, v, kp, &b->curve));
            has_points = true;
            points_d = v;
        } else {
            at_unknown(p, v, kp);
        }
    }
    if (!has_source)
        return at_fail(p, t, path,
                       "no source (key, mouse_button, mouse_delta, mouse_wheel, pad_button, "
                       "pad_axis or gyro)");
    if (b->curve.type == DE_CURVE_POWER && !has_exponent)
        b->curve.exponent = 2.0f;
    if (b->curve.type == DE_CURVE_CUSTOM && !has_points)
        return at_fail(p, curve_d, path, "a custom curve needs points = [[x, y], ...]");
    if (b->curve.type != DE_CURVE_CUSTOM && has_points)
        return at_fail(p, points_d, path, "points need curve = \"custom\"");
    /* The message prefix; a cut one is still a usable message (the result is only checked so
     * GCC's -Wformat-truncation sees that a cut is expected). */
    char what[AT_PATH_MAX + 96];
    const int wn = t->lineno > 0
                       ? snprintf(what, sizeof what, "%s:%d: %s", p->source, t->lineno, path)
                       : snprintf(what, sizeof what, "%s: %s", p->source, path);
    if (wn < 0)
        what[0] = '\0';
    return de__binding_validate(type, b, what);
}

static de_result at_name(const at_parser *p, const toml_datum_t *d, const char *path,
                         const char *key, int len, char *out) {
    if (len <= 0 || len > DE_ACTION_NAME_MAX)
        return at_fail(p, d, path, "names have 1 to %d bytes", DE_ACTION_NAME_MAX);
    memcpy(out, key, (size_t)len);
    out[len] = '\0';
    return DE_OK;
}

/* [maps.<map>.actions.<action>] */
static de_result at_action(at_parser *p, const toml_datum_t *t, const char *path, de_action_map m,
                           const char *name) {
    if (t->type != TOML_TABLE)
        return at_fail(p, t, path, "expected a table, found %s", at_type_name(t->type));
    de_action_desc d;
    memset(&d, 0, sizeof d);
    d.name = name;
    d.type = DE_ACTION_BUTTON;
    d.interaction = DE_INTERACT_PRESS;
    de_binding bindings[DE_ACTION_MAX_BINDINGS];

    /* The type first: bindings are checked against it, wherever it is in the table. */
    for (int32_t i = 0; i < t->u.tab.size; ++i) {
        if (!at_key_is(t, i, "type"))
            continue;
        char kp[AT_PATH_MAX];
        at_path(kp, sizeof kp, path, "type", 4);
        u32 v = 0;
        DE_TRY(at_enum(p, &t->u.tab.value[i], kp, k_type_names, 3, &v));
        d.type = (de_action_type)v;
    }
    for (int32_t i = 0; i < t->u.tab.size; ++i) {
        const toml_datum_t *v = &t->u.tab.value[i];
        char kp[AT_PATH_MAX];
        if (!at_path(kp, sizeof kp, path, t->u.tab.key[i], t->u.tab.len[i]))
            return at_fail(p, v, path, "key too long");
        if (at_key_is(t, i, "type")) {
            continue;
        } else if (at_key_is(t, i, "interaction")) {
            u32 x = 0;
            DE_TRY(at_enum(p, v, kp, k_interaction_names, 5, &x));
            d.interaction = (de_interaction)x;
        } else if (at_key_is(t, i, "hold_seconds")) {
            DE_TRY(at_f32(p, v, kp, 0.001, 3600.0, &d.hold_seconds));
        } else if (at_key_is(t, i, "tap_seconds")) {
            DE_TRY(at_f32(p, v, kp, 0.001, 3600.0, &d.tap_seconds));
        } else if (at_key_is(t, i, "double_tap_seconds")) {
            DE_TRY(at_f32(p, v, kp, 0.001, 3600.0, &d.double_tap_seconds));
        } else if (at_key_is(t, i, "press_threshold")) {
            DE_TRY(at_f32(p, v, kp, 0.001, 1.0e6, &d.press_threshold));
        } else if (at_key_is(t, i, "rebindable")) {
            DE_TRY(at_bool(p, v, kp, &d.rebindable));
        } else if (at_key_is(t, i, "bindings")) {
            if (v->type != TOML_ARRAY)
                return at_fail(p, v, kp, "expected an array of inline tables, found %s",
                               at_type_name(v->type));
            if (v->u.arr.size > DE_ACTION_MAX_BINDINGS)
                return at_fail(p, v, kp, "%d bindings (max %d)", (int)v->u.arr.size,
                               DE_ACTION_MAX_BINDINGS);
            for (int32_t k = 0; k < v->u.arr.size; ++k) {
                /* Room for "[<int>]" after a full path, so the index is never cut. */
                char bp[AT_PATH_MAX + 16];
                const int bn = snprintf(bp, sizeof bp, "%s[%d]", kp, (int)k);
                if (bn < 0 || (size_t)bn >= sizeof bp)
                    return at_fail(p, v, kp, "key too long");
                const toml_datum_t *bd = &v->u.arr.elem[k];
                DE_TRY(at_binding(p, bd->lineno > 0 ? bd : v, bp, d.type, &bindings[k]));
            }
            d.bindings = bindings;
            d.binding_count = (u32)v->u.arr.size;
        } else {
            at_unknown(p, v, kp);
        }
    }
    if (!p->apply)
        return DE_OK;
    const de_action a = de_action_add(p->in, m, &d);
    if (!DE_HANDLE_VALID(a))
        return DE_FAIL_WRAP(DE_ERR_INVALID_ARGUMENT, "%s:%d: %s: could not add the action",
                            p->source, t->lineno, path);
    return DE_OK;
}

/* [maps.<map>] */
static de_result at_map(at_parser *p, const toml_datum_t *t, const char *path, const char *name) {
    if (t->type != TOML_TABLE)
        return at_fail(p, t, path, "expected a table, found %s", at_type_name(t->type));
    de_action_map m = {0};
    if (p->apply) {
        m = de_action_map_create(p->in, name);
        if (!DE_HANDLE_VALID(m))
            return DE_FAIL_WRAP(DE_ERR_INVALID_ARGUMENT, "%s: %s: could not create the map",
                                p->source, path);
    }
    for (int32_t i = 0; i < t->u.tab.size; ++i) {
        const toml_datum_t *v = &t->u.tab.value[i];
        char kp[AT_PATH_MAX];
        if (!at_path(kp, sizeof kp, path, t->u.tab.key[i], t->u.tab.len[i]))
            return at_fail(p, v, path, "key too long");
        if (!at_key_is(t, i, "actions")) {
            at_unknown(p, v, kp);
            continue;
        }
        if (v->type != TOML_TABLE)
            return at_fail(p, v, kp, "expected a table, found %s", at_type_name(v->type));
        for (int32_t k = 0; k < v->u.tab.size; ++k) {
            const toml_datum_t *av = &v->u.tab.value[k];
            char ap[AT_PATH_MAX];
            if (!at_path(ap, sizeof ap, kp, v->u.tab.key[k], v->u.tab.len[k]))
                return at_fail(p, av, kp, "key too long");
            char aname[DE_ACTION_NAME_MAX + 1];
            DE_TRY(at_name(p, av, ap, v->u.tab.key[k], v->u.tab.len[k], aname));
            DE_TRY(at_action(p, av, ap, m, aname));
        }
    }
    return DE_OK;
}

static de_result parse_top(at_parser *p, const toml_datum_t *top) {
    for (int32_t i = 0; i < top->u.tab.size; ++i) {
        const toml_datum_t *v = &top->u.tab.value[i];
        char path[AT_PATH_MAX];
        if (!at_path(path, sizeof path, "", top->u.tab.key[i], top->u.tab.len[i]))
            return at_fail(p, v, "(top level)", "key too long");
        if (!at_key_is(top, i, "maps")) {
            at_unknown(p, v, path);
            continue;
        }
        if (v->type != TOML_TABLE)
            return at_fail(p, v, path, "expected a table, found %s", at_type_name(v->type));
        for (int32_t k = 0; k < v->u.tab.size; ++k) {
            const toml_datum_t *mv = &v->u.tab.value[k];
            char mp[AT_PATH_MAX];
            if (!at_path(mp, sizeof mp, path, v->u.tab.key[k], v->u.tab.len[k]))
                return at_fail(p, mv, path, "key too long");
            char mname[DE_ACTION_NAME_MAX + 1];
            DE_TRY(at_name(p, mv, mp, v->u.tab.key[k], v->u.tab.len[k], mname));
            DE_TRY(at_map(p, mv, mp, mname));
        }
    }
    return DE_OK;
}

de_result de__action_maps_parse(de_input *in, const char *text, size_t len, const char *source,
                                u32 *unknown_keys) {
    if (unknown_keys != NULL)
        *unknown_keys = 0;
    if (in == NULL || (text == NULL && len != 0))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_action_maps_load: NULL input or text");
    if (len > (size_t)INT_MAX)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_action_maps_load: %zu bytes is too long", len);
    at_parser p;
    memset(&p, 0, sizeof p);
    p.in = in;
    p.source = source != NULL ? source : "actions.toml";
    p.log = in->log;

    toml_result_t res = de__toml_parse(&in->alloc, text != NULL ? text : "", (int)len);
    if (!res.ok) {
        const de_result r = DE_FAIL(DE_ERR_PARSE, "%s: %s", p.source, res.errmsg);
        de__toml_free(res);
        return r;
    }
    de_result r = parse_top(&p, &res.toptab);
    if (r == DE_OK) {
        p.apply = true;
        r = parse_top(&p, &res.toptab);
    }
    de__toml_free(res);
    if (unknown_keys != NULL)
        *unknown_keys = p.unknown_keys;
    return r;
}

de_result de_action_maps_load(de_input *in, const char *vpath) {
    if (in == NULL || vpath == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_action_maps_load: NULL %s",
                       in == NULL ? "input state" : "path");
    void *data = NULL;
    u64 size = 0;
    const de_result rr = de_file_read_all(vpath, &in->alloc, &data, &size);
    if (rr != DE_OK)
        return DE_FAIL_WRAP(rr, "de_action_maps_load: reading %s", vpath);
    de_result r;
    if (size > (u64)INT_MAX)
        r = DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s: %" PRIu64 " bytes is too large", vpath, size);
    else
        r = de__action_maps_parse(in, (const char *)data, (size_t)size, vpath, NULL);
    DE_FREE(&in->alloc, data, (size_t)size + 1);
    return r;
}
