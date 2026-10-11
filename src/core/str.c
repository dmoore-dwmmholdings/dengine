/* str.c: string views (de_base.h) and the string builder (core/de_strbuf.h). SPEC §5.6.
 *
 * The view functions never allocate and never read past `len`, so they are safe on untrusted
 * input (DENG-155 fuzzes the parsers). Everything is byte-wise and ignores the C locale, except
 * that de_str_to_f64 adapts its input to the locale's decimal point before calling strtod. */
#include <dengine/de_base.h>

#include "core/de_strbuf.h"

#include <errno.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Bytes of the input quoted in parse error messages. */
#define STR_ERROR_QUOTE_MAX 64
/* Longest text de_str_to_f64 accepts; it is copied to the stack to NUL-terminate it for strtod. */
#define STR_F64_MAX 511

static int quote_len(de_str s) {
    return s.len > STR_ERROR_QUOTE_MAX ? STR_ERROR_QUOTE_MAX : (int)s.len;
}

static const char *quote_ptr(de_str s) {
    return s.ptr != NULL ? s.ptr : "";
}

/* ---------- Views ---------- */

de_str de_str_from_cstr(const char *s) {
    de_str r = {s, s != NULL ? strlen(s) : 0};
    return r;
}

bool de_str_eq(de_str a, de_str b) {
    return a.len == b.len && (a.len == 0 || memcmp(a.ptr, b.ptr, a.len) == 0);
}

bool de_str_starts_with(de_str s, de_str prefix) {
    return s.len >= prefix.len && (prefix.len == 0 || memcmp(s.ptr, prefix.ptr, prefix.len) == 0);
}

size_t de_str_find(de_str s, de_str needle) {
    if (needle.len == 0)
        return 0;
    if (needle.len > s.len)
        return DE_STR_NPOS;
    const size_t last = s.len - needle.len;
    const char first = needle.ptr[0];
    for (size_t i = 0; i <= last; ++i) {
        const char *hit = (const char *)memchr(s.ptr + i, first, last - i + 1);
        if (hit == NULL)
            return DE_STR_NPOS;
        i = (size_t)(hit - s.ptr);
        if (memcmp(hit + 1, needle.ptr + 1, needle.len - 1) == 0)
            return i;
    }
    return DE_STR_NPOS;
}

bool de_str_split_next(de_str *rest, char sep, de_str *out) {
    if (rest->ptr == NULL)
        return false;
    const char *hit = rest->len != 0 ? (const char *)memchr(rest->ptr, sep, rest->len) : NULL;
    if (hit == NULL) {
        *out = *rest;
        rest->ptr = NULL;
        rest->len = 0;
        return true;
    }
    const size_t n = (size_t)(hit - rest->ptr);
    out->ptr = rest->ptr;
    out->len = n;
    rest->ptr = hit + 1;
    rest->len -= n + 1;
    return true;
}

static bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

de_str de_str_trim(de_str s) {
    size_t b = 0, e = s.len;
    while (b < e && is_space(s.ptr[b]))
        ++b;
    while (e > b && is_space(s.ptr[e - 1]))
        --e;
    de_str r = {s.ptr != NULL ? s.ptr + b : NULL, e - b};
    return r;
}

/* ---------- Number parsing ---------- */

static int digit_value(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return 99;
}

de_result de_str_to_i64(de_str s, i64 *out) {
    size_t i = 0;
    bool neg = false;
    if (i < s.len && (s.ptr[i] == '+' || s.ptr[i] == '-'))
        neg = s.ptr[i++] == '-';
    unsigned base = 10;
    if (s.len - i >= 2 && s.ptr[i] == '0') {
        const char p = s.ptr[i + 1];
        if (p == 'x' || p == 'X')
            base = 16;
        else if (p == 'b' || p == 'B')
            base = 2;
        if (base != 10)
            i += 2;
    }
    if (i == s.len)
        return DE_FAIL(DE_ERR_PARSE, "not an integer: \"%.*s\"", quote_len(s), quote_ptr(s));
    /* Accumulate the magnitude; a negative number may reach 2^63. */
    const u64 limit = neg ? (u64)INT64_MAX + 1u : (u64)INT64_MAX;
    u64 v = 0;
    for (; i < s.len; ++i) {
        const int d = digit_value(s.ptr[i]);
        if (d >= (int)base)
            return DE_FAIL(DE_ERR_PARSE, "not an integer: \"%.*s\"", quote_len(s), quote_ptr(s));
        if (v > (limit - (u64)d) / base)
            return DE_FAIL(DE_ERR_PARSE, "integer out of range: \"%.*s\"", quote_len(s),
                           quote_ptr(s));
        v = v * base + (u64)d;
    }
    /* -(2^63) has no positive i64 counterpart: negate in unsigned arithmetic, then convert. */
    *out = neg ? (v == (u64)INT64_MAX + 1u ? INT64_MIN : -(i64)v) : (i64)v;
    return DE_OK;
}

