/* test_breadcrumbs.c — GPU crash diagnostics: breadcrumbs, device-lost reports, the crash bundle
 * (DENG-328; SPEC §18.5, §12.3). Executable test_breadcrumbs, CTest names
 * gpu_breadcrumbs.breadcrumbs.<test>; each runs in its own process.
 *
 * Front-end (null backend, every host). The null backend is wrapped (de__gpu_register_backend):
 * the wrapper "executes" breadcrumbs at record time by writing the ring slot (the null backend's
 * READBACK buffers are CPU memory) until a test says the GPU hangs, injects DE_ERR_DEVICE_LOST
 * into submits, and counts every call that reaches the backend.
 *   - report_null: named breadcrumbs inside markers, a hang, a lost submit: the report names the
 *     last completed breadcrumb and the ones not reached, with their lists and markers; the
 *     backend's section and the `gpu_fault` annotation are there.
 *   - callback_once: the callback runs once; later calls fail with DE_ERR_DEVICE_LOST without
 *     reaching the backend; destroys still work; a callback set after the loss runs at once; a
 *     loss seen only by the backend's poll is reported too; report buffer sizes.
 *   - ring_wrap: more breadcrumbs than ring slots and a recording past its chunks; recording
 *     allocates nothing; breadcrumbs off is a no-op.
 *   - crash_bundle_annotation (POSIX): a forked child loses its device and then crashes; its
 *     crash report has the `gpu_fault` annotation and `<stem>.gpu_report.txt`, and the next
 *     launch's upload gets de_crash_report.gpu_breadcrumbs.
 * Backends (skip without the device or the shader importer):
 *   - ring_writes_<backend>: breadcrumbs around a dispatch (graphics, compute) and a copy (copy
 *     queue) land in the ring on the GPU; the device is not lost; the debug layer is clean. Metal
 *     devices below the Base tier (CI's paravirtual GPU) cannot run the kernel: copies there.
 *   - mtl_command_buffer_error_report: a real list with a breadcrumb, then a command-buffer
 *     failure recorded as the completion handler records one (de__mtl_errors_simulate; Apple GPUs
 *     discard out-of-bounds accesses rather than fault, so no real fault can be made on demand):
 *     the next call fails fast and the report has the Metal error, each encoder's status and
 *     debug groups, and the last breadcrumb that landed.
 */
#if defined(__linux__)
#define _GNU_SOURCE /* mkdtemp under -std=c11 */
#elif defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "de_test.h"

#include <dengine/de_debug.h>
#include <dengine/de_gpu.h>
#include <dengine/de_platform.h>

#include "gpu/breadcrumbs_internal.h"
#include "gpu/gpu_backend.h"
#include "pipeline_shaders.h"
#include "platform/crash_internal.h"

#if DE_HAS_GPU_VULKAN
#include "gpu/vulkan/vk_internal.h"
#endif
#if DE_HAS_GPU_METAL
#include "gpu/metal/mtl_internal.h"
#endif
#if defined(_WIN32) && DE_HAS_GPU_D3D12
#include "gpu/d3d12/d3d12_agility_exports.h"
#include "gpu/d3d12/d3d12_internal.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define BC_FORK_TESTS 1
#endif

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

static bool has(const char *text, const char *what) {
    return text && strstr(text, what) != NULL;
}

/* ---------- The wrapped null backend ---------- */

typedef struct fake_state {
    bool hang;         /* breadcrumbs stop landing (the GPU is stuck) */
    bool inject_lost;  /* queue_submit returns DE_ERR_DEVICE_LOST */
    bool poll_lost;    /* device_lost says so */
    u32 calls;         /* calls that reached the backend (after the device was made) */
    u32 breadcrumbs;   /* cmd_breadcrumb calls */
    u32 marker_pushes; /* cmd_marker_push calls */
} fake_state;

static fake_state g_fake;
static de__gpu_backend_vtable g_vt;

static de_result fake_submit(de_gpu_device *d, de_gpu_queue q, const de_gpu_submit *s, u64 v) {
    g_fake.calls++;
    de_result r = de__gpu_null_vtable.queue_submit(d, q, s, v); /* completes the values */
    if (g_fake.inject_lost)
        return DE_FAIL(DE_ERR_DEVICE_LOST, "null (test): the device was removed");
    return r;
}

static void fake_breadcrumb(de_gpu_cmd *c, de__gpu_buffer *ring, u64 offset, u32 value) {
    (void)c;
    g_fake.calls++;
    g_fake.breadcrumbs++;
    if (!g_fake.hang)
        ((u32 *)ring->mapped)[offset / 4u] = value;
}

static void fake_marker_push(de_gpu_cmd *c, const char *name, u32 color) {
    (void)c;
    (void)name;
    (void)color;
    g_fake.calls++;
    g_fake.marker_pushes++;
}

static de_result fake_buffer_create(de_gpu_device *d, de__gpu_buffer *b,
                                    const de_gpu_buffer_desc *desc) {
    g_fake.calls++;
    return de__gpu_null_vtable.buffer_create(d, b, desc);
}

static de_result fake_texture_create(de_gpu_device *d, de__gpu_texture *t,
                                     const de_gpu_texture_desc *desc) {
    g_fake.calls++;
    return de__gpu_null_vtable.texture_create(d, t, desc);
}

static de_result fake_fence_create(de_gpu_device *d, de__gpu_fence *f, u64 initial) {
    g_fake.calls++;
    return de__gpu_null_vtable.fence_create(d, f, initial);
}

