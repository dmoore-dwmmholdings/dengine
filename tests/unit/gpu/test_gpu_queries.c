/* test_gpu_queries.c — timestamps, readback, debug markers and object names on every backend
 * (DENG-326; SPEC §12.1, §12.3, §18.1, §18.3). Executable test_gpu_queries, CTest names
 * gpu_queries.queries.<test>.
 *
 * Front-end (null backend, every host):
 *   - query_null_backend: query heap descs, the recording checks (index, render pass, queue,
 *     resolve range and alignment), the marker stack (names, colours, path, depth past the kept
 *     levels, long names), the render-pass marker rule, default "<kind>#<slot>" names.
 *   - timestamp_to_ms: ticks to milliseconds, per-queue frequencies from the backend.
 *   - profile_zones: GPU zones reach a profiler context per queue (DE_PROFILE builds with Tracy).
 * Backends (skip without the device, the shader importer or the feature):
 *   - timestamps_<backend>: T0, one dispatch of DENG-323's compute.slang over 2M elements, T1,
 *     eight more, T2, on the graphics and the compute queue; resolved twice (two offsets) into a
 *     READBACK buffer: T0 < T1 < T2, positive milliseconds. Then a copy-queue heap around an
 *     8 MiB copy (skipped with the reason where the device cannot time copies).
 *   - markers_nested_<backend>: nested markers around a dispatch and inside a render pass; the
 *     list runs with the debug layer clean. Metal: a programmatic capture (.gputrace) holds the
 *     group names (needs MTL_CAPTURE_ENABLED=1 before the first device: the test sets it, so it
 *     needs its own process, as ctest runs it).
 *   - object_names_vk: a validation error forced on a named buffer inside nested markers: the
 *     layer's message and objects name the buffer and its command-buffer labels are the markers.
 *   - object_names_metal / object_names_d3d12: the native objects carry the names.
 * Vulkan runs on the best Base-tier device or adapter 1 forced (MoltenVK), with the validation
 * layer when installed; D3D12 on the best adapter (CI: the vendored WARP) with the debug layer;
 * Metal with API validation (MTL_DEBUG_LAYER=1). Each requires zero validation errors.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* setenv */
#endif
#include "de_test.h"

#include <dengine/de_debug.h>
#include <dengine/de_gpu.h>
#include <dengine/de_platform.h>

#include "gpu/gpu_backend.h"
#include "pipeline_shaders.h"

#ifdef DE_PROFILE
#include "core/prof/prof_internal.h"
#endif

#if DE_HAS_GPU_VULKAN
#include "gpu/vulkan/vk_internal.h"
#endif
#if DE_HAS_GPU_METAL
#include "gpu/metal/mtl_internal.h"

#include <dirent.h>
#endif
#if defined(_WIN32) && DE_HAS_GPU_D3D12
#include "gpu/d3d12/d3d12_agility_exports.h"
#include "gpu/d3d12/d3d12_internal.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WAIT_NS ((u64)30 * 1000 * 1000 * 1000)

static const char *errchain(char *buf, size_t cap) {
    size_t used = 0;
    buf[0] = '\0';
    for (const de_error *e = de_error_last(); e && used + 1 < cap; e = e->cause) {
        int w = snprintf(buf + used, cap - used, "%s%s", used ? " <- " : "", e->message);
        if (w < 0)
            break;
        used += (size_t)w < cap - used ? (size_t)w : cap - used - 1;
    }
    return buf;
}

static bool error_has(const char *text) {
    for (const de_error *e = de_error_last(); e; e = e->cause)
        if (strstr(e->message, text))
            return true;
    return false;
}

