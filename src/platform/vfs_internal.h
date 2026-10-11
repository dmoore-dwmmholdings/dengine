/* vfs_internal.h: what the files and async I/O sources of src/platform share (DENG-247).
 *
 * vfs.c owns virtual paths and the mount table and implements the path-based API on top of the
 * native calls below. file_posix.c / file_win32.c implement the native calls and the
 * handle-based API (de_file_read, de_file_write, ...). known_dirs.c finds the per-OS
 * directories. async_io.c reads through de__file_pread.
 *
 * Native paths are UTF-8 with `/` separators (Win32 converts to UTF-16 and `\`). They are at most
 * DE_FS_PATH_MAX bytes with the NUL. */
#ifndef DE_PLATFORM_VFS_INTERNAL_H
#define DE_PLATFORM_VFS_INTERNAL_H

#include <dengine/de_platform.h>

/* ---- Native file calls (file_posix.c / file_win32.c) ---------------------------------------- */

/** Opens a native file; the record comes from `a`. */
de_result de__file_open_native(const char *native, de_file_mode mode, const de_allocator *a,
                               de_file **out);
/** One positional read of at most `bytes` (the caller loops). `*got` == 0 at the end of file. */
de_result de__file_pread(de_file *f, void *dst, u64 offset, u64 bytes, u64 *got);
/** True if `f` was opened with DE_FILE_READ. */
bool de__file_readable(const de_file *f);

/** DE_OK with `*is_dir` set, DE_ERR_NOT_FOUND, or DE_ERR_IO. Sets no error context. */
de_result de__native_stat(const char *native, bool *is_dir);
/** Creates one directory. An existing directory is DE_OK. */
de_result de__native_mkdir(const char *native);
/** Removes a file or an empty directory. */
de_result de__native_remove(const char *native);
/** Writes `bytes` to a new file at `native` (fails if it exists) and flushes it to disk. */
de_result de__native_write_new_synced(const char *native, const void *data, u64 bytes);
/** Renames `from` over `to`, replacing it atomically. */
de_result de__native_rename_replace(const char *from, const char *to);

/** Native directory listing callback; names exclude `.` and `..`. Return false to stop. */
typedef bool (*de__native_dir_fn)(const char *name, bool is_dir, void *user);
de_result de__native_dir_list(const char *native, de__native_dir_fn fn, void *user);

/** A number that differs between processes (pid), for temporary file names. */
u32 de__native_process_id(void);

/* ---- Known directories (known_dirs.c) ------------------------------------------------------- */

/** Directory of the running executable, without a trailing separator. */
de_result de__known_exe_dir(char *buf, size_t cap);
/** Per-user data directory for org/app (see de_fs_desc.user_dir). Not created. */
de_result de__known_user_dir(const char *org, const char *app, char *buf, size_t cap);
/** Per-user cache directory for org/app (see de_fs_desc.cache_dir). Not created. */
de_result de__known_cache_dir(const char *org, const char *app, char *buf, size_t cap);

/* ---- Helpers (vfs.c) ------------------------------------------------------------------------ */

/** Appends `src` to the NUL-terminated `buf`; false (buf unchanged) if it does not fit. */
bool de__path_append(char *buf, size_t cap, const char *src);
/** Creates `native` and its missing parents. */
de_result de__native_mkdirs(const char *native);

#endif