static bool ieq(de_str s, size_t at, const char *word) {
    const size_t n = strlen(word);
    if (s.len - at != n)
        return false;
    for (size_t k = 0; k < n; ++k) {
        char c = s.ptr[at + k];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c != word[k])
            return false;
    }
    return true;
}

de_result de_str_to_f64(de_str s, f64 *out) {
    size_t i = 0;
    bool neg = false;
    if (i < s.len && (s.ptr[i] == '+' || s.ptr[i] == '-'))
        neg = s.ptr[i++] == '-';
    if (ieq(s, i, "inf") || ieq(s, i, "infinity")) {
        *out = neg ? -(f64)INFINITY : (f64)INFINITY;
        return DE_OK;
    }
    if (ieq(s, i, "nan")) {
        *out = neg ? -(f64)NAN : (f64)NAN;
        return DE_OK;
    }

    /* Validate the whole syntax first, so strtod sees only plain decimal input (no hex floats,
     * no leading whitespace, no "nan(...)") and must consume all of it. */
    size_t digits = 0, dot = (size_t)-1;
    for (; i < s.len && s.ptr[i] >= '0' && s.ptr[i] <= '9'; ++i)
        ++digits;
    if (i < s.len && s.ptr[i] == '.') {
        dot = i++;
        for (; i < s.len && s.ptr[i] >= '0' && s.ptr[i] <= '9'; ++i)
            ++digits;
    }
    if (digits == 0)
        return DE_FAIL(DE_ERR_PARSE, "not a number: \"%.*s\"", quote_len(s), quote_ptr(s));
    if (i < s.len && (s.ptr[i] == 'e' || s.ptr[i] == 'E')) {
        ++i;
        if (i < s.len && (s.ptr[i] == '+' || s.ptr[i] == '-'))
            ++i;
        size_t exp_digits = 0;
        for (; i < s.len && s.ptr[i] >= '0' && s.ptr[i] <= '9'; ++i)
            ++exp_digits;
        if (exp_digits == 0)
            return DE_FAIL(DE_ERR_PARSE, "not a number: \"%.*s\"", quote_len(s), quote_ptr(s));
    }
    if (i != s.len)
        return DE_FAIL(DE_ERR_PARSE, "not a number: \"%.*s\"", quote_len(s), quote_ptr(s));
    if (s.len > STR_F64_MAX)
        return DE_FAIL(DE_ERR_PARSE, "number longer than %d bytes: \"%.*s...\"", STR_F64_MAX,
                       quote_len(s), quote_ptr(s));

    char buf[STR_F64_MAX + 1];
    memcpy(buf, s.ptr, s.len);
    buf[s.len] = '\0';
    /* strtod uses the C locale's decimal point; the engine's text always uses '.'. */
    if (dot != (size_t)-1) {
        const struct lconv *lc = localeconv();
        if (lc != NULL && lc->decimal_point != NULL && lc->decimal_point[0] != '\0' &&
            lc->decimal_point[1] == '\0')
            buf[dot] = lc->decimal_point[0];
    }
    char *end = NULL;
    errno = 0;
    const double v = strtod(buf, &end);
    if (end != buf + s.len)
        return DE_FAIL(DE_ERR_PARSE, "not a number: \"%.*s\"", quote_len(s), quote_ptr(s));
    if (errno == ERANGE && isinf(v))
        return DE_FAIL(DE_ERR_PARSE, "number out of range: \"%.*s\"", quote_len(s), quote_ptr(s));
    /* Underflow (ERANGE with a tiny or zero result) keeps the rounded value. */
    *out = v;
    return DE_OK;
}

