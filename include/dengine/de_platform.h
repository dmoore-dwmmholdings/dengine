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

/* ---------- Sleep and frame pacing (S03, DENG-249) ----------
 * Implementation: src/platform/sleep_pacing.c. */

/**
 * The last stretch before a sleep deadline that de_sleep_until_ns() spins instead of asking the
 * OS to sleep (1 ms). OS sleeps overshoot by tens of microseconds (Linux timer slack) to several
 * hundred (Windows high-resolution timers, macOS timer leeway), so the OS sleep stops this far
 * before the deadline and a de_cpu_pause() spin on de_time_now_ns() does the rest.
 */
#define DE_SLEEP_SPIN_NS 1000000ull

/**
 * Sleeps the calling thread for at least `ns` nanoseconds, then returns as close to that as the
 * scheduler allows. Same as `de_sleep_until_ns(de_time_now_ns() + ns)`; 0 returns at once.
 */
DE_API void de_sleep_ns(u64 ns);

/**
 * Sleeps until de_time_now_ns() >= `deadline_ns`. Never returns early.
 *
 * The OS sleep (`nanosleep` on POSIX; on Windows a waitable timer created with
 * `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`, or `Sleep` after `timeBeginPeriod(1)` before Windows
 * 10 1803) runs until DE_SLEEP_SPIN_NS before the deadline, then the thread spins on the clock.
 * Each OS sleep asks for half of the time left before that point, because timer leeway can grow
 * with the length of a sleep (macOS, and VMs on it); a 16 ms wait takes about 7 short sleeps.
 * A late return only comes from preemption. The spin
 * costs up to DE_SLEEP_SPIN_NS of CPU per call; frame pacing pays it once per frame. Thread-safe,
 * no allocation. A deadline in the past returns at once.
 */
DE_API void de_sleep_until_ns(u64 deadline_ns);

/** How a de_frame_pacer waits at the end of a frame. */
typedef enum de_frame_pacer_mode {
    /** Never waits (uncapped). Stats are still kept. The zero value. */
    DE_FRAME_PACER_NONE,
    /** Frame cap: de_frame_pacer_wait() sleeps until each deadline. */
    DE_FRAME_PACER_CAP,
    /**
     * For a vsync'ed swapchain: wakes `vsync_lead_ns` before each deadline and leaves the last
     * wait to the present call that blocks on the vblank. Keeps the CPU from running frames
     * ahead of the display (less input latency) without fighting the swapchain's own clock.
     */
    DE_FRAME_PACER_VSYNC_ASSIST
} de_frame_pacer_mode;

/** Settings of a de_frame_pacer. `(de_frame_pacer_desc){0}` is valid: uncapped. */
typedef struct de_frame_pacer_desc {
    u32 target_hz;            /**< frames per second; 0 = uncapped whatever the mode */
    de_frame_pacer_mode mode; /**< see de_frame_pacer_mode */
    u64 vsync_lead_ns;        /**< VSYNC_ASSIST only; 0 = DE_FRAME_PACER_DEFAULT_VSYNC_LEAD_NS */
} de_frame_pacer_desc;

/** `vsync_lead_ns` used when the desc gives 0: 1 ms. */
#define DE_FRAME_PACER_DEFAULT_VSYNC_LEAD_NS 1000000ull

/** Counters of a de_frame_pacer, from de_frame_pacer_stats(). All times in nanoseconds. */
typedef struct de_frame_pacer_stats {
    u64 last_frame_ns;    /**< begin-to-begin time of the last complete frame; 0 before one */
    u64 last_work_ns;     /**< begin-to-wait time of the last frame (the frame's own work) */
    u64 frames;           /**< de_frame_pacer_begin() calls since init */
    u64 missed_deadlines; /**< waits that found their deadline already passed */
    u64 resyncs;          /**< missed deadlines more than 2 periods late: the schedule restarted */
} de_frame_pacer_stats;