static void set_env(const char *name, const char *value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

/* ---------- Fixture ---------- */

typedef struct q_fx {
    de_tracking_allocator tracker;
    de_allocator alloc;
    de_gpu_device *d;
    de_gpu_backend backend;
    bool expect_errors; /* the test forces validation errors on purpose */
    char skip[1400];
    char err[1100];
} q_fx;

static void q_fx_setup(q_fx *f) {
    f->alloc = de_tracking_allocator_init(&f->tracker, NULL, DE_MEM_RENDER);
}

/* Validation errors of the device's layer (0 when it has none). */
static u32 validation_errors(de_gpu_device *d) {
    switch (de_gpu_get_caps(d)->backend) {
#if DE_HAS_GPU_VULKAN
    case DE_GPU_VULKAN: {
        u32 warnings = 0;
        return de__gpu_vk_validation_messages(d, &warnings);
    }
#endif
#if defined(_WIN32) && DE_HAS_GPU_D3D12
    case DE_GPU_D3D12:
        return de__d3d12_debug_error_count(d);
#endif
    default:
        return 0;
    }
}

static void q_fx_teardown(q_fx *f) {
    if (f->d) {
        de_gpu_wait_idle(f->d);
        if (!f->expect_errors)
            EXPECT_EQ(validation_errors(f->d), 0u);
        de_gpu_device_destroy(f->d);
        f->d = NULL;
    }
    de_tracking_stats st = de_tracking_allocator_stats(&f->tracker);
    EXPECT_EQ(st.blocks_live, 0u);
    EXPECT_EQ(st.bytes_live, 0u);
}

static de__shader_target target_of(de_gpu_backend b) {
    switch (b) {
    case DE_GPU_VULKAN:
        return DE__SHADER_TARGET_SPIRV;
    case DE_GPU_D3D12:
        return DE__SHADER_TARGET_DXIL;
    case DE_GPU_METAL:
        return DE__SHADER_TARGET_METALLIB;
    default:
        return DE__SHADER_TARGET_NONE;
    }
}

/* The shared S10 shaders for the backend, or SKIP. */
static void need_shaders(q_fx *f, de_gpu_backend b) {
    char why[300];
    if (!de_pipe_test_cook(target_of(b), why, sizeof why)) {
        snprintf(f->skip, sizeof f->skip, "shared pipeline shaders not cooked: %s", why);
        SKIP(f->skip);
    }
}

/* A device of backend `b` with its debug layer, or SKIP. Vulkan falls back to adapter 1 (MoltenVK
 * is not Base-tier capable). */
static void open_backend(q_fx *f, de_gpu_backend b) {
    if (b == DE_GPU_METAL)
        set_env("MTL_DEBUG_LAYER", "1"); /* before the first Metal device of the process */
    de_gpu_device_desc desc = {.backend = b,
                               .debug_layer = true,
                               .gpu_validation = true, /* Vulkan: synchronization validation */
                               .allocator = &f->alloc,
                               .pipeline_cache_dir = ""};
    de_result r = de_gpu_device_create(&desc, &f->d);
    if (r == DE_ERR_UNSUPPORTED && b == DE_GPU_VULKAN) {
        desc.adapter = 1;
        r = de_gpu_device_create(&desc, &f->d);
    }
    if (r != DE_OK) {
        snprintf(f->skip, sizeof f->skip, "no usable %s device: %s", de_gpu_backend_name(b),
                 errchain(f->err, sizeof f->err));
        f->d = NULL;
        SKIP(f->skip);
    }
    f->backend = b;
    printf("device: \"%s\" (%s), caps.timestamp_frequency %llu Hz\n",
           de_gpu_get_caps(f->d)->device_name, de_gpu_backend_name(b),
           (unsigned long long)de_gpu_get_caps(f->d)->timestamp_frequency);
#if defined(_WIN32) && DE_HAS_GPU_D3D12
    if (b == DE_GPU_D3D12) {
        const char *why = NULL;
        if (!de__d3d12_pipelines_supported(f->d, &why)) {
            snprintf(f->skip, sizeof f->skip, "this D3D12 device has no pipelines (%s)", why);
            SKIP(f->skip);
        }
    }
#endif
}

static de_gpu_pipeline compute_pipeline(q_fx *f) {
    de_pipe_test_shader cs;
    if (!de_pipe_test_get(target_of(f->backend), "compute.slang", "cs_main", &cs)) {
        printf("compute.slang:cs_main was not cooked\n");
        ASSERT_TRUE(false);
    }
    de_gpu_pipeline_desc pd = {
        .kind = DE_PIPE_COMPUTE,
        .cs = cs.blob,
        .push_constant_bytes = cs.push_constant_bytes,
        .threadgroup_size = {cs.threadgroup[0], cs.threadgroup[1], cs.threadgroup[2]},
        .name = "queries compute"};
    de_gpu_pipeline p = de_gpu_pipeline_create(f->d, &pd);
    if (!DE_HANDLE_VALID(p))
        printf("compute pipeline: %s\n", errchain(f->err, sizeof f->err));
    ASSERT_TRUE(DE_HANDLE_VALID(p));
    return p;
}

static de_gpu_barrier buf_barrier(de_gpu_buffer b, u32 before, u32 after) {
    de_gpu_barrier x;
    memset(&x, 0, sizeof x);
    x.buffer = b;
    x.before = before;
    x.after = after;
    return x;
}

static de_gpu_barrier tex_barrier(de_gpu_texture t, u32 before, u32 after) {
    de_gpu_barrier x;
    memset(&x, 0, sizeof x);
    x.texture = t;
    x.before = before;
    x.after = after;
    return x;
}

static void submit_wait(de_gpu_device *d, de_gpu_queue q, de_gpu_cmd *c) {
    de_gpu_fence fence = de_gpu_fence_create(d, 0);
    ASSERT_TRUE(DE_HANDLE_VALID(fence));
    de_gpu_submit s = {.cmds = &c, .cmd_count = 1, .signal = fence, .signal_value = 1};
    ASSERT_EQ(de_gpu_queue_submit(d, q, &s), DE_OK);
    ASSERT_EQ(de_gpu_fence_wait(d, fence, 1, WAIT_NS), DE_OK);
    de_gpu_fence_destroy(d, fence);
}

/* ---------- Front-end: null backend ---------- */

static de_gpu_device *null_device(q_fx *f) {
    de_gpu_device *d = NULL;
    de_gpu_device_desc desc = {.backend = DE_GPU_NULL, .allocator = &f->alloc};
    if (de_gpu_device_create(&desc, &d) != DE_OK)
        return NULL;
    return d;
}

TEST_F(queries, query_null_backend, q_fx) {
    f->d = null_device(f);
    ASSERT_TRUE(f->d != NULL);
    de_gpu_device *d = f->d;

    /* Descs. */
    de_gpu_query_heap h = de_gpu_query_heap_create(d, &(de_gpu_query_heap_desc){.count = 4});
    ASSERT_TRUE(DE_HANDLE_VALID(h));
    de_error_clear();
    EXPECT_FALSE(DE_HANDLE_VALID(de_gpu_query_heap_create(
        d, &(de_gpu_query_heap_desc){.count = 4, .queue = (de_gpu_queue)7, .name = "bad"})));
    EXPECT_TRUE(error_has("queue 7 is not a de_gpu_queue value"));
    EXPECT_FALSE(DE_HANDLE_VALID(
        de_gpu_query_heap_create(d, &(de_gpu_query_heap_desc){.count = DE_GPU_MAX_QUERIES + 1})));
    de_gpu_query_heap hc = de_gpu_query_heap_create(
        d, &(de_gpu_query_heap_desc){.count = 2, .queue = DE_QUEUE_COPY, .name = "copy heap"});
    ASSERT_TRUE(DE_HANDLE_VALID(hc));

#if DE__GPU_DEFAULT_NAMES
    /* Unnamed objects: "<kind>#<slot>". */
    char want[DE__GPU_NAME_MAX];
    snprintf(want, sizeof want, "query heap#%u", h.id & (DE__GPU_POOL_MAX_SLOTS - 1u));
    EXPECT_STREQ(de__gpu_get_query_heap(d, h)->name, want);
    de_gpu_buffer ub = de_gpu_buffer_create(
        d,
        &(de_gpu_buffer_desc){.size = 256, .usage = DE_USAGE_COPY_DST, .memory = DE_MEM_READBACK});
    ASSERT_TRUE(DE_HANDLE_VALID(ub));
    snprintf(want, sizeof want, "buffer#%u", ub.id & (DE__GPU_POOL_MAX_SLOTS - 1u));
    EXPECT_STREQ(de__gpu_get_buffer(d, ub)->name, want);
    de_gpu_buffer_destroy(d, ub);
#endif
    EXPECT_STREQ(de__gpu_get_query_heap(d, hc)->name, "copy heap");

    de_gpu_buffer rb = de_gpu_buffer_create(
        d, &(de_gpu_buffer_desc){
               .size = 64, .usage = DE_USAGE_COPY_DST, .memory = DE_MEM_READBACK, .name = "rb"});
    de_gpu_buffer nocopy = de_gpu_buffer_create(
        d, &(de_gpu_buffer_desc){.size = 64, .usage = DE_USAGE_STORAGE, .name = "nocopy"});
    de_gpu_texture rt =
        de_gpu_texture_create(d, &(de_gpu_texture_desc){.dim = DE_TEX_2D,
                                                        .format = DE_FMT_RGBA8_UNORM,
                                                        .width = 16,
                                                        .height = 16,
                                                        .usage = DE_USAGE_RENDER_TARGET,
                                                        .name = "rt"});
    ASSERT_TRUE(DE_HANDLE_VALID(rb) && DE_HANDLE_VALID(nocopy) && DE_HANDLE_VALID(rt));

    de_gpu_cmd *c = de_gpu_cmd_begin(d, DE_QUEUE_GRAPHICS, "null list");
    ASSERT_TRUE(c != NULL);
    de_error_clear();
    de_gpu_cmd_timestamp(c, h, 0);
    de_gpu_cmd_timestamp(c, h, 3);
    de_gpu_cmd_resolve_queries(c, h, 0, 4, rb, 0);
    de_gpu_cmd_resolve_queries(c, h, 1, 2, rb, 8);
    EXPECT_TRUE(de_error_last() == NULL);

    /* Marker stack: names, colours, path. */
    de_gpu_cmd_marker_push(c, "frame", 0xFF102030u); /* alpha bits are dropped */
    de_gpu_cmd_marker_push(c, "shadows", 0x00FF00u);
    EXPECT_EQ(de__gpu_cmd_marker_depth(c), 2u);
    EXPECT_STREQ(de__gpu_cmd_marker_name(c, 0), "frame");
    EXPECT_STREQ(de__gpu_cmd_marker_name(c, 1), "shadows");
    EXPECT_STREQ(de__gpu_cmd_marker_name(c, 2), "");
    EXPECT_EQ(de__gpu_cmd_marker_color(c, 0), 0x102030u);
    EXPECT_EQ(de__gpu_cmd_marker_color(c, 1), 0x00FF00u);
    char path[256];
    EXPECT_EQ(de__gpu_cmd_marker_path(c, path, sizeof path), strlen("frame/shadows"));
    EXPECT_STREQ(path, "frame/shadows");
    char cut[8];
    de__gpu_cmd_marker_path(c, cut, sizeof cut);
    EXPECT_STREQ(cut, "frame/s"); /* cut, terminated */
    de_gpu_cmd_marker_pop(c);
    de_gpu_cmd_marker_push(c, "lights", 0);
    EXPECT_STREQ(de__gpu_cmd_marker_name(c, 1), "lights");
    de_gpu_cmd_marker_pop(c);
    de_gpu_cmd_marker_pop(c);
    EXPECT_EQ(de__gpu_cmd_marker_depth(c), 0u);
    EXPECT_EQ(c->marker_text_used, 0u);

    /* Deeper than the kept levels, and names longer than the text buffer: counted, cut, never an
     * overflow; popping restores the stack exactly. */
    static char long_name[3000];
    memset(long_name, 'x', sizeof long_name - 1);
    for (u32 i = 0; i < DE__GPU_MARKER_LEVELS + 8u; ++i)
        de_gpu_cmd_marker_push(c, i == 1 ? long_name : "level", i);
    EXPECT_EQ(de__gpu_cmd_marker_depth(c), DE__GPU_MARKER_LEVELS + 8u);
    EXPECT_STREQ(de__gpu_cmd_marker_name(c, 0), "level");
    EXPECT_EQ(strlen(de__gpu_cmd_marker_name(c, 1)), (size_t)DE__GPU_MARKER_TEXT - 7u);
    EXPECT_STREQ(de__gpu_cmd_marker_name(c, 2), ""); /* no room left */
    EXPECT_STREQ(de__gpu_cmd_marker_name(c, DE__GPU_MARKER_LEVELS), "");
    EXPECT_LE(c->marker_text_used, DE__GPU_MARKER_TEXT);
    for (u32 i = 0; i < DE__GPU_MARKER_LEVELS + 7u; ++i)
        de_gpu_cmd_marker_pop(c);
    EXPECT_STREQ(de__gpu_cmd_marker_name(c, 0), "level");
    de_gpu_cmd_marker_pop(c);
    EXPECT_EQ(c->marker_text_used, 0u);
    EXPECT_TRUE(de_error_last() == NULL);

#if DE__GPU_VALIDATE_COMMANDS
    /* Recording checks (Debug/Dev). */
    de_error_clear();
    de_gpu_cmd_timestamp(c, h, 4);
    EXPECT_TRUE(error_has("query 4, but"));
    de_error_clear();
    de_gpu_cmd_timestamp(c, hc, 0);
    EXPECT_TRUE(error_has("is for the copy queue, the list is on the graphics/compute queue"));
    de_error_clear();
    de_gpu_cmd_resolve_queries(c, h, 2, 3, rb, 0);
    EXPECT_TRUE(error_has("queries [2, +3) are outside"));
    de_error_clear();
    de_gpu_cmd_resolve_queries(c, h, 0, 1, rb, 4);
    EXPECT_TRUE(error_has("misaligned"));
    de_error_clear();
    de_gpu_cmd_resolve_queries(c, h, 0, 8, rb, 8);
    EXPECT_TRUE(error_has("outside"));
    de_error_clear();
    de_gpu_cmd_resolve_queries(c, h, 0, 1, nocopy, 0);
    EXPECT_TRUE(error_has("lacks DE_USAGE_COPY_DST"));
    de_error_clear();
    de_gpu_cmd_marker_pop(c);
    EXPECT_TRUE(error_has("no marker_push to pop"));

    /* Render passes: no timestamps; markers opened inside close inside. */
    de_gpu_attachment att = {.texture = rt, .clear = true, .store = true};
    de_gpu_cmd_marker_push(c, "outer", 0);
    de_gpu_cmd_begin_render(c, &att, 1, NULL);
    de_error_clear();
    de_gpu_cmd_timestamp(c, h, 0);
    EXPECT_TRUE(error_has("timestamp inside a render pass"));
    de_error_clear();
    de_gpu_cmd_marker_pop(c);
    EXPECT_TRUE(error_has("pushed before the render pass"));
    EXPECT_EQ(de__gpu_cmd_marker_depth(c), 1u); /* refused: still open */
    de_gpu_cmd_marker_push(c, "inner", 0);
    de_error_clear();
    de_gpu_cmd_end_render(c);
    EXPECT_TRUE(error_has("still open"));
    EXPECT_FALSE(c->in_render); /* the pass ends anyway */
    de_gpu_cmd_marker_pop(c);
    de_gpu_cmd_marker_pop(c);
    de_gpu_cmd_begin_render(c, &att, 1, NULL);
    de_gpu_cmd_marker_push(c, "inside", 0);
    de_gpu_cmd_marker_pop(c);
    de_error_clear();
    de_gpu_cmd_end_render(c);
    EXPECT_TRUE(de_error_last() == NULL);

    /* An open marker blocks the submit. */
    de_gpu_cmd_marker_push(c, "left open", 0);
    de_gpu_submit s = {.cmds = &c, .cmd_count = 1};
    EXPECT_EQ(de_gpu_queue_submit(d, DE_QUEUE_GRAPHICS, &s), DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(error_has("open marker_push"));
    de_gpu_cmd_marker_pop(c);
#endif
    de_gpu_submit s2 = {.cmds = &c, .cmd_count = 1};
    EXPECT_EQ(de_gpu_queue_submit(d, DE_QUEUE_GRAPHICS, &s2), DE_OK);

    /* A copy list takes the copy heap. */
    de_gpu_cmd *cc = de_gpu_cmd_begin(d, DE_QUEUE_COPY, "copy list");
    ASSERT_TRUE(cc != NULL);
    de_error_clear();
    de_gpu_cmd_timestamp(cc, hc, 1);
    de_gpu_cmd_resolve_queries(cc, hc, 0, 2, rb, 0);
    EXPECT_TRUE(de_error_last() == NULL);
#if DE__GPU_VALIDATE_COMMANDS
    de_gpu_cmd_timestamp(cc, h, 0);
    EXPECT_TRUE(error_has("is for the graphics/compute queue, the list is on the copy queue"));
#endif
    de_gpu_submit s3 = {.cmds = &cc, .cmd_count = 1};
    EXPECT_EQ(de_gpu_queue_submit(d, DE_QUEUE_COPY, &s3), DE_OK);

    de_gpu_texture_destroy(d, rt);
    de_gpu_buffer_destroy(d, nocopy);
    de_gpu_buffer_destroy(d, rb);
    de_gpu_query_heap_destroy(d, hc);
    de_gpu_query_heap_destroy(d, h);
}

/* A null backend whose copy queue counts at another rate and that knows the GPU time. */
static u64 fake_frequency(de_gpu_device *d, de_gpu_queue q) {
    (void)d;
    return q == DE_QUEUE_COPY ? 24000000u : 0u; /* 0: caps.timestamp_frequency */
}
static bool fake_now(de_gpu_device *d, de_gpu_queue q, u64 *ticks) {
    (void)d;
    *ticks = 5000u + (u64)q;
    return true;
}

TEST_F(queries, timestamp_to_ms, q_fx) {
    EXPECT_EQ(de_gpu_timestamp_frequency(NULL, DE_QUEUE_GRAPHICS), 0u);
    EXPECT_TRUE(de_gpu_timestamp_to_ms(NULL, DE_QUEUE_GRAPHICS, 1000) == 0.0);

    f->d = null_device(f);
    ASSERT_TRUE(f->d != NULL);
    /* The null backend counts nanoseconds. */
    EXPECT_EQ(de_gpu_timestamp_frequency(f->d, DE_QUEUE_GRAPHICS), 1000000000u);
    EXPECT_FLOAT_EQ(de_gpu_timestamp_to_ms(f->d, DE_QUEUE_GRAPHICS, 2500000u), 2.5, 1e-12);
    EXPECT_FLOAT_EQ(de_gpu_timestamp_to_ms(f->d, DE_QUEUE_COMPUTE, 1u), 1e-6, 1e-15);
    EXPECT_TRUE(de_gpu_timestamp_to_ms(f->d, DE_QUEUE_COPY, 0) == 0.0);
    de_gpu_device_destroy(f->d);
    f->d = NULL;

    /* Per-queue frequencies from the backend (D3D12 asks each queue). */
    static de__gpu_backend_vtable fake;
    fake = de__gpu_null_vtable;
    fake.queue_timestamp_frequency = fake_frequency;
    de__gpu_register_backend(DE_GPU_NULL, &fake);
    f->d = null_device(f);
    de__gpu_register_backend(DE_GPU_NULL, NULL);
    ASSERT_TRUE(f->d != NULL);
    EXPECT_EQ(de_gpu_timestamp_frequency(f->d, DE_QUEUE_COPY), 24000000u);
    EXPECT_EQ(de_gpu_timestamp_frequency(f->d, DE_QUEUE_GRAPHICS), 1000000000u);
    EXPECT_FLOAT_EQ(de_gpu_timestamp_to_ms(f->d, DE_QUEUE_COPY, 24000u), 1.0, 1e-12);
    EXPECT_FLOAT_EQ(de_gpu_timestamp_to_ms(f->d, DE_QUEUE_GRAPHICS, 24000u), 0.024, 1e-12);
    /* Out of range: the device-wide frequency. */
    EXPECT_EQ(de_gpu_timestamp_frequency(f->d, (de_gpu_queue)9), 1000000000u);
}

TEST_F(queries, profile_zones, q_fx) {
    static de__gpu_backend_vtable fake;
    fake = de__gpu_null_vtable;
    fake.queue_timestamp_now = fake_now;
    de__gpu_register_backend(DE_GPU_NULL, &fake);
    f->d = null_device(f);
    de__gpu_register_backend(DE_GPU_NULL, NULL);
    ASSERT_TRUE(f->d != NULL);
    de_gpu_device *d = f->d;

    const u64 ticks[6] = {1000, 2000, 1100, 1500, 0, UINT64_MAX};
    const de_gpu_zone zones[] = {
        {"frame", 0xFF0000u, 0, 1, 0},      {"shadows", 0, 2, 3, 1},
        {"unwritten", 0, 4, 5, 1}, /* skipped, with its child */
        {"child of unwritten", 0, 2, 3, 2}, {"bad index", 0, 0, 99, 1},
    };
    const u32 nz = (u32)(sizeof zones / sizeof zones[0]);

    /* Nothing without a running profiler (and bad arguments are ignored). */
    de_gpu_profile_zones(d, DE_QUEUE_GRAPHICS, zones, nz, ticks, 6);
    de_gpu_profile_zones(NULL, DE_QUEUE_GRAPHICS, zones, nz, ticks, 6);
    de_gpu_profile_zones(d, (de_gpu_queue)5, zones, nz, ticks, 6);
    de_gpu_profile_zones(d, DE_QUEUE_GRAPHICS, NULL, nz, ticks, 6);
    EXPECT_EQ(d->prof_gpu_context[DE_QUEUE_GRAPHICS], 0u);

    if (de_prof_init() != DE_OK) {
        SKIP("no profiler backend in this build (DE_PROFILE with DE_TRACY): GPU zones compile to "
             "nothing, checked above");
    }
#ifdef DE_PROFILE
    de_gpu_profile_zones(d, DE_QUEUE_GRAPHICS, zones, nz, ticks, 6);
    const u32 ctx = d->prof_gpu_context[DE_QUEUE_GRAPHICS];
    EXPECT_NE(ctx, 0u); /* a context for the queue, in this span */
    EXPECT_EQ(d->prof_gpu_span[DE_QUEUE_GRAPHICS], de__prof_gpu_span());
    de_gpu_profile_zones(d, DE_QUEUE_GRAPHICS, zones, nz, ticks, 6);
    EXPECT_EQ(d->prof_gpu_context[DE_QUEUE_GRAPHICS], ctx); /* reused */
    EXPECT_EQ(d->prof_gpu_context[DE_QUEUE_COMPUTE], 0u);
    de_gpu_profile_zones(d, DE_QUEUE_COMPUTE, zones, 1, ticks, 6);
    EXPECT_NE(d->prof_gpu_context[DE_QUEUE_COMPUTE], 0u);
    EXPECT_NE(d->prof_gpu_context[DE_QUEUE_COMPUTE], ctx);
    /* Only unwritten ticks: no context is made. */
    de_gpu_profile_zones(d, DE_QUEUE_COPY, &zones[2], 1, ticks, 6);
    EXPECT_EQ(d->prof_gpu_context[DE_QUEUE_COPY], 0u);
    de_prof_shutdown();
    /* A new span makes a new context. */
    ASSERT_EQ(de_prof_init(), DE_OK);
    de_gpu_profile_zones(d, DE_QUEUE_GRAPHICS, zones, nz, ticks, 6);
    EXPECT_NE(d->prof_gpu_context[DE_QUEUE_GRAPHICS], ctx);
    de_prof_shutdown();
#endif
}

/* ---------- Timestamps on a backend ---------- */

#define TS_ELEMS (1u << 21) /* 2M u32 = 8 MiB per buffer; 32768 groups of 64 */

/* True on MoltenVK (a Vulkan portability driver): it writes timestamps when the command buffer
 * completes, so a resolve in the same list misses the ones after the last encoder (they read 0).
 * The tests resolve those in the next list there; conformant drivers, D3D12 and Metal resolve in
 * the same list. */
static bool late_timestamps(q_fx *f) {
#if DE_HAS_GPU_VULKAN
    if (f->backend == DE_GPU_VULKAN && de__vk(f->d)->phys.ext.portability_subset)
        return true;
#endif
    (void)f;
    return false;
}

/* The list to record the resolve in: `c`, or after submitting it a new one (late_timestamps). */
static de_gpu_cmd *resolve_list(q_fx *f, de_gpu_queue q, de_gpu_cmd *c) {
    if (!late_timestamps(f))
        return c;
    submit_wait(f->d, q, c);
    printf("portability driver: resolving in the next list\n");
    c = de_gpu_cmd_begin(f->d, q, "queries resolve");
    if (!c)
        printf("second list: %s\n", errchain(f->err, sizeof f->err));
    return c;
}

/* T0, one dispatch, T1, eight dispatches, T2 on queue `q`; resolved at offset 0 (all three) and
 * at offset 32 (T1, T2). SKIPs when the device has no timestamps. */
static void timestamps_on_queue(q_fx *f, de_gpu_pipeline pipe, de_gpu_queue q) {
    de_gpu_device *d = f->d;
    de_gpu_query_heap heap = de_gpu_query_heap_create(
        d, &(de_gpu_query_heap_desc){.count = 4, .queue = q, .name = "queries timestamps"});
    if (!DE_HANDLE_VALID(heap)) {
        printf("query heap: %s\n", errchain(f->err, sizeof f->err));
        if (de_error_last() && de_error_last()->result == DE_ERR_UNSUPPORTED) {
            snprintf(f->skip, sizeof f->skip, "no timestamps: %s", f->err);
            SKIP(f->skip);
        }
        ASSERT_TRUE(false);
    }
    de_gpu_buffer src = de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = TS_ELEMS * 4u,
                                                                      .usage = DE_USAGE_STORAGE,
                                                                      .stride = 4,
                                                                      .name = "queries src"});
    de_gpu_buffer dst = de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = TS_ELEMS * 4u,
                                                                      .usage = DE_USAGE_STORAGE,
                                                                      .stride = 4,
                                                                      .name = "queries dst"});
    de_gpu_buffer rb = de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = 64,
                                                                     .usage = DE_USAGE_COPY_DST,
                                                                     .memory = DE_MEM_READBACK,
                                                                     .name = "queries readback"});
    ASSERT_TRUE(DE_HANDLE_VALID(src) && DE_HANDLE_VALID(dst) && DE_HANDLE_VALID(rb));
    u64 *res = (u64 *)de_gpu_buffer_map(d, rb);
    ASSERT_TRUE(res != NULL);
    memset(res, 0, 64);

    de_error_clear();
    de_gpu_cmd *c = de_gpu_cmd_begin(d, q, "queries timestamps");
    ASSERT_TRUE(c != NULL);
    de_gpu_barrier b[3] = {buf_barrier(src, DE_ACCESS_NONE, DE_ACCESS_SHADER_READ),
                           buf_barrier(dst, DE_ACCESS_NONE, DE_ACCESS_SHADER_WRITE),
                           buf_barrier(rb, DE_ACCESS_NONE, DE_ACCESS_COPY_DST)};
    de_gpu_cmd_barriers(c, b, 3);
    de_gpu_cmd_set_pipeline(c, pipe);
    const u32 params[4] = {de_gpu_buffer_bindless(d, src), de_gpu_buffer_bindless(d, dst), TS_ELEMS,
                           DE_PIPE_TEST_COMPUTE_SEED};
    de_gpu_cmd_push_constants(c, params, sizeof params);
    de_gpu_cmd_timestamp(c, heap, 0);
    de_gpu_cmd_dispatch(c, TS_ELEMS / 64u, 1, 1);
    de_gpu_cmd_timestamp(c, heap, 1);
    for (u32 k = 0; k < 8; ++k) {
        de_gpu_barrier w = buf_barrier(dst, DE_ACCESS_SHADER_WRITE, DE_ACCESS_SHADER_WRITE);
        de_gpu_cmd_barriers(c, &w, 1);
        de_gpu_cmd_dispatch(c, TS_ELEMS / 64u, 1, 1);
    }
    de_gpu_cmd_timestamp(c, heap, 2);
    c = resolve_list(f, q, c);
    de_gpu_cmd_resolve_queries(c, heap, 0, 3, rb, 0);
    de_gpu_cmd_resolve_queries(c, heap, 1, 2, rb, 32);
    EXPECT_TRUE(de_error_last() == NULL);
    if (de_error_last())
        printf("recording: %s\n", errchain(f->err, sizeof f->err));
    submit_wait(d, q, c);

    const f64 first = de_gpu_timestamp_to_ms(d, q, res[1] - res[0]);
    const f64 rest = de_gpu_timestamp_to_ms(d, q, res[2] - res[1]);
    printf("%s queue: T0 %llu, T1 %llu, T2 %llu ticks at %llu Hz: one dispatch %.4f ms, eight "
           "%.4f ms\n",
           q == DE_QUEUE_GRAPHICS ? "graphics" : "compute", (unsigned long long)res[0],
           (unsigned long long)res[1], (unsigned long long)res[2],
           (unsigned long long)de_gpu_timestamp_frequency(d, q), first, rest);
    EXPECT_NE(res[0], 0u);
    EXPECT_NE(res[0], UINT64_MAX);
    EXPECT_GT(res[1], res[0]); /* monotonic within the list */
    EXPECT_GT(res[2], res[1]);
    EXPECT_TRUE(first > 0.0 && first < 10000.0);
    EXPECT_TRUE(rest > 0.0 && rest < 10000.0);
    /* The second resolve: the same values at another offset, in order. */
    EXPECT_EQ(res[4], res[1]);
    EXPECT_EQ(res[5], res[2]);

    de_gpu_buffer_destroy(d, rb);
    de_gpu_buffer_destroy(d, dst);
    de_gpu_buffer_destroy(d, src);
    de_gpu_query_heap_destroy(d, heap);
}