/* ---------- String builder ---------- */

void de_strbuf_init(de_strbuf *b, const de_allocator *alloc) {
    b->ptr = NULL;
    b->len = 0;
    b->cap = 0;
    b->alloc = alloc;
    b->tag = DE_MEM_GENERAL;
}

void de_strbuf_free(de_strbuf *b) {
    if (b->ptr != NULL)
        DE_FREE(b->alloc, b->ptr, b->cap);
    b->ptr = NULL;
    b->len = 0;
    b->cap = 0;
}

void de_strbuf_clear(de_strbuf *b) {
    b->len = 0;
    if (b->ptr != NULL)
        b->ptr[0] = '\0';
}

de_result de_strbuf_reserve(de_strbuf *b, size_t extra) {
    if (extra > (size_t)-1 - b->len - 1)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_strbuf: size overflow");
    const size_t need = b->len + extra + 1;
    if (need <= b->cap)
        return DE_OK;
    size_t cap = b->cap < 64 ? 64 : b->cap;
    while (cap < need)
        cap = cap > (size_t)-1 / 2 ? need : cap * 2;
    char *p;
    if (b->ptr == NULL)
        p = (char *)DE_ALLOC(b->alloc, cap, 1, b->tag);
    else
        p = (char *)b->alloc->realloc(b->alloc->user, b->ptr, b->cap, cap, 1, b->tag);
    if (p == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_strbuf: cannot grow to %zu bytes", cap);
    if (b->ptr == NULL)
        p[0] = '\0';
    b->ptr = p;
    b->cap = cap;
    return DE_OK;
}

de_result de_strbuf_append(de_strbuf *b, de_str s) {
    if (s.len == 0)
        return DE_OK;
    /* `s` may point into the buffer, which the reserve can move. */
    const bool inside = b->ptr != NULL && s.ptr >= b->ptr && s.ptr < b->ptr + b->cap;
    const size_t offset = inside ? (size_t)(s.ptr - b->ptr) : 0;
    DE_TRY(de_strbuf_reserve(b, s.len));
    memmove(b->ptr + b->len, inside ? b->ptr + offset : s.ptr, s.len);
    b->len += s.len;
    b->ptr[b->len] = '\0';
    return DE_OK;
}

de_result de_strbuf_append_cstr(de_strbuf *b, const char *s) {
    return de_strbuf_append(b, de_str_from_cstr(s));
}

de_result de_strbuf_append_char(de_strbuf *b, char c) {
    de_str s = {&c, 1};
    return de_strbuf_append(b, s);
}

de_result de_strbuf_vappendf(de_strbuf *b, const char *fmt, va_list args) {
    va_list again;
    va_copy(again, args);
    /* Pass 1: format into the free space (none yet if nothing was allocated). */
    const size_t room = b->cap > b->len ? b->cap - b->len : 0;
    const int n = vsnprintf(room != 0 ? b->ptr + b->len : NULL, room, fmt, args);
    if (n < 0) {
        va_end(again);
        if (b->ptr != NULL)
            b->ptr[b->len] = '\0';
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_strbuf: format error in \"%s\"", fmt);
    }
    if ((size_t)n < room) {
        va_end(again);
        b->len += (size_t)n;
        return DE_OK;
    }
    /* Pass 2: grow to the exact size and format again. */
    if (b->ptr != NULL)
        b->ptr[b->len] = '\0'; /* undo the cut-off pass 1 output in case the reserve fails */
    const de_result r = de_strbuf_reserve(b, (size_t)n);
    if (r != DE_OK) {
        va_end(again);
        return r;
    }
    vsnprintf(b->ptr + b->len, (size_t)n + 1, fmt, again);
    va_end(again);
    b->len += (size_t)n;
    return DE_OK;
}

de_result de_strbuf_appendf(de_strbuf *b, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const de_result r = de_strbuf_vappendf(b, fmt, args);
    va_end(args);
    return r;
}

de_str de_strbuf_view(const de_strbuf *b) {
    de_str s = {b->ptr != NULL ? b->ptr : "", b->len};
    return s;
}

const char *de_strbuf_cstr(const de_strbuf *b) {
    return b->ptr != NULL ? b->ptr : "";
}
