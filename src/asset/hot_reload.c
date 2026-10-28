/*
 * hot_reload.c: asset hot reload (SPEC §11.9, §11.3 RELOADED; DENG-214). See
 * hot_reload_internal.h for the steps and the rules. Debug and Dev builds only; Release and
 * Shipping compile de_asset_last_error and no-op hooks.
 */

#include "asset/diagnostic_internal.h"
#include "asset/hot_reload_internal.h"

#include "asset/asset_internal.h"
#include "asset/budget_internal.h"
#include "asset/load_internal.h"
#include "asset/manifest_internal.h"
#include "asset/mount_internal.h"
#include "asset/pipeline_internal.h"
#include "asset/resolve_internal.h"
#include "asset/slots_internal.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- de_asset_last_error (every build) --------------------------------------------------------
 */

static void copy_msg(char *msg, u32 cap, const char *text) {
    if (msg != NULL && cap != 0)
        snprintf(msg, cap, "%s", text != NULL ? text : "");
}

#if DE__ASSET_RELOAD_ENABLED
static de_result reload_error_of(de_asset_system *s, de_asset_id id, const char **msg);
#endif

de_result de_asset_last_error(de_asset_system *s, de_asset_id id, char *msg, u32 cap) {
    copy_msg(msg, cap, "");
    if (s == NULL)
        return DE_ERR_INVALID_ARGUMENT;
    u32 h = de__slot_find(s, id);
    const de__slot *sl = h != 0 ? de__slot_get(s, h) : NULL;
    if (sl == NULL)
        return DE_OK;
#if DE__ASSET_RELOAD_ENABLED
    if (sl->state == DE_LOAD_READY) {
        const char *text = NULL;
        de_result r = reload_error_of(s, id, &text);
        if (r != DE_OK) {
            copy_msg(msg, cap, text);
            return r;
        }
    }
#endif
    if (sl->state != DE_LOAD_FAILED)
        return DE_OK;
    de_result code = sl->error != DE_OK ? sl->error : DE_ERR_INTERNAL;
    const de__load_slot *ls = de__load_slot_find(s, h);
    if (ls != NULL && ls->error_msg != NULL) {
        copy_msg(msg, cap, ls->error_msg);
    } else if (msg != NULL && cap != 0) {
        const char *path = de__resolved_path(s->resolved, id);
        if (path != NULL)
            snprintf(msg, cap, "asset \"%s\" failed to load: %s", path, de_result_str(code));
        else
            snprintf(msg, cap, "asset %016llx failed to load: %s", (unsigned long long)id.hash,
                     de_result_str(code));
    }
    return code;
}

#if !DE__ASSET_RELOAD_ENABLED

/* ---- Release and Shipping: hot reload is compiled out ------------------------------------------
 */

de_result de__asset_reload_init(de_asset_system *s) {
    if (s->desc.hot_reload)
        DE_INFO(s->log, "hot reload is compiled out of this build (Debug and Dev builds only); "
                        "de_asset_system_desc.hot_reload is ignored");
    return DE_OK;
}
void de__asset_reload_shutdown(de_asset_system *s) {
    (void)s;
}
void de__asset_reload_update(de_asset_system *s) {
    (void)s;
}
void de__asset_reload_on_change(de_asset_system *s, const char *vpath, de_watch_change change,
                                bool is_dir) {
    (void)s;
    (void)vpath;
    (void)change;
    (void)is_dir;
}
void de__asset_reload_record_paths(de_asset_system *s, struct de_asset_load_ctx *job) {
    (void)s;
    (void)job;
}
void de__asset_reload_swap(de_asset_system *s, struct de_asset_load_ctx *job, de_asset_id *deps) {
    /* Unreachable: no reload job exists without hot reload. Drop the result like a stale one. */
    if (job->object != NULL && job->type->unload != NULL)
        job->type->unload(job->object, job->type->user);
    job->object = NULL;
    if (deps != NULL)
        DE_FREE(s->alloc, deps, sizeof(de_asset_id) * job->dep_slot_count);
}
void de__asset_reload_failed(de_asset_system *s, struct de_asset_load_ctx *job, de_result code,
                             const char *msg) {
    (void)s;
    (void)job;
    (void)code;
    (void)msg;
}
void de__asset_reload_job_gone(de_asset_system *s, struct de_asset_load_ctx *job) {
    (void)s;
    (void)job;
}
void de__asset_reload_set_watching(de_asset_system *s, bool watching) {
    (void)s;
    (void)watching;
}
void de__asset_reload_get_stats(const de_asset_system *s, de__asset_reload_stats *out) {
    (void)s;
    memset(out, 0, sizeof *out);
}
bool de__asset_reload_queue(de_asset_system *s, de_asset_id id) {
    (void)s;
    (void)id;
    return false;
}

