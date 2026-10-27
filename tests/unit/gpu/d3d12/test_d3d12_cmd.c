/* test_d3d12_cmd.c — D3D12 command lists, barriers, copies, fences and swapchains (DENG-286).
 * CTest names: gpu_d3d12.d3d12.<test>. Windows with the D3D12 backend only (CI: WARP).
 *
 * The GPU tests mirror DENG-284's Vulkan tests (tests/unit/gpu/vulkan/test_vk_cmd.c) with the same
 * expected values, so results compare across backends:
 *   - clear_readback: a 256x256 RGBA8 render target cleared to (0.2, 0.4, 0.6, 1.0) reads back as
 *     bytes (51, 102, 153, 255) in every texel (twice: the second pass reuses the recycled list).
 *   - copy_bc7_mips: a 64x64 BC7 texture with 4 mips, filled from a buffer (byte i of mip m's
 *     region = (u8)(i * 13 + 7 + m)), copied back byte for byte.
 *   - cross_queue_timeline: 64 KiB (byte i = (u8)(i * 7 + 3)) copied
 *     on the copy queue, read on the graphics queue after a fence wait submitted first (wait before
 *     signal), with the buffer's ownership released and acquired through COMMON.
 *   - legacy_barriers_forced: the three bodies again on a device forced to legacy ResourceBarrier
 *     (cvar gpu.d3d12.force_legacy_barriers), so CI covers the fallback whatever the adapter.
 *   - swapchain_present_resize: 120 frames of acquire / clear / present on a hidden 320x240
 *     window, a resize of the window (to 200x150, through SDL) and of the swapchain, 30 more
 * frames, 8 resizes each right after a present (DENG-402: the last present must be done before
 * the buffers go), then 10 frames each with IMMEDIATE and MAILBOX swapchains, each destroyed
 * right after a present. The debug-layer error count is checked after the resizes and destroys,
 * so a failure names the step. SKIPs without a video driver (no desktop session). Every device has
 * the debug layer and GPU-based validation on and must end with zero debug-layer errors. They run
 * on the best adapter (WARP on CI) and print which barrier path it took.
 */
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "de_test.h"

#include <dengine/de_debug.h>
#include <dengine/de_gpu.h>
#include <dengine/de_platform.h>

#include "gpu/d3d12/d3d12_internal.h"
#include "gpu/gpu_backend.h"

#include <stdio.h>
#include <string.h>

#if defined(DE_D3D12_TEST_SDL)
#include <SDL3/SDL.h>
#endif

#define KIB     ((u64)1 << 10)
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

/* ---------- Fixture: the best adapter with the debug layer ---------- */

typedef struct cmd_fx {
    de_tracking_allocator tracker;
    de_allocator alloc;
    de_gpu_device *d;
    char err[1100];
    char skip[1200];
} cmd_fx;

static void force_legacy(bool on) {
    de_cvar_desc cd = {.name = "gpu.d3d12.force_legacy_barriers", .type = DE_CVAR_BOOL};
    de_cvar cv = de_cvar_register(&cd);
    ASSERT_TRUE(cv.id != 0);
    ASSERT_EQ(de_cvar_set(cv, on ? "true" : "false"), DE_OK);
}

static void open_device(cmd_fx *f, bool gpu_validation) {
    f->alloc = de_tracking_allocator_init(&f->tracker, NULL, DE_MEM_RENDER);
    de_gpu_device_desc desc = {.backend = DE_GPU_D3D12,
                               .debug_layer = true,
                               .gpu_validation = gpu_validation,
                               .allocator = &f->alloc};
    de_result r = de_gpu_device_create(&desc, &f->d);
    if (r == DE_ERR_UNSUPPORTED) {
        snprintf(f->skip, sizeof f->skip, "no D3D12 device: %s", errchain(f->err, sizeof f->err));
        SKIP(f->skip);
    }
    ASSERT_EQ(r, DE_OK);
    ASSERT_TRUE(de__d3d12_debug_layer_active(f->d));
}

/* The barrier path is the device's default (DE_D3D12_FORCE_LEGACY_BARRIERS=1 forces legacy for a
 * whole run). */
static void cmd_fx_setup(cmd_fx *f) {
    open_device(f, true);
}

static void cmd_fx_teardown(cmd_fx *f) {
    if (f->d) {
        de_gpu_wait_idle(f->d);
        EXPECT_EQ(de__d3d12_debug_error_count(f->d), 0u);
        de_gpu_device_destroy(f->d);
        de_tracking_stats st = de_tracking_allocator_stats(&f->tracker);
        EXPECT_EQ(st.blocks_live, 0u);
        EXPECT_EQ(st.bytes_live, 0u);
    }
}

typedef cmd_fx legacy_fx;
static void legacy_fx_setup(legacy_fx *f) {
    force_legacy(true);
    open_device(f, true);
}
static void legacy_fx_teardown(legacy_fx *f) {
    cmd_fx_teardown(f);
    force_legacy(false);
}