static de_result fake_fence_wait(de_gpu_device *d, de__gpu_fence *f, u64 value, u64 timeout) {
    g_fake.calls++;
    return de__gpu_null_vtable.fence_wait(d, f, value, timeout);
}

static de_result fake_cmd_begin(de_gpu_cmd *c, const char *name) {
    g_fake.calls++;
    return de__gpu_null_vtable.cmd_begin ? de__gpu_null_vtable.cmd_begin(c, name) : DE_OK;
}

static bool fake_device_lost(de_gpu_device *d) {
    (void)d;
    return g_fake.poll_lost;
}

static size_t fake_lost_report(de_gpu_device *d, char *out, size_t cap) {
    (void)d;
    int w = snprintf(out, cap, "fake backend: fault at 0xdeadbeef\n");
    return w < 0 ? 0 : (size_t)w < cap ? (size_t)w : cap - 1;
}

static void fake_install(void) {
    memset(&g_fake, 0, sizeof g_fake);
    g_vt = de__gpu_null_vtable;
    g_vt.queue_submit = fake_submit;
    g_vt.cmd_breadcrumb = fake_breadcrumb;
    g_vt.cmd_marker_push = fake_marker_push;
    g_vt.buffer_create = fake_buffer_create;
    g_vt.texture_create = fake_texture_create;
    g_vt.fence_create = fake_fence_create;
    g_vt.fence_wait = fake_fence_wait;
    g_vt.cmd_begin = fake_cmd_begin;
    g_vt.device_lost = fake_device_lost;
    g_vt.device_lost_report = fake_lost_report;
    de__gpu_register_backend(DE_GPU_NULL, &g_vt);
}

/* ---------- Fixture ---------- */

typedef struct bc_fx {
    de_tracking_allocator tracker;
    de_allocator alloc;
    de_gpu_device *d;
    de_gpu_backend backend;
    bool expect_errors;
    char skip[1400];
    char err[1100];
    char report[DE_GPU_DEVICE_LOST_REPORT_MAX];
} bc_fx;

static void bc_fx_setup(bc_fx *f) {
    f->alloc = de_tracking_allocator_init(&f->tracker, NULL, DE_MEM_RENDER);
    fake_install();
}

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

static void bc_fx_teardown(bc_fx *f) {
    if (f->d) {
        de_gpu_wait_idle(f->d);
        if (!f->expect_errors)
            EXPECT_EQ(validation_errors(f->d), 0u);
        de_gpu_device_destroy(f->d);
        f->d = NULL;
    }
    de__gpu_register_backend(DE_GPU_NULL, NULL);
    de_tracking_stats st = de_tracking_allocator_stats(&f->tracker);
    EXPECT_EQ(st.blocks_live, 0u);
    EXPECT_EQ(st.bytes_live, 0u);
}

static de_gpu_device *fake_device(bc_fx *f, bool breadcrumbs) {
    de_gpu_device *d = NULL;
    de_gpu_device_desc desc = {
        .backend = DE_GPU_NULL, .crash_breadcrumbs = breadcrumbs, .allocator = &f->alloc};
    if (de_gpu_device_create(&desc, &d) != DE_OK) {
        printf("null device: %s\n", errchain(f->err, sizeof f->err));
        return NULL;
    }
    g_fake.calls = 0;
    g_fake.breadcrumbs = 0;
    g_fake.marker_pushes = 0;
    return d;
}

/* Breadcrumb ids of the "frame" the null tests record. */
enum { ID_SHADOW = 0x100, ID_GBUFFER = 0x101, ID_LIGHTING = 0x102, ID_POST = 0x103 };

static void name_passes(de_gpu_device *d) {
    ASSERT_EQ(de_gpu_breadcrumb_name(d, ID_SHADOW, "shadow"), DE_OK);
    ASSERT_EQ(de_gpu_breadcrumb_name(d, ID_GBUFFER, "gbuffer"), DE_OK);
    ASSERT_EQ(de_gpu_breadcrumb_name(d, ID_LIGHTING, "lighting"), DE_OK);
    ASSERT_EQ(de_gpu_breadcrumb_name(d, ID_POST, "post"), DE_OK);
}

/* One pass: its marker inside "frame", the breadcrumb, then the marker closed. */
static void pass(de_gpu_cmd *c, const char *name, u32 id) {
    de_gpu_cmd_marker_push(c, name, 0x00FF00u);
    de_gpu_cmd_breadcrumb(c, id);
    de_gpu_cmd_marker_pop(c);
}

/* Records shadow and gbuffer, then the GPU "hangs", then lighting and post; submits it. */
static de_result record_frame(de_gpu_device *d, const char *list_name) {
    de_gpu_cmd *c = de_gpu_cmd_begin(d, DE_QUEUE_GRAPHICS, list_name);
    if (!c)
        return DE_ERR_INTERNAL;
    de_gpu_cmd_marker_push(c, "frame", 0);
    pass(c, "shadow", ID_SHADOW);
    pass(c, "gbuffer", ID_GBUFFER);
    g_fake.hang = true;
    pass(c, "lighting", ID_LIGHTING);
    pass(c, "post", ID_POST);
    de_gpu_cmd_marker_pop(c);
    de_gpu_submit s = {.cmds = &c, .cmd_count = 1};
    return de_gpu_queue_submit(d, DE_QUEUE_GRAPHICS, &s);
}

/* ---------- Front-end: null backend ---------- */

