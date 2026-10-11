/* de_platform.h — windows, time, threads, files, async I/O, file watch, dynamic libs.
 * Implemented on SDL3 + per-OS code. No SDL types appear here. SPEC.md §6. */
#ifndef DE_PLATFORM_H
#define DE_PLATFORM_H
#include "de_base.h"
#include "de_memory.h"
DE_EXTERN_C_BEGIN

DE_HANDLE(de_window);

typedef enum de_window_mode {
    DE_WINDOW_WINDOWED,
    DE_WINDOW_BORDERLESS,
    DE_WINDOW_FULLSCREEN
} de_window_mode;

typedef struct de_window_desc {
    const char *title;
    u32 width, height; /* logical pixels; 0 = 1280x720 */
    de_window_mode mode;
    i32 display_index; /* -1 = primary */
    bool resizable, high_dpi, hidden;
} de_window_desc;

typedef struct de_display_info {
    u32 width, height, refresh_hz;
    f32 content_scale;
    bool hdr_capable;
    f32 sdr_white_nits, max_nits;
} de_display_info;

/* Native handles for the RHI only (HWND, CAMetalLayer*, VkSurface helper data). */
typedef struct de_native_window {
    void *hwnd;        /* Windows */
    void *metal_layer; /* macOS */
    void *sdl_window;  /* Vulkan surface creation path; opaque */
} de_native_window;

DE_API de_result de_window_create(const de_window_desc *desc, de_window *out);
DE_API void de_window_destroy(de_window w);
DE_API void de_window_size_pixels(de_window w, u32 *width, u32 *height);
DE_API void de_window_set_mode(de_window w, de_window_mode mode);
DE_API void de_window_set_title(de_window w, const char *title);
DE_API de_display_info de_window_display(de_window w);
DE_API de_native_window de_window_native(de_window w);

/* Event pump. Input events are routed to de_input (de_input.h); this returns window/app events. */
typedef enum de_app_event_kind {
    DE_EV_QUIT,
    DE_EV_WINDOW_RESIZED,
    DE_EV_WINDOW_FOCUS,
    DE_EV_WINDOW_MINIMIZED,
    DE_EV_DISPLAY_CHANGED,
    DE_EV_DROP_FILE,
    DE_EV_LOW_MEMORY
} de_app_event_kind;
typedef struct de_app_event {
    de_app_event_kind kind;
    de_window window;
    u32 a, b;
    const char *path;
} de_app_event;
DE_API u32 de_platform_pump(de_app_event *events, u32 max_events);

/* ==========================================================================================
 * Time and threads (S02, DENG-147). Engine-owned per OS; <threads.h> is not used (SPEC §3.2).
 * Implementation: src/platform/{thread,time}_{posix,win32}.c. Atomics are internal:
 * src/platform/de_atomic.h.
 * ========================================================================================== */

/* ---------- Time ---------- */

/**
 * Monotonic time in nanoseconds since an unspecified start point.
 *
 * Never goes backwards and is not affected by wall-clock changes. Sources: `mach_absolute_time`
 * (macOS), `clock_gettime(CLOCK_MONOTONIC)` (Linux), `QueryPerformanceCounter` (Windows).
 * Thread-safe, lock-free, no allocation.
 */
DE_API u64 de_time_now_ns(void);

/** Sleeps the calling thread for at least `ns` nanoseconds. Not implemented yet: S03 DENG-249. */
DE_API void de_sleep_ns(u64 ns);

/* ---------- Threads ---------- */

/** An OS thread created by de_thread_create(). Owned by the creator until de_thread_join(). */
typedef struct de_thread de_thread;

/** Thread entry point. The return value is handed to de_thread_join(). */
typedef i32 (*de_thread_fn)(void *user);

/**
 * Scheduling class. Mapped per OS, best effort (raising the priority may need privileges):
 * - macOS: QoS class UTILITY, DEFAULT, USER_INITIATED, USER_INTERACTIVE.
 * - Linux: nice +5, 0, -5, -10 for the thread (a failure to raise it is ignored).
 * - Windows: THREAD_PRIORITY_BELOW_NORMAL, NORMAL, ABOVE_NORMAL, TIME_CRITICAL.
 */
typedef enum de_thread_priority {
    DE_THREAD_LOW,
    DE_THREAD_NORMAL,
    DE_THREAD_HIGH,
    DE_THREAD_REALTIME
} de_thread_priority;

/** Stack size used when de_thread_create() gets `stack_bytes == 0`, the same on every OS. */
#define DE_THREAD_DEFAULT_STACK_BYTES ((size_t)1 << 20)

/** Longest thread name kept, in bytes without the NUL. Linux shows only the first 15 bytes. */
#define DE_THREAD_NAME_MAX 63

