/* de_base.h — core types, results, error context, asserts, panic, logging.
 * DEngine API sketch v0.1. See SPEC.md §5.1–5.2. */
#ifndef DE_BASE_H
#define DE_BASE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
#define DE_EXTERN_C_BEGIN extern "C" {
#define DE_EXTERN_C_END   }
#else
#define DE_EXTERN_C_BEGIN
#define DE_EXTERN_C_END
#endif

/* Export macro. DE_BUILD_SHARED is defined when building the engine as a shared library. */
#if defined(DE_STATIC)
#define DE_API
#elif defined(_WIN32)
#ifdef DE_BUILD_SHARED
#define DE_API __declspec(dllexport)
#else
#define DE_API __declspec(dllimport)
#endif
#else
#define DE_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
#define DE_ALIGN(n)              alignas(n)
#define DE_ALIGNOF(T)            alignof(T)
#define DE_STATIC_ASSERT(c, msg) static_assert(c, msg)
#else
#define DE_ALIGN(n)              _Alignas(n)
#define DE_ALIGNOF(T)            _Alignof(T)
#define DE_STATIC_ASSERT(c, msg) _Static_assert(c, msg)
#endif

/**
 * Marks a function that never returns. Put it first, before DE_API:
 * `DE_NORETURN DE_API void f(void);`.
 */
#if defined(__cplusplus)
#define DE_NORETURN [[noreturn]]
#elif defined(_MSC_VER) && !defined(__clang__)
#define DE_NORETURN __declspec(noreturn)
#else
#define DE_NORETURN _Noreturn
#endif

/**
 * printf-style format checking. `fmt_index` is the 1-based position of the format parameter,
 * `args_index` the position of the `...`. Put it after the parameter list. No-op on MSVC.
 */
#if defined(__GNUC__) || defined(__clang__)
#define DE_PRINTF_FMT(fmt_index, args_index) __attribute__((format(printf, fmt_index, args_index)))
#else
#define DE_PRINTF_FMT(fmt_index, args_index)
#endif

DE_EXTERN_C_BEGIN

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
typedef float f32;
typedef double f64;

/* Declares a typed handle. Zero is always invalid. Low bits index, high bits generation. */
#define DE_HANDLE(name)                                                                            \
    typedef struct name {                                                                          \
        u32 id;                                                                                    \
    } name
#define DE_HANDLE_VALID(h) ((h).id != 0)

/* ---------- Names and strings ---------- */

/* Stable 64-bit hashed name (asset IDs, interned strings). */
typedef struct de_name {
    u64 hash;
} de_name;
DE_API de_name de_name_from(const char *str); /* interns in dev builds for reverse lookup */
DE_API const char *de_name_str(de_name n);    /* NULL in shipping builds */

typedef struct de_str {
    const char *ptr;
    size_t len;
} de_str; /* non-owning view */
#define DE_STR(lit) ((de_str){(lit), sizeof(lit) - 1})

/* Hashing and string helpers (DENG-153) go at the end of this section. */

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

/** Capacity of a de_error message, including the terminating NUL. Longer messages are cut. */
#define DE_ERROR_MESSAGE_MAX 512

/**
 * Records in each thread's error ring. A cause chain holds at most this many records; when a
 * wrap would exceed it, the oldest record is dropped (the cause of the record before it becomes
 * NULL).
 */
#define DE_ERROR_CHAIN_MAX 8

/**
 * One error record. Records live in a per-thread ring (the "error arena"); nothing is
 * heap-allocated. A record and its cause chain stay valid until the same thread sets, wraps or
 * clears an error again. Copy what you need to keep.
 */
typedef struct de_error {
    de_result result;
    char message[DE_ERROR_MESSAGE_MAX]; /**< formatted message, always NUL-terminated */
    const char *file;
    const char *function;
    u32 line;
    const struct de_error *cause; /**< the error this one wraps, or NULL; same thread's ring */
} de_error;

/** The name of a result code (`"DE_ERR_IO"`). Never NULL; out-of-range values give
 * `"DE_ERR_UNKNOWN"`. */
DE_API const char *de_result_str(de_result r);

/** The calling thread's most recent error, or NULL if none was set since the last clear. */
DE_API const de_error *de_error_last(void);

/** Forgets the calling thread's error. de_error_last() returns NULL afterwards. */
DE_API void de_error_clear(void);

/**
 * Starts a new error on the calling thread (no cause) and returns `r`. Use DE_FAIL. The message
 * is formatted with vsnprintf and cut to DE_ERROR_MESSAGE_MAX - 1 bytes. Arguments may point into
 * the current error records (for example de_error_last()->message).
 */
DE_API de_result de_error_set_(de_result r, const char *file, u32 line, const char *func,
                               const char *fmt, ...) DE_PRINTF_FMT(5, 6);

/**
 * Like de_error_set_, but the new error's `cause` is the thread's previous error (if any), so
 * callers up the stack can add context. Use DE_FAIL_WRAP. Chains hold at most
 * DE_ERROR_CHAIN_MAX records.
 */
