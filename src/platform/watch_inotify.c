/* watch_inotify.c: file watch backend for Linux (SPEC §6, DENG-248).
 *
 * One inotify instance per watch, read by a watcher thread ("de-watch") that waits in poll() on
 * the inotify descriptor and a stop pipe. inotify watches one directory per watch descriptor,
 * so a recursive watch adds a descriptor for every directory below the root at start, and for
 * each directory that appears later (IN_CREATE or IN_MOVED_TO with IN_ISDIR). A directory
 * created later is also scanned once after its descriptor is added, because entries made in
 * it before that are not reported by inotify; the scan reports them as CREATED (a duplicate
 * merges with the real event). A directory renamed away loses its descriptors.
 *
 * Events: IN_CREATE -> CREATED, IN_CLOSE_WRITE -> MODIFIED (once per writer, not per write),
 * IN_DELETE -> DELETED, IN_MOVED_FROM / IN_MOVED_TO -> MOVED_FROM / MOVED_TO paired by the
 * inotify cookie, IN_Q_OVERFLOW -> RESCAN.
 *
 * IN_CREATE of a regular file comes from open(O_CREAT), so its writer still has it open and
 * IN_CLOSE_WRITE follows: it is posted as CREATED_OPEN, which holds the path until the next
 * change (DENG-383). Otherwise a writer slower than the debounce window, such as the fsync of
 * de_file_write_all_atomic under I/O load, gets its temporary file delivered as CREATED before
 * the rename that makes it an atomic save. */
#define _GNU_SOURCE /* pipe2, O_CLOEXEC */

#include <dengine/de_platform.h>

#include "platform/watch_internal.h"

#if DE__WATCH_ENABLED

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#define INO_BUF_BYTES 65536u
#define INO_MASK                                                                                   \
    (IN_CLOSE_WRITE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE | IN_CREATE | IN_ONLYDIR |           \
     IN_DONT_FOLLOW | IN_EXCL_UNLINK)

typedef struct ino_dir {
    int wd;
    u32 rel_len;
    char *rel; /* relative path of the directory ("" for the root), NUL-terminated */
} ino_dir;

struct de__watch_backend {
    de_file_watch *w;
    const de_allocator *alloc;
    bool recursive;
    int fd;
    int stop_r, stop_w;
    de_thread *thread;
    /* Watched directories. Changed at start (caller thread) and then only on the watcher. */
    ino_dir *dirs;
    u32 count, cap;
    u32 last; /* index of the last lookup hit */
    char root[DE_FS_PATH_MAX];
    size_t root_len;
    u8 *buf; /* INO_BUF_BYTES, aligned for struct inotify_event */
};

/* root + "/" + rel into `out`; false if it does not fit. */
static bool native_path(const de__watch_backend *b, const char *rel, size_t rel_len, char *out,
                        size_t cap) {
    bool slash = rel_len > 0 && !(b->root_len == 1 && b->root[0] == '/');
    size_t n = b->root_len + (slash ? 1u : 0u) + rel_len;
    if (n + 1 > cap)
        return false;
    memcpy(out, b->root, b->root_len);
    if (slash)
        out[b->root_len] = '/';
    memcpy(out + b->root_len + (slash ? 1u : 0u), rel, rel_len);
    out[n] = '\0';
    return true;
}

/* rel + "/" + name into `out` (just `name` when rel is empty). */
static size_t join_rel(const char *rel, size_t rel_len, const char *name, char *out, size_t cap) {
    size_t nl = strlen(name);
    size_t n = rel_len + (rel_len > 0 ? 1u : 0u) + nl;
    if (n + 1 > cap)
        return 0;
    memcpy(out, rel, rel_len);
    if (rel_len > 0)
        out[rel_len] = '/';
    memcpy(out + rel_len + (rel_len > 0 ? 1u : 0u), name, nl);
    out[n] = '\0';
    return n;
}

static ino_dir *find_dir(de__watch_backend *b, int wd) {
    if (b->last < b->count && b->dirs[b->last].wd == wd)
        return &b->dirs[b->last];
    for (u32 i = 0; i < b->count; ++i) {
        if (b->dirs[i].wd == wd) {
            b->last = i;
            return &b->dirs[i];
        }
    }
    return NULL;
}

static void drop_dir(de__watch_backend *b, u32 i) {
    DE_FREE(b->alloc, b->dirs[i].rel, (size_t)b->dirs[i].rel_len + 1);
    b->dirs[i] = b->dirs[--b->count];
}

