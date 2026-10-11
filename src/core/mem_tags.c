/* mem_tags.c: memory tags, per-tag stats and the allocation hooks (SPEC §5.3, §18.1–18.2).
 *
 * The tag table is static (64 names of 32 bytes), so registering and reporting never allocate
 * and work before anything else is initialised. Registration takes a small spinlock and
 * publishes the new count with a release store; readers load it with acquire.
 *
 * Stats are relaxed atomic counters: each one is exact, but a reader may see the four of a tag
 * from slightly different moments. The peak is raised with a CAS loop.
 *
 * Hooks are one pointer to caller storage. With none installed, a report costs the counter
 * updates plus one relaxed load. A thread-local flag stops a hook that allocates from calling
 * the hooks again on the same thread.
 */
#include <dengine/de_memory.h>

#include "core/de_compiler.h"
#include "core/mem_internal.h"
#include "platform/de_atomic.h"

#include <string.h>

#define DE__TAG_NAME_MAX 32

typedef struct de__tag_stats {
    de_atomic_u64 bytes_live;
    de_atomic_u64 bytes_peak;
    de_atomic_u64 alloc_count;
    de_atomic_u64 free_count;
} de__tag_stats;

static char de__tag_names[DE_MEM_TAG_MAX][DE__TAG_NAME_MAX] = {
    "general", "ecs", "asset", "render", "physics", "audio", "net", "ui", "scratch",
};
DE_STATIC_ASSERT(DE_MEM_TAG_BUILTIN_COUNT_ == 9, "name every builtin tag above");

static de_atomic_u32 de__tag_count = DE_ATOMIC_INIT(DE_MEM_TAG_BUILTIN_COUNT_);
static de_atomic_u32 de__tag_lock = DE_ATOMIC_INIT(0);
static de__tag_stats de__stats[DE_MEM_TAG_MAX];
static de_atomic_ptr de__hooks = DE_ATOMIC_INIT(NULL);
static DE_THREAD_LOCAL bool de__in_hook;

/* ---- Tags -------------------------------------------------------------------------------- */

static void de__tag_lock_acquire(void) {
    u32 expected = 0;
    while (!de_atomic_cas_weak_u32(&de__tag_lock, &expected, 1, DE_ATOMIC_ACQUIRE,
                                   DE_ATOMIC_RELAXED)) {
        expected = 0;
        de_cpu_pause();
    }
}

static void de__tag_lock_release(void) {
    de_atomic_store_u32(&de__tag_lock, 0, DE_ATOMIC_RELEASE);
}

/* Length of `name` as stored: cut to DE__TAG_NAME_MAX - 1 bytes. */
static size_t de__tag_name_len(const char *name) {
    size_t len = strlen(name);
    return len < DE__TAG_NAME_MAX ? len : DE__TAG_NAME_MAX - 1;
}

/* A long name matches its cut copy. */
static bool de__tag_name_eq(const char *stored, const char *name) {
    const size_t len = de__tag_name_len(name);
    return strlen(stored) == len && memcmp(stored, name, len) == 0;
}

u32 de_mem_register_tag(const char *name) {
    if (name == NULL || name[0] == '\0')
        return DE_MEM_GENERAL;
    de__tag_lock_acquire();
    const u32 n = de_atomic_load_u32(&de__tag_count, DE_ATOMIC_RELAXED);
    for (u32 i = 0; i < n; ++i) {
        if (de__tag_name_eq(de__tag_names[i], name)) {
            de__tag_lock_release();
            return i;
        }
    }
    if (n >= DE_MEM_TAG_MAX) {
        de__tag_lock_release();
        DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_mem_register_tag(%s): all %d tags are in use", name,
                DE_MEM_TAG_MAX);
        return DE_MEM_GENERAL;
    }
    const size_t len = de__tag_name_len(name);
    memcpy(de__tag_names[n], name, len);
    de__tag_names[n][len] = '\0';
    de_atomic_store_u32(&de__tag_count, n + 1, DE_ATOMIC_RELEASE);
    de__tag_lock_release();
    return n;
}

