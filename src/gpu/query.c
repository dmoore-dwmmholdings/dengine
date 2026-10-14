/* query.c — de_gpu front-end parts of debugging and timing (DENG-326; SPEC §12.1, §12.3, §18.1,
 * §18.3): the marker stack of a command list, debug names, timestamp conversion and GPU zones in
 * the profiler. The backends do the native work (gpu_backend.h: cmd_marker_*, cmd_timestamp,
 * cmd_resolve_queries, object_name, queue_timestamp_*).
 *
 * Marker stack. Every command list keeps the names and colours of its open markers inline
 * (de_gpu_cmd.marker_*): pushing a marker copies its name into a fixed buffer and never allocates
 * (no heap traffic in per-frame paths, CONVENTIONS §3). Names that do not fit are cut; levels past
 * DE__GPU_MARKER_LEVELS are counted but not kept. DENG-328 reads the stack for its crash report.
 *
 * Names. Every create passes the desc's name to the backend's object_name. In Debug and Dev an
 * unnamed object is named "<kind>#<slot>" (record and native object), so captures and messages
 * always name it.
 *
 * GPU zones. de_gpu_profile_zones turns resolved timestamps of named regions into profiler GPU
 * zones: one context per device and queue (made on first use in each profiler span), the regions
 * sent in nesting order (begin, children, end). Core (src/core/prof/) owns the Tracy side and only
 * sees integers and strings: core never includes gpu headers. Without DE_PROFILE nothing here
 * refers to the profiler.
 */
#include "gpu/gpu_backend.h"

#include <stdio.h>
#include <string.h>

#ifdef DE_PROFILE
#include "core/prof/prof_internal.h"
#endif

/* ---------- Marker stack ---------- */

void de__gpu_cmd_marker_record(de_gpu_cmd *c, const char *name, u32 color) {
    const u32 level = c->marker_depth - 1u;
    if (level >= DE__GPU_MARKER_LEVELS)
        return;
    u32 at = c->marker_text_used;
    if (at > DE__GPU_MARKER_TEXT - 1u)
        at = DE__GPU_MARKER_TEXT - 1u; /* full: the name is "" (the shared terminator) */
    size_t n = 0;
    const size_t room = DE__GPU_MARKER_TEXT - 1u - at;
    if (name)
        while (n < room && name[n])
            ++n;
    if (n)
        memcpy(c->marker_text + at, name, n);
    c->marker_text[at + n] = '\0';
    c->marker_offset[level] = (u16)at;
    c->marker_color[level] = color & 0xFFFFFFu;
    c->marker_text_used = at + (u32)n + 1u;
}

void de__gpu_cmd_marker_forget(de_gpu_cmd *c) {
    const u32 level = c->marker_depth; /* the level just closed */
    if (level < DE__GPU_MARKER_LEVELS)
        c->marker_text_used = c->marker_offset[level];
}

u32 de__gpu_cmd_marker_depth(const de_gpu_cmd *c) {
    return c ? c->marker_depth : 0;
}

const char *de__gpu_cmd_marker_name(const de_gpu_cmd *c, u32 level) {
    if (!c || level >= c->marker_depth || level >= DE__GPU_MARKER_LEVELS)
        return "";
    return c->marker_text + c->marker_offset[level];
}

u32 de__gpu_cmd_marker_color(const de_gpu_cmd *c, u32 level) {
    if (!c || level >= c->marker_depth || level >= DE__GPU_MARKER_LEVELS)
        return 0;
    return c->marker_color[level];
}

size_t de__gpu_cmd_marker_path(const de_gpu_cmd *c, char *out, size_t cap) {
    if (!out || cap == 0)
        return 0;
    size_t used = 0;
    out[0] = '\0';
    const u32 depth = de__gpu_cmd_marker_depth(c);
    for (u32 l = 0; l < depth && used + 1 < cap; ++l) {
        const char *name = l < DE__GPU_MARKER_LEVELS ? de__gpu_cmd_marker_name(c, l) : "...";
        int w = snprintf(out + used, cap - used, "%s%s", l ? "/" : "", name);
        if (w < 0)
            break;
        used += (size_t)w < cap - used ? (size_t)w : cap - used - 1;
    }
    return used;
}

/* ---------- Names ---------- */

void de__gpu_default_name(char name[DE__GPU_NAME_MAX], de__gpu_kind kind, u32 id) {
#if DE__GPU_DEFAULT_NAMES
    if (name[0] == '\0')
        snprintf(name, DE__GPU_NAME_MAX, "%s#%u", de__gpu_kind_name(kind),
                 (unsigned)(id & (DE__GPU_POOL_MAX_SLOTS - 1u)));
#else
    (void)name;
    (void)kind;
    (void)id;
#endif
}

void de__gpu_name_object(de_gpu_device *d, de__gpu_kind kind, u32 id, void *rec, const char *name) {
    if (!d->vt->object_name)
        return;
    char tmp[DE__GPU_NAME_MAX];
    if (!name) {
        tmp[0] = '\0';
        de__gpu_default_name(tmp, kind, id);
        name = tmp;
    }
    if (name[0])
        d->vt->object_name(d, kind, rec, name);
}

/* ---------- Timestamps ---------- */

