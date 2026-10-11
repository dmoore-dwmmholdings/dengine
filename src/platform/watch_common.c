/* watch_common.c: de_file_watch, the part every OS shares (SPEC §6, DENG-248).
 *
 * The backend (watch_fsevents.c, watch_inotify.c, watch_win32.c) reports raw changes from its
 * own thread through de__watch_post(). They go into a pending table keyed by relative path,
 * behind one mutex. Each entry holds the merged state of its path and the time of its last raw
 * change; de_file_watch_poll() takes the entries whose last change is at least the debounce
 * window old, and calls back for them without the lock, on the polling thread.
 *
 * Merging (per path, see de_platform.h): CREATED + MODIFIED -> CREATED, CREATED + DELETED ->
 * nothing, DELETED + CREATED -> MODIFIED. Renames arrive as MOVED_FROM(old, cookie) and
 * MOVED_TO(new, cookie): the old entry becomes DELETED and remembers the cookie; a MOVED_TO with
 * the same cookie turns it into RENAMED on the new entry (old path attached) and drops the old
 * one. A MOVED_FROM whose path was itself created in this window (the temporary file of an
 * atomic save) makes the target MODIFIED instead, and nothing is reported for the temporary.
 *
 * Memory: the watch record and one record per pending path, from the watch's allocator. A poll
 * with nothing pending touches no allocator. More than WATCH_MAX_PENDING pending paths (a huge
 * burst nobody polls) clears the table and reports a rescan of the root instead. */
#include <dengine/de_debug.h>
#include <dengine/de_platform.h>

#include "platform/vfs_internal.h"
#include "platform/watch_internal.h"

#include <string.h>

#if DE__WATCH_ENABLED

/* Pending paths before the table is dropped for a rescan. */
#define WATCH_MAX_PENDING 65536u
/* Recent MOVED_FROM cookies remembered for pairing. A rename's two halves arrive within
 * microseconds of each other, so a small ring is enough. */
#define WATCH_MOVE_RING 256u

typedef enum entry_state {
    ST_NONE,     /* nothing to report (merged away) */
    ST_CREATED,  /* new */
    ST_MODIFIED, /* contents changed */
    ST_DELETED,  /* gone (or moved away, while `cookie` != 0 waits for the pair) */
    ST_RENAMED,  /* moved here from `old` */
} entry_state;

typedef struct watch_entry {
    struct watch_entry *next; /* delivery list */
    char *path;               /* relative, NUL-terminated, path_len + 1 bytes */
    char *old;                /* RENAMED: the source. Other states: a path that is gone too */
    u32 path_len, old_len;
    u64 hash;    /* of path */
    u64 last_ns; /* time of the last raw change */
    u64 cookie;  /* DELETED by a MOVED_FROM: its cookie, until paired (0 = none) */
    u8 state;
    bool is_dir;
    bool prior_created; /* DELETED by a MOVED_FROM of a path CREATED in this window */
} watch_entry;

typedef struct move_rec {
    u64 cookie;
    u64 hash;
} move_rec;

struct de_file_watch {
    const de_allocator *alloc;
    de_watch_fn fn;
    void *user;
    u32 debounce_ms; /* 0 = the cvar */
    de_cvar debounce_cvar;
    de__watch_backend *backend;
    bool delivering; /* inside a callback (Debug check against destroy / nested poll) */

    de_mutex lock;       /* everything below */
    watch_entry **items; /* insertion order = order of the first change */
    u32 count, cap;
    u32 *slots; /* open addressing: index into items + 1; 0 = empty */
    u32 slot_cap;
    move_rec moves[WATCH_MOVE_RING];
    u32 move_next;
    bool rescan;
    u64 rescan_ns;

    de__watch_info info;
    char vdir[DE_FS_PATH_MAX]; /* normalised virtual path of the watched directory */
};

/* ---- Pending table ------------------------------------------------------------------------ */

