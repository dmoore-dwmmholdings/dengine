/* cvar.c: the console variable registry (SPEC §5.8). The TOML side is in cvar_toml.c.
 *
 * One process-wide registry, created on first use (the API takes no context, like de_name). It
 * holds:
 *   - `order`: a de_array of entry pointers in registration order. Listing and saving walk it;
 *     the hash maps are never iterated (SPEC §5.6).
 *   - `by_name`: de_hashmap from the name's de_name hash to its index in `order`.
 *   - `prefixes`: the set of every dotted prefix of a registered name ("r", "r.shadows"), so a
 *     name that would be both a value and a TOML table is refused.
 *   - `pending`: values set for names nobody registered yet (de_array + de_hashmap by hash).
 *   - `chunks`: a two-level table of atomic entry pointers indexed by the handle. Getters go
 *     through it with acquire loads and never take the mutex. `order` cannot serve them, because
 *     its storage moves when it grows.
 *
 * Values live in the entry as atomics: numbers (and bool, enum index) as 64 bits, strings as a
 * pointer to an immutable copy. A set stores the new value with release; a getter loads it with
 * acquire. Old string copies are kept on the entry, so a pointer a getter returned stays valid.
 *
 * Handles: bits 0-15 are the index + 1, bits 16-31 the registry generation. The generation only
 * changes when a test resets the registry, so handles from before a reset are detected as stale.
 */
#include <dengine/de_base.h>
#include <dengine/de_debug.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "core/de_array.h"
#include "core/de_cvar_internal.h"
#include "core/de_hashmap.h"
#include "platform/de_atomic.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test hooks (tests/unit/core/test_cvar.c declares them). Not part of the API. */
void de__cvar_test_reset(void);
void de__cvar_test_lock(void);
void de__cvar_test_unlock(void);

/* Shipping drops DEV_ONLY cvars; Debug and Dev allow CHEAT cvars without `cheats`. */
#if defined(DE_SHIPPING)
#define CVAR_SHIPPING 1
#else
#define CVAR_SHIPPING 0
#endif
#if (defined(DE_DEBUG) || defined(DE_DEV)) && !defined(DE_SHIPPING)
#define CVAR_CHEATS_ALWAYS 1
#else
#define CVAR_CHEATS_ALWAYS 0
#endif

enum {
    CHUNK_SHIFT = 8,
    CHUNK_SIZE = 1 << CHUNK_SHIFT, /* entry pointers per chunk */
    CHUNK_COUNT = (DE_CVAR_MAX_COUNT + CHUNK_SIZE) / CHUNK_SIZE,
    STATE_UNINIT = 0,
    STATE_INITIALIZING = 1,
    STATE_READY = 2,
    VALUE_PRINT_MAX = 64, /* bytes of a bad value quoted in an error message */
};

/* A value set for a name nobody registered yet. `name == NULL` marks a used slot. */
typedef struct pending_value {
    char *name;
    size_t name_len;
    char *value;
    size_t value_size; /* bytes allocated, with the NUL */
} pending_value;

static struct {
    de_atomic_u32 state;
    de_atomic_u32 generation; /* 1..0xFFFF; outlives resets */
    de_atomic_ptr chunks[CHUNK_COUNT];
    de_mutex lock;
    const de_allocator *a; /* de_heap_allocator() */
    de_array order;        /* de__cvar_entry * */
    de_hashmap by_name;    /* u64 name hash -> u32 index in order */
    de_hashmap prefixes;
    de_array pending;           /* pending_value */
    de_hashmap pending_by_name; /* u64 name hash -> u32 index in pending */
    bool startup_locked;
    de_cvar cheats;
} g_cvars;

/* ---- Small helpers ----------------------------------------------------------------------- */

static int print_len(size_t len) {
    return len > VALUE_PRINT_MAX ? VALUE_PRINT_MAX : (int)len;
}

static i64 f64_to_i64_sat(f64 v) {
    if (v != v)
        return 0;
    if (v >= 9223372036854775808.0)
        return INT64_MAX;
    if (v < -9223372036854775808.0)
        return INT64_MIN;
    return (i64)v;
}

static u64 f64_bits(f64 v) {
    u64 b;
    memcpy(&b, &v, sizeof b);
    return b;
}

static f64 bits_f64(u64 b) {
    f64 v;
    memcpy(&v, &b, sizeof v);
    return v;
}

static char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool str_ieq(de_str s, const char *lit) {
    size_t n = strlen(lit);
    if (s.len != n)
        return false;
    for (size_t i = 0; i < n; ++i)
        if (ascii_lower(s.ptr[i]) != lit[i])
            return false;
    return true;
}

bool de__cvar_name_valid(const char *name, size_t len) {
    if (name == NULL || len == 0 || len > DE_CVAR_NAME_MAX)
        return false;
    bool segment_start = true;
    u32 parts = 1;
    for (size_t i = 0; i < len; ++i) {
        char c = name[i];
        if (c == '.') {
            if (segment_start || ++parts > DE_CVAR_NAME_MAX_PARTS)
                return false; /* leading dot, "..", or too many segments */
            segment_start = true;
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
            segment_start = false;
        } else {
            return false;
        }
    }
    return !segment_start; /* no trailing dot */
}

/* ---- Lifetime ---------------------------------------------------------------------------- */

