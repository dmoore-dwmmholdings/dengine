/* error.c: the thread-local error context and its cause chain (SPEC §5.2).
 *
 * Each thread owns a ring of DE_ERROR_CHAIN_MAX records (the "error arena" that de_error.cause
 * points into). The current chain occupies `len` consecutive slots ending at `head`, newest
 * first through ->cause:
 *
 *     ring:  [ .. | oldest | ... | prev | head | .. ]
 *     head->cause == prev, ..., oldest->cause == NULL
 *
 * DE_FAIL starts a new chain in the slot after head. DE_FAIL_WRAP appends to the chain; when the
 * chain already fills the ring, the new record reuses the oldest slot and the record that pointed
 * at it gets cause = NULL. No heap allocation anywhere.
 */
#include <dengine/de_base.h>

#include "core/de_compiler.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

_Static_assert(DE_ERROR_CHAIN_MAX >= 8, "SPEC/DENG-146: the error ring holds at least 8 records");

typedef struct de__error_tls {
    de_error ring[DE_ERROR_CHAIN_MAX];
    u32 head; /* slot of the newest record; meaningful when len > 0 */
    u32 len;  /* records in the current chain; 0 = no error */
} de__error_tls;

static DE_THREAD_LOCAL de__error_tls de__err;

/* Formats into a stack buffer first: the arguments may point into ring records (a wrap that
 * quotes de_error_last()->message), and the slot being written may be one of them. */
static void de__error_format(char out[DE_ERROR_MESSAGE_MAX], const char *fmt, va_list ap) {
    if (fmt == NULL) {
        out[0] = '\0';
        return;
    }
    /* vsnprintf always NUL-terminates when the size is > 0 and returns the untruncated length,
     * so a long message is cut safely. A negative return is an encoding error. */
    int n = vsnprintf(out, DE_ERROR_MESSAGE_MAX, fmt, ap);
    if (DE_UNLIKELY(n < 0))
        out[0] = '\0';
}

static de_error *de__error_push(int wrap) {
    de__error_tls *t = &de__err;
    const de_error *prev = NULL;
    if (wrap && t->len > 0)
        prev = &t->ring[t->head];

    u32 slot = (t->head + 1u) % DE_ERROR_CHAIN_MAX;
    if (prev == NULL) {
        t->len = 1;
    } else if (t->len < DE_ERROR_CHAIN_MAX) {
        t->len++;
    } else {
        /* The ring is full: `slot` holds the oldest record of this chain. Cut the chain before
         * it. The next slot holds the second-oldest record, whose cause is `slot`. */
        u32 second_oldest = (slot + 1u) % DE_ERROR_CHAIN_MAX;
        t->ring[second_oldest].cause = NULL;
    }
    t->head = slot;
    de_error *e = &t->ring[slot];
    e->cause = prev;
    return e;
}

static de_result de__error_record(int wrap, de_result r, const char *file, u32 line,
                                  const char *func, const char *fmt, va_list ap) {
    char msg[DE_ERROR_MESSAGE_MAX];
    de__error_format(msg, fmt, ap);
    de_error *e = de__error_push(wrap);
    e->result = r;
    memcpy(e->message, msg, sizeof msg);
    e->file = file;
    e->function = func;
    e->line = line;
    return r;
}

de_result de_error_set_(de_result r, const char *file, u32 line, const char *func, const char *fmt,
                        ...) {
    va_list ap;
    va_start(ap, fmt);
    de_result out = de__error_record(0, r, file, line, func, fmt, ap);
    va_end(ap);
    return out;
}

de_result de_error_wrap_(de_result r, const char *file, u32 line, const char *func, const char *fmt,
                         ...) {
    va_list ap;
    va_start(ap, fmt);
    de_result out = de__error_record(1, r, file, line, func, fmt, ap);
    va_end(ap);
    return out;
}

const de_error *de_error_last(void) {
    de__error_tls *t = &de__err;
    return t->len > 0 ? &t->ring[t->head] : NULL;
}

void de_error_clear(void) {
    de__err.len = 0;
}
