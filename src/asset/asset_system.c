/*
 * asset_system.c: de_asset_system create/destroy, manifest load, locale (SPEC §11; DENG-138).
 *
 * The system is the home of every S07 subsystem (asset_internal.h). This file owns the desc,
 * the allocator, the active manifest and the locale. Later tasks hang their state off the struct
 * and add their own init/shutdown calls to create and destroy.
 */

#include "asset/asset_internal.h"
#include "asset/budget_internal.h"   /* DENG-144 */
#include "asset/load_internal.h"     /* DENG-141 */
#include "asset/mount_internal.h"    /* DENG-212 */
#include "asset/pipeline_internal.h" /* DENG-142 */
#include "asset/slots_internal.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h> /* DENG-144: de_time_now_ns */

#include <string.h>

#define DE__ASSET_DEFAULT_CACHE_DIR    "cache:/decache"
#define DE__ASSET_DEFAULT_IO_THREADS   2u
#define DE__ASSET_DEFAULT_STAGING_RING ((u64)128 * 1024 * 1024)

/* Copies `locale` into the system. false (nothing changed) if it is too long. */
static bool asset_store_locale(de_asset_system *s, const char *locale) {
    if (locale == NULL) {
        s->locale_buf[0] = '\0';
        return true;
    }
    size_t len = strlen(locale);
    if (len > DE__ASSET_LOCALE_MAX)
        return false;
    memcpy(s->locale_buf, locale, len + 1);
    return true;
}

de_result de_asset_system_create(const de_asset_system_desc *desc, de_asset_system **out) {
    if (out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_asset_system_create: out is NULL");
    *out = NULL;
    de_asset_system_desc d = desc != NULL ? *desc : (de_asset_system_desc){0};
    const de_allocator *a = d.allocator != NULL ? d.allocator : de_heap_allocator();

    if (d.locale != NULL && strlen(d.locale) > DE__ASSET_LOCALE_MAX)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_asset_system_create: locale \"%.40s...\" is longer than %d bytes",
                       d.locale, DE__ASSET_LOCALE_MAX);

    de_asset_system *s = DE_NEW(a, de_asset_system, DE_MEM_ASSET);
    if (s == NULL)
        return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "de_asset_system_create: out of memory");
    memset(s, 0, sizeof *s);
    s->alloc = a;
    s->tasks = d.tasks;

    const char *cache_dir = d.cache_dir != NULL ? d.cache_dir : DE__ASSET_DEFAULT_CACHE_DIR;
    s->cache_dir_size = strlen(cache_dir) + 1;
    s->cache_dir_buf = (char *)DE_ALLOC(a, s->cache_dir_size, 1, DE_MEM_ASSET);
    if (s->cache_dir_buf == NULL) {
        DE_FREE(a, s, sizeof *s);
        return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "de_asset_system_create: out of memory");
    }
    memcpy(s->cache_dir_buf, cache_dir, s->cache_dir_size);
    (void)asset_store_locale(s, d.locale);

    d.allocator = a;
    d.cache_dir = s->cache_dir_buf;
    if (d.io_threads == 0)
        d.io_threads = DE__ASSET_DEFAULT_IO_THREADS;
    if (d.staging_ring_bytes == 0)
        d.staging_ring_bytes = DE__ASSET_DEFAULT_STAGING_RING;
    d.locale = NULL; /* the current locale is locale_buf (de__asset_locale) */
    s->desc = d;
    s->log = de_log_register_category("asset", DE_LOG_INFO);

    /* DENG-387: the wake object of blocking waits, before anything can finish a load. */
    if (de__asset_wake_init(s) != DE_OK) {
        de_asset_system_destroy(s);
        return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "de_asset_system_create: out of memory");
    }
    /* DENG-142: the load pipeline is the backend (it starts its threads on the first load). */
    if (de__pipeline_install(s) != DE_OK) {
        de_asset_system_destroy(s);
        return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "de_asset_system_create: out of memory");
    }
    /* DENG-144: budgets (from desc.budgets), residency accounting, LRU lists. */
    if (de__budget_init(s) != DE_OK) {
        de_asset_system_destroy(s);
        return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "de_asset_system_create: out of memory");
    }

    *out = s;
    return DE_OK;
}

void de_asset_system_destroy(de_asset_system *s) {
    if (s == NULL)
        return;
    const de_allocator *a = s->alloc;
    de__asset_events_detach(s);        /* DENG-143: first, so the teardown sends no events */
    de__load_shutdown(s);              /* DENG-141: releases every request and its slot refs */
    de__pipeline_shutdown(s);          /* DENG-142: after the requests cancelled its jobs */
    de__mount_shutdown(s);             /* DENG-212: no job reads a pack any more */
    de__slots_shutdown(s);             /* DENG-140: unloads objects through their types, so first */
    de__asset_types_shutdown(s);       /* DENG-140 */
    de__budget_shutdown(s);            /* DENG-144: records only; objects went with the slots */
    de__resolved_destroy(s->resolved); /* DENG-139: before their manifests */
    de__resolved_destroy(s->failed_resolved);
    de__manifest_destroy(s->manifest);
    de__manifest_destroy(s->failed_manifest);
    DE_FREE(a, s->cache_dir_buf, s->cache_dir_size);
    de__asset_wake_shutdown(s); /* DENG-387: last; every job came back in the pipeline shutdown */
    DE_FREE(a, s, sizeof *s);
}

