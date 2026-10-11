/* name.c: de_name interning (SPEC §5.6, §11.1).
 *
 * A de_name is de_hash64 of the exact bytes, seed 0, in every build. With DE_NAME_LOOKUP (Debug
 * and Dev) the bytes are also copied into one process-wide intern table so de_name_str() can print
 * names, and a second string with an already-used hash is a DE_PANIC: two assets or names that
 * collide must be renamed, not silently merged.
 *
 * The table is the one hidden global of this file, forced by the API (de_name_from takes no
 * context). It is dev-only and not per-frame. The strings are copied into a dedicated virtual
 * arena (DENG-148), so they never move; the slot array is its own small open-addressing table on
 * the system heap (the S02 hash map, DENG-151, is not used, so this does not depend on it).
 * Nothing is ever freed: interned strings live as long as the process.
 */
#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "platform/de_atomic.h"

#include <stdlib.h>
#include <string.h>

/* Test hooks (tests/unit/core/test_hash_name.c declares them). Not part of the API. Without
 * DE_NAME_LOOKUP they exist but do nothing, so both builds of name.c define the same symbols. */
typedef u64 (*de__name_hash_fn)(const void *data, size_t len);
void de__name_test_set_hash(de__name_hash_fn fn);
size_t de__name_test_count(void);

#if DE_NAME_LOOKUP

enum {
    NAME_TABLE_INITIAL_CAP = 1024, /* slots; a power of two */
    NAME_PANIC_PRINT_MAX = 200,    /* bytes of each string quoted in the collision panic */
    NAME_STATE_UNINIT = 0,
    NAME_STATE_INITIALIZING = 1,
    NAME_STATE_READY = 2,
};

typedef struct name_entry {
    u64 hash;
    const char *str; /* NUL-terminated copy in the arena; NULL marks an empty slot */
    size_t len;
} name_entry;

static struct {
    de_atomic_u32 state;
    de_mutex lock;
    name_entry *slots; /* cap entries */
    size_t cap;
    size_t count;
    de_arena *strings; /* created on the first intern; never destroyed */
    bool strings_failed;
} g_names;

/* Address space for interned strings. Only committed as it fills (64 KiB steps). */
#define NAME_ARENA_RESERVE ((size_t)1 << 30)

/* Set only by tests, while no other thread uses names. */
static de__name_hash_fn g_test_hash;

void de__name_test_set_hash(de__name_hash_fn fn) {
    g_test_hash = fn;
}

/* The mutex is created on first use. de_mutex has no static initialiser, so the first caller
 * initialises it and the others spin for the few nanoseconds that takes. */
static void names_init_once(void) {
    if (de_atomic_load_u32(&g_names.state, DE_ATOMIC_ACQUIRE) == NAME_STATE_READY)
        return;
    u32 expected = NAME_STATE_UNINIT;
    if (de_atomic_cas_u32(&g_names.state, &expected, NAME_STATE_INITIALIZING, DE_ATOMIC_ACQ_REL,
                          DE_ATOMIC_ACQUIRE)) {
        de_mutex_init(&g_names.lock);
        de_atomic_store_u32(&g_names.state, NAME_STATE_READY, DE_ATOMIC_RELEASE);
        return;
    }
    while (de_atomic_load_u32(&g_names.state, DE_ATOMIC_ACQUIRE) != NAME_STATE_READY)
        de_cpu_pause();
}

/* Copies `len` bytes plus a NUL into the string arena. NULL when out of memory. Caller holds the
 * lock. */
static const char *names_copy(const char *s, size_t len) {
    if (g_names.strings == NULL) {
        if (g_names.strings_failed)
            return NULL;
        const de_arena_desc desc = {
            .reserve_bytes = NAME_ARENA_RESERVE, .tag = DE_MEM_GENERAL, .name = "de_name strings"};
        if (de_arena_create(&desc, &g_names.strings) != DE_OK) {
            g_names.strings_failed = true; /* do not retry the reservation on every name */
            return NULL;
        }
    }
    char *dst = (char *)de_arena_push(g_names.strings, len + 1, 1);
    if (dst == NULL)
        return NULL;
    if (len != 0)
        memcpy(dst, s, len);
    dst[len] = '\0';
    return dst;
}

