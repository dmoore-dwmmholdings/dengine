/*
 * cook_stage.c: the COOK stage of the load pipeline (SPEC §11.5; DENG-209). See
 * cook_stage_internal.h. With DE_HAS_COOKER (Debug and Dev builds with tools) the stage runs the
 * cooker library in-process on its own normal-priority cook threads (DENG-387); without it, this
 * file references no cooker symbol and an importer type fails with "cooker not available", as it
 * did in S07.
 */

#include "asset/cook_stage_internal.h"

#include "asset/asset_internal.h"
#include "asset/manifest_internal.h"
#include "asset/pipeline_internal.h"
#include "asset/resolve_internal.h"
#include "asset/slots_internal.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_debug.h> /* DENG-387: de_crash_thread_init on the cook threads */
#include <dengine/de_math.h>  /* DENG-387: de_fp_env_init_thread on the cook threads */
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_task.h>

#include "core/de_compiler.h"
#include "platform/de_atomic.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if DE_HAS_COOKER
#include "cook/lib/de_cooker.h"
#include "core/de_hashmap.h"
#endif

static void plan_fail(de__cook_plan *p, de_result code, const char *fmt, ...) DE_PRINTF_FMT(3, 4);
static void plan_fail(de__cook_plan *p, de_result code, const char *fmt, ...) {
    p->error = code;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->msg, sizeof p->msg, fmt, ap);
    va_end(ap);
}

#if !DE_HAS_COOKER

/* ---- Builds without the cooker (Release, Shipping, DE_BUILD_TOOLS=OFF) ----------------------- */

bool de__cook_stage_plan(de_asset_system *s, u32 slot, de__cook_plan *p) {
    const de__slot *sl = de__slot_get(s, slot);
    if (sl == NULL || s->manifest == NULL || s->desc.mode == DE_ASSET_MODE_PACKED)
        return false;
    const de__manifest_asset *a = de__resolved_asset(s->resolved, sl->id);
    const de_asset_type_desc *type = de__asset_type_get(s, de__slot_type(s, sl));
    if (a == NULL || type == NULL || type->import == NULL)
        return false;
    p->subasset = NULL;
    plan_fail(p, DE_ERR_UNSUPPORTED,
              "asset \"%s\": cooker not available (type \"%s\" has an importer; this build does "
              "not link the cooker: Debug and Dev builds cook source assets, Release and Shipping "
              "load packs)",
              a->id, a->type);
    return true;
}

void de__cook_stage_begin(de__pjob *job, const de__cook_plan *p) {
    (void)p; /* unreachable: the plan always fails without the cooker */
    de__pipeline_job_fail(job, DE_ERR_UNSUPPORTED, "cooker not available");
    de__pipeline_finish(job);
}

void de__cook_stage_shutdown(de__pipeline *pl) {
    (void)pl;
}

bool de__cook_stage_subasset_type(de_asset_system *s, de_asset_id id, u64 *type_hash) {
    (void)s;
    (void)id;
    (void)type_hash;
    return false;
}

void de__cook_stage_get_stats(const de_asset_system *s, de__cook_stage_stats *out) {
    (void)s;
    memset(out, 0, sizeof *out);
}

#else /* DE_HAS_COOKER */

/* ---- The stage ------------------------------------------------------------------------------- */

/* A sub-asset some cook declared. */
typedef struct cook_sub {
    de_asset_id id;
    de_asset_id parent;
    u64 type_hash;
    char *name; /* from the pipeline allocator */
    size_t name_size;
} cook_sub;

typedef struct cook_task cook_task;

/* DENG-387: the most cook threads a stage starts (one per 4 logical cores, at least one). */
#define DE__COOK_THREADS_MAX 4u

struct de__cook_stage {
    de_cooker *cooker;
    de_mutex lock; /* guards the sub-asset table: cooks add on workers, the main thread reads */
    cook_sub *subs;
    u32 sub_count, sub_cap;
    de_hashmap sub_index; /* id hash -> index into subs (probed, never iterated) */

