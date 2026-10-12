/*
 * ser_common.c: helpers of the TOML text serializer (DENG-295): output staging, field paths,
 * shortest round-trip float text, hex IDs, the "reflect" log category, and de_scene_id_new().
 * See ser_internal.h.
 */

/* getentropy() is not POSIX: ask for the platform extensions (glibc >= 2.25, macOS 10.12). */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#if defined(__linux__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif
/* rand_s (the MSVC / UCRT wrapper around RtlGenRandom) is declared only with _CRT_RAND_S. */
#if defined(_WIN32) && !defined(_CRT_RAND_S)
#define _CRT_RAND_S
#endif

#include "ser_internal.h"

#include "core/de_compiler.h"

#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
/* rand_s is in <stdlib.h>. */
#elif defined(__APPLE__)
#include <sys/types.h>
#include <sys/random.h>
#else
#include <unistd.h>
#endif

/* ---- Output staging ------------------------------------------------------------------------ */

void de__ser_out_init(de__ser_out *o, de_writer *w) {
    o->w = w;
    o->err = DE_OK;
    o->len = 0;
}

static void out_drain(de__ser_out *o) {
    if (o->err != DE_OK || o->len == 0)
        return;
    de_result r = o->w->write(o->w->user, o->buf, o->len);
    if (r != DE_OK)
        o->err = DE_FAIL_WRAP(r, "text serializer: the writer failed");
    o->len = 0;
}

void de__ser_out_put(de__ser_out *o, const char *s, size_t n) {
    while (n > 0 && o->err == DE_OK) {
        size_t room = DE__SER_OUT_CAP - o->len;
        if (room == 0) {
            out_drain(o);
            continue;
        }
        size_t k = n < room ? n : room;
        memcpy(o->buf + o->len, s, k);
        o->len += k;
        s += k;
        n -= k;
    }
}

void de__ser_out_puts(de__ser_out *o, const char *s) {
    de__ser_out_put(o, s, strlen(s));
}

void de__ser_out_putf(de__ser_out *o, const char *fmt, ...) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    /* Only short, fixed-shape text (numbers, keys) goes through here. */
    DE_ASSERT(n >= 0 && (size_t)n < sizeof tmp);
    if (n > 0)
        de__ser_out_put(o, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
}

de_result de__ser_out_flush(de__ser_out *o) {
    out_drain(o);
    return o->err;
}

/* ---- Field paths --------------------------------------------------------------------------- */

static void path_append(de__ser_path *p, const char *s, size_t n) {
    if (p->len + n + 1 > DE__SER_PATH_CAP) {
        /* Cut: keep the start and mark it. Restoring an earlier length still works. */
        size_t room = DE__SER_PATH_CAP - 1 - p->len;
        if (room > 3) {
            memcpy(p->buf + p->len, s, room - 3);
            memcpy(p->buf + p->len + room - 3, "...", 3);
            p->len += room;
        }
        p->buf[p->len] = '\0';
        return;
    }
    memcpy(p->buf + p->len, s, n);
    p->len += n;
    p->buf[p->len] = '\0';
}

size_t de__ser_path_push_name(de__ser_path *p, const char *name) {
    size_t at = p->len;
    if (p->len != 0)
        path_append(p, ".", 1);
    path_append(p, name, strlen(name));
    return at;
}

size_t de__ser_path_push_index(de__ser_path *p, u32 index) {
    size_t at = p->len;
    char tmp[16];
    int n = snprintf(tmp, sizeof tmp, "[%u]", (unsigned)index);
    path_append(p, tmp, (size_t)n);
    return at;
}

void de__ser_path_pop(de__ser_path *p, size_t len) {
    if (len < p->len) {
        p->len = len;
        p->buf[len] = '\0';
    }
}

const char *de__ser_path_str(const de__ser_path *p) {
    return p->len != 0 ? p->buf : "(object)";
}

/* ---- Floats -------------------------------------------------------------------------------- */

/*
 * Float text is built from the significant digits and the decimal exponent that "%.*e" gives
 * (only the digits and the exponent are taken from it, so the C locale's decimal point does not
 * matter), and laid out like Python's repr: plain decimal for exponents -4 to 15 ("0.001",
 * "1000000.25", "10.0"), scientific outside ("1e-5", "3.4028235e38", "5e-324"). The exponent has
 * no '+' and no leading zeros. The shortest digit count that reads back to the same value wins.
 */
static size_t layout_digits(bool neg, const char *digits, size_t n, int exp10, char *buf) {
    size_t o = 0;
    if (neg)
        buf[o++] = '-';
    if (exp10 >= -4 && exp10 < 16) {
        if (exp10 < 0) {
            buf[o++] = '0';
            buf[o++] = '.';
            for (int i = -1; i > exp10; --i)
                buf[o++] = '0';
            memcpy(buf + o, digits, n);
            o += n;
        } else {
            size_t int_len = (size_t)exp10 + 1;
            for (size_t i = 0; i < int_len; ++i)
                buf[o++] = i < n ? digits[i] : '0';
            buf[o++] = '.';
            if (n > int_len) {
                memcpy(buf + o, digits + int_len, n - int_len);
                o += n - int_len;
            } else {
                buf[o++] = '0';
            }
        }
    } else {
        buf[o++] = digits[0];
        if (n > 1) {
            buf[o++] = '.';
            memcpy(buf + o, digits + 1, n - 1);
            o += n - 1;
        }
        o += (size_t)snprintf(buf + o, DE__SER_NUM_CAP - o, "e%d", exp10);
    }
    buf[o] = '\0';
    return o;
}