static de_cvar register_locked(const de_cvar_desc *desc);

static void registry_create(void) {
    de_mutex_init(&g_cvars.lock);
    g_cvars.a = de_heap_allocator();
    de_array_init(&g_cvars.order, g_cvars.a, (u32)sizeof(de__cvar_entry *), 0, DE_MEM_GENERAL);
    de_array_init(&g_cvars.pending, g_cvars.a, (u32)sizeof(pending_value), 0, DE_MEM_GENERAL);
    de_result r = DE_HASHMAP_INIT(&g_cvars.by_name, u64, u32, g_cvars.a);
    if (r == DE_OK)
        r = de_hashmap_init(&g_cvars.prefixes,
                            &(const de_hashmap_desc){.allocator = g_cvars.a, .key_size = 8});
    if (r == DE_OK)
        r = DE_HASHMAP_INIT(&g_cvars.pending_by_name, u64, u32, g_cvars.a);
    DE_VERIFY(r == DE_OK); /* only fails on invalid descs */
    for (u32 i = 0; i < CHUNK_COUNT; ++i)
        de_atomic_init_ptr(&g_cvars.chunks[i], NULL);
    g_cvars.startup_locked = false;
    if (de_atomic_load_u32(&g_cvars.generation, DE_ATOMIC_RELAXED) == 0)
        de_atomic_store_u32(&g_cvars.generation, 1, DE_ATOMIC_RELAXED);

    /* The built-in cheat gate. Replicated so a server can grant it (S32). */
    g_cvars.cheats = register_locked(&(const de_cvar_desc){
        .name = "cheats",
        .help = "Allows CHEAT cvars to be set (always allowed in Debug and Dev builds).",
        .type = DE_CVAR_BOOL,
        .flags = DE_CVAR_REPLICATED,
    });
    if (!DE_HANDLE_VALID(g_cvars.cheats))
        de_error_clear(); /* out of memory: cheats then count as off */
}

/* Creates the registry on first use. de_mutex has no static initialiser, so the first caller
 * creates it and any other caller spins for the short time that takes. */
static void registry_init_once(void) {
    if (de_atomic_load_u32(&g_cvars.state, DE_ATOMIC_ACQUIRE) == STATE_READY)
        return;
    u32 expected = STATE_UNINIT;
    if (de_atomic_cas_u32(&g_cvars.state, &expected, STATE_INITIALIZING, DE_ATOMIC_ACQ_REL,
                          DE_ATOMIC_ACQUIRE)) {
        registry_create();
        de_atomic_store_u32(&g_cvars.state, STATE_READY, DE_ATOMIC_RELEASE);
        return;
    }
    while (de_atomic_load_u32(&g_cvars.state, DE_ATOMIC_ACQUIRE) != STATE_READY)
        de_cpu_pause();
}

const de_allocator *de__cvar_allocator(void) {
    registry_init_once();
    return g_cvars.a;
}

void de__cvar_lock(void) {
    registry_init_once();
    de_mutex_lock(&g_cvars.lock);
}

void de__cvar_unlock(void) {
    de_mutex_unlock(&g_cvars.lock);
}

u32 de__cvar_count_locked(void) {
    return g_cvars.order.count;
}

const de__cvar_entry *de__cvar_at_locked(u32 i) {
    return DE_ARRAY_AT(&g_cvars.order, de__cvar_entry *, i);
}

/* ---- Handle -> entry (lock-free) --------------------------------------------------------- */

static de__cvar_entry *entry_of(de_cvar cv) {
    u32 index1 = cv.id & 0xFFFFu;
    if (index1 == 0)
        return NULL;
    if ((cv.id >> 16) != de_atomic_load_u32(&g_cvars.generation, DE_ATOMIC_RELAXED))
        return NULL;
    u32 index = index1 - 1u;
    de_atomic_ptr *chunk = (de_atomic_ptr *)de_atomic_load_ptr(
        &g_cvars.chunks[index >> CHUNK_SHIFT], DE_ATOMIC_ACQUIRE);
    if (chunk == NULL)
        return NULL;
    return (de__cvar_entry *)de_atomic_load_ptr(&chunk[index & (CHUNK_SIZE - 1)],
                                                DE_ATOMIC_ACQUIRE);
}

/* Index of the entry named `name[0..len)` with hash `hash`, or UINT32_MAX. Caller holds the lock.
 */
static u32 find_locked(const char *name, size_t len, u64 hash) {
    const u32 *idx = DE_HASHMAP_GET(&g_cvars.by_name, u32, hash, &hash);
    if (idx == NULL)
        return UINT32_MAX;
    const de__cvar_entry *e = DE_ARRAY_AT(&g_cvars.order, de__cvar_entry *, *idx);
    if (e->name_len != len || memcmp(e->name, name, len) != 0)
        return UINT32_MAX; /* a different name with the same hash (Release/Shipping) */
    return *idx;
}

/* ---- Values ------------------------------------------------------------------------------ */

static bool cheats_allowed(void) {
    if (CVAR_CHEATS_ALWAYS)
        return true;
    const de__cvar_entry *c = entry_of(g_cvars.cheats);
    return c != NULL && de_atomic_load_u64(&c->bits, DE_ATOMIC_ACQUIRE) != 0;
}

