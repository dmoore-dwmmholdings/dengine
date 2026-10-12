/*
 * pipeline.c: the asset load pipeline as the load API's backend (SPEC §11.4; DENG-142): jobs,
 * the dispatch queue, the main-thread update hook that publishes finished jobs, dependencies and
 * runtime cycles, cancellation, shutdown. See pipeline_internal.h for the stages and the rules;
 * io_stage.c, decompress_stage.c and load_ctx.c hold the stages.
 */

#include "asset/pipeline_internal.h"

#include "asset/asset_internal.h"
#include "asset/cook_stage_internal.h" /* DENG-209 */
#include "asset/backend_internal.h"
#include "asset/load_internal.h"
#include "asset/manifest_internal.h"
#include "asset/mount_internal.h" /* DENG-212 */
#include "asset/resolve_internal.h"
#include "asset/slots_internal.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_task.h>

#include "core/de_compiler.h"
#include "core/de_hashmap.h"
#include "platform/de_atomic.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---- Small helpers ----------------------------------------------------------------------------
 */

static void stat_add(de_atomic_u64 *c) {
    (void)de_atomic_fetch_add_u64(c, 1, DE_ATOMIC_RELAXED);
}

static u64 slot_key_hash(u32 slot) {
    u64 x = (u64)slot * 0x9E3779B97F4A7C15ull;
    return x ^ (x >> 29);
}

static size_t align_up(size_t v, size_t a) {
    return (v + (a - 1u)) & ~(a - 1u);
}

/* DE_LOAD_* flag of a priority (the internal dependency requests use it). */
static u32 priority_flag(u8 priority) {
    switch (priority) {
    case DE__LOAD_PRIORITY_CRITICAL:
        return DE_LOAD_PRIORITY_CRITICAL;
    case DE__LOAD_PRIORITY_HIGH:
        return DE_LOAD_PRIORITY_HIGH;
    case DE__LOAD_PRIORITY_LOW:
        return DE_LOAD_PRIORITY_LOW;
    case DE__LOAD_PRIORITY_BACKGROUND:
        return DE_LOAD_PRIORITY_BACKGROUND;
    default:
        return 0;
    }
}

/* The asset path of `id` for messages, or its hash. */
static const char *asset_name(de_asset_system *s, de_asset_id id, char *buf, size_t size) {
    const char *p = de__resolved_path(s->resolved, id);
    if (p != NULL)
        return p;
    snprintf(buf, size, "%016llx", (unsigned long long)id.hash);
    return buf;
}

/* ---- Job table (main thread) ------------------------------------------------------------------
 */

static de__pjob *job_of(de__pipeline *pl, u32 slot) {
    if (!pl->started || slot == 0)
        return NULL;
    de__pjob **p = DE_HASHMAP_GET(&pl->jobs, de__pjob *, slot_key_hash(slot), &slot);
    return p != NULL ? *p : NULL;
}

static bool job_table_put(de__pipeline *pl, de__pjob *job) {
    de__pjob **value = NULL;
    bool inserted = false;
    if (de_hashmap_get_or_put(&pl->jobs, slot_key_hash(job->slot), &job->slot, (void **)&value,
                              &inserted) != DE_OK)
        return false;
    /* The seam never submits a slot twice without a completion or a cancel in between, and
     * both take the slot's job out of the table. */
    DE_ASSERT(inserted);
    *value = job;
    job->in_table = true;
    return true;
}

static void job_table_remove(de__pipeline *pl, de__pjob *job) {
    if (!job->in_table)
        return;
    (void)de_hashmap_remove(&pl->jobs, slot_key_hash(job->slot), &job->slot, NULL);
    job->in_table = false;
}

/* ---- Waiting list (main thread) ---------------------------------------------------------------
 */

static void wait_link(de__pipeline *pl, de__pjob *job) {
    job->wait_prev = pl->wait_tail;
    job->wait_next = NULL;
    if (pl->wait_tail != NULL)
        pl->wait_tail->wait_next = job;
    else
        pl->wait_head = job;
    pl->wait_tail = job;
    job->waiting = true;
}

static void wait_unlink(de__pipeline *pl, de__pjob *job) {
    if (!job->waiting)
        return;
    if (job->wait_prev != NULL)
        job->wait_prev->wait_next = job->wait_next;
    else
        pl->wait_head = job->wait_next;
    if (job->wait_next != NULL)
        job->wait_next->wait_prev = job->wait_prev;
    else
        pl->wait_tail = job->wait_prev;
    job->wait_prev = job->wait_next = NULL;
    job->waiting = false;
}

/* ---- Dispatch queue (under `lock`) ------------------------------------------------------------
 */

static void queue_push(de__pipeline *pl, de__pjob *job) {
    u32 p = job->priority;
    job->q_next = NULL;
    job->q_prev = pl->q_tail[p];
    if (pl->q_tail[p] != NULL)
        pl->q_tail[p]->q_next = job;
    else
        pl->q_head[p] = job;
    pl->q_tail[p] = job;
    pl->queued++;
}

static void queue_remove(de__pipeline *pl, de__pjob *job) {
    u32 p = job->priority;
    if (job->q_prev != NULL)
        job->q_prev->q_next = job->q_next;
    else
        pl->q_head[p] = job->q_next;
    if (job->q_next != NULL)
        job->q_next->q_prev = job->q_prev;
    else
        pl->q_tail[p] = job->q_prev;
    job->q_prev = job->q_next = NULL;
    pl->queued--;
}