/* `v` (finite, non-zero) with `prec` significant digits, laid out as above. */
static size_t format_prec(f64 v, int prec, char *buf) {
    char tmp[DE__SER_NUM_CAP];
    int len = snprintf(tmp, sizeof tmp, "%.*e", prec - 1, fabs(v));
    DE_ASSERT(len > 0 && (size_t)len < sizeof tmp);
    char digits[24];
    size_t n = 0;
    int i = 0;
    for (; i < len && tmp[i] != 'e' && tmp[i] != 'E'; ++i)
        if (tmp[i] >= '0' && tmp[i] <= '9' && n < sizeof digits)
            digits[n++] = tmp[i];
    int exp10 = i < len ? atoi(tmp + i + 1) : 0;
    while (n > 1 && digits[n - 1] == '0')
        --n; /* "1.50e+00" at a higher precision than needed: drop the zeros */
    return layout_digits(signbit(v) != 0, digits, n, exp10, buf);
}

static size_t format_special(f64 v, char *buf) {
    const char *s;
    if (v != v)
        s = "nan";
    else if (isinf(v))
        s = v > 0 ? "inf" : "-inf";
    else
        s = signbit(v) ? "-0.0" : "0.0";
    size_t n = strlen(s);
    memcpy(buf, s, n + 1);
    return n;
}

static bool parse_back(const char *s, size_t n, f64 *out) {
    return de_str_to_f64((de_str){s, n}, out) == DE_OK;
}

size_t de__ser_format_f64(f64 v, char *buf) {
    if (!isfinite(v) || v == 0)
        return format_special(v, buf);
    size_t n = 0;
    for (int prec = 1; prec <= 17; ++prec) {
        n = format_prec(v, prec, buf);
        f64 back;
        if (parse_back(buf, n, &back) && back == v)
            return n;
    }
    return n; /* 17 significant digits always round-trip */
}

bool de__ser_f64_to_f32(f64 d, f32 *out) {
    /* Half an ULP above FLT_MAX: anything below rounds to FLT_MAX, anything from here on is an
     * overflow (to infinity in IEEE arithmetic, undefined behaviour in C). */
    static const f64 k_round_limit = 0x1.ffffffp127;
    if (isfinite(d) && fabs(d) > (f64)FLT_MAX) {
        if (fabs(d) >= k_round_limit)
            return false;
        *out = d < 0 ? -FLT_MAX : FLT_MAX;
        return true;
    }
    *out = (f32)d;
    return true;
}

size_t de__ser_format_f32(f32 v, char *buf) {
    if (!isfinite(v) || v == 0)
        return format_special((f64)v, buf);
    size_t n = 0;
    for (int prec = 1; prec <= 9; ++prec) {
        n = format_prec((f64)v, prec, buf);
        f64 back;
        f32 back32;
        if (parse_back(buf, n, &back) && de__ser_f64_to_f32(back, &back32) && back32 == v)
            return n;
    }
    return n; /* 9 significant digits always round-trip an f32 */
}

/* ---- Hex IDs ------------------------------------------------------------------------------- */

size_t de__ser_format_hex64(u64 v, char *buf) {
    static const char k_digits[] = "0123456789abcdef";
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; ++i)
        buf[2 + i] = k_digits[(v >> (60 - 4 * i)) & 0xFu];
    buf[18] = '\0';
    return 18;
}

bool de__ser_parse_hex64(const char *s, size_t len, u64 *out) {
    if (len < 3 || len > 18 || s[0] != '0' || (s[1] != 'x' && s[1] != 'X'))
        return false;
    u64 v = 0;
    for (size_t i = 2; i < len; ++i) {
        char ch = s[i];
        u64 d;
        if (ch >= '0' && ch <= '9')
            d = (u64)(ch - '0');
        else if (ch >= 'a' && ch <= 'f')
            d = (u64)(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F')
            d = (u64)(ch - 'A' + 10);
        else
            return false;
        v = (v << 4) | d;
    }
    *out = v;
    return true;
}

/* ---- Log category -------------------------------------------------------------------------- */

de_log_category de__ser_log_category(void) {
    /* Registration returns the existing category after the first call, and is thread-safe. */
    return de_log_register_category("reflect", DE_LOG_INFO);
}

/* ---- Scene IDs ----------------------------------------------------------------------------- */

static bool os_entropy(u64 *out) {
#if defined(_WIN32)
    unsigned int lo, hi;
    if (rand_s(&lo) != 0 || rand_s(&hi) != 0)
        return false;
    *out = ((u64)hi << 32) | (u64)lo;
    return true;
#else
    return getentropy(out, sizeof *out) == 0;
#endif
}

u64 de_scene_id_new(void) {
    u64 id = 0;
    if (!os_entropy(&id)) {
        /* No OS entropy (never seen in practice): mix the time, a per-thread counter and an
         * address, so IDs from one process still differ. Not cryptographic; it does not need to
         * be, a scene ID only has to be unique. */
        static DE_THREAD_LOCAL u64 t_counter;
        struct {
            u64 t, c, n;
            const void *addr;
        } seed;
        memset(&seed, 0, sizeof seed);
        seed.t = (u64)time(NULL);
        seed.c = (u64)clock();
        seed.n = ++t_counter;
        seed.addr = (const void *)&t_counter;
        id = de_hash64(&seed, sizeof seed, 0x5ce9e1d5ce9e1d5cull);
    }
    /* 0 is "no scene ID": draw again (probability 2^-64). */
    while (id == 0) {
        if (!os_entropy(&id))
            id = 1;
    }
    return id;
}