#else /* DE__ASSET_RELOAD_ENABLED */

/* ---- State ----------------------------------------------------------------------------------- */

/* What hot reload knows about one asset: its reload job in flight, its last reload error. */
typedef struct reload_rec {
    de_asset_id id;
    de__pjob *job;   /* the reload job in flight, or NULL */
    de_result error; /* the last reload failure while it holds; DE_OK */
    char *msg;       /* its message (cause chain), owned; NULL */
    u32 msg_size;
    bool pack_logged; /* "served by a pack" logged once */
} reload_rec;

/* An object a reload replaced, unloaded `frames_left` updates later with its dependency refs. */
typedef struct retired_obj {
    de_asset_id id;
    const de_asset_type_desc *type;
    void *object;
    de_asset_id *deps; /* the old dependency list; its asset refs are still held */
    u32 dep_count;
    u32 frames_left;
} retired_obj;

/* One directory watch. */
typedef struct reload_watch {
    char *dir; /* normalised virtual path, owned */
    size_t dir_size;
    bool recursive;
    bool keep; /* (sync) still wanted */
    de_file_watch *w;
} reload_watch;

struct de__asset_reload {
    de_asset_system *s;
    const de_allocator *alloc;
    de__rindex index;
    reload_rec *recs; /* in creation order; never removed before shutdown */
    u32 rec_count, rec_cap;
    de_hashmap rec_index; /* u64 ID -> u32 index into recs (probed, never iterated) */
    de_asset_id *queue;   /* assets to reload this frame (duplicates dropped when started) */
    u32 queue_count, queue_cap;
    retired_obj *retired;
    u32 retired_count, retired_cap;
    reload_watch *watches;
    u32 watch_count, watch_cap;
    u64 watch_key; /* manifest_key of the manifest the watches were made for; 0 = none */
    bool watching; /* false: tests drive de__asset_reload_on_change */
    bool manifest_dirty;
    bool rescan;
    u32 retire_frames;
    de__asset_reload_stats stats;
};

typedef struct de__asset_reload de__asset_reload;

/* Grows `*arr` (of `elem` bytes) to hold `need`; false when out of memory. */
static bool grow(const de_allocator *a, void **arr, u32 *cap, u32 need, size_t elem, size_t align) {
    if (need <= *cap)
        return true;
    u32 n = *cap != 0 ? *cap : 16u;
    while (n < need)
        n *= 2u;
    void *p = DE_REALLOC(a, *arr, elem * *cap, elem * n, align, DE_MEM_ASSET);
    if (p == NULL)
        return false;
    *arr = p;
    *cap = n;
    return true;
}

/* The asset path of `id`, or its hex. */
static const char *name_of(de_asset_system *s, de_asset_id id, char *buf, size_t size) {
    const char *p = de__resolved_path(s->resolved, id);
    if (p != NULL)
        return p;
    snprintf(buf, size, "%016llx", (unsigned long long)id.hash);
    return buf;
}

/* ---- Records ---------------------------------------------------------------------------------
 */

static reload_rec *rec_find(de__asset_reload *R, de_asset_id id) {
    const u32 *i = (const u32 *)de_hashmap_get(&R->rec_index, id.hash, &id.hash);
    return i != NULL ? &R->recs[*i] : NULL;
}

/* The record of `id`, created if needed (NULL when out of memory). Pointers into the records
 * stay valid until the next rec_get. */
static reload_rec *rec_get(de__asset_reload *R, de_asset_id id) {
    reload_rec *rec = rec_find(R, id);
    if (rec != NULL)
        return rec;
    if (!grow(R->alloc, (void **)&R->recs, &R->rec_cap, R->rec_count + 1u, sizeof(reload_rec),
              DE_ALIGNOF(reload_rec)))
        return NULL;
    u32 at = R->rec_count;
    if (de_hashmap_put(&R->rec_index, id.hash, &id.hash, &at) != DE_OK)
        return NULL;
    rec = &R->recs[at];
    memset(rec, 0, sizeof *rec);
    rec->id = id;
    rec->error = DE_OK;
    R->rec_count++;
    return rec;
}

static void rec_clear_error(de__asset_reload *R, reload_rec *rec) {
    if (rec->msg != NULL)
        DE_FREE(R->alloc, rec->msg, rec->msg_size);
    rec->msg = NULL;
    rec->msg_size = 0;
    rec->error = DE_OK;
}