/**
 * Drift-free frame pacer. Caller storage; one thread uses it (the main loop). Treat the fields
 * as private. No allocation.
 *
 * Deadline n is `epoch + n * 1e9 / target_hz`, computed from integers, so rounding never
 * accumulates and N frames take N periods. A wait that finds its deadline passed counts a miss
 * and returns at once; the following frames catch up toward the schedule. When a wait finds the
 * deadline more than 2 periods gone (a stall: loading, a debugger, a minimized window) the
 * schedule restarts at that moment instead of running a burst of short frames.
 *
 *     de_frame_pacer p;
 *     de_frame_pacer_init(&p, &(de_frame_pacer_desc){.target_hz = 60, .mode = DE_FRAME_PACER_CAP});
 *     for (;;) {
 *         de_frame_pacer_begin(&p);
 *         run_frame();
 *         de_frame_pacer_wait(&p);
 *     }
 */
typedef struct de_frame_pacer {
    de_frame_pacer_desc desc;
    u64 epoch_ns; /* time of deadline 0 of the current schedule */
    u64 index;    /* frames since epoch_ns: the next deadline is deadline(index + 1) */
    u64 begin_ns; /* time of the last de_frame_pacer_begin(); 0 = none yet */
    bool started; /* a schedule exists (set by the first begin after init/set_target) */
    de_frame_pacer_stats stats;
} de_frame_pacer;

/** Initializes `p`. `desc` may be NULL (uncapped). */
DE_API void de_frame_pacer_init(de_frame_pacer *p, const de_frame_pacer_desc *desc);

/**
 * Changes the rate or mode at run time (settings menu, display change). The schedule restarts
 * at the next de_frame_pacer_begin(); the stats are kept.
 */
DE_API void de_frame_pacer_set(de_frame_pacer *p, const de_frame_pacer_desc *desc);

/**
 * Marks the start of a frame. Call once per frame, before the frame's work. The first call
 * after init or set starts the schedule: the first deadline is one period later.
 */
DE_API void de_frame_pacer_begin(de_frame_pacer *p);

/**
 * Ends the frame: sleeps (de_sleep_until_ns) until the frame's deadline, or returns at once when
 * the pacer is uncapped or the deadline has passed (see de_frame_pacer). Call once per frame,
 * after de_frame_pacer_begin(); a wait without a begin is ignored.
 */
DE_API void de_frame_pacer_wait(de_frame_pacer *p);

/** The counters of `p`. */
DE_API de_frame_pacer_stats de_frame_pacer_stats_get(const de_frame_pacer *p);

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

/* ==========================================================================================
 * Files and async I/O (S03, DENG-247). Engine-owned per OS (SPEC §6), no SDL.
 * Implementation: src/platform/vfs.c (paths, mounts), file_{posix,win32}.c (OS calls),
 * known_dirs.c (per-user directories), async_io.c (I/O threads).
 * ========================================================================================== */

/* ---------- Virtual paths and mounts ----------
 *
 * Every engine file access names a virtual path `<root>:/<path>`, so asset IDs and data never
 * depend on where the files are on disk (SPEC §11.1):
 *   - `<root>` is 1 to DE_FS_ROOT_MAX bytes of `[a-z0-9_-]`. Standard roots (de_fs_init):
 *     `game:/` (install directory, read-only), `user:/` (saves, settings; writable), `cache:/`
 *     (writable), `dev:/` (project directory, Debug and Dev builds only). `pack:/` is reserved
 *     for pack archives (S08) and cannot be mounted.
 *   - `<path>` uses `/` separators and is UTF-8. Empty and `.` segments are dropped; `..` removes
 *     the segment before it. A `..` that would leave the root, a backslash, a `:`, a control
 *     byte or invalid UTF-8 is rejected with DE_ERR_INVALID_ARGUMENT, and so is a native path
 *     (`/etc/passwd`, `C:\x`): there is no way to name a file outside the mounted directories.
 *
 * A root can have several mounts. Reads use the first mount, in priority order, where the path
 * exists. Writes go to the first writable mount; a root with no writable mount refuses writes
 * with DE_ERR_INVALID_ARGUMENT. The mount table is process-wide and thread-safe; it works
 * without de_fs_init (tests mount temporary directories with de_fs_mount alone).
 */