    /* DENG-387: the cook threads (DE_THREAD_NORMAL) and their queue, one FIFO per load priority,
     * guarded by q_lock. None without a task system: cooks then run inline. */
    de_mutex q_lock;
    de_cond q_cond;
    cook_task *q_head[DE__LOAD_PRIORITY_COUNT], *q_tail[DE__LOAD_PRIORITY_COUNT];
    bool stopping;
    de_thread *threads[DE__COOK_THREADS_MAX];
    u32 thread_count;
    de_atomic_u64 cooks, cooks_on_threads;
};

static void cook_task_run(void *user);
static i32 cook_thread_main(void *user);

/* Stops and joins the cook threads. Nothing may be queued. */
static void threads_stop(de__cook_stage *cs) {
    de_mutex_lock(&cs->q_lock);
    cs->stopping = true;
    de_cond_broadcast(&cs->q_cond);
    de_mutex_unlock(&cs->q_lock);
    for (u32 i = 0; i < cs->thread_count; ++i)
        (void)de_thread_join(cs->threads[i]);
    cs->thread_count = 0;
}

/* The stage, created on the first cook. Main thread. With a task system it starts the cook
 * threads: normal priority, so no load waits on a below-normal thread (Trama "Decision: Asset
 * loads never wait on below-normal threads"), and not the frame workers, so a long cook never
 * stalls a frame. On failure the error context says why. */
static de__cook_stage *stage_get(de__pipeline *pl) {
    if (pl->cook != NULL)
        return pl->cook;
    de__cook_stage *cs = DE_NEW(pl->alloc, de__cook_stage, DE_MEM_ASSET);
    if (cs == NULL) {
        (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY, "out of memory for the cooker");
        return NULL;
    }
    memset(cs, 0, sizeof *cs);
    de_cooker_desc cd = {.allocator = pl->alloc,
                         .tasks = pl->tasks,
                         .platform = DE_COOK_PLATFORM_HOST,
                         .cache_dir = pl->s->desc.cache_dir};
    if (de_cooker_create(&cd, &cs->cooker) != DE_OK) {
        DE_FREE(pl->alloc, cs, sizeof *cs);
        return NULL;
    }
    de_hashmap_desc hd = {.allocator = pl->alloc,
                          .key_size = sizeof(u64),
                          .value_size = sizeof(u32),
                          .tag = DE_MEM_ASSET};
    if (de_hashmap_init(&cs->sub_index, &hd) != DE_OK) {
        de_cooker_destroy(cs->cooker);
        DE_FREE(pl->alloc, cs, sizeof *cs);
        (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY, "out of memory for the cooker");
        return NULL;
    }
    de_mutex_init(&cs->lock);
    de_mutex_init(&cs->q_lock);
    de_cond_init(&cs->q_cond);
    de_atomic_init_u64(&cs->cooks, 0);
    de_atomic_init_u64(&cs->cooks_on_threads, 0);
    if (pl->tasks != NULL) {
        u32 n = de_cpu_logical_cores() / 4u;
        n = n < 1u ? 1u : n > DE__COOK_THREADS_MAX ? DE__COOK_THREADS_MAX : n;
        de_result r = DE_OK;
        for (u32 i = 0; i < n && r == DE_OK; ++i) {
            char name[24]; /* "de-cook-" and any u32 */
            snprintf(name, sizeof name, "de-cook-%u", (unsigned)i);
            r = de_thread_create(cook_thread_main, cs, name, DE_THREAD_NORMAL, 0,
                                 &cs->threads[cs->thread_count]);
            if (r == DE_OK)
                cs->thread_count++;
        }
        if (cs->thread_count == 0) {
            const de_error *e = de_error_last();
            char msg[DE__LOAD_MSG_MAX]; /* the job's message adds the asset; keep 200 bytes */
            snprintf(msg, sizeof msg, "%.200s", e != NULL ? e->message : de_result_str(r));
            de_cond_destroy(&cs->q_cond);
            de_mutex_destroy(&cs->q_lock);
            de_mutex_destroy(&cs->lock);
            de_hashmap_free(&cs->sub_index);
            de_cooker_destroy(cs->cooker);
            DE_FREE(pl->alloc, cs, sizeof *cs);
            (void)DE_FAIL(r, "cannot start a cook thread: %s", msg);
            return NULL;
        }
    }
    pl->cook = cs;
    return cs;
}