/* Two timestamps around an 8 MiB copy on the copy queue, with a copy-queue heap. */
static void timestamps_on_copy_queue(q_fx *f) {
    de_gpu_device *d = f->d;
    de_error_clear();
    de_gpu_query_heap heap = de_gpu_query_heap_create(
        d, &(de_gpu_query_heap_desc){.count = 2, .queue = DE_QUEUE_COPY, .name = "copy times"});
    if (!DE_HANDLE_VALID(heap)) {
        /* Not every device times copies (D3D12 CopyQueueTimestampQueriesSupported, Vulkan
         * transfer families with timestampValidBits 0). */
        printf("copy queue: no timestamps: %s\n", errchain(f->err, sizeof f->err));
        EXPECT_TRUE(de_error_last() && de_error_last()->result == DE_ERR_UNSUPPORTED);
        return;
    }
    de_gpu_buffer a = de_gpu_buffer_create(
        d,
        &(de_gpu_buffer_desc){.size = TS_ELEMS * 4u, .usage = DE_USAGE_COPY_SRC, .name = "copy a"});
    de_gpu_buffer b = de_gpu_buffer_create(
        d,
        &(de_gpu_buffer_desc){.size = TS_ELEMS * 4u, .usage = DE_USAGE_COPY_DST, .name = "copy b"});
    de_gpu_buffer rb = de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = 16,
                                                                     .usage = DE_USAGE_COPY_DST,
                                                                     .memory = DE_MEM_READBACK,
                                                                     .name = "copy readback"});
    ASSERT_TRUE(DE_HANDLE_VALID(a) && DE_HANDLE_VALID(b) && DE_HANDLE_VALID(rb));
    u64 *res = (u64 *)de_gpu_buffer_map(d, rb);
    memset(res, 0, 16);
    de_error_clear();
    de_gpu_cmd *c = de_gpu_cmd_begin(d, DE_QUEUE_COPY, "queries copy");
    ASSERT_TRUE(c != NULL);
    de_gpu_barrier bs[3] = {buf_barrier(a, DE_ACCESS_NONE, DE_ACCESS_COPY_SRC),
                            buf_barrier(b, DE_ACCESS_NONE, DE_ACCESS_COPY_DST),
                            buf_barrier(rb, DE_ACCESS_NONE, DE_ACCESS_COPY_DST)};
    de_gpu_cmd_barriers(c, bs, 3);
    de_gpu_cmd_timestamp(c, heap, 0);
    de_gpu_cmd_copy_buffer(c, b, 0, a, 0, TS_ELEMS * 4u);
    de_gpu_cmd_timestamp(c, heap, 1);
    c = resolve_list(f, DE_QUEUE_COPY, c);
    de_gpu_cmd_resolve_queries(c, heap, 0, 2, rb, 0);
    EXPECT_TRUE(de_error_last() == NULL);
    submit_wait(d, DE_QUEUE_COPY, c);
    printf("copy queue: 8 MiB copy %.4f ms (%llu Hz)\n",
           de_gpu_timestamp_to_ms(d, DE_QUEUE_COPY, res[1] - res[0]),
           (unsigned long long)de_gpu_timestamp_frequency(d, DE_QUEUE_COPY));
    EXPECT_NE(res[0], 0u);
    EXPECT_GT(res[1], res[0]);
    de_gpu_buffer_destroy(d, rb);
    de_gpu_buffer_destroy(d, b);
    de_gpu_buffer_destroy(d, a);
    de_gpu_query_heap_destroy(d, heap);
}