/** Longest root name, in bytes. */
#define DE_FS_ROOT_MAX 15
/** Longest virtual or native path, in bytes with the NUL. */
#define DE_FS_PATH_MAX 1024
/** Mounts the table holds, across all roots. */
#define DE_FS_MAX_MOUNTS 32

/** de_fs_mount flags. */
typedef enum de_mount_flags {
    DE_MOUNT_READ_ONLY = 0,      /**< reads only (the default) */
    DE_MOUNT_WRITABLE = 1u << 0, /**< writes, deletes and directory creation are allowed */
} de_mount_flags;

/**
 * Standard mounts. `(de_fs_desc){0}` is valid: `game:/` is the executable's directory, and
 * `user:/` and `cache:/` are the per-OS directories for the application "dengine".
 */
typedef struct de_fs_desc {
    /** Organisation folder above `app` on macOS and Windows (not used on Linux); NULL = none. */
    const char *org;
    /** Application folder name for `user:/` and `cache:/`; NULL = "dengine". */
    const char *app;
    /** Native directory for `game:/`; NULL = the directory of the executable. */
    const char *game_dir;
    /**
     * Native directory for `user:/`; NULL = per OS: `~/Library/Application Support/<org>/<app>`
     * (macOS), `$XDG_DATA_HOME/<app>` or `~/.local/share/<app>` (Linux),
     * `%APPDATA%\<org>\<app>` (Windows). Created if missing.
     */
    const char *user_dir;
    /**
     * Native directory for `cache:/`; NULL = per OS: `~/Library/Caches/<org>/<app>` (macOS),
     * `$XDG_CACHE_HOME/<app>` or `~/.cache/<app>` (Linux), `%LOCALAPPDATA%\<org>\<app>\cache`
     * (Windows). Created if missing.
     */
    const char *cache_dir;
    /** Native project directory for `dev:/` (writable). NULL, Release and Shipping: no `dev:/`. */
    const char *dev_dir;
    /** Allocator for open-file records; NULL = de_heap_allocator(). */
    const de_allocator *allocator;
} de_fs_desc;

/**
 * Mounts the standard roots at priority 0. `desc` may be NULL (same as a zeroed desc).
 * @return DE_OK, DE_ERR_ALREADY_EXISTS (already initialised), DE_ERR_NOT_FOUND (a directory or
 *         the home directory could not be found), DE_ERR_IO (a directory could not be created).
 */
DE_API de_result de_fs_init(const de_fs_desc *desc);
/** Removes every mount, standard or not. Files that are still open stay usable. */
DE_API void de_fs_shutdown(void);

/**
 * Adds `native_dir` (an existing directory) under `root` (a root name without `:/`, e.g.
 * "user"). Mounts with a higher `priority` are searched first; among equal priorities the newer
 * mount comes first.
 * @param flags  de_mount_flags.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (bad root name or path too long), DE_ERR_UNSUPPORTED
 *         (`pack`, reserved for S08), DE_ERR_NOT_FOUND (not a directory), DE_ERR_OUT_OF_MEMORY
 *         (DE_FS_MAX_MOUNTS reached).
 */
DE_API de_result de_fs_mount(const char *root, const char *native_dir, u32 flags, i32 priority);
/** Removes the mount of `native_dir` under `root` (as given to de_fs_mount). DE_ERR_NOT_FOUND. */
DE_API de_result de_fs_unmount(const char *root, const char *native_dir);

