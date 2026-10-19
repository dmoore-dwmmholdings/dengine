#include "text/loc/loc_internal.h"
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void de__loc_write(de__loc_writer *w, const char *s, size_t n) {
    if (w->out && w->capacity && w->needed < w->capacity - 1) {
        size_t room = w->capacity - 1 - (size_t)w->needed, take = n < room ? n : room;
        memcpy(w->out + (size_t)w->needed, s, take);
    }
    w->needed = n > UINT64_MAX - w->needed ? UINT64_MAX : w->needed + n;
}
u32 de__loc_finish(de__loc_writer *w) {
    if (w->out && w->capacity) {
        size_t end = w->needed < w->capacity ? w->needed : w->capacity - 1;
        /* If truncated mid-codepoint, remove its incomplete prefix. */
        if (w->needed >= w->capacity && end) {
            size_t start = end;
            while (start && ((u8)w->out[start - 1] & 0xc0) == 0x80)
                --start;
            if (start < end) {
                size_t lead = start ? start - 1 : 0;
                u8 c = (u8)w->out[lead];
                u32 width = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
                if (end - lead < width)
                    end = lead;
            } else if ((u8)w->out[end - 1] >= 0xc0)
                --end;
        }
        w->out[end] = 0;
    }
    return w->needed > UINT32_MAX ? UINT32_MAX : (u32)w->needed;
}
typedef struct parser {
    const char *begin, *end, *error, *locale;
    const de_loc_arg *args;
    u32 count;
    bool validate;
} parser;
static void spaces(parser *p, const char **at) {
    while (*at < p->end && isspace((unsigned char)**at))
        ++*at;
}
static bool token(const char *s, size_t n, const char *expect) {
    return n == strlen(expect) && memcmp(s, expect, n) == 0;
}
static const de_loc_arg *argument(parser *p, const char *s, size_t n) {
    for (u32 i = 0; i < p->count; ++i)
        if (p->args[i].name && strlen(p->args[i].name) == n && !memcmp(p->args[i].name, s, n))
            return &p->args[i];
    return NULL;
}
static f64 numeric(const de_loc_arg *a) {
    return !a ? 0 : a->kind == DE_LOC_ARG_INT ? (f64)a->i : a->kind == DE_LOC_ARG_FLOAT ? a->f : 0;
}
static void value(parser *p, de__loc_writer *w, const de_loc_arg *arg, const char *kind,
                  const char *style) {
    if (!w || !arg)
        return;
    char text[768];
    u32 n = 0;
    if (!kind) {
        if (arg->kind == DE_LOC_ARG_STR) {
            if (arg->s)
                de__loc_write(w, arg->s, strlen(arg->s));
            return;
        }
        if (arg->kind == DE_LOC_ARG_INT) {
            int length = snprintf(text, sizeof text, "%lld", (long long)arg->i);
            if (length > 0)
                de__loc_write(w, text, (size_t)length);
            return;
        }
    }
    if (kind && strcmp(kind, "date") == 0) {
        f64 number = numeric(arg);
        if (!isfinite(number) || number < -62135596800.0 || number > 253402300799.0)
            return;
        n = de__loc_date(p->locale, (i64)number, style, text, sizeof text);
    } else
        n = de__loc_number(p->locale, numeric(arg), style, text, sizeof text);
    if (n >= sizeof text)
        n = sizeof text - 1;
    de__loc_write(w, text, n);
}
static bool sequence(parser *p, const char **at, bool nested, u32 depth, de__loc_writer *w,
                     bool plural, f64 count);
