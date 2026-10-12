/*
 * prof_internal.h: the profiler backend interface (DENG-94; SPEC §18.1). Internal to src/core/prof/
 * and its tests; include as "core/prof/prof_internal.h".
 *
 * prof.c is the C side of de_debug.h's profiling section: the running state, the memory hooks,
 * the log sink, the lock bookkeeping. It forwards each event to one backend, a table of the
 * functions below:
 *   - de__prof_tracy_backend (prof_tracy.cpp): Tracy's C API. Built when the build has Tracy
 *     (DE_TRACY, defined on de_core when de_tp_tracy exists).
 *   - de__prof_mock_backend: defined by tests/unit/core/test_prof.c. The test compiles its own copy
 *     of prof.c with DE_PROF_MOCK, which selects it, so the test sees every call in order.
 *   - none (DE_TRACY off): de_prof_init returns DE_ERR_UNSUPPORTED.
 * The backend is only called between its startup and shutdown, so its functions need no
 * "running" check of their own. Names are passed through as given (the backend may keep them).
 */
#ifndef DE_CORE_PROF_INTERNAL_H
#define DE_CORE_PROF_INTERNAL_H

#include <dengine/de_base.h>
#include <dengine/de_debug.h>

DE_EXTERN_C_BEGIN

/** Severity of a backend message, the same order as de_log_level. */
typedef enum de__prof_severity {
    DE__PROF_SEV_TRACE,
    DE__PROF_SEV_DEBUG,
    DE__PROF_SEV_INFO,
    DE__PROF_SEV_WARN,
    DE__PROF_SEV_ERROR,
    DE__PROF_SEV_FATAL,
} de__prof_severity;

typedef struct de__prof_backend {
    const char *name;
    void (*startup)(void);
    void (*shutdown)(void);
    bool (*connected)(void);
    de_prof_zone (*zone_begin)(const de_prof_srcloc *loc);
    void (*zone_end)(de_prof_zone z);
    void (*frame_mark)(const char *name);
    void (*plot)(const char *name, f64 value);
    void (*alloc)(const void *ptr, size_t size, const char *pool);
    void (*free)(const void *ptr, const char *pool);
    /** `text` is `len` bytes, not NUL-terminated; the backend copies it. `color` is 0xRRGGBB. */
    void (*message)(const char *text, size_t len, de__prof_severity sev, u32 color);
    void (*thread_name)(const char *name);
    void *(*lock_announce)(const de_prof_srcloc *loc);
    void (*lock_terminate)(void *ctx);
    bool (*lock_before)(void *ctx);
    void (*lock_after)(void *ctx);
    void (*lock_after_try)(void *ctx, bool acquired);
    void (*lock_after_unlock)(void *ctx);
} de__prof_backend;

#if defined(DE_PROF_MOCK)
extern const de__prof_backend de__prof_mock_backend;
#elif defined(DE_TRACY)
extern const de__prof_backend de__prof_tracy_backend;
#endif

/** Colours of the log messages sent by the profiler log sink (0xRRGGBB). */
#define DE__PROF_COLOR_WARN  0xFFC107u
#define DE__PROF_COLOR_ERROR 0xFF4040u
#define DE__PROF_COLOR_FATAL 0xFF40FFu

DE_EXTERN_C_END

#endif /* DE_CORE_PROF_INTERNAL_H */