void de__cook_stage_shutdown(de__pipeline *pl) {
    de__cook_stage *cs = pl != NULL ? pl->cook : NULL;
    if (cs == NULL)
        return;
    threads_stop(cs); /* every job is back, so the queue is empty */
    de_cond_destroy(&cs->q_cond);
    de_mutex_destroy(&cs->q_lock);
    for (u32 i = 0; i < cs->sub_count; ++i)
        DE_FREE(pl->alloc, cs->subs[i].name, cs->subs[i].name_size);
    if (cs->subs != NULL)
        DE_FREE(pl->alloc, cs->subs, sizeof(cook_sub) * cs->sub_cap);
    de_hashmap_free(&cs->sub_index);
    de_mutex_destroy(&cs->lock);
    de_cooker_destroy(cs->cooker);
    DE_FREE(pl->alloc, cs, sizeof *cs);
    pl->cook = NULL;
}

/* The sub-asset entry of `id` (under the lock), or NULL. */
static const cook_sub *sub_find_locked(const de__cook_stage *cs, de_asset_id id) {
    const u32 *i = (const u32 *)de_hashmap_get(&cs->sub_index, id.hash, &id.hash);
    return i != NULL ? &cs->subs[*i] : NULL;
}

/* Records the sub-assets of one cook result (any thread). An entry declared again takes the
 * newest parent, name and type. Out of memory drops the entry: the sub-asset is then unknown. */
static void subs_record(de__pipeline *pl, de__cook_stage *cs, de_asset_id parent,
                        const de_cook_result *r) {
    de_mutex_lock(&cs->lock);
    for (u32 k = 0; k < r->subasset_count; ++k) {
        const de_cook_subasset *s = &r->subassets[k];
        size_t name_size = strlen(s->name) + 1u;
        char *name = (char *)DE_ALLOC(pl->alloc, name_size, 1, DE_MEM_ASSET);
        if (name == NULL)
            continue;
        memcpy(name, s->name, name_size);
        cook_sub e = {s->id, parent, de_hash64_str(s->type), name, name_size};
        u32 *idx = (u32 *)de_hashmap_get(&cs->sub_index, s->id.hash, &s->id.hash);
        if (idx != NULL) {
            DE_FREE(pl->alloc, cs->subs[*idx].name, cs->subs[*idx].name_size);
            cs->subs[*idx] = e;
            continue;
        }
        if (cs->sub_count == cs->sub_cap) {
            u32 ncap = cs->sub_cap != 0 ? cs->sub_cap * 2u : 16u;
            cook_sub *n =
                (cook_sub *)DE_REALLOC(pl->alloc, cs->subs, sizeof(cook_sub) * cs->sub_cap,
                                       sizeof(cook_sub) * ncap, DE_ALIGNOF(cook_sub), DE_MEM_ASSET);
            if (n == NULL) {
                DE_FREE(pl->alloc, name, name_size);
                continue;
            }
            cs->subs = n;
            cs->sub_cap = ncap;
        }
        u32 at = cs->sub_count;
        if (de_hashmap_put(&cs->sub_index, s->id.hash, &s->id.hash, &at) != DE_OK) {
            DE_FREE(pl->alloc, name, name_size);
            continue;
        }
        cs->subs[cs->sub_count++] = e;
    }
    de_mutex_unlock(&cs->lock);
}

void de__cook_stage_get_stats(const de_asset_system *s, de__cook_stage_stats *out) {
    memset(out, 0, sizeof *out);
    const de__cook_stage *cs = s != NULL && s->pipeline != NULL ? s->pipeline->cook : NULL;
    if (cs == NULL)
        return;
    out->threads = cs->thread_count;
    out->cooks = de_atomic_load_u64(&cs->cooks, DE_ATOMIC_ACQUIRE);
    out->cooks_on_threads = de_atomic_load_u64(&cs->cooks_on_threads, DE_ATOMIC_ACQUIRE);
}

bool de__cook_stage_subasset_type(de_asset_system *s, de_asset_id id, u64 *type_hash) {
    de__cook_stage *cs = s != NULL && s->pipeline != NULL ? s->pipeline->cook : NULL;
    if (cs == NULL || id.hash == 0)
        return false;
    de_mutex_lock(&cs->lock);
    const cook_sub *e = sub_find_locked(cs, id);
    if (e != NULL && type_hash != NULL)
        *type_hash = e->type_hash;
    de_mutex_unlock(&cs->lock);
    return e != NULL;
}