static bool placeholder(parser *p, const char **at, u32 depth, de__loc_writer *w,
                        bool parent_plural, f64 parent_count) {
    const char *start = *at;
    spaces(p, at);
    const char *name = *at;
    while (*at < p->end && **at != ',' && **at != '}' && !isspace((unsigned char)**at))
        ++*at;
    size_t name_length = (size_t)(*at - name);
    if (!name_length)
        return false;
    const de_loc_arg *arg = argument(p, name, name_length);
    spaces(p, at);
    if (*at == p->end)
        return false;
    if (**at == '}') {
        ++*at;
        value(p, w, arg, NULL, NULL);
        return true;
    }
    if (**at != ',')
        return false;
    ++*at;
    spaces(p, at);
    const char *kind = *at;
    while (*at < p->end && **at != ',' && **at != '}' && !isspace((unsigned char)**at))
        ++*at;
    size_t kind_length = (size_t)(*at - kind);
    spaces(p, at);
    bool is_plural = token(kind, kind_length, "plural"),
         is_select = token(kind, kind_length, "select");
    if (is_plural || is_select) {
        if (*at == p->end || **at != ',')
            return false;
        ++*at;
        const char *chosen = NULL, *chosen_end = NULL;
        int best = -1;
        bool other = false;
        f64 number = numeric(arg);
        const char *category = de__loc_plural_name(de__loc_plural_number(p->locale, number));
        for (;;) {
            spaces(p, at);
            if (*at == p->end)
                return false;
            if (**at == '}') {
                ++*at;
                break;
            }
            const char *selector = *at;
            while (*at < p->end && **at != '{' && !isspace((unsigned char)**at))
                ++*at;
            size_t length = (size_t)(*at - selector);
            spaces(p, at);
            if (!length || *at == p->end || **at != '{')
                return false;
            ++*at;
            int match = -1;
            if (token(selector, length, "other")) {
                other = true;
                match = 0;
            } else if (is_plural && selector[0] == '=') {
                if (length >= 64)
                    return false;
                char exact[64];
                memcpy(exact, selector + 1, length - 1);
                exact[length - 1] = 0;
                char *end = NULL;
                f64 test = strtod(exact, &end);
                if (end == exact || *end || !isfinite(test))
                    return false;
                if (test == number)
                    match = 2;
            } else if (is_plural) {
                bool valid = false;
                for (u32 i = 0; i < 6; ++i)
                    if (token(selector, length, de__loc_plural_name((de__plural)i)))
                        valid = true;
                if (!valid)
                    return false;
                if (token(selector, length, category))
                    match = 1;
            } else if (arg && arg->kind == DE_LOC_ARG_STR && arg->s &&
                       token(selector, length, arg->s))
                match = 1;
            const char *body = *at;
            if (!sequence(p, at, true, depth + 1, NULL, is_plural ? true : parent_plural,
                          is_plural ? number : parent_count))
                return false;
            if (match > best) {
                chosen = body;
                chosen_end = *at - 1;
                best = match;
            }
        }
        if (!other)
            return false;
        if (w && chosen) {
            parser sub = *p;
            sub.end = chosen_end;
            const char *body = chosen;
            if (!sequence(&sub, &body, false, depth + 1, w, is_plural ? true : parent_plural,
                          is_plural ? number : parent_count))
                return false;
        }
        return true;
    }
    if (!token(kind, kind_length, "number") && !token(kind, kind_length, "date"))
        return false;
    char style[24] = {0};
    if (*at < p->end && **at == ',') {
        ++*at;
        spaces(p, at);
        const char *s = *at;
        while (*at < p->end && **at != '}')
            ++*at;
        size_t n = (size_t)(*at - s);
        while (n && isspace((unsigned char)s[n - 1]))
            --n;
        if (n >= sizeof style)
            return false;
        memcpy(style, s, n);
    }
    if (*at == p->end || **at != '}')
        return false;
    ++*at;
    bool date = token(kind, kind_length, "date");
    if (style[0] && !(date ? (strcmp(style, "short") == 0 || strcmp(style, "medium") == 0)
                           : (strcmp(style, "integer") == 0 || strcmp(style, "percent") == 0)))
        return false;
    value(p, w, arg, date ? "date" : "number", style);
    (void)start;
    return true;
}
static bool sequence(parser *p, const char **at, bool nested, u32 depth, de__loc_writer *w,
                     bool plural, f64 count) {
    if (depth > 32) {
        p->error = *at;
        return false;
    }
    while (*at < p->end) {
        char c = **at;
        if (c == '}') {
            if (nested) {
                ++*at;
                return true;
            }
            p->error = *at;
            return false;
        }
        if (c == '{') {
            ++*at;
            if (!placeholder(p, at, depth, w, plural, count)) {
                p->error = *at;
                return false;
            }
            continue;
        }
        if (c == '\'') {
            ++*at;
            if (*at < p->end && **at == '\'') {
                if (w)
                    de__loc_write(w, "'", 1);
                ++*at;
                continue;
            }
            if (*at == p->end || (**at != '{' && **at != '}' && **at != '#')) {
                if (w)
                    de__loc_write(w, "'", 1);
                continue;
            }
            while (*at < p->end) {
                if (**at == '\'') {
                    ++*at;
                    if (*at < p->end && **at == '\'') {
                        if (w)
                            de__loc_write(w, "'", 1);
                        ++*at;
                        continue;
                    }
                    break;
                }
                if (w)
                    de__loc_write(w, *at, 1);
                ++*at;
            }
            continue;
        }
        if (c == '#' && plural && w) {
            char number[768];
            u32 n = de__loc_number(p->locale, count, NULL, number, sizeof number);
            if (n >= sizeof number)
                n = sizeof number - 1;
            de__loc_write(w, number, n);
        } else if (w)
            de__loc_write(w, *at, 1);
        ++*at;
    }
    if (nested) {
        p->error = *at;
        return false;
    }
    return true;
}
de_result de__loc_validate(const char *pattern, size_t bytes, size_t *error_offset) {
    if (!pattern && bytes)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "NULL message pattern");
    if (!pattern)
        pattern = "";
    parser p = {pattern, pattern + bytes, NULL, "en", NULL, 0, true};
    const char *at = pattern;
    bool valid = sequence(&p, &at, false, 0, NULL, false, 0);
    if (error_offset)
        *error_offset = valid ? 0 : (size_t)((p.error ? p.error : at) - pattern);
    return valid ? DE_OK
                 : DE_FAIL(DE_ERR_PARSE, "invalid localization message at byte %zu",
                           (size_t)((p.error ? p.error : at) - pattern));
}
de_result de__loc_message(const char *pattern, size_t bytes, const char *locale,
                          const de_loc_arg *args, u32 count, char *out, u32 capacity, u32 *needed) {
    if (needed)
        *needed = 0;
    if (out && capacity)
        out[0] = 0;
    if ((!pattern && bytes) || (!args && count))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "invalid message arguments");
    if (!pattern)
        pattern = "";
    parser p = {pattern, pattern + bytes, NULL, locale ? locale : "en", args, count, false};
    const char *at = pattern;
    de__loc_writer w = {out, capacity, 0};
    if (!sequence(&p, &at, false, 0, &w, false, 0)) {
        if (out && capacity)
            out[0] = 0;
        return DE_FAIL(DE_ERR_PARSE, "invalid localization message");
    }
    u32 n = de__loc_finish(&w);
    if (needed)
        *needed = n;
    return DE_OK;
}