/* Keeps `code` and `msg` as the reload error of `id`, logs it at Error, and marks the slot. */
static void record_error(de__asset_reload *R, de_asset_id id, de_result code, const char *msg) {
    de_asset_system *s = R->s;
    char hex[24];
    const char *text = msg != NULL && msg[0] != '\0' ? msg : de_result_str(code);
    DE_ERROR(s->log, "hot reload of \"%s\" failed; the loaded version stays: %s",
             name_of(s, id, hex, sizeof hex), text);
    R->stats.failed++;
    u32 h = de__slot_find(s, id);
    de__slot *sl = h != 0 ? de__slot_get(s, h) : NULL;
    if (sl != NULL && sl->state == DE_LOAD_READY)
        sl->error = code;
    reload_rec *rec = rec_get(R, id);
    if (rec == NULL)
        return;
    rec_clear_error(R, rec);
    rec->error = code;
    size_t n = strlen(text) + 1u;
    rec->msg = (char *)DE_ALLOC(R->alloc, n, 1, DE_MEM_ASSET);
    if (rec->msg != NULL) {
        memcpy(rec->msg, text, n);
        rec->msg_size = (u32)n;
    }
}

static de_result reload_error_of(de_asset_system *s, de_asset_id id, const char **msg) {
    de__asset_reload *R = s->reload;
    const reload_rec *rec = R != NULL ? rec_find(R, id) : NULL;
    if (rec == NULL || rec->error == DE_OK)
        return DE_OK;
    *msg = rec->msg != NULL ? rec->msg : de_result_str(rec->error);
    return rec->error;
}

/* ---- Queue ---------------------------------------------------------------------------------
 */

static void queue_add(de__asset_reload *R, de_asset_id id) {
    if (!grow(R->alloc, (void **)&R->queue, &R->queue_cap, R->queue_count + 1u, sizeof(de_asset_id),
              DE_ALIGNOF(de_asset_id))) {
        DE_ERROR(R->s->log, "hot reload: out of memory queueing an asset; the change is lost");
        return;
    }
    R->queue[R->queue_count++] = id;
}

