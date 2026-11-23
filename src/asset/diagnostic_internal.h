/* Bounded compiler-location reader for Slang/DXC and clang diagnostics (DENG-662). */
#ifndef DE_ASSET_DIAGNOSTIC_INTERNAL_H
#define DE_ASSET_DIAGNOSTIC_INTERNAL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline bool de__asset_diag_number(const char **at, uint32_t *out) {
    const char *p = *at;
    uint32_t n = 0;
    if (*p < '0' || *p > '9')
        return false;
    while (*p >= '0' && *p <= '9') {
        const uint32_t d = (uint32_t)(*p++ - '0');
        if (n > (UINT32_MAX - d) / 10u)
            return false;
        n = n * 10u + d;
    }
    *at = p;
    *out = n;
    return n != 0;
}

/* 1: error/warning, 2: note. A new heading ends the previous rich diagnostic. */
static inline unsigned de__asset_diag_severity(const char *p) {
    static const char *const names[] = {"fatal error", "error", "warning", "note"};
    for (unsigned i = 0; i < 4; ++i) {
        const size_t n = strlen(names[i]);
        if (!strncmp(p, names[i], n) && (p[n] == ':' || p[n] == '[' || p[n] == ' ' || p[n] == '\0'))
            return i == 3 ? 2u : 1u;
    }
    return 0;
}

/* Pinned Slang rich diagnostics put --> file:row:column after a severity heading.
 * Do not borrow a later diagnostic's location. No allocation, and failed parses leave outputs. */
static inline bool de__asset_diag_rich_location(const char *heading, char *source, size_t cap,
                                                uint32_t *line) {
    const char *next = strchr(heading, '\n');
    while (next && *++next) {
        const char *p = next;
        while (*p == ' ' || *p == '\t')
            ++p;
        if (de__asset_diag_severity(p))
            return false;
        if (!strncmp(p, "-->", 3)) {
            p += 3;
            while (*p == ' ' || *p == '\t')
                ++p;
            for (const char *colon = p; *colon && *colon != '\n'; ++colon) {
                if (*colon != ':')
                    continue;
                const char *q = colon + 1;
                uint32_t row, column;
                if (!de__asset_diag_number(&q, &row) || *q != ':')
                    continue;
                ++q;
                if (!de__asset_diag_number(&q, &column))
                    continue;
                if (*q == '\r')
                    ++q;
                if (*q && *q != '\n')
                    continue;
                const size_t n = (size_t)(colon - p);
                if (!n || n >= cap)
                    continue;
                memcpy(source, p, n);
                source[n] = '\0';
                *line = row;
                return true;
            }
        }
        next = strchr(next, '\n');
    }
    return false;
}

/* Outputs are changed only on success. Source is copied because the event outlives its job. */
static inline bool de__asset_diagnostic_location(const char *text, char *source, size_t cap,
                                                 uint32_t *line) {
    if (!text || !source || !cap || !line)
        return false;
    const char *start = text;
    for (const char *p = text; *p; ++p) {
        if (p == start) {
            const char *heading = start;
            while (*heading == ' ' || *heading == '\t')
                ++heading;
            if (!strncmp(heading, "asset \"", 7)) {
                const char *end = strstr(heading + 7, "\": ");
                const char *newline = strchr(heading, '\n');
                if (end && (!newline || end < newline))
                    heading = end + 3;
            }
            /* shader_importer wraps Slang with the target and permutation. */
            static const char *const targets[] = {"spirv", "dxil",      "metallib",
                                                  "metal", "spirv-asm", "dxil-asm"};
            for (unsigned i = 0; i < sizeof targets / sizeof targets[0]; ++i) {
                const size_t n = strlen(targets[i]);
                if (strncmp(heading, targets[i], n) || strncmp(heading + n, " (", 2))
                    continue;
                const char *preamble = heading + n + 2;
                if (!strncmp(preamble, "no defines): ", 13)) {
                    heading = preamble + 13;
                    break;
                }
                if (!strncmp(preamble, "permutation \"", 13)) {
                    const char *end = strstr(preamble + 13, "\"): ");
                    const char *newline = strchr(preamble, '\n');
                    if (end && (!newline || end < newline))
                        heading = end + 4;
                    break;
                }
            }
            if (de__asset_diag_severity(heading) == 1 &&
                de__asset_diag_rich_location(heading, source, cap, line))
                return true;
        }
        if (*p == '\n') {
            start = p + 1;
            continue;
        }
        if (*p != '(' && *p != ':')
            continue;
        /* A numeric segment after a prior colon is the column, not another filename/line. */
        if (*p == ':') {
            const char *digits = p;
            while (digits > start && digits[-1] >= '0' && digits[-1] <= '9')
                --digits;
            if (digits != p && digits > start && digits[-1] == ':')
                continue;
        }
        const char *q = p + 1;
        uint32_t row, column;
        if (!de__asset_diag_number(&q, &row))
            continue;
        if (*p == '(') {
            if (*q == ',') {
                ++q;
                if (!de__asset_diag_number(&q, &column))
                    continue;
            }
            if (*q++ != ')')
                continue;
        } else if (*q == ':' && q[1] >= '0' && q[1] <= '9') {
            ++q;
            if (!de__asset_diag_number(&q, &column))
                continue;
        }
        if (*q != ':')
            continue;
        ++q;
        while (*q == ' ' || *q == '\t')
            ++q;
        if (strncmp(q, "error", 5) && strncmp(q, "fatal error", 11) && strncmp(q, "warning", 7))
            continue;
        while (start < p && (*start == ' ' || *start == '\t'))
            ++start;
        if (strncmp(start, "asset \"", 7) == 0) {
            const char *prefix = strstr(start + 7, "\": ");
            if (prefix != NULL && prefix < p)
                start = prefix + 3;
        }
        const size_t len = (size_t)(p - start);
        if (!len || len >= cap)
            continue;
        memcpy(source, start, len);
        source[len] = '\0';
        *line = row;
        return true;
    }
    return false;
}
#endif
