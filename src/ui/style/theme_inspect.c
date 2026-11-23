/* DENG-781: theme listing, theme variables and the editor theme preview. Main thread. */
#include "stylesheet_internal.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static toml_datum_t sheet_themes(const de_ui_style_sheet_slot *slot) {
    return toml_get(slot->sheet->document.toptab, "theme");
}
bool de_ui__theme_declared(const de_ui_style_system *state, const char *name) {
    for (u32 i = 0; i < state->sheet_count; ++i) {
        toml_datum_t themes = sheet_themes(&state->sheets[i]);
        if (themes.type == TOML_TABLE && toml_get(themes, name).type == TOML_TABLE)
            return true;
    }
    return false;
}

de_result de_ui_list_themes(const de_ui *ui, de_ui_theme_info *out, u32 capacity, u32 *count) {
    if (!ui || !ui->style || !count || (!out && capacity))
        return DE_ERR_INVALID_ARGUMENT;
    const de_ui_style_system *state = ui->style;
    /* Selection by order: no allocation, and the number of themes is small. */
    const char *previous = NULL;
    u32 n = 0;
    for (;;) {
        const char *best = NULL;
        u32 sheets = 0;
        for (u32 s = 0; s < state->sheet_count; ++s) {
            toml_datum_t themes = sheet_themes(&state->sheets[s]);
            if (themes.type != TOML_TABLE)
                continue;
            for (int k = 0; k < themes.u.tab.size; ++k) {
                const char *key = themes.u.tab.key[k];
                if (themes.u.tab.value[k].type != TOML_TABLE ||
                    (size_t)themes.u.tab.len[k] != strlen(key) ||
                    strlen(key) >= DE_UI_THEME_NAME_MAX || !key[0])
                    continue;
                if (previous && strcmp(key, previous) <= 0)
                    continue;
                int order = best ? strcmp(key, best) : -1;
                if (order < 0) {
                    best = key;
                    sheets = 1;
                } else if (order == 0)
                    ++sheets;
            }
        }
        if (!best)
            break;
        if (n < capacity) {
            memset(&out[n], 0, sizeof out[n]);
            memcpy(out[n].name, best, strlen(best) + 1);
            out[n].sheets = sheets;
        }
        ++n;
        previous = best;
    }
    *count = n;
    return DE_OK;
}

