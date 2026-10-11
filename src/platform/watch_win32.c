/* watch_win32.c: file watch backend for Windows (SPEC §6, DENG-248).
 *
 * One directory handle per watch (FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED) and a
 * watcher thread ("de-watch") that keeps one overlapped ReadDirectoryChangesW pending on a
 * 64 KiB buffer and waits for it or the stop event. The subtree flag follows the watch's
 * `recursive`. Filter: FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME |
 * FILE_NOTIFY_CHANGE_DIR_NAME.
 *
 * Actions: ADDED -> CREATED, REMOVED -> DELETED, MODIFIED -> MODIFIED (directories are dropped
 * by the common layer), RENAMED_OLD_NAME / RENAMED_NEW_NAME -> MOVED_FROM / MOVED_TO. Windows
 * reports the two halves of a rename next to each other, so a counter is the cookie. A
 * completion with zero bytes (the buffer overflowed) or ERROR_NOTIFY_ENUM_DIR -> RESCAN.
 *
 * A removed path cannot be asked whether it was a directory, so the backend keeps the set of
 * directories in the tree (de__watch_dirset, DENG-370): scanned before create returns, then kept
 * current from ADDED / REMOVED / RENAMED (a directory added or renamed here is scanned, so the
 * directories inside it are known too), and scanned again after an overflow.
 *
 * The first read is issued by the watcher thread before de__watch_backend_start returns (the
 * start waits for it), so changes made after create are not lost. The thread issues every
 * read itself because Windows cancels pending I/O of a thread that exits. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <dengine/de_platform.h>

#include "platform/watch_internal.h"

#if DE__WATCH_ENABLED

#include <windows.h>
#include <stddef.h>
#include <string.h>

#define RDC_BUF_BYTES 65536u
#define RDC_FILTER                                                                                 \
    (FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME)
/* UTF-16 units of a native path: at most one per UTF-8 byte. */
#define WPATH_MAX DE_FS_PATH_MAX

struct de__watch_backend {
    de_file_watch *w;
    const de_allocator *alloc;
    bool recursive;
    HANDLE dir;
    HANDLE io_event, stop_event;
    OVERLAPPED ov;
    de_thread *thread;
    de_sem started; /* posted once the first read is pending (or failed) */
    de_result start_result;
    DWORD start_error; /* GetLastError of a failed first read, on the watcher thread */
    u64 cookie;        /* rename pairs */
    bool moved_dir;    /* the last RENAMED_OLD_NAME was a directory */
    const char *native_dir;
    de__watch_dirset dirs; /* only the watcher thread uses it after start */
    DWORD *buf;            /* RDC_BUF_BYTES, DWORD-aligned as ReadDirectoryChangesW requires */
    wchar_t root[WPATH_MAX];
    size_t root_len; /* in wchar_t, no trailing separator */
};

static bool to_wide(const char *s, wchar_t *out, size_t cap) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, out, (int)cap);
    if (n <= 0)
        return false;
    for (wchar_t *p = out; *p != 0; ++p) {
        if (*p == L'/')
            *p = L'\\';
    }
    return true;
}