static i64 clamp_i64(const de__cvar_entry *e, i64 v) {
    if (!e->clamp)
        return v;
    if ((f64)v < e->min) {
        i64 m = f64_to_i64_sat(e->min); /* towards zero; then up to ceil(min) */
        if ((f64)m < e->min && m != INT64_MAX)
            ++m;
        return m;
    }
    if ((f64)v > e->max) {
        i64 m = f64_to_i64_sat(e->max); /* towards zero; then down to floor(max) */
        if ((f64)m > e->max && m != INT64_MIN)
            --m;
        return m;
    }
    return v;
}

static f64 clamp_f64(const de__cvar_entry *e, f64 v) {
    if (!e->clamp)
        return v;
    if (v < e->min)
        return e->min;
    if (v > e->max)
        return e->max;
    return v;
}

static de_result fail_parse(const de__cvar_entry *e, const char *text, const char *what) {
    size_t n = strlen(text);
    return DE_FAIL(DE_ERR_PARSE, "cvar %s: \"%.*s\"%s is not %s", e->name, print_len(n), text,
                   n > VALUE_PRINT_MAX ? "..." : "", what);
}

/* Parses `text` for an enum: an exact name or an index. */
static de_result parse_enum(const de__cvar_entry *e, const char *text, de_str t, u64 *out) {
    for (u32 i = 0; i < e->enum_count; ++i) {
        if (de_str_eq(t, de_str_from_cstr(e->enum_values[i]))) {
            *out = i;
            return DE_OK;
        }
    }
    i64 index;
    if (t.len != 0 && t.ptr[0] >= '0' && t.ptr[0] <= '9' && de_str_to_i64(t, &index) == DE_OK &&
        index >= 0 && index < (i64)e->enum_count) {
        *out = (u64)index;
        return DE_OK;
    }
    char list[256];
    size_t used = 0;
    list[0] = '\0';
    for (u32 i = 0; i < e->enum_count && used + 1 < sizeof list; ++i) {
        int n = snprintf(list + used, sizeof list - used, "%s%s", i ? ", " : "", e->enum_values[i]);
        if (n < 0)
            break;
        used += (size_t)n;
    }
    if (used >= sizeof list)
        memcpy(list + sizeof list - 4, "...", 4);
    size_t n = strlen(text);
    return DE_FAIL(DE_ERR_PARSE, "cvar %s: \"%.*s\"%s is not one of: %s", e->name, print_len(n),
                   text, n > VALUE_PRINT_MAX ? "..." : "", list);
}

/* Parses `text` into the 64-bit representation for a non-string cvar. */
static de_result parse_bits(const de__cvar_entry *e, const char *text, u64 *out) {
    de_str t = de_str_trim(de_str_from_cstr(text));
    switch (e->type) {
    case DE_CVAR_BOOL:
        if (str_ieq(t, "1") || str_ieq(t, "true") || str_ieq(t, "on") || str_ieq(t, "yes")) {
            *out = 1;
            return DE_OK;
        }
        if (str_ieq(t, "0") || str_ieq(t, "false") || str_ieq(t, "off") || str_ieq(t, "no")) {
            *out = 0;
            return DE_OK;
        }
        return fail_parse(e, text, "a bool (1/0, true/false, on/off, yes/no)");
    case DE_CVAR_INT: {
        i64 v;
        if (de_str_to_i64(t, &v) != DE_OK)
            return fail_parse(e, text, "an integer");
        *out = (u64)clamp_i64(e, v);
        return DE_OK;
    }
    case DE_CVAR_FLOAT: {
        f64 v;
        if (de_str_to_f64(t, &v) != DE_OK)
            return fail_parse(e, text, "a number");
        if (v != v)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "cvar %s: NaN is not a valid value", e->name);
        *out = f64_bits(clamp_f64(e, v));
        return DE_OK;
    }
    case DE_CVAR_ENUM:
        return parse_enum(e, text, t, out);
    case DE_CVAR_STRING:
    case DE_CVAR_TYPE_COUNT_:
        break;
    }
    return DE_FAIL(DE_ERR_INTERNAL, "cvar %s: bad type", e->name);
}

/* Publishes `text` as the value of a string cvar, reusing an earlier copy of the same text. */
static de_result publish_string_locked(de__cvar_entry *e, const char *text, bool *changed) {
    size_t len = strlen(text);
    const char *cur = (const char *)de_atomic_load_ptr(&e->str, DE_ATOMIC_RELAXED);
    if (cur != NULL && strcmp(cur, text) == 0) {
        *changed = false;
        return DE_OK;
    }
    de__cvar_strval *sv = e->strings;
    while (sv != NULL && (sv->len != len || memcmp(sv->text, text, len) != 0))
        sv = sv->next;
    if (sv == NULL) {
        sv = (de__cvar_strval *)DE_ALLOC(g_cvars.a, sizeof(de__cvar_strval) + len + 1,
                                         DE_ALIGNOF(de__cvar_strval), DE_MEM_GENERAL);
        if (sv == NULL)
            return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "cvar %s: no memory for the value", e->name);
        sv->len = len;
        memcpy(sv->text, text, len + 1);
        sv->next = e->strings;
        e->strings = sv;
    }
    de_atomic_store_ptr(&e->str, sv->text, DE_ATOMIC_RELEASE);
    *changed = true;
    return DE_OK;
}