/**
 * Writes the normalised form of `vpath` (`user:/a/../b//c` -> `user:/b/c`) into `buf`.
 * Checks the syntax only; the root need not be mounted.
 * @return DE_OK or DE_ERR_INVALID_ARGUMENT (bad path, or `cap` too small).
 */
DE_API de_result de_path_normalize(const char *vpath, char *buf, size_t cap);

/**
 * Writes the native path for `vpath` into `buf`: the first mount where it exists, else the
 * mount a write would use (the first writable one), else the first mount.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (bad path or `cap` too small), DE_ERR_NOT_FOUND (root
 *         not mounted).
 */
DE_API de_result de_fs_resolve(const char *vpath, char *buf, size_t cap);

/* ---------- Files ----------
 * Synchronous, blocking calls; safe from any thread. A de_file may be read from several threads
 * at once (reads take an offset); writes to one de_file must not overlap. */
typedef struct de_file de_file;
typedef enum de_file_mode {
    DE_FILE_READ,  /**< existing file, reads only */
    DE_FILE_WRITE, /**< created or truncated, sequential writes */
    DE_FILE_APPEND /**< created if missing, every write goes to the end */
} de_file_mode;

/**
 * Opens a file. READ searches the mounts in priority order; WRITE and APPEND use the first
 * writable mount of the root, and its parent directory must exist (see de_dir_create).
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (bad path, no writable mount, a directory),
 *         DE_ERR_NOT_FOUND, DE_ERR_IO, DE_ERR_OUT_OF_MEMORY.
 */
DE_API de_result de_file_open(const char *vpath, de_file_mode mode, de_file **out);
/**
 * Reads up to `bytes` at `offset` into `dst`. `*read` (optional) gets the count, which is less
 * than `bytes` only at the end of the file. Does not move a file position.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (not opened for reading), DE_ERR_IO.
 */
DE_API de_result de_file_read(de_file *f, void *dst, u64 offset, u64 bytes, u64 *read);
/** Writes all `bytes` after the previous write (WRITE) or at the end (APPEND). DE_ERR_IO. */
DE_API de_result de_file_write(de_file *f, const void *src, u64 bytes);
/** Size of the open file in bytes; 0 on error. */
DE_API u64 de_file_size(de_file *f);
/** Closes `f` (NULL is ignored). Written data is handed to the OS; it is not flushed to disk. */
DE_API void de_file_close(de_file *f);

/**
 * Reads a whole file into one block from `a` (NULL = de_heap_allocator()). The block has
 * `*size + 1` bytes with a NUL after the data; free it with `DE_FREE(a, *data, *size + 1)`.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT, DE_ERR_NOT_FOUND, DE_ERR_IO, DE_ERR_OUT_OF_MEMORY.
 */
DE_API de_result de_file_read_all(const char *vpath, const de_allocator *a, void **data, u64 *size);
/**
 * Replaces the file at `vpath` with `bytes` of `data` (first writable mount): writes a temporary
 * file in the same directory, flushes it to disk, then renames it over the target, so a reader
 * or a crash sees either the old or the new file, never a part.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT, DE_ERR_NOT_FOUND (no parent directory), DE_ERR_IO.
 */
DE_API de_result de_file_write_all_atomic(const char *vpath, const void *data, u64 bytes);
/** True if `vpath` names an existing file or directory in any mount of its root. */
DE_API bool de_path_exists(const char *vpath);
/**
 * Deletes a file or an empty directory from the first writable mount that has it.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (bad path, no writable mount), DE_ERR_NOT_FOUND,
 *         DE_ERR_IO (e.g. a directory that is not empty).
 */
DE_API de_result de_file_delete(const char *vpath);
/**
 * Creates the directory `vpath` and its missing parents in the first writable mount. An
 * existing directory is not an error.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT, DE_ERR_IO.
 */
DE_API de_result de_dir_create(const char *vpath);

