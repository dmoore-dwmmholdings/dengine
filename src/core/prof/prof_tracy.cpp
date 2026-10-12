/*
 * prof_tracy.cpp: the Tracy backend of the profiler (DENG-94; SPEC §18.1). C++ only because the
 * Tracy client is C++ (DENG-2 Q1); it uses nothing but Tracy's C API (tracy/TracyC.h) and exports
 * one C table, de__prof_tracy_backend (prof_internal.h). prof.c calls it only between startup and
 * shutdown.
 *
 * de_tp_tracy (third_party/tracy/CMakeLists.txt) builds the client with TRACY_MANUAL_LIFETIME and
 * TRACY_DELAYED_INIT, so nothing starts until de_prof_init, and with TRACY_ON_DEMAND, so nothing
 * is recorded until a profiler connects. Its compile definitions reach this file through the link,
 * so TracyCZoneCtx has the same layout here as in the client.
 */
#if defined(DE_PROFILE) && defined(DE_TRACY)

#include "core/prof/prof_internal.h"

#include <tracy/TracyC.h>

#include <cstddef>
#include <cstring>

#if !defined(TRACY_ENABLE) || !defined(TRACY_MANUAL_LIFETIME) || !defined(TRACY_ON_DEMAND)
#error "prof_tracy.cpp needs de_tp_tracy's definitions (TRACY_ENABLE, _MANUAL_LIFETIME, _ON_DEMAND)"
#endif

/* de_prof_srcloc is handed to Tracy as its source location struct: same members, same layout. */
static_assert(sizeof(de_prof_srcloc) == sizeof(___tracy_source_location_data), "srcloc size");
static_assert(offsetof(de_prof_srcloc, name) == offsetof(___tracy_source_location_data, name),
              "srcloc.name");
static_assert(offsetof(de_prof_srcloc, function) ==
                  offsetof(___tracy_source_location_data, function),
              "srcloc.function");
static_assert(offsetof(de_prof_srcloc, file) == offsetof(___tracy_source_location_data, file),
              "srcloc.file");
static_assert(offsetof(de_prof_srcloc, line) == offsetof(___tracy_source_location_data, line),
              "srcloc.line");
static_assert(offsetof(de_prof_srcloc, color) == offsetof(___tracy_source_location_data, color),
              "srcloc.color");

/* A zone context travels in de_prof_zone's opaque bytes. */
static_assert(sizeof(TracyCZoneCtx) <= sizeof(de_prof_zone), "de_prof_zone is too small");
static_assert(alignof(TracyCZoneCtx) <= alignof(de_prof_zone), "de_prof_zone is under-aligned");

namespace {

const ___tracy_source_location_data *srcloc(const de_prof_srcloc *loc) {
    return reinterpret_cast<const ___tracy_source_location_data *>(loc);
}

void startup() {
    ___tracy_startup_profiler();
}

void shutdown() {
    ___tracy_shutdown_profiler();
}

bool connected() {
    return ___tracy_connected() != 0;
}

de_prof_zone zone_begin(const de_prof_srcloc *loc) {
    const TracyCZoneCtx ctx = ___tracy_emit_zone_begin(srcloc(loc), 1);
    de_prof_zone z;
    std::memset(&z, 0, sizeof z);
    std::memcpy(&z, &ctx, sizeof ctx);
    return z;
}

void zone_end(de_prof_zone z) {
    TracyCZoneCtx ctx;
    std::memcpy(&ctx, &z, sizeof ctx);
    ___tracy_emit_zone_end(ctx);
}

void frame_mark(const char *name) {
    ___tracy_emit_frame_mark(name);
}

void plot(const char *name, f64 value) {
    ___tracy_emit_plot(name, value);
}

void alloc(const void *ptr, size_t size, const char *pool) {
    if (pool != nullptr)
        ___tracy_emit_memory_alloc_named(ptr, size, pool);
    else
        ___tracy_emit_memory_alloc(ptr, size);
}

void free_(const void *ptr, const char *pool) {
    if (pool != nullptr)
        ___tracy_emit_memory_free_named(ptr, pool);
    else
        ___tracy_emit_memory_free(ptr);
}

void message(const char *text, size_t len, de__prof_severity sev, u32 color) {
    static_assert(TracyMessageSeverityTrace == static_cast<int>(DE__PROF_SEV_TRACE) &&
                      TracyMessageSeverityFatal == static_cast<int>(DE__PROF_SEV_FATAL),
                  "severity order");
    ___tracy_emit_logString(static_cast<int8_t>(sev), static_cast<int32_t>(color), 0, len, text);
}

void thread_name(const char *name) {
    ___tracy_set_thread_name(name);
}

void *lock_announce(const de_prof_srcloc *loc) {
    return ___tracy_announce_lockable_ctx(srcloc(loc));
}

__tracy_lockable_context_data *lock_ctx(void *ctx) {
    return static_cast<__tracy_lockable_context_data *>(ctx);
}

void lock_terminate(void *ctx) {
    ___tracy_terminate_lockable_ctx(lock_ctx(ctx));
}

bool lock_before(void *ctx) {
    return ___tracy_before_lock_lockable_ctx(lock_ctx(ctx)) != 0;
}

void lock_after(void *ctx) {
    ___tracy_after_lock_lockable_ctx(lock_ctx(ctx));
}

void lock_after_try(void *ctx, bool acquired) {
    ___tracy_after_try_lock_lockable_ctx(lock_ctx(ctx), acquired ? 1 : 0);
}

void lock_after_unlock(void *ctx) {
    ___tracy_after_unlock_lockable_ctx(lock_ctx(ctx));
}

} // namespace

extern "C" const de__prof_backend de__prof_tracy_backend = {
    "tracy",       startup,        shutdown,    connected,  zone_begin,     zone_end,
    frame_mark,    plot,           alloc,       free_,      message,        thread_name,
    lock_announce, lock_terminate, lock_before, lock_after, lock_after_try, lock_after_unlock,
};

#endif /* DE_PROFILE && DE_TRACY */