TEST_F(breadcrumbs, report_null, bc_fx) {
    f->d = fake_device(f, true);
    ASSERT_TRUE(f->d != NULL);
    de_gpu_device *d = f->d;
    name_passes(d);
    EXPECT_FALSE(de_gpu_device_is_lost(d));
    EXPECT_EQ(de_gpu_device_lost_report(d, f->report, sizeof f->report), DE_ERR_NOT_FOUND);

    /* An earlier frame that completed, then the frame that hangs and loses the device. */
    de_gpu_cmd *c = de_gpu_cmd_begin(d, DE_QUEUE_GRAPHICS, "frame 1");
    ASSERT_TRUE(c != NULL);
    pass(c, "shadow", ID_SHADOW);
    de_gpu_submit s = {.cmds = &c, .cmd_count = 1};
    ASSERT_EQ(de_gpu_queue_submit(d, DE_QUEUE_GRAPHICS, &s), DE_OK);
    u32 seq = 0, id = 0;
    ASSERT_TRUE(de__gpu_breadcrumb_last(d, DE_QUEUE_GRAPHICS, &seq, &id));
    EXPECT_EQ(id, (u32)ID_SHADOW);

    g_fake.inject_lost = true;
    EXPECT_EQ(record_frame(d, "frame 2"), DE_ERR_DEVICE_LOST);
    EXPECT_TRUE(de_gpu_device_is_lost(d));
    ASSERT_EQ(de_gpu_device_lost_report(d, f->report, sizeof f->report), DE_OK);
    printf("%s", f->report);
    const char *r = f->report;
    EXPECT_TRUE(has(r, "DEngine GPU device-lost report"));
    EXPECT_TRUE(has(r, "found by: de_gpu_queue_submit"));
    EXPECT_TRUE(has(r, "null (test): the device was removed"));
    EXPECT_TRUE(has(r, "[graphics queue]"));
    EXPECT_TRUE(has(r, "last completed: id 0x00000101 \"gbuffer\" (list \"frame 2\""));
    EXPECT_TRUE(has(r, "markers frame/gbuffer"));
    EXPECT_TRUE(has(r, "not reached: 2"));
    EXPECT_TRUE(has(r, "id 0x00000102 \"lighting\" (list \"frame 2\""));
    EXPECT_TRUE(has(r, "markers frame/lighting"));
    EXPECT_TRUE(has(r, "id 0x00000103 \"post\""));
    EXPECT_TRUE(has(r, "fake backend: fault at 0xdeadbeef"));
    /* Not reached comes after last completed, lighting before post. */
    EXPECT_TRUE(strstr(r, "\"lighting\"") < strstr(r, "\"post\""));
    EXPECT_TRUE(strstr(r, "last completed") < strstr(r, "\"lighting\""));

    char annot[DE_CRASH_ANNOTATION_VALUE_MAX];
    ASSERT_TRUE(de__crash_annotation_get("gpu_fault", annot, sizeof annot));
    printf("gpu_fault = %s\n", annot);
    EXPECT_TRUE(has(annot, "null device lost (de_gpu_queue_submit)"));
    EXPECT_TRUE(has(annot, "graphics: last done gbuffer, next lighting"));

    /* The crash handler's copy is the same text. */
    size_t n = 0;
    EXPECT_STREQ(de__gpu_crash_report_copy(&n), f->report);
    EXPECT_EQ(n, strlen(f->report));
}

static u32 g_callbacks;
static char g_callback_report[DE_GPU_DEVICE_LOST_REPORT_MAX];

static void on_lost(const char *report, void *user) {
    EXPECT_TRUE(user == (void *)&g_callbacks);
    ++g_callbacks;
    snprintf(g_callback_report, sizeof g_callback_report, "%s", report);
}

