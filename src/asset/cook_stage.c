/*
 * cook_stage.c: the COOK stage of the load pipeline (SPEC §11.5; DENG-209). See
 * cook_stage_internal.h. With DE_HAS_COOKER (Debug and Dev builds with tools) the stage runs the
 * cooker library in-process; without it, this file references no cooker symbol and an importer
 * type fails with "cooker not available", as it did in S07.
 */

#include "asset/cook_stage_internal.h"

#include "asset/asset_internal.h"
#include "asset/manifest_internal.h"
#include "asset/pipeline_internal.h"
#include "asset/resolve_internal.h"
#include "asset/slots_internal.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_task.h>

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

struct de__cook_stage {
    de_cooker *cooker;
    de_mutex lock; /* guards the sub-asset table: cooks add on workers, the main thread reads */
    cook_sub *subs;
    u32 sub_count, sub_cap;
    de_hashmap sub_index; /* id hash -> index into subs (probed, never iterated) */
};

/* The stage, created on the first cook. Main thread. */
static de__cook_stage *stage_get(de__pipeline *pl) {
    if (pl->cook != NULL)
        return pl->cook;
    de__cook_stage *cs = DE_NEW(pl->alloc, de__cook_stage, DE_MEM_ASSET);
    if (cs == NULL)
        return NULL;
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
        return NULL;
    }
    de_mutex_init(&cs->lock);
    pl->cook = cs;
    return cs;
}

void de__cook_stage_shutdown(de__pipeline *pl) {
    de__cook_stage *cs = pl != NULL ? pl->cook : NULL;
    if (cs == NULL)
        return;
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
        plan_fail(p, DE_ERR_OUT_OF_MEMORY, "asset \"%s\": out of memory for the cooker", shown);
        return true;
    }
    if (!de_cooker_has_type(cs->cooker, import_type->name) &&
        de_cooker_register_type(cs->cooker, import_type) != DE_OK) {
        const de_error *e = de_error_last();
        plan_fail(p, e != NULL ? e->result : DE_ERR_INTERNAL, "asset \"%s\": %s", shown,
                  e != NULL ? e->message : "the cooker refused the type");
        return true;
    }

    p->asset = *a;
    if (p->subasset != NULL)
        p->asset.id = p->id_buf;
    p->load_type = load_type;
    p->import_type = import_type;
    p->parent_id = a->id;
    return true;
}

/* ---- The cook task (a background worker) ------------------------------------------------------
 */

/* What the task needs besides the job: one block with the strings. */
typedef struct cook_task {
    de__pjob *job;
    const de_asset_type_desc *import_type; /* registry entries never move */
    const char *parent_id;
    const char *subasset; /* NULL or the name */
    const char *asset_root;
    const char *source;
    size_t size;
} cook_task;

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
    de_result rc = de_cooker_cook(cs->cooker, &in, &r);
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
    t->job = job;
    t->import_type = p->import_type;
    t->parent_id = put_str(&at, p->parent_id);
    t->asset_root = put_str(&at, p->asset_root);
    t->source = put_str(&at, p->source);
    t->subasset = p->subasset != NULL ? put_str(&at, p->subasset) : NULL;
    t->size = size;
    /* The cook reads files and may run long: a background worker, like the OPEN stage, and the
     * job shows as OPEN (cancel then lets it finish and discards it). */
    de__pjob_set_stage(job, DE__PJOB_OPEN);
    if (pl->tasks != NULL)
        de_task_submit(pl->tasks, DE_TASK_POOL_BACKGROUND, cook_task_run, t, &pl->task_counter);
    else
        cook_task_run(t);
}

#endif /* DE_HAS_COOKER */