static void run_timestamps(q_fx *f, de_gpu_backend backend) {
    need_shaders(f, backend);
    open_backend(f, backend);
    de_gpu_pipeline pipe = compute_pipeline(f);
    timestamps_on_queue(f, pipe, DE_QUEUE_GRAPHICS);
    timestamps_on_queue(f, pipe, DE_QUEUE_COMPUTE);
    timestamps_on_copy_queue(f);
    de_gpu_pipeline_destroy(f->d, pipe);
}

/* ---------- Nested markers on a backend ---------- */

static const char *const k_marker_names[4] = {"de_test_frame", "de_test_compute", "de_test_render",
                                              "de_test_draws"};

/* Nested markers around a dispatch and inside a render pass, run on the graphics queue. */
static void record_markers(q_fx *f, de_gpu_pipeline pipe) {
    de_gpu_device *d = f->d;
    de_gpu_buffer buf = de_gpu_buffer_create(
        d, &(de_gpu_buffer_desc){
               .size = 4096, .usage = DE_USAGE_STORAGE, .stride = 4, .name = "markers buffer"});
    de_gpu_texture rt =
        de_gpu_texture_create(d, &(de_gpu_texture_desc){.dim = DE_TEX_2D,
                                                        .format = DE_FMT_RGBA8_UNORM,
                                                        .width = 64,
                                                        .height = 64,
                                                        .usage = DE_USAGE_RENDER_TARGET,
                                                        .name = "markers target"});
    ASSERT_TRUE(DE_HANDLE_VALID(buf) && DE_HANDLE_VALID(rt));
    de_error_clear();
    de_gpu_cmd *c = de_gpu_cmd_begin(d, DE_QUEUE_GRAPHICS, "markers");
    ASSERT_TRUE(c != NULL);
    de_gpu_cmd_marker_push(c, k_marker_names[0], 0xFF8000u);
    de_gpu_cmd_marker_push(c, k_marker_names[1], 0x00FF00u);
    de_gpu_barrier b = buf_barrier(buf, DE_ACCESS_NONE, DE_ACCESS_SHADER_WRITE);
    de_gpu_cmd_barriers(c, &b, 1);
    de_gpu_cmd_set_pipeline(c, pipe);
    const u32 params[4] = {de_gpu_buffer_bindless(d, buf), de_gpu_buffer_bindless(d, buf), 0, 0};
    de_gpu_cmd_push_constants(c, params, sizeof params);
    de_gpu_cmd_dispatch(c, 1, 1, 1);
    char path[128];
    de__gpu_cmd_marker_path(c, path, sizeof path);
    EXPECT_STREQ(path, "de_test_frame/de_test_compute");
    de_gpu_cmd_marker_pop(c);
    de_gpu_barrier tb = tex_barrier(rt, DE_ACCESS_NONE, DE_ACCESS_RENDER_TARGET);
    de_gpu_cmd_barriers(c, &tb, 1);
    de_gpu_cmd_marker_push(c, k_marker_names[2], 0x0000FFu);
    de_gpu_attachment att = {
        .texture = rt, .clear = true, .clear_color = {0.1f, 0.2f, 0.3f, 1.0f}, .store = true};
    de_gpu_cmd_begin_render(c, &att, 1, NULL);
    de_gpu_cmd_marker_push(c, k_marker_names[3], 0);
    de_gpu_cmd_viewport(c, 0, 0, 64, 64);
    de_gpu_cmd_marker_pop(c);
    de_gpu_cmd_end_render(c);
    de_gpu_cmd_marker_pop(c);
    de_gpu_cmd_marker_pop(c);
    EXPECT_EQ(de__gpu_cmd_marker_depth(c), 0u);
    EXPECT_TRUE(de_error_last() == NULL);
    if (de_error_last())
        printf("recording: %s\n", errchain(f->err, sizeof f->err));
    submit_wait(d, DE_QUEUE_GRAPHICS, c);
    de_gpu_texture_destroy(d, rt);
    de_gpu_buffer_destroy(d, buf);
}