TEST_F(breadcrumbs, callback_once, bc_fx) {
    f->d = fake_device(f, true);
    ASSERT_TRUE(f->d != NULL);
    de_gpu_device *d = f->d;
    g_callbacks = 0;
    de_gpu_set_device_lost_callback(d, on_lost, &g_callbacks);
    EXPECT_EQ(g_callbacks, 0u);
    de_gpu_buffer kept = de_gpu_buffer_create(
        d, &(de_gpu_buffer_desc){.size = 256, .usage = DE_USAGE_STORAGE, .name = "kept"});
    ASSERT_TRUE(DE_HANDLE_VALID(kept));
    de_gpu_fence fence = de_gpu_fence_create(d, 0);
    ASSERT_TRUE(DE_HANDLE_VALID(fence));
    de_gpu_cmd *open = de_gpu_cmd_begin(d, DE_QUEUE_COMPUTE, "open across the loss");
    ASSERT_TRUE(open != NULL);

    g_fake.inject_lost = true;
    EXPECT_EQ(record_frame(d, "lost frame"), DE_ERR_DEVICE_LOST);
    EXPECT_EQ(g_callbacks, 1u);
    EXPECT_TRUE(has(g_callback_report, "\"lighting\"") || has(g_callback_report, "id 0x00000102"));

    /* Everything fails fast now, and nothing reaches the backend. */
    const u32 before = g_fake.calls;
    de_error_clear();
    EXPECT_EQ(de_gpu_fence_wait(d, fence, 1, WAIT_NS), DE_ERR_DEVICE_LOST);
    EXPECT_TRUE(de_error_last() && has(de_error_last()->message, "the GPU device is lost"));
    EXPECT_TRUE(de_gpu_cmd_begin(d, DE_QUEUE_GRAPHICS, "after") == NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_DEVICE_LOST);
    EXPECT_FALSE(DE_HANDLE_VALID(
        de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = 256, .usage = DE_USAGE_STORAGE})));
    EXPECT_EQ(de_error_last()->result, DE_ERR_DEVICE_LOST);
    EXPECT_FALSE(DE_HANDLE_VALID(de_gpu_texture_create(
        d, &(de_gpu_texture_desc){
               .format = DE_FMT_RGBA8_UNORM, .width = 4, .usage = DE_USAGE_SAMPLED})));
    EXPECT_FALSE(DE_HANDLE_VALID(de_gpu_fence_create(d, 0)));
    EXPECT_FALSE(DE_HANDLE_VALID(de_gpu_sampler_create(d, &(de_gpu_sampler_desc){0})));
    EXPECT_FALSE(
        DE_HANDLE_VALID(de_gpu_query_heap_create(d, &(de_gpu_query_heap_desc){.count = 2})));
    de_gpu_cmd_marker_push(open, "ignored", 0); /* recording on a list begun before the loss */
    de_gpu_cmd_breadcrumb(open, ID_POST);
    de_gpu_cmd_marker_pop(open);
    de_gpu_submit s = {.cmds = &open, .cmd_count = 1};
    EXPECT_EQ(de_gpu_queue_submit(d, DE_QUEUE_COMPUTE, &s), DE_ERR_DEVICE_LOST);
    EXPECT_EQ(g_fake.calls, before);
    EXPECT_EQ(g_callbacks, 1u);

    /* Setting it again does not call it again; destroys still release (the teardown checks the
     * allocator). */
    de_gpu_set_device_lost_callback(d, on_lost, &g_callbacks);
    EXPECT_EQ(g_callbacks, 1u);
    de_gpu_buffer_destroy(d, kept);
    de_gpu_fence_destroy(d, fence);

    /* Report sizes: too tiny is cut, with DE_ERR_OUT_OF_MEMORY. */
    char tiny[32];
    EXPECT_EQ(de_gpu_device_lost_report(d, tiny, sizeof tiny), DE_ERR_OUT_OF_MEMORY);
    EXPECT_EQ(strlen(tiny), sizeof tiny - 1u);
    EXPECT_EQ(de_gpu_device_lost_report(d, tiny, 0), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_gpu_device_lost_report(NULL, tiny, sizeof tiny), DE_ERR_INVALID_ARGUMENT);
    de_gpu_device_destroy(d);
    f->d = NULL;

    /* A loss only the backend's poll knows of, with the callback set after it: it runs once, on
     * the call that sees the loss. */
    f->d = fake_device(f, false);
    ASSERT_TRUE(f->d != NULL);
    d = f->d;
    g_fake.inject_lost = false;
    g_fake.poll_lost = true;
    g_callbacks = 0;
    EXPECT_TRUE(de_gpu_device_is_lost(d));
    ASSERT_EQ(de_gpu_device_lost_report(d, f->report, sizeof f->report), DE_OK);
    EXPECT_TRUE(has(f->report, "found by: de_gpu_device_is_lost"));
    EXPECT_TRUE(has(f->report, "breadcrumbs: off"));
    EXPECT_TRUE(has(f->report, "fake backend: fault at 0xdeadbeef"));
    de_gpu_set_device_lost_callback(d, on_lost, &g_callbacks);
    EXPECT_EQ(g_callbacks, 1u);
    de_gpu_set_device_lost_callback(d, on_lost, &g_callbacks);
    EXPECT_EQ(g_callbacks, 1u);
}