/* The most urgent queued job, taken off the queue; NULL when empty. */
static de__pjob *queue_pop(de__pipeline *pl) {
    for (u32 p = 0; p < DE__LOAD_PRIORITY_COUNT; ++p) {
        de__pjob *job = pl->q_head[p];
        if (job != NULL) {
            queue_remove(pl, job);
            return job;
        }
    }
    return NULL;
}

/* ---- Between the stages (any thread) ----------------------------------------------------------
 */

void de__pipeline_run(de__pipeline *pl, de_task_pool pool, de_task_fn fn, de__pjob *job) {
    if (pl->tasks != NULL)
        de_task_submit(pl->tasks, pool, fn, job, &pl->task_counter);
    else
        fn(job);
}

void de__pipeline_finish(de__pjob *job) {
    de__pipeline *pl = job->pl;
    de__pjob_set_stage(job, DE__PJOB_FINISHED);
    void *head = de_atomic_load_ptr(&pl->done_head, DE_ATOMIC_RELAXED);
    do {
        job->done_next = (de__pjob *)head;
    } while (
        !de_atomic_cas_weak_ptr(&pl->done_head, &head, job, DE_ATOMIC_RELEASE, DE_ATOMIC_RELAXED));
}

void de__pipeline_job_fail(de__pjob *job, de_result code, const char *fmt, ...) {
    if (job->msg_set)
        return;
    job->result = code != DE_OK ? code : DE_ERR_INTERNAL;
    int n = snprintf(job->msg, sizeof job->msg, "asset \"%s\": ", job->asset);
    if (n < 0 || (size_t)n >= sizeof job->msg)
        n = 0;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(job->msg + n, sizeof job->msg - (size_t)n, fmt, ap);
    va_end(ap);
    job->msg_set = true;
}

void de__pipeline_progress(de__pjob *job, u64 bytes_done, u64 bytes_total) {
    de__pipeline *pl = job->pl;
    /* Under the lock: once cancel (which sets `cancelled` under it) returns, nothing more is
     * reported for this submission, even if the slot is submitted again at once. */
    de_mutex_lock(&pl->lock);
    if (!de__pjob_cancelled(job))
        de__slot_progress(job->s, job->slot, bytes_done, bytes_total);
    de_mutex_unlock(&pl->lock);
}

/* Inline stages (no task system) can call dispatch from inside dispatch; the outer call on the
 * thread keeps popping, so the inner one returns at once (no recursion per job). */
static DE_THREAD_LOCAL u32 tl_dispatching;

void de__pipeline_dispatch(de__pipeline *pl) {
    if (tl_dispatching != 0)
        return;
    tl_dispatching = 1;
    for (;;) {
        de__pjob *job = NULL;
        de_mutex_lock(&pl->lock);
        if (pl->io_active < pl->io_limit) {
            job = queue_pop(pl);
            if (job != NULL) {
                pl->io_active++;
                job->io.priority = job->priority;
                de__pjob_set_stage(job, DE__PJOB_OPEN);
            }
        }
        de_mutex_unlock(&pl->lock);
        if (job == NULL)
            break;
        /* A frame worker, not a background worker (DENG-386). Background workers run below
         * normal priority, and Windows schedules strictly by priority: under a busy main thread
         * and frame workers (an uncapped loop, other processes) a background worker ran no open
         * for seconds, and a job it held stayed in the OPEN/READ window while the rest of the
         * queue passed it. The open is short (a file open and a size query; a pack lookup in
         * S08), and at most io_limit of them run at once. */
        de__pipeline_run(pl, DE_TASK_POOL_FRAME, de__io_stage_open, job);
    }
    tl_dispatching = 0;
}

void de__pipeline_io_done(de__pipeline *pl) {
    de_mutex_lock(&pl->lock);
    DE_ASSERT(pl->io_active != 0);
    pl->io_active--;
    de_mutex_unlock(&pl->lock);
    de__pipeline_dispatch(pl);
}

de_result de__pipeline_upload_stage(de_asset_system *s, de_asset_load_ctx *ctx) {
    /* S11: GPU-resident types hand their data to the staging ring and the copy queue here; the
     * job then waits for the upload before it goes back to the main thread. */
    (void)s;
    (void)ctx;
    return DE_OK;
}

/* ---- Jobs (main thread) -----------------------------------------------------------------------
 */

/* Bytes the settings copy needs: the kv array, the string-array item pointers, the characters. */
static void settings_size(const de__manifest_asset *a, size_t *ptrs, size_t *chars) {
    *ptrs = 0;
    *chars = 0;
    for (u32 i = 0; i < a->setting_count; ++i) {
        const de__manifest_kv *kv = &a->settings[i];
        *chars += strlen(kv->key) + 1u;
        if (kv->value.type == DE__MANIFEST_STRING) {
            *chars += strlen(kv->value.u.s) + 1u;
        } else if (kv->value.type == DE__MANIFEST_STRING_ARRAY) {
            *ptrs += kv->value.u.arr.count;
            for (u32 k = 0; k < kv->value.u.arr.count; ++k)
                *chars += strlen(kv->value.u.arr.items[k]) + 1u;
        }
    }
}

static char *copy_str(char **at, const char *src) {
    size_t n = strlen(src) + 1u;
    char *dst = *at;
    memcpy(dst, src, n);
    *at += n;
    return dst;
}