static void run_markers(q_fx *f, de_gpu_backend backend) {
    need_shaders(f, backend);
    open_backend(f, backend);
    de_gpu_pipeline pipe = compute_pipeline(f);
    record_markers(f, pipe);
    record_markers(f, pipe); /* a recycled list: stale per-level state must not leak in */
    de_gpu_pipeline_destroy(f->d, pipe);
}

/* ---------- Metal ---------- */
#if DE_HAS_GPU_METAL

TEST_F(queries, timestamps_metal, q_fx) {
    run_timestamps(f, DE_GPU_METAL);
}

/* True when `file` contains `text` (binary search of the bytes). */
static bool file_has(const char *file, const char *text) {
    FILE *fp = fopen(file, "rb");
    if (!fp)
        return false;
    static char buf[1 << 20];
    const size_t n = fread(buf, 1, sizeof buf, fp);
    fclose(fp);
    const size_t tl = strlen(text);
    for (size_t i = 0; i + tl <= n; ++i)
        if (memcmp(buf + i, text, tl) == 0)
            return true;
    return false;
}

/* True when a file of the .gputrace bundle `dir` contains `text`; lists the files the first
 * time. */
static bool trace_has(const char *dir, const char *text) {
    static bool listed;
    DIR *dp = opendir(dir);
    if (!dp)
        return false;
    bool found = false;
    struct dirent *e;
    while (!found && (e = readdir(dp)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        char path[700];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (!listed)
            printf("  capture file %s\n", e->d_name);
        found = file_has(path, text);
    }
    listed = true;
    closedir(dp);
    return found;
}

TEST_F(queries, markers_nested_metal, q_fx) {
    /* Capture must be allowed before the process's first Metal device. */
    set_env("MTL_CAPTURE_ENABLED", "1");
    need_shaders(f, DE_GPU_METAL);
    open_backend(f, DE_GPU_METAL);
    de_gpu_pipeline pipe = compute_pipeline(f);
    char trace[512];
    snprintf(trace, sizeof trace, "%s/markers-%llu.gputrace", DE_TEST_TMP_DIR,
             (unsigned long long)de_time_now_ns());
    const de_result cap = de__mtl_capture_begin(f->d, trace);
    if (cap != DE_OK)
        printf("no GPU capture (%s): the markers run without it\n",
               errchain(f->err, sizeof f->err));
    record_markers(f, pipe);
    record_markers(f, pipe);
    de_gpu_pipeline_destroy(f->d, pipe);
    if (cap != DE_OK) {
        snprintf(f->skip, sizeof f->skip,
                 "markers recorded and ran; the capture check needs MTL_CAPTURE_ENABLED=1 before "
                 "the process's first Metal device (run the test on its own): %s",
                 f->err);
        SKIP(f->skip);
    }
    de__mtl_capture_end(f->d);
    printf("capture: %s\n", trace);
    /* Metal may still be writing the document after stopCapture (seen once under a loaded
     * ctest -j 8): look again for up to 10 s before deciding. */
    u32 missing = 4;
    for (u32 attempt = 0; attempt < 100 && missing; ++attempt) {
        if (attempt)
            de_sleep_ns((u64)100 * 1000 * 1000);
        missing = 0;
        for (u32 i = 0; i < 4; ++i)
            missing += trace_has(trace, k_marker_names[i]) ? 0u : 1u;
    }
    for (u32 i = 0; i < 4 && missing; ++i)
        if (!trace_has(trace, k_marker_names[i]))
            printf("debug group \"%s\" is not in the capture\n", k_marker_names[i]);
    if (missing && !de_gpu_get_caps(f->d)->base_tier_ok) {
        /* GitHub's paravirtual GPU (below the Base tier) writes a capture whose files do not hold
         * the names as plain bytes; the M-series host's captures do. */
        snprintf(f->skip, sizeof f->skip,
                 "markers recorded and ran; this device's capture (below the Base tier: \"%s\") "
                 "does not store the group names as plain bytes",
                 de_gpu_get_caps(f->d)->device_name);
        SKIP(f->skip);
    }
    EXPECT_EQ(missing, 0u);
}

TEST_F(queries, object_names_metal, q_fx) {
    open_backend(f, DE_GPU_METAL);
    de_gpu_device *d = f->d;
    de_gpu_buffer named = de_gpu_buffer_create(
        d, &(de_gpu_buffer_desc){.size = 256, .usage = DE_USAGE_STORAGE, .name = "de_test_label"});
    de_gpu_buffer unnamed =
        de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = 256, .usage = DE_USAGE_STORAGE});
    de_gpu_texture tex =
        de_gpu_texture_create(d, &(de_gpu_texture_desc){.dim = DE_TEX_2D,
                                                        .format = DE_FMT_RGBA8_UNORM,
                                                        .width = 8,
                                                        .height = 8,
                                                        .usage = DE_USAGE_SAMPLED,
                                                        .name = "de_test_texture"});
    de_gpu_fence fence = de_gpu_fence_create(d, 0);
    ASSERT_TRUE(DE_HANDLE_VALID(named) && DE_HANDLE_VALID(unnamed) && DE_HANDLE_VALID(tex) &&
                DE_HANDLE_VALID(fence));
    char label[128];
    EXPECT_TRUE(de__mtl_object_label(d, DE__GPU_BUFFER, named.id, label, sizeof label));
    EXPECT_STREQ(label, "de_test_label");
    EXPECT_TRUE(de__mtl_object_label(d, DE__GPU_TEXTURE, tex.id, label, sizeof label));
    EXPECT_STREQ(label, "de_test_texture");