/* Checks the flags, parses and publishes. Caller holds the lock. */
static de_result set_locked(de__cvar_entry *e, const char *text, de__cvar_source src,
                            bool *changed) {
    *changed = false;
    if (e->flags & DE_CVAR_READONLY) {
        if (src == DE__CVAR_SRC_CONSOLE)
            return DE_FAIL(DE_ERR_UNSUPPORTED, "cvar %s is read-only", e->name);
        if (g_cvars.startup_locked)
            return DE_FAIL(DE_ERR_UNSUPPORTED,
                           "cvar %s is read-only and startup is over (de_cvar_lock_startup)",
                           e->name);
    }
    if ((e->flags & DE_CVAR_CHEAT) && !cheats_allowed())
        return DE_FAIL(DE_ERR_UNSUPPORTED, "cvar %s is a cheat; set cheats to 1 first", e->name);
    if (e->type == DE_CVAR_STRING)
        return publish_string_locked(e, text, changed);
    u64 bits;
    DE_TRY(parse_bits(e, text, &bits));
    if (de_atomic_load_u64(&e->bits, DE_ATOMIC_RELAXED) != bits) {
        de_atomic_store_u64(&e->bits, bits, DE_ATOMIC_RELEASE);
        *changed = true;
    }
    return DE_OK;
}

/* ---- Pending values ---------------------------------------------------------------------- */

static void pending_free_strings(pending_value *p) {
    DE_FREE(g_cvars.a, p->name, p->name_len + 1);
    DE_FREE(g_cvars.a, p->value, p->value_size);
    p->name = NULL;
    p->value = NULL;
}

/* Copies `len` bytes and a NUL from the registry allocator. */
static char *copy_str(const char *s, size_t len) {
    char *c = (char *)DE_ALLOC(g_cvars.a, len + 1, 1, DE_MEM_GENERAL);
    if (c != NULL) {
        memcpy(c, s, len);
        c[len] = '\0';
    }
    return c;
}

/* Remembers `text` for the unregistered name. Caller holds the lock. */
static de_result pending_put_locked(const char *name, size_t len, u64 hash, const char *text) {
    size_t value_size = strlen(text) + 1;
    char *value = copy_str(text, value_size - 1);
    if (value == NULL)
        return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "cvar %.*s: no memory for the pending value",
                            (int)len, name);
    const u32 *idx = DE_HASHMAP_GET(&g_cvars.pending_by_name, u32, hash, &hash);
    if (idx != NULL) {
        pending_value *p = &DE_ARRAY_AT(&g_cvars.pending, pending_value, *idx);
        if (p->name_len != len || memcmp(p->name, name, len) != 0) {
            DE_FREE(g_cvars.a, value, value_size);
            return DE_FAIL(DE_ERR_ALREADY_EXISTS,
                           "cvar %.*s: its name hash collides with the pending cvar %s", (int)len,
                           name, p->name);
        }
        DE_FREE(g_cvars.a, p->value, p->value_size);
        p->value = value;
        p->value_size = value_size;
        return DE_OK;
    }
    char *name_copy = copy_str(name, len);
    u32 index = g_cvars.pending.count;
    de_result r = name_copy != NULL ? DE_OK : DE_ERR_OUT_OF_MEMORY;
    if (r == DE_OK)
        r = DE_ARRAY_PUSH(&g_cvars.pending, pending_value,
                          (pending_value){name_copy, len, value, value_size});
    if (r == DE_OK) {
        r = de_hashmap_put(&g_cvars.pending_by_name, hash, &hash, &index);
        if (r != DE_OK)
            (void)de_array_pop(&g_cvars.pending, NULL);
    }
    if (r != DE_OK) {
        if (name_copy != NULL)
            DE_FREE(g_cvars.a, name_copy, len + 1);
        DE_FREE(g_cvars.a, value, value_size);
        return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "cvar %.*s: no memory for the pending value",
                            (int)len, name);
    }
    return DE_OK;
}

/* Takes the pending value of `name` (caller frees it with value_size), or returns false. */
static bool pending_take_locked(const char *name, size_t len, u64 hash, pending_value *out) {
    u32 index;
    const u32 *idx = DE_HASHMAP_GET(&g_cvars.pending_by_name, u32, hash, &hash);
    if (idx == NULL)
        return false;
    index = *idx;
    pending_value *p = &DE_ARRAY_AT(&g_cvars.pending, pending_value, index);
    if (p->name_len != len || memcmp(p->name, name, len) != 0)
        return false;
    *out = *p;
    (void)de_hashmap_remove(&g_cvars.pending_by_name, hash, &hash, NULL);
    DE_FREE(g_cvars.a, p->name, p->name_len + 1);
    p->name = NULL;
    p->value = NULL;
    return true;
}

/* ---- Registration ------------------------------------------------------------------------ */

static size_t align_up(size_t v, size_t a) {
    return (v + a - 1u) & ~(a - 1u);
}

static de_cvar invalid_handle(void) {
    return (de_cvar){0};
}