/* ---- Plan (main thread) ---------------------------------------------------------------------- */

bool de__cook_stage_plan(de_asset_system *s, u32 slot, de__cook_plan *p) {
    const de__slot *sl = de__slot_get(s, slot);
    if (sl == NULL || s->manifest == NULL || s->pipeline == NULL ||
        s->desc.mode == DE_ASSET_MODE_PACKED)
        return false;
    de__pipeline *pl = s->pipeline;
    p->error = DE_OK;
    p->msg[0] = '\0';
    p->subasset = NULL;
    const de__manifest_asset *a = de__resolved_asset(s->resolved, sl->id);
    if (a == NULL) {
        /* A sub-asset an earlier cook declared? Its parent is cooked again (the cook cache,
         * DENG-210, makes that cheap) and the sub-asset's bytes are loaded. */
        de__cook_stage *cs = pl->cook;
        if (cs == NULL)
            return false;
        bool found = false;
        de_asset_id parent = {0};
        de_mutex_lock(&cs->lock);
        const cook_sub *e = sub_find_locked(cs, sl->id);
        if (e != NULL) {
            found = true;
            parent = e->parent;
            snprintf(p->id_buf, sizeof p->id_buf, "%s", e->name); /* the name, for now */
        }
        de_mutex_unlock(&cs->lock);
        if (!found)
            return false;
        a = de__resolved_asset(s->resolved, parent);
        if (a == NULL) {
            plan_fail(p, DE_ERR_NOT_FOUND,
                      "sub-asset \"%s\" (%016llx): its parent %016llx is no longer in the asset "
                      "manifest",
                      p->id_buf, (unsigned long long)sl->id.hash, (unsigned long long)parent.hash);
            return true;
        }
        char name[DE_FS_PATH_MAX];
        memcpy(name, p->id_buf, strlen(p->id_buf) + 1u);
        int n = snprintf(p->id_buf, sizeof p->id_buf, "%s#%s", a->id, name);
        if (n < 0 || (size_t)n >= sizeof p->id_buf) {
            plan_fail(p, DE_ERR_INVALID_ARGUMENT, "sub-asset \"%s#%s\": ID too long", a->id, name);
            return true;
        }
        p->subasset = p->id_buf + strlen(a->id) + 1u;
    }
    const de_asset_type_desc *import_type = de__asset_type_get(s, de__asset_type_find(s, a->type));
    if (p->subasset == NULL && (import_type == NULL || import_type->import == NULL))
        return false; /* loads from its source as it is */
    if (import_type == NULL || import_type->import == NULL) {
        plan_fail(p, DE_ERR_UNSUPPORTED,
                  "sub-asset \"%s\": its parent's type \"%s\" is not registered with an importer",
                  p->id_buf, a->type);
        return true;
    }
    const char *shown = p->subasset != NULL ? p->id_buf : a->id;
    const de_asset_type_desc *load_type =
        p->subasset != NULL ? de__asset_type_get(s, de__slot_type(s, sl)) : import_type;
    if (load_type == NULL) {
        plan_fail(p, DE_ERR_UNSUPPORTED, "asset \"%s\": its asset type is not registered", shown);
        return true;
    }
    if (load_type->load == NULL) {
        plan_fail(p, DE_ERR_UNSUPPORTED, "asset \"%s\": type \"%s\" has no loader", shown,
                  load_type->name);
        return true;
    }

    /* The asset root: `asset_root` next to the root manifest (as the source path of
     * pipeline.c); the source with the locale override, relative to it. */
    const de__manifest *m = s->manifest;
    const char *slash = strrchr(m->root, '/');
    int dir = slash != NULL ? (int)(slash - m->root) : 0;
    char joined[DE_FS_PATH_MAX];
    int n = snprintf(joined, sizeof joined, "%.*s/%s", dir, m->root, m->asset_root);
    if (n < 0 || (size_t)n >= sizeof joined ||
        de_path_normalize(joined, p->asset_root, sizeof p->asset_root) != DE_OK) {
        plan_fail(p, DE_ERR_INVALID_ARGUMENT, "asset \"%s\": the asset root \"%s\" is not valid",
                  shown, m->asset_root);
        return true;
    }
    const char *src = de__manifest_source_for(m, a, de__asset_locale(s));
    n = snprintf(p->source, sizeof p->source, "%s", src);
    int vn = snprintf(joined, sizeof joined, "%s/%s", p->asset_root, src);
    if (n < 0 || (size_t)n >= sizeof p->source || vn < 0 || (size_t)vn >= sizeof joined ||
        de_path_normalize(joined, p->vpath, sizeof p->vpath) != DE_OK) {
        plan_fail(p, DE_ERR_INVALID_ARGUMENT,
                  "asset \"%s\": source \"%s\" is not a valid path below the asset root", shown,
                  src);
        return true;
    }

    /* The cooker mirrors the runtime registration of the type. */
    de__cook_stage *cs = stage_get(pl);
    if (cs == NULL) {
        const de_error *e = de_error_last();
        plan_fail(p, e != NULL ? e->result : DE_ERR_OUT_OF_MEMORY, "asset \"%s\": %s", shown,
                  e != NULL ? e->message : "out of memory for the cooker");
        return true;
    }
    if (!de_cooker_has_type(cs->cooker, import_type->name) &&
        de_cooker_register_type(cs->cooker, import_type) != DE_OK) {
        const de_error *e = de_error_last();
        plan_fail(p, e != NULL ? e->result : DE_ERR_INTERNAL, "asset \"%s\": %s", shown,
                  e != NULL ? e->message : "the cooker refused the type");
        return true;
    }
    /* DENG-101: every other runtime type too, so an importer can cook a sub-asset through the
     * importer of another type (de_cooker_import_cook_subasset: a model's textures and prefab)
     * and the result does not depend on which assets were cooked first. Types registered later
     * (a game reload) are added on the next plan. */
    u32 type_count = 0;
    const char *const *type_names = de__asset_type_names(s, &type_count);
    for (u32 i = 0; i < type_count; ++i) {
        if (de_cooker_has_type(cs->cooker, type_names[i]))
            continue;
        const de_asset_type_desc *t = de__asset_type_get(s, de__asset_type_find(s, type_names[i]));
        if (t != NULL && de_cooker_register_type(cs->cooker, t) != DE_OK)
            DE_WARN(s->log, "cook stage: the cooker refused type \"%s\"", type_names[i]);
    }

    p->asset = *a;
    if (p->subasset != NULL)
        p->asset.id = p->id_buf;
    p->load_type = load_type;
    p->import_type = import_type;
    p->parent_id = a->id;
    return true;
}