static u64 path_hash(const char *s, size_t n) {
    return de_hash64(s, n, 0x77617463u /* "watc" */);
}

static char *dup_str(const de_allocator *a, const char *s, size_t n) {
    char *p = (char *)DE_ALLOC(a, n + 1, 1, DE_MEM_GENERAL);
    if (p != NULL) {
        memcpy(p, s, n);
        p[n] = '\0';
    }
    return p;
}

static void free_entry(de_file_watch *w, watch_entry *e) {
    if (e->old != NULL)
        DE_FREE(w->alloc, e->old, (size_t)e->old_len + 1);
    DE_FREE(w->alloc, e->path, (size_t)e->path_len + 1);
    DE_FREE(w->alloc, e, sizeof *e);
}

static void set_old(de_file_watch *w, watch_entry *e, char *old, u32 old_len) {
    if (e->old != NULL)
        DE_FREE(w->alloc, e->old, (size_t)e->old_len + 1);
    e->old = old;
    e->old_len = old_len;
}

static void index_insert(de_file_watch *w, u32 item) {
    u32 mask = w->slot_cap - 1;
    for (u32 s = (u32)w->items[item]->hash & mask;; s = (s + 1) & mask) {
        if (w->slots[s] == 0) {
            w->slots[s] = item + 1;
            return;
        }
    }
}

/* Rebuilds the index with `slot_cap` slots (a power of two above twice the count). */
static bool index_rebuild(de_file_watch *w, u32 slot_cap) {
    if (slot_cap != w->slot_cap) {
        u32 *slots = (u32 *)DE_ALLOC(w->alloc, (size_t)slot_cap * sizeof(u32), 4, DE_MEM_GENERAL);
        if (slots == NULL)
            return false;
        if (w->slots != NULL)
            DE_FREE(w->alloc, w->slots, (size_t)w->slot_cap * sizeof(u32));
        w->slots = slots;
        w->slot_cap = slot_cap;
    }
    memset(w->slots, 0, (size_t)w->slot_cap * sizeof(u32));
    for (u32 i = 0; i < w->count; ++i)
        index_insert(w, i);
    return true;
}

static watch_entry *find_entry(de_file_watch *w, const char *rel, size_t len, u64 h) {
    if (w->slot_cap == 0)
        return NULL;
    u32 mask = w->slot_cap - 1;
    for (u32 s = (u32)h & mask; w->slots[s] != 0; s = (s + 1) & mask) {
        watch_entry *e = w->items[w->slots[s] - 1];
        if (e->hash == h && e->path_len == len && memcmp(e->path, rel, len) == 0)
            return e;
    }
    return NULL;
}

/* The entry in DELETED state that waits for the MOVED_TO of `cookie`, or NULL. */
static watch_entry *find_moved(de_file_watch *w, u64 cookie) {
    if (cookie == 0 || w->slot_cap == 0)
        return NULL;
    for (u32 i = 0; i < WATCH_MOVE_RING; ++i) {
        const move_rec *m = &w->moves[i];
        if (m->cookie != cookie)
            continue;
        u32 mask = w->slot_cap - 1;
        for (u32 s = (u32)m->hash & mask; w->slots[s] != 0; s = (s + 1) & mask) {
            watch_entry *e = w->items[w->slots[s] - 1];
            if (e->hash == m->hash && e->state == ST_DELETED && e->cookie == cookie)
                return e;
        }
    }
    return NULL;
}

static void clear_table(de_file_watch *w) {
    for (u32 i = 0; i < w->count; ++i)
        free_entry(w, w->items[i]);
    w->count = 0;
    if (w->slots != NULL)
        memset(w->slots, 0, (size_t)w->slot_cap * sizeof(u32));
    memset(w->moves, 0, sizeof w->moves);
}

/* Too many pending paths or no memory: forget them and ask for a rescan of the root. */
static void overflow(de_file_watch *w, u64 now) {
    clear_table(w);
    w->rescan = true;
    w->rescan_ns = now;
}