/**
 * Starts a thread that runs `fn(user)`.
 *
 * @param name         Optional UTF-8 name shown in debuggers and profilers. It is copied and
 *                     truncated to DE_THREAD_NAME_MAX bytes (15 on Linux) at a UTF-8 character
 *                     boundary, and set from inside the new thread before `fn` runs. NULL keeps
 *                     the OS default (none; on Linux the creator's name).
 * @param prio         See de_thread_priority.
 * @param stack_bytes  Stack size; 0 = DE_THREAD_DEFAULT_STACK_BYTES. Rounded up to the page size.
 * @param out          Receives the thread. Every thread must be joined exactly once.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (`fn` or `out` is NULL), DE_ERR_OUT_OF_MEMORY, or
 *         DE_ERR_INTERNAL when the OS refuses the thread.
 *
 * The small thread record comes from the system heap, not from a de_allocator: the engine heap
 * itself is built on these primitives, and threads are created at startup only.
 */
DE_API de_result de_thread_create(de_thread_fn fn, void *user, const char *name,
                                  de_thread_priority prio, size_t stack_bytes, de_thread **out);

/** Waits for `t` to finish, frees it and returns the value its entry point returned. */
DE_API i32 de_thread_join(de_thread *t);

/**
 * Restricts `t` to the logical cores set in `core_mask` (bit i = core i).
 *
 * Linux: `pthread_setaffinity_np`. Windows: `SetThreadAffinityMask` (processor group 0).
 * macOS has no hard affinity: an affinity tag (lowest set bit + 1) is passed as a hint and the
 * kernel may ignore it; Apple Silicon does not support it at all.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (`t` NULL or no core in the mask), DE_ERR_UNSUPPORTED
 *         (the OS ignores affinity), or DE_ERR_INTERNAL.
 */
DE_API de_result de_thread_set_affinity(de_thread *t, u64 core_mask);

/** Names the calling thread (for threads the engine did not create, such as the main thread). */
DE_API void de_thread_set_name(const char *name);

/** OS id of the calling thread (low 32 bits on macOS). Never 0. Cached per thread. */
DE_API u32 de_thread_current_id(void);

/** Number of logical cores this process may run on. At least 1. */
DE_API u32 de_cpu_logical_cores(void);

/* ---------- Synchronization ----------
 * All objects live in caller-provided storage, are not movable or copyable once initialized,
 * and must be destroyed by the caller. init/destroy are not thread-safe; everything else is.
 * Initialization failures of mutex, condvar and rwlock are fatal (they cannot fail on the
 * supported OSes short of memory corruption). */

/** Non-recursive mutex. pthread mutex (macOS: first-fit policy) or SRWLOCK (Windows). */
typedef struct de_mutex {
    DE_ALIGN(8) u8 opaque[64];
} de_mutex;

/** Condition variable, used with a de_mutex. Waits can wake spuriously: always wait in a loop. */
typedef struct de_cond {
    DE_ALIGN(8) u8 opaque[64];
} de_cond;

/**
 * Reader/writer lock: many readers or one writer. Not upgradable and not recursive, not even for
 * readers: a second read lock on the same thread can deadlock behind a waiting writer. On glibc
 * the lock prefers writers (the glibc default would let a stream of readers starve them).
 */
typedef struct de_rwlock {
    DE_ALIGN(8) u8 opaque[256];
} de_rwlock;

/** Counting semaphore. dispatch_semaphore (macOS), sem_t (Linux), Win32 semaphore. */
typedef struct de_sem {
    DE_ALIGN(8) u8 opaque[64];
} de_sem;

/** Thread-local storage slot. Zero is invalid. */
typedef struct de_tls_key {
    u64 id;
} de_tls_key;

/** Destructor for a de_tls_key value; called on thread exit for non-NULL values only. */
typedef void (*de_tls_dtor)(void *value);

DE_API void de_mutex_init(de_mutex *m);
DE_API void de_mutex_lock(de_mutex *m);
/** Returns true if the lock was taken. */
DE_API bool de_mutex_try_lock(de_mutex *m);
DE_API void de_mutex_unlock(de_mutex *m);
DE_API void de_mutex_destroy(de_mutex *m);

DE_API void de_cond_init(de_cond *c);
/** Atomically unlocks `m`, waits, and locks `m` again before returning. */
DE_API void de_cond_wait(de_cond *c, de_mutex *m);
/** Like de_cond_wait(), at most `timeout_ns` (monotonic). Returns false on timeout. */
DE_API bool de_cond_wait_timeout(de_cond *c, de_mutex *m, u64 timeout_ns);
DE_API void de_cond_signal(de_cond *c);
DE_API void de_cond_broadcast(de_cond *c);
DE_API void de_cond_destroy(de_cond *c);