static int id_cmp(const void *a, const void *b) {
    u64 x = ((const de_asset_id *)a)->hash, y = ((const de_asset_id *)b)->hash;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* Sorts the queue by ID and drops duplicates: each asset reloads once per frame, in ID order. */
static void queue_unique(de__asset_reload *R) {
    if (R->queue_count > 1u)
        qsort(R->queue, R->queue_count, sizeof(de_asset_id), id_cmp);
    u32 w = 0;
    for (u32 i = 0; i < R->queue_count; ++i)
        if (w == 0 || R->queue[w - 1u].hash != R->queue[i].hash)
            R->queue[w++] = R->queue[i];
    R->queue_count = w;
    R->stats.queued += w;
}

static void queue_cb(de_asset_id id, void *user) {
    queue_add((de__asset_reload *)user, id);
}

/* ---- Retired objects --------------------------------------------------------------------------
 */

static void retire_now(de_asset_system *s, const retired_obj *o) {
    if (o->object != NULL && o->type != NULL && o->type->unload != NULL)
        o->type->unload(o->object, o->type->user);
    for (u32 i = 0; i < o->dep_count; ++i) {
        u32 h = de__slot_find(s, o->deps[i]);
        if (h != 0)
            (void)de__slot_unref_asset(s, h);
    }
    if (o->deps != NULL)
        DE_FREE(s->alloc, o->deps, sizeof(de_asset_id) * o->dep_count);
}

static void retire_push(de__asset_reload *R, de_asset_id id, const de_asset_type_desc *type,
                        const de__slot_swapped *old) {
    retired_obj o = {id, type, old->object, old->deps, old->dep_count, R->retire_frames};
    if (!grow(R->alloc, (void **)&R->retired, &R->retired_cap, R->retired_count + 1u,
              sizeof(retired_obj), DE_ALIGNOF(retired_obj))) {
        DE_WARN(R->s->log, "hot reload: out of memory; the old version is unloaded at once");
        retire_now(R->s, &o);
        R->stats.retired++;
        return;
    }
    R->retired[R->retired_count++] = o;
}

/* Once per update, before anything else: an entry retired in update U is unloaded in update
 * U + retire_frames. Kept in order. */
static void retire_tick(de__asset_reload *R) {
    u32 w = 0;
    for (u32 i = 0; i < R->retired_count; ++i) {
        retired_obj *o = &R->retired[i];
        if (o->frames_left > 0)
            o->frames_left--;
        if (o->frames_left == 0) {
            retire_now(R->s, o);
            R->stats.retired++;
            continue;
        }
        R->retired[w++] = *o;
    }
    R->retired_count = w;
}

/* ---- Watches ----------------------------------------------------------------------------------
 */

static void watch_cb(const de_watch_event *ev, void *user) {
    de__asset_reload *R = (de__asset_reload *)user;
    if (ev->change == DE_WATCH_RENAMED && ev->old_path != NULL)
        de__asset_reload_on_change(R->s, ev->old_path, DE_WATCH_DELETED, ev->is_dir);
    de__asset_reload_on_change(R->s, ev->path, ev->change, ev->is_dir);
}

/* A key of what the watches depend on: the root, the asset root and every manifest file. */
static u64 manifest_key(const de__manifest *m) {
    if (m == NULL)
        return 0;
    u64 h = de_hash64_str(m->root);
    h = de_hash64(m->asset_root, strlen(m->asset_root), h);
    for (u32 i = 0; i < m->file_count; ++i)
        h = de_hash64(m->files[i], strlen(m->files[i]) + 1u, h);
    return h != 0 ? h : 1u;
}

/* The directory of the normalised file path `file` ("dev:/a/b.toml" -> "dev:/a", "dev:/b.toml"
 * -> "dev:/"). False if it does not fit. */
static bool dir_of(const char *file, char *out, size_t size) {
    const char *slash = strrchr(file, '/');
    if (slash == NULL)
        return false;
    size_t n = (size_t)(slash - file);
    if (n > 0 && file[n - 1u] == ':')
        n++; /* keep the root's "/" */
    if (n + 1u > size)
        return false;
    memcpy(out, file, n);
    out[n] = '\0';
    return true;
}

/* True when `dir` is `parent` or inside it. */
static bool dir_within(const char *dir, const char *parent) {
    size_t n = strlen(parent);
    if (strncmp(dir, parent, n) != 0)
        return false;
    return dir[n] == '\0' || dir[n] == '/' || (n > 0 && parent[n - 1u] == '/');
}

typedef struct want_dir {
    char dir[DE_FS_PATH_MAX];
    bool recursive;
} want_dir;

static void watch_destroy(de__asset_reload *R, reload_watch *w) {
    de_file_watch_destroy(w->w);
    if (w->w != NULL && R->stats.watches > 0)
        R->stats.watches--;
    w->w = NULL;
    if (w->dir != NULL)
        DE_FREE(R->alloc, w->dir, w->dir_size);
    w->dir = NULL;
}

static void watches_clear(de__asset_reload *R) {
    for (u32 i = 0; i < R->watch_count; ++i)
        watch_destroy(R, &R->watches[i]);
    R->watch_count = 0;
    R->watch_key = 0;
}

/* Makes the watch for `want` unless one exists (marks it kept). */
static void watch_want(de__asset_reload *R, const want_dir *want) {
    for (u32 i = 0; i < R->watch_count; ++i) {
        reload_watch *w = &R->watches[i];
        if (w->recursive == want->recursive && strcmp(w->dir, want->dir) == 0) {
            w->keep = true;
            return;
        }
    }
    if (!grow(R->alloc, (void **)&R->watches, &R->watch_cap, R->watch_count + 1u,
              sizeof(reload_watch), DE_ALIGNOF(reload_watch)))
        return;
    reload_watch *w = &R->watches[R->watch_count];
    memset(w, 0, sizeof *w);
    w->dir_size = strlen(want->dir) + 1u;
    w->dir = (char *)DE_ALLOC(R->alloc, w->dir_size, 1, DE_MEM_ASSET);
    if (w->dir == NULL)
        return;
    memcpy(w->dir, want->dir, w->dir_size);
    w->recursive = want->recursive;
    w->keep = true;
    R->watch_count++;
    de_file_watch_desc d = {.dir = w->dir,
                            .recursive = want->recursive,
                            .fn = watch_cb,
                            .user = R,
                            .allocator = R->alloc};
    de_result r = de_file_watch_create(&d, &w->w);
    if (r != DE_OK) {
        const de_error *e = de_error_last();
        DE_WARN(R->s->log, "hot reload: cannot watch %s: %s; its changes are not seen", w->dir,
                e != NULL && e->result == r ? e->message : de_result_str(r));
        w->w = NULL;
        return;
    }
    R->stats.watches++;
}

/* Watches the asset root (recursive) and the directory of every manifest file of the active
 * manifest. Main thread; once per manifest (the key changes when it does). */
static void watches_sync(de__asset_reload *R) {
    de_asset_system *s = R->s;
    if (!R->watching || s->manifest == NULL)
        return;
    u64 key = manifest_key(s->manifest);
    if (key == R->watch_key)
        return;
    R->watch_key = key;
    const de__manifest *m = s->manifest;
    for (u32 i = 0; i < R->watch_count; ++i)
        R->watches[i].keep = false;

    /* The asset root, like the pipeline's source paths: `asset_root` next to the root manifest. */
    want_dir root;
    root.dir[0] = '\0';
    root.recursive = true;
    char mdir[DE_FS_PATH_MAX], joined[DE_FS_PATH_MAX * 2];
    bool have_root = false;
    if (dir_of(m->root, mdir, sizeof mdir)) {
        int n = snprintf(joined, sizeof joined, "%s/%s", mdir, m->asset_root);
        have_root = n > 0 && (size_t)n < sizeof joined &&
                    de_path_normalize(joined, root.dir, sizeof root.dir) == DE_OK;
    }
    if (have_root)
        watch_want(R, &root);
    for (u32 i = 0; i < m->file_count; ++i) {
        want_dir d;
        d.recursive = false;
        char raw[DE_FS_PATH_MAX];
        if (!dir_of(m->files[i], raw, sizeof raw) ||
            de_path_normalize(raw, d.dir, sizeof d.dir) != DE_OK)
            continue;
        if (have_root && dir_within(d.dir, root.dir))
            continue; /* the recursive watch sees it */
        watch_want(R, &d);
    }
    u32 w = 0;
    for (u32 i = 0; i < R->watch_count; ++i) {
        if (!R->watches[i].keep) {
            watch_destroy(R, &R->watches[i]);
            continue;
        }
        R->watches[w++] = R->watches[i];
    }
    R->watch_count = w;
}

/* ---- Manifest ---------------------------------------------------------------------------------
 */

static bool is_manifest_file(const de__manifest *m, const char *vpath) {
    for (u32 i = 0; m != NULL && i < m->file_count; ++i)
        if (strcmp(m->files[i], vpath) == 0)
            return true;
    return false;
}

/* Loads the manifest again and reloads what changed (hot_reload_internal.h, step 2). */
static void manifest_reload(de__asset_reload *R) {
    de_asset_system *s = R->s;
    const de__manifest *cur = s->manifest != NULL ? s->manifest : s->failed_manifest;
    if (cur == NULL)
        return;
    char root[DE_FS_PATH_MAX];
    snprintf(root, sizeof root, "%s", cur->root); /* the load frees `cur` */
    de__manifest_digest old_d, new_d;
    if (de__manifest_digest_build(s->manifest, s->resolved, R->alloc, &old_d) != DE_OK) {
        DE_ERROR(s->log, "hot reload: out of memory; the manifest %s is not reloaded", root);
        return;
    }
    de_error_clear();
    de_result r = de_asset_manifest_load(s, root);
    if (r != DE_OK) {
        const de_error *e = de_error_last();
        DE_ERROR(s->log, "hot reload: the manifest %s did not load; the previous one stays: %s",
                 root, e != NULL && e->result == r ? e->message : de_result_str(r));
        de__manifest_digest_free(&old_d);
        return;
    }
    R->stats.manifest_reloads++;
    de__manifest_change *ch = NULL;
    u32 n = 0;
    if (de__manifest_digest_build(s->manifest, s->resolved, R->alloc, &new_d) != DE_OK ||
        de__manifest_diff(&old_d, &new_d, R->alloc, &ch, &n) != DE_OK) {
        DE_ERROR(s->log, "hot reload: out of memory comparing the manifests; nothing reloads");
        de__manifest_digest_free(&old_d);
        de__manifest_digest_free(&new_d);
        return;
    }
    u32 added = 0, removed = 0, changed = 0;
    for (u32 i = 0; i < n; ++i) {
        de_asset_id id = ch[i].id;
        u32 h = de__slot_find(s, id);
        de__slot *sl = h != 0 ? de__slot_get(s, h) : NULL;
        if (ch[i].kind == DE__MANIFEST_ADDED) {
            added++; /* typed access and the load API find it in the new manifest */
        } else if (ch[i].kind == DE__MANIFEST_REMOVED) {
            removed++;
            de__rindex_remove(&R->index, id);
            if (sl != NULL && de__slot_object(sl) != NULL)
                DE_WARN(s->log,
                        "hot reload: asset %016llx was removed from the manifest; its loaded data "
                        "stays while it is referenced",
                        (unsigned long long)id.hash);
        } else {
            changed++;
            if (sl != NULL)
                queue_add(R, id); /* start_queued refuses a changed type */
        }
    }
    DE_INFO(s->log, "hot reload: manifest %s loaded again: %u added, %u removed, %u changed", root,
            added, removed, changed);
    if (ch != NULL)
        DE_FREE(R->alloc, ch, sizeof(de__manifest_change) * n);
    de__manifest_digest_free(&old_d);
    de__manifest_digest_free(&new_d);
}

/* ---- Starting reloads -------------------------------------------------------------------------
 */

static void start_queued(de__asset_reload *R) {
    de_asset_system *s = R->s;
    queue_unique(R);
    u32 keep = 0;
    for (u32 i = 0; i < R->queue_count; ++i) {
        de_asset_id id = R->queue[i];
        u32 h = de__slot_find(s, id);
        de__slot *sl = h != 0 ? de__slot_get(s, h) : NULL;
        if (sl == NULL)
            continue;
        if (sl->state == DE_LOAD_PENDING || sl->state == DE_LOAD_LOADING) {
            R->queue[keep++] = id; /* a load in flight may have read the old file: next frame */
            continue;
        }
        if (sl->state != DE_LOAD_READY)
            continue; /* not loaded: the next request reads the new file */
        char hex[24];
        if (de__mount_any(s) && de__mount_lookup(s, id, NULL)) {
            R->stats.skipped_pack++;
            reload_rec *rec = rec_get(R, id);
            if (rec != NULL && !rec->pack_logged) {
                rec->pack_logged = true;
                DE_INFO(s->log,
                        "hot reload: \"%s\" is served by a mounted pack; its source is not "
                        "reloaded",
                        name_of(s, id, hex, sizeof hex));
            }
            continue;
        }
        const de__manifest_asset *a = de__resolved_asset(s->resolved, id);
        if (a != NULL && de_hash64_str(a->type) != sl->type_hash) {
            /* A slot keeps the type it was created with (DENG-140): its typed handles and its
             * object belong to that type, so a new type cannot be swapped in. */
            char msg[DE__LOAD_MSG_MAX];
            snprintf(msg, sizeof msg,
                     "asset \"%s\": its type changed to \"%s\" in the manifest; a loaded asset "
                     "keeps the type it was loaded with until the program restarts",
                     a->id, a->type);
            record_error(R, id, DE_ERR_INVALID_ARGUMENT, msg);
            continue;
        }
        reload_rec *rec = rec_get(R, id);
        if (rec == NULL) {
            DE_ERROR(s->log, "hot reload: out of memory; \"%s\" is not reloaded",
                     name_of(s, id, hex, sizeof hex));
            continue;
        }
        if (rec->job != NULL) {
            /* A newer change: the job in flight read the old file. It is discarded when it comes
             * back (de__asset_reload_job_gone forgets it). */
            de__pipeline_cancel_reload(s, rec->job);
            rec->job = NULL;
            R->stats.superseded++;
        }
        de_error_clear();
        de__pjob *job = de__pipeline_submit_reload(s, h, DE__LOAD_PRIORITY_NORMAL);
        if (job == NULL) {
            const de_error *e = de_error_last();
            record_error(R, id, e != NULL ? e->result : DE_ERR_INTERNAL,
                         e != NULL ? e->message : NULL);
            continue;
        }
        rec = rec_find(R, id); /* record_error is not called on this path; stays valid */
        if (rec != NULL)
            rec->job = job;
        R->stats.started++;
    }
    R->queue_count = keep;
}

/* ---- Lifetime and update ----------------------------------------------------------------------
 */

de_result de__asset_reload_init(de_asset_system *s) {
    if (!s->desc.hot_reload)
        return DE_OK;
    if (s->desc.mode == DE_ASSET_MODE_PACKED) {
        DE_INFO(s->log, "hot reload: PACKED mode loads packs only, which do not change; nothing is "
                        "watched");
        return DE_OK;
    }
    de__asset_reload *R = DE_NEW(s->alloc, de__asset_reload, DE_MEM_ASSET);
    if (R == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "asset hot reload: out of memory");
    memset(R, 0, sizeof *R);
    R->s = s;
    R->alloc = s->alloc;
    R->watching = true;
    R->retire_frames = s->desc.reload_retire_frames != 0 ? s->desc.reload_retire_frames
                                                         : DE__ASSET_RELOAD_RETIRE_DEFAULT;
    de_hashmap_desc hd = {.allocator = s->alloc,
                          .key_size = sizeof(u64),
                          .value_size = sizeof(u32),
                          .tag = DE_MEM_ASSET};
    if (de__rindex_init(&R->index, s->alloc) != DE_OK ||
        de_hashmap_init(&R->rec_index, &hd) != DE_OK) {
        DE_FREE(s->alloc, R, sizeof *R);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "asset hot reload: cannot create its tables");
    }
    s->reload = R;
    return DE_OK;
}

