/* watch_fsevents.c: file watch backend for macOS (SPEC §6, DENG-248).
 *
 * One FSEvents stream per watch with file-level events (kFSEventStreamCreateFlagFileEvents),
 * no deferral of the first event (NoDefer) and extended data (each event carries the inode),
 * delivered on a private serial dispatch queue. FSEvents always watches the whole tree; the
 * common layer drops nested paths for a non-recursive watch.
 *
 * FSEvents reports per path a set of flags that may be coalesced, and a flag can stay set on
 * later events of the same path (a file created and then modified reports ItemCreated again).
 * So the flags are resolved against the file system: `lstat` decides whether the path exists
 * now, and a small ring of (path, inode) pairs already reported as created or renamed marks
 * such repeated flags as stale. ItemRenamed is split by existence: a path that exists with the
 * event's inode is the new name (MOVED_TO), otherwise it is the old name (MOVED_FROM); the inode
 * is the cookie that pairs them. */
#define _DARWIN_C_SOURCE /* F_GETPATH, O_DIRECTORY */

#include <dengine/de_platform.h>

#include "platform/de_atomic.h"
#include "platform/watch_internal.h"

#if DE__WATCH_ENABLED

#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <time.h>
#include <stdio.h>
#include <unistd.h>

/* (path, inode) pairs already reported as created or renamed. Flags repeat for a short time
 * after the change, so a few hundred recent ones are enough. */
#define FSE_SEEN_RING 1024u
/* FSEvents latency: changes after the first one of a quiet period are batched this long. The
 * common debounce window does the real coalescing. */
#define FSE_LATENCY_S 0.02
/* Longest wait for the start-up barrier event before giving up on it. */
#define FSE_BARRIER_TIMEOUT_NS 2000000000ll

typedef struct fse_seen {
    u64 hash;
    u64 ino;
} fse_seen;

struct de__watch_backend {
    de_file_watch *w;
    const de_allocator *alloc;
    FSEventStreamRef stream;
    dispatch_queue_t queue;
    /* The watched directory as FSEvents spells it (F_GETPATH: /private/var, real case), and as
     * the mount table spells it. Events are matched against both. */
    char canon[MAXPATHLEN];
    size_t canon_len;
    char given[DE_FS_PATH_MAX];
    size_t given_len;
    fse_seen seen[FSE_SEEN_RING]; /* touched only on the queue */
    u32 seen_next;
    /* Start-up barrier: a private directory under the user temp dir, also watched by the
     * stream, where start creates a marker file. Until the marker's event arrives, events are
     * from before the start: they only feed the seen ring and are not reported. */
    char barrier[MAXPATHLEN];
    size_t barrier_len;
    bool priming; /* read and written on the queue */
    /* Inode of the barrier marker when it is on the watched volume, else 0. APFS and HFS+
     * hand out file IDs in increasing order, so an item with a lower ID existed before the
     * start: its ItemCreated flag is stale even when the item is gone and cannot be stat'ed. */
    u64 ino_floor;
    dispatch_semaphore_t barrier_seen;
    struct timespec start_time; /* wall clock at start, compared with file birth times */
};

static bool seen_has(const de__watch_backend *b, u64 h, u64 ino) {
    for (u32 i = 0; i < FSE_SEEN_RING; ++i) {
        if (b->seen[i].hash == h && b->seen[i].ino == ino)
            return true;
    }
    return false;
}

static void seen_add(de__watch_backend *b, u64 h, u64 ino) {
    if (seen_has(b, h, ino))
        return;
    b->seen[b->seen_next].hash = h;
    b->seen[b->seen_next].ino = ino;
    b->seen_next = (b->seen_next + 1) % FSE_SEEN_RING;
}

/* The part of `path` below the watched directory, or NULL (outside, or the directory itself). */
static const char *relative(const de__watch_backend *b, const char *path) {
    const char *roots[2] = {b->canon, b->given};
    size_t lens[2] = {b->canon_len, b->given_len};
    for (int i = 0; i < 2; ++i) {
        size_t n = lens[i];
        if (n == 0 || strncmp(path, roots[i], n) != 0)
            continue;
        if (n == 1 && roots[i][0] == '/')
            return path[1] != '\0' ? path + 1 : NULL;
        if (path[n] == '/' && path[n + 1] != '\0')
            return path + n + 1;
    }
    return NULL;
}