/* Validates the desc; on success `*default_bits` holds the default (not for strings). */
static de_result check_desc(const de_cvar_desc *d, size_t name_len, u64 *default_bits) {
    if (!de__cvar_name_valid(d->name, name_len))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "cvar \"%.*s\": names are 1 to %d dotted [a-z0-9_] segments, at most %d "
                       "bytes",
                       print_len(name_len), d->name, (int)DE_CVAR_NAME_MAX_PARTS,
                       (int)DE_CVAR_NAME_MAX);
    if ((u32)d->type >= (u32)DE_CVAR_TYPE_COUNT_)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "cvar %s: bad type %d", d->name, (int)d->type);
    bool clamp = d->min < d->max;
    switch (d->type) {
    case DE_CVAR_BOOL:
        *default_bits = d->default_number != 0.0 ? 1u : 0u;
        break;
    case DE_CVAR_INT: {
        i64 v = f64_to_i64_sat(d->default_number);
        if (clamp) {
            de__cvar_entry tmp = {.clamp = true, .min = d->min, .max = d->max};
            v = clamp_i64(&tmp, v);
        }
        *default_bits = (u64)v;
        break;
    }
    case DE_CVAR_FLOAT: {
        f64 v = d->default_number;
        if (v != v)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "cvar %s: the default is NaN", d->name);
        if (clamp)
            v = v < d->min ? d->min : (v > d->max ? d->max : v);
        *default_bits = f64_bits(v);
        break;
    }
    case DE_CVAR_ENUM: {
        if (d->enum_count == 0 || d->enum_values == NULL)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "cvar %s: an enum needs enum_values", d->name);
        for (u32 i = 0; i < d->enum_count; ++i)
            if (d->enum_values[i] == NULL || d->enum_values[i][0] == '\0')
                return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "cvar %s: enum value %u is empty", d->name,
                               i);
        if (d->default_string != NULL) {
            u32 i = 0;
            while (i < d->enum_count && strcmp(d->enum_values[i], d->default_string) != 0)
                ++i;
            if (i == d->enum_count)
                return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                               "cvar %s: the default \"%s\" is not one of its values", d->name,
                               d->default_string);
            *default_bits = i;
        } else {
            i64 i = f64_to_i64_sat(d->default_number);
            if (i < 0 || i >= (i64)d->enum_count)
                return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                               "cvar %s: the default index %lld is out of range", d->name,
                               (long long)i);
            *default_bits = (u64)i;
        }
        break;
    }
    case DE_CVAR_STRING:
    case DE_CVAR_TYPE_COUNT_:
        *default_bits = 0;
        break;
    }
    return DE_OK;
}

/* Fails if `name` is a dotted prefix of a registered name, or has a registered name as one. */
static de_result check_table_conflicts_locked(const char *name, size_t len, u64 hash,
                                              u32 *out_prefix_count) {
    if (de_hashmap_contains(&g_cvars.prefixes, hash, &hash))
        return DE_FAIL(DE_ERR_ALREADY_EXISTS,
                       "cvar %s: other cvars use it as a table (%s.*), so it cannot be a value",
                       name, name);
    u32 prefixes = 0;
    for (size_t i = 0; i < len; ++i) {
        if (name[i] != '.')
            continue;
        ++prefixes;
        u64 ph = de_hash64(name, i, 0);
        if (find_locked(name, i, ph) != UINT32_MAX)
            return DE_FAIL(DE_ERR_ALREADY_EXISTS,
                           "cvar %s: %.*s is a cvar, so it cannot also be a table", name, (int)i,
                           name);
    }
    *out_prefix_count = prefixes;
    return DE_OK;
}

/* Makes sure the chunk for `index` exists. */
static de_result ensure_chunk_locked(u32 index) {
    de_atomic_ptr *slot = &g_cvars.chunks[index >> CHUNK_SHIFT];
    if (de_atomic_load_ptr(slot, DE_ATOMIC_RELAXED) != NULL)
        return DE_OK;
    de_atomic_ptr *chunk = (de_atomic_ptr *)DE_ALLOC(g_cvars.a, sizeof(de_atomic_ptr) * CHUNK_SIZE,
                                                     DE_ALIGNOF(de_atomic_ptr), DE_MEM_GENERAL);
    if (chunk == NULL)
        return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "cvar registry: no memory for a handle chunk");
    for (u32 i = 0; i < CHUNK_SIZE; ++i)
        de_atomic_init_ptr(&chunk[i], NULL);
    de_atomic_store_ptr(slot, chunk, DE_ATOMIC_RELEASE);
    return DE_OK;
}