/* ---- The cook task (a cook thread) -----------------------------------------------------------
 */

/* What the task needs besides the job: one block with the strings. */
struct cook_task {
    cook_task *next; /* (q_lock) the queue of its priority */
    u8 priority;     /* de__load_priority at submit */
    de__pjob *job;
    const de_asset_type_desc *import_type; /* registry entries never move */
    const char *parent_id;
    const char *subasset; /* NULL or the name */
    const char *asset_root;
    const char *source;
    size_t size;
};

/* DENG-387: set on the cook threads (the stats count the cooks that ran there). */
static DE_THREAD_LOCAL bool tl_cook_thread;

/* The most urgent queued task, or NULL. Under q_lock. */
static cook_task *queue_pop(de__cook_stage *cs) {
    for (u32 p = 0; p < DE__LOAD_PRIORITY_COUNT; ++p) {
        cook_task *t = cs->q_head[p];
        if (t == NULL)
            continue;
        cs->q_head[p] = t->next;
        if (cs->q_head[p] == NULL)
            cs->q_tail[p] = NULL;
        return t;
    }
    return NULL;
}

static i32 cook_thread_main(void *user) {
    de__cook_stage *cs = (de__cook_stage *)user;
    de_fp_env_init_thread(); /* as the task workers: importers run under the engine's FP mode */
    de_crash_thread_init();
    tl_cook_thread = true;
    de_mutex_lock(&cs->q_lock);
    for (;;) {
        cook_task *t = queue_pop(cs);
        if (t == NULL) {
            if (cs->stopping)
                break;
            de_cond_wait(&cs->q_cond, &cs->q_lock);
            continue;
        }
        de_mutex_unlock(&cs->q_lock);
        cook_task_run(t);
        de_mutex_lock(&cs->q_lock);
    }
    de_mutex_unlock(&cs->q_lock);
    return 0;
}