#if DE__GPU_DEFAULT_NAMES
    char want[DE__GPU_NAME_MAX];
    snprintf(want, sizeof want, "buffer#%u", unnamed.id & (DE__GPU_POOL_MAX_SLOTS - 1u));
    EXPECT_TRUE(de__mtl_object_label(d, DE__GPU_BUFFER, unnamed.id, label, sizeof label));
    EXPECT_STREQ(label, want);
    snprintf(want, sizeof want, "fence#%u", fence.id & (DE__GPU_POOL_MAX_SLOTS - 1u));
    EXPECT_TRUE(de__mtl_object_label(d, DE__GPU_FENCE, fence.id, label, sizeof label));
    EXPECT_STREQ(label, want);
#endif
    de_gpu_query_heap heap =
        de_gpu_query_heap_create(d, &(de_gpu_query_heap_desc){.count = 2, .name = "de_test_heap"});
    if (DE_HANDLE_VALID(heap)) {
        EXPECT_TRUE(de__mtl_object_label(d, DE__GPU_QUERY_HEAP, heap.id, label, sizeof label));
        EXPECT_STREQ(label, "de_test_heap");
        de_gpu_query_heap_destroy(d, heap);
    }
    EXPECT_FALSE(de__mtl_object_label(d, DE__GPU_BUFFER, 0, label, sizeof label));
    de_gpu_fence_destroy(d, fence);
    de_gpu_texture_destroy(d, tex);
    de_gpu_buffer_destroy(d, unnamed);
    de_gpu_buffer_destroy(d, named);
}

