/*
 * de_strbuf.h: growable string builder (internal, not installed). SPEC §5.6.
 *
 * Include as "core/de_strbuf.h". The buffer comes from the de_allocator given to
 * de_strbuf_init() and is always NUL-terminated once anything was appended. Not thread-safe.
 *
 *     de_strbuf b;
 *     de_strbuf_init(&b, alloc);
 *     DE_TRY(de_strbuf_appendf(&b, "%s=%d", key, value));
 *     use(de_strbuf_cstr(&b));
 *     de_strbuf_free(&b);
 *
 * A failed append (DE_ERR_OUT_OF_MEMORY, with the error context set) leaves the contents as they
 * were. Growth doubles the capacity (at least 64 bytes), so appends are amortised O(1).
 */
#ifndef DE_CORE_STRBUF_H
#define DE_CORE_STRBUF_H

#include <dengine/de_base.h>
#include <dengine/de_memory.h>

#include <stdarg.h>

typedef struct de_strbuf {
    char *ptr;  /**< NULL until the first allocation */
    size_t len; /**< bytes, without the NUL */
    size_t cap; /**< bytes allocated, including room for the NUL */
    const de_allocator *alloc;
    u32 tag; /**< memory tag passed to the allocator (DE_MEM_GENERAL after init) */
} de_strbuf;

/** An empty builder that allocates from `alloc` (not NULL). Allocates nothing yet. */
void de_strbuf_init(de_strbuf *b, const de_allocator *alloc);

/** Frees the buffer. The builder is empty afterwards and may be reused. */
void de_strbuf_free(de_strbuf *b);

/** Sets the length to 0 and keeps the buffer. */
void de_strbuf_clear(de_strbuf *b);

/** Makes room for `extra` more bytes (plus the NUL) without further allocation. */
de_result de_strbuf_reserve(de_strbuf *b, size_t extra);

/** Appends the bytes of `s` (they may point into the builder itself). */
de_result de_strbuf_append(de_strbuf *b, de_str s);

/** Appends a NUL-terminated string. NULL appends nothing. */
de_result de_strbuf_append_cstr(de_strbuf *b, const char *s);

/** Appends one byte. */
de_result de_strbuf_append_char(de_strbuf *b, char c);

/**
 * Appends printf-formatted text. Formats once into the free space; if that is too small, grows
 * the buffer to the exact size and formats again. Arguments must not point into the builder.
 * DE_ERR_INVALID_ARGUMENT if vsnprintf reports an encoding error.
 */
de_result de_strbuf_appendf(de_strbuf *b, const char *fmt, ...) DE_PRINTF_FMT(2, 3);

/** de_strbuf_appendf with a va_list (it is consumed, as with vsnprintf). */
de_result de_strbuf_vappendf(de_strbuf *b, const char *fmt, va_list args) DE_PRINTF_FMT(2, 0);

/** A view of the contents. Valid until the next change to the builder. */
de_str de_strbuf_view(const de_strbuf *b);

/** The contents as a NUL-terminated string ("" when nothing was allocated yet). Valid until the
 * next change to the builder. */
const char *de_strbuf_cstr(const de_strbuf *b);

#endif /* DE_CORE_STRBUF_H */
