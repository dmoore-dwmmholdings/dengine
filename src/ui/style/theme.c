#include "stylesheet_internal.h"
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
static de_result bad(toml_datum_t d, const char *message) {
    return DE_FAIL(DE_ERR_PARSE, "UI stylesheet line %d: %s", d.lineno > 0 ? d.lineno : 1, message);
}
static bool number(toml_datum_t d, f32 *value) {
    if (d.type != TOML_INT64 && d.type != TOML_FP64)
        return false;
    double n = d.type == TOML_INT64 ? (double)d.u.int64 : d.u.fp64;
    if (!isfinite(n) || fabs(n) > 1000000)
        return false;
    *value = (f32)n;
    return true;
}
static int hex(char c) {
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                  : -1;
}
static f32 linear(f32 c) {
    return c <= .04045f ? c / 12.92f : powf((c + .055f) / 1.055f, 2.4f);
}
static bool color(const char *s, f32 out[4]) {
    size_t n = strlen(s);
    if (s[0] != '#' || (n != 4 && n != 7 && n != 9))
        return false;
    out[3] = 1;
    u32 channels = n == 9 ? 4 : 3;
    for (u32 i = 0; i < channels; ++i) {
        int a = hex(s[1 + (n == 4 ? i : 2 * i)]), b = n == 4 ? a : hex(s[2 + 2 * i]);
        if (a < 0 || b < 0)
            return false;
        out[i] = (f32)(a * 16 + b) / 255.f;
        if (i < 3)
            out[i] = linear(out[i]);
    }
    return true;
}
static bool length(toml_datum_t d, f32 *value, de_ui_unit *unit) {
    *unit = DE_UI_PX;
    if (number(d, value))
        return true;
    if (d.type != TOML_STRING)
        return false;
    if (!strcmp(d.u.s, "auto")) {
        *value = 0;
        *unit = DE_UI_AUTO;
        return true;
    }
    char *end = NULL;
    errno = 0;
    float n = strtof(d.u.s, &end);
    if (end == d.u.s || errno || !isfinite(n) || fabsf(n) > 1000000)
        return false;
    if (!strcmp(end, "%"))
        *unit = DE_UI_PERCENT;
    else if (strcmp(end, "px") && *end)
        return false;
    *value = n;
    return true;
}
static toml_datum_t variable(de_ui_style_system *state, de_ui_sheet_document *sheet,
                             const char *name) {
    toml_datum_t root = sheet->document.toptab;
    if (state && state->theme.hash) {
        toml_datum_t themes = toml_get(root, "theme");
        if (themes.type == TOML_TABLE)
            for (int i = 0; i < themes.u.tab.size; ++i)
                if (de_name_from(themes.u.tab.key[i]).hash == state->theme.hash) {
                    toml_datum_t found = toml_get(toml_get(themes.u.tab.value[i], "vars"), name);
                    if (found.type != TOML_UNKNOWN)
                        return found;
                }
    }
    return toml_get(toml_get(root, "vars"), name);
}
de_result de_ui__style_value(de_ui_style_system *state, de_ui_sheet_document *sheet,
                             const de_ui_style_property *property, toml_datum_t d,
                             de_ui_style_value *out) {
    memset(out, 0, sizeof *out);
    for (u32 depth = 0; d.type == TOML_STRING && d.u.s[0] == '$'; ++depth) {
        if (depth >= 8)
            return bad(d, "variable cycle or excessive indirection");
        toml_datum_t replacement = variable(state, sheet, d.u.s + 1);
        if (replacement.type == TOML_UNKNOWN)
            return bad(d, "unknown theme variable");
        d = replacement;
    }
    switch (property->kind) {
    case DE_UI_PROP_FLOAT:
        if (!number(d, &out->number[0]))
            return bad(d, "expected finite numeric property");
        break;
    case DE_UI_PROP_COLOR:
        if (d.type != TOML_STRING || !color(d.u.s, out->number))
            return bad(d, "expected #rgb, #rrggbb or #rrggbbaa color");
        break;
    case DE_UI_PROP_LENGTH:
        if (!length(d, &out->number[0], &out->unit))
            return bad(d, "expected pixel, percent or auto length");
        break;
    case DE_UI_PROP_ASSET:
        if (d.type != TOML_STRING || !d.u.s[0])
            return bad(d, "expected asset ID string");
        out->asset = de_asset_id_from(d.u.s);
        if (!out->asset.hash)
            return bad(d, "invalid asset ID");
        break;
    case DE_UI_PROP_VEC4:
        if (d.type != TOML_ARRAY || d.u.arr.size != 4)
            return bad(d, "expected four numeric borders");
        for (u32 i = 0; i < 4; ++i)
            if (!number(d.u.arr.elem[i], &out->number[i]))
                return bad(d, "invalid border");
        break;
    case DE_UI_PROP_EDGES:
        if (d.type != TOML_ARRAY) {
            if (!length(d, &out->number[0], &out->unit) || out->unit != DE_UI_PX)
                return bad(d, "spacing must use pixels");
            for (u32 i = 1; i < 4; ++i)
                out->number[i] = out->number[0];
        } else {
            int n = d.u.arr.size;
            if (n != 1 && n != 2 && n != 4)
                return bad(d, "spacing accepts one, two or four values");
            for (u32 i = 0; i < 4; ++i) {
                int j = n == 1 ? 0 : n == 2 ? (int)(i % 2) : (int)i;
                de_ui_unit unit;
                if (!length(d.u.arr.elem[j], &out->number[i], &unit) || unit != DE_UI_PX)
                    return bad(d, "spacing must use pixels");
            }
        }
        break;
    default:
        return bad(d, "unsupported property kind");
    }
    return DE_OK;
}
de_result de_ui_set_theme(de_ui *ui, const char *name) {
    if (!ui || !ui->style)
        return DE_ERR_INVALID_ARGUMENT;
    de_name theme = de_name_from(name ? name : "");
    if (theme.hash != ui->style->theme.hash) {
        de_name previous = ui->style->theme;
        ui->style->theme = theme;
        for (u32 i = 0; i < ui->style->sheet_count; ++i) {
            de_result result = de_ui__compile_sheet(ui->style, &ui->style->sheets[i]);
            if (result != DE_OK) {
                ui->style->theme = previous;
                for (u32 j = 0; j < ui->style->sheet_count; ++j)
                    (void)de_ui__compile_sheet(ui->style, &ui->style->sheets[j]);
                return result;
            }
        }
        ++ui->style->revision;
    }
    return DE_OK;
}
static de_result easing_value(toml_datum_t d, de_ui_easing *out) {
    static const char *const names[] = {"linear",       "quad-in",  "quad-out",
                                        "quad-in-out",  "cubic-in", "cubic-out",
                                        "cubic-in-out", "back-out", "elastic-out"};
    if (d.type != TOML_STRING)
        return bad(d, "expected easing name");
    for (u32 i = 0; i < DE_UI_STYLE_COUNT(names); ++i)
        if (!strcmp(d.u.s, names[i])) {
            *out = (de_ui_easing)i;
            return DE_OK;
        }
    return bad(d, "unknown easing");
}
de_result de_ui__compile_sheet(de_ui_style_system *state, de_ui_style_sheet_slot *slot) {
    de_ui_sheet_document *sheet = slot->sheet;
    for (u32 i = 0; i < sheet->rule_count; ++i) {
        de_ui_compiled_rule *rule = &slot->compiled[i];
        memset(rule, 0, sizeof *rule);
        toml_datum_t props = sheet->rules[i].properties;
        for (u32 p = 0; p < de_ui__style_property_count; ++p) {
            toml_datum_t d = toml_get(props, de_ui__style_properties[p].name);
            if (d.type == TOML_UNKNOWN)
                continue;
            DE_TRY(
                de_ui__style_value(state, sheet, &de_ui__style_properties[p], d, &rule->values[p]));
            rule->mask |= 1u << p;
        }
        toml_datum_t transition = toml_get(props, "transition");
        if (transition.type != TOML_UNKNOWN &&
            (!number(transition, &rule->transition) || rule->transition < 0))
            return bad(transition, "transition must be nonnegative seconds");
        toml_datum_t easing = toml_get(props, "easing");
        if (easing.type != TOML_UNKNOWN)
            DE_TRY(easing_value(easing, &rule->easing));
    }
    return DE_OK;
}
