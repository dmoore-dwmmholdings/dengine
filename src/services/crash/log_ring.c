/* log_ring.c: the log tail of the crash bundle (SPEC §18.5, DENG-96). See bundle_internal.h.
 *
 * A static ring of DE__CRASH_LOG_RING_BYTES bytes and one 64-bit count of bytes ever written.
 * The writer is the log sink, which runs under the log's drain lock, so there is one writer at a
 * time: it copies a formatted line into the ring, then publishes the new count with a release
 * store. The crash handler reads the count (acquire) and writes the newest bytes. Nothing waits
 * and nothing allocates.
 *
 * Limits (documented on the Trama page "Module: services/crash"):
 *   - Only drained lines are here. Lines still queued in a thread's log buffer (S02 per-thread
 *     rings) when the process crashes are missing; de_log_flush before a deliberate crash.
 *   - A line the log thread is writing at the moment of the crash may overwrite the oldest bytes
 *     while the handler copies them; the handler skips the partial oldest line, so at worst the
 *     first line of the tail is cut. */
#include "services/crash/bundle_internal.h"

#include "platform/crash_internal.h"
#include "platform/de_atomic.h"

#include <string.h>

static char g_ring[DE__CRASH_LOG_RING_BYTES];
static de_atomic_u64 g_written;

/* ---- Writer (the log sink) ------------------------------------------------------------- */

typedef struct line_buf {
    char *p;
    size_t len, cap;
} line_buf;

static void lb_put(line_buf *b, const char *s, size_t n) {
    if (n > b->cap - b->len)
        n = b->cap - b->len;
    memcpy(b->p + b->len, s, n);
    b->len += n;
}

static void lb_puts(line_buf *b, const char *s) {
    if (s != NULL)
        lb_put(b, s, strlen(s));
}

static void lb_u64(line_buf *b, u64 v) {
    char num[24];
    size_t n = de__crash_fmt_u64(num, v, 10);
    lb_put(b, num, n);
}

static const char *basename_of(const char *path) {
    const char *base = path;
    for (const char *p = path; *p != '\0'; ++p)
        if (*p == '/' || *p == '\\')
            base = p + 1;
    return base;
}

static void ring_append(const char *s, size_t n) {
    if (n > DE__CRASH_LOG_RING_BYTES) { /* cannot happen with one line; keep the newest bytes */
        s += n - DE__CRASH_LOG_RING_BYTES;
        n = DE__CRASH_LOG_RING_BYTES;
    }
    u64 pos = de_atomic_load_u64(&g_written, DE_ATOMIC_RELAXED);
    size_t at = (size_t)(pos % DE__CRASH_LOG_RING_BYTES);
    size_t first = DE__CRASH_LOG_RING_BYTES - at;
    if (first > n)
        first = n;
    memcpy(g_ring + at, s, first);
    memcpy(g_ring, s + first, n - first);
    de_atomic_store_u64(&g_written, pos + n, DE_ATOMIC_RELEASE);
}

void de__crash_log_ring_sink(const de_log_record *rec, void *user) {
    (void)user;
    if (rec == NULL)
        return;
    /* A whole message (at most DE_LOG_MESSAGE_MAX) plus the prefix. */
    char line[DE_LOG_MESSAGE_MAX + 256];
    line_buf b = {line, 0, sizeof line - 1};
    u64 ms = rec->timestamp_ns / 1000000u;
    lb_u64(&b, ms / 1000u);
    char frac[4] = {(char)('0' + (ms % 1000u) / 100u), (char)('0' + (ms % 100u) / 10u),
                    (char)('0' + ms % 10u), '\0'};
    lb_puts(&b, ".");
    lb_puts(&b, frac);
    lb_puts(&b, " [");
    lb_puts(&b, de_log_level_name(rec->level));
    lb_puts(&b, "][");
    lb_puts(&b, de_log_category_name(rec->category));
    lb_puts(&b, "][");
    lb_u64(&b, rec->thread_id);
    lb_puts(&b, "] ");
    if (rec->file != NULL) {
        lb_puts(&b, basename_of(rec->file));
        lb_puts(&b, ":");
        lb_u64(&b, rec->line);
        lb_puts(&b, " ");
    }
    if (rec->message.ptr != NULL)
        lb_put(&b, rec->message.ptr, rec->message.len);
    line[b.len++] = '\n';
    ring_append(line, b.len);
}

/* ---- Readers ------------------------------------------------------------------------------ */

/* The readable text as up to two pieces, oldest first. */
static void ring_pieces(const char **a, size_t *an, const char **b, size_t *bn) {
    u64 end = de_atomic_load_u64(&g_written, DE_ATOMIC_ACQUIRE);
    u64 begin = end > DE__CRASH_LOG_RING_BYTES ? end - DE__CRASH_LOG_RING_BYTES : 0;
    if (end > DE__CRASH_LOG_RING_BYTES) {
        /* Wrapped: the oldest line is partial (and may be being overwritten); start after it. */
        while (begin < end && g_ring[begin % DE__CRASH_LOG_RING_BYTES] != '\n')
            ++begin;
        if (begin < end)
            ++begin;
    }
    size_t n = (size_t)(end - begin);
    size_t at = (size_t)(begin % DE__CRASH_LOG_RING_BYTES);
    size_t first = DE__CRASH_LOG_RING_BYTES - at;
    if (first > n)
        first = n;
    *a = g_ring + at;
    *an = first;
    *b = g_ring;
    *bn = n - first;
}

void de__crash_log_ring_write_fd(intptr_t fd) {
    const char *a, *b;
    size_t an, bn;
    ring_pieces(&a, &an, &b, &bn);
    if (an > 0)
        de__crash_os_write(fd, a, an);
    if (bn > 0)
        de__crash_os_write(fd, b, bn);
}

size_t de__crash_log_ring_copy(char *out, size_t cap) {
    if (out == NULL || cap == 0)
        return 0;
    const char *a, *b;
    size_t an, bn;
    ring_pieces(&a, &an, &b, &bn);
    /* Keep the newest bytes when `out` is small. */
    size_t total = an + bn;
    size_t skip = total > cap - 1 ? total - (cap - 1) : 0;
    size_t len = 0;
    if (skip < an) {
        memcpy(out, a + skip, an - skip);
        len = an - skip;
        skip = 0;
    } else {
        skip -= an;
    }
    memcpy(out + len, b + skip, bn - skip);
    len += bn - skip;
    out[len] = '\0';
    return len;
}

u64 de__crash_log_ring_written(void) {
    return de_atomic_load_u64(&g_written, DE_ATOMIC_ACQUIRE);
}

void de__crash_log_ring_reset_for_tests(void) {
    de_atomic_store_u64(&g_written, 0, DE_ATOMIC_RELEASE);
}