static watch_entry *get_entry(de_file_watch *w, const char *rel, size_t len, u64 now) {
    u64 h = path_hash(rel, len);
    watch_entry *e = find_entry(w, rel, len, h);
    if (e != NULL)
        return e;
    if (w->count >= WATCH_MAX_PENDING) {
        overflow(w, now);
        return NULL;
    }
    if (w->count == w->cap) {
        u32 cap = w->cap != 0 ? w->cap * 2 : 64;
        watch_entry **items =
            (watch_entry **)DE_REALLOC(w->alloc, w->items, (size_t)w->cap * sizeof *items,
                                       (size_t)cap * sizeof *items, sizeof(void *), DE_MEM_GENERAL);
        if (items == NULL) {
            overflow(w, now);
            return NULL;
        }
        w->items = items;
        w->cap = cap;
    }
    if ((w->count + 1) * 2 > w->slot_cap) {
        u32 slot_cap = w->slot_cap != 0 ? w->slot_cap * 2 : 128;
        if (!index_rebuild(w, slot_cap)) {
            overflow(w, now);
            return NULL;
        }
    }
    e = (watch_entry *)DE_ALLOC(w->alloc, sizeof *e, _Alignof(watch_entry), DE_MEM_GENERAL);
    char *p = dup_str(w->alloc, rel, len);
    if (e == NULL || p == NULL) {
        if (e != NULL)
            DE_FREE(w->alloc, e, sizeof *e);
        if (p != NULL)
            DE_FREE(w->alloc, p, len + 1);
        overflow(w, now);
        return NULL;
    }
    memset(e, 0, sizeof *e);
    e->path = p;
    e->path_len = (u32)len;
    e->hash = h;
    w->items[w->count] = e;
    index_insert(w, w->count);
    w->count++;
    return e;
}

/* ---- Merge rules -------------------------------------------------------------------------- */

static void apply_created(watch_entry *e) {
    switch ((entry_state)e->state) {
    case ST_NONE:
        e->state = ST_CREATED;
        break;
    case ST_DELETED:
        /* Replaced. A temporary that moved away and came back is simply new. */
        e->state = e->prior_created ? ST_CREATED : ST_MODIFIED;
        break;
    case ST_CREATED:
    case ST_MODIFIED:
    case ST_RENAMED:
        break;
    }
    e->cookie = 0;
    e->prior_created = false;
}

static void apply_modified(watch_entry *e) {
    switch ((entry_state)e->state) {
    case ST_NONE:
        e->state = ST_MODIFIED;
        break;
    case ST_DELETED: /* deleted, then written again: as DELETED + CREATED */
        apply_created(e);
        break;
    case ST_CREATED:
    case ST_MODIFIED:
    case ST_RENAMED:
        break;
    }
}

static void apply_deleted(de_file_watch *w, watch_entry *e) {
    switch ((entry_state)e->state) {
    case ST_NONE:
    case ST_MODIFIED:
    case ST_DELETED:
        e->state = ST_DELETED;
        break;
    case ST_CREATED:
        e->state = ST_NONE; /* created and deleted in one window: nothing happened */
        break;
    case ST_RENAMED:
        /* Moved here, then deleted: the source is gone too. `old` keeps it, reported first. */
        e->state = ST_DELETED;
        break;
    }
    e->cookie = 0;
    e->prior_created = false;
    (void)w;
}

static void apply_moved_from(de_file_watch *w, watch_entry *e, u64 cookie) {
    bool created = e->state == ST_CREATED || (e->state == ST_DELETED && e->prior_created);
    e->state = ST_DELETED;
    e->prior_created = created;
    e->cookie = cookie;
    if (cookie != 0) {
        move_rec *m = &w->moves[w->move_next];
        m->cookie = cookie;
        m->hash = e->hash;
        w->move_next = (w->move_next + 1) % WATCH_MOVE_RING;
    }
}