/* Match the golden harness's API-only validation as a separate control (DENG-651). */
typedef cmd_fx api_copy_fx;
static void api_copy_fx_setup(api_copy_fx *f) {
    open_device(f, false);
}
static void api_copy_fx_teardown(api_copy_fx *f) {
    cmd_fx_teardown(f);
}
typedef cmd_fx legacy_api_copy_fx;
static void legacy_api_copy_fx_setup(legacy_api_copy_fx *f) {
    force_legacy(true);
    open_device(f, false);
}
static void legacy_api_copy_fx_teardown(legacy_api_copy_fx *f) {
    legacy_fx_teardown(f);
}

static void print_mode(de_gpu_device *d) {
    const de_gpu_caps *c = de_gpu_get_caps(d);
    printf("device \"%s\": %s barriers (caps.enhanced_barriers %d)\n", c->device_name,
           de__d3d12_enhanced_barriers_active(d) ? "enhanced" : "legacy", c->enhanced_barriers);
}

static de_gpu_barrier tex_barrier(de_gpu_texture t, u32 before, u32 after) {
    de_gpu_barrier b;
    memset(&b, 0, sizeof b);
    b.texture = t;
    b.before = before;
    b.after = after;
    return b;
}

static de_gpu_barrier buf_barrier(de_gpu_buffer buf, u32 before, u32 after) {
    de_gpu_barrier b;
    memset(&b, 0, sizeof b);
    b.buffer = buf;
    b.before = before;
    b.after = after;
    return b;
}

static void submit_wait(de_gpu_device *d, de_gpu_queue q, de_gpu_cmd *c, de_gpu_fence fence,
                        u64 value) {
    de_gpu_submit s = {.cmds = &c, .cmd_count = 1, .signal = fence, .signal_value = value};
    ASSERT_EQ(de_gpu_queue_submit(d, q, &s), DE_OK);
    ASSERT_EQ(de_gpu_fence_wait(d, fence, value, WAIT_NS), DE_OK);
    EXPECT_GE(de_gpu_fence_value(d, fence), value);
}

/* ---------- Bodies ---------- */

static void body_clear_readback(de_gpu_device *d) {
    enum { W = 256, H = 256, PITCH = W * 4 };
    de_gpu_texture tex = de_gpu_texture_create(
        d, &(de_gpu_texture_desc){.format = DE_FMT_RGBA8_UNORM,
                                  .width = W,
                                  .height = H,
                                  .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_COPY_SRC,
                                  .name = "clear target"});
    de_gpu_buffer rb = de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = (u64)PITCH * H,
                                                                     .usage = DE_USAGE_COPY_DST,
                                                                     .memory = DE_MEM_READBACK,
                                                                     .name = "readback"});
    de_gpu_fence fence = de_gpu_fence_create(d, 0);
    ASSERT_TRUE(DE_HANDLE_VALID(tex) && DE_HANDLE_VALID(rb) && DE_HANDLE_VALID(fence));
    EXPECT_EQ(de_gpu_fence_value(d, fence), 0u);
    u8 *out = (u8 *)de_gpu_buffer_map(d, rb);
    ASSERT_TRUE(out != NULL);
    memset(out, 0xEE, (size_t)PITCH * H);

    /* Twice, so the second pass reuses the recycled command list and its allocator. */
    for (u32 pass = 0; pass < 2; ++pass) {
        de_error_clear();
        de_gpu_cmd *c = de_gpu_cmd_begin(d, DE_QUEUE_GRAPHICS, "clear_readback");
        ASSERT_TRUE(c != NULL);
        de_gpu_barrier b = tex_barrier(tex, DE_ACCESS_NONE, DE_ACCESS_RENDER_TARGET);
        de_gpu_cmd_barriers(c, &b, 1);
        de_gpu_attachment color = {
            .texture = tex, .clear = true, .clear_color = {0.2f, 0.4f, 0.6f, 1.0f}, .store = true};
        de_gpu_cmd_begin_render(c, &color, 1, NULL);
        de_gpu_cmd_viewport(c, 0.0f, 0.0f, (f32)W, (f32)H);
        de_gpu_cmd_end_render(c);
        b = tex_barrier(tex, DE_ACCESS_RENDER_TARGET, DE_ACCESS_COPY_SRC);
        de_gpu_cmd_barriers(c, &b, 1);
        de_gpu_cmd_copy_texture_to_buffer(c, rb, 0, PITCH, tex, 0, 0);
        EXPECT_TRUE(de_error_last() == NULL); /* every command passed validation */
        submit_wait(d, DE_QUEUE_GRAPHICS, c, fence, pass + 1u);
    }

    u32 bad = 0;
    for (u32 i = 0; i < (u32)W * H; ++i) {
        const u8 *p = out + (size_t)i * 4;
        if (p[0] != 51 || p[1] != 102 || p[2] != 153 || p[3] != 255) {
            if (bad == 0)
                printf("texel %u: %u %u %u %u\n", i, p[0], p[1], p[2], p[3]);
            ++bad;
        }
    }
    EXPECT_EQ(bad, 0u);
    de_gpu_texture_destroy(d, tex);
    de_gpu_buffer_destroy(d, rb);
    de_gpu_fence_destroy(d, fence);
}

