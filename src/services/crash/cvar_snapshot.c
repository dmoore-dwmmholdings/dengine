/* cvar_snapshot.c: the cvar dump of the crash bundle (SPEC §18.5, DENG-96). See
 * bundle_internal.h.
 *
 * The crash handler cannot walk the cvar registry (it takes a mutex and its strings may be
 * mid-update), so the main thread keeps a preformatted TOML text of every cvar in one of two
 * static buffers and publishes it with an atomic index. A refresh writes the buffer that is not
 * current, then flips the index; the handler freezes the snapshot (no later refresh writes
 * anything) and then writes the current buffer as it is.
 *
 * A refresh first hashes the values through the lock-free getters (no allocation) and formats
 * only when something changed, so the once-per-second frame-end refresh costs one walk of the
 * registry. Formatting is hand-written except for non-integral floats, which use snprintf with
 * the shortest round-tripping precision (the C library may allocate there; it runs only after a
 * float cvar changed). */
#include "services/crash/bundle_internal.h"

#include "platform/crash_internal.h"
#include "platform/de_atomic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_buf[2][DE__CRASH_SNAPSHOT_BYTES];
static de_atomic_u32 g_len[2];
static de_atomic_u32 g_index;  /* the current buffer */
static de_atomic_u32 g_frozen; /* set by the crash handler: no more refreshes */
static de_atomic_u32 g_busy;   /* a refresh is running */
static de_atomic_u64 g_count;  /* snapshots published */
static u64 g_hash;             /* of the values in the current snapshot (under g_busy) */
static bool g_hash_valid;

/* ---- Change detection ------------------------------------------------------------------- */

static u64 hash_mix(u64 h, u64 v) {
    /* FNV-1a over the 8 bytes of v. */
    for (int i = 0; i < 8; ++i) {
        h ^= (v >> (i * 8)) & 0xFFu;
        h *= 0x100000001B3ull;
    }
    return h;
}

static void hash_visit(de_cvar cv, void *user) {
    u64 *h = (u64 *)user;
    de_cvar_type type = de_cvar_get_type(cv);
    *h = hash_mix(*h, ((u64)cv.id << 8) | (u64)type);
    switch (type) {
    case DE_CVAR_FLOAT: {
        f64 v = de_cvar_float(cv);
        u64 bits;
        memcpy(&bits, &v, sizeof bits);
        *h = hash_mix(*h, bits);
        break;
    }
    case DE_CVAR_STRING:
        /* Each distinct value is its own immutable copy that lives as long as the process, so
         * the pointer changes exactly when the text does. */
        *h = hash_mix(*h, (u64)(uintptr_t)de_cvar_string(cv));
        break;
    default: /* bool, int, enum index */
        *h = hash_mix(*h, (u64)de_cvar_int(cv));
        break;
    }
}

/* ---- Formatting ------------------------------------------------------------------------- */

typedef struct snap_writer {
    char *p;
    size_t len, cap; /* cap leaves room for the closing comment */
    u32 written, omitted;
} snap_writer;

/* A line under construction (one cvar). */
typedef struct line_buf {
    char p[DE_CVAR_NAME_MAX + 3 * 1024];
    size_t len;
    bool overflow;
} line_buf;

static void lb_put(line_buf *b, const char *s, size_t n) {
    if (n > sizeof b->p - b->len) {
        b->overflow = true;
        n = sizeof b->p - b->len;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
}

static void lb_puts(line_buf *b, const char *s) {
    lb_put(b, s, strlen(s));
}

static void lb_i64(line_buf *b, i64 v) {
    char num[24];
    if (v < 0) {
        lb_puts(b, "-");
        size_t n = de__crash_fmt_u64(num, (u64)0 - (u64)v, 10);
        lb_put(b, num, n);
        return;
    }
    size_t n = de__crash_fmt_u64(num, (u64)v, 10);
    lb_put(b, num, n);
}

static void lb_float(line_buf *b, f64 v) {
    if (v != v) {
        lb_puts(b, "nan");
        return;
    }
    if (v > 1.7976931348623157e308) {
        lb_puts(b, "inf");
        return;
    }
    if (v < -1.7976931348623157e308) {
        lb_puts(b, "-inf");
        return;
    }
    /* Whole numbers below 2^53 exactly: digits and ".0", no C library call. */
    if (v > -9007199254740992.0 && v < 9007199254740992.0 && v == (f64)(i64)v) {
        if (v == 0.0) {
            u64 bits;
            memcpy(&bits, &v, sizeof bits);
            lb_puts(b, (bits >> 63) != 0 ? "-0.0" : "0.0");
            return;
        }
        lb_i64(b, (i64)v);
        lb_puts(b, ".0");
        return;
    }
    char num[40];
    for (int prec = 1; prec <= 17; ++prec) {
        snprintf(num, sizeof num, "%.*g", prec, v);
        if (strtod(num, NULL) == v)
            break;
    }
    lb_puts(b, num);
    /* TOML needs a '.' or an exponent in a float ("1e+30" is fine, "100" is an integer). */
    if (strpbrk(num, ".eE") == NULL)
        lb_puts(b, ".0");
}

static void lb_string(line_buf *b, const char *s) {
    static const char hex[] = "0123456789ABCDEF";
    lb_puts(b, "\"");
    for (; s != NULL && *s != '\0'; ++s) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':
            lb_puts(b, "\\\"");
            break;
        case '\\':
            lb_puts(b, "\\\\");
            break;
        case '\b':
            lb_puts(b, "\\b");
            break;
        case '\t':
            lb_puts(b, "\\t");
            break;
        case '\n':
            lb_puts(b, "\\n");
            break;
        case '\f':
            lb_puts(b, "\\f");
            break;
        case '\r':
            lb_puts(b, "\\r");
            break;
        default:
            if (c < 0x20u || c == 0x7Fu) {
                char esc[7] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 0xFu], '\0'};
                lb_puts(b, esc);
            } else {
                lb_put(b, (const char *)&c, 1);
            }
            break;
        }
    }
    lb_puts(b, "\"");
}