/* Builds the entry and its strings in one block. */
static de__cvar_entry *entry_create(const de_cvar_desc *d, size_t name_len) {
    const char *help = d->help != NULL ? d->help : "";
    size_t help_len = strlen(help);
    u32 enum_count = d->type == DE_CVAR_ENUM ? d->enum_count : 0;
    size_t enums_off = align_up(sizeof(de__cvar_entry), DE_ALIGNOF(const char *));
    size_t name_off = enums_off + sizeof(const char *) * enum_count;
    size_t help_off = name_off + name_len + 1;
    size_t size = help_off + help_len + 1;
    for (u32 i = 0; i < enum_count; ++i)
        size += strlen(d->enum_values[i]) + 1;

    u8 *block = (u8 *)DE_ALLOC(g_cvars.a, size, DE_ALIGNOF(de__cvar_entry), DE_MEM_GENERAL);
    if (block == NULL)
        return NULL;
    de__cvar_entry *e = (de__cvar_entry *)(void *)block;
    memset(e, 0, sizeof *e);
    const char **enums = (const char **)(void *)(block + enums_off);
    char *name = (char *)block + name_off;
    char *help_copy = (char *)block + help_off;
    memcpy(name, d->name, name_len);
    name[name_len] = '\0';
    memcpy(help_copy, help, help_len + 1);
    char *p = help_copy + help_len + 1;
    for (u32 i = 0; i < enum_count; ++i) {
        size_t n = strlen(d->enum_values[i]) + 1;
        memcpy(p, d->enum_values[i], n);
        enums[i] = p;
        p += n;
    }
    e->name = name;
    e->name_len = name_len;
    e->help = help_copy;
    e->enum_values = enum_count != 0 ? enums : NULL;
    e->enum_count = enum_count;
    e->type = d->type;
    e->flags = d->flags;
    e->clamp = d->min < d->max && (d->type == DE_CVAR_INT || d->type == DE_CVAR_FLOAT);
    e->min = d->min;
    e->max = d->max;
    e->block_size = size;
    e->on_change = d->on_change;
    e->user = d->user;
    de_atomic_init_u64(&e->bits, 0);
    de_atomic_init_ptr(&e->str, NULL);
    return e;
}

static void entry_destroy(de__cvar_entry *e) {
    de__cvar_strval *sv = e->strings;
    while (sv != NULL) {
        de__cvar_strval *next = sv->next;
        DE_FREE(g_cvars.a, sv, sizeof(de__cvar_strval) + sv->len + 1);
        sv = next;
    }
    DE_FREE(g_cvars.a, e, e->block_size);
}

static de_cvar register_locked(const de_cvar_desc *d) {
    size_t name_len = d->name != NULL ? strlen(d->name) : 0;
    u64 default_bits = 0;
    if (check_desc(d, name_len, &default_bits) != DE_OK)
        return invalid_handle();
#if CVAR_SHIPPING /* #if, not if: MSVC C4127 rejects a constant condition under /W4 /WX */
    if (d->flags & DE_CVAR_DEV_ONLY)
        return invalid_handle();
#endif

    u64 hash = de_name_from(d->name).hash;
    const u32 *existing = DE_HASHMAP_GET(&g_cvars.by_name, u32, hash, &hash);
    if (existing != NULL) {
        de__cvar_entry *e = DE_ARRAY_AT(&g_cvars.order, de__cvar_entry *, *existing);
        if (e->name_len != name_len || memcmp(e->name, d->name, name_len) != 0) {
            (void)DE_FAIL(DE_ERR_ALREADY_EXISTS, "cvar %s: its name hash collides with cvar %s",
                          d->name, e->name);
            return invalid_handle();
        }
        if (e->type != d->type) {
            (void)DE_FAIL(DE_ERR_ALREADY_EXISTS, "cvar %s is already registered with another type",
                          d->name);
            return invalid_handle();
        }
        e->on_change = d->on_change; /* a reloaded library brings new function pointers */
        e->user = d->user;
        return e->handle;
    }

    u32 prefix_count = 0;
    if (check_table_conflicts_locked(d->name, name_len, hash, &prefix_count) != DE_OK)
        return invalid_handle();
    u32 index = g_cvars.order.count;
    if (index >= DE_CVAR_MAX_COUNT) {
        (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY, "cvar %s: the registry is full (%u cvars)", d->name,
                      (unsigned)DE_CVAR_MAX_COUNT);
        return invalid_handle();
    }

    /* Everything that can fail happens before anything is published. The maps are never
     * removed from, so after the reserves the puts below cannot allocate or fail. */
    if (ensure_chunk_locked(index) != DE_OK ||
        de_array_reserve(&g_cvars.order, index + 1u) != DE_OK ||
        de_hashmap_reserve(&g_cvars.by_name, index + 1u) != DE_OK ||
        de_hashmap_reserve(&g_cvars.prefixes, de_hashmap_count(&g_cvars.prefixes) + prefix_count) !=
            DE_OK)
        return invalid_handle();
    de__cvar_entry *e = entry_create(d, name_len);
    if (e == NULL) {
        (void)DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "cvar %s: no memory for the entry", d->name);
        return invalid_handle();
    }
    de_atomic_init_u64(&e->bits, default_bits);
    if (d->type == DE_CVAR_STRING) {
        bool changed;
        if (publish_string_locked(e, d->default_string != NULL ? d->default_string : "",
                                  &changed) != DE_OK) {
            entry_destroy(e);
            return invalid_handle();
        }
    }
    u32 gen = de_atomic_load_u32(&g_cvars.generation, DE_ATOMIC_RELAXED);
    e->handle = (de_cvar){(gen << 16) | (index + 1u)};

    DE_VERIFY(de_array_push(&g_cvars.order, &e) == DE_OK);
    DE_VERIFY(de_hashmap_put(&g_cvars.by_name, hash, &hash, &index) == DE_OK);
    for (size_t i = 0; i < name_len; ++i) {
        if (d->name[i] != '.')
            continue;
        u64 ph = de_hash64(d->name, i, 0);
        DE_VERIFY(de_hashmap_put(&g_cvars.prefixes, ph, &ph, NULL) == DE_OK);
    }
    de_atomic_ptr *chunk = (de_atomic_ptr *)de_atomic_load_ptr(
        &g_cvars.chunks[index >> CHUNK_SHIFT], DE_ATOMIC_RELAXED);
    de_atomic_store_ptr(&chunk[index & (CHUNK_SIZE - 1)], e, DE_ATOMIC_RELEASE);

    /* A value from the command line or config.toml that arrived before the registration. */
    pending_value p;
    if (pending_take_locked(e->name, name_len, hash, &p)) {
        bool changed;
        if (set_locked(e, p.value, DE__CVAR_SRC_STARTUP, &changed) != DE_OK) {
            /* Registration still succeeds with the default; say so, since nobody else will. */
            const de_error *err = de_error_last();
            size_t n = strlen(p.value);
            DE_WARN(DE_LOG_CAT_CORE, "cvar %s: ignored the pending value \"%.*s\"%s: %s", e->name,
                    print_len(n), p.value, n > VALUE_PRINT_MAX ? "..." : "",
                    err != NULL ? err->message : "");
        }
        DE_FREE(g_cvars.a, p.value, p.value_size);
    }
    return e->handle;
}