void de__asset_reload_shutdown(de_asset_system *s) {
    de__asset_reload *R = s != NULL ? s->reload : NULL;
    if (R == NULL)
        return;
    watches_clear(R);
    if (R->watches != NULL)
        DE_FREE(R->alloc, R->watches, sizeof(reload_watch) * R->watch_cap);
    for (u32 i = 0; i < R->retired_count; ++i)
        retire_now(s, &R->retired[i]); /* the slots and types still exist */
    if (R->retired != NULL)
        DE_FREE(R->alloc, R->retired, sizeof(retired_obj) * R->retired_cap);
    for (u32 i = 0; i < R->rec_count; ++i)
        rec_clear_error(R, &R->recs[i]);
    if (R->recs != NULL)
        DE_FREE(R->alloc, R->recs, sizeof(reload_rec) * R->rec_cap);
    de_hashmap_free(&R->rec_index);
    if (R->queue != NULL)
        DE_FREE(R->alloc, R->queue, sizeof(de_asset_id) * R->queue_cap);
    de__rindex_free(&R->index);
    s->reload = NULL;
    DE_FREE(R->alloc, R, sizeof *R);
}

bool de__asset_reload_queue(de_asset_system *s, de_asset_id id) {
    de__asset_reload *R = s->reload;
    if (R == NULL || id.hash == 0)
        return false;
    queue_add(R, id);
    return true;
}