/* Doubles the table. False when the system heap is exhausted (the old table stays). */
static bool names_grow(void) {
    size_t cap = g_names.cap == 0 ? (size_t)NAME_TABLE_INITIAL_CAP : g_names.cap * 2;
    name_entry *slots = (name_entry *)calloc(cap, sizeof(name_entry));
    if (slots == NULL)
        return false;
    for (size_t i = 0; i < g_names.cap; ++i) {
        const name_entry *e = &g_names.slots[i];
        if (e->str == NULL)
            continue;
        size_t j = (size_t)e->hash & (cap - 1);
        while (slots[j].str != NULL)
            j = (j + 1) & (cap - 1);
        slots[j] = *e;
    }
    free(g_names.slots);
    g_names.slots = slots;
    g_names.cap = cap;
    return true;
}

/* The slot holding `hash`, or NULL. Caller holds the lock. */
static const name_entry *names_find(u64 hash) {
    if (g_names.cap == 0)
        return NULL;
    size_t mask = g_names.cap - 1;
    for (size_t i = (size_t)hash & mask;; i = (i + 1) & mask) {
        const name_entry *e = &g_names.slots[i];
        if (e->str == NULL)
            return NULL;
        if (e->hash == hash)
            return e;
    }
}

static int print_len(size_t len) {
    return len > NAME_PANIC_PRINT_MAX ? NAME_PANIC_PRINT_MAX : (int)len;
}

static void names_intern(u64 hash, const char *s, size_t len) {
    names_init_once();
    de_mutex_lock(&g_names.lock);
    const name_entry *found = names_find(hash);
    if (found != NULL) {
        if (found->len == len && (len == 0 || memcmp(found->str, s, len) == 0)) {
            de_mutex_unlock(&g_names.lock);
            return;
        }
        /* The interned copy never moves or goes away, so it can be printed after unlocking.
         * Unlocking first keeps the table usable if a panic hook leaves by longjmp. */
        const char *other = found->str;
        size_t other_len = found->len;
        de_mutex_unlock(&g_names.lock);
        DE_PANIC("de_name: hash collision 0x%016llx between \"%.*s\" (%zu bytes) and \"%.*s\" "
                 "(%zu bytes); rename one of them",
                 (unsigned long long)hash, print_len(other_len), other, other_len,
                 print_len(len), s, len);
    }
    /* Keep the load factor at or below 70 %. */
    if ((g_names.count + 1) * 10 > g_names.cap * 7 && !names_grow()) {
        de_mutex_unlock(&g_names.lock);
        return; /* out of memory: the name still works, it just cannot be printed */
    }
    const char *copy = names_copy(s, len);
    if (copy != NULL) {
        size_t mask = g_names.cap - 1;
        size_t i = (size_t)hash & mask;
        while (g_names.slots[i].str != NULL)
            i = (i + 1) & mask;
        g_names.slots[i] = (name_entry){hash, copy, len};
        ++g_names.count;
    }
    de_mutex_unlock(&g_names.lock);
}

de_name de_name_from_view(de_str s) {
    const char *p = s.ptr != NULL ? s.ptr : "";
    size_t len = s.ptr != NULL ? s.len : 0;
    de_name n = {g_test_hash != NULL ? g_test_hash(p, len) : de_hash64(p, len, 0)};
    names_intern(n.hash, p, len);
    return n;
}

const char *de_name_str(de_name n) {
    if (de_atomic_load_u32(&g_names.state, DE_ATOMIC_ACQUIRE) != NAME_STATE_READY)
        return NULL; /* nothing was interned yet */
    de_mutex_lock(&g_names.lock);
    const name_entry *e = names_find(n.hash);
    const char *str = e != NULL ? e->str : NULL;
    de_mutex_unlock(&g_names.lock);
    return str;
}

size_t de__name_test_count(void) {
    if (de_atomic_load_u32(&g_names.state, DE_ATOMIC_ACQUIRE) != NAME_STATE_READY)
        return 0;
    de_mutex_lock(&g_names.lock);
    size_t count = g_names.count;
    de_mutex_unlock(&g_names.lock);
    return count;
}

#else /* !DE_NAME_LOOKUP: Release and Shipping keep no strings. */

void de__name_test_set_hash(de__name_hash_fn fn) {
    (void)fn;
}

size_t de__name_test_count(void) {
    return 0;
}

de_name de_name_from_view(de_str s) {
    if (s.ptr == NULL)
        return (de_name){de_hash64("", 0, 0)};
    return (de_name){de_hash64(s.ptr, s.len, 0)};
}

const char *de_name_str(de_name n) {
    (void)n;
    return NULL;
}

#endif /* DE_NAME_LOOKUP */

de_name de_name_from(const char *str) {
    return de_name_from_view(de_str_from_cstr(str));
}
