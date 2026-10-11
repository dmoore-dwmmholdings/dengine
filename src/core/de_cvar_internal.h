/*
 * de_cvar_internal.h: what cvar.c shares with cvar_toml.c (internal, not installed). SPEC §5.8.
 *
 * Include as "core/de_cvar_internal.h". Only the two cvar sources use it.
 */
#ifndef DE_CORE_CVAR_INTERNAL_H
#define DE_CORE_CVAR_INTERNAL_H

#include <dengine/de_debug.h>
#include <dengine/de_memory.h>

#include "platform/de_atomic.h"

DE_EXTERN_C_BEGIN

/** One copy of a string value. Copies are kept until the registry is reset (tests only), so a
 * pointer from de_cvar_string never dangles. */
/* MSVC C4200 calls a C99 flexible array member a "nonstandard extension" (/W4 /WX); it is
 * standard C11, so silence only that warning, only here. */
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push)
#pragma warning(disable : 4200)
#endif
typedef struct de__cvar_strval {
    struct de__cvar_strval *next;
    size_t len;
    char text[]; /* len bytes + NUL */
} de__cvar_strval;
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif

/**
 * A registered cvar. One allocation holds the entry, the enum name pointers and every string
 * (name, help, enum names). Everything except the fields marked "lock" is written once before
 * the entry is published and then only read, so getters read it without the lock.
 */
typedef struct de__cvar_entry {
    de_atomic_u64 bits; /* bool, int, enum index: the i64 value; float: the f64 bits */
    de_atomic_ptr str;  /* DE_CVAR_STRING: const char * into a de__cvar_strval */
    const char *name;
    size_t name_len;
    const char *help; /* "" when none */
    const char *const *enum_values;
    u32 enum_count;
    de_cvar_type type;
    u32 flags;
    bool clamp; /* min < max */
    f64 min, max;
    de_cvar handle;
    size_t block_size;           /* bytes of this allocation */
    de_cvar_change_fn on_change; /* lock */
    void *user;                  /* lock */
    de__cvar_strval *strings;    /* lock: every string value this cvar has had */
} de__cvar_entry;

/** Who sets a value. Startup sources may set DE_CVAR_READONLY cvars until the startup lock. */
typedef enum de__cvar_source {
    DE__CVAR_SRC_CONSOLE, /* de_cvar_set */
    DE__CVAR_SRC_STARTUP, /* command line, config.toml, pending values at registration */
} de__cvar_source;

/** The registry's allocator. Creates the registry on first use. */
const de_allocator *de__cvar_allocator(void);

/** Takes / releases the registry mutex (creating the registry on first use). */
void de__cvar_lock(void);
void de__cvar_unlock(void);

/** Registered cvars, and the i-th in registration order. Caller holds the lock. */
u32 de__cvar_count_locked(void);
const de__cvar_entry *de__cvar_at_locked(u32 i);

/** True if `name[0..len)` follows the naming rules of de_debug.h. */
bool de__cvar_name_valid(const char *name, size_t len);

/**
 * Sets the cvar called `name[0..len)` from NUL-terminated `text`, or keeps the text pending when
 * no such cvar is registered. Calls on_change (without the lock) when the value changed. `what`
 * names the source in error messages ("command line", "config.toml").
 */
de_result de__cvar_set_by_name(const char *name, size_t len, const char *text, de__cvar_source src,
                               const char *what);

/**
 * First-failure bookkeeping for a batch of sets (command line, config.toml): every key is still
 * tried; the batch returns the first failure, and the thread's error context ends up describing
 * it (with a count when there were more).
 */
typedef struct de__cvar_batch {
    de_result first;
    u32 failures;
    char message[DE_ERROR_MESSAGE_MAX];
} de__cvar_batch;
void de__cvar_batch_note(de__cvar_batch *b, de_result r);
de_result de__cvar_batch_result(const de__cvar_batch *b);

DE_EXTERN_C_END

#endif /* DE_CORE_CVAR_INTERNAL_H */