TEST_F(breadcrumbs, ring_wrap, bc_fx) {
    f->d = fake_device(f, true);
    ASSERT_TRUE(f->d != NULL);
    de_gpu_device *d = f->d;
    name_passes(d);

    /* Recording allocates nothing per breadcrumb: a warm list, then 200 breadcrumbs. */
    de_gpu_cmd *c = de_gpu_cmd_begin(d, DE_QUEUE_GRAPHICS, "warm");
    ASSERT_TRUE(c != NULL);
    de_gpu_submit s = {.cmds = &c, .cmd_count = 1};
    ASSERT_EQ(de_gpu_queue_submit(d, DE_QUEUE_GRAPHICS, &s), DE_OK);
    c = de_gpu_cmd_begin(d, DE_QUEUE_GRAPHICS, "no allocations");
    ASSERT_TRUE(c != NULL);
    const de_tracking_stats a0 = de_tracking_allocator_stats(&f->tracker);
    for (u32 i = 0; i < 200; ++i)
        de_gpu_cmd_breadcrumb(c, i);
    const de_tracking_stats a1 = de_tracking_allocator_stats(&f->tracker);
    EXPECT_EQ(a1.alloc_count, a0.alloc_count);
    EXPECT_EQ(a1.realloc_count, a0.realloc_count);
    ASSERT_EQ(de_gpu_queue_submit(d, DE_QUEUE_GRAPHICS, &s), DE_OK);

    /* 300 lists of 10 breadcrumbs: 3000 + 200 > the ring's slots (and chunk tails skip slots). */
    for (u32 frame = 0; frame < 300; ++frame) {
        c = de_gpu_cmd_begin(d, DE_QUEUE_GRAPHICS, "wrap");
        ASSERT_TRUE(c != NULL);
        for (u32 i = 0; i < 10; ++i)
            de_gpu_cmd_breadcrumb(c, 0x1000u + frame * 16u + i);
        ASSERT_EQ(de_gpu_queue_submit(d, DE_QUEUE_GRAPHICS, &s), DE_OK);
    }
    u32 seq = 0, id = 0;
    ASSERT_TRUE(de__gpu_breadcrumb_last(d, DE_QUEUE_GRAPHICS, &seq, &id));
    EXPECT_EQ(id, 0x1000u + 299u * 16u + 9u);
    EXPECT_GT(seq, DE__GPU_BC_SLOTS);

    /* A recording past its chunks drops the rest, and says so. */
    c = de_gpu_cmd_begin(d, DE_QUEUE_GRAPHICS, "too many");
    ASSERT_TRUE(c != NULL);
    for (u32 i = 0; i < DE__GPU_BC_RANGES * DE__GPU_BC_CHUNK + 5u; ++i)
        de_gpu_cmd_breadcrumb(c, 0x5000u + i);
    EXPECT_EQ(de__gpu_breadcrumb_dropped(d), 5u);
    ASSERT_EQ(de_gpu_queue_submit(d, DE_QUEUE_GRAPHICS, &s), DE_OK);
    ASSERT_TRUE(de__gpu_breadcrumb_last(d, DE_QUEUE_GRAPHICS, &seq, &id));
    EXPECT_EQ(id, 0x5000u + DE__GPU_BC_RANGES * DE__GPU_BC_CHUNK - 1u);

    /* The lost frame after the wrap: still named correctly. */
    g_fake.inject_lost = true;
    EXPECT_EQ(record_frame(d, "after the wrap"), DE_ERR_DEVICE_LOST);
    ASSERT_EQ(de_gpu_device_lost_report(d, f->report, sizeof f->report), DE_OK);
    printf("%s", f->report);
    EXPECT_TRUE(
        has(f->report, "last completed: id 0x00000101 \"gbuffer\" (list \"after the wrap\""));
    EXPECT_TRUE(has(f->report, "not reached: 2"));
    EXPECT_TRUE(has(f->report, "more were overwritten in the ring"));
    EXPECT_TRUE(has(f->report, "breadcrumbs dropped"));
    de_gpu_device_destroy(d);
    f->d = NULL;

    /* Without crash_breadcrumbs, breadcrumbs reach no backend and the rings do not exist. */
    f->d = fake_device(f, false);
    ASSERT_TRUE(f->d != NULL);
    d = f->d;
    g_fake.inject_lost = false;
    g_fake.hang = false;
    EXPECT_TRUE(de__gpu_breadcrumb_ring(d, DE_QUEUE_GRAPHICS) == NULL);
    c = de_gpu_cmd_begin(d, DE_QUEUE_GRAPHICS, "off");
    ASSERT_TRUE(c != NULL);
    de_gpu_cmd_breadcrumb(c, 1);
    EXPECT_EQ(g_fake.breadcrumbs, 0u);
    ASSERT_EQ(de_gpu_queue_submit(d, DE_QUEUE_GRAPHICS, &s), DE_OK);
    EXPECT_FALSE(de__gpu_breadcrumb_last(d, DE_QUEUE_GRAPHICS, NULL, NULL));
    de_gpu_device_destroy(d);
    f->d = NULL;

    /* Inside a render pass a breadcrumb never reaches the backend (refused in Debug and Dev). */
    f->d = fake_device(f, true);
    ASSERT_TRUE(f->d != NULL);
    d = f->d;
    de_gpu_texture rt =
        de_gpu_texture_create(d, &(de_gpu_texture_desc){.format = DE_FMT_RGBA8_UNORM,
                                                        .width = 8,
                                                        .height = 8,
                                                        .usage = DE_USAGE_RENDER_TARGET,
                                                        .name = "bc target"});
    ASSERT_TRUE(DE_HANDLE_VALID(rt));
    c = de_gpu_cmd_begin(d, DE_QUEUE_GRAPHICS, "render pass");
    ASSERT_TRUE(c != NULL);
    de_gpu_attachment att;
    memset(&att, 0, sizeof att);
    att.texture = rt;
    att.store = true;
    de_gpu_cmd_begin_render(c, &att, 1, NULL);
    de_error_clear();
    de_gpu_cmd_breadcrumb(c, 7);
    EXPECT_EQ(g_fake.breadcrumbs, 0u);
#if DE__GPU_VALIDATE_COMMANDS
    EXPECT_TRUE(de_error_last() && has(de_error_last()->message, "inside a render pass"));
#endif
    de_gpu_cmd_end_render(c);
    de_gpu_cmd_breadcrumb(c, 8);
    EXPECT_EQ(g_fake.breadcrumbs, 1u);
    ASSERT_EQ(de_gpu_queue_submit(d, DE_QUEUE_GRAPHICS, &s), DE_OK);
    de_gpu_texture_destroy(d, rt);
}

/* ---------- The crash bundle (fork, POSIX) ---------- */

#if defined(BC_FORK_TESTS)

static char g_file[64 * 1024];

static const char *read_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return NULL;
    size_t n = fread(g_file, 1, sizeof g_file - 1, fp);
    fclose(fp);
    g_file[n] = '\0';
    return g_file;
}