static bool under(const char *path, const char *dir, size_t n) {
    return n > 0 && strncmp(path, dir, n) == 0 && (path[n] == '/' || path[n] == '\0');
}

static void handle_event(de__watch_backend *b, const char *path, FSEventStreamEventFlags f,
                         u64 ev_ino) {
    de_file_watch *w = b->w;
    if (under(path, b->barrier, b->barrier_len)) {
        if (b->priming && strcmp(path + b->barrier_len, "/marker") == 0) {
            b->priming = false;
            dispatch_semaphore_signal(b->barrier_seen);
        }
        return;
    }
    if (f & (kFSEventStreamEventFlagMustScanSubDirs | kFSEventStreamEventFlagUserDropped |
             kFSEventStreamEventFlagKernelDropped)) {
        de__watch_post(w, DE__WATCH_RAW_RESCAN, "", 0, true, 0);
        return;
    }
    if (f & (kFSEventStreamEventFlagHistoryDone | kFSEventStreamEventFlagRootChanged |
             kFSEventStreamEventFlagMount | kFSEventStreamEventFlagUnmount))
        return;
    const char *rel = relative(b, path);
    if (rel == NULL)
        return;
    size_t rl = strlen(rel);
    /* A trailing separator (directories, sometimes) is not part of the name. */
    while (rl > 0 && rel[rl - 1] == '/')
        --rl;
    if (rl == 0)
        return;

    struct stat st;
    bool exists = lstat(path, &st) == 0;
    bool is_dir = exists ? S_ISDIR(st.st_mode) : (f & kFSEventStreamEventFlagItemIsDir) != 0;
    bool created = (f & kFSEventStreamEventFlagItemCreated) != 0;
    bool removed = (f & kFSEventStreamEventFlagItemRemoved) != 0;
    bool renamed = (f & kFSEventStreamEventFlagItemRenamed) != 0;
    bool modified = (f & kFSEventStreamEventFlagItemModified) != 0;
    if (!created && !removed && !renamed && !modified)
        return; /* metadata, xattrs, owner: not a content change */

    u64 h = de_hash64(rel, rl, 0);
    u64 ino = ev_ino != 0 ? ev_ino : (exists ? (u64)st.st_ino : 0);
    bool stale = ino != 0 && seen_has(b, h, ino);
    if ((created || renamed) && ino != 0)
        seen_add(b, h, ino);
    if (b->priming)
        return;
    /* A file born before the watch started was not created now, whatever the flags say (a
     * renamed file keeps its birth time, so this applies to ItemCreated only). */
    bool born_before = exists && (st.st_birthtimespec.tv_sec < b->start_time.tv_sec ||
                                  (st.st_birthtimespec.tv_sec == b->start_time.tv_sec &&
                                   st.st_birthtimespec.tv_nsec < b->start_time.tv_nsec));
    bool old_inode = b->ino_floor != 0 && ino != 0 && ino < b->ino_floor;
    bool created_now = created && !stale && !born_before && !old_inode;
    bool renamed_now = renamed && !stale;

    if (exists) {
        if (renamed_now && ino == (u64)st.st_ino) {
            de__watch_post(w, DE__WATCH_RAW_MOVED_TO, rel, rl, is_dir, ino);
        } else if (renamed_now) {
            /* The renamed file left; another one is here now. */
            de__watch_post(w, DE__WATCH_RAW_MOVED_FROM, rel, rl, is_dir, ino);
            de__watch_post(w, DE__WATCH_RAW_CREATED, rel, rl, is_dir, 0);
        } else if (removed && (created_now || ino != (u64)st.st_ino)) {
            de__watch_post(w, DE__WATCH_RAW_DELETED, rel, rl, is_dir, 0);
            de__watch_post(w, DE__WATCH_RAW_CREATED, rel, rl, is_dir, 0);
        } else if (created_now) {
            de__watch_post(w, DE__WATCH_RAW_CREATED, rel, rl, is_dir, 0);
        }
        if (modified && !is_dir)
            de__watch_post(w, DE__WATCH_RAW_MODIFIED, rel, rl, false, 0);
    } else {
        if (created_now)
            de__watch_post(w, DE__WATCH_RAW_CREATED, rel, rl, is_dir, 0);
        if (renamed) /* even a stale flag: an unpaired MOVED_FROM ends as DELETED */
            de__watch_post(w, DE__WATCH_RAW_MOVED_FROM, rel, rl, is_dir, ino);
        else
            de__watch_post(w, DE__WATCH_RAW_DELETED, rel, rl, is_dir, 0);
    }
}