/* A new job for `slot`: one block with the asset path, the source path and the settings. */
static de__pjob *job_new(de__pipeline *pl, u32 slot, const de__slot *sl,
                         const de__manifest_asset *a, const de_asset_type_desc *type,
                         const char *vpath, de__load_priority priority) {
    size_t ptrs = 0, chars = 0;
    settings_size(a, &ptrs, &chars);
    size_t off_kv = align_up(sizeof(de__pjob), DE_ALIGNOF(de__manifest_kv));
    size_t off_ptrs =
        align_up(off_kv + sizeof(de__manifest_kv) * a->setting_count, DE_ALIGNOF(const char *));
    size_t off_chars = off_ptrs + sizeof(const char *) * ptrs;
    size_t size = off_chars + chars + strlen(a->id) + 1u + strlen(vpath) + 1u;
    u8 *block = (u8 *)DE_ALLOC(pl->alloc, size, DE_ALIGNOF(de__pjob), DE_MEM_ASSET);
    if (block == NULL)
        return NULL;
    memset(block, 0, sizeof(de__pjob));
    de__pjob *job = (de__pjob *)block;
    de_atomic_init_u32(&job->stage, DE__PJOB_QUEUED);
    de_atomic_init_u32(&job->cancelled, 0);
    job->pl = pl;
    job->s = pl->s;
    job->slot = slot;
    job->id = sl->id;
    job->type = type;
    job->priority = (u8)priority;
    job->block_size = size;
    job->result = DE_OK;

    de__manifest_kv *kvs = (de__manifest_kv *)(block + off_kv);
    const char **items = (const char **)(block + off_ptrs);
    char *at = (char *)(block + off_chars);
    for (u32 i = 0; i < a->setting_count; ++i) {
        const de__manifest_kv *src = &a->settings[i];
        de__manifest_kv *dst = &kvs[i];
        *dst = *src;
        dst->key = copy_str(&at, src->key);
        if (src->value.type == DE__MANIFEST_STRING) {
            dst->value.u.s = copy_str(&at, src->value.u.s);
        } else if (src->value.type == DE__MANIFEST_STRING_ARRAY) {
            for (u32 k = 0; k < src->value.u.arr.count; ++k)
                items[k] = copy_str(&at, src->value.u.arr.items[k]);
            dst->value.u.arr.items = items;
            items += src->value.u.arr.count;
        }
    }
    job->settings = a->setting_count != 0 ? kvs : NULL;
    job->setting_count = a->setting_count;
    job->asset = copy_str(&at, a->id);
    job->vpath = copy_str(&at, vpath);

    job->all_prev = NULL;
    job->all_next = pl->all_head;
    if (pl->all_head != NULL)
        pl->all_head->all_prev = job;
    pl->all_head = job;
    pl->live_jobs++;
    stat_add(&pl->stats.submits);
    return job;
}

/* Gives a job's partial object to its type's `unload`. */
static void job_unload(de__pipeline *pl, de__pjob *job) {
    if (job->object == NULL)
        return;
    if (job->type->unload != NULL)
        job->type->unload(job->object, job->type->user);
    job->object = NULL;
    stat_add(&pl->stats.unloads);
}

/* Frees a job and everything it holds (asset refs, its dependency request). The object must be
 * gone already (unloaded or handed to the slot). */
static void job_free(de__pipeline *pl, de__pjob *job) {
    de_asset_system *s = pl->s;
    DE_ASSERT(job->object == NULL);
    job_table_remove(pl, job);
    wait_unlink(pl, job);
    if (job->all_prev != NULL)
        job->all_prev->all_next = job->all_next;
    else
        pl->all_head = job->all_next;
    if (job->all_next != NULL)
        job->all_next->all_prev = job->all_prev;
    for (u32 i = 0; i < job->dep_slot_count; ++i)
        (void)de__slot_unref_asset(s, job->dep_slots[i]);
    if (job->dep_slots != NULL)
        DE_FREE(pl->alloc, job->dep_slots, sizeof(u32) * job->dep_count);
    if (DE_HANDLE_VALID(job->dep_req)) {
        /* May cancel dependencies nobody else waits on: cancel only flags their jobs. */
        de_load_handle h = job->dep_req;
        job->dep_req = (de_load_handle){0};
        de_asset_release(s, h);
    }
    if (job->deps != NULL)
        DE_FREE(pl->alloc, job->deps, sizeof(de_asset_id) * job->dep_capacity);
    if (job->span_open)
        pl->source.close(pl->source.user, &job->span);
    de__pack_stage_free(job); /* DENG-212: the run table and the pack reference */
    if (job->buf != NULL)
        DE_FREE(pl->alloc, job->buf, (size_t)job->buf_size);
    if (job->raw != NULL)
        DE_FREE(pl->alloc, job->raw, (size_t)(job->raw_size != 0 ? job->raw_size : 1u));
    DE_ASSERT(pl->live_jobs != 0);
    pl->live_jobs--;
    DE_FREE(pl->alloc, job, job->block_size);
}

/* A cancelled job is back on the main thread (or never left it): drop it and its object. */
static void job_discard(de__pipeline *pl, de__pjob *job) {
    job_unload(pl, job);
    stat_add(&pl->stats.discarded);
    job_free(pl, job);
}

/* Fails the job's slot now and frees the job. */
static void job_fail_now(de__pipeline *pl, de__pjob *job, de_result code, const char *msg) {
    de_asset_system *s = pl->s;
    u32 slot = job->slot;
    char text[DE__LOAD_MSG_MAX];
    snprintf(text, sizeof text, "%s", msg != NULL ? msg : "");
    job_unload(pl, job);
    job_free(pl, job);
    stat_add(&pl->stats.failed);
    de__slot_complete_now(s, slot, code != DE_OK ? code : DE_ERR_INTERNAL, 0,
                          text[0] != '\0' ? text : NULL);
}

/* Publishes a loaded job whose dependencies (if any) are READY: object, dependency list and the
 * READY state, then frees the job. */