static void rm_tree(const char *path) {
    DIR *dir = opendir(path);
    if (dir == NULL) {
        (void)unlink(path);
        return;
    }
    struct dirent *e;
    char p[1024];
    while ((e = readdir(dir)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        snprintf(p, sizeof p, "%s/%s", path, e->d_name);
        rm_tree(p);
    }
    closedir(dir);
    (void)rmdir(path);
}

/* The child: crash handling, a device lost in a frame, then a crash. */
static void child_lose_and_crash(const char *dir) {
    if (de_crash_install(dir, NULL, NULL) != DE_OK)
        _exit(2);
    fake_install();
    de_gpu_device *d = NULL;
    de_gpu_device_desc desc = {.backend = DE_GPU_NULL, .crash_breadcrumbs = true};
    if (de_gpu_device_create(&desc, &d) != DE_OK)
        _exit(3);
    de_gpu_breadcrumb_name(d, ID_GBUFFER, "gbuffer");
    de_gpu_breadcrumb_name(d, ID_LIGHTING, "lighting");
    g_fake.inject_lost = true;
    if (record_frame(d, "child frame") != DE_ERR_DEVICE_LOST)
        _exit(4);
    (void)raise(SIGSEGV);
    _exit(5); /* the crash handler should have ended the process */
}

static int find_report(const char *dir, char *stem, size_t cap) {
    DIR *d = opendir(dir);
    int n = 0;
    if (!d)
        return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (strncmp(e->d_name, "crash-", 6) != 0 || len < 10 ||
            strcmp(e->d_name + len - 4, ".txt") != 0 || memchr(e->d_name, '.', len - 4) != NULL)
            continue;
        snprintf(stem, cap, "%s/%.*s", dir, (int)(len - 4), e->d_name);
        ++n;
    }
    closedir(d);
    return n;
}

typedef struct upload_rec {
    u32 calls;
    bool gpu_fault;
    char breadcrumbs[4096];
} upload_rec;

static void record_upload(const de_crash_report *r, void *user) {
    upload_rec *u = (upload_rec *)user;
    u->calls++;
    u->gpu_fault = r->gpu_fault;
    snprintf(u->breadcrumbs, sizeof u->breadcrumbs, "%s",
             r->gpu_breadcrumbs ? r->gpu_breadcrumbs : "(null)");
}

TEST(breadcrumbs, crash_bundle_annotation) {
    char dir[512];
    const char *tmp = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/de_gpu_crash_XXXXXX", tmp && tmp[0] ? tmp : "/tmp");
    ASSERT_TRUE(mkdtemp(dir) != NULL);

    fflush(NULL);
    pid_t pid = fork();
    ASSERT_TRUE(pid >= 0);
    if (pid == 0) {
        char path[600];
        snprintf(path, sizeof path, "%s/child_stderr.log", dir);
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0)
            (void)dup2(fd, 2);
        child_lose_and_crash(dir);
    }
    int status = 0;
    for (int waited = 0;; waited += 10) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid)
            break;
        if (r < 0 && errno != EINTR)
            FAIL("waitpid failed");
        if (waited > 30000) {
            (void)kill(pid, SIGKILL);
            (void)waitpid(pid, &status, 0);
            FAIL("the child did not finish in 30 s");
        }
        struct timespec ts = {0, 10 * 1000 * 1000};
        (void)nanosleep(&ts, NULL);
    }
    if (!(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV)) {
        char path[600];
        snprintf(path, sizeof path, "%s/child_stderr.log", dir);
        const char *t = read_file(path);
        printf("child status 0x%x; stderr:\n%s\n", (unsigned)status, t ? t : "(none)");
    }
    EXPECT_TRUE(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);

    char stem[700], path[800];
    ASSERT_EQ(find_report(dir, stem, sizeof stem), 1);
    snprintf(path, sizeof path, "%s.txt", stem);
    const char *text = read_file(path);
    ASSERT_TRUE(text != NULL);
    printf("---- %s ----\n%s\n", path, text);
    EXPECT_TRUE(has(text, "gpu_fault"));
    EXPECT_TRUE(has(text, "last done gbuffer, next lighting"));
    EXPECT_TRUE(has(text, "gpu_report: "));
    snprintf(path, sizeof path, "%s.gpu_report.txt", stem);
    text = read_file(path);
    ASSERT_TRUE(text != NULL);
    EXPECT_TRUE(has(text, "DEngine GPU device-lost report"));
    EXPECT_TRUE(has(text, "\"gbuffer\""));
    EXPECT_TRUE(has(text, "\"lighting\""));

    /* The next launch: the upload callback gets the breadcrumbs. */
    upload_rec u;
    memset(&u, 0, sizeof u);
    ASSERT_EQ(de_crash_install(dir, record_upload, &u), DE_OK);
    EXPECT_EQ(u.calls, 1u);
    EXPECT_TRUE(u.gpu_fault);
    EXPECT_TRUE(has(u.breadcrumbs, "DEngine GPU device-lost report"));
    EXPECT_TRUE(has(u.breadcrumbs, "\"gbuffer\""));

    const char *keep = getenv("DE_TEST_KEEP");
    if (!(keep && keep[0] == '1'))
        rm_tree(dir);
}

#endif /* BC_FORK_TESTS */

/* ---------- Backends ---------- */

#if DE_HAS_GPU_VULKAN || DE_HAS_GPU_METAL || (defined(_WIN32) && DE_HAS_GPU_D3D12)