static void fse_callback(ConstFSEventStreamRef stream, void *info, size_t n, void *paths,
                         const FSEventStreamEventFlags flags[], const FSEventStreamEventId ids[]) {
    (void)stream;
    (void)ids;
    de__watch_backend *b = (de__watch_backend *)info;
    CFArrayRef arr = (CFArrayRef)paths;
    char path[MAXPATHLEN * 2];
    for (size_t i = 0; i < n; ++i) {
        CFDictionaryRef d = (CFDictionaryRef)CFArrayGetValueAtIndex(arr, (CFIndex)i);
        if (d == NULL)
            continue;
        CFStringRef p =
            (CFStringRef)CFDictionaryGetValue(d, kFSEventStreamEventExtendedDataPathKey);
        if (p == NULL || !CFStringGetCString(p, path, sizeof path, kCFStringEncodingUTF8))
            continue;
        u64 ino = 0;
        CFNumberRef num =
            (CFNumberRef)CFDictionaryGetValue(d, kFSEventStreamEventExtendedFileIDKey);
        if (num != NULL) {
            long long v = 0;
            if (CFNumberGetValue(num, kCFNumberLongLongType, &v))
                ino = (u64)v;
        }
        handle_event(b, path, flags[i], ino);
    }
}

static void fse_noop(void *ctx) {
    (void)ctx;
}

typedef struct fse_floor_set {
    de__watch_backend *b;
    u64 floor;
} fse_floor_set;

static void fse_set_floor(void *ctx) {
    const fse_floor_set *f = (const fse_floor_set *)ctx;
    f->b->ino_floor = f->floor;
}

static void fse_end_priming(void *ctx) {
    ((de__watch_backend *)ctx)->priming = false;
}

/* F_GETPATH spelling of the directory `path` (real case, /private prefix), or false. */
static bool canonical_dir(const char *path, char *out, size_t *len) {
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return false;
    bool ok = fcntl(fd, F_GETPATH, out) != -1;
    close(fd);
    if (!ok)
        return false;
    *len = strlen(out);
    while (*len > 1 && out[*len - 1] == '/')
        out[--*len] = '\0';
    return true;
}

/* Creates the barrier directory (see de__watch_backend.barrier); barrier_len stays 0 if the
 * user temp directory is not usable. */
static void make_barrier_dir(de__watch_backend *b) {
    static de_atomic_u32 counter;
    char tmp[MAXPATHLEN];
    char dir[MAXPATHLEN + 64];
    size_t n = confstr(_CS_DARWIN_USER_TEMP_DIR, tmp, sizeof tmp);
    if (n == 0 || n > sizeof tmp)
        return;
    snprintf(dir, sizeof dir, "%s%sde-watch-%d-%u", tmp, tmp[n - 2] == '/' ? "" : "/",
             (int)getpid(), (unsigned)de_atomic_fetch_add_u32(&counter, 1, DE_ATOMIC_RELAXED));
    if (mkdir(dir, 0700) != 0)
        return;
    if (!canonical_dir(dir, b->barrier, &b->barrier_len)) {
        rmdir(dir);
        b->barrier_len = 0;
    }
}