static void job_publish(de__pipeline *pl, de__pjob *job) {
    de_asset_system *s = pl->s;
    de__slot *sl = de__slot_get(s, job->slot);
    if (sl == NULL) { /* cannot happen while a request waits on the slot */
        job_discard(pl, job);
        return;
    }
    de_asset_id *deps = NULL;
    if (job->dep_slot_count != 0) {
        deps = (de_asset_id *)DE_ALLOC(s->alloc, sizeof(de_asset_id) * job->dep_slot_count,
                                       DE_ALIGNOF(de_asset_id), DE_MEM_ASSET);
        if (deps == NULL) {
            char msg[DE__LOAD_MSG_MAX];
            snprintf(msg, sizeof msg, "asset \"%s\": out of memory publishing it", job->asset);
            job_fail_now(pl, job, DE_ERR_OUT_OF_MEMORY, msg);
            return;
        }
        for (u32 i = 0; i < job->dep_slot_count; ++i) {
            const de__slot *d = de__slot_get(s, job->dep_slots[i]);
            deps[i] = d != NULL ? d->id : (de_asset_id){0};
        }
    }

    /* Published again (a reload, or a retry after a cancel): the previous object goes to its
     * type's unload, and the previous dependency refs are dropped. */
    void *old = de__slot_object(sl);
    if (old != NULL && old != job->object) {
        const de_asset_type_desc *td = de__asset_type_get(s, de__slot_type(s, sl));
        de__slot_set_object(sl, NULL);
        if (td != NULL && td->unload != NULL)
            td->unload(old, td->user);
    }
    for (u32 i = 0; i < sl->dep_count; ++i) {
        u32 h = de__slot_find(s, sl->deps[i]);
        if (h != 0)
            (void)de__slot_unref_asset(s, h);
    }
    if (sl->deps != NULL)
        DE_FREE(s->alloc, sl->deps, sizeof(de_asset_id) * sl->dep_count);

    /* The object and the dependency refs first, then READY (backend_internal.h). The job's
     * asset refs now belong to the slot. */
    sl->deps = deps;
    sl->dep_count = job->dep_slot_count;
    job->dep_slot_count = 0;
    de__slot_set_object(sl, job->object);
    job->object = NULL;
    u32 slot = job->slot;
    u64 bytes = job->object_bytes;
    job_free(pl, job);
    stat_add(&pl->stats.published);
    de__slot_complete_now(s, slot, DE_OK, bytes, NULL);
}

/* ---- Dependencies (main thread) ---------------------------------------------------------------
 */

/* Looks for a cycle through `start`: dependency edges of the jobs that wait on their own
 * dependencies. On a hit writes "a -> b -> a" into `out` and returns true. */
static bool find_cycle(de__pipeline *pl, de__pjob *start, char *out, size_t size) {
    de_asset_system *s = pl->s;
    if (++pl->epoch == 0)
        pl->epoch = 1;
    u32 top = 0;
    if (pl->dfs_capacity == 0) {
        pl->dfs = (de__pjob **)DE_ALLOC(pl->alloc, sizeof(de__pjob *) * 64, DE_ALIGNOF(de__pjob *),
                                        DE_MEM_ASSET);
        if (pl->dfs == NULL)
            return false;
        pl->dfs_capacity = 64;
    }
    start->visit_epoch = pl->epoch;
    start->visit_parent = NULL;
    pl->dfs[top++] = start;
    while (top != 0) {
        de__pjob *cur = pl->dfs[--top];
        const de__request *r = de__request_get(s, cur->dep_req);
        for (u32 i = 0; r != NULL && i < r->count; ++i) {
            u32 slot = r->entries[i].slot;
            if (slot == 0)
                continue;
            if (slot == start->slot) {
                /* start -> ... -> cur -> start: walk the parents back from cur. */
                de__pjob *chain[64];
                u32 n = 0;
                for (de__pjob *j = cur; j != NULL && n < 64; j = j->visit_parent)
                    chain[n++] = j;
                size_t len = 0;
                out[0] = '\0';
                for (u32 k = n; k-- > 0 && len + 1 < size;) {
                    int w = snprintf(out + len, size - len, "%s -> ", chain[k]->asset);
                    len = w < 0 ? size : len + (size_t)w;
                }
                if (len + 1 < size)
                    snprintf(out + len, size - len, "%s", start->asset);
                return true;
            }
            de__pjob *j = job_of(pl, slot);
            if (j == NULL || !j->waiting || de__pjob_cancelled(j) || j->visit_epoch == pl->epoch)
                continue;
            j->visit_epoch = pl->epoch;
            j->visit_parent = cur;
            if (top == pl->dfs_capacity) {
                u32 cap = pl->dfs_capacity * 2u;
                void *p =
                    DE_REALLOC(pl->alloc, pl->dfs, sizeof(de__pjob *) * pl->dfs_capacity,
                               sizeof(de__pjob *) * cap, DE_ALIGNOF(de__pjob *), DE_MEM_ASSET);
                if (p == NULL)
                    return false; /* no memory to look further: no cycle reported */
                pl->dfs = (de__pjob **)p;
                pl->dfs_capacity = cap;
            }
            pl->dfs[top++] = j;
        }
    }
    return false;
}

/* A loaded job declared dependencies: request them (no events, the job's priority), hold an
 * asset ref on each, and wait. */
