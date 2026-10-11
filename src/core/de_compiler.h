/*
 * de_compiler.h: compiler portability macros for engine sources (internal, not installed).
 *
 * Include as "core/de_compiler.h". DE_NORETURN and DE_PRINTF_FMT are defined in the public
 * de_base.h, because public declarations (de_panic_, de_error_set_, de_log_) use them; this
 * header includes it so engine code gets every macro from one place.
 */
#ifndef DE_CORE_COMPILER_H
#define DE_CORE_COMPILER_H

#include <dengine/de_base.h>

/* Thread-local storage duration. MSVC's C mode has no _Thread_local; __declspec(thread) works
 * there and in clang-cl. */
#if defined(__cplusplus)
#define DE_THREAD_LOCAL thread_local
#elif defined(_MSC_VER)
#define DE_THREAD_LOCAL __declspec(thread)
#else
#define DE_THREAD_LOCAL _Thread_local
#endif

/* Branch hints. Use only on paths measured or known to be cold (errors, panics). */
#if defined(__GNUC__) || defined(__clang__)
#define DE_LIKELY(x)   __builtin_expect(!!(x), 1)
#define DE_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define DE_LIKELY(x)   (!!(x))
#define DE_UNLIKELY(x) (!!(x))
#endif

#endif /* DE_CORE_COMPILER_H */