u32 de_mem_tag_count(void) {
    return de_atomic_load_u32(&de__tag_count, DE_ATOMIC_ACQUIRE);
}

const char *de_mem_tag_name(u32 tag) {
    if (tag >= de_mem_tag_count())
        return NULL;
    return de__tag_names[tag];
}

/* ---- Stats ------------------------------------------------------------------------------- */

de_mem_stats de_mem_stats_for_tag(u32 tag) {
    de_mem_stats s;
    memset(&s, 0, sizeof s);
    if (tag >= DE_MEM_TAG_MAX)
        return s;
    const de__tag_stats *t = &de__stats[tag];
    s.bytes_live = de_atomic_load_u64(&t->bytes_live, DE_ATOMIC_RELAXED);
    s.bytes_peak = de_atomic_load_u64(&t->bytes_peak, DE_ATOMIC_RELAXED);
    s.alloc_count = de_atomic_load_u64(&t->alloc_count, DE_ATOMIC_RELAXED);
    s.free_count = de_atomic_load_u64(&t->free_count, DE_ATOMIC_RELAXED);
    return s;
}

/* Unknown tags are counted as general. */
static inline u32 de__clamp_tag(u32 tag) {
    return tag < DE_MEM_TAG_MAX ? tag : DE_MEM_GENERAL;
}

/* ---- Hooks ------------------------------------------------------------------------------- */

const de_mem_hooks *de_mem_set_hooks(const de_mem_hooks *hooks) {
    /* acq_rel: publishes *hooks to reporters, and lets the caller read the old hooks safely. */
    return (const de_mem_hooks *)de_atomic_exchange_ptr(&de__hooks, (void *)(uintptr_t)hooks,
                                                        DE_ATOMIC_ACQ_REL);
}

/* The installed hooks, or NULL. One relaxed load when none are installed; the acquire load that
 * follows otherwise makes the hook struct's fields visible. */
static inline const de_mem_hooks *de__hooks_get(void) {
    if (DE_LIKELY(de_atomic_load_ptr(&de__hooks, DE_ATOMIC_RELAXED) == NULL))
        return NULL;
    return (const de_mem_hooks *)de_atomic_load_ptr(&de__hooks, DE_ATOMIC_ACQUIRE);
}

/* ---- Reporting --------------------------------------------------------------------------- */

void de__mem_report_alloc(void *ptr, size_t size, u32 tag) {
    tag = de__clamp_tag(tag);
    de__tag_stats *t = &de__stats[tag];
    de_atomic_fetch_add_u64(&t->alloc_count, 1, DE_ATOMIC_RELAXED);
    const u64 live = de_atomic_fetch_add_u64(&t->bytes_live, (u64)size, DE_ATOMIC_RELAXED) + size;
    u64 peak = de_atomic_load_u64(&t->bytes_peak, DE_ATOMIC_RELAXED);
    while (live > peak && !de_atomic_cas_weak_u64(&t->bytes_peak, &peak, live, DE_ATOMIC_RELAXED,
                                                  DE_ATOMIC_RELAXED)) {
    }

    const de_mem_hooks *h = de__hooks_get();
    if (h != NULL && h->on_alloc != NULL && !de__in_hook) {
        de__in_hook = true;
        h->on_alloc(h->user, ptr, size, tag);
        de__in_hook = false;
    }
}

void de__mem_report_free(void *ptr, size_t size, u32 tag) {
    tag = de__clamp_tag(tag);
    const de_mem_hooks *h = de__hooks_get();
    if (h != NULL && h->on_free != NULL && !de__in_hook) {
        de__in_hook = true;
        h->on_free(h->user, ptr, size, tag);
        de__in_hook = false;
    }

    de__tag_stats *t = &de__stats[tag];
    de_atomic_fetch_add_u64(&t->free_count, 1, DE_ATOMIC_RELAXED);
    de_atomic_fetch_sub_u64(&t->bytes_live, (u64)size, DE_ATOMIC_RELAXED);
}