static void job_start_deps(de__pipeline *pl, de__pjob *job) {
    de_asset_system *s = pl->s;
    char msg[DE__LOAD_MSG_MAX];
    de_load_handle h = de_asset_load_many(s, job->deps, job->dep_count,
                                          DE_LOAD_NO_EVENTS | priority_flag(job->priority));
    const de__request *r = de__request_get(s, h);
    if (r != NULL) {
        job->dep_req = h;
        job->dep_slots =
            (u32 *)DE_ALLOC(pl->alloc, sizeof(u32) * job->dep_count, DE_ALIGNOF(u32), DE_MEM_ASSET);
    }
    if (r == NULL || job->dep_slots == NULL) {
        snprintf(msg, sizeof msg, "asset \"%s\": out of memory requesting its dependencies",
                 job->asset);
        job_fail_now(pl, job, DE_ERR_OUT_OF_MEMORY, msg);
        return;
    }
    for (u32 i = 0; i < r->count; ++i)
        if (r->entries[i].slot != 0 && de__slot_ref_asset(s, r->entries[i].slot))
            job->dep_slots[job->dep_slot_count++] = r->entries[i].slot;
    de__pjob_set_stage(job, DE__PJOB_WAIT_DEPS);
    wait_link(pl, job);

    char path[DE__LOAD_MSG_MAX - 64];
    if (find_cycle(pl, job, path, sizeof path)) {
        snprintf(msg, sizeof msg, "asset \"%s\": dependency cycle: %s", job->asset, path);
        job_fail_now(pl, job, DE_ERR_PARSE, msg);
    }
}

typedef enum deps_state { DEPS_PENDING, DEPS_READY, DEPS_FAILED } deps_state;

/* Where the dependencies of a waiting job are. On DEPS_FAILED, `*failed` is the first failed
 * entry in request order (NULL: the request itself is gone). */
static deps_state job_deps_state(de__pipeline *pl, const de__pjob *job,
                                 const de__load_entry **failed) {
    de_asset_system *s = pl->s;
    const de__request *r = de__request_get(s, job->dep_req);
    *failed = NULL;
    if (r == NULL)
        return DEPS_FAILED;
    bool pending = false;
    for (u32 i = 0; i < r->count; ++i) {
        const de__load_entry *e = &r->entries[i];
        const de__slot *sl = e->slot != 0 ? de__slot_get(s, e->slot) : NULL;
        if (sl == NULL || sl->state == DE_LOAD_FAILED) {
            *failed = e;
            return DEPS_FAILED;
        }
        if (sl->state != DE_LOAD_READY)
            pending = true;
    }
    return pending ? DEPS_PENDING : DEPS_READY;
}

/* Fails a waiting job because of its dependency `e`, with the cause chain in the message. */
static void job_fail_dep(de__pipeline *pl, de__pjob *job, const de__load_entry *e) {
    de_asset_system *s = pl->s;
    char msg[DE__LOAD_MSG_MAX];
    if (e == NULL) {
        snprintf(msg, sizeof msg, "asset \"%s\": its dependency request was lost", job->asset);
        job_fail_now(pl, job, DE_ERR_INTERNAL, msg);
        return;
    }
    char hex[24];
    const char *name = asset_name(s, e->id, hex, sizeof hex);
    de_result code;
    const char *cause;
    char cause_buf[64];
    if (e->slot == 0) {
        code = e->error != DE_OK ? e->error : DE_ERR_NOT_FOUND;
        cause = code == DE_ERR_NOT_FOUND ? "it is not in the asset manifest" : de_result_str(code);
    } else {
        const de__slot *sl = de__slot_get(s, e->slot);
        const de__load_slot *ls = de__load_slot_find(s, e->slot);
        code = sl != NULL && sl->error != DE_OK ? sl->error : DE_ERR_INTERNAL;
        if (ls != NULL && ls->error_msg != NULL) {
            cause = ls->error_msg;
        } else {
            snprintf(cause_buf, sizeof cause_buf, "%s", de_result_str(code));
            cause = cause_buf;
        }
    }
    snprintf(msg, sizeof msg, "asset \"%s\": dependency \"%s\" failed: %s", job->asset, name,
             cause);
    job_fail_now(pl, job, code, msg);
}

/* Publishes or fails waiting jobs until nothing changes, so a parent whose last dependency was
 * published in this update is published in this update too (a chain resolves in one call). */
static void resolve_waiting(de__pipeline *pl) {
    bool changed;
    do {
        changed = false;
        for (de__pjob *job = pl->wait_head, *next = NULL; job != NULL; job = next) {
            /* Publishing or failing `job` only flags other jobs (cancel), it never frees one
             * that is on this list, so `next` stays valid. */
            next = job->wait_next;
            if (de__pjob_cancelled(job)) {
                job_discard(pl, job);
                changed = true;
                continue;
            }
            const de__load_entry *failed = NULL;
            deps_state st = job_deps_state(pl, job, &failed);
            if (st == DEPS_READY) {
                job_publish(pl, job);
                changed = true;
            } else if (st == DEPS_FAILED) {
                job_fail_dep(pl, job, failed);
                changed = true;
            }
        }
    } while (changed);
}

/* A job is back on the main thread. */
static void job_finished(de__pipeline *pl, de__pjob *job) {
    if (de__pjob_cancelled(job)) {
        job_discard(pl, job);
        return;
    }
    if (job->result != DE_OK) {
        if (!job->msg_set)
            de__pipeline_job_fail(job, job->result, "%s", de_result_str(job->result));
        job_fail_now(pl, job, job->result, job->msg);
        return;
    }
    if (job->dep_count == 0) {
        job_publish(pl, job);
        return;
    }
    job_start_deps(pl, job);
}

/* ---- Backend (main thread) --------------------------------------------------------------------
 */