/* Waits until the events from before the start have gone by (see `priming`). */
static void pass_barrier(de__watch_backend *b) {
    if (b->barrier_len == 0) {
        /* No barrier directory: flush what fseventsd already has, and hope for the best. */
        FSEventStreamFlushSync(b->stream);
        dispatch_sync_f(b->queue, b, fse_end_priming);
        return;
    }
    /* The floor file first: its inode bounds the IDs of items created before the start. */
    char path[MAXPATHLEN + 16];
    snprintf(path, sizeof path, "%s/floor", b->barrier);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd >= 0) {
        struct stat fs, ws;
        u64 floor = 0;
        if (fstat(fd, &fs) == 0 && stat(b->canon, &ws) == 0 && fs.st_dev == ws.st_dev)
            floor = (u64)fs.st_ino;
        close(fd);
        unlink(path);
        dispatch_sync_f(b->queue, &(fse_floor_set){b, floor}, fse_set_floor);
    }
    char marker[MAXPATHLEN + 16];
    snprintf(marker, sizeof marker, "%s/marker", b->barrier);
    fd = open(marker, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd >= 0)
        close(fd);
    dispatch_time_t limit = dispatch_time(DISPATCH_TIME_NOW, (int64_t)FSE_BARRIER_TIMEOUT_NS);
    if (fd < 0 || dispatch_semaphore_wait(b->barrier_seen, limit) != 0)
        dispatch_sync_f(b->queue, b, fse_end_priming);
    unlink(marker);
    rmdir(b->barrier);
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
    b->given_len = strlen(info->native_dir);
    memcpy(b->given, info->native_dir, b->given_len + 1);
    if (!canonical_dir(info->native_dir, b->canon, &b->canon_len)) {
        memcpy(b->canon, b->given, b->given_len + 1);
        b->canon_len = b->given_len;
    }
    make_barrier_dir(b);

    CFStringRef dirs[2] = {NULL, NULL};
    CFIndex ndirs = 0;
    dirs[ndirs] = CFStringCreateWithCString(kCFAllocatorDefault, b->canon, kCFStringEncodingUTF8);
    ndirs += dirs[ndirs] != NULL ? 1 : 0;
    if (b->barrier_len > 0) {
        dirs[ndirs] =
            CFStringCreateWithCString(kCFAllocatorDefault, b->barrier, kCFStringEncodingUTF8);
        ndirs += dirs[ndirs] != NULL ? 1 : 0;
    }
    CFArrayRef arr = ndirs > 0 ? CFArrayCreate(kCFAllocatorDefault, (const void **)dirs, ndirs,
                                               &kCFTypeArrayCallBacks)
                               : NULL;
    FSEventStreamContext ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.info = b;
    if (arr != NULL)
        b->stream = FSEventStreamCreate(
            kCFAllocatorDefault, fse_callback, &ctx, arr, kFSEventStreamEventIdSinceNow,
            FSE_LATENCY_S,
            kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer |
                kFSEventStreamCreateFlagUseCFTypes | kFSEventStreamCreateFlagUseExtendedData);
    if (arr != NULL)
        CFRelease(arr);
    for (CFIndex i = 0; i < ndirs; ++i)
        CFRelease(dirs[i]);
    if (b->stream == NULL) {
        if (b->barrier_len > 0)
            rmdir(b->barrier);
        DE_FREE(b->alloc, b, sizeof *b);
        return DE_FAIL(DE_ERR_IO, "file watch: FSEventStreamCreate failed for '%s'",
                       info->native_dir);
    }
    b->queue = dispatch_queue_create("de.file_watch", DISPATCH_QUEUE_SERIAL);
    b->barrier_seen = dispatch_semaphore_create(0);
    FSEventStreamSetDispatchQueue(b->stream, b->queue);
    (void)clock_gettime(CLOCK_REALTIME, &b->start_time);
    b->priming = true;
    if (!FSEventStreamStart(b->stream)) {
        FSEventStreamInvalidate(b->stream);
        FSEventStreamRelease(b->stream);
        dispatch_release(b->queue);
        dispatch_release(b->barrier_seen);
        if (b->barrier_len > 0)
            rmdir(b->barrier);
        DE_FREE(b->alloc, b, sizeof *b);
        return DE_FAIL(DE_ERR_IO, "file watch: FSEventStreamStart failed for '%s'",
                       info->native_dir);
    }
    /* "Since now" still delivers changes made just before the start (fseventsd numbers events
     * when it receives them from the kernel), and their flags stick to the paths afterwards. */
    pass_barrier(b);
    *out = b;
    return DE_OK;
}

void de__watch_backend_stop(de__watch_backend *b) {
    if (b == NULL)
        return;
    FSEventStreamStop(b->stream);
    FSEventStreamInvalidate(b->stream);
    FSEventStreamRelease(b->stream);
    /* The queue is serial: once this empty item runs, a callback that was running has ended. */
    dispatch_sync_f(b->queue, NULL, fse_noop);
    dispatch_release(b->queue);
    dispatch_release(b->barrier_seen);
    DE_FREE(b->alloc, b, sizeof *b);
}

#endif