/* Contents arrived at `b` (an atomic save's rename). */
static void apply_arrived(watch_entry *b) {
    switch ((entry_state)b->state) {
    case ST_NONE:
    case ST_DELETED:
        b->state = ST_MODIFIED;
        break;
    case ST_CREATED:
    case ST_MODIFIED:
    case ST_RENAMED:
        break;
    }
    b->cookie = 0;
    b->prior_created = false;
}

static void apply_moved_to(de_file_watch *w, watch_entry *b, u64 cookie) {
    watch_entry *a = find_moved(w, cookie);
    if (a == NULL || a == b) {
        apply_created(b); /* moved in from outside the tree */
        return;
    }
    a->cookie = 0;
    if (a->prior_created) {
        /* A file created in this window and renamed over `b`: an atomic save of `b`. */
        a->state = ST_NONE;
        a->prior_created = false;
        apply_arrived(b);
        return;
    }
    /* The source as the consumer knows it: `a`, or what `a` was renamed from. */
    char *src;
    u32 src_len;
    if (a->old != NULL) {
        src = a->old;
        src_len = a->old_len;
        a->old = NULL;
        a->old_len = 0;
    } else {
        src = dup_str(w->alloc, a->path, a->path_len);
        src_len = a->path_len;
    }
    a->state = ST_NONE;
    if (src == NULL) { /* out of memory: report a delete + create instead */
        a->state = ST_DELETED;
        apply_created(b);
        return;
    }
    if (src_len == b->path_len && memcmp(src, b->path, src_len) == 0) {
        DE_FREE(w->alloc, src, (size_t)src_len + 1); /* renamed back */
        apply_arrived(b);
        return;
    }
    set_old(w, b, src, src_len);
    b->state = ST_RENAMED;
    b->cookie = 0;
    b->prior_created = false;
}

void de__watch_post(de_file_watch *w, de__watch_raw kind, const char *rel, size_t rel_len,
                    bool is_dir, u64 cookie) {
    u64 now = de_time_now_ns();
    de_mutex_lock(&w->lock);
    if (kind == DE__WATCH_RAW_RESCAN) {
        w->rescan = true;
        w->rescan_ns = now;
        de_mutex_unlock(&w->lock);
        return;
    }
    /* Directories have no contents to reload; FSEvents watches recursively anyway. */
    bool skip = rel_len == 0 || rel_len >= DE_FS_PATH_MAX ||
                (kind == DE__WATCH_RAW_MODIFIED && is_dir) ||
                (!w->info.recursive && memchr(rel, '/', rel_len) != NULL);
    watch_entry *e = skip ? NULL : get_entry(w, rel, rel_len, now);
    if (e != NULL) {
        e->last_ns = now;
        if (kind == DE__WATCH_RAW_CREATED || kind == DE__WATCH_RAW_MOVED_TO)
            e->is_dir = is_dir;
        else
            e->is_dir = e->is_dir || is_dir;
        switch (kind) {
        case DE__WATCH_RAW_CREATED:
            apply_created(e);
            break;
        case DE__WATCH_RAW_MODIFIED:
            apply_modified(e);
            break;
        case DE__WATCH_RAW_DELETED:
            apply_deleted(w, e);
            break;
        case DE__WATCH_RAW_MOVED_FROM:
            apply_moved_from(w, e, cookie);
            break;
        case DE__WATCH_RAW_MOVED_TO:
            apply_moved_to(w, e, cookie);
            break;
        case DE__WATCH_RAW_RESCAN:
            break;
        }
    }
    de_mutex_unlock(&w->lock);
}

u32 de__watch_pending_count(de_file_watch *w) {
    de_mutex_lock(&w->lock);
    u32 n = w->count + (w->rescan ? 1u : 0u);
    de_mutex_unlock(&w->lock);
    return n;
}

/* ---- API ---------------------------------------------------------------------------------- */