DE_API void de_rwlock_init(de_rwlock *l);
DE_API void de_rwlock_read_lock(de_rwlock *l);
DE_API void de_rwlock_read_unlock(de_rwlock *l);
DE_API void de_rwlock_write_lock(de_rwlock *l);
DE_API void de_rwlock_write_unlock(de_rwlock *l);
DE_API void de_rwlock_destroy(de_rwlock *l);

/** Creates a semaphore with count `initial`. DE_ERR_INTERNAL if the OS refuses. */
DE_API de_result de_sem_init(de_sem *s, u32 initial);
/** Adds `count` to the semaphore, waking up to `count` waiters. */
DE_API void de_sem_post(de_sem *s, u32 count);
/** Waits until the count is positive, then decrements it. */
DE_API void de_sem_wait(de_sem *s);
/** Decrements the count if it is positive, without blocking. Returns true if it did. */
DE_API bool de_sem_try_wait(de_sem *s);
/** Like de_sem_wait(), at most `timeout_ns`. Returns false on timeout. */
DE_API bool de_sem_wait_timeout(de_sem *s, u64 timeout_ns);
/** Destroys the semaphore. No thread may be waiting on it. */
DE_API void de_sem_destroy(de_sem *s);

/**
 * Allocates a thread-local slot; every thread starts with the value NULL.
 *
 * @param dtor  Optional. Runs on the exiting thread for each thread whose value is non-NULL
 *              when it exits (threads from de_thread_create() and other OS threads alike; not for
 *              the main thread at process exit). POSIX `pthread_key_create`, Windows FLS.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT, or DE_ERR_OUT_OF_MEMORY when the OS has no free slot.
 */
DE_API de_result de_tls_key_create(de_tls_key *out, de_tls_dtor dtor);
/**
 * Frees the slot. Clear the values first: whether destructors run for values still set differs
 * per OS (POSIX: never; Windows: FlsFree runs them).
 */
DE_API void de_tls_key_destroy(de_tls_key key);
DE_API void *de_tls_get(de_tls_key key);
/** Sets the calling thread's value. DE_ERR_OUT_OF_MEMORY if the OS cannot store it. */
DE_API de_result de_tls_set(de_tls_key key, void *value);

/* ==========================================================================================
 * End of the time and threads sections (DENG-147).
 * ========================================================================================== */

/* ---------- Files ---------- */
typedef struct de_file de_file;
typedef enum de_file_mode { DE_FILE_READ, DE_FILE_WRITE, DE_FILE_APPEND } de_file_mode;

/* Virtual roots: "game:/" (install dir), "user:/" (saves/settings), "cache:/", "dev:/" (project,
 * dev only). */
DE_API de_result de_file_open(const char *vpath, de_file_mode mode, de_file **out);
DE_API de_result de_file_read(de_file *f, void *dst, u64 offset, u64 bytes, u64 *read);
DE_API de_result de_file_write(de_file *f, const void *src, u64 bytes);
DE_API u64 de_file_size(de_file *f);
DE_API void de_file_close(de_file *f);
DE_API de_result de_file_read_all(const char *vpath, const de_allocator *a, void **data, u64 *size);
DE_API bool de_path_exists(const char *vpath);

/* ---------- Async I/O ---------- */
typedef struct de_io_request {
    de_file *file;
    u64 offset, bytes;
    void *dst;
    void (*on_complete)(struct de_io_request *req, de_result r, u64 bytes_read);
    void *user;
    u8 priority; /* 0 = highest */
} de_io_request;
DE_API de_result de_io_submit(de_io_request *req); /* req must stay alive until on_complete */
DE_API void de_io_cancel(de_io_request *req);

/* ---------- File watching (dev builds) ---------- */
typedef struct de_file_watch de_file_watch;
typedef enum de_watch_change {
    DE_WATCH_CREATED,
    DE_WATCH_MODIFIED,
    DE_WATCH_DELETED,
    DE_WATCH_RENAMED
} de_watch_change;
typedef void (*de_watch_fn)(const char *path, de_watch_change change, void *user);
DE_API de_result de_file_watch_create(const char *dir, bool recursive, de_watch_fn fn, void *user,
                                      de_file_watch **out);
DE_API void de_file_watch_poll(de_file_watch *w); /* delivers on the calling thread */
DE_API void de_file_watch_destroy(de_file_watch *w);

/* ---------- Dynamic libraries ---------- */
typedef struct de_dylib de_dylib;
DE_API de_result de_dylib_open(const char *path, de_dylib **out);
DE_API void *de_dylib_symbol(de_dylib *lib, const char *name);
DE_API void de_dylib_close(de_dylib *lib);

DE_EXTERN_C_END
#endif