static void set_env(const char *name, const char *value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
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

static void need_shaders(bc_fx *f, de_gpu_backend b) {
    char why[300];
    if (!de_pipe_test_cook(target_of(b), why, sizeof why)) {
        snprintf(f->skip, sizeof f->skip, "shared pipeline shaders not cooked: %s", why);
        SKIP(f->skip);
    }
}

/* A device of backend `b` with crash diagnostics (and its debug layer when `debug`), or SKIP. */
static void open_backend(bc_fx *f, de_gpu_backend b, bool debug) {
    if (b == DE_GPU_METAL && debug)
        set_env("MTL_DEBUG_LAYER", "1"); /* before the first Metal device of the process */
    de_gpu_device_desc desc = {.backend = b,
                               .debug_layer = debug,
                               .gpu_validation = debug,
                               .crash_breadcrumbs = true,
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
    printf("device: \"%s\" (%s)\n", de_gpu_get_caps(f->d)->device_name, de_gpu_backend_name(b));
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

static de_gpu_pipeline compute_pipeline(bc_fx *f) {
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
        .name = "breadcrumbs compute"};
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

static de_result submit_wait(de_gpu_device *d, de_gpu_queue q, de_gpu_cmd *c) {
    de_gpu_fence fence = de_gpu_fence_create(d, 0);
    if (!DE_HANDLE_VALID(fence))
        return de_error_last() ? de_error_last()->result : DE_ERR_INTERNAL;
    de_gpu_submit s = {.cmds = &c, .cmd_count = 1, .signal = fence, .signal_value = 1};
    de_result r = de_gpu_queue_submit(d, q, &s);
    if (r == DE_OK)
        r = de_gpu_fence_wait(d, fence, 1, WAIT_NS);
    de_gpu_fence_destroy(d, fence);
    return r;
}

#define RW_ELEMS 4096u

/* The shared compute kernel needs Metal 3 (bindless through gpuAddress); Metal devices below the
 * Base tier (the paravirtual GPU of CI's macOS runners) hang on it
 * (kIOGPUCommandBufferCallbackErrorHang, as test_mtl_cmd.c's kernels). They get a copy instead. */
static bool can_dispatch(bc_fx *f) {
    return f->backend != DE_GPU_METAL || de_gpu_get_caps(f->d)->base_tier_ok;
}

/* Records the work between two breadcrumbs: the shared compute kernel over RW_ELEMS elements, or
 * (copy queue, or no kernel) a copy of the same buffers. */
static void record_work(bc_fx *f, de_gpu_cmd *c, de_gpu_queue q, de_gpu_pipeline pipe,
                        de_gpu_buffer src, de_gpu_buffer dst) {
    de_gpu_device *d = f->d;
    if (q == DE_QUEUE_COPY || !DE_HANDLE_VALID(pipe)) {
        de_gpu_barrier bb[2] = {buf_barrier(src, DE_ACCESS_NONE, DE_ACCESS_COPY_SRC),
                                buf_barrier(dst, DE_ACCESS_NONE, DE_ACCESS_COPY_DST)};
        de_gpu_cmd_barriers(c, bb, 2);
        de_gpu_cmd_copy_buffer(c, dst, 0, src, 0, RW_ELEMS * 4u);
        return;
    }
    de_gpu_barrier bb[2] = {buf_barrier(src, DE_ACCESS_NONE, DE_ACCESS_SHADER_READ),
                            buf_barrier(dst, DE_ACCESS_NONE, DE_ACCESS_SHADER_WRITE)};
    de_gpu_cmd_barriers(c, bb, 2);
    de_gpu_cmd_set_pipeline(c, pipe);
    const u32 params[4] = {de_gpu_buffer_bindless(d, src), de_gpu_buffer_bindless(d, dst), RW_ELEMS,
                           DE_PIPE_TEST_COMPUTE_SEED};
    de_gpu_cmd_push_constants(c, params, sizeof params);
    de_gpu_cmd_dispatch(c, RW_ELEMS / 64u, 1, 1);
}

/* Breadcrumbs around a dispatch (graphics, compute) or a copy (copy queue) land in the ring. */
static void run_ring_writes(bc_fx *f, de_gpu_backend b) {
    open_backend(f, b, true);
    de_gpu_device *d = f->d;
    de_gpu_pipeline pipe = {0};
    if (can_dispatch(f)) {
        need_shaders(f, b);
        pipe = compute_pipeline(f);
    } else {
        printf("below the Base tier: copies instead of the compute kernel\n");
    }
    de_gpu_buffer src =
        de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = RW_ELEMS * 4u,
                                                      .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                                      .stride = 4,
                                                      .name = "bc src"});
    de_gpu_buffer dst =
        de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = RW_ELEMS * 4u,
                                                      .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_DST,
                                                      .stride = 4,
                                                      .name = "bc dst"});
    ASSERT_TRUE(DE_HANDLE_VALID(src) && DE_HANDLE_VALID(dst));
    for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q) {
        de_error_clear();
        de_gpu_cmd *c = de_gpu_cmd_begin(d, (de_gpu_queue)q, "breadcrumb writes");
        ASSERT_TRUE(c != NULL);
        const u32 base = 0x700u + q * 0x10u;
        de_gpu_cmd_marker_push(c, "de_test_ring", 0);
        de_gpu_cmd_breadcrumb(c, base);
        record_work(f, c, (de_gpu_queue)q, pipe, src, dst);
        de_gpu_cmd_breadcrumb(c, base + 1u);
        de_gpu_cmd_marker_pop(c);
        if (de_error_last())
            printf("recording: %s\n", errchain(f->err, sizeof f->err));
        EXPECT_TRUE(de_error_last() == NULL);
        ASSERT_EQ(submit_wait(d, (de_gpu_queue)q, c), DE_OK);
        u32 seq = 0, id = 0;
        EXPECT_TRUE(de__gpu_breadcrumb_last(d, (de_gpu_queue)q, &seq, &id));
        printf("%s queue: last breadcrumb 0x%x (sequence %u)\n",
               q == 0   ? "graphics"
               : q == 1 ? "compute"
                        : "copy",
               id, seq);
        EXPECT_EQ(id, base + 1u);
    }
    EXPECT_FALSE(de_gpu_device_is_lost(d));
    EXPECT_EQ(de_gpu_device_lost_report(d, f->report, sizeof f->report), DE_ERR_NOT_FOUND);
    de_gpu_buffer_destroy(d, dst);
    de_gpu_buffer_destroy(d, src);
    if (DE_HANDLE_VALID(pipe))
        de_gpu_pipeline_destroy(d, pipe);
}