/* Adds a watch descriptor for the directory `rel`. */
static de_result add_watch(de__watch_backend *b, const char *rel, size_t rel_len) {
    char path[DE_FS_PATH_MAX];
    if (!native_path(b, rel, rel_len, path, sizeof path))
        return DE_ERR_INVALID_ARGUMENT;
    int wd = inotify_add_watch(b->fd, path, INO_MASK);
    if (wd < 0)
        return errno == ENOMEM || errno == ENOSPC ? DE_ERR_OUT_OF_MEMORY : DE_ERR_IO;
    ino_dir *d = find_dir(b, wd);
    if (d == NULL) {
        if (b->count == b->cap) {
            u32 cap = b->cap != 0 ? b->cap * 2 : 16;
            ino_dir *dirs = (ino_dir *)DE_REALLOC(b->alloc, b->dirs, (size_t)b->cap * sizeof *dirs,
                                                  (size_t)cap * sizeof *dirs, _Alignof(ino_dir),
                                                  DE_MEM_GENERAL);
            if (dirs == NULL) {
                inotify_rm_watch(b->fd, wd);
                return DE_ERR_OUT_OF_MEMORY;
            }
            b->dirs = dirs;
            b->cap = cap;
        }
        d = &b->dirs[b->count++];
        d->wd = wd;
        d->rel = NULL;
        d->rel_len = 0;
    }
    char *copy = (char *)DE_ALLOC(b->alloc, rel_len + 1, 1, DE_MEM_GENERAL);
    if (copy == NULL) {
        inotify_rm_watch(b->fd, wd);
        if (d->rel != NULL)
            DE_FREE(b->alloc, d->rel, (size_t)d->rel_len + 1);
        *d = b->dirs[--b->count];
        return DE_ERR_OUT_OF_MEMORY;
    }
    memcpy(copy, rel, rel_len);
    copy[rel_len] = '\0';
    if (d->rel != NULL)
        DE_FREE(b->alloc, d->rel, (size_t)d->rel_len + 1);
    d->rel = copy;
    d->rel_len = (u32)rel_len;
    return DE_OK;
}

/* Watches `rel` and, for a recursive watch, every directory below it (symlinks are not
 * followed). With `report`, every entry found is posted as CREATED. */
static void add_tree(de__watch_backend *b, const char *rel, size_t rel_len, bool report) {
    if (add_watch(b, rel, rel_len) != DE_OK || !b->recursive)
        return;
    char path[DE_FS_PATH_MAX];
    if (!native_path(b, rel, rel_len, path, sizeof path))
        return;
    DIR *dir = opendir(path);
    if (dir == NULL)
        return;
    for (;;) {
        struct dirent *e = readdir(dir);
        if (e == NULL)
            break;
        const char *name = e->d_name;
        if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
            continue;
        char child[DE_FS_PATH_MAX];
        size_t cl = join_rel(rel, rel_len, name, child, sizeof child);
        if (cl == 0)
            continue;
        bool is_dir = e->d_type == DT_DIR;
        if (e->d_type == DT_UNKNOWN) {
            char cp[DE_FS_PATH_MAX];
            struct stat st;
            is_dir = native_path(b, child, cl, cp, sizeof cp) && lstat(cp, &st) == 0 &&
                     S_ISDIR(st.st_mode);
        }
        if (report)
            de__watch_post(b->w, DE__WATCH_RAW_CREATED, child, cl, is_dir, 0);
        if (is_dir)
            add_tree(b, child, cl, report);
    }
    closedir(dir);
}

/* Removes the descriptors of `rel` and every directory below it. */
static void remove_tree(de__watch_backend *b, const char *rel, size_t rel_len) {
    for (u32 i = 0; i < b->count;) {
        const ino_dir *d = &b->dirs[i];
        bool inside = d->rel_len >= rel_len && memcmp(d->rel, rel, rel_len) == 0 &&
                      (d->rel_len == rel_len || d->rel[rel_len] == '/');
        if (inside) {
            inotify_rm_watch(b->fd, d->wd);
            drop_dir(b, i);
        } else {
            ++i;
        }
    }
}

/* Whether IN_CREATE of the file `rel` is followed by an IN_CLOSE_WRITE: a regular file with one
 * link was made by open(O_CREAT) (link(), symlink() and mknod() make others and report no
 * close). A file that is gone already counts too: its IN_DELETE or IN_MOVED_FROM is queued and
 * ends the hold. A file created without write access reports no close; the hold limit of the
 * common layer covers it. */
static bool created_open(const de__watch_backend *b, const char *rel, size_t rel_len) {
    char path[DE_FS_PATH_MAX];
    struct stat st;
    if (!native_path(b, rel, rel_len, path, sizeof path))
        return false;
    if (lstat(path, &st) != 0)
        return errno == ENOENT;
    return S_ISREG(st.st_mode) && st.st_nlink == 1;
}

