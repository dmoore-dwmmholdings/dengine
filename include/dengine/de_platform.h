/* de_platform.h — windows, time, threads, files, async I/O, file watch, dynamic libs.
 * Implemented on SDL3 + per-OS code. No SDL types appear here. SPEC.md §6. */
#ifndef DE_PLATFORM_H
#define DE_PLATFORM_H
#include "de_base.h"
#include "de_memory.h"
DE_EXTERN_C_BEGIN

DE_HANDLE(de_window);

typedef enum de_window_mode { DE_WINDOW_WINDOWED, DE_WINDOW_BORDERLESS, DE_WINDOW_FULLSCREEN } de_window_mode;

typedef struct de_window_desc {
    const char    *title;
    u32            width, height;      /* logical pixels; 0 = 1280x720 */
    de_window_mode mode;
    i32            display_index;      /* -1 = primary */
    bool           resizable, high_dpi, hidden;
} de_window_desc;

typedef struct de_display_info {
    u32  width, height, refresh_hz;
    f32  content_scale;
    bool hdr_capable;
    f32  sdr_white_nits, max_nits;
} de_display_info;

/* Native handles for the RHI only (HWND, CAMetalLayer*, VkSurface helper data). */
typedef struct de_native_window {
    void *hwnd;           /* Windows */
    void *metal_layer;    /* macOS */
    void *sdl_window;     /* Vulkan surface creation path; opaque */
} de_native_window;

DE_API de_result de_window_create(const de_window_desc *desc, de_window *out);
DE_API void      de_window_destroy(de_window w);
DE_API void      de_window_size_pixels(de_window w, u32 *width, u32 *height);
DE_API void      de_window_set_mode(de_window w, de_window_mode mode);
DE_API void      de_window_set_title(de_window w, const char *title);
DE_API de_display_info  de_window_display(de_window w);
DE_API de_native_window de_window_native(de_window w);

/* Event pump. Input events are routed to de_input (de_input.h); this returns window/app events. */
typedef enum de_app_event_kind {
    DE_EV_QUIT, DE_EV_WINDOW_RESIZED, DE_EV_WINDOW_FOCUS, DE_EV_WINDOW_MINIMIZED,
    DE_EV_DISPLAY_CHANGED, DE_EV_DROP_FILE, DE_EV_LOW_MEMORY
} de_app_event_kind;
typedef struct de_app_event { de_app_event_kind kind; de_window window; u32 a, b; const char *path; } de_app_event;
DE_API u32 de_platform_pump(de_app_event *events, u32 max_events);

/* ---------- Time ---------- */
DE_API u64 de_time_now_ns(void);
DE_API void de_sleep_ns(u64 ns);

/* ---------- Threads and sync ---------- */
typedef struct de_thread de_thread;
typedef struct de_mutex  { DE_ALIGN(8) u8 opaque[64]; } de_mutex;
typedef struct de_cond   { DE_ALIGN(8) u8 opaque[64]; } de_cond;
typedef i32 (*de_thread_fn)(void *user);
typedef enum de_thread_priority { DE_THREAD_LOW, DE_THREAD_NORMAL, DE_THREAD_HIGH, DE_THREAD_REALTIME } de_thread_priority;

DE_API de_result de_thread_create(de_thread_fn fn, void *user, const char *name,
                                  de_thread_priority prio, size_t stack_bytes, de_thread **out);
DE_API i32  de_thread_join(de_thread *t);
DE_API void de_thread_set_affinity(de_thread *t, u64 core_mask);
DE_API u32  de_thread_current_id(void);
DE_API u32  de_cpu_logical_cores(void);
DE_API void de_mutex_init(de_mutex *m);
DE_API void de_mutex_lock(de_mutex *m);
DE_API void de_mutex_unlock(de_mutex *m);
DE_API void de_mutex_destroy(de_mutex *m);
DE_API void de_cond_init(de_cond *c);
DE_API void de_cond_wait(de_cond *c, de_mutex *m);
DE_API void de_cond_signal(de_cond *c);
DE_API void de_cond_broadcast(de_cond *c);
DE_API void de_cond_destroy(de_cond *c);

/* ---------- Files ---------- */
typedef struct de_file de_file;
typedef enum de_file_mode { DE_FILE_READ, DE_FILE_WRITE, DE_FILE_APPEND } de_file_mode;

/* Virtual roots: "game:/" (install dir), "user:/" (saves/settings), "cache:/", "dev:/" (project, dev only). */
DE_API de_result de_file_open(const char *vpath, de_file_mode mode, de_file **out);
DE_API de_result de_file_read(de_file *f, void *dst, u64 offset, u64 bytes, u64 *read);
DE_API de_result de_file_write(de_file *f, const void *src, u64 bytes);
DE_API u64       de_file_size(de_file *f);
DE_API void      de_file_close(de_file *f);
DE_API de_result de_file_read_all(const char *vpath, const de_allocator *a, void **data, u64 *size);
DE_API bool      de_path_exists(const char *vpath);

/* ---------- Async I/O ---------- */
typedef struct de_io_request {
    de_file *file;
    u64      offset, bytes;
    void    *dst;
    void   (*on_complete)(struct de_io_request *req, de_result r, u64 bytes_read);
    void    *user;
    u8       priority;   /* 0 = highest */
} de_io_request;
DE_API de_result de_io_submit(de_io_request *req);   /* req must stay alive until on_complete */
DE_API void      de_io_cancel(de_io_request *req);

/* ---------- File watching (dev builds) ---------- */
typedef struct de_file_watch de_file_watch;
typedef enum de_watch_change { DE_WATCH_CREATED, DE_WATCH_MODIFIED, DE_WATCH_DELETED, DE_WATCH_RENAMED } de_watch_change;
typedef void (*de_watch_fn)(const char *path, de_watch_change change, void *user);
DE_API de_result de_file_watch_create(const char *dir, bool recursive, de_watch_fn fn, void *user, de_file_watch **out);
DE_API void      de_file_watch_poll(de_file_watch *w);     /* delivers on the calling thread */
DE_API void      de_file_watch_destroy(de_file_watch *w);

/* ---------- Dynamic libraries ---------- */
typedef struct de_dylib de_dylib;
DE_API de_result de_dylib_open(const char *path, de_dylib **out);
DE_API void     *de_dylib_symbol(de_dylib *lib, const char *name);
DE_API void      de_dylib_close(de_dylib *lib);

DE_EXTERN_C_END
#endif
