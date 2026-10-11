/* file_win32.c: native file calls on Windows (SPEC §6, DENG-247).
 *
 * Paths arrive as UTF-8 with '/' or '\' separators; each call converts them to UTF-16 with '\'
 * (MultiByteToWideChar, invalid UTF-8 refused) for the W functions. Reads are positional:
 * non-overlapped ReadFile with the offset in an OVERLAPPED, so several I/O threads can read one
 * handle. Handles share read, write and delete access, so a file being read can still be
 * replaced by an atomic write (MoveFileExW with MOVEFILE_REPLACE_EXISTING). */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <dengine/de_platform.h>

#include "platform/vfs_internal.h"

#include <windows.h>
#include <string.h>
#include <wchar.h>

/* UTF-16 code units for a path: at most one per UTF-8 byte. */
#define WPATH_MAX DE_FS_PATH_MAX

struct de_file {
    HANDLE h;
    de_file_mode mode;
    const de_allocator *alloc;
};

/* UTF-8 path -> UTF-16 with backslashes. False if it is not valid UTF-8 or too long. */
static bool widen(const char *utf8, wchar_t *out, int cap) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, out, cap);
    if (n <= 0)
        return false;
    for (int i = 0; i < n; ++i) {
        if (out[i] == L'/')
            out[i] = L'\\';
    }
    return true;
}

static de_result win32_result(DWORD e) {
    switch (e) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
        return DE_ERR_NOT_FOUND;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        return DE_ERR_ALREADY_EXISTS;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        return DE_ERR_OUT_OF_MEMORY;
    default:
        return DE_ERR_IO;
    }
}

de_result de__file_open_native(const char *native, de_file_mode mode, const de_allocator *a,
                               de_file **out) {
    wchar_t w[WPATH_MAX];
    if (!widen(native, w, WPATH_MAX))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "'%s': not a valid UTF-8 path", native);
    DWORD access = 0, disposition = 0;
    switch (mode) {
    case DE_FILE_READ:
        access = GENERIC_READ;
        disposition = OPEN_EXISTING;
        break;
    case DE_FILE_WRITE:
        access = GENERIC_WRITE;
        disposition = CREATE_ALWAYS;
        break;
    case DE_FILE_APPEND:
        access = FILE_APPEND_DATA | SYNCHRONIZE;
        disposition = OPEN_ALWAYS;
        break;
    }
    de_file *f = DE_NEW(a, de_file, DE_MEM_GENERAL);
    if (f == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "cannot allocate a file record");
    HANDLE h = CreateFileW(w, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           disposition, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        DE_FREE(a, f, sizeof *f);
        return DE_FAIL(win32_result(e), "CreateFileW '%s': error %lu", native, (unsigned long)e);
    }
    f->h = h;
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
    DWORD want = bytes > 0x40000000u ? 0x40000000u : (DWORD)bytes;
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    ov.Offset = (DWORD)(offset & 0xFFFFFFFFu);
    ov.OffsetHigh = (DWORD)(offset >> 32);
    DWORD n = 0;
    if (!ReadFile(f->h, dst, want, &n, &ov)) {
        DWORD e = GetLastError();
        if (e == ERROR_HANDLE_EOF)
            return DE_OK;
        return DE_FAIL(DE_ERR_IO, "ReadFile of %lu bytes at %llu: error %lu", (unsigned long)want,
                       (unsigned long long)offset, (unsigned long)e);
    }
    *got = n;
    return DE_OK;
}

de_result de_file_write(de_file *f, const void *src, u64 bytes) {
    if (f == NULL || (src == NULL && bytes > 0))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_file_write: file or src is NULL");
    if (f->mode == DE_FILE_READ)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_file_write: file opened for reading");
    const u8 *p = (const u8 *)src;
    while (bytes > 0) {
        DWORD chunk = bytes > 0x40000000u ? 0x40000000u : (DWORD)bytes;
        DWORD n = 0;
        if (!WriteFile(f->h, p, chunk, &n, NULL))
            return DE_FAIL(DE_ERR_IO, "de_file_write: WriteFile error %lu",
                           (unsigned long)GetLastError());
        p += n;
        bytes -= n;
    }
    return DE_OK;
}

u64 de_file_size(de_file *f) {
    if (f == NULL)
        return 0;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f->h, &sz) || sz.QuadPart < 0)
        return 0;
    return (u64)sz.QuadPart;
}

