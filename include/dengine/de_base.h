/* de_base.h — core types, results, error context, asserts, logging.
 * DEngine API sketch v0.1. See SPEC.md §5.1–5.2. */
#ifndef DE_BASE_H
#define DE_BASE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
#define DE_EXTERN_C_BEGIN extern "C" {
#define DE_EXTERN_C_END }
#else
#define DE_EXTERN_C_BEGIN
#define DE_EXTERN_C_END
#endif

/* Export macro. DE_BUILD_SHARED is defined when building the engine as a shared library. */
#if defined(DE_STATIC)
#  define DE_API
#elif defined(_WIN32)
#  ifdef DE_BUILD_SHARED
#    define DE_API __declspec(dllexport)
#  else
#    define DE_API __declspec(dllimport)
#  endif
#else
#  define DE_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
#  define DE_ALIGN(n)    alignas(n)
#  define DE_ALIGNOF(T)  alignof(T)
#  define DE_STATIC_ASSERT(c, msg) static_assert(c, msg)
#else
#  define DE_ALIGN(n)    _Alignas(n)
#  define DE_ALIGNOF(T)  _Alignof(T)
#  define DE_STATIC_ASSERT(c, msg) _Static_assert(c, msg)
#endif

DE_EXTERN_C_BEGIN

typedef uint8_t  u8;  typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t   i8;  typedef int16_t  i16; typedef int32_t  i32; typedef int64_t  i64;
typedef float    f32; typedef double   f64;

/* Declares a typed handle. Zero is always invalid. Low bits index, high bits generation. */
#define DE_HANDLE(name) typedef struct name { u32 id; } name
#define DE_HANDLE_VALID(h) ((h).id != 0)

/* Stable 64-bit hashed name (asset IDs, interned strings). */
typedef struct de_name { u64 hash; } de_name;
DE_API de_name de_name_from(const char *str);          /* interns in dev builds for reverse lookup */
DE_API const char *de_name_str(de_name n);              /* NULL in shipping builds */

typedef struct de_str { const char *ptr; size_t len; } de_str;   /* non-owning view */
#define DE_STR(lit) ((de_str){ (lit), sizeof(lit) - 1 })

/* ---------- Results and error context ---------- */

typedef enum de_result {
    DE_OK = 0,
    DE_ERR_INVALID_ARGUMENT,
    DE_ERR_OUT_OF_MEMORY,
    DE_ERR_IO,
    DE_ERR_NOT_FOUND,
    DE_ERR_ALREADY_EXISTS,
    DE_ERR_UNSUPPORTED,
    DE_ERR_PARSE,
    DE_ERR_VERSION_MISMATCH,
    DE_ERR_DEVICE_LOST,
    DE_ERR_TIMEOUT,
    DE_ERR_BUSY,
    DE_ERR_CANCELLED,
    DE_ERR_NETWORK,
    DE_ERR_INTERNAL,
    DE_RESULT_COUNT_
} de_result;

typedef struct de_error {
    de_result        result;
    char             message[512];
    const char      *file;
    const char      *function;
    u32              line;
    const struct de_error *cause;   /* optional chain, owned by the thread's error arena */
} de_error;

DE_API const char     *de_result_str(de_result r);
DE_API const de_error *de_error_last(void);              /* thread-local; NULL if none */
DE_API void            de_error_clear(void);
DE_API de_result       de_error_set_(de_result r, const char *file, u32 line,
                                     const char *func, const char *fmt, ...);
/* Set error context and return the code: `return DE_FAIL(DE_ERR_IO, "open %s", path);` */
#define DE_FAIL(r, ...) de_error_set_((r), __FILE__, __LINE__, __func__, __VA_ARGS__)
#define DE_TRY(expr) do { de_result r__ = (expr); if (r__ != DE_OK) return r__; } while (0)

/* ---------- Asserts ---------- */

DE_API void de_panic_(const char *file, u32 line, const char *func, const char *fmt, ...);
#define DE_PANIC(...) de_panic_(__FILE__, __LINE__, __func__, __VA_ARGS__)
#define DE_VERIFY(c) do { if (!(c)) DE_PANIC("verify failed: %s", #c); } while (0)
#if defined(DE_DEBUG) || defined(DE_DEV)
#  define DE_ASSERT(c) DE_VERIFY(c)
#else
#  define DE_ASSERT(c) ((void)0)
#endif

/* ---------- Logging ---------- */

typedef enum de_log_level {
    DE_LOG_TRACE, DE_LOG_DEBUG, DE_LOG_INFO, DE_LOG_WARN, DE_LOG_ERROR, DE_LOG_FATAL
} de_log_level;

typedef struct de_log_category { u32 id; } de_log_category;

typedef struct de_log_record {
    de_log_level    level;
    de_log_category category;
    u64             timestamp_ns;
    u32             thread_id;
    const char     *file;
    u32             line;
    de_str          message;
} de_log_record;

typedef void (*de_log_sink_fn)(const de_log_record *rec, void *user);

DE_API de_log_category de_log_register_category(const char *name, de_log_level default_level);
DE_API void de_log_set_level(de_log_category cat, de_log_level level);
DE_API void de_log_add_sink(de_log_sink_fn fn, void *user);
DE_API void de_log_(de_log_level lvl, de_log_category cat, const char *file, u32 line,
                    const char *fmt, ...);
#define DE_LOG(lvl, cat, ...) de_log_((lvl), (cat), __FILE__, __LINE__, __VA_ARGS__)
#define DE_INFO(cat, ...)  DE_LOG(DE_LOG_INFO,  cat, __VA_ARGS__)
#define DE_WARN(cat, ...)  DE_LOG(DE_LOG_WARN,  cat, __VA_ARGS__)
#define DE_ERROR(cat, ...) DE_LOG(DE_LOG_ERROR, cat, __VA_ARGS__)

/* ---------- Version ---------- */

#define DE_VERSION_MAJOR 0
#define DE_VERSION_MINOR 1
#define DE_VERSION_PATCH 0
DE_API u32 de_version(void);   /* (major << 22) | (minor << 12) | patch */

DE_EXTERN_C_END
#endif /* DE_BASE_H */