/* ---- Public API -------------------------------------------------------------------------- */

de_cvar de_cvar_register(const de_cvar_desc *desc) {
    if (desc == NULL) {
        (void)DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_cvar_register: desc is NULL");
        return invalid_handle();
    }
    de__cvar_lock();
    de_cvar cv = register_locked(desc);
    de__cvar_unlock();
    return cv;
}

de_cvar de_cvar_find(const char *name) {
    if (name == NULL)
        return invalid_handle();
    size_t len = strlen(name);
    if (!de__cvar_name_valid(name, len))
        return invalid_handle();
    u64 hash = de_hash64(name, len, 0);
    de__cvar_lock();
    u32 index = find_locked(name, len, hash);
    de_cvar cv = index != UINT32_MAX ? DE_ARRAY_AT(&g_cvars.order, de__cvar_entry *, index)->handle
                                     : invalid_handle();
    de__cvar_unlock();
    return cv;
}

bool de_cvar_bool(de_cvar cv) {
    const de__cvar_entry *e = entry_of(cv);
    if (e == NULL || e->type == DE_CVAR_STRING)
        return false;
    u64 bits = de_atomic_load_u64(&e->bits, DE_ATOMIC_ACQUIRE);
    if (e->type == DE_CVAR_FLOAT)
        return bits_f64(bits) != 0.0;
    return bits != 0;
}

i64 de_cvar_int(de_cvar cv) {
    const de__cvar_entry *e = entry_of(cv);
    if (e == NULL || e->type == DE_CVAR_STRING)
        return 0;
    u64 bits = de_atomic_load_u64(&e->bits, DE_ATOMIC_ACQUIRE);
    if (e->type == DE_CVAR_FLOAT)
        return f64_to_i64_sat(bits_f64(bits));
    return (i64)bits;
}

f64 de_cvar_float(de_cvar cv) {
    const de__cvar_entry *e = entry_of(cv);
    if (e == NULL || e->type == DE_CVAR_STRING)
        return 0.0;
    u64 bits = de_atomic_load_u64(&e->bits, DE_ATOMIC_ACQUIRE);
    if (e->type == DE_CVAR_FLOAT)
        return bits_f64(bits);
    return (f64)(i64)bits;
}

const char *de_cvar_string(de_cvar cv) {
    const de__cvar_entry *e = entry_of(cv);
    if (e == NULL)
        return NULL;
    if (e->type == DE_CVAR_STRING)
        return (const char *)de_atomic_load_ptr(&e->str, DE_ATOMIC_ACQUIRE);
    if (e->type == DE_CVAR_ENUM) {
        u64 index = de_atomic_load_u64(&e->bits, DE_ATOMIC_ACQUIRE);
        return index < e->enum_count ? e->enum_values[index] : NULL;
    }
    return NULL;
}

const char *de_cvar_name(de_cvar cv) {
    const de__cvar_entry *e = entry_of(cv);
    return e != NULL ? e->name : NULL;
}

const char *de_cvar_help(de_cvar cv) {
    const de__cvar_entry *e = entry_of(cv);
    return e != NULL ? e->help : "";
}

de_cvar_type de_cvar_get_type(de_cvar cv) {
    const de__cvar_entry *e = entry_of(cv);
    return e != NULL ? e->type : DE_CVAR_BOOL;
}

u32 de_cvar_get_flags(de_cvar cv) {
    const de__cvar_entry *e = entry_of(cv);
    return e != NULL ? e->flags : 0u;
}

/* Sets `e` and calls its on_change without the lock. Takes the lock itself. */
static de_result set_and_notify(de__cvar_entry *e, const char *text, de__cvar_source src) {
    de__cvar_lock();
    bool changed = false;
    de_result r = set_locked(e, text, src, &changed);
    de_cvar_change_fn fn = e->on_change;
    void *user = e->user;
    de__cvar_unlock();
    if (r == DE_OK && changed && fn != NULL)
        fn(e->handle, user);
    return r;
}

