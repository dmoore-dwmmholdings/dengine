/* watch_internal.h: what the file watch sources of src/platform share (DENG-248).
 *
 * watch_common.c owns de_file_watch: it resolves the virtual directory, keeps the pending
 * table, merges and debounces changes, and delivers them in de_file_watch_poll(). One backend
 * per OS reports raw changes into it from its own thread:
 *
 *   watch_fsevents.c  macOS    FSEvents stream on a private serial dispatch queue
 *   watch_inotify.c   Linux    inotify on a watcher thread
 *   watch_win32.c     Windows  overlapped ReadDirectoryChangesW on a watcher thread
 *
 * Relative paths are UTF-8 with `/` separators, relative to the watched native directory, and
 * never empty (a change of the directory itself is a RESCAN). */
#ifndef DE_PLATFORM_WATCH_INTERNAL_H
#define DE_PLATFORM_WATCH_INTERNAL_H

#include <dengine/de_platform.h>

/* Watching is compiled in Debug and Dev builds only (SPEC §6: dev only). */
#if (defined(DE_DEBUG) || defined(DE_DEV)) && !defined(DE_SHIPPING)
#define DE__WATCH_ENABLED 1
#else
#define DE__WATCH_ENABLED 0
#endif

/** Raw changes a backend reports. */
typedef enum de__watch_raw {
    DE__WATCH_RAW_CREATED,
    DE__WATCH_RAW_MODIFIED,
    DE__WATCH_RAW_DELETED,
    /** Renamed away from `rel`. `cookie` pairs it with the MOVED_TO of the same rename; an
     * unpaired MOVED_FROM becomes a DELETED. */
    DE__WATCH_RAW_MOVED_FROM,
    /** Renamed to `rel`. An unpaired MOVED_TO (from outside the tree) becomes a CREATED. */
    DE__WATCH_RAW_MOVED_TO,
    /** Events were lost: report the watched directory as MODIFIED. `rel` is ignored. */
    DE__WATCH_RAW_RESCAN,
} de__watch_raw;

/** The parts of a watch the backends read. The rest is private to watch_common.c. */
typedef struct de__watch_info {
    const de_allocator *alloc;
    bool recursive;
    char native_dir[DE_FS_PATH_MAX]; /* no trailing separator (except a bare "/") */
} de__watch_info;

/** Backend state (one per watch), defined by the backend. */
typedef struct de__watch_backend de__watch_backend;

/**
 * Starts watching `info->native_dir`. Raw changes go to de__watch_post(w, ...), from any thread,
 * until de__watch_backend_stop returns. Sets the error context on failure.
 */
de_result de__watch_backend_start(de_file_watch *w, const de__watch_info *info,
                                  de__watch_backend **out);
/** Stops the backend: when it returns, no de__watch_post for `w` runs or will run. */
void de__watch_backend_stop(de__watch_backend *b);

/**
 * Reports one raw change. `rel` has `rel_len` bytes (no NUL needed). `cookie` pairs MOVED_FROM
 * with MOVED_TO (0 = none). Thread-safe; never calls back.
 */
void de__watch_post(de_file_watch *w, de__watch_raw kind, const char *rel, size_t rel_len,
                    bool is_dir, u64 cookie);

/** Test hook: number of changes waiting in the pending table (matured or not). */
u32 de__watch_pending_count(de_file_watch *w);

#endif