/** de_dir_list callback: one entry name (no path). Return false to stop the listing. */
typedef bool (*de_dir_fn)(const char *name, bool is_dir, void *user);
/**
 * Calls `fn` once for each entry of the directory `vpath`, merged over the mounts of its root: a
 * name that a higher mount also has is reported once, for the higher mount. `.` and `..` are
 * skipped. The order is unspecified (OS order). `fn` may call other file functions.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT, DE_ERR_NOT_FOUND (no mount has the directory),
 *         DE_ERR_IO, DE_ERR_OUT_OF_MEMORY.
 */
DE_API de_result de_dir_list(const char *vpath, de_dir_fn fn, void *user);

/* ---------- Async I/O ----------
 *
 * v1 async I/O (SPEC §6): a pool of I/O threads doing blocking reads. Requests are caller-owned
 * and linked into the queues intrusively, so submitting allocates nothing. Each thread takes the
 * request with the lowest `priority` value (FIFO within a priority) and reads it in chunks of
 * at most DE_IO_CHUNK_BYTES, checking for cancellation between chunks.
 *
 * Completions are delivered exactly once per submitted request: by de_io_pump() on the thread
 * that pumps, or, with DE_IO_CALLBACK_ON_IO_THREAD, directly on the thread that finishes the
 * request (an I/O thread, or the thread calling de_io_cancel/de_io_shutdown for a request that
 * never started). The request is free again (may be resubmitted or released) when `on_complete`
 * runs, and the I/O system does not touch it afterwards.
 */

/** Largest read an I/O thread issues at once; cancellation is checked between chunks. */
#define DE_IO_CHUNK_BYTES ((u64)1 << 20)
/** I/O threads when de_io_desc.thread_count is 0. */
#define DE_IO_DEFAULT_THREADS 2

/** de_io_request.flags. */
enum {
    /** Call `on_complete` on the thread that finishes the request, not from de_io_pump(). */
    DE_IO_CALLBACK_ON_IO_THREAD = 1u << 0,
};

typedef struct de_io_request {
    de_file *file; /**< opened with DE_FILE_READ; must stay open until on_complete */
    u64 offset;    /**< file offset of the first byte */
    u64 bytes;     /**< bytes to read; reading stops early at the end of the file */
    void *dst;     /**< at least `bytes` bytes */
    /**
     * Required. `r` is DE_OK (`bytes_read` < `bytes` only at the end of the file), DE_ERR_IO, or
     * DE_ERR_CANCELLED (`bytes_read` is what was read before the cancel).
     */
    void (*on_complete)(struct de_io_request *req, de_result r, u64 bytes_read);
    void *user;  /**< for the caller */
    u8 priority; /**< 0 = highest */
    u32 flags;   /**< DE_IO_CALLBACK_ON_IO_THREAD */
    /** Owned by the I/O system from de_io_submit until on_complete. Zero it before first use. */
    struct {
        struct de_io_request *next, *prev;
        u64 done;
        i32 result;
        u32 state, cancel;
    } internal_;
} de_io_request;

typedef struct de_io_desc {
    u32 thread_count; /**< I/O threads; 0 = DE_IO_DEFAULT_THREADS */
} de_io_desc;

/**
 * Starts the I/O threads (named "de-io-<n>"). `desc` may be NULL.
 * @return DE_OK, DE_ERR_ALREADY_EXISTS (running), DE_ERR_OUT_OF_MEMORY, DE_ERR_INTERNAL.
 */
DE_API de_result de_io_init(const de_io_desc *desc);
/**
 * Stops the I/O system: queued requests complete with DE_ERR_CANCELLED, running ones stop at
 * their next chunk boundary, the threads are joined, and every pending completion is delivered
 * on the calling thread (call it from the thread that pumps). Does nothing if not running.
 */
DE_API void de_io_shutdown(void);
/**
 * Queues `req`, which must stay valid and unchanged until its on_complete runs. Allocates
 * nothing. On an error the request is not queued and on_complete is not called.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (NULL request, file, callback or `dst`, a file not
 *         opened for reading), DE_ERR_BUSY (`req` is already submitted), DE_ERR_UNSUPPORTED
 *         (de_io_init not called).
 */