#endif /* DE_HAS_GPU_METAL */

/* ---------- Vulkan ---------- */
#if DE_HAS_GPU_VULKAN

TEST_F(queries, timestamps_vulkan, q_fx) {
    run_timestamps(f, DE_GPU_VULKAN);
}

TEST_F(queries, markers_nested_vulkan, q_fx) {
    run_markers(f, DE_GPU_VULKAN);
    printf("validation layer: %s\n",
           de__gpu_vk_validation_enabled(f->d) ? "on" : "NOT installed: not checked");
}

/* A validation error forced on a named buffer inside nested markers: the layer names the buffer
 * and reports the open labels. */
TEST_F(queries, object_names_vk, q_fx) {
    open_backend(f, DE_GPU_VULKAN);
    de_gpu_device *d = f->d;
    if (!de__gpu_vk_validation_enabled(d))
        SKIP("needs VK_LAYER_KHRONOS_validation (VK_LAYER_PATH): the names and labels are seen "
             "through its messages");
    ASSERT_TRUE(de__gpu_vk_debug_capture(d, true));
    de_gpu_buffer buf = de_gpu_buffer_create(
        d, &(de_gpu_buffer_desc){.size = 256, .usage = DE_USAGE_STORAGE, .name = "de_test_named"});
    ASSERT_TRUE(DE_HANDLE_VALID(buf));
    de_gpu_cmd *c = de_gpu_cmd_begin(d, DE_QUEUE_GRAPHICS, "names");
    ASSERT_TRUE(c != NULL);
    de_gpu_cmd_marker_push(c, "de_test_outer", 0xFF0000u);
    de_gpu_cmd_marker_push(c, "de_test_inner", 0);
    /* A fill that starts past the end of the buffer: VUID-vkCmdFillBuffer-dstOffset-00024. The
     * list is never submitted; the device destroy frees it. */
    de__vk_device *vk = de__vk(d);
    vk->fn.vkCmdFillBuffer((VkCommandBuffer)(uintptr_t)de__gpu_vk_cmd_buffer(c),
                           (VkBuffer)(uintptr_t)de__gpu_vk_buffer_handle(d, buf), 1024, 4, 0);
    char message[2048], objects[512], labels[512];
    const u32 n = de__gpu_vk_debug_captured(d, message, sizeof message, objects, sizeof objects,
                                            labels, sizeof labels);
    printf("captured %u error(s): %s\n  objects: %s\n  labels: %s\n", n, message, objects, labels);
    EXPECT_GE(n, 1u);
    EXPECT_TRUE(strstr(message, "de_test_named") != NULL);
    EXPECT_TRUE(strstr(objects, "de_test_named") != NULL);
    EXPECT_STREQ(labels, "de_test_outer/de_test_inner");
    de_gpu_cmd_marker_pop(c);
    de_gpu_cmd_marker_pop(c);
    EXPECT_TRUE(de__gpu_vk_debug_capture(d, false));
    f->expect_errors = true;
    de_gpu_buffer_destroy(d, buf);
}