de_result de_cvar_set(de_cvar cv, const char *value) {
    de__cvar_entry *e = entry_of(cv);
    if (e == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_cvar_set: invalid or stale cvar handle");
    if (value == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_cvar_set: cvar %s: value is NULL", e->name);
    return set_and_notify(e, value, DE__CVAR_SRC_CONSOLE);
}

de_result de__cvar_set_by_name(const char *name, size_t len, const char *text, de__cvar_source src,
                               const char *what) {
    if (!de__cvar_name_valid(name, len))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "%s: \"%.*s\" is not a cvar name (1 to %d dotted [a-z0-9_] segments, at "
                       "most %d bytes)",
                       what, print_len(len), name, (int)DE_CVAR_NAME_MAX_PARTS,
                       (int)DE_CVAR_NAME_MAX);
    u64 hash = de_hash64(name, len, 0);
    de__cvar_lock();
    u32 index = find_locked(name, len, hash);
    if (index == UINT32_MAX) {
        de_result r = pending_put_locked(name, len, hash, text);
        de__cvar_unlock();
        return r;
    }
    de__cvar_entry *e = DE_ARRAY_AT(&g_cvars.order, de__cvar_entry *, index);
    de__cvar_unlock();
    /* Entries never go away (except in a test reset), so the lock can be dropped here. */
    return set_and_notify(e, text, src);
}

void de__cvar_batch_note(de__cvar_batch *b, de_result r) {
    if (r == DE_OK)
        return;
    if (b->failures++ == 0) {
        b->first = r;
        const de_error *err = de_error_last();
        snprintf(b->message, sizeof b->message, "%s", err != NULL ? err->message : "");
    }
}

de_result de__cvar_batch_result(const de__cvar_batch *b) {
    if (b->failures == 0)
        return DE_OK;
    if (b->failures == 1)
        return b->first; /* the thread's error context still holds its message */
    return DE_FAIL(b->first, "%s (and %u more failures)", b->message, b->failures - 1u);
}

de_result de_cvar_apply_args(int argc, char **argv) {
    if (argc > 0 && argv == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_cvar_apply_args: argv is NULL");
    de__cvar_batch b = {DE_OK, 0, {0}};
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (arg == NULL || arg[0] != '+' || arg[1] == '\0')
            continue;
        const char *name = arg + 1;
        if (i + 1 >= argc || argv[i + 1] == NULL) {
            de__cvar_batch_note(
                &b, DE_FAIL(DE_ERR_INVALID_ARGUMENT, "command line: +%s has no value", name));
            continue;
        }
        const char *value = argv[++i];
        de__cvar_batch_note(&b, de__cvar_set_by_name(name, strlen(name), value,
                                                     DE__CVAR_SRC_STARTUP, "command line"));
    }
    return de__cvar_batch_result(&b);
}

void de_cvar_lock_startup(void) {
    de__cvar_lock();
    g_cvars.startup_locked = true;
    de__cvar_unlock();
}

void de_cvar_foreach_flag(u32 flags, de_cvar_visit_fn fn, void *user) {
    if (fn == NULL)
        return;
    de__cvar_lock();
    u32 count = g_cvars.order.count;
    de__cvar_unlock();
    for (u32 i = 0; i < count; ++i) {
        de__cvar_lock();
        const de__cvar_entry *e = DE_ARRAY_AT(&g_cvars.order, de__cvar_entry *, i);
        de__cvar_unlock();
        if ((e->flags & flags) == flags)
            fn(e->handle, user);
    }
}

/* ---- Test hooks -------------------------------------------------------------------------- */

/* Frees everything and starts a new generation, so earlier handles become stale. Only for tests:
 * no other thread may use cvars during or after the call with an old handle's string. */
void de__cvar_test_reset(void) {
    if (de_atomic_load_u32(&g_cvars.state, DE_ATOMIC_ACQUIRE) != STATE_READY)
        return;
    for (u32 i = 0; i < g_cvars.order.count; ++i)
        entry_destroy(DE_ARRAY_AT(&g_cvars.order, de__cvar_entry *, i));
    for (u32 i = 0; i < g_cvars.pending.count; ++i) {
        pending_value *p = &DE_ARRAY_AT(&g_cvars.pending, pending_value, i);
        if (p->name != NULL)
            pending_free_strings(p);
    }
    for (u32 i = 0; i < CHUNK_COUNT; ++i) {
        void *chunk = de_atomic_load_ptr(&g_cvars.chunks[i], DE_ATOMIC_RELAXED);
        if (chunk != NULL)
            DE_FREE(g_cvars.a, chunk, sizeof(de_atomic_ptr) * CHUNK_SIZE);
    }
    de_array_free(&g_cvars.order);
    de_array_free(&g_cvars.pending);
    de_hashmap_free(&g_cvars.by_name);
    de_hashmap_free(&g_cvars.prefixes);
    de_hashmap_free(&g_cvars.pending_by_name);
    de_mutex_destroy(&g_cvars.lock);
    u32 gen = de_atomic_load_u32(&g_cvars.generation, DE_ATOMIC_RELAXED) + 1u;
    de_atomic_store_u32(&g_cvars.generation, (gen & 0xFFFFu) == 0 ? 1u : (gen & 0xFFFFu),
                        DE_ATOMIC_RELAXED);
    de_atomic_store_u32(&g_cvars.state, STATE_UNINIT, DE_ATOMIC_RELEASE);
}

/* Holds the registry mutex, so a test can show that the getters do not take it. */
void de__cvar_test_lock(void) {
    de__cvar_lock();
}

void de__cvar_test_unlock(void) {
    de__cvar_unlock();
}