static void body_copy_bc7_mips(de_gpu_device *d, de_gpu_queue q) {
    enum { SIZE = 64, MIPS = 4, PITCH = 256 };
    de_gpu_texture tex = de_gpu_texture_create(
        d, &(de_gpu_texture_desc){.format = DE_FMT_BC7_UNORM,
                                  .width = SIZE,
                                  .height = SIZE,
                                  .mip_count = MIPS,
                                  .usage = DE_USAGE_COPY_SRC | DE_USAGE_COPY_DST,
                                  .name = "bc7"});
    ASSERT_TRUE(DE_HANDLE_VALID(tex));
    /* Mip m: (64 >> m) / 4 rows of blocks, 256-byte rows, regions at 512-byte offsets. */
    u64 offset[MIPS], rows[MIPS], row_bytes[MIPS], total = 0;
    for (u32 m = 0; m < MIPS; ++m) {
        offset[m] = total;
        rows[m] = (u64)(SIZE >> m) / 4u;
        row_bytes[m] = rows[m] * 16u; /* blocks per row = rows (square), 16 bytes each */
        total += (rows[m] * PITCH + 511u) & ~(u64)511u;
    }
    de_gpu_buffer up = de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = total,
                                                                     .usage = DE_USAGE_COPY_SRC,
                                                                     .memory = DE_MEM_UPLOAD,
                                                                     .name = "bc7 upload"});
    de_gpu_buffer rb = de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = total,
                                                                     .usage = DE_USAGE_COPY_DST,
                                                                     .memory = DE_MEM_READBACK,
                                                                     .name = "bc7 readback"});
    de_gpu_fence fence = de_gpu_fence_create(d, 0);
    ASSERT_TRUE(DE_HANDLE_VALID(up) && DE_HANDLE_VALID(rb) && DE_HANDLE_VALID(fence));
    u8 *src = (u8 *)de_gpu_buffer_map(d, up);
    u8 *dst = (u8 *)de_gpu_buffer_map(d, rb);
    ASSERT_TRUE(src && dst);
    for (u32 m = 0; m < MIPS; ++m)
        for (u64 i = 0; i < rows[m] * PITCH; ++i)
            src[offset[m] + i] = (u8)(i * 13u + 7u + m);
    memset(dst, 0, (size_t)total);

    de_error_clear();
    de_gpu_cmd *c = de_gpu_cmd_begin(d, q, "bc7 round trip");
    ASSERT_TRUE(c != NULL);
    de_gpu_barrier b = tex_barrier(tex, DE_ACCESS_NONE, DE_ACCESS_COPY_DST);
    de_gpu_cmd_barriers(c, &b, 1);
    for (u32 m = 0; m < MIPS; ++m)
        de_gpu_cmd_copy_buffer_to_texture(c, tex, m, 0, up, offset[m], PITCH);
    b = tex_barrier(tex, DE_ACCESS_COPY_DST, DE_ACCESS_COPY_SRC);
    de_gpu_cmd_barriers(c, &b, 1);
    for (u32 m = 0; m < MIPS; ++m)
        de_gpu_cmd_copy_texture_to_buffer(c, rb, offset[m], PITCH, tex, m, 0);
    EXPECT_TRUE(de_error_last() == NULL);
    submit_wait(d, q, c, fence, 1);

    for (u32 m = 0; m < MIPS; ++m) {
        u32 bad = 0;
        for (u64 r = 0; r < rows[m]; ++r)
            for (u64 k = 0; k < row_bytes[m]; ++k) {
                const u64 i = offset[m] + r * PITCH + k;
                bad += dst[i] != src[i];
            }
        EXPECT_EQ(bad, 0u);
        if (bad)
            printf("mip %u: %u bytes differ\n", m, bad);
    }
    de_gpu_texture_destroy(d, tex);
    de_gpu_buffer_destroy(d, up);
    de_gpu_buffer_destroy(d, rb);
    de_gpu_fence_destroy(d, fence);
}

