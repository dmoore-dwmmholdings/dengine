/* rt_guard.c: the real-time guard of the audio threads (DENG-308; SPEC §14 "no allocation or
 * locks on the audio thread").
 *
 * Debug and Dev builds: a thread-local depth marks the mixer thread, the device render callbacks
 * and de__audio_null_pump as audio threads. Allocation hooks (de_mem_set_hooks: every block of the
 * heap and of tracking allocators) and lock hooks (de_lock_set_hooks: de_mutex_lock, try_lock and
 * the rwlock locks) check the mark. A hit is counted and, unless a test switched it off, panics
 * with what happened. Release and Shipping builds compile everything here to nothing.
 *
 * The hooks are process-wide and installed by de_audio_create (first create installs, last destroy
 * removes). They chain to the hooks installed before them, so the alloc guard of the tests and
 * Tracy keep working; the usual rule holds: install and uninstall in reverse order. */
#include "audio/audio_internal.h"

#include "core/de_compiler.h"

#include <stdint.h>

#if defined(DE_DEBUG) || defined(DE_DEV)
#define DE__RT_GUARD 1
#else
#define DE__RT_GUARD 0
#endif

#if DE__RT_GUARD

static DE_THREAD_LOCAL u32 tl_depth;

static de_atomic_u64 g_hits = DE_ATOMIC_INIT(0);
static de_atomic_u32 g_fatal = DE_ATOMIC_INIT(1);
/* Install count; install and uninstall run on non-audio threads that create and destroy audio
 * systems, which may overlap, so the slot changes under a tiny spin lock (`g_busy`). */
static de_atomic_u32 g_busy = DE_ATOMIC_INIT(0);
static u32 g_installs;
/* The hooks installed before ours. Atomic: hooks read them on any thread. */
static de_atomic_ptr g_prev_mem = DE_ATOMIC_INIT(NULL);
static de_atomic_ptr g_prev_lock = DE_ATOMIC_INIT(NULL);

static void hit(const char *what, size_t size) {
    de_atomic_fetch_add_u64(&g_hits, 1, DE_ATOMIC_RELAXED);
    if (de_atomic_load_u32(&g_fatal, DE_ATOMIC_RELAXED))
        DE_PANIC("audio real-time guard: %s on an audio thread (%zu bytes)", what, size);
}

static void on_alloc(void *user, void *ptr, size_t size, u32 tag) {
    (void)user;
    if (tl_depth != 0)
        hit("allocation", size);
    const de_mem_hooks *prev =
        (const de_mem_hooks *)de_atomic_load_ptr(&g_prev_mem, DE_ATOMIC_ACQUIRE);
    if (prev != NULL && prev->on_alloc != NULL)
        prev->on_alloc(prev->user, ptr, size, tag);
}

static void on_free(void *user, void *ptr, size_t size, u32 tag) {
    (void)user;
    if (tl_depth != 0)
        hit("free", size);
    const de_mem_hooks *prev =
        (const de_mem_hooks *)de_atomic_load_ptr(&g_prev_mem, DE_ATOMIC_ACQUIRE);
    if (prev != NULL && prev->on_free != NULL)
        prev->on_free(prev->user, ptr, size, tag);
}

static void on_lock(void *user, const void *lock) {
    (void)user;
    if (tl_depth != 0)
        hit("lock", 0);
    const de_lock_hooks *prev =
        (const de_lock_hooks *)de_atomic_load_ptr(&g_prev_lock, DE_ATOMIC_ACQUIRE);
    if (prev != NULL && prev->on_lock != NULL)
        prev->on_lock(prev->user, lock);
}

static const de_mem_hooks k_mem_hooks = {on_alloc, on_free, NULL};
static const de_lock_hooks k_lock_hooks = {on_lock, NULL};

static void busy_lock(void) {
    u32 expected = 0;
    while (!de_atomic_cas_weak_u32(&g_busy, &expected, 1, DE_ATOMIC_ACQUIRE, DE_ATOMIC_RELAXED)) {
        expected = 0;
        de_cpu_pause();
    }
}

static void busy_unlock(void) {
    de_atomic_store_u32(&g_busy, 0, DE_ATOMIC_RELEASE);
}

bool de__rt_guard_enabled(void) {
    return true;
}

void de__rt_guard_enter(void) {
    ++tl_depth;
}

void de__rt_guard_leave(void) {
    DE_ASSERT(tl_depth > 0);
    --tl_depth;
}

bool de__rt_guard_active(void) {
    return tl_depth != 0;
}

void de__rt_guard_install(void) {
    busy_lock();
    if (g_installs++ == 0) {
        /* A hook that runs between the set and the store below skips the chain once. Install
         * before the hooks of a region that must see every block (the tests' alloc guard). */
        const de_mem_hooks *pm = de_mem_set_hooks(&k_mem_hooks);
        const de_lock_hooks *pl = de_lock_set_hooks(&k_lock_hooks);
        de_atomic_store_ptr(&g_prev_mem, (void *)(uintptr_t)pm, DE_ATOMIC_RELEASE);
        de_atomic_store_ptr(&g_prev_lock, (void *)(uintptr_t)pl, DE_ATOMIC_RELEASE);
    }
    busy_unlock();
}

void de__rt_guard_uninstall(void) {
    busy_lock();
    DE_ASSERT(g_installs > 0);
    if (g_installs > 0 && --g_installs == 0) {
        de_mem_set_hooks((const de_mem_hooks *)de_atomic_load_ptr(&g_prev_mem, DE_ATOMIC_ACQUIRE));
        de_lock_set_hooks(
            (const de_lock_hooks *)de_atomic_load_ptr(&g_prev_lock, DE_ATOMIC_ACQUIRE));
        de_atomic_store_ptr(&g_prev_mem, NULL, DE_ATOMIC_RELEASE);
        de_atomic_store_ptr(&g_prev_lock, NULL, DE_ATOMIC_RELEASE);
    }
    busy_unlock();
}

u64 de__rt_guard_hits(void) {
    return de_atomic_load_u64(&g_hits, DE_ATOMIC_RELAXED);
}

void de__rt_guard_set_fatal(bool fatal) {
    de_atomic_store_u32(&g_fatal, fatal ? 1u : 0u, DE_ATOMIC_RELAXED);
}

#else /* !DE__RT_GUARD */

bool de__rt_guard_enabled(void) {
    return false;
}
void de__rt_guard_enter(void) {}
void de__rt_guard_leave(void) {}
bool de__rt_guard_active(void) {
    return false;
}
void de__rt_guard_install(void) {}
void de__rt_guard_uninstall(void) {}
u64 de__rt_guard_hits(void) {
    return 0;
}
void de__rt_guard_set_fatal(bool fatal) {
    (void)fatal;
}

#endif