u64 de_gpu_timestamp_frequency(de_gpu_device *d, de_gpu_queue q) {
    if (!d)
        return 0;
    if ((u32)q < DE_QUEUE_COUNT_ && d->vt->queue_timestamp_frequency) {
        const u64 f = d->vt->queue_timestamp_frequency(d, q);
        if (f)
            return f;
    }
    return d->caps.timestamp_frequency;
}

f64 de_gpu_timestamp_to_ms(de_gpu_device *d, de_gpu_queue q, u64 ticks) {
    const u64 f = de_gpu_timestamp_frequency(d, q);
    return f ? (f64)ticks * 1000.0 / (f64)f : 0.0;
}

/* ---------- GPU zones in the profiler ---------- */

#ifdef DE_PROFILE

static const char *const k_queue_names[DE_QUEUE_COUNT_] = {"graphics", "compute", "copy"};

/* The profiler's API number for a backend (prof_internal.h, Tracy's GpuContextType). */
static u32 prof_api(de_gpu_backend b) {
    switch (b) {
    case DE_GPU_VULKAN:
        return DE__PROF_GPU_API_VULKAN;
    case DE_GPU_D3D12:
        return DE__PROF_GPU_API_D3D12;
    case DE_GPU_METAL:
        return DE__PROF_GPU_API_METAL;
    default:
        return DE__PROF_GPU_API_CUSTOM;
    }
}

static bool tick_ok(u64 t) {
    return t != 0 && t != UINT64_MAX;
}

/* The queue's context in the running span, made now when it has none. Under submit_lock[q]. */
static u32 queue_context(de_gpu_device *d, de_gpu_queue q, u32 span, u64 fallback_ticks) {
    if (d->prof_gpu_context[q] && d->prof_gpu_span[q] == span)
        return d->prof_gpu_context[q] - 1u;
    const u64 freq = de_gpu_timestamp_frequency(d, q);
    if (!freq)
        return DE__PROF_GPU_NO_CONTEXT;
    /* The GPU time now, so the profiler lines GPU and CPU time up. Without it, the first region
     * stands in: its zones then show a frame or two late. */
    u64 now = 0;
    if (!d->vt->queue_timestamp_now || !d->vt->queue_timestamp_now(d, q, &now) || !now)
        now = fallback_ticks;
    char name[sizeof d->caps.device_name + 32];
    snprintf(name, sizeof name, "%s %s (%s)", d->caps.device_name, k_queue_names[q],
             de_gpu_backend_name(d->caps.backend));
    const u32 ctx =
        de__prof_gpu_context(name, (i64)now, (f32)(1e9 / (f64)freq), prof_api(d->caps.backend));
    if (ctx == DE__PROF_GPU_NO_CONTEXT)
        return ctx;
    d->prof_gpu_context[q] = ctx + 1u;
    d->prof_gpu_span[q] = span;
    return ctx;
}

#endif /* DE_PROFILE */

void de_gpu_profile_zones(de_gpu_device *d, de_gpu_queue q, const de_gpu_zone *zones,
                          u32 zone_count, const u64 *ticks, u32 tick_count) {
#ifdef DE_PROFILE
    if (!d || (u32)q >= DE_QUEUE_COUNT_ || !zones || !zone_count || !ticks || !tick_count)
        return;
    const u32 span = de__prof_gpu_span();
    if (!span)
        return; /* the profiler is not running */
    u64 first = 0;
    for (u32 i = 0; i < zone_count && !first; ++i)
        if (zones[i].begin < tick_count && tick_ok(ticks[zones[i].begin]))
            first = ticks[zones[i].begin];
    if (!first)
        return;
    de_mutex_lock(&d->submit_lock[q]);
    const u32 ctx = queue_context(d, q, span, first);
    if (ctx != DE__PROF_GPU_NO_CONTEXT) {
        /* Open zones as a stack: a region at depth n closes every open region at depth >= n
         * first. A skipped region (bad indices or ticks) skips its children too. */
        enum { MAX_DEPTH = 64 };
        u64 end_tick[MAX_DEPTH];
        u32 open = 0;      /* zones sent and not yet ended */
        u32 skip_at = ~0u; /* depth of a skipped region whose children are skipped */
        for (u32 i = 0; i < zone_count; ++i) {
            const de_gpu_zone *z = &zones[i];
            u32 depth = z->depth < MAX_DEPTH ? z->depth : MAX_DEPTH - 1u;
            if (skip_at != ~0u) {
                if (depth > skip_at)
                    continue;
                skip_at = ~0u;
            }
            if (depth > open)
                depth = open; /* a level was left out: nest under the innermost open zone */
            while (open > depth)
                de__prof_gpu_zone_end(ctx, (i64)end_tick[--open]);
            if (z->begin >= tick_count || z->end >= tick_count || !tick_ok(ticks[z->begin]) ||
                !tick_ok(ticks[z->end])) {
                skip_at = depth;
                continue;
            }
            u64 b = ticks[z->begin], e = ticks[z->end];
            if (e < b)
                e = b;
            de__prof_gpu_zone_begin(ctx, z->name ? z->name : "", z->color, (i64)b);
            end_tick[open++] = e;
        }
        while (open > 0)
            de__prof_gpu_zone_end(ctx, (i64)end_tick[--open]);
    }
    de_mutex_unlock(&d->submit_lock[q]);
#else
    (void)d;
    (void)q;
    (void)zones;
    (void)zone_count;
    (void)ticks;
    (void)tick_count;
#endif
}