void de_file_close(de_file *f) {
    if (f == NULL)
        return;
    CloseHandle(f->h);
    DE_FREE(f->alloc, f, sizeof *f);
}

de_result de__native_stat(const char *native, bool *is_dir) {
    wchar_t w[WPATH_MAX];
    if (!widen(native, w, WPATH_MAX))
        return DE_ERR_NOT_FOUND;
    DWORD attr = GetFileAttributesW(w);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        de_result r = win32_result(GetLastError());
        return r == DE_ERR_NOT_FOUND ? DE_ERR_NOT_FOUND : DE_ERR_IO;
    }
    *is_dir = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return DE_OK;
}

de_result de__native_mkdir(const char *native) {
    wchar_t w[WPATH_MAX];
    if (!widen(native, w, WPATH_MAX))
        return DE_ERR_INVALID_ARGUMENT;
    if (CreateDirectoryW(w, NULL))
        return DE_OK;
    DWORD e = GetLastError();
    if (e == ERROR_ALREADY_EXISTS) {
        DWORD attr = GetFileAttributesW(w);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0)
            return DE_OK;
    }
    return win32_result(e) == DE_ERR_NOT_FOUND ? DE_ERR_NOT_FOUND : DE_ERR_IO;
}

de_result de__native_remove(const char *native) {
    wchar_t w[WPATH_MAX];
    if (!widen(native, w, WPATH_MAX))
        return DE_ERR_INVALID_ARGUMENT;
    DWORD attr = GetFileAttributesW(w);
    if (attr == INVALID_FILE_ATTRIBUTES)
        return win32_result(GetLastError());
    BOOL ok = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0 ? RemoveDirectoryW(w) : DeleteFileW(w);
    return ok ? DE_OK : win32_result(GetLastError());
}

de_result de__native_write_new_synced(const char *native, const void *data, u64 bytes) {
    wchar_t w[WPATH_MAX];
    if (!widen(native, w, WPATH_MAX))
        return DE_ERR_INVALID_ARGUMENT;
    HANDLE h = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return win32_result(GetLastError());
    const u8 *p = (const u8 *)data;
    de_result r = DE_OK;
    while (bytes > 0) {
        DWORD chunk = bytes > 0x40000000u ? 0x40000000u : (DWORD)bytes;
        DWORD n = 0;
        if (!WriteFile(h, p, chunk, &n, NULL)) {
            r = DE_ERR_IO;
            break;
        }
        p += n;
        bytes -= n;
    }
    if (r == DE_OK && !FlushFileBuffers(h))
        r = DE_ERR_IO;
    CloseHandle(h);
    return r;
}

de_result de__native_rename_replace(const char *from, const char *to) {
    wchar_t wf[WPATH_MAX], wt[WPATH_MAX];
    if (!widen(from, wf, WPATH_MAX) || !widen(to, wt, WPATH_MAX))
        return DE_ERR_INVALID_ARGUMENT;
    if (MoveFileExW(wf, wt, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return DE_OK;
    return win32_result(GetLastError());
}

de_result de__native_dir_list(const char *native, de__native_dir_fn fn, void *user) {
    wchar_t w[WPATH_MAX + 2];
    if (!widen(native, w, WPATH_MAX))
        return DE_ERR_INVALID_ARGUMENT;
    size_t len = wcslen(w);
    if (len > 0 && w[len - 1] != L'\\')
        w[len++] = L'\\';
    w[len++] = L'*';
    w[len] = L'\0';
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(w, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        return e == ERROR_FILE_NOT_FOUND ? DE_OK : win32_result(e); /* empty */
    }
    de_result r = DE_OK;
    bool stopped = false;
    char name[DE_FS_PATH_MAX];
    do {
        const wchar_t *n = fd.cFileName;
        if (n[0] == L'.' && (n[1] == L'\0' || (n[1] == L'.' && n[2] == L'\0')))
            continue;
        if (WideCharToMultiByte(CP_UTF8, 0, n, -1, name, (int)sizeof name, NULL, NULL) <= 0)
            continue;
        if (!fn(name, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, user)) {
            stopped = true;
            break;
        }
    } while (FindNextFileW(h, &fd));
    if (!stopped && GetLastError() != ERROR_NO_MORE_FILES)
        r = DE_ERR_IO;
    FindClose(h);
    return r;
}

u32 de__native_process_id(void) {
    return (u32)GetCurrentProcessId();
}
