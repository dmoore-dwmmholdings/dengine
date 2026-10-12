/*
 * load_ctx.c: the LOAD stage of the load pipeline and the de_asset_load_ctx accessors loaders
 * use (SPEC §11.4; DENG-142). A load context is the pipeline job itself (pipeline_internal.h).
 *
 * Loaders run on a frame worker, or on the main thread inside de_asset_system_update when their
 * type sets `main_thread_load`. No pipeline lock is held while a loader runs.
 */

#include "asset/pipeline_internal.h"

#include "asset/manifest_internal.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_task.h>

#include "platform/de_atomic.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---- Stage --------------------------------------------------------------------------------- */

static void job_free_bytes(de__pjob *job) {
    de__pipeline *pl = job->pl;
    if (job->buf != NULL)
        DE_FREE(pl->alloc, job->buf, (size_t)job->buf_size);
    if (job->raw != NULL)
        DE_FREE(pl->alloc, job->raw, (size_t)(job->raw_size != 0 ? job->raw_size : 1u));
    job->buf = job->raw = NULL;
    job->buf_size = job->raw_size = 0;
    job->data = NULL;
    job->data_size = 0;
}

void de__load_stage_call(de__pjob *job) {
    if (!de__pjob_cancelled(job)) {
        de__pipeline *pl = job->pl;
        (void)de_atomic_fetch_add_u64(&pl->stats.loads, 1, DE_ATOMIC_RELAXED);
        /* A stale error of this thread must not become the loader's message. */
        de_error_clear();
        de_result r = job->type->load(job);
        if (r == DE_OK)
            r = de__pipeline_upload_stage(job->s, job);
        if (r != DE_OK) {
            if (!job->msg_set) {
                const de_error *e = de_error_last();
                if (e != NULL && e->result == r)
                    de__pipeline_job_fail(job, r, "%s", e->message);
                else
                    de__pipeline_job_fail(job, r, "the \"%s\" loader failed: %s", job->type->name,
                                          de_result_str(r));
            }
            job->result = r;
        }
    }
    /* The loaded bytes are only valid during `load`. */
    job_free_bytes(job);
}

void de__load_stage_run(void *user) {
    de__pjob *job = (de__pjob *)user;
    de__load_stage_call(job);
    de__pipeline_finish(job);
}

void de__load_stage_begin(de__pjob *job, bool on_frame_worker) {
    de__pipeline *pl = job->pl;
    if (de__pjob_cancelled(job)) {
        job_free_bytes(job);
        de__pipeline_finish(job);
        return;
    }
    if (job->type->main_thread_load) {
        de_mutex_lock(&pl->lock);
        de__pjob_set_stage(job, DE__PJOB_MAIN_LOAD);
        job->m_next = NULL;
        if (pl->main_tail != NULL)
            pl->main_tail->m_next = job;
        else
            pl->main_head = job;
        pl->main_tail = job;
        de_mutex_unlock(&pl->lock);
        return;
    }
    de__pjob_set_stage(job, DE__PJOB_LOAD);
    if (on_frame_worker || pl->tasks == NULL)
        de__load_stage_run(job);
    else
        de__pipeline_run(pl, DE_TASK_POOL_FRAME, de__load_stage_run, job);
}

/* ---- Accessors ----------------------------------------------------------------------------- */

const void *de_asset_load_ctx_data(const de_asset_load_ctx *ctx, u64 *size) {
    if (size != NULL)
        *size = ctx != NULL ? ctx->data_size : 0u;
    if (ctx == NULL || ctx->data == NULL)
        return "";
    return ctx->data;
}

de_asset_id de_asset_load_ctx_id(const de_asset_load_ctx *ctx) {
    return ctx != NULL ? ctx->id : (de_asset_id){0};
}

void *de_asset_load_ctx_user(const de_asset_load_ctx *ctx) {
    return ctx != NULL ? ctx->type->user : NULL;
}

static const de__manifest_kv *ctx_setting(const de_asset_load_ctx *ctx, const char *key) {
    if (ctx == NULL || key == NULL || ctx->setting_count == 0)
        return NULL;
    return de__manifest_find_setting(ctx->settings, ctx->setting_count, key);
}