#endif

#if DE_HAS_GPU_METAL

TEST_F(breadcrumbs, ring_writes_metal, bc_fx) {
    run_ring_writes(f, DE_GPU_METAL);
}

/* A Metal command-buffer failure, recorded the way the completion handler records a real one
 * (de__mtl_errors_simulate): Apple GPUs discard out-of-bounds accesses instead of faulting, so a
 * test cannot fault the GPU on demand (DENG-328: a 4 GiB out-of-bounds write and writes to address
 * 0x8 both completed without an error on an M5 Pro). The breadcrumbs are real GPU writes. */
TEST_F(breadcrumbs, mtl_command_buffer_error_report, bc_fx) {
    open_backend(f, DE_GPU_METAL, false);
    de_gpu_device *d = f->d;
    de_gpu_pipeline pipe = {0};
    if (can_dispatch(f)) {
        need_shaders(f, DE_GPU_METAL);
        pipe = compute_pipeline(f);
    }
    de_gpu_buffer src =
        de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = RW_ELEMS * 4u,
                                                      .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                                      .stride = 4,
                                                      .name = "fault src"});
    de_gpu_buffer dst =
        de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = RW_ELEMS * 4u,
                                                      .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_DST,
                                                      .stride = 4,
                                                      .name = "fault dst"});
    ASSERT_TRUE(DE_HANDLE_VALID(src) && DE_HANDLE_VALID(dst));
    ASSERT_EQ(de_gpu_breadcrumb_name(d, 1, "before the fault"), DE_OK);
    g_callbacks = 0;
    de_gpu_set_device_lost_callback(d, on_lost, &g_callbacks);

    de_gpu_cmd *c = de_gpu_cmd_begin(d, DE_QUEUE_COMPUTE, "de_test_faulting_list");
    ASSERT_TRUE(c != NULL);
    de_gpu_cmd_marker_push(c, "de_test_fault", 0xFF0000u);
    de_gpu_cmd_breadcrumb(c, 1);
    record_work(f, c, DE_QUEUE_COMPUTE, pipe, src, dst);
    de_gpu_cmd_marker_pop(c);
    ASSERT_EQ(submit_wait(d, DE_QUEUE_COMPUTE, c), DE_OK);
    EXPECT_FALSE(de_gpu_device_is_lost(d));

    ASSERT_TRUE(de__mtl_errors_simulate(d, "de_test_faulting_list", "de_test_fault"));
    /* The next call sees the loss (the backend's poll) and fails fast. */
    EXPECT_TRUE(de_gpu_cmd_begin(d, DE_QUEUE_COMPUTE, "after") == NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_DEVICE_LOST);
    EXPECT_EQ(g_callbacks, 1u);
    ASSERT_EQ(de_gpu_device_lost_report(d, f->report, sizeof f->report), DE_OK);
    printf("%s", f->report);
    const char *rep = f->report;
    EXPECT_TRUE(has(rep, "found by: de_gpu_cmd_begin"));
    EXPECT_TRUE(has(rep, "[metal backend]"));
    EXPECT_TRUE(has(rep, "command buffer \"de_test_faulting_list\" on the compute queue failed: "
                         "MTLCommandBufferErrorDomain PageFault"));
    EXPECT_TRUE(has(rep, "underlying: IOGPUCommandQueueErrorDomain code 3"));
    EXPECT_TRUE(has(rep, "encoder \"de_test compute\": FAULTED; debug groups de_test_fault"));
    EXPECT_TRUE(has(rep, "encoder \"de breadcrumb\": pending"));
    EXPECT_TRUE(has(rep, "[compute queue]"));
    EXPECT_TRUE(has(rep, "last completed: id 0x00000001 \"before the fault\""));
    EXPECT_TRUE(has(rep, "markers de_test_fault"));
    char annot[DE_CRASH_ANNOTATION_VALUE_MAX];
    ASSERT_TRUE(de__crash_annotation_get("gpu_fault", annot, sizeof annot));
    EXPECT_TRUE(has(annot, "metal device lost (de_gpu_cmd_begin)"));
    de_gpu_buffer_destroy(d, dst);
    de_gpu_buffer_destroy(d, src);
    if (DE_HANDLE_VALID(pipe))
        de_gpu_pipeline_destroy(d, pipe);
}

#endif /* DE_HAS_GPU_METAL */

#if DE_HAS_GPU_VULKAN
TEST_F(breadcrumbs, ring_writes_vulkan, bc_fx) {
    run_ring_writes(f, DE_GPU_VULKAN);
}
#endif

#if defined(_WIN32) && DE_HAS_GPU_D3D12
TEST_F(breadcrumbs, ring_writes_d3d12, bc_fx) {
    run_ring_writes(f, DE_GPU_D3D12);
    /* DRED was switched on before the device was made. */
    EXPECT_TRUE(de__d3d12(f->d)->dred);
}
#endif