static void snap_put(snap_writer *w, const char *s, size_t n) {
    memcpy(w->p + w->len, s, n);
    w->len += n;
}

static void format_visit(de_cvar cv, void *user) {
    snap_writer *w = (snap_writer *)user;
    static line_buf b; /* main thread only (under g_busy); too big for some worker stacks */
    b.len = 0;
    b.overflow = false;
    lb_puts(&b, de_cvar_name(cv));
    lb_puts(&b, " = ");
    switch (de_cvar_get_type(cv)) {
    case DE_CVAR_BOOL:
        lb_puts(&b, de_cvar_bool(cv) ? "true" : "false");
        break;
    case DE_CVAR_INT:
        lb_i64(&b, de_cvar_int(cv));
        break;
    case DE_CVAR_FLOAT:
        lb_float(&b, de_cvar_float(cv));
        break;
    case DE_CVAR_STRING:
    case DE_CVAR_ENUM: /* the enum's value name */
        lb_string(&b, de_cvar_string(cv));
        break;
    default:
        lb_puts(&b, "\"?\"");
        break;
    }
    lb_puts(&b, "\n");
    if (b.overflow || b.len > w->cap - w->len) {
        w->omitted++;
        return;
    }
    snap_put(w, b.p, b.len);
    w->written++;
}

/* ---- Refresh and read ------------------------------------------------------------------- */

static const char k_header[] =
    "# DEngine cvar snapshot (crash bundle): every registered cvar and its value, in "
    "registration order\n";

bool de__crash_cvar_snapshot_refresh(bool force) {
    if (de_atomic_load_u32(&g_frozen, DE_ATOMIC_SEQ_CST) != 0)
        return false;
    u32 idle = 0;
    if (!de_atomic_cas_u32(&g_busy, &idle, 1u, DE_ATOMIC_ACQUIRE, DE_ATOMIC_RELAXED))
        return false;

    u64 h = 0xCBF29CE484222325ull;
    de_cvar_foreach_flag(0, hash_visit, &h);
    if (!force && g_hash_valid && h == g_hash) {
        de_atomic_store_u32(&g_busy, 0, DE_ATOMIC_RELEASE);
        return false;
    }

    u32 target = 1u - de_atomic_load_u32(&g_index, DE_ATOMIC_ACQUIRE);
    static const size_t k_footer_room = 96;
    snap_writer w = {g_buf[target], 0, DE__CRASH_SNAPSHOT_BYTES - k_footer_room, 0, 0};
    snap_put(&w, k_header, sizeof k_header - 1);
    de_cvar_foreach_flag(0, format_visit, &w);
    if (w.omitted > 0) {
        char num[24];
        size_t n = de__crash_fmt_u64(num, w.omitted, 10);
        snap_put(&w, "# ", 2);
        snap_put(&w, num, n);
        static const char tail[] = " more cvars not listed: the snapshot is full\n";
        snap_put(&w, tail, sizeof tail - 1);
    }

    bool published = false;
    /* A crash that froze the snapshot meanwhile is reading the current buffer: keep it. */
    if (de_atomic_load_u32(&g_frozen, DE_ATOMIC_SEQ_CST) == 0) {
        de_atomic_store_u32(&g_len[target], (u32)w.len, DE_ATOMIC_RELAXED);
        de_atomic_store_u32(&g_index, target, DE_ATOMIC_SEQ_CST);
        de_atomic_fetch_add_u64(&g_count, 1, DE_ATOMIC_RELAXED);
        g_hash = h;
        g_hash_valid = true;
        published = true;
    }
    de_atomic_store_u32(&g_busy, 0, DE_ATOMIC_RELEASE);
    return published;
}

const char *de__crash_cvar_snapshot_current(size_t *len) {
    u32 i = de_atomic_load_u32(&g_index, DE_ATOMIC_SEQ_CST);
    if (len != NULL)
        *len = de_atomic_load_u32(&g_len[i], DE_ATOMIC_RELAXED);
    return g_buf[i];
}

void de__crash_cvar_snapshot_write_fd(intptr_t fd) {
    /* Freeze first, then read the index: a refresh that is running now finishes into the other
     * buffer and does not publish, and no later refresh starts. */
    de_atomic_store_u32(&g_frozen, 1u, DE_ATOMIC_SEQ_CST);
    size_t len = 0;
    const char *text = de__crash_cvar_snapshot_current(&len);
    if (len > 0)
        de__crash_os_write(fd, text, len);
}

u64 de__crash_cvar_snapshot_count(void) {
    return de_atomic_load_u64(&g_count, DE_ATOMIC_RELAXED);
}

void de__crash_cvar_snapshot_reset_for_tests(void) {
    de_atomic_store_u32(&g_len[0], 0, DE_ATOMIC_RELAXED);
    de_atomic_store_u32(&g_len[1], 0, DE_ATOMIC_RELAXED);
    de_atomic_store_u32(&g_index, 0, DE_ATOMIC_RELAXED);
    de_atomic_store_u32(&g_frozen, 0, DE_ATOMIC_RELAXED);
    de_atomic_store_u32(&g_busy, 0, DE_ATOMIC_RELAXED);
    de_atomic_store_u64(&g_count, 0, DE_ATOMIC_RELAXED);
    g_hash = 0;
    g_hash_valid = false;
}