DE_API de_result de_io_submit(de_io_request *req);
/**
 * Cancels `req`: still queued -> it completes with DE_ERR_CANCELLED and 0 bytes; being read ->
 * it stops at the next chunk boundary with DE_ERR_CANCELLED; already finished or not submitted
 * -> nothing happens. `req` must be valid memory for the duration of the call.
 */
DE_API void de_io_cancel(de_io_request *req);
/**
 * Delivers up to `max_completions` finished requests (0 = all that are ready) on the calling
 * thread, in completion order. Never blocks on I/O. Returns the number delivered.
 */
DE_API u32 de_io_pump(u32 max_completions);

/* ==========================================================================================
 * End of the files and async I/O sections (DENG-247).
 * ========================================================================================== */

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

/* ---------- Dynamic libraries (S03, DENG-249) ----------
 * The game library in dev builds (SPEC §10.1) and its hot reload. Implementation:
 * src/platform/dylib_{posix,win32}.c. */

/** A loaded shared library. Opaque; the pointer is the OS handle, so nothing is allocated. */
typedef struct de_dylib de_dylib;

/** A function pointer from de_dylib_function(). Cast it to the real type before calling. */
typedef void (*de_dylib_fn)(void);

/**
 * Loads the shared library at `path`, a native UTF-8 file path (not a VFS path: the game library
 * sits next to the executable). POSIX `dlopen(RTLD_NOW | RTLD_LOCAL)`: every symbol is resolved
 * now, and none leaks into the global namespace. Windows `LoadLibraryExW`; an absolute path also
 * searches the library's own directory for its dependencies (`LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR`).
 *
 * Each distinct file is a separate image: a copy of a library under a new name loads next to the
 * original, with its own globals (hot reload, SPEC §10.1). Opening the same path twice gives the
 * same image, reference-counted by the OS; close each open once.
 *
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (NULL or empty `path`, NULL `out`), DE_ERR_NOT_FOUND
 *         (no such file), or DE_ERR_IO (the OS could not load it: wrong architecture, a missing
 *         dependency, an unresolved symbol). The error message carries the OS text (`dlerror`,
 *         `FormatMessageW`).
 */
DE_API de_result de_dylib_open(const char *path, de_dylib **out);

/**
 * The address of the exported data or function `name` in `lib`, or NULL with the error context
 * set (DE_ERR_INVALID_ARGUMENT, DE_ERR_NOT_FOUND with the OS text). For a function, use
 * de_dylib_function(): ISO C has no cast between object and function pointers.
 */
DE_API void *de_dylib_symbol(de_dylib *lib, const char *name);

/** Like de_dylib_symbol(), for an exported function. NULL with the error context set if absent. */
DE_API de_dylib_fn de_dylib_function(de_dylib *lib, const char *name);

/**
 * Releases `lib` (`dlclose`, `FreeLibrary`). The image unloads when its last open is closed;
 * pointers into it are invalid then. NULL is ignored.
 */
DE_API void de_dylib_close(de_dylib *lib);

/**
 * Writes the platform file name of the library `base` to `buf`: `libgame.dylib` (macOS),
 * `libgame.so` (Linux), `game.dll` (Windows) for `base` = "game". A directory part is kept,
 * with the prefix added to the last component only: "bin/game" → "bin/libgame.so".
 *
 * @param cap  Size of `buf` in bytes, NUL included.
 * @return DE_OK, or DE_ERR_INVALID_ARGUMENT (NULL argument, empty file name, or `buf` too small;
 *         `buf` then holds an empty string if `cap` > 0).
 */
DE_API de_result de_dylib_platform_name(const char *base, char *buf, size_t cap);

DE_EXTERN_C_END
#endif