static void body_cross_queue(de_gpu_device *d) {
    const u64 size = 64 * KIB;
    de_gpu_buffer up = de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = size,
                                                                     .usage = DE_USAGE_COPY_SRC,
                                                                     .memory = DE_MEM_UPLOAD,
                                                                     .name = "xq upload"});
    de_gpu_buffer mid = de_gpu_buffer_create(
        d, &(de_gpu_buffer_desc){
               .size = size, .usage = DE_USAGE_COPY_SRC | DE_USAGE_COPY_DST, .name = "xq gpu"});
    de_gpu_buffer rb = de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = size,
                                                                     .usage = DE_USAGE_COPY_DST,
                                                                     .memory = DE_MEM_READBACK,
                                                                     .name = "xq readback"});
    de_gpu_fence copy_done = de_gpu_fence_create(d, 0);
    de_gpu_fence gfx_done = de_gpu_fence_create(d, 10);
    ASSERT_TRUE(DE_HANDLE_VALID(up) && DE_HANDLE_VALID(mid) && DE_HANDLE_VALID(rb) &&
                DE_HANDLE_VALID(copy_done) && DE_HANDLE_VALID(gfx_done));
    EXPECT_EQ(de_gpu_fence_value(d, gfx_done), 10u);
    u8 *src = (u8 *)de_gpu_buffer_map(d, up);
    u8 *dst = (u8 *)de_gpu_buffer_map(d, rb);
    ASSERT_TRUE(src && dst);
    for (u64 i = 0; i < size; ++i)
        src[i] = (u8)(i * 7u + 3u);
    memset(dst, 0, (size_t)size);

    /* The ownership transfer of `mid` from the copy queue to the graphics queue: the same
     * barrier on both sides (release to COMMON, acquire from COMMON). */
    de_gpu_barrier xfer = buf_barrier(mid, DE_ACCESS_COPY_DST, DE_ACCESS_COPY_SRC);
    xfer.src_queue = DE_QUEUE_COPY;
    xfer.dst_queue = DE_QUEUE_GRAPHICS;

    /* Graphics first: it waits for a value the copy queue has not been asked to signal yet. */
    de_error_clear();
    de_gpu_cmd *g = de_gpu_cmd_begin(d, DE_QUEUE_GRAPHICS, "xq read");
    ASSERT_TRUE(g != NULL);
    de_gpu_cmd_barriers(g, &xfer, 1);
    de_gpu_cmd_copy_buffer(g, rb, 0, mid, 0, size);
    de_gpu_wait w = {.fence = copy_done, .value = 1};
    de_gpu_submit gs = {.cmds = &g,
                        .cmd_count = 1,
                        .waits = &w,
                        .wait_count = 1,
                        .signal = gfx_done,
                        .signal_value = 11};
    ASSERT_EQ(de_gpu_queue_submit(d, DE_QUEUE_GRAPHICS, &gs), DE_OK);
    /* Not done: its wait is not satisfied. Polling times out. */
    EXPECT_EQ(de_gpu_fence_wait(d, gfx_done, 11, 0), DE_ERR_TIMEOUT);
    EXPECT_EQ(de_gpu_fence_value(d, gfx_done), 10u);

    de_gpu_cmd *c = de_gpu_cmd_begin(d, DE_QUEUE_COPY, "xq upload");
    ASSERT_TRUE(c != NULL);
    de_gpu_barrier first = buf_barrier(mid, DE_ACCESS_NONE, DE_ACCESS_COPY_DST);
    de_gpu_cmd_barriers(c, &first, 1);
    de_gpu_cmd_copy_buffer(c, mid, 0, up, 0, size);
    de_gpu_cmd_barriers(c, &xfer, 1);
    EXPECT_TRUE(de_error_last() == NULL);
    de_gpu_submit cs = {.cmds = &c, .cmd_count = 1, .signal = copy_done, .signal_value = 1};
    ASSERT_EQ(de_gpu_queue_submit(d, DE_QUEUE_COPY, &cs), DE_OK);

    ASSERT_EQ(de_gpu_fence_wait(d, gfx_done, 11, WAIT_NS), DE_OK);
    EXPECT_EQ(de_gpu_fence_value(d, gfx_done), 11u);
    EXPECT_GE(de_gpu_fence_value(d, copy_done), 1u);
    u64 bad = 0;
    for (u64 i = 0; i < size; ++i)
        bad += dst[i] != (u8)(i * 7u + 3u);
    EXPECT_EQ(bad, (u64)0);

    /* A transfer recorded on a third queue is an error (Debug and Dev) and records nothing. */
#if DE__GPU_VALIDATE_COMMANDS
    de_gpu_cmd *k = de_gpu_cmd_begin(d, DE_QUEUE_COMPUTE, "xq foreign");
    ASSERT_TRUE(k != NULL);
    de_error_clear();
    de_gpu_cmd_barriers(k, &xfer, 1);
    EXPECT_TRUE(de_error_last() != NULL &&
                strstr(de_error_last()->message, "record it on both of them") != NULL);
    ASSERT_EQ(
        de_gpu_queue_submit(d, DE_QUEUE_COMPUTE, &(de_gpu_submit){.cmds = &k, .cmd_count = 1}),
        DE_OK);
