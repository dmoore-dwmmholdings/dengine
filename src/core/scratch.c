/* scratch.c: per-thread scratch arenas (SPEC §5.3).
 *
 * Each thread has two arenas, created on its first de_scratch_begin that needs them. Two are
 * enough for the usual pattern: a function gets an arena for its result (maybe the caller's
 * scratch) and needs temporary memory that must not alias it, so it passes the result arena as a
 * conflict and gets the other one.
 *
 * The arena pointers are in DE_THREAD_LOCAL storage (the fast path). Each arena is also stored in
 * a thread-specific key (POSIX pthread key, Windows FLS slot) whose destructor releases it when
 * the thread exits, so short-lived threads do not leak address space. Do not begin a scratch from
 * another thread-exit destructor that may run after ours: the arena would be created again and
 * released by the next destructor round (POSIX) or leaked (Windows).
 */
#include <dengine/de_memory.h>

#include "core/de_compiler.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

#define DE__SCRATCH_COUNT 2
/* A scratch arena that grew past this keeps only this much committed after a reset, so one huge
 * temporary buffer does not pin its memory for the thread's lifetime. */
#define DE__SCRATCH_DECOMMIT_THRESHOLD ((size_t)64 << 20)

static DE_THREAD_LOCAL de_arena *de__scratch[DE__SCRATCH_COUNT];

static void de__scratch_release(void *p) {
    de_arena *a = (de_arena *)p;
    if (a == NULL)
        return;
    for (u32 i = 0; i < DE__SCRATCH_COUNT; ++i)
        if (de__scratch[i] == a)
            de__scratch[i] = NULL;
    de_arena_destroy(a);
}

/* ---- Thread-exit cleanup ------------------------------------------------------------------- */

#if defined(_WIN32)
static DWORD de__scratch_fls[DE__SCRATCH_COUNT];
static bool de__scratch_fls_ok;
static INIT_ONCE de__scratch_once = INIT_ONCE_STATIC_INIT;

static void NTAPI de__scratch_fls_callback(void *p) {
    de__scratch_release(p);
}

static BOOL CALLBACK de__scratch_init_keys(PINIT_ONCE once, void *param, void **ctx) {
    (void)once;
    (void)param;
    (void)ctx;
    bool ok = true;
    for (u32 i = 0; i < DE__SCRATCH_COUNT; ++i) {
        de__scratch_fls[i] = FlsAlloc(de__scratch_fls_callback);
        ok = ok && de__scratch_fls[i] != FLS_OUT_OF_INDEXES;
    }
    de__scratch_fls_ok = ok;
    return TRUE;
}

static void de__scratch_register(u32 i, de_arena *a) {
    InitOnceExecuteOnce(&de__scratch_once, de__scratch_init_keys, NULL, NULL);
    if (de__scratch_fls_ok)
        FlsSetValue(de__scratch_fls[i], a);
}
#else
static pthread_key_t de__scratch_key[DE__SCRATCH_COUNT];
static bool de__scratch_key_ok;
static pthread_once_t de__scratch_once = PTHREAD_ONCE_INIT;

static void de__scratch_init_keys(void) {
    bool ok = true;
    for (u32 i = 0; i < DE__SCRATCH_COUNT; ++i)
        ok = ok && pthread_key_create(&de__scratch_key[i], de__scratch_release) == 0;
    de__scratch_key_ok = ok;
}

static void de__scratch_register(u32 i, de_arena *a) {
    pthread_once(&de__scratch_once, de__scratch_init_keys);
    if (de__scratch_key_ok)
        pthread_setspecific(de__scratch_key[i], a);
}
#endif

/* ---- Scratch scopes ------------------------------------------------------------------------ */

static de_arena *de__scratch_get(u32 i) {
    de_arena *a = de__scratch[i];
    if (DE_LIKELY(a != NULL))
        return a;
    static const char *const names[DE__SCRATCH_COUNT] = {"scratch0", "scratch1"};
    de_arena_desc d = {0};
    d.tag = DE_MEM_SCRATCH;
    d.name = names[i];
    d.decommit_threshold = DE__SCRATCH_DECOMMIT_THRESHOLD;
    if (de_arena_create(&d, &a) != DE_OK)
        DE_PANIC("thread scratch: cannot create arena %u: %s", i, de_error_last()->message);
    de__scratch[i] = a;
    de__scratch_register(i, a);
    return a;
}

static bool de__in_conflicts(const de_arena *a, de_arena *const *conflicts, u32 n) {
    for (u32 k = 0; k < n; ++k)
        if (conflicts[k] == a)
            return true;
    return false;
}

de_scratch de_scratch_begin(de_arena *const *conflicts, u32 conflict_count) {
    if (conflicts == NULL)
        conflict_count = 0;
    for (u32 i = 0; i < DE__SCRATCH_COUNT; ++i) {
        /* A slot not created yet cannot be in `conflicts`. */
        const de_arena *cur = de__scratch[i];
        if (cur != NULL && de__in_conflicts(cur, conflicts, conflict_count))
            continue;
        de_scratch s;
        s.arena = de__scratch_get(i);
        s.mark = de_arena_mark_get(s.arena);
        return s;
    }
    DE_PANIC("de_scratch_begin: all %d scratch arenas of this thread are in conflicts",
             DE__SCRATCH_COUNT);
}

void de_scratch_end(de_scratch s) {
    DE_ASSERT(s.arena != NULL);
    DE_ASSERT(s.arena == de__scratch[0] || s.arena == de__scratch[1]);
    de_arena_reset_to(s.arena, s.mark);
}