static void handle(de__watch_backend *b, const struct inotify_event *e) {
    if (e->mask & IN_Q_OVERFLOW) {
        de__watch_post(b->w, DE__WATCH_RAW_RESCAN, "", 0, true, 0);
        return;
    }
    ino_dir *d = find_dir(b, e->wd);
    if (d == NULL)
        return;
    if (e->mask & IN_IGNORED) { /* removed, deleted or unmounted */
        drop_dir(b, (u32)(d - b->dirs));
        return;
    }
    if (e->len == 0 || e->name[0] == '\0')
        return; /* an event of the directory itself */
    char rel[DE_FS_PATH_MAX];
    size_t rl = join_rel(d->rel, d->rel_len, e->name, rel, sizeof rel);
    if (rl == 0)
        return;
    bool is_dir = (e->mask & IN_ISDIR) != 0;
    if (e->mask & IN_CREATE) {
        bool writing = !is_dir && created_open(b, rel, rl);
        de__watch_post(b->w, writing ? DE__WATCH_RAW_CREATED_OPEN : DE__WATCH_RAW_CREATED, rel, rl,
                       is_dir, 0);
        if (is_dir && b->recursive)
            add_tree(b, rel, rl, true);
    }
    if (e->mask & IN_CLOSE_WRITE)
        de__watch_post(b->w, DE__WATCH_RAW_MODIFIED, rel, rl, false, 0);
    if (e->mask & IN_DELETE)
        de__watch_post(b->w, DE__WATCH_RAW_DELETED, rel, rl, is_dir, 0);
    if (e->mask & IN_MOVED_FROM) {
        de__watch_post(b->w, DE__WATCH_RAW_MOVED_FROM, rel, rl, is_dir, e->cookie);
        if (is_dir && b->recursive)
            remove_tree(b, rel, rl);
    }
    if (e->mask & IN_MOVED_TO) {
        de__watch_post(b->w, DE__WATCH_RAW_MOVED_TO, rel, rl, is_dir, e->cookie);
        if (is_dir && b->recursive)
            add_tree(b, rel, rl, false);
    }
}

static i32 watch_thread(void *user) {
    de__watch_backend *b = (de__watch_backend *)user;
    for (;;) {
        struct pollfd p[2];
        p[0].fd = b->fd;
        p[0].events = POLLIN;
        p[0].revents = 0;
        p[1].fd = b->stop_r;
        p[1].events = POLLIN;
        p[1].revents = 0;
        if (poll(p, 2, -1) < 0) {
            if (errno == EINTR)
                continue;
            return 1;
        }
        if (p[1].revents != 0)
            return 0;
        if ((p[0].revents & POLLIN) == 0)
            continue;
        ssize_t n = read(b->fd, b->buf, INO_BUF_BYTES);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            return 1;
        }
        for (size_t off = 0; off + sizeof(struct inotify_event) <= (size_t)n;) {
            const struct inotify_event *e = (const struct inotify_event *)(b->buf + off);
            handle(b, e);
            off += sizeof(struct inotify_event) + e->len;
        }
    }
}

static void free_backend(de__watch_backend *b) {
    for (u32 i = 0; i < b->count; ++i)
        DE_FREE(b->alloc, b->dirs[i].rel, (size_t)b->dirs[i].rel_len + 1);
    if (b->dirs != NULL)
        DE_FREE(b->alloc, b->dirs, (size_t)b->cap * sizeof *b->dirs);
    if (b->buf != NULL)
        DE_FREE(b->alloc, b->buf, INO_BUF_BYTES);
    if (b->fd >= 0)
        close(b->fd);
    if (b->stop_r >= 0)
        close(b->stop_r);
    if (b->stop_w >= 0)
        close(b->stop_w);
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
    b->fd = b->stop_r = b->stop_w = -1;
    b->root_len = strlen(info->native_dir);
    memcpy(b->root, info->native_dir, b->root_len + 1);
    b->buf =
        (u8 *)DE_ALLOC(b->alloc, INO_BUF_BYTES, _Alignof(struct inotify_event), DE_MEM_GENERAL);
    if (b->buf == NULL) {
        free_backend(b);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "file watch: out of memory");
    }
    int pipefd[2];
    b->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (b->fd < 0 || pipe2(pipefd, O_CLOEXEC) != 0) {
        int err = errno;
        free_backend(b);
        return DE_FAIL(DE_ERR_IO, "file watch: inotify setup failed: %s", strerror(err));
    }
    b->stop_r = pipefd[0];
    b->stop_w = pipefd[1];
    de_result r = add_watch(b, "", 0);
    if (r != DE_OK) {
        int err = errno;
        free_backend(b);
        return DE_FAIL(r, "file watch: inotify_add_watch '%s' failed: %s", info->native_dir,
                       strerror(err));
    }
    /* The root's own entry stays; add_tree adds it again (same descriptor, same path). */
    add_tree(b, "", 0, false);
    r = de_thread_create(watch_thread, b, "de-watch", DE_THREAD_LOW, 0, &b->thread);
    if (r != DE_OK) {
        free_backend(b);
        return r;
    }
    *out = b;
    return DE_OK;
}

void de__watch_backend_stop(de__watch_backend *b) {
    if (b == NULL)
        return;
    char c = 1;
    while (write(b->stop_w, &c, 1) < 0 && errno == EINTR) {
    }
    (void)de_thread_join(b->thread);
    free_backend(b);
}

#endif