#endif

    /* Lists, waits and destruction recycle: everything is released after wait_idle. */
    de_gpu_buffer_destroy(d, up);
    de_gpu_buffer_destroy(d, mid);
    de_gpu_buffer_destroy(d, rb);
    de_gpu_fence_destroy(d, copy_done);
    de_gpu_fence_destroy(d, gfx_done);
    de_gpu_wait_idle(d);
    EXPECT_EQ(de__gpu_deferred_count(d), 0u);
}

/* DENG-651 diagnostic: a sampled RGBA texture crosses copy->graphics->copy repeatedly.
 * Both queues are real; graphics is submitted first and waits for the upload fence. */
static void body_texture_copy_queue(de_gpu_device *d) {
    enum { W = 64, H = 64, PITCH = W * 4, BYTES = PITCH * H };
    de_gpu_texture tex = de_gpu_texture_create(
        d, &(de_gpu_texture_desc){.format = DE_FMT_RGBA8_UNORM,
                                  .width = W,
                                  .height = H,
                                  .usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST | DE_USAGE_COPY_SRC,
                                  .name = "DENG-651 sampled copy texture"});
    de_gpu_buffer up = de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = BYTES,
                                                                     .usage = DE_USAGE_COPY_SRC,
                                                                     .memory = DE_MEM_UPLOAD,
                                                                     .name = "DENG-651 upload"});
    de_gpu_buffer rb = de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = BYTES,
                                                                     .usage = DE_USAGE_COPY_DST,
                                                                     .memory = DE_MEM_READBACK,
                                                                     .name = "DENG-651 readback"});
    de_gpu_fence copied = de_gpu_fence_create(d, 0), consumed = de_gpu_fence_create(d, 0);
    ASSERT_TRUE(DE_HANDLE_VALID(tex) && DE_HANDLE_VALID(up) && DE_HANDLE_VALID(rb) &&
                DE_HANDLE_VALID(copied) && DE_HANDLE_VALID(consumed));
    u8 *src = de_gpu_buffer_map(d, up), *dst = de_gpu_buffer_map(d, rb);
    ASSERT_TRUE(src && dst);
    de_gpu_barrier to_graphics = tex_barrier(tex, DE_ACCESS_COPY_DST, DE_ACCESS_COPY_SRC);
    to_graphics.src_queue = DE_QUEUE_COPY;
    to_graphics.dst_queue = DE_QUEUE_GRAPHICS;
    de_gpu_barrier to_copy = tex_barrier(tex, DE_ACCESS_COPY_SRC, DE_ACCESS_COPY_DST);
    to_copy.src_queue = DE_QUEUE_GRAPHICS;
    to_copy.dst_queue = DE_QUEUE_COPY;
    for (u32 phase = 0; phase < 3; ++phase) {
        for (u32 i = 0; i < BYTES; ++i)
            src[i] = (u8)(i * 17u + phase * 31u + 7u);
        memset(dst, 0xEE, BYTES);
        de_error_clear();
        de_gpu_cmd *c = de_gpu_cmd_begin(d, DE_QUEUE_COPY, "DENG-651 texture upload");
        ASSERT_TRUE(c != NULL);
        de_gpu_barrier first =
            phase ? to_copy : tex_barrier(tex, DE_ACCESS_NONE, DE_ACCESS_COPY_DST);
        de_gpu_cmd_barriers(c, &first, 1);
        if (!phase)
            print_mode(d); /* Actual mode is selected by that first barrier. */
        de_gpu_cmd_copy_buffer_to_texture(c, tex, 0, 0, up, 0, PITCH);
        de_gpu_cmd_barriers(c, &to_graphics, 1);
        de_gpu_cmd *g = de_gpu_cmd_begin(d, DE_QUEUE_GRAPHICS, "DENG-651 graphics consumer");
        ASSERT_TRUE(g != NULL);
        de_gpu_cmd_barriers(g, &to_graphics, 1);
        de_gpu_cmd_copy_texture_to_buffer(g, rb, 0, PITCH, tex, 0, 0);
        if (phase < 2)
            de_gpu_cmd_barriers(g, &to_copy, 1);
        EXPECT_TRUE(de_error_last() == NULL);
        const de_gpu_wait wait = {.fence = copied, .value = phase + 1u};
        const de_gpu_submit gs = {.cmds = &g,
                                  .cmd_count = 1,
                                  .waits = &wait,
                                  .wait_count = 1,
                                  .signal = consumed,
                                  .signal_value = phase + 1u};
        ASSERT_EQ(de_gpu_queue_submit(d, DE_QUEUE_GRAPHICS, &gs), DE_OK);
        const de_gpu_submit cs = {
            .cmds = &c, .cmd_count = 1, .signal = copied, .signal_value = phase + 1u};
        ASSERT_EQ(de_gpu_queue_submit(d, DE_QUEUE_COPY, &cs), DE_OK);
        ASSERT_EQ(de_gpu_fence_wait(d, consumed, phase + 1u, WAIT_NS), DE_OK);
        u32 bad = 0;
        for (u32 i = 0; i < BYTES; ++i)
            bad += dst[i] != (u8)(i * 17u + phase * 31u + 7u);
        printf("DENG-651 phase%u: %u bytes, %u mismatches, debug errors %u\n", phase, BYTES, bad,
               de__d3d12_debug_error_count(d));
        EXPECT_EQ(bad, 0u);
        EXPECT_EQ(de__d3d12_debug_error_count(d), 0u);
    }
    de_gpu_texture_destroy(d, tex);
    de_gpu_buffer_destroy(d, up);
    de_gpu_buffer_destroy(d, rb);
    de_gpu_fence_destroy(d, copied);
    de_gpu_fence_destroy(d, consumed);
}