static de_log_level asset_issue_level(de_asset_issue_severity sev) {
    switch (sev) {
    case DE_ASSET_ISSUE_ERROR:
        return DE_LOG_ERROR;
    case DE_ASSET_ISSUE_WARNING:
        return DE_LOG_WARN;
    default:
        return DE_LOG_INFO;
    }
}

de_result de_asset_manifest_load(de_asset_system *s, const char *vpath) {
    if (s == NULL || vpath == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_asset_manifest_load: NULL argument");
    de__manifest *m = NULL;
    de__manifest_parse_desc pd = {.path = vpath, .allocator = s->alloc};
    de_result r = de__manifest_parse(&pd, &m);
    if (r != DE_OK)
        return r;

    /* DENG-139: a manifest that parsed cleanly is resolved (IDs, bundle members, closures);
     * its issue list (manifest issues + resolution issues) is the one logged and kept. */
    de__resolved *res = NULL;
    if (m->error_count == 0) {
        de__resolve_desc rd = {.allocator = s->alloc};
        r = de__manifest_resolve(m, &rd, &res);
        if (r != DE_OK) {
            de__manifest_destroy(m);
            return r;
        }
    }
    const de__manifest_issue *issues = res != NULL ? res->issues : m->issues;
    u32 issue_count = res != NULL ? res->issue_count : m->issue_count;
    u32 error_count = res != NULL ? res->error_count : m->error_count;

    for (u32 i = 0; i < issue_count; ++i) {
        const de__manifest_issue *is = &issues[i];
        DE_LOG(asset_issue_level(is->severity), s->log, "%s:%u: %s%s%s", is->file, is->line,
               is->key_path, is->key_path[0] != '\0' ? ": " : "", is->message);
    }

    de__resolved_destroy(s->failed_resolved);
    s->failed_resolved = NULL;
    de__manifest_destroy(s->failed_manifest);
    s->failed_manifest = NULL;
    if (error_count != 0) {
        s->failed_manifest = m;
        s->failed_resolved = res;
        /* A root file that could not be read keeps its read error; anything else is a parse
         * error. */
        const de__manifest_issue *first = NULL;
        for (u32 i = 0; i < issue_count && first == NULL; ++i)
            if (issues[i].severity == DE_ASSET_ISSUE_ERROR)
                first = &issues[i];
        de_result code = (m->file_count == 0 && first != NULL) ? first->code : DE_ERR_PARSE;
        return DE_FAIL(code, "asset manifest %s: %u error%s; first: %s:%u: %s%s%s", m->root,
                       error_count, error_count == 1 ? "" : "s", first->file, first->line,
                       first->key_path, first->key_path[0] != '\0' ? ": " : "", first->message);
    }
    de__resolved_destroy(s->resolved);
    de__manifest_destroy(s->manifest);
    s->manifest = m;
    s->resolved = res;
    return DE_OK;
}

const de_asset_manifest_issue *de_asset_manifest_issues(const de_asset_system *s, u32 *count) {
    const de__manifest *m = NULL;
    const de__resolved *res = NULL; /* DENG-139: its list includes the manifest's issues */
    if (s != NULL) {
        bool failed = s->failed_manifest != NULL;
        m = failed ? s->failed_manifest : s->manifest;
        res = failed ? s->failed_resolved : s->resolved;
    }
    if (res != NULL) {
        if (count != NULL)
            *count = res->issue_count;
        return res->issues;
    }
    if (count != NULL)
        *count = m != NULL ? m->issue_count : 0u;
    return m != NULL ? m->issues : NULL;
}

void de_asset_set_locale(de_asset_system *s, const char *locale) {
    if (s == NULL)
        return;
    if (!asset_store_locale(s, locale))
        DE_WARN(s->log, "de_asset_set_locale: \"%.40s...\" is longer than %d bytes; keeping \"%s\"",
                locale, DE__ASSET_LOCALE_MAX, s->locale_buf);
}

/* Once per frame, main thread. Each S07 task adds its step here, in this order: the ECS
 * attachment (DENG-143; first, so this update's events are queued), load completions and request
 * states (DENG-141; they emit the events), then eviction (DENG-144). */
void de_asset_system_update(de_asset_system *s, de_world *world) {
    if (s == NULL)
        return;
    if (world != NULL)
        de__asset_events_attach(s, world); /* DENG-143: once; then a pointer check */
    de__load_update(s);                    /* DENG-141 */
    /* DENG-144: candidates, eviction while over budget, the I/O rate; advances the LRU frame. */
    de__budget_update(s, de_time_now_ns());
}