/* Bounded text writer; a cut never splits a UTF-8 sequence. */
typedef struct value_writer {
    char *out;
    u32 capacity, length;
    bool truncated;
} value_writer;
static void put(value_writer *w, const char *s, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (w->length + 1 >= w->capacity) {
            w->truncated = true;
            return;
        }
        w->out[w->length++] = s[i];
    }
}
static void put_str(value_writer *w, const char *s) {
    put(w, s, strlen(s));
}
static void finish(value_writer *w) {
    if (w->truncated && w->length) {
        u32 start = w->length;
        while (start > 0 && ((u8)w->out[start - 1] & 0xC0) == 0x80)
            --start;
        if (start > 0) {
            u8 lead = (u8)w->out[start - 1];
            u32 need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
            if (w->length - (start - 1) < need)
                w->length = start - 1;
        }
    }
    w->out[w->length] = 0;
}
static void put_quoted(value_writer *w, const char *s, size_t n) {
    put(w, "\"", 1);
    for (size_t i = 0; i < n; ++i) {
        u8 c = (u8)s[i];
        char escape[8];
        if (c == '"' || c == '\\') {
            escape[0] = '\\';
            escape[1] = (char)c;
            put(w, escape, 2);
        } else if (c == '\n')
            put(w, "\\n", 2);
        else if (c == '\t')
            put(w, "\\t", 2);
        else if (c == '\r')
            put(w, "\\r", 2);
        else if (c < 0x20 || c == 0x7F) {
            snprintf(escape, sizeof escape, "\\u%04X", c);
            put_str(w, escape);
        } else
            put(w, (const char *)&s[i], 1);
    }
    put(w, "\"", 1);
}
static bool bare_key(const char *s, size_t n) {
    if (!n)
        return false;
    for (size_t i = 0; i < n; ++i) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-'))
            return false;
    }
    return true;
}
static void put_float(value_writer *w, double v) {
    char text[40];
    if (isnan(v)) {
        put_str(w, "nan");
        return;
    }
    if (isinf(v)) {
        put_str(w, v < 0 ? "-inf" : "inf");
        return;
    }
    snprintf(text, sizeof text, "%.15g", v);
    if (strtod(text, NULL) != v)
        snprintf(text, sizeof text, "%.17g", v);
    if (!strpbrk(text, ".eE")) {
        const size_t len = strlen(text);
        snprintf(text + len, sizeof text - len, ".0");
    }
    put_str(w, text);
}
static void put_datum(value_writer *w, const toml_datum_t *d, u32 depth) {
    char text[48];
    if (depth > 8) {
        put_str(w, "...");
        w->truncated = true;
        return;
    }
    switch (d->type) {
    case TOML_STRING:
        put_quoted(w, d->u.str.ptr, (size_t)d->u.str.len);
        break;
    case TOML_INT64:
        snprintf(text, sizeof text, "%" PRId64, d->u.int64);
        put_str(w, text);
        break;
    case TOML_FP64:
        put_float(w, d->u.fp64);
        break;
    case TOML_BOOLEAN:
        put_str(w, d->u.boolean ? "true" : "false");
        break;
    case TOML_DATE:
    case TOML_DATETIME:
    case TOML_DATETIMETZ:
        snprintf(text, sizeof text, "%04d-%02d-%02d", d->u.ts.year, d->u.ts.month, d->u.ts.day);
        put_str(w, text);
        if (d->type == TOML_DATE)
            break;
        put_str(w, "T");
        /* fall through */
    case TOML_TIME:
        snprintf(text, sizeof text, "%02d:%02d:%02d", d->u.ts.hour, d->u.ts.minute, d->u.ts.second);
        put_str(w, text);
        if (d->u.ts.usec) {
            snprintf(text, sizeof text, ".%06d", (int)d->u.ts.usec);
            put_str(w, text);
        }
        if (d->type == TOML_DATETIMETZ) {
            int tz = d->u.ts.tz;
            if (!tz)
                put_str(w, "Z");
            else {
                snprintf(text, sizeof text, "%c%02d:%02d", tz < 0 ? '-' : '+', abs(tz) / 60,
                         abs(tz) % 60);
                put_str(w, text);
            }
        }
        break;
    case TOML_ARRAY:
        put_str(w, "[");
        for (int i = 0; i < d->u.arr.size; ++i) {
            if (i)
                put_str(w, ", ");
            put_datum(w, &d->u.arr.elem[i], depth + 1);
        }
        put_str(w, "]");
        break;
    case TOML_TABLE:
        put_str(w, "{");
        for (int i = 0; i < d->u.tab.size; ++i) {
            put_str(w, i ? ", " : " ");
            const char *key = d->u.tab.key[i];
            size_t n = (size_t)d->u.tab.len[i];
            if (bare_key(key, n))
                put(w, key, n);
            else
                put_quoted(w, key, n);
            put_str(w, " = ");
            put_datum(w, &d->u.tab.value[i], depth + 1);
        }
        put_str(w, d->u.tab.size ? " }" : "}");
        break;
    default:
        put_str(w, "\"\"");
        w->truncated = true;
        break;
    }
}
static void emit(de_ui_theme_var *out, u32 capacity, u32 *n, u32 sheet, const char *key,
                 size_t key_length, const toml_datum_t *value, bool themed) {
    if (*n < capacity) {
        de_ui_theme_var *v = &out[*n];
        memset(v, 0, sizeof *v);
        value_writer name = {v->name, DE_UI_THEME_NAME_MAX, 0, false};
        put(&name, key, key_length);
        finish(&name);
        value_writer text = {v->value, DE_UI_THEME_VALUE_MAX, 0, false};
        put_datum(&text, value, 0);
        finish(&text);
        v->sheet = sheet;
        v->themed = themed;
        v->truncated = name.truncated || text.truncated;
    }
    ++*n;
}
de_result de_ui_theme_vars(const de_ui *ui, const char *theme, de_ui_theme_var *out, u32 capacity,
                           u32 *count) {
    if (!ui || !ui->style || !count || (!out && capacity))
        return DE_ERR_INVALID_ARGUMENT;
    const de_ui_style_system *state = ui->style;
    if (theme && theme[0] && !de_ui__theme_declared(state, theme))
        return DE_FAIL(DE_ERR_NOT_FOUND, "UI theme '%.64s' is not in a loaded stylesheet", theme);
    const char *name = theme ? theme : state->theme_active;
    u32 n = 0;
    for (u32 s = 0; s < state->sheet_count; ++s) {
        toml_datum_t root = state->sheets[s].sheet->document.toptab;
        toml_datum_t base = toml_get(root, "vars");
        toml_datum_t overlay = {0};
        if (name[0])
            overlay = toml_get(toml_get(toml_get(root, "theme"), name), "vars");
        if (base.type == TOML_TABLE)
            for (int k = 0; k < base.u.tab.size; ++k) {
                const char *key = base.u.tab.key[k];
                toml_datum_t themed =
                    overlay.type == TOML_TABLE ? toml_get(overlay, key) : (toml_datum_t){0};
                bool from_theme = themed.type != TOML_UNKNOWN;
                emit(out, capacity, &n, s, key, (size_t)base.u.tab.len[k],
                     from_theme ? &themed : &base.u.tab.value[k], from_theme);
            }
        if (overlay.type == TOML_TABLE)
            for (int k = 0; k < overlay.u.tab.size; ++k) {
                const char *key = overlay.u.tab.key[k];
                if (base.type == TOML_TABLE && toml_get(base, key).type != TOML_UNKNOWN)
                    continue;
                emit(out, capacity, &n, s, key, (size_t)overlay.u.tab.len[k],
                     &overlay.u.tab.value[k], true);
            }
    }
    *count = n;
    return DE_OK;
}

de_result de_ui_preview_theme(de_ui *ui, const char *name) {
    if (!ui || !ui->style)
        return DE_ERR_INVALID_ARGUMENT;
    de_ui_style_system *state = ui->style;
    if (!name) {
        if (!state->theme_previewing)
            return DE_OK;
        /* On an error the preview stays, so the state still tells the truth. */
        DE_TRY(de_ui__apply_theme(state, state->theme_selected));
        state->theme_previewing = false;
        return DE_OK;
    }
    if (strlen(name) >= DE_UI_THEME_NAME_MAX)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "UI theme name longer than %u bytes",
                       DE_UI_THEME_NAME_MAX - 1);
    if (name[0] && !de_ui__theme_declared(state, name))
        return DE_FAIL(DE_ERR_NOT_FOUND, "UI theme '%s' is not in a loaded stylesheet", name);
    DE_TRY(de_ui__apply_theme(state, name));
    state->theme_previewing = true;
    return DE_OK;
}
de_result de_ui_get_theme_state(const de_ui *ui, de_ui_theme_state *out) {
    if (!ui || !ui->style || !out)
        return DE_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof *out);
    memcpy(out->active, ui->style->theme_active, sizeof out->active);
    memcpy(out->selected, ui->style->theme_selected, sizeof out->selected);
    out->previewing = ui->style->theme_previewing;
    return DE_OK;
}
