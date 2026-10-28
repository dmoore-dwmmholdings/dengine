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

/* Outputs are changed only on success. Source is copied because the event outlives its job. */
static inline bool de__asset_diagnostic_location(const char *text, char *source, size_t cap,
                                                 uint32_t *line) {
    if (!text || !source || !cap || !line)
        return false;
    const char *start = text;
    for (const char *p = text; *p; ++p) {
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
