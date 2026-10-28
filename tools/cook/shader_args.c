#include "shader_cmd.h"
#include "asset/diagnostic_internal.h"
#include <stdio.h>
#include <string.h>

static bool invalid(char error[256], const char *message) {
    snprintf(error, 256, "%s", message);
    return false;
}
bool de_cook_shader_parse(int argc, const char *const *argv, de_cook_shader_options *o,
                          char error[256]) {
    if (!o || !error)
        return false;
    memset(o, 0, sizeof *o);
    error[0] = '\0';
    if (argc < 0 || argc > 256 || (argc && !argv))
        return invalid(error, "invalid argument list");
    for (int i = 0; i < argc; ++i) {
        const char *arg = argv[i];
        if (!arg || strlen(arg) > 4095)
            return invalid(error, "invalid or oversized argument");
        if (!strcmp(arg, "--help") || !strcmp(arg, "-h")) {
            o->help = true;
            continue;
        }
        if (arg[0] != '-') {
            if (o->source)
                return invalid(error, "only one source is accepted");
            o->source = arg;
            continue;
        }
        const char **slot = NULL;
        const char *value = NULL;
        if (!strcmp(arg, "--target"))
            slot = &o->target;
        else if (!strcmp(arg, "--code"))
            slot = &o->code;
        else if (!strcmp(arg, "--reflection"))
            slot = &o->reflection;
        else if (!strcmp(arg, "--report"))
            slot = &o->report;
        else if (!strncmp(arg, "-I", 2)) {
            if (o->include_count == DE_COOK_SHADER_MAX_ARGS)
                return invalid(error, "too many include roots");
            slot = &o->includes[o->include_count++];
            value = arg + 2;
        } else if (!strncmp(arg, "-D", 2)) {
            if (o->define_count == DE_COOK_SHADER_MAX_ARGS)
                return invalid(error, "too many defines");
            slot = &o->defines[o->define_count++];
            value = arg + 2;
        } else if (!strcmp(arg, "--entry")) {
            if (o->entry_count == DE_COOK_SHADER_MAX_ARGS)
                return invalid(error, "too many entry points");
            slot = &o->entries[o->entry_count++];
        } else
            return invalid(error, "unknown shader option");
        if (*slot)
            return invalid(error, "duplicate output or target option");
        if (!value || !*value) {
            if (++i >= argc || !argv[i] || !*argv[i] || argv[i][0] == '-' || strlen(argv[i]) > 4095)
                return invalid(error, "option requires a value");
            value = argv[i];
        }
        *slot = value;
    }
    if (o->help)
        return true;
    if (!o->source || !*o->source || !o->target || !o->code || !o->reflection || !o->report)
        return invalid(error, "source, target, code, reflection and report paths are required");
    if (strcmp(o->target, "spirv") && strcmp(o->target, "dxil") && strcmp(o->target, "metal"))
        return invalid(error, "target must be spirv, dxil or metal");
    if (!strcmp(o->code, o->reflection) || !strcmp(o->code, o->report) ||
        !strcmp(o->reflection, o->report))
        return invalid(error, "output paths must be distinct and must not exist");
    for (u32 i = 0; i < o->define_count; ++i) {
        const char *p = o->defines[i];
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '_'))
            return invalid(error, "invalid define name");
        for (++p; *p && *p != '='; ++p)
            if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                  (*p >= '0' && *p <= '9') || *p == '_'))
                return invalid(error, "invalid define name");
    }
    return true;
}

static const char *severity(const char *p, const char **message) {
    static const char *const names[] = {"fatal error", "error", "warning", "note"};
    for (u32 i = 0; i < 4; ++i) {
        const size_t n = strlen(names[i]);
        if (!strncmp(p, names[i], n) && (p[n] == ':' || p[n] == '[' || p[n] == ' ' || !p[n])) {
            *message = p;
            return i < 2 ? "error" : names[i];
        }
    }
    return NULL;
}
/* Slang's rich form puts the position and underline after its diagnostic heading. Never
 * borrow a location from a later diagnostic. Classic file(line,column) remains supported. */
static void rich_location(const char *text, de_cook_shader_diagnostic *out) {
    const char *next = strchr(text, '\n');
    while (next && *++next) {
        const char *p = next;
        while (*p == ' ' || *p == '\t')
            ++p;
        const char *unused = NULL;
        if (severity(p, &unused))
            return;
        if (!out->line && !strncmp(p, "-->", 3)) {
            p += 3;
            while (*p == ' ' || *p == '\t')
                ++p;
            for (const char *colon = p; *colon && *colon != '\n'; ++colon) {
                if (*colon != ':')
                    continue;
                const char *q = colon + 1;
                u32 row, column;
                if (!de__asset_diag_number(&q, &row) || *q++ != ':' ||
                    !de__asset_diag_number(&q, &column))
                    continue;
                if (*q && *q != '\r' && *q != '\n')
                    continue;
                const size_t n = (size_t)(colon - p);
                if (!n || n >= sizeof out->source)
                    continue;
                memcpy(out->source, p, n);
                out->source[n] = '\0';
                out->line = row;
                out->column = column;
                break;
            }
        } else if (out->column && *p == '|') {
            ++p;
            while (*p == ' ' || *p == '\t')
                ++p;
            if (*p == '^') {
                u32 width = 0;
                while (*p == '^' || *p == '~') {
                    ++width;
                    ++p;
                }
                if (width <= UINT32_MAX - out->column)
                    out->end_column = out->column + width;
                return;
            }
        }
        next = strchr(next, '\n');
    }
}
bool de_cook_shader_diagnostic_parse(const char *text, de_cook_shader_diagnostic *out) {
    if (!text || !out)
        return false;
    memset(out, 0, sizeof *out);
    while (*text == ' ' || *text == '\t')
        ++text;
    for (const char *p = text; *p && *p != '\n'; ++p) {
        if (*p != '(' && *p != ':')
            continue;
        if (*p == ':') {
            const char *digits = p;
            while (digits > text && digits[-1] >= '0' && digits[-1] <= '9')
                --digits;
            if (digits != p && digits > text && digits[-1] == ':')
                continue;
        }
        const char *q = p + 1;
        u32 row, col = 0;
        if (!de__asset_diag_number(&q, &row))
            continue;
        if (*p == '(') {
            if (*q == ',') {
                ++q;
                if (!de__asset_diag_number(&q, &col))
                    continue;
            }
            if (*q++ != ')')
                continue;
        } else if (*q == ':' && q[1] >= '0' && q[1] <= '9') {
            ++q;
            if (!de__asset_diag_number(&q, &col))
                continue;
        }
        if (*q++ != ':')
            continue;
        while (*q == ' ' || *q == '\t')
            ++q;
        const char *s = severity(q, &out->message);
        const size_t size = (size_t)(p - text);
        if (!s || !size || size >= sizeof out->source)
            continue;
        memcpy(out->source, text, size);
        out->source[size] = '\0';
        out->line = row;
        out->column = col;
        out->severity = s;
        return true;
    }
    out->severity = severity(text, &out->message);
    if (out->severity)
        rich_location(text, out);
    return out->severity != NULL;
}
