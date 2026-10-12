/* de_det_harness.c: the determinism harness (DENG-353). See de_det_harness.h. */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* fopen */
#endif
#include "de_det_harness.h"

#include <dengine/de_math.h>

#include <stdio.h>
#include <string.h>

de_result de_det_session_begin(de_det_session *s, const de_det_scenario *scenario, u64 seed,
                               u32 worker_count) {
    if (s == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_det_session_begin: NULL session");
    memset(s, 0, sizeof *s);
    if (scenario == NULL || scenario->setup == NULL || scenario->name == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_det_session_begin: incomplete scenario");

    s->scenario = scenario;
    s->seed = seed;
    s->worker_count = worker_count;
    s->dt = scenario->dt > 0.0f ? scenario->dt : DE_DET_DEFAULT_DT;
    s->mask = scenario->checksum_mask != 0 ? scenario->checksum_mask : (u32)DE_COMP_SNAPSHOT;

    /* The calling thread runs systems too (serially, or as frame worker 0). */
    de_fp_env_init_thread();

    de_result r = DE_OK;
    if (worker_count > 0) {
        de_task_system_desc td;
        memset(&td, 0, sizeof td);
        td.worker_count = worker_count;
        td.background_worker_count = 1;
        r = de_task_system_create(&td, &s->tasks);
        if (r != DE_OK)
            goto fail;
    }
    r = de_type_registry_create(NULL, &s->types);
    if (r != DE_OK)
        goto fail;
    de_world_desc wd;
    memset(&wd, 0, sizeof wd);
    wd.types = s->types;
    wd.tasks = s->tasks;
    r = de_world_create(&wd, &s->world);
    if (r != DE_OK)
        goto fail;
    if (scenario->state_size > 0) {
        s->state = DE_ALLOC(de_heap_allocator(), scenario->state_size, 16, DE_MEM_GENERAL);
        if (s->state == NULL) {
            r = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_det_session_begin: scenario state");
            goto fail;
        }
        memset(s->state, 0, scenario->state_size);
    }
    r = scenario->setup(s->world, seed, s->state);
    if (r != DE_OK)
        goto fail;
    return DE_OK;

fail:
    de_det_session_end(s);
    return r;
}

de_result de_det_session_step(de_det_session *s, u64 *out_checksum) {
    if (s == NULL || s->world == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_det_session_step: no session");
    if (s->scenario->tick != NULL)
        s->scenario->tick(s->world, de_world_tick(s->world), s->state);
    const de_result r = de_world_run_schedule(s->world, DE_SCHEDULE_FIXED_UPDATE, s->dt, 0.0f);
    if (r != DE_OK)
        return r;
    if (out_checksum != NULL)
        *out_checksum = de_world_checksum(s->world, s->mask);
    return DE_OK;
}

void de_det_session_end(de_det_session *s) {
    if (s == NULL)
        return;
    if (s->world != NULL && s->scenario != NULL && s->scenario->teardown != NULL)
        s->scenario->teardown(s->world, s->state);
    de_world_destroy(s->world);
    if (s->state != NULL)
        DE_FREE(de_heap_allocator(), s->state, s->scenario->state_size);
    de_type_registry_destroy(s->types);
    de_task_system_destroy(s->tasks);
    memset(s, 0, sizeof *s);
}

de_result de_det_run(const de_det_scenario *scenario, u64 seed, u32 worker_count,
                     u64 *out_checksums) {
    if (scenario == NULL || (out_checksums == NULL && scenario->ticks > 0))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_det_run: NULL argument");
    de_det_session s;
    de_result r = de_det_session_begin(&s, scenario, seed, worker_count);
    if (r != DE_OK)
        return r;
    for (u32 t = 0; t < scenario->ticks && r == DE_OK; ++t)
        r = de_det_session_step(&s, &out_checksums[t]);
    de_det_session_end(&s);
    return r;
}

u32 de_det_compare(const u64 *a, const u64 *b, u32 count) {
    if (a == NULL || b == NULL)
        return count > 0 ? 0u : DE_DET_NO_DIVERGENCE;
    for (u32 t = 0; t < count; ++t) {
        if (a[t] != b[t])
            return t;
    }
    return DE_DET_NO_DIVERGENCE;
}

u32 de_det_report(const char *what, const u64 *a, const u64 *b, u32 count) {
    const u32 first = de_det_compare(a, b, count);
    if (first == DE_DET_NO_DIVERGENCE || a == NULL || b == NULL)
        return first;
    u32 differing = 0;
    for (u32 t = first; t < count; ++t)
        differing += a[t] != b[t];
    fprintf(stderr, "determinism: %s: first divergent tick %u (%u of %u ticks differ)\n",
            what != NULL ? what : "runs", first, differing, count);
    const u32 lo = first >= 2u ? first - 2u : 0u;
    const u32 hi = first + 3u < count ? first + 3u : count;
    for (u32 t = lo; t < hi; ++t) {
        fprintf(stderr, "  tick %5u  %016llx  %016llx%s\n", t, (unsigned long long)a[t],
                (unsigned long long)b[t], a[t] != b[t] ? "  <- differs" : "");
    }
    return first;
}

de_result de_det_write_checksums(const char *path, const char *scenario_name, u64 seed,
                                 const u64 *checksums, u32 count) {
    if (path == NULL || scenario_name == NULL || (checksums == NULL && count > 0))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_det_write_checksums: NULL argument");
    /* Binary mode: LF line ends on every platform, so the files compare byte for byte. */
    FILE *f = fopen(path, "wb");
    if (f == NULL)
        return DE_FAIL(DE_ERR_IO, "de_det_write_checksums: cannot open '%s'", path);
    bool ok = fprintf(f,
                      "# de_det v1 scenario=%s seed=0x%llx ticks=%u: de_world_checksum after each "
                      "tick, one per line\n",
                      scenario_name, (unsigned long long)seed, count) > 0;
    for (u32 t = 0; ok && t < count; ++t)
        ok = fprintf(f, "%016llx\n", (unsigned long long)checksums[t]) > 0;
    ok = (fclose(f) == 0) && ok;
    if (!ok)
        return DE_FAIL(DE_ERR_IO, "de_det_write_checksums: cannot write '%s'", path);
    return DE_OK;
}

/* Parses one line of exactly 1..16 hex digits (surrounding blanks allowed). */
static bool parse_hex_line(const char *line, u64 *out) {
    const char *p = line;
    while (*p == ' ' || *p == '\t')
        ++p;
    u64 v = 0;
    u32 digits = 0;
    for (;; ++p) {
        const char c = *p;
        u32 d;
        if (c >= '0' && c <= '9')
            d = (u32)(c - '0');
        else if (c >= 'a' && c <= 'f')
            d = (u32)(c - 'a') + 10u;
        else if (c >= 'A' && c <= 'F')
            d = (u32)(c - 'A') + 10u;
        else
            break;
        if (++digits > 16u)
            return false;
        v = (v << 4) | d;
    }
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        ++p;
    if (digits == 0 || *p != '\0')
        return false;
    *out = v;
    return true;
}

de_result de_det_read_checksums(const char *path, u64 *out, u32 capacity, u32 *out_count) {
    if (path == NULL || out_count == NULL || (out == NULL && capacity > 0))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_det_read_checksums: NULL argument");
    *out_count = 0;
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return DE_FAIL(DE_ERR_NOT_FOUND, "de_det_read_checksums: cannot open '%s'", path);
    char line[256];
    u32 n = 0, line_no = 0;
    de_result r = DE_OK;
    while (fgets(line, (int)sizeof line, f) != NULL) {
        ++line_no;
        const size_t len = strlen(line);
        if (len + 1 == sizeof line && line[len - 1] != '\n') {
            /* A line longer than the buffer is never a checksum; a long comment is fine. */
            if (line[0] != '#') {
                r = DE_FAIL(DE_ERR_PARSE, "%s:%u: line too long", path, line_no);
                break;
            }
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n') {
            }
            continue;
        }
        if (line[0] == '#' || line[0] == '\n' || (line[0] == '\r' && line[1] == '\n'))
            continue;
        u64 v;
        if (!parse_hex_line(line, &v)) {
            r = DE_FAIL(DE_ERR_PARSE, "%s:%u: not a hex checksum", path, line_no);
            break;
        }
        if (n >= capacity) {
            r = DE_FAIL(DE_ERR_PARSE, "%s:%u: more than %u checksums", path, line_no, capacity);
            break;
        }
        out[n++] = v;
    }
    fclose(f);
    *out_count = n;
    return r;
}