/* ---------- Tests ---------- */

TEST_F(d3d12, texture_copy_queue_handoff, cmd_fx) {
    body_texture_copy_queue(f->d);
}
TEST_F(d3d12, legacy_texture_copy_queue_handoff, legacy_fx) {
    body_texture_copy_queue(f->d);
}
TEST_F(d3d12, texture_copy_queue_handoff_api, api_copy_fx) {
    body_texture_copy_queue(f->d);
}
TEST_F(d3d12, legacy_texture_copy_queue_handoff_api, legacy_api_copy_fx) {
    body_texture_copy_queue(f->d);
}

TEST_F(d3d12, copy_queue_bc7_mips, cmd_fx) {
    body_copy_bc7_mips(f->d, DE_QUEUE_COPY);
}
TEST_F(d3d12, legacy_copy_queue_bc7_mips, legacy_fx) {
    body_copy_bc7_mips(f->d, DE_QUEUE_COPY);
}

TEST_F(d3d12, clear_readback, cmd_fx) {
    print_mode(f->d);
    body_clear_readback(f->d);
}

TEST_F(d3d12, copy_bc7_mips, cmd_fx) {
    print_mode(f->d);
    body_copy_bc7_mips(f->d, DE_QUEUE_GRAPHICS);
}

TEST_F(d3d12, cross_queue_timeline, cmd_fx) {
    print_mode(f->d);
    body_cross_queue(f->d);
    /* Fence basics: an infinite wait on a reached value, a wait with a timeout that expires. */
    de_gpu_fence fe = de_gpu_fence_create(f->d, 5);
    ASSERT_TRUE(DE_HANDLE_VALID(fe));
    EXPECT_EQ(de_gpu_fence_wait(f->d, fe, 5, UINT64_MAX), DE_OK);
    EXPECT_EQ(de_gpu_fence_wait(f->d, fe, 6, 1000000u), DE_ERR_TIMEOUT); /* 1 ms */
    ASSERT_EQ(de_gpu_queue_submit(f->d, DE_QUEUE_COMPUTE,
                                  &(de_gpu_submit){.signal = fe, .signal_value = 6}),
              DE_OK);
    EXPECT_EQ(de_gpu_fence_wait(f->d, fe, 6, UINT64_MAX), DE_OK);
    EXPECT_EQ(de_gpu_fence_value(f->d, fe), 6u);
    de_gpu_fence_destroy(f->d, fe);
}

TEST_F(d3d12, legacy_barriers_forced, legacy_fx) {
    /* The mode is chosen by the device's first command list. */
    de_gpu_cmd *c = de_gpu_cmd_begin(f->d, DE_QUEUE_GRAPHICS, "first");
    ASSERT_TRUE(c != NULL);
    ASSERT_EQ(
        de_gpu_queue_submit(f->d, DE_QUEUE_GRAPHICS, &(de_gpu_submit){.cmds = &c, .cmd_count = 1}),
        DE_OK);
    EXPECT_FALSE(de__d3d12_enhanced_barriers_active(f->d));
    print_mode(f->d);
    body_clear_readback(f->d);
    body_copy_bc7_mips(f->d, DE_QUEUE_GRAPHICS);
    body_cross_queue(f->d);
}

/* ---------- Swapchain ---------- */

typedef struct sc_fx {
    cmd_fx gpu;
    bool platform;
    de_window window;
    de_gpu_swapchain sc;
} sc_fx;