DE_API de_result de_error_wrap_(de_result r, const char *file, u32 line, const char *func,
                                const char *fmt, ...) DE_PRINTF_FMT(5, 6);

/* Set error context and return the code: `return DE_FAIL(DE_ERR_IO, "open %s", path);` */
#define DE_FAIL(r, ...) de_error_set_((r), __FILE__, __LINE__, __func__, __VA_ARGS__)
/* Wrap the previous error: `return DE_FAIL_WRAP(r, "loading level %s", name);` */
#define DE_FAIL_WRAP(r, ...) de_error_wrap_((r), __FILE__, __LINE__, __func__, __VA_ARGS__)
#define DE_TRY(expr)                                                                               \
    do {                                                                                           \
        de_result r__ = (expr);                                                                    \
        if (r__ != DE_OK)                                                                          \
            return r__;                                                                            \
    } while (0)

/* ---------- Asserts and panic ---------- */

/** What a panic hook receives. The strings are valid only during the hook call. */
typedef struct de_panic_info {
    const char *message; /**< the formatted DE_PANIC message */
    const char *file;
    const char *function;
    u32 line;
} de_panic_info;

/**
 * Called once per panic, on the panicking thread, before the message goes to stderr and the
 * process aborts. It may return (the panic then continues) or not return (exit, longjmp). A panic
 * inside the hook aborts at once without calling it again.
 */
typedef void (*de_panic_hook_fn)(const de_panic_info *info, void *user);

/**
 * Installs the process-wide panic hook. There is one slot: a new hook replaces the old one, and
 * `fn == NULL` removes it. The S03 crash handler (de_crash_install) uses this slot. Thread-safe.
 */
DE_API void de_panic_set_hook(de_panic_hook_fn fn, void *user);

/**
 * Fatal error: formats the message (cut to 1023 bytes), calls the panic hook, writes the message
 * with its location to stderr, then calls abort(). Use DE_PANIC. Uses no allocator: the message
 * is formatted on the stack.
 */
DE_NORETURN DE_API void de_panic_(const char *file, u32 line, const char *func, const char *fmt,
                                  ...) DE_PRINTF_FMT(4, 5);

#define DE_PANIC(...) de_panic_(__FILE__, __LINE__, __func__, __VA_ARGS__)
/* Always evaluated, in every build type. */
#define DE_VERIFY(c)                                                                               \
    do {                                                                                           \
        if (!(c))                                                                                  \
            DE_PANIC("verify failed: %s", #c);                                                     \
    } while (0)
/* Evaluated only in Debug and Dev builds; compiled out (not evaluated) otherwise. */
#if defined(DE_DEBUG) || defined(DE_DEV)
#define DE_ASSERT(c) DE_VERIFY(c)
#else
#define DE_ASSERT(c) ((void)0)
#endif

/* ---------- Logging ---------- */

typedef enum de_log_level {
    DE_LOG_TRACE,
    DE_LOG_DEBUG,
    DE_LOG_INFO,
    DE_LOG_WARN,
    DE_LOG_ERROR,
    DE_LOG_FATAL
} de_log_level;

typedef struct de_log_category {
    u32 id;
} de_log_category;

typedef struct de_log_record {
    de_log_level level;
    de_log_category category;
    u64 timestamp_ns;
    u32 thread_id;
    const char *file;
    u32 line;
    de_str message;
} de_log_record;

typedef void (*de_log_sink_fn)(const de_log_record *rec, void *user);

DE_API de_log_category de_log_register_category(const char *name, de_log_level default_level);
DE_API void de_log_set_level(de_log_category cat, de_log_level level);
DE_API void de_log_add_sink(de_log_sink_fn fn, void *user);
DE_API void de_log_(de_log_level lvl, de_log_category cat, const char *file, u32 line,
                    const char *fmt, ...) DE_PRINTF_FMT(5, 6);
#define DE_LOG(lvl, cat, ...) de_log_((lvl), (cat), __FILE__, __LINE__, __VA_ARGS__)
#define DE_INFO(cat, ...)     DE_LOG(DE_LOG_INFO, cat, __VA_ARGS__)
#define DE_WARN(cat, ...)     DE_LOG(DE_LOG_WARN, cat, __VA_ARGS__)
#define DE_ERROR(cat, ...)    DE_LOG(DE_LOG_ERROR, cat, __VA_ARGS__)

/* Log init and flush (DENG-150) go at the end of this section. */

/* ---------- Version ---------- */

#define DE_VERSION_MAJOR 0
#define DE_VERSION_MINOR 1
#define DE_VERSION_PATCH 0
/** Packs a version like de_version(). major < 1024, minor < 1024, patch < 4096. */
#define DE_MAKE_VERSION(major, minor, patch)                                                       \
    (((u32)(major) << 22) | ((u32)(minor) << 12) | (u32)(patch))
DE_API u32 de_version(void); /* (major << 22) | (minor << 12) | patch */

DE_EXTERN_C_END
#endif /* DE_BASE_H */