static char *put_str(char **at, const char *src) {
    size_t n = strlen(src) + 1u;
    char *dst = *at;
    memcpy(dst, src, n);
    *at += n;
    return dst;
}

/* The cooker's message without the leading `asset "<id>": `, which the job adds itself. */
static const char *strip_prefix(const char *msg, const char *id) {
    char prefix[DE__LOAD_MSG_MAX];
    int n = snprintf(prefix, sizeof prefix, "asset \"%s\": ", id);
    if (n > 0 && (size_t)n < sizeof prefix && strncmp(msg, prefix, (size_t)n) == 0)
        return msg + n;
    return msg;
}

/* DENG-214: copies the cook's extra sources into the job as normalised virtual paths (below the
 * asset root), for the hot reload index. Out of memory or an odd path only leaves a path out:
 * hot reload then misses edits of that file. */
static void extras_record(de__pjob *job, const char *asset_root, const de_cook_result *r) {
    if (r->extra_source_count == 0)
        return;
    de__pipeline *pl = job->pl;
    size_t size = 0;
    for (u32 i = 0; i < r->extra_source_count; ++i)
        size += strlen(asset_root) + 1u + strlen(r->extra_sources[i]) + 1u;
    char *block = (char *)DE_ALLOC(pl->alloc, size, 1, DE_MEM_ASSET);
    if (block == NULL)
        return;
    size_t at = 0;
    u32 n = 0;
    for (u32 i = 0; i < r->extra_source_count; ++i) {
        char joined[DE_FS_PATH_MAX];
        int w = snprintf(joined, sizeof joined, "%s/%s", asset_root, r->extra_sources[i]);
        /* The normalised form is never longer than the joined one, which fits its share. */
        if (w < 0 || (size_t)w >= sizeof joined ||
            de_path_normalize(joined, block + at, size - at) != DE_OK)
            continue;
        at += strlen(block + at) + 1u;
        n++;
    }
    job->extra_paths = block;
    job->extra_size = size;
    job->extra_count = n;
}

static void cook_task_run(void *user) {
    cook_task *t = (cook_task *)user;
    de__pjob *job = t->job;
    de__pipeline *pl = job->pl;
    de__cook_stage *cs = pl->cook;
    if (de__pjob_cancelled(job)) {
        DE_FREE(pl->alloc, t, t->size);
        de__pipeline_finish(job);
        return;
    }
    (void)de_atomic_fetch_add_u64(&cs->cooks, 1, DE_ATOMIC_ACQ_REL);
    if (tl_cook_thread)
        (void)de_atomic_fetch_add_u64(&cs->cooks_on_threads, 1, DE_ATOMIC_ACQ_REL);

    /* The job's copies, not the manifest: a manifest reload may free it meanwhile. */
    de__manifest_asset a;
    memset(&a, 0, sizeof a);
    a.id = t->parent_id;
    a.type = t->import_type->name;
    a.source = t->source;
    a.file = "";
    a.settings = job->settings;
    a.setting_count = job->setting_count;
    de_cook_input in = {.asset = &a, .asset_root = t->asset_root, .source = NULL};
    de_cook_result r;
    de_error_clear();
    const u64 started_ns = de_time_now_ns();
    de_result rc = de_cooker_cook(cs->cooker, &in, &r);
    job->cook_ms = (f64)(de_time_now_ns() - started_ns) / 1000000.0;
    job->has_cook_ms = true;
    if (rc != DE_OK) {
        const de_error *e = de_error_last();
        const char *msg = e != NULL && e->result == rc ? e->message : de_result_str(rc);
        if (t->subasset != NULL)
            de__pipeline_job_fail(job, rc, "cooking \"%s\" failed: %s", t->parent_id,
                                  strip_prefix(msg, t->parent_id));
        else
            de__pipeline_job_fail(job, rc, "%s", strip_prefix(msg, t->parent_id));
        DE_FREE(pl->alloc, t, t->size);
        de__pipeline_finish(job);
        return;
    }
    subs_record(pl, cs, de_asset_id_from(t->parent_id), &r);
    if (job->s->reload != NULL)
        extras_record(job, t->asset_root, &r); /* DENG-214: hot reload watches them */

    const void *bytes = r.data;
    u64 size = r.size;
    rc = DE_OK;
    if (t->subasset != NULL) {
        const de_cook_subasset *sub = de_cook_result_subasset(&r, t->subasset);
        if (sub == NULL) {
            rc = DE_ERR_NOT_FOUND;
            de__pipeline_job_fail(job, rc, "the cook of \"%s\" no longer declares sub-asset \"%s\"",
                                  t->parent_id, t->subasset);
        } else {
            bytes = sub->data;
            size = sub->size;
        }
    } else {
        /* The importer's dependencies load with the asset (packs keep them in the TOC). */
        for (u32 i = 0; i < r.dependency_count && rc == DE_OK; ++i) {
            rc = de_asset_load_ctx_add_dependency(job, r.dependencies[i]);
            if (rc != DE_OK)
                de__pipeline_job_fail(job, rc, "out of memory for its dependencies");
        }
    }
    if (rc == DE_OK && size != 0) {
        job->buf = DE_ALLOC(pl->alloc, (size_t)size, 16, DE_MEM_ASSET);
        if (job->buf == NULL) {
            rc = DE_ERR_OUT_OF_MEMORY;
            de__pipeline_job_fail(job, rc, "out of memory for %llu cooked bytes",
                                  (unsigned long long)size);
        } else {
            memcpy(job->buf, bytes, (size_t)size);
            job->buf_size = size;
        }
    }
    de_cook_result_free(&r);
    DE_FREE(pl->alloc, t, t->size);
    if (rc != DE_OK) {
        de__pipeline_finish(job);
        return;
    }
    job->data = job->buf;
    job->data_size = size;
    de__pipeline_progress(job, size, size);
    de__load_stage_begin(job, false);
}