void de__asset_reload_update(de_asset_system *s) {
    de__asset_reload *R = s->reload;
    if (R == NULL)
        return;
    retire_tick(R);
    watches_sync(R);
    for (u32 i = 0; i < R->watch_count; ++i)
        (void)de_file_watch_poll(R->watches[i].w);
    if (R->rescan) {
        /* Lost OS events or a directory change: everything that was read may have changed. */
        R->rescan = false;
        R->manifest_dirty = true;
        (void)de__rindex_each_id(&R->index, queue_cb, R);
    }
    if (R->manifest_dirty) {
        R->manifest_dirty = false;
        manifest_reload(R);
        watches_sync(R); /* new includes, another asset root */
    }
    if (R->queue_count != 0)
        start_queued(R);
}

void de__asset_reload_on_change(de_asset_system *s, const char *vpath, de_watch_change change,
                                bool is_dir) {
    de__asset_reload *R = s != NULL ? s->reload : NULL;
    if (R == NULL || vpath == NULL)
        return;
    /* A file: created, modified, renamed to and deleted all mean "read it again". */
    R->stats.changes++;
    if (is_dir) {
        /* MODIFIED: the OS lost events. DELETED, RENAMED: files under it moved. A new directory
         * changes nothing that was read (its files report their own changes). */
        if (change != DE_WATCH_CREATED)
            R->rescan = true;
        return;
    }
    char norm[DE_FS_PATH_MAX];
    if (de_path_normalize(vpath, norm, sizeof norm) != DE_OK)
        return;
    if (is_manifest_file(s->manifest, norm) || is_manifest_file(s->failed_manifest, norm))
        R->manifest_dirty = true;
    (void)de__rindex_lookup(&R->index, norm, queue_cb, R);
}