/* Is root\name (name: UTF-16, `len` units) a directory now? */
static bool is_directory(const de__watch_backend *b, const wchar_t *name, size_t len) {
    wchar_t full[WPATH_MAX * 2];
    if (b->root_len + 1 + len + 1 > sizeof full / sizeof full[0])
        return false;
    memcpy(full, b->root, b->root_len * sizeof(wchar_t));
    full[b->root_len] = L'\\';
    memcpy(full + b->root_len + 1, name, len * sizeof(wchar_t));
    full[b->root_len + 1 + len] = 0;
    DWORD a = GetFileAttributesW(full);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static void handle_buffer(de__watch_backend *b, DWORD bytes) {
    const u8 *p = (const u8 *)b->buf;
    const u8 *end = p + bytes;
    /* A record is its header and FileNameLength bytes of name. The last record is not padded to
     * sizeof(FILE_NOTIFY_INFORMATION): a one-letter name makes a 14-byte record, which a
     * `sizeof` bound used to drop (DENG-370). */
    const size_t head = offsetof(FILE_NOTIFY_INFORMATION, FileName);
    char rel[DE_FS_PATH_MAX];
    for (;;) {
        if ((size_t)(end - p) < head)
            break;
        const FILE_NOTIFY_INFORMATION *n = (const FILE_NOTIFY_INFORMATION *)p;
        if (n->FileNameLength > (size_t)(end - p) - head)
            break;
        size_t wlen = n->FileNameLength / sizeof(WCHAR);
        int rl = WideCharToMultiByte(CP_UTF8, 0, n->FileName, (int)wlen, rel, (int)sizeof rel - 1,
                                     NULL, NULL);
        if (rl > 0) {
            for (int i = 0; i < rl; ++i) {
                if (rel[i] == '\\')
                    rel[i] = '/';
            }
            rel[rl] = '\0';
            size_t len = (size_t)rl;
            bool dir;
            switch (n->Action) {
            case FILE_ACTION_ADDED:
                dir = is_directory(b, n->FileName, wlen);
                if (dir) /* also what is already inside (moved in from outside the tree) */
                    de__watch_dirset_scan(&b->dirs, b->native_dir, rel, len, b->recursive);
                de__watch_post(b->w, DE__WATCH_RAW_CREATED, rel, len, dir, 0);
                break;
            case FILE_ACTION_REMOVED:
                dir = de__watch_dirset_remove_tree(&b->dirs, rel, len);
                de__watch_post(b->w, DE__WATCH_RAW_DELETED, rel, len, dir, 0);
                break;
            case FILE_ACTION_MODIFIED:
                /* A directory whose contents changed (the common layer drops it); it may be
                 * gone already when a deletion of its tree follows. */
                dir =
                    de__watch_dirset_has(&b->dirs, rel, len) || is_directory(b, n->FileName, wlen);
                de__watch_post(b->w, DE__WATCH_RAW_MODIFIED, rel, len, dir, 0);
                break;
            case FILE_ACTION_RENAMED_OLD_NAME:
                ++b->cookie;
                b->moved_dir = de__watch_dirset_remove_tree(&b->dirs, rel, len);
                de__watch_post(b->w, DE__WATCH_RAW_MOVED_FROM, rel, len, b->moved_dir, b->cookie);
                break;
            case FILE_ACTION_RENAMED_NEW_NAME:
                /* Renamed again since: it was a directory if its old name was one. */
                dir = is_directory(b, n->FileName, wlen) || b->moved_dir;
                if (dir)
                    de__watch_dirset_scan(&b->dirs, b->native_dir, rel, len, b->recursive);
                b->moved_dir = false;
                de__watch_post(b->w, DE__WATCH_RAW_MOVED_TO, rel, len, dir, b->cookie);
                break;
            default:
                break;
            }
        }
        if (n->NextEntryOffset == 0)
            break;
        p += n->NextEntryOffset;
    }
}

static bool issue_read(de__watch_backend *b) {
    ResetEvent(b->io_event);
    memset(&b->ov, 0, sizeof b->ov);
    b->ov.hEvent = b->io_event;
    return ReadDirectoryChangesW(b->dir, b->buf, RDC_BUF_BYTES, b->recursive ? TRUE : FALSE,
                                 RDC_FILTER, NULL, &b->ov, NULL) != 0;
}

/* Forgets the known directories and reads them from disk again. */
static void rescan_dirs(de__watch_backend *b) {
    de__watch_dirset_clear(&b->dirs);
    de__watch_dirset_scan(&b->dirs, b->native_dir, "", 0, b->recursive);
}

static i32 watch_thread(void *user) {
    de__watch_backend *b = (de__watch_backend *)user;
    bool ok = issue_read(b);
    b->start_result = ok ? DE_OK : DE_ERR_IO;
    b->start_error = ok ? 0 : GetLastError();
    /* After the read is pending, so a directory made from here on is in the scan or in an ADDED
     * event (or both); before create returns, so the set is complete for what follows. */
    if (ok)
        rescan_dirs(b);
    de_sem_post(&b->started, 1);
    if (!ok)
        return 1;
    HANDLE waits[2] = {b->io_event, b->stop_event};
    for (;;) {
        DWORD r = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (r != WAIT_OBJECT_0) {
            /* Stop (or a failed wait): cancel the pending read and wait for it to end, so the
             * buffer is not written after it is freed. */
            DWORD n = 0;
            CancelIoEx(b->dir, &b->ov);
            GetOverlappedResult(b->dir, &b->ov, &n, TRUE);
            return 0;
        }
        DWORD bytes = 0;
        if (!GetOverlappedResult(b->dir, &b->ov, &bytes, FALSE)) {
            DWORD err = GetLastError();
            if (err != ERROR_NOTIFY_ENUM_DIR)
                return 1; /* the directory went away */
            bytes = 0;
        }
        if (bytes == 0) {
            rescan_dirs(b);
            de__watch_post(b->w, DE__WATCH_RAW_RESCAN, "", 0, true, 0);
        } else
            handle_buffer(b, bytes);
        if (!issue_read(b))
            return 1;
    }
}

static void free_backend(de__watch_backend *b) {
    de__watch_dirset_free(&b->dirs);
    if (b->dir != INVALID_HANDLE_VALUE && b->dir != NULL)
        CloseHandle(b->dir);
    if (b->io_event != NULL)
        CloseHandle(b->io_event);
    if (b->stop_event != NULL)
        CloseHandle(b->stop_event);
    if (b->buf != NULL)
        DE_FREE(b->alloc, b->buf, RDC_BUF_BYTES);
    DE_FREE(b->alloc, b, sizeof *b);
}

de_result de__watch_backend_start(de_file_watch *w, const de__watch_info *info,
                                  de__watch_backend **out) {
    de__watch_backend *b = (de__watch_backend *)DE_ALLOC(
        info->alloc, sizeof *b, _Alignof(de__watch_backend), DE_MEM_GENERAL);
    if (b == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "file watch: out of memory");
    memset(b, 0, sizeof *b);
    b->w = w;
    b->alloc = info->alloc;
    b->recursive = info->recursive;
    b->native_dir = info->native_dir;
    de__watch_dirset_init(&b->dirs, info->alloc, true);
    b->dir = INVALID_HANDLE_VALUE;
    if (!to_wide(info->native_dir, b->root, WPATH_MAX)) {
        free_backend(b);
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "file watch: '%s' is not valid UTF-8",
                       info->native_dir);
    }
    b->root_len = wcslen(b->root);
    while (b->root_len > 0 && b->root[b->root_len - 1] == L'\\')
        b->root[--b->root_len] = 0;
    b->buf = (DWORD *)DE_ALLOC(b->alloc, RDC_BUF_BYTES, sizeof(DWORD), DE_MEM_GENERAL);
    b->io_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    b->stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (b->buf == NULL || b->io_event == NULL || b->stop_event == NULL) {
        free_backend(b);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "file watch: out of memory");
    }
    b->dir = CreateFileW(b->root, FILE_LIST_DIRECTORY,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
    if (b->dir == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        free_backend(b);
        return DE_FAIL(DE_ERR_IO, "file watch: cannot open '%s' (error %lu)", info->native_dir,
                       (unsigned long)err);
    }
    de_result r = de_sem_init(&b->started, 0);
    if (r != DE_OK) {
        free_backend(b);
        return r;
    }
    r = de_thread_create(watch_thread, b, "de-watch", DE_THREAD_LOW, 0, &b->thread);
    if (r != DE_OK) {
        de_sem_destroy(&b->started);
        free_backend(b);
        return r;
    }
    de_sem_wait(&b->started);
    de_sem_destroy(&b->started);
    if (b->start_result != DE_OK) {
        DWORD err = b->start_error;
        (void)de_thread_join(b->thread);
        free_backend(b);
        return DE_FAIL(DE_ERR_IO, "file watch: ReadDirectoryChangesW failed on '%s' (error %lu)",
                       info->native_dir, (unsigned long)err);
    }
    *out = b;
    return DE_OK;
}

void de__watch_backend_stop(de__watch_backend *b) {
    if (b == NULL)
        return;
    SetEvent(b->stop_event);
    (void)de_thread_join(b->thread);
    free_backend(b);
}

#endif