/* Starts what the first load needs: the job table and the I/O threads. */
static de_result pipeline_start(de__pipeline *pl) {
    if (pl->started)
        return DE_OK;
    de_hashmap_desc hd = {.allocator = pl->alloc,
                          .key_size = sizeof(u32),
                          .value_size = sizeof(de__pjob *),
                          .tag = DE_MEM_ASSET};
    if (de_hashmap_init(&pl->jobs, &hd) != DE_OK)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "asset pipeline: out of memory");
    if (pl->tasks != NULL) {
        u32 n = de_task_worker_count(pl->tasks);
        pl->dctx =
            (void **)DE_ALLOC(pl->alloc, sizeof(void *) * n, DE_ALIGNOF(void *), DE_MEM_ASSET);
        if (pl->dctx == NULL) {
            de_hashmap_free(&pl->jobs);
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "asset pipeline: out of memory");
        }
        memset(pl->dctx, 0, sizeof(void *) * n);
        pl->dctx_count = n;
    }
    de_result r = de__io_stage_start(pl);
    if (r != DE_OK) {
        if (pl->dctx != NULL)
            DE_FREE(pl->alloc, pl->dctx, sizeof(void *) * pl->dctx_count);
        pl->dctx = NULL;
        pl->dctx_count = 0;
        de_hashmap_free(&pl->jobs);
        return r;
    }
    pl->started = true;
    return DE_OK;
}

/* Answers a submission at once with a failure (queued; applied by the next update). */
static void fail_early(de_asset_system *s, u32 slot, de_result code, const char *fmt, ...)
    DE_PRINTF_FMT(4, 5);