static void sc_fx_setup(sc_fx *f) {
    de_platform_desc pd = {0};
    pd.app_name = "dengine test_d3d12_cmd";
    pd.app_org = "DEngine";
    de_result r = de_platform_init(&pd);
    if (r == DE_ERR_UNSUPPORTED)
        SKIP("no video driver (no desktop session)");
    ASSERT_EQ(r, DE_OK);
    f->platform = true;
    de_window_desc wd = {0};
    wd.title = "test_d3d12_cmd";
    wd.width = 320;
    wd.height = 240;
    wd.display_index = -1;
    wd.hidden = true;
    wd.surface = DE_WINDOW_SURFACE_NONE;
    r = de_window_create(&wd, &f->window);
    if (r == DE_ERR_UNSUPPORTED) {
        snprintf(f->gpu.skip, sizeof f->gpu.skip, "no window: %s",
                 errchain(f->gpu.err, sizeof f->gpu.err));
        SKIP(f->gpu.skip);
    }
    ASSERT_EQ(r, DE_OK);
    cmd_fx_setup(&f->gpu);
}

static void sc_fx_teardown(sc_fx *f) {
    if (f->gpu.d && f->sc.id)
        de_gpu_swapchain_destroy(f->gpu.d, f->sc);
    cmd_fx_teardown(&f->gpu);
    if (DE_HANDLE_VALID(f->window))
        de_window_destroy(f->window);
    if (f->platform)
        de_platform_shutdown();
}

/* One frame: acquire, clear to a colour that changes per frame, present. Returns whether the frame
 * was presented; `*image` gets the image used. */
static bool frame(sc_fx *f, u32 n, de_gpu_texture *image) {
    de_gpu_device *d = f->gpu.d;
    de_gpu_texture img = {0};
    de_result r = de_gpu_swapchain_acquire(d, f->sc, &img);
    EXPECT_EQ(r, DE_OK);
    if (r != DE_OK)
        return false;
    *image = img;
    de_gpu_cmd *c = de_gpu_cmd_begin(d, DE_QUEUE_GRAPHICS, "present frame");
    EXPECT_TRUE(c != NULL);
    if (!c)
        return false;
    de_gpu_barrier b = tex_barrier(img, DE_ACCESS_NONE, DE_ACCESS_RENDER_TARGET);
    de_gpu_cmd_barriers(c, &b, 1);
    const f32 t = (f32)(n % 60u) / 60.0f;
    de_gpu_attachment color = {
        .texture = img, .clear = true, .clear_color = {t, 0.25f, 1.0f - t, 1.0f}, .store = true};
    de_gpu_cmd_begin_render(c, &color, 1, NULL);
    de_gpu_cmd_end_render(c);
    b = tex_barrier(img, DE_ACCESS_RENDER_TARGET, DE_ACCESS_PRESENT);
    de_gpu_cmd_barriers(c, &b, 1);
    EXPECT_EQ(
        de_gpu_queue_submit(d, DE_QUEUE_GRAPHICS, &(de_gpu_submit){.cmds = &c, .cmd_count = 1}),
        DE_OK);
    r = de_gpu_swapchain_present(d, f->sc);
    EXPECT_EQ(r, DE_OK);
    de_platform_pump(NULL, 0);
    return r == DE_OK;
}

static void image_size(de_gpu_device *d, de_gpu_texture img, u32 *w, u32 *h) {
    de__gpu_texture *t = de__gpu_get_texture(d, img);
    *w = t ? t->width : 0u;
    *h = t ? t->height : 0u;
}

/* No debug-layer error so far; `step` names where in the test a new one came. */
static void expect_no_debug_errors(de_gpu_device *d, const char *step) {
    const u32 n = de__d3d12_debug_error_count(d);
    if (n)
        printf("debug-layer errors after %s: %u\n", step, n);
    EXPECT_EQ(n, 0u);
}