static de_cvar register_debounce_cvar(void) {
    de_cvar_desc d = {0};
    d.name = "fs.watch_debounce_ms";
    d.help = "File watch debounce window in ms: a change is reported once its path is quiet "
             "this long";
    d.type = DE_CVAR_INT;
    d.flags = DE_CVAR_DEV_ONLY;
    d.default_number = DE_WATCH_DEBOUNCE_DEFAULT_MS;
    d.min = 0;
    d.max = 60000;
    return de_cvar_register(&d);
}

de_result de_file_watch_create(const de_file_watch_desc *desc, de_file_watch **out) {
    if (out != NULL)
        *out = NULL;
    if (desc == NULL || out == NULL || desc->dir == NULL || desc->fn == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_file_watch_create: desc, desc->dir, desc->fn and out are required");
    char vdir[DE_FS_PATH_MAX];
    char native[DE_FS_PATH_MAX];
    DE_TRY(de_path_normalize(desc->dir, vdir, sizeof vdir));
    DE_TRY(de_fs_resolve(vdir, native, sizeof native));
    bool is_dir = false;
    if (de__native_stat(native, &is_dir) != DE_OK)
        return DE_FAIL(DE_ERR_NOT_FOUND, "de_file_watch_create: '%s' does not exist", vdir);
    if (!is_dir)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_file_watch_create: '%s' is not a directory",
                       vdir);

    const de_allocator *a = desc->allocator != NULL ? desc->allocator : de_heap_allocator();
    de_file_watch *w =
        (de_file_watch *)DE_ALLOC(a, sizeof *w, _Alignof(de_file_watch), DE_MEM_GENERAL);
    if (w == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_file_watch_create: out of memory");
    memset(w, 0, sizeof *w);
    w->alloc = a;
    w->fn = desc->fn;
    w->user = desc->user;
    w->debounce_ms = desc->debounce_ms;
    /* Registered lazily (DENG-154 registry); registering again returns the same cvar. */
    w->debounce_cvar = register_debounce_cvar();
    de_error_clear(); /* a failed registration only means the default window */
    de_mutex_init(&w->lock);
    w->info.alloc = a;
    w->info.recursive = desc->recursive;
    memcpy(w->info.native_dir, native, strlen(native) + 1);
    memcpy(w->vdir, vdir, strlen(vdir) + 1);

    de_result r = de__watch_backend_start(w, &w->info, &w->backend);
    if (r != DE_OK) {
        de_mutex_destroy(&w->lock);
        DE_FREE(a, w, sizeof *w);
        return r;
    }
    *out = w;
    return DE_OK;
}

static u64 debounce_ns(const de_file_watch *w) {
    i64 ms = w->debounce_ms;
    if (ms == 0)
        ms =
            w->debounce_cvar.id != 0 ? de_cvar_int(w->debounce_cvar) : DE_WATCH_DEBOUNCE_DEFAULT_MS;
    if (ms < 0)
        ms = 0;
    return (u64)ms * 1000000u;
}

/* `vdir` + "/" + `rel` into `buf` (cut to fit, which DE_FS_PATH_MAX-sized paths never need). */
static const char *join_vpath(const de_file_watch *w, const char *rel, char *buf, size_t cap) {
    size_t vl = strlen(w->vdir);
    size_t rl = strlen(rel);
    bool slash = vl > 0 && w->vdir[vl - 1] != '/';
    if (vl + (slash ? 1u : 0u) + rl + 1 > cap)
        rl = cap - 1 - vl - (slash ? 1u : 0u);
    memcpy(buf, w->vdir, vl);
    if (slash)
        buf[vl++] = '/';
    memcpy(buf + vl, rel, rl);
    buf[vl + rl] = '\0';
    return buf;
}

static void emit(de_file_watch *w, const char *path, const char *old, de_watch_change change,
                 bool is_dir) {
    de_watch_event ev;
    ev.path = path;
    ev.old_path = old;
    ev.change = change;
    ev.is_dir = is_dir;
    w->fn(&ev, w->user);
}

u32 de_file_watch_poll(de_file_watch *w) {
    if (w == NULL)
        return 0;
    DE_ASSERT(!w->delivering);
    u64 window = debounce_ns(w);
    watch_entry *head = NULL, **tail = &head;
    bool rescan = false;

    de_mutex_lock(&w->lock);
    u64 now = de_time_now_ns();
    if (w->rescan && now - w->rescan_ns >= window) {
        rescan = true;
        w->rescan = false;
    }
    u32 kept = 0;
    for (u32 i = 0; i < w->count; ++i) {
        watch_entry *e = w->items[i];
        if (now - e->last_ns >= window) {
            e->next = NULL;
            *tail = e;
            tail = &e->next;
        } else {
            w->items[kept++] = e;
        }
    }
    if (kept != w->count) {
        w->count = kept;
        (void)index_rebuild(w, w->slot_cap); /* same size: cannot fail */
    }
    de_mutex_unlock(&w->lock);

    if (head == NULL && !rescan)
        return 0;
    /* Two paths of at most DE_FS_PATH_MAX each; the virtual prefix adds at most that much. */
    char path[2 * DE_FS_PATH_MAX];
    char old[2 * DE_FS_PATH_MAX];
    u32 calls = 0;
    w->delivering = true;
    if (rescan) {
        emit(w, w->vdir, NULL, DE_WATCH_MODIFIED, true);
        ++calls;
    }
    for (watch_entry *e = head; e != NULL;) {
        watch_entry *next = e->next;
        join_vpath(w, e->path, path, sizeof path);
        if (e->old != NULL && e->state != ST_RENAMED && e->state != ST_NONE) {
            /* A rename source that is gone as well (renamed here, then deleted or replaced). */
            emit(w, join_vpath(w, e->old, old, sizeof old), NULL, DE_WATCH_DELETED, e->is_dir);
            ++calls;
        }
        switch ((entry_state)e->state) {
        case ST_NONE:
            break;
        case ST_CREATED:
            emit(w, path, NULL, DE_WATCH_CREATED, e->is_dir);
            ++calls;
            break;
        case ST_MODIFIED:
            if (!e->is_dir) {
                emit(w, path, NULL, DE_WATCH_MODIFIED, false);
                ++calls;
            }
            break;
        case ST_DELETED:
            if (!e->prior_created) {
                emit(w, path, NULL, DE_WATCH_DELETED, e->is_dir);
                ++calls;
            }
            break;
        case ST_RENAMED:
            emit(w, path, join_vpath(w, e->old, old, sizeof old), DE_WATCH_RENAMED, e->is_dir);
            ++calls;
            break;
        }
        free_entry(w, e);
        e = next;
    }
    w->delivering = false;
    return calls;
}

void de_file_watch_destroy(de_file_watch *w) {
    if (w == NULL)
        return;
    DE_ASSERT(!w->delivering);
    /* After this no backend thread posts, so the table is ours alone. */
    de__watch_backend_stop(w->backend);
    clear_table(w);
    if (w->items != NULL)
        DE_FREE(w->alloc, w->items, (size_t)w->cap * sizeof *w->items);
    if (w->slots != NULL)
        DE_FREE(w->alloc, w->slots, (size_t)w->slot_cap * sizeof(u32));
    de_mutex_destroy(&w->lock);
    DE_FREE(w->alloc, w, sizeof *w);
}

#else /* !DE__WATCH_ENABLED: Release and Shipping */

de_result de_file_watch_create(const de_file_watch_desc *desc, de_file_watch **out) {
    (void)desc;
    if (out != NULL)
        *out = NULL;
    return DE_FAIL(DE_ERR_UNSUPPORTED, "file watching exists only in Debug and Dev builds");
}

u32 de_file_watch_poll(de_file_watch *w) {
    (void)w;
    return 0;
}

void de_file_watch_destroy(de_file_watch *w) {
    (void)w;
}

#endif