static void fail_early(de_asset_system *s, u32 slot, de_result code, const char *fmt, ...) {
    char msg[DE__LOAD_MSG_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    (void)DE_FAIL(code, "%s", msg);
    de__slot_complete(s, slot, code, 0);
}

/* The source path of `a`: `asset_root` next to the root manifest, then the (locale) source. */
static bool source_path(const de_asset_system *s, const de__manifest_asset *a, char *out,
                        size_t size) {
    const de__manifest *m = s->manifest;
    const char *src = de__manifest_source_for(m, a, de__asset_locale(s));
    const char *slash = strrchr(m->root, '/');
    int dir = slash != NULL ? (int)(slash - m->root) : 0;
    char joined[DE_FS_PATH_MAX];
    int n = m->asset_root[0] != '\0'
                ? snprintf(joined, sizeof joined, "%.*s/%s/%s", dir, m->root, m->asset_root, src)
                : snprintf(joined, sizeof joined, "%.*s/%s", dir, m->root, src);
    if (n < 0 || (size_t)n >= sizeof joined)
        return false;
    return de_path_normalize(joined, out, size) == DE_OK;
}

/* DENG-209: an asset whose type has an importer, or a sub-asset of a cooked asset, is cooked
 * in-process (cook_stage.c), then loaded; without the cooker it fails here. False: the asset loads
 * from its source as it is. */
static bool submit_cook(de__pipeline *pl, de_asset_system *s, u32 slot,
                        de__load_priority priority) {
    de__cook_plan plan;
    if (!de__cook_stage_plan(s, slot, &plan))
        return false;
    if (plan.error != DE_OK) {
        fail_early(s, slot, plan.error, "%s", plan.msg);
        return true;
    }
    de__pjob *job =
        job_new(pl, slot, de__slot_get(s, slot), &plan.asset, plan.load_type, plan.vpath, priority);
    if (job != NULL && !job_table_put(pl, job)) {
        job_free(pl, job);
        job = NULL;
    }
    if (job == NULL) {
        fail_early(s, slot, DE_ERR_OUT_OF_MEMORY, "asset \"%s\": out of memory", plan.asset.id);
        return true;
    }
    de__cook_stage_begin(job, &plan);
    return true;
}

/* DENG-212: an asset a mounted pack serves (every asset in PACKED mode; in SOURCE mode the IDs a
 * pack overrides) is read from the pack (pack_stage.c). False: it is not. */
static bool submit_pack(de__pipeline *pl, de_asset_system *s, u32 slot,
                        de__load_priority priority) {
    de__pack_plan plan;
    if (!de__pack_stage_plan(s, slot, &plan))
        return false;
    if (plan.error != DE_OK) {
        fail_early(s, slot, plan.error, "%s", plan.msg);
        return true;
    }
    de__pjob *job =
        job_new(pl, slot, de__slot_get(s, slot), &plan.asset, plan.type, plan.vpath, priority);
    if (job != NULL && (!job_table_put(pl, job) || de__pack_stage_attach(job, &plan) != DE_OK)) {
        job_free(pl, job);
        job = NULL;
    }
    if (job == NULL) {
        fail_early(s, slot, DE_ERR_OUT_OF_MEMORY, "asset \"%s\": out of memory", plan.asset.id);
        return true;
    }
    de_mutex_lock(&pl->lock);
    queue_push(pl, job);
    de_mutex_unlock(&pl->lock);
    de__pipeline_dispatch(pl);
    return true;
}

static void pl_submit(void *user, de_asset_system *s, u32 slot, de__load_priority priority) {
    de__pipeline *pl = (de__pipeline *)user;
    de_result r = pipeline_start(pl);
    if (r != DE_OK) {
        fail_early(s, slot, r, "the asset load pipeline could not start: %s",
                   de_error_last() != NULL ? de_error_last()->message : de_result_str(r));
        return;
    }
    if (submit_pack(pl, s, slot, priority)) /* DENG-212: before the cooker (packs win) */
        return;
    if (submit_cook(pl, s, slot, priority)) /* DENG-209 */
        return;
    const de__slot *sl = de__slot_get(s, slot);
    const de__manifest_asset *a = sl != NULL ? de__resolved_asset(s->resolved, sl->id) : NULL;
    if (a == NULL || s->manifest == NULL) {
        fail_early(s, slot, DE_ERR_NOT_FOUND, "asset %016llx is not in the asset manifest",
                   sl != NULL ? (unsigned long long)sl->id.hash : 0ull);
        return;
    }
    const de_asset_type_desc *type = de__asset_type_get(s, de__slot_type(s, sl));
    if (type == NULL) {
        fail_early(s, slot, DE_ERR_UNSUPPORTED, "asset \"%s\": no asset type \"%s\" is registered",
                   a->id, a->type);
        return;
    }
    if (type->load == NULL) {
        fail_early(s, slot, DE_ERR_UNSUPPORTED, "asset \"%s\": type \"%s\" has no loader", a->id,
                   a->type);
        return;
    }
    char vpath[DE_FS_PATH_MAX];
    if (!source_path(s, a, vpath, sizeof vpath)) {
        fail_early(s, slot, DE_ERR_INVALID_ARGUMENT,
                   "asset \"%s\": source \"%s\" is not a valid path below the asset root", a->id,
                   a->source);
        return;
    }
    de__pjob *job = job_new(pl, slot, sl, a, type, vpath, priority);
    if (job == NULL) {
        fail_early(s, slot, DE_ERR_OUT_OF_MEMORY, "asset \"%s\": out of memory", a->id);
        return;
    }
    if (!job_table_put(pl, job)) {
        job_free(pl, job);
        fail_early(s, slot, DE_ERR_OUT_OF_MEMORY, "asset \"%s\": out of memory", a->id);
        return;
    }
    de_mutex_lock(&pl->lock);
    queue_push(pl, job);
    de_mutex_unlock(&pl->lock);
    de__pipeline_dispatch(pl);
}

static void job_cancel(de__pipeline *pl, de__pjob *job) {
    job_table_remove(pl, job);
    bool io_cancel = false, queued = false;
    de_mutex_lock(&pl->lock);
    de_atomic_store_u32(&job->cancelled, 1, DE_ATOMIC_RELEASE);
    u32 stage = de_atomic_load_u32(&job->stage, DE_ATOMIC_ACQUIRE);
    if (stage == DE__PJOB_QUEUED) {
        queue_remove(pl, job);
        queued = true;
    } else if (stage == DE__PJOB_READ) {
        io_cancel = true;
    }
    de_mutex_unlock(&pl->lock);
    if (queued) {
        job_discard(pl, job); /* no other thread has seen it */
        return;
    }
    /* The job is alive until the main thread frees it, so its request is valid here. A read
     * that is still queued completes at once (its callback may run right here). */
    if (io_cancel && job->pack != NULL)
        de__pack_stage_cancel(job); /* DENG-212: a pack job has one read per run */
    else if (io_cancel)
        de_io_cancel(&job->io);
    /* WAIT_DEPS: the update hook discards it; every other stage brings it back to the main
     * thread through the finished list. */
}

static void pl_cancel(void *user, de_asset_system *s, u32 slot) {
    (void)s;
    de__pipeline *pl = (de__pipeline *)user;
    de__pjob *job = job_of(pl, slot);
    if (job != NULL)
        job_cancel(pl, job);
}

static void pl_reprioritize(void *user, de_asset_system *s, u32 slot, de__load_priority priority) {
    (void)s;
    de__pipeline *pl = (de__pipeline *)user;
    de__pjob *job = job_of(pl, slot);
    if (job == NULL)
        return;
    de_mutex_lock(&pl->lock);
    if (de_atomic_load_u32(&job->stage, DE_ATOMIC_ACQUIRE) == DE__PJOB_QUEUED) {
        queue_remove(pl, job);
        job->priority = (u8)priority;
        queue_push(pl, job);
    } else {
        /* Already reading or later: the new priority only applies to its dependencies. */
        job->priority = (u8)priority;
    }
    de_mutex_unlock(&pl->lock);
}

/* The backend's update hook: main-thread loads, finished jobs, then waiting parents. */
static void pl_update(void *user, de_asset_system *s) {
    (void)s;
    de__pipeline *pl = (de__pipeline *)user;
    if (!pl->started || pl->in_update)
        return;
    pl->in_update = true;

    /* Main-thread loaders (main_thread_load). Taken as a whole; never run under the lock. */
    de_mutex_lock(&pl->lock);
    de__pjob *m = pl->main_head;
    pl->main_head = pl->main_tail = NULL;
    de_mutex_unlock(&pl->lock);
    while (m != NULL) {
        de__pjob *next = m->m_next;
        if (!de__pjob_cancelled(m))
            stat_add(&pl->stats.main_loads);
        de__load_stage_call(m);
        de__pipeline_finish(m);
        m = next;
    }

    /* Finished jobs, in the order they finished (the stack is LIFO). */
    de__pjob *list = (de__pjob *)de_atomic_exchange_ptr(&pl->done_head, NULL, DE_ATOMIC_ACQUIRE);
    de__pjob *fifo = NULL;
    while (list != NULL) {
        de__pjob *next = list->done_next;
        list->done_next = fifo;
        fifo = list;
        list = next;
    }
    while (fifo != NULL) {
        de__pjob *next = fifo->done_next;
        job_finished(pl, fifo);
        fifo = next;
    }

    resolve_waiting(pl);
    /* DENG-143 / DENG-144: per-slot final states of this update are visible from here (the load
     * API applies its own events where it applies the completions). */
    pl->in_update = false;
}

/* ---- Lifetime ---------------------------------------------------------------------------------
 */

de_result de__pipeline_install(de_asset_system *s) {
    de__pipeline *pl = DE_NEW(s->alloc, de__pipeline, DE_MEM_ASSET);
    if (pl == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "asset pipeline: out of memory");
    memset(pl, 0, sizeof *pl);
    pl->s = s;
    pl->alloc = s->alloc;
    pl->tasks = s->tasks;
    pl->io_limit = 2u * (s->desc.io_threads != 0 ? s->desc.io_threads : 1u);
    pl->source = *de__pipeline_default_source();
    pl->start_error = DE_OK;
    de_mutex_init(&pl->lock);
    de_mutex_init(&pl->dctx_lock);
    de_atomic_init_ptr(&pl->done_head, NULL);
    de_atomic_u64 *counters[] = {&pl->stats.submits,   &pl->stats.reads,  &pl->stats.io_cancelled,
                                 &pl->stats.decoded,   &pl->stats.loads,  &pl->stats.main_loads,
                                 &pl->stats.published, &pl->stats.failed, &pl->stats.discarded,
                                 &pl->stats.unloads};
    for (u32 i = 0; i < (u32)(sizeof counters / sizeof counters[0]); ++i)
        de_atomic_init_u64(counters[i], 0);
    s->pipeline = pl;
    de__load_backend b = {.submit = pl_submit,
                          .cancel = pl_cancel,
                          .reprioritize = pl_reprioritize,
                          .user = pl,
                          .update = pl_update};
    de__asset_set_backend(s, &b);
    if (de__asset_backend(s) == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "asset pipeline: out of memory");
    return DE_OK;
}

void de__pipeline_shutdown(de_asset_system *s) {
    de__pipeline *pl = s != NULL ? s->pipeline : NULL;
    if (pl == NULL)
        return;
    if (pl->started) {
        /* de__load_shutdown cancelled every submission; cancel anything left all the same. */
        for (de__pjob *job = pl->all_head, *next = NULL; job != NULL; job = next) {
            next = job->all_next;
            if (!de__pjob_cancelled(job))
                job_cancel(pl, job);
        }
        /* Wait for every job to come back: help with frame tasks on the main thread, else
         * sleep briefly while the I/O threads finish. */
        while (pl->live_jobs != 0) {
            pl_update(pl, s);
            if (pl->live_jobs == 0)
                break;
            if (pl->tasks != NULL && de_task_worker_index() == 0 &&
                !de_task_done(&pl->task_counter))
                de_task_wait(pl->tasks, &pl->task_counter);
            else
                de_sleep_ns(50000u);
        }
        if (pl->tasks != NULL && de_task_worker_index() == 0)
            de_task_wait(pl->tasks, &pl->task_counter);
        de__io_stage_stop(pl);
        de__decompress_stage_shutdown(pl);
        de_hashmap_free(&pl->jobs);
        if (pl->dfs != NULL)
            DE_FREE(pl->alloc, pl->dfs, sizeof(de__pjob *) * pl->dfs_capacity);
    }
    de__cook_stage_shutdown(pl); /* DENG-209: every job is back */
    de_mutex_destroy(&pl->dctx_lock);
    de_mutex_destroy(&pl->lock);
    s->pipeline = NULL;
    DE_FREE(pl->alloc, pl, sizeof *pl);
}

void de__pipeline_set_source(de_asset_system *s, const de__pipeline_source *source) {
    if (s == NULL || s->pipeline == NULL)
        return;
    DE_ASSERT(s->pipeline->live_jobs == 0);
    s->pipeline->source = source != NULL ? *source : *de__pipeline_default_source();
}

void de__pipeline_get_stats(const de_asset_system *s, de__pipeline_stats *out) {
    memset(out, 0, sizeof *out);
    const de__pipeline *pl = s != NULL ? s->pipeline : NULL;
    if (pl == NULL)
        return;
    out->submits = de_atomic_load_u64(&pl->stats.submits, DE_ATOMIC_RELAXED);
    out->reads = de_atomic_load_u64(&pl->stats.reads, DE_ATOMIC_RELAXED);
    out->io_cancelled = de_atomic_load_u64(&pl->stats.io_cancelled, DE_ATOMIC_RELAXED);
    out->decoded = de_atomic_load_u64(&pl->stats.decoded, DE_ATOMIC_RELAXED);
    out->loads = de_atomic_load_u64(&pl->stats.loads, DE_ATOMIC_RELAXED);
    out->main_loads = de_atomic_load_u64(&pl->stats.main_loads, DE_ATOMIC_RELAXED);
    out->published = de_atomic_load_u64(&pl->stats.published, DE_ATOMIC_RELAXED);
    out->failed = de_atomic_load_u64(&pl->stats.failed, DE_ATOMIC_RELAXED);
    out->discarded = de_atomic_load_u64(&pl->stats.discarded, DE_ATOMIC_RELAXED);
    out->unloads = de_atomic_load_u64(&pl->stats.unloads, DE_ATOMIC_RELAXED);
    out->live_jobs = pl->live_jobs;
}

u32 de__pipeline_debug_stage(de_asset_system *s, u32 slot) {
    de__pipeline *pl = s != NULL ? s->pipeline : NULL;
    de__pjob *job = pl != NULL ? job_of(pl, slot) : NULL;
    return job != NULL ? de_atomic_load_u32(&job->stage, DE_ATOMIC_ACQUIRE) : DE__PJOB_NONE;
}