bool de__asset_reload_manifest_now(de_asset_system *s) {
    de__asset_reload *R = s != NULL ? s->reload : NULL;
    if (R == NULL)
        return false;
    R->manifest_dirty = false;
    manifest_reload(R);
    watches_sync(R);
    return true;
}

/* ---- Pipeline hooks ---------------------------------------------------------------------------
 */

void de__asset_reload_record_paths(de_asset_system *s, de__pjob *job) {
    de__asset_reload *R = s->reload;
    if (R == NULL || job->pack != NULL)
        return; /* packs do not change at run time */
    if (de__rindex_set(&R->index, job->id, job->vpath, job->extra_paths, job->extra_count) != DE_OK)
        DE_WARN(s->log, "hot reload: out of memory; changes to \"%s\" are not seen", job->asset);
}

typedef struct dep_walk {
    de_asset_id dep;
    u32 self;
    u64 calls;
} dep_walk;

static void dependents_fn(de_asset_system *s, u32 handle_id, void *user) {
    dep_walk *w = (dep_walk *)user;
    if (handle_id == w->self)
        return;
    de__slot *sl = de__slot_get(s, handle_id);
    if (sl == NULL || sl->state != DE_LOAD_READY || sl->dep_count == 0)
        return;
    void *object = de__slot_object(sl);
    if (object == NULL)
        return;
    for (u32 i = 0; i < sl->dep_count; ++i) {
        if (sl->deps[i].hash != w->dep.hash)
            continue;
        const de_asset_type_desc *td = de__asset_type_get(s, de__slot_type(s, sl));
        if (td != NULL && td->dependency_reloaded != NULL) {
            td->dependency_reloaded(object, w->dep, td->user);
            w->calls++;
        }
        return;
    }
}

static void reload_details(de__pjob *job, de_asset_event *ev, const char *message);