TEST_F(d3d12, swapchain_present_resize, sc_fx) {
    de_gpu_device *d = f->gpu.d;
    print_mode(d);
    f->sc = de_gpu_swapchain_create(d, &(de_gpu_swapchain_desc){.window = f->window,
                                                                .image_count = 3,
                                                                .present = DE_PRESENT_VSYNC});
    ASSERT_TRUE(DE_HANDLE_VALID(f->sc));
    EXPECT_EQ(de_gpu_swapchain_colorspace(d, f->sc), DE_CS_SRGB);
    /* HDR color spaces come in S18. */
    de_gpu_swapchain hdr = de_gpu_swapchain_create(
        d, &(de_gpu_swapchain_desc){.window = f->window, .colorspace = DE_CS_HDR10_PQ});
    EXPECT_FALSE(DE_HANDLE_VALID(hdr));

    u32 pw = 0, ph = 0;
    de_window_size_pixels(f->window, &pw, &ph);
    u32 presented = 0;
    de_gpu_texture img = {0};
    for (u32 n = 0; n < 120; ++n)
        presented += frame(f, n, &img) ? 1u : 0u;
    u32 w0 = 0, h0 = 0;
    image_size(d, img, &w0, &h0);
    printf("120 frames: %u presented at %ux%u (window %ux%u pixels)\n", presented, w0, h0, pw, ph);
    EXPECT_EQ(presented, 120u);
    EXPECT_EQ(w0, pw);
    EXPECT_EQ(h0, ph);
    /* Acquire without present returns the same image. */
    de_gpu_texture a = {0}, b = {0};
    ASSERT_EQ(de_gpu_swapchain_acquire(d, f->sc, &a), DE_OK);
    ASSERT_EQ(de_gpu_swapchain_acquire(d, f->sc, &b), DE_OK);
    EXPECT_EQ(a.id, b.id);

    /* Resize the window (the host does this on DE_EV_WINDOW_RESIZED), then the swapchain: the old
     * images are gone, new ones come at the window's new pixel size. */
#if defined(DE_D3D12_TEST_SDL)
    SDL_Window *sw = (SDL_Window *)de_window_native(f->window).sdl_window;
    ASSERT_TRUE(sw != NULL);
    EXPECT_TRUE(SDL_SetWindowSize(sw, 200, 150));
    SDL_SyncWindow(sw);
    de_platform_pump(NULL, 0);
#endif
    /* The swapchain takes the window's pixel size (the HWND's client area). */
    u32 pw2 = 0, ph2 = 0;
    de_window_size_pixels(f->window, &pw2, &ph2);
    RECT rc;
    GetClientRect((HWND)de_window_native(f->window).hwnd, &rc);
    printf("after the window resize: de_window_size_pixels %ux%u, client %ldx%ld\n", pw2, ph2,
           (long)(rc.right - rc.left), (long)(rc.bottom - rc.top));
    const de_gpu_texture old = img;
    ASSERT_EQ(de_gpu_swapchain_resize(d, f->sc, pw2, ph2), DE_OK);
    expect_no_debug_errors(d, "the first resize");
    EXPECT_TRUE(de__gpu_get_texture(d, old) == NULL);
    de_gpu_texture fresh = {0};
    ASSERT_EQ(de_gpu_swapchain_acquire(d, f->sc, &fresh), DE_OK);
    EXPECT_TRUE(fresh.id != old.id);
    u32 w1 = 0, h1 = 0;
    image_size(d, fresh, &w1, &h1);
    printf("after resize: images %ux%u\n", w1, h1);
    EXPECT_EQ(w1, pw2);
    EXPECT_EQ(h1, ph2);
#if defined(DE_D3D12_TEST_SDL)
    EXPECT_TRUE(w1 != pw || h1 != ph); /* the size really changed */
#endif
    /* A minimized window (0x0) keeps the images. */
    EXPECT_EQ(de_gpu_swapchain_resize(d, f->sc, 0, 0), DE_OK);
    EXPECT_TRUE(de__gpu_get_texture(d, fresh) != NULL);
    presented = 0;
    for (u32 n = 0; n < 30; ++n)
        presented += frame(f, n, &img) ? 1u : 0u;
    EXPECT_EQ(presented, 30u);

    /* Resize right after a present, with nothing in between: the buffers are released only once
     * that present is done with them (DENG-402). The swapchain's size need not be the window's. */
    for (u32 n = 0; n < 8; ++n) {
        EXPECT_TRUE(frame(f, n, &img));
        const u32 rw = (n & 1u) ? pw2 : pw2 + 16u, rh = (n & 1u) ? ph2 : ph2 + 8u;
        ASSERT_EQ(de_gpu_swapchain_resize(d, f->sc, rw, rh), DE_OK);
        de_gpu_texture cur = {0};
        ASSERT_EQ(de_gpu_swapchain_acquire(d, f->sc, &cur), DE_OK);
        u32 cw = 0, ch = 0;
        image_size(d, cur, &cw, &ch);
        EXPECT_EQ(cw, rw);
        EXPECT_EQ(ch, rh);
    }
    expect_no_debug_errors(d, "the resizes right after a present");

    /* The other present modes; the new swapchain replaces the destroyed one on the same window. */
    static const de_gpu_present k_modes[] = {DE_PRESENT_IMMEDIATE, DE_PRESENT_MAILBOX};
    for (u32 m = 0; m < 2; ++m) {
        de_gpu_swapchain_destroy(d, f->sc);
        f->sc = de_gpu_swapchain_create(
            d, &(de_gpu_swapchain_desc){.window = f->window, .present = k_modes[m]});
        ASSERT_TRUE(DE_HANDLE_VALID(f->sc));
        presented = 0;
        for (u32 n = 0; n < 10; ++n)
            presented += frame(f, n, &img) ? 1u : 0u;
        EXPECT_EQ(presented, 10u);
        expect_no_debug_errors(d, m ? "the MAILBOX frames" : "the IMMEDIATE frames");
    }
    /* Destroy right after a present; wait_idle releases it (deferred destruction). */
    de_gpu_swapchain_destroy(d, f->sc);
    f->sc = (de_gpu_swapchain){0};
    de_gpu_wait_idle(d);
    expect_no_debug_errors(d, "the destroy right after a present");
}
