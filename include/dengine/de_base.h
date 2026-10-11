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

/* ---- Hashing and names (DENG-153) ---- */

/**
 * 64-bit hash of `len` bytes: XXH3-64 (xxHash 0.8) with `seed`. `data` may be NULL when `len` is
 * 0. The same bytes and seed give the same value on every platform and in every build type.
 *
 * Asset IDs and de_name values are de_hash64(bytes, len, 0). They are stored in cooked data, so
 * the algorithm and the seed are part of the pack format: never change them without bumping the
 * pack format version (S08).
 */
DE_API u64 de_hash64(const void *data, size_t len, u64 seed);

/** de_hash64(str, strlen(str), 0). A NULL `str` hashes like "". */
DE_API u64 de_hash64_str(const char *str);

/**
 * 1 when this build keeps the reverse lookup of de_name values (DE_DEBUG or DE_DEV, and not
 * DE_SHIPPING), so de_name_str() can return the string. 0 otherwise (Release, Shipping).
 */
#if (defined(DE_DEBUG) || defined(DE_DEV)) && !defined(DE_SHIPPING)
#define DE_NAME_LOOKUP 1
#else
#define DE_NAME_LOOKUP 0
#endif

/*
 * de_name_from(str): the name of the exact bytes of `str` (no case folding, no path
 * normalisation): `.hash == de_hash64_str(str)`. With DE_NAME_LOOKUP, the string is also copied
 * into a process-wide, thread-safe intern table, and two different strings with the same hash
 * are a fatal error (DE_PANIC). A NULL `str` gives the name of "".
 *
 * de_name_str(n): the interned string of `n` (valid for the life of the process), or NULL when
 * the name was never interned. Always NULL without DE_NAME_LOOKUP.
 */

/** Like de_name_from(), for a string view (the bytes need no NUL terminator). */
DE_API de_name de_name_from_view(de_str s);

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

/* ---------- String views (DENG-153) ----------
 * A de_str is a non-owning view: `ptr` may be NULL only when `len` is 0, and the bytes need no
 * NUL terminator. No function here allocates or keeps the view. Byte-wise and locale-independent:
 * "whitespace" means the ASCII characters space, \t, \n, \v, \f and \r. */

/** de_str_find() result when the needle does not occur. */
#define DE_STR_NPOS ((size_t)-1)

/** A view of a NUL-terminated string (without the NUL). NULL gives {NULL, 0}. */
DE_API de_str de_str_from_cstr(const char *s);

/** True if both views have the same length and bytes. */
DE_API bool de_str_eq(de_str a, de_str b);

/** True if `s` begins with `prefix`. Every view begins with the empty view. */
DE_API bool de_str_starts_with(de_str s, de_str prefix);

/** Offset of the first occurrence of `needle` in `s`, or DE_STR_NPOS. An empty needle gives 0. */
DE_API size_t de_str_find(de_str s, de_str needle);

/**
 * Split iterator. Takes the text of `*rest` up to the first `sep` into `*out` and moves `*rest`
 * past that separator. When `*rest` holds no separator, `*out` gets all of it and `*rest` becomes
 * {NULL, 0}. Returns false (and leaves `*out` alone) when `rest->ptr` is NULL.
 *
 *     de_str rest = DE_STR("a,,b"), tok;
 *     while (de_str_split_next(&rest, ',', &tok)) { ... }   // "a", "", "b"
 *
 * So N separators always give N + 1 tokens: "" gives one empty token, "a," gives "a" and "".
 */
DE_API bool de_str_split_next(de_str *rest, char sep, de_str *out);

/** `s` without leading and trailing ASCII whitespace. */
DE_API de_str de_str_trim(de_str s);

/**
 * Parses all of `s` as a signed 64-bit integer: an optional `+` or `-`, then decimal digits, or
 * `0x`/`0X` and hex digits, or `0b`/`0B` and binary digits. No whitespace, no digit separators.
 *
 * @return DE_OK, or DE_ERR_PARSE (empty, a stray character, or out of range); `*out` is written
 *         only on DE_OK. Failures set the error context.
 */
DE_API de_result de_str_to_i64(de_str s, i64 *out);

/**
 * Parses all of `s` as a double: an optional sign, decimal digits with an optional `.` fraction
 * (at least one digit in all) and an optional exponent (`e`/`E`, optional sign, digits); or
 * `inf`, `infinity`, `nan` in any case, with an optional sign. No whitespace, no hex floats.
 * Uses `.` as the decimal point whatever the C locale says.
 *
 * @return DE_OK, or DE_ERR_PARSE (bad syntax, longer than 511 bytes, or a finite value too large
 *         for a double); `*out` is written only on DE_OK. Failures set the error context.
 */
DE_API de_result de_str_to_f64(de_str s, f64 *out);

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