void de__asset_reload_swap(de_asset_system *s, de__pjob *job, de_asset_id *deps) {
    de__asset_reload *R = s->reload;
    const u32 dep_count = job->dep_slot_count;
    de__slot *sl = de__slot_get(s, job->slot);
    if (R == NULL || sl == NULL || sl->state != DE_LOAD_READY) {
        /* Evicted (or freed) while the reload ran: the next load reads the new file anyway.
         * job_free drops the job's dependency refs. */
        if (job->object != NULL && job->type->unload != NULL)
            job->type->unload(job->object, job->type->user);
        job->object = NULL;
        if (deps != NULL)
            DE_FREE(s->alloc, deps, sizeof(de_asset_id) * dep_count);
        if (R != NULL)
            R->stats.discarded++;
        return;
    }
    const de_asset_id id = sl->id;
    const de_asset_type_desc *old_type = de__asset_type_get(s, de__slot_type(s, sl));
    de__slot_swapped old;
    de__slot_swap(s, job->slot, job->object, job->object_bytes, deps, dep_count, &old);
    job->object = NULL;
    job->dep_slot_count = 0; /* the job's asset refs on its dependencies now belong to the slot */
    de__budget_on_publish(s, job->slot, job->object_bytes);
    retire_push(R, id, old_type, &old);

    reload_rec *rec = rec_find(R, id);
    if (rec != NULL)
        rec_clear_error(R, rec);
    de__asset_reload_record_paths(s, job);
    R->stats.swapped++;
    DE_INFO(s->log, "hot reload: \"%s\" reloaded (version %u)", job->asset, sl->version);

    de_asset_event ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = DE_ASSET_EV_RELOADED;
    reload_details(job, &ev, NULL);
    ev.id = id;
    ev.state = DE_LOAD_READY;
    ev.error = DE_OK;
    de__asset_emit(s, &ev);

    /* DENG-289: the type's own reload callback, with the new object in place. */
    const de_asset_type_desc *new_type = de__asset_type_get(s, de__slot_type(s, sl));
    if (new_type != NULL && new_type->reloaded != NULL) {
        void *object = de__slot_object(sl);
        if (object != NULL)
            new_type->reloaded(object, id, new_type->user);
    }

    dep_walk w = {id, job->slot, 0};
    de__slot_for_each_live(s, dependents_fn, &w);
    R->stats.dependents += w.calls;
}

static void reload_details(de__pjob *job, de_asset_event *ev, const char *message) {
    ev->has_cook_ms = job->has_cook_ms;
    ev->cook_ms = job->cook_ms;
    char diagnostic[DE_ASSET_EVENT_SOURCE_CAP];
    const bool located =
        de__asset_diagnostic_location(message, diagnostic, sizeof diagnostic, &ev->line);
    const char *source = located ? diagnostic : job->vpath;
    if (source == NULL)
        return;
    if (strstr(source, ":/") != NULL && !(source[1] == ':' && source[2] == '/')) {
        if (de_fs_resolve(source, ev->source, sizeof ev->source) == DE_OK)
            return;
        de_error_clear();
    }
    const size_t n = strlen(source);
    if (n < sizeof ev->source)
        memcpy(ev->source, source, n + 1);
}

void de__asset_reload_failed(de_asset_system *s, de__pjob *job, de_result code, const char *msg) {
    de__asset_reload *R = s->reload;
    if (R == NULL)
        return;
    record_error(R, job->id, code, msg);
    /* A failed recook leaves the previous object usable, but editors still need the failure. */
    de_asset_event event = {0};
    event.kind = DE_ASSET_EV_FAILED;
    event.id = job->id;
    event.state = DE_LOAD_READY;
    event.error = code;
    reload_details(job, &event, msg);
    de__asset_emit(s, &event);
}

void de__asset_reload_job_gone(de_asset_system *s, de__pjob *job) {
    de__asset_reload *R = s->reload;
    reload_rec *rec = R != NULL ? rec_find(R, job->id) : NULL;
    if (rec != NULL && rec->job == job)
        rec->job = NULL;
}

/* ---- Tests ------------------------------------------------------------------------------------
 */

void de__asset_reload_set_watching(de_asset_system *s, bool watching) {
    de__asset_reload *R = s != NULL ? s->reload : NULL;
    if (R == NULL)
        return;
    R->watching = watching;
    if (!watching)
        watches_clear(R);
}

void de__asset_reload_get_stats(const de_asset_system *s, de__asset_reload_stats *out) {
    memset(out, 0, sizeof *out);
    const de__asset_reload *R = s != NULL ? s->reload : NULL;
    if (R != NULL)
        *out = R->stats;
}

#endif /* DE__ASSET_RELOAD_ENABLED */