bool de_asset_load_ctx_settings_has(const de_asset_load_ctx *ctx, const char *key) {
    return ctx_setting(ctx, key) != NULL;
}

bool de_asset_load_ctx_settings_bool(const de_asset_load_ctx *ctx, const char *key, bool fallback) {
    const de__manifest_kv *kv = ctx_setting(ctx, key);
    return kv != NULL && kv->value.type == DE__MANIFEST_BOOL ? kv->value.u.b : fallback;
}

i64 de_asset_load_ctx_settings_int(const de_asset_load_ctx *ctx, const char *key, i64 fallback) {
    const de__manifest_kv *kv = ctx_setting(ctx, key);
    return kv != NULL && kv->value.type == DE__MANIFEST_INT ? kv->value.u.i : fallback;
}

f64 de_asset_load_ctx_settings_float(const de_asset_load_ctx *ctx, const char *key, f64 fallback) {
    const de__manifest_kv *kv = ctx_setting(ctx, key);
    if (kv == NULL)
        return fallback;
    if (kv->value.type == DE__MANIFEST_FLOAT)
        return kv->value.u.f;
    if (kv->value.type == DE__MANIFEST_INT)
        return (f64)kv->value.u.i;
    return fallback;
}

const char *de_asset_load_ctx_settings_string(const de_asset_load_ctx *ctx, const char *key,
                                              const char *fallback) {
    const de__manifest_kv *kv = ctx_setting(ctx, key);
    return kv != NULL && kv->value.type == DE__MANIFEST_STRING ? kv->value.u.s : fallback;
}

u32 de_asset_load_ctx_settings_string_array(const de_asset_load_ctx *ctx, const char *key,
                                            const char *const **items) {
    const de__manifest_kv *kv = ctx_setting(ctx, key);
    bool ok = kv != NULL && kv->value.type == DE__MANIFEST_STRING_ARRAY;
    if (items != NULL)
        *items = ok ? kv->value.u.arr.items : NULL;
    return ok ? kv->value.u.arr.count : 0u;
}

void de_asset_load_ctx_set_object(de_asset_load_ctx *ctx, void *object, u64 bytes) {
    if (ctx == NULL)
        return;
    if (ctx->object != NULL && ctx->object != object && ctx->type->unload != NULL)
        ctx->type->unload(ctx->object, ctx->type->user);
    ctx->object = object;
    ctx->object_bytes = object != NULL ? bytes : 0u;
}

de_result de_asset_load_ctx_add_dependency(de_asset_load_ctx *ctx, de_asset_id id) {
    if (ctx == NULL || id.hash == 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_asset_load_ctx_add_dependency: %s",
                       ctx == NULL ? "ctx is NULL" : "asset ID 0");
    for (u32 i = 0; i < ctx->dep_count; ++i)
        if (ctx->deps[i].hash == id.hash)
            return DE_OK;
    if (ctx->dep_count == ctx->dep_capacity) {
        u32 cap = ctx->dep_capacity != 0 ? ctx->dep_capacity * 2u : 4u;
        void *p = DE_REALLOC(ctx->pl->alloc, ctx->deps, sizeof(de_asset_id) * ctx->dep_capacity,
                             sizeof(de_asset_id) * cap, DE_ALIGNOF(de_asset_id), DE_MEM_ASSET);
        if (p == NULL)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "asset \"%s\": out of memory adding a dependency",
                           ctx->asset);
        ctx->deps = (de_asset_id *)p;
        ctx->dep_capacity = cap;
    }
    ctx->deps[ctx->dep_count++] = id;
    return DE_OK;
}

de_result de_asset_load_ctx_fail(de_asset_load_ctx *ctx, de_result result, const char *fmt, ...) {
    if (result == DE_OK)
        result = DE_ERR_INTERNAL;
    if (ctx == NULL || fmt == NULL)
        return result;
    char msg[DE__LOAD_MSG_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    de__pipeline_job_fail(ctx, result, "%s", msg);
    return result;
}