#endif /* DE_HAS_GPU_VULKAN */

/* ---------- D3D12 ---------- */
#if defined(_WIN32) && DE_HAS_GPU_D3D12

TEST_F(queries, timestamps_d3d12, q_fx) {
    run_timestamps(f, DE_GPU_D3D12);
}

TEST_F(queries, markers_nested_d3d12, q_fx) {
    run_markers(f, DE_GPU_D3D12);
}

TEST_F(queries, object_names_d3d12, q_fx) {
    open_backend(f, DE_GPU_D3D12);
    de_gpu_device *d = f->d;
    de_gpu_buffer named = de_gpu_buffer_create(
        d, &(de_gpu_buffer_desc){.size = 256, .usage = DE_USAGE_STORAGE, .name = "de_test_named"});
    de_gpu_buffer unnamed =
        de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = 256, .usage = DE_USAGE_STORAGE});
    de_gpu_fence fence = de_gpu_fence_create(d, 0);
    de_gpu_query_heap heap =
        de_gpu_query_heap_create(d, &(de_gpu_query_heap_desc){.count = 2, .name = "de_test_heap"});
    ASSERT_TRUE(DE_HANDLE_VALID(named) && DE_HANDLE_VALID(unnamed) && DE_HANDLE_VALID(fence) &&
                DE_HANDLE_VALID(heap));
    char name[128];
    EXPECT_TRUE(de__d3d12_object_debug_name(d, DE__GPU_BUFFER, named.id, name, sizeof name));
    EXPECT_STREQ(name, "de_test_named");
    EXPECT_TRUE(de__d3d12_object_debug_name(d, DE__GPU_QUERY_HEAP, heap.id, name, sizeof name));
    EXPECT_STREQ(name, "de_test_heap");
#if DE__GPU_DEFAULT_NAMES
    char want[DE__GPU_NAME_MAX];
    snprintf(want, sizeof want, "buffer#%u", unnamed.id & (DE__GPU_POOL_MAX_SLOTS - 1u));
    EXPECT_TRUE(de__d3d12_object_debug_name(d, DE__GPU_BUFFER, unnamed.id, name, sizeof name));
    EXPECT_STREQ(name, want);
    snprintf(want, sizeof want, "fence#%u", fence.id & (DE__GPU_POOL_MAX_SLOTS - 1u));
    EXPECT_TRUE(de__d3d12_object_debug_name(d, DE__GPU_FENCE, fence.id, name, sizeof name));
    EXPECT_STREQ(name, want);
#endif
    de_gpu_query_heap_destroy(d, heap);
    de_gpu_fence_destroy(d, fence);
    de_gpu_buffer_destroy(d, unnamed);
    de_gpu_buffer_destroy(d, named);
}

#endif /* _WIN32 && DE_HAS_GPU_D3D12 */
