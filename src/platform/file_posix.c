/* file_posix.c: native file calls on macOS and Linux (SPEC §6, DENG-247).
 *
 * `open` + `pread` (positional, so one de_file can serve several I/O threads), `fstat` for the
 * size, `fsync` + `rename` for atomic replaces, `opendir`/`readdir` for listings. Every call
 * retries on EINTR. Paths are UTF-8 and passed through unchanged. */
#if defined(__linux__)
#define _GNU_SOURCE /* O_CLOEXEC, DT_* */
#elif defined(__APPLE__)
#define _DARWIN_C_SOURCE /* DT_*, d_type */
#endif

#include <dengine/de_platform.h>

#include "platform/vfs_internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h> /* rename */
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

struct de_file {
    int fd;
    de_file_mode mode;
    const de_allocator *alloc;
};

/* Maps errno to a result. Callers add the context. */
static de_result errno_result(int e) {
    switch (e) {
    case ENOENT:
    case ENOTDIR:
        return DE_ERR_NOT_FOUND;
    case EEXIST:
        return DE_ERR_ALREADY_EXISTS;
    case ENOMEM:
        return DE_ERR_OUT_OF_MEMORY;
    default:
        return DE_ERR_IO;
    }
}

de_result de__file_open_native(const char *native, de_file_mode mode, const de_allocator *a,
                               de_file **out) {
    int flags = O_CLOEXEC;
    switch (mode) {
    case DE_FILE_READ:
        flags |= O_RDONLY;
        break;
    case DE_FILE_WRITE:
        flags |= O_WRONLY | O_CREAT | O_TRUNC;
        break;
    case DE_FILE_APPEND:
        flags |= O_WRONLY | O_CREAT | O_APPEND;
        break;
    }
    de_file *f = DE_NEW(a, de_file, DE_MEM_GENERAL);
    if (f == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "cannot allocate a file record");
    int fd;
    do {
        fd = open(native, flags, 0644);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        int e = errno;
        DE_FREE(a, f, sizeof *f);
        return DE_FAIL(errno_result(e), "open '%s': %s", native, strerror(e));
    }
    f->fd = fd;
    f->mode = mode;
    f->alloc = a;
    *out = f;
    return DE_OK;
}

bool de__file_readable(const de_file *f) {
    return f->mode == DE_FILE_READ;
}

de_result de__file_pread(de_file *f, void *dst, u64 offset, u64 bytes, u64 *got) {
    *got = 0;
    if (offset > (u64)INT64_MAX)
        return DE_OK; /* past any possible end of file */
    ssize_t n;
    do {
        n = pread(f->fd, dst, (size_t)bytes, (off_t)offset);
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
        int e = errno;
        return DE_FAIL(DE_ERR_IO, "read of %llu bytes at %llu: %s", (unsigned long long)bytes,
                       (unsigned long long)offset, strerror(e));
    }
    *got = (u64)n;
    return DE_OK;
}

de_result de_file_write(de_file *f, const void *src, u64 bytes) {
    if (f == NULL || (src == NULL && bytes > 0))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_file_write: file or src is NULL");
    if (f->mode == DE_FILE_READ)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_file_write: file opened for reading");
    const u8 *p = (const u8 *)src;
    while (bytes > 0) {
        size_t chunk = bytes > ((u64)1 << 30) ? (size_t)1 << 30 : (size_t)bytes;
        ssize_t n = write(f->fd, p, chunk);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            int e = errno;
            return DE_FAIL(DE_ERR_IO, "de_file_write: %s", strerror(e));
        }
        p += n;
        bytes -= (u64)n;
    }
    return DE_OK;
}

u64 de_file_size(de_file *f) {
    if (f == NULL)
        return 0;
    struct stat st;
    if (fstat(f->fd, &st) != 0)
        return 0;
    return st.st_size > 0 ? (u64)st.st_size : 0;
}

void de_file_close(de_file *f) {
    if (f == NULL)
        return;
    /* No retry on EINTR: the descriptor is released either way (POSIX leaves it unspecified,
     * Linux and macOS always release it). */
    (void)close(f->fd);
    DE_FREE(f->alloc, f, sizeof *f);
}

de_result de__native_stat(const char *native, bool *is_dir) {
    struct stat st;
    if (stat(native, &st) != 0)
        return errno == ENOENT || errno == ENOTDIR ? DE_ERR_NOT_FOUND : DE_ERR_IO;
    *is_dir = S_ISDIR(st.st_mode);
    return DE_OK;
}

de_result de__native_mkdir(const char *native) {
    if (mkdir(native, 0755) == 0)
        return DE_OK;
    int e = errno;
    struct stat st;
    if (e == EEXIST && stat(native, &st) == 0 && S_ISDIR(st.st_mode))
        return DE_OK;
    return errno_result(e) == DE_ERR_NOT_FOUND ? DE_ERR_NOT_FOUND : DE_ERR_IO;
}

de_result de__native_remove(const char *native) {
    struct stat st;
    if (lstat(native, &st) != 0)
        return errno_result(errno);
    int rc = S_ISDIR(st.st_mode) ? rmdir(native) : unlink(native);
    return rc == 0 ? DE_OK : errno_result(errno);
}

de_result de__native_write_new_synced(const char *native, const void *data, u64 bytes) {
    int fd;
    do {
        fd = open(native, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0)
        return errno_result(errno);
    const u8 *p = (const u8 *)data;
    de_result r = DE_OK;
    while (bytes > 0) {
        size_t chunk = bytes > ((u64)1 << 30) ? (size_t)1 << 30 : (size_t)bytes;
        ssize_t n = write(fd, p, chunk);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            r = DE_ERR_IO;
            break;
        }
        p += n;
        bytes -= (u64)n;
    }
    /* fsync, not F_FULLFSYNC: the rename needs ordering against the OS cache, not a drive-cache
     * flush, and F_FULLFSYNC costs tens of milliseconds per call on macOS. */
    if (r == DE_OK && fsync(fd) != 0)
        r = DE_ERR_IO;
    if (close(fd) != 0 && r == DE_OK && errno != EINTR)
        r = DE_ERR_IO;
    return r;
}

de_result de__native_rename_replace(const char *from, const char *to) {
    return rename(from, to) == 0 ? DE_OK : errno_result(errno);
}

de_result de__native_dir_list(const char *native, de__native_dir_fn fn, void *user) {
    DIR *d = opendir(native);
    if (d == NULL)
        return errno_result(errno);
    char path[DE_FS_PATH_MAX];
    size_t base = strlen(native);
    de_result r = DE_OK;
    for (;;) {
        errno = 0;
        struct dirent *e = readdir(d);
        if (e == NULL) {
            if (errno != 0)
                r = DE_ERR_IO;
            break;
        }
        const char *name = e->d_name;
        if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
            continue;
        bool is_dir;
        if (e->d_type == DT_DIR) {
            is_dir = true;
        } else if (e->d_type == DT_UNKNOWN || e->d_type == DT_LNK) {
            /* Some file systems do not fill d_type; links report their target. */
            size_t nl = strlen(name);
            if (base + 1 + nl + 1 > sizeof path)
                continue;
            memcpy(path, native, base);
            path[base] = '/';
            memcpy(path + base + 1, name, nl + 1);
            is_dir = false;
            (void)de__native_stat(path, &is_dir);
        } else {
            is_dir = false;
        }
        if (!fn(name, is_dir, user))
            break;
    }
    closedir(d);
    return r;
}

u32 de__native_process_id(void) {
    return (u32)getpid();
}