void de__cook_stage_begin(de__pjob *job, const de__cook_plan *p) {
    de__pipeline *pl = job->pl;
    size_t size = sizeof(cook_task) + strlen(p->parent_id) + 1u + strlen(p->asset_root) + 1u +
                  strlen(p->source) + 1u + (p->subasset != NULL ? strlen(p->subasset) + 1u : 0u);
    cook_task *t = (cook_task *)DE_ALLOC(pl->alloc, size, DE_ALIGNOF(cook_task), DE_MEM_ASSET);
    if (t == NULL) {
        de__pipeline_job_fail(job, DE_ERR_OUT_OF_MEMORY, "out of memory starting its cook");
        de__pipeline_finish(job);
        return;
    }
    char *at = (char *)(t + 1);
    t->next = NULL;
    t->priority =
        job->priority < DE__LOAD_PRIORITY_COUNT ? job->priority : (u8)DE__LOAD_PRIORITY_NORMAL;
    t->job = job;
    t->import_type = p->import_type;
    t->parent_id = put_str(&at, p->parent_id);
    t->asset_root = put_str(&at, p->asset_root);
    t->source = put_str(&at, p->source);
    t->subasset = p->subasset != NULL ? put_str(&at, p->subasset) : NULL;
    t->size = size;
    /* The job shows as OPEN (cancel then lets the cook finish and discards it). The cook reads
     * files and may run long, so it runs on a cook thread, most urgent load priority first
     * (DENG-387). Not DE_TASK_POOL_BACKGROUND: its workers run below normal priority, and on
     * Windows a busy main thread and frame workers starve them for seconds, while a game waits
     * on the cooked asset (Trama "Decision: Asset loads never wait on below-normal threads").
     * Not DE_TASK_POOL_FRAME: a cold cook can take long, and it would stall the frame. */
    de__pjob_set_stage(job, DE__PJOB_OPEN);
    de__cook_stage *cs = pl->cook;
    if (cs == NULL || cs->thread_count == 0) {
        cook_task_run(t); /* no task system: every stage runs inline */
        return;
    }
    de_mutex_lock(&cs->q_lock);
    if (cs->q_tail[t->priority] != NULL)
        cs->q_tail[t->priority]->next = t;
    else
        cs->q_head[t->priority] = t;
    cs->q_tail[t->priority] = t;
    de_cond_signal(&cs->q_cond);
    de_mutex_unlock(&cs->q_lock);
}

#endif /* DE_HAS_COOKER */
