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
 *
 * GPU zones (DENG-326, SPEC §18.1): src/gpu/query.c also includes this header, for the
 * de__prof_gpu_* functions at the end (DE_PROFILE builds). They take plain integers and strings,
 * so core never includes a gpu header (layering); the backend entries behind them are optional
 * (NULL in a backend without GPU zones, such as the test mock).
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
    /* GPU zones (DENG-326). Optional: NULL when the backend has none. `ctx` comes from
     * de__prof_gpu_context; `query` ids are unique among the zones in flight. Names are copied.
     * gpu_context: a new GPU timeline whose clock reads `gpu_time` now, `period_ns` ns per tick,
     * of kind `api` (DE__PROF_GPU_API_*), named `name` (`len` bytes, not NUL-terminated). */
    void (*gpu_context)(u8 ctx, const char *name, size_t len, i64 gpu_time, f32 period_ns, u8 api);
    void (*gpu_zone_begin)(u8 ctx, u16 query, const char *name, size_t len, u32 color,
                           i64 gpu_time);
    void (*gpu_zone_end)(u8 ctx, u16 query, i64 gpu_time);
} de__prof_backend;

#if defined(DE_PROF_MOCK)
extern const de__prof_backend de__prof_mock_backend;
#elif defined(DE_TRACY)
extern const de__prof_backend de__prof_tracy_backend;
#endif

/* ---------- GPU zones (DENG-326) ----------
 * For src/gpu/query.c (de_gpu_profile_zones), DE_PROFILE builds only. Each call does nothing
 * while the profiler is stopped or when its backend has no GPU zones. */
#if defined(DE_PROFILE)

/** Kinds of GPU timeline, numbered as Tracy's GpuContextType. */
#define DE__PROF_GPU_API_VULKAN 2u
#define DE__PROF_GPU_API_D3D12  4u
#define DE__PROF_GPU_API_METAL  6u
#define DE__PROF_GPU_API_CUSTOM 7u
/** No context (profiler stopped, no GPU zones in the backend, or all 255 ids used). */
#define DE__PROF_GPU_NO_CONTEXT 0xFFFFFFFFu

/** The running profiler's span (it changes with every de_prof_init), 0 while it is stopped. A
 * GPU context belongs to the span it was made in. */
u32 de__prof_gpu_span(void);
/** A new GPU context (0 .. 254) whose clock reads `gpu_time` now and ticks every `period_ns`
 * nanoseconds; `name` is copied. DE__PROF_GPU_NO_CONTEXT when there is none. Ids are never
 * reused in a process (Tracy has 255). */
u32 de__prof_gpu_context(const char *name, i64 gpu_time, f32 period_ns, u32 api);
/** Opens a GPU zone of context `ctx` at `gpu_time` (`name` copied, `color` 0xRRGGBB). */
void de__prof_gpu_zone_begin(u32 ctx, const char *name, u32 color, i64 gpu_time);
/** Closes the innermost open GPU zone of context `ctx` at `gpu_time`. */
void de__prof_gpu_zone_end(u32 ctx, i64 gpu_time);

#endif /* DE_PROFILE */

/** Colours of the log messages sent by the profiler log sink (0xRRGGBB). */
#define DE__PROF_COLOR_WARN  0xFFC107u
#define DE__PROF_COLOR_ERROR 0xFF4040u
#define DE__PROF_COLOR_FATAL 0xFF40FFu

DE_EXTERN_C_END

#endif /* DE_CORE_PROF_INTERNAL_H */
