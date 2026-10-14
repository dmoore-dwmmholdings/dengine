/* test_mtl_cmd.c — Metal command lists, barriers, copies, fences and residency (DENG-288).
 * CTest names: gpu_metal.mtl.<test>.
 *
 * The first three mirror DENG-284's Vulkan tests with the same names and expected values, so the
 * backends compare:
 *   - clear_readback: a 256x256 RGBA8 render target cleared to (0.2, 0.4, 0.6, 1.0) reads back as
 *     bytes (51, 102, 153, 255) in every texel.
 *   - copy_bc7_mips: a 64x64 BC7 texture with 4 mips, filled from a buffer (byte i of mip m's
 *     region = (u8)(i * 13 + 7 + m)), copied back byte for byte.
 *   - cross_queue_timeline: 64 KiB (byte i = (u8)(i * 7 + 3)) copied on the copy queue, read on
 *     the graphics queue after a fence wait submitted first (wait before signal).
 * Then:
 *   - untracked_barrier_1000: 1000 times, one blit encoder fills a 1 MiB GPU_ONLY buffer
 *     (untracked heap) and a later blit encoder of the same command buffer copies its last 256
 *     bytes out, with only de_gpu barriers between them.
 *   - residency_default_path: the default device's path follows the OS and SDK, and a kernel reads
 *     and writes buffers only through the bindless table (Base tier).
 *   - residency_forced_use_heap: the same tests and the kernel on a device made with
 *     gpu.metal.force_use_heap (useHeaps: per encoder, the macOS 14 path).
 *   - timestamp_frequency: caps.timestamp_frequency against GPU timestamp counter samples.
 *   - barrier_translation_table (pure): the access -> render stage table and copy regions.
 *
 * Metal API validation (MTL_DEBUG_LAYER=1, test_mtl_device.c) is on for the whole executable and
 * shader validation (MTL_SHADER_VALIDATION=1, below) for the mtl.* tests: a validation error
 * aborts the test. The device tests skip without a Metal device.
 */
#include "de_test.h"

#include <dengine/de_debug.h>
#include <dengine/de_gpu.h>

#include "gpu/metal/mtl_internal.h"
#include "mtl_cmd_helpers.h"
#include "mtl_test_helpers.h"

#include <crt_externs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Before main and before any Metal call: Metal shader validation too, for the mtl.* tests (CTest
 * runs one test per process with --filter=<suite>.<test>). Not for the whole executable: DENG-287's
 * bindless_gpu_read loses its storage-texture write under shader validation (on dev as well; a
 * follow-up of DENG-288). Not on CI (the CI variable is set): with shader validation on, the
 * paravirtual GPU of GitHub's macOS runners intermittently reported
 * kIOGPUCommandBufferCallbackErrorHang, which also killed other test processes' GPU work. */
__attribute__((constructor)) static void enable_shader_validation(void) {
    if (getenv("CI"))
        return;
    char **argv = *_NSGetArgv();
    const int argc = *_NSGetArgc();
    for (int i = 1; i < argc; ++i)
        if (strncmp(argv[i], "--filter=mtl.", 13) == 0)
            setenv("MTL_SHADER_VALIDATION", "1", 0);
}

#define KIB ((u64)1 << 10)
#define MIB ((u64)1 << 20)
/* Fence waits. Generous: the paravirtual GPU of GitHub's macOS runners stalls for ~10 s at times,
 * across processes (seen in CI: trivial device-create tests took 10 s at the same moment). */
#define WAIT_NS ((u64)60 * 1000 * 1000 * 1000)

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

/* Submits one list on `q` signalling `fence` = value, and waits for it. */
static void submit_wait(de_gpu_device *d, de_gpu_queue q, de_gpu_cmd *c, de_gpu_fence fence,
                        u64 value) {
    de_gpu_submit s = {.cmds = &c, .cmd_count = 1, .signal = fence, .signal_value = value};
    ASSERT_EQ(de_gpu_queue_submit(d, q, &s), DE_OK);
    ASSERT_EQ(de_gpu_fence_wait(d, fence, value, WAIT_NS), DE_OK);
    EXPECT_GE(de_gpu_fence_value(d, fence), value);
}

/* ---------- Test bodies (run on the default device and on a forced useHeaps: device) ---------- */

static void clear_readback_body(de_gpu_device *d) {
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

    /* Twice, so the second pass reuses the recycled command list and its fences. */
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

/* False (nothing run) when the device has no BC formats. */
static bool copy_bc7_mips_body(de_gpu_device *d) {
    enum { SIZE = 64, MIPS = 4, PITCH = 256 };
    de_gpu_texture tex = de_gpu_texture_create(
        d, &(de_gpu_texture_desc){.format = DE_FMT_BC7_UNORM,
                                  .width = SIZE,
                                  .height = SIZE,
                                  .mip_count = MIPS,
                                  .usage = DE_USAGE_COPY_SRC | DE_USAGE_COPY_DST,
                                  .name = "bc7"});
    if (!DE_HANDLE_VALID(tex) && de_error_last() && de_error_last()->result == DE_ERR_UNSUPPORTED)
        return false;
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

    de_gpu_cmd *c = de_gpu_cmd_begin(d, DE_QUEUE_GRAPHICS, "bc7 round trip");
    ASSERT_TRUE(c != NULL);
    de_gpu_barrier b = tex_barrier(tex, DE_ACCESS_NONE, DE_ACCESS_COPY_DST);
    de_gpu_cmd_barriers(c, &b, 1);
    for (u32 m = 0; m < MIPS; ++m)
        de_gpu_cmd_copy_buffer_to_texture(c, tex, m, 0, up, offset[m], PITCH);
    b = tex_barrier(tex, DE_ACCESS_COPY_DST, DE_ACCESS_COPY_SRC);
    de_gpu_cmd_barriers(c, &b, 1);
    for (u32 m = 0; m < MIPS; ++m)
        de_gpu_cmd_copy_texture_to_buffer(c, rb, offset[m], PITCH, tex, m, 0);
    submit_wait(d, DE_QUEUE_GRAPHICS, c, fence, 1);

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
    return true;
}

static void cross_queue_timeline_body(de_gpu_device *d) {
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
     * barrier on both sides (Metal has no queue families: an ordinary barrier). */
    de_gpu_barrier xfer = buf_barrier(mid, DE_ACCESS_COPY_DST, DE_ACCESS_COPY_SRC);
    xfer.src_queue = DE_QUEUE_COPY;
    xfer.dst_queue = DE_QUEUE_GRAPHICS;

    /* Graphics first: it waits for a value the copy queue has not been asked to signal yet. */
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
    de_gpu_submit cs = {.cmds = &c, .cmd_count = 1, .signal = copy_done, .signal_value = 1};
    ASSERT_EQ(de_gpu_queue_submit(d, DE_QUEUE_COPY, &cs), DE_OK);

    ASSERT_EQ(de_gpu_fence_wait(d, gfx_done, 11, WAIT_NS), DE_OK);
    EXPECT_EQ(de_gpu_fence_value(d, gfx_done), 11u);
    EXPECT_GE(de_gpu_fence_value(d, copy_done), 1u);
    u64 bad = 0;
    for (u64 i = 0; i < size; ++i)
        bad += dst[i] != (u8)(i * 7u + 3u);
    EXPECT_EQ(bad, (u64)0);

    /* Lists, waits and destruction recycle: everything is released after wait_idle. */
    de_gpu_buffer_destroy(d, up);
    de_gpu_buffer_destroy(d, mid);
    de_gpu_buffer_destroy(d, rb);
    de_gpu_fence_destroy(d, copy_done);
    de_gpu_fence_destroy(d, gfx_done);
    de_gpu_wait_idle(d);
    EXPECT_EQ(de__gpu_deferred_count(d), 0u);
}

/* 1000 x: fill (blit encoder 1) -> barrier -> copy the tail out (blit encoder 2). 25 iterations
 * per command buffer (25 MiB of fills: light, for the paravirtual GPU of CI, which hung under
 * heavier load), so the barrier at the end of one list orders the next list's fill too. */
static void untracked_barrier_body(de_gpu_device *d, u32 iterations) {
    const u64 big = 1 * MIB, sample = 256;
    de_gpu_buffer mid = de_gpu_buffer_create(
        d, &(de_gpu_buffer_desc){
               .size = big, .usage = DE_USAGE_COPY_SRC | DE_USAGE_COPY_DST, .name = "filled"});
    de_gpu_buffer rb = de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = sample * iterations,
                                                                     .usage = DE_USAGE_COPY_DST,
                                                                     .memory = DE_MEM_READBACK,
                                                                     .name = "samples"});
    de_gpu_fence fence = de_gpu_fence_create(d, 0);
    ASSERT_TRUE(DE_HANDLE_VALID(mid) && DE_HANDLE_VALID(rb) && DE_HANDLE_VALID(fence));
    u8 *out = (u8 *)de_gpu_buffer_map(d, rb);
    ASSERT_TRUE(out != NULL);
    memset(out, 0, (size_t)(sample * iterations));
    void *mid_obj = de__mtl_buffer_object(d, mid);
    ASSERT_TRUE(mid_obj != NULL);

    u64 value = 0;
    de_gpu_cmd *c = NULL;
    for (u32 i = 0; i < iterations; ++i) {
        if (!c) {
            c = de_gpu_cmd_begin(d, DE_QUEUE_GRAPHICS, "fill then copy");
            ASSERT_TRUE(c != NULL);
        }
        de_gpu_barrier b =
            buf_barrier(mid, i ? DE_ACCESS_COPY_SRC : DE_ACCESS_NONE, DE_ACCESS_COPY_DST);
        de_gpu_cmd_barriers(c, &b, 1);
        void *enc = de__mtl_cmd_encoder(c, DE__MTL_ENC_BLIT);
        ASSERT_TRUE(enc != NULL);
        de_test_mtl_fill(enc, mid_obj, 0, big, (u8)(i * 37u + 11u));
        b = buf_barrier(mid, DE_ACCESS_COPY_DST, DE_ACCESS_COPY_SRC);
        de_gpu_cmd_barriers(c, &b, 1);
        de_gpu_cmd_copy_buffer(c, rb, (u64)i * sample, mid, big - sample, sample);
        if (i % 25u == 24u || i + 1u == iterations) {
            de_gpu_submit s = {
                .cmds = &c, .cmd_count = 1, .signal = fence, .signal_value = ++value};
            ASSERT_EQ(de_gpu_queue_submit(d, DE_QUEUE_GRAPHICS, &s), DE_OK);
            c = NULL;
            /* At most two command buffers in flight: on CI's paravirtual GPU, a long queue of
             * command buffers that each wait for the one before tripped its hang watchdog. */
            if (value > 1)
                ASSERT_EQ(de_gpu_fence_wait(d, fence, value - 1, WAIT_NS), DE_OK);
        }
    }
    ASSERT_EQ(de_gpu_fence_wait(d, fence, value, WAIT_NS), DE_OK);
    u32 bad = 0;
    for (u32 i = 0; i < iterations; ++i)
        for (u64 k = 0; k < sample; ++k)
            if (out[(u64)i * sample + k] != (u8)(i * 37u + 11u)) {
                if (bad == 0)
                    printf("iteration %u byte %llu: %u, not %u\n", i, (unsigned long long)k,
                           out[(u64)i * sample + k], (u8)(i * 37u + 11u));
                ++bad;
                break;
            }
    EXPECT_EQ(bad, 0u);
    de_gpu_buffer_destroy(d, mid);
    de_gpu_buffer_destroy(d, rb);
    de_gpu_fence_destroy(d, fence);
}

/* A kernel that reads and writes buffers only through the bindless table, on the backend's
 * compute encoder (its residency), then a barrier, then a copy to the CPU. */
static void bindless_dispatch_body(de_gpu_device *d) {
    enum { N = 4 };
    de_gpu_buffer in = de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = N * 16,
                                                                     .usage = DE_USAGE_CONSTANT,
                                                                     .memory = DE_MEM_UPLOAD,
                                                                     .name = "kernel in"});
    de_gpu_buffer outb =
        de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = N * 16,
                                                      .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                                      .name = "kernel out"});
    de_gpu_buffer rb = de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = N * 16,
                                                                     .usage = DE_USAGE_COPY_DST,
                                                                     .memory = DE_MEM_READBACK,
                                                                     .name = "kernel readback"});
    de_gpu_fence fence = de_gpu_fence_create(d, 0);
    ASSERT_TRUE(DE_HANDLE_VALID(in) && DE_HANDLE_VALID(outb) && DE_HANDLE_VALID(rb) &&
                DE_HANDLE_VALID(fence));
    f32 *src = (f32 *)de_gpu_buffer_map(d, in);
    f32 *dst = (f32 *)de_gpu_buffer_map(d, rb);
    ASSERT_TRUE(src && dst);
    for (u32 i = 0; i < N * 4; ++i)
        src[i] = (f32)i + 0.5f;
    memset(dst, 0, N * 16);

    de_gpu_cmd *c = de_gpu_cmd_begin(d, DE_QUEUE_COMPUTE, "bindless kernel");
    ASSERT_TRUE(c != NULL);
    de_gpu_barrier b = buf_barrier(outb, DE_ACCESS_NONE, DE_ACCESS_SHADER_WRITE);
    de_gpu_cmd_barriers(c, &b, 1);
    void *enc = de__mtl_cmd_encoder(c, DE__MTL_ENC_COMPUTE);
    ASSERT_TRUE(enc != NULL);
    char err[512];
    ASSERT_TRUE(de_test_mtl_dispatch_copy(d, enc, de_gpu_buffer_bindless(d, in),
                                          de_gpu_buffer_bindless(d, outb), N, err,
                                          sizeof err) == 1);
    /* Inside the compute encoder: memoryBarrierWithScope:, then a blit after it waits. */
    b = buf_barrier(outb, DE_ACCESS_SHADER_WRITE, DE_ACCESS_COPY_SRC);
    de_gpu_cmd_barriers(c, &b, 1);
    de_gpu_cmd_copy_buffer(c, rb, 0, outb, 0, N * 16);
    submit_wait(d, DE_QUEUE_COMPUTE, c, fence, 1);
    u32 bad = 0;
    for (u32 i = 0; i < N * 4; ++i)
        bad += dst[i] != ((f32)i + 0.5f) * 2.0f;
    EXPECT_EQ(bad, 0u);
    de_gpu_buffer_destroy(d, in);
    de_gpu_buffer_destroy(d, outb);
    de_gpu_buffer_destroy(d, rb);
    de_gpu_fence_destroy(d, fence);
}

/* ---------- Fixture ---------- */

/* mtl_fx (mtl_test_helpers.h) under the name the tests use. */
typedef mtl_fx cmd_fx;
static void cmd_fx_setup(cmd_fx *f) {
    mtl_fx_setup(f);
}
static void cmd_fx_teardown(cmd_fx *f) {
    mtl_fx_teardown(f);
}

TEST_F(mtl, clear_readback, cmd_fx) {
    clear_readback_body(f->d);
}

TEST_F(mtl, copy_bc7_mips, cmd_fx) {
    if (!copy_bc7_mips_body(f->d))
        SKIP("this Metal device has no BC formats");
}

TEST_F(mtl, cross_queue_timeline, cmd_fx) {
    cross_queue_timeline_body(f->d);
}

TEST_F(mtl, untracked_barrier_1000, cmd_fx) {
    untracked_barrier_body(f->d, 1000);
}

/* ---------- Residency ---------- */

/* The default device: a residency set where the OS has them (macOS 15+) and the build has the
 * path (macOS 15 SDK), useHeaps: otherwise; a kernel reads and writes through the table. */
TEST_F(mtl, residency_default_path, cmd_fx) {
    const bool sets = de_test_mtl_has_residency_sets() && de__mtl_residency_sets_built();
    printf(
        "residency: %s (OS %s residency sets, build %s them)\n",
        sets ? "MTLResidencySet" : "useHeaps:", de_test_mtl_has_residency_sets() ? "has" : "lacks",
        de__mtl_residency_sets_built() ? "has" : "lacks");
    EXPECT_EQ(de__mtl_residency_uses_heaps(f->d), !sets);
    mtl_require_base_tier(f); /* the kernel needs Metal 3 */
    bindless_dispatch_body(f->d);
}

/* The forced macOS 14 path. The cvar is set (as a pending value) before the process's first
 * device, so the test has one device: work on a second Metal device of one process hung the
 * paravirtual GPU of CI (kIOGPUCommandBufferCallbackErrorHang). */
TEST(mtl, residency_forced_use_heap) {
    static const char k_force[] = "gpu.metal.force_use_heap = true\n";
    ASSERT_EQ(de_cvar_load_from_memory(k_force, sizeof k_force - 1), DE_OK);
    cmd_fx fx;
    memset(&fx, 0, sizeof fx);
    cmd_fx *f = &fx;
    cmd_fx_setup(f);
    de_cvar_set(de_cvar_find("gpu.metal.force_use_heap"), "0"); /* later devices: the default */
    EXPECT_TRUE(de__mtl_residency_uses_heaps(f->d));
    clear_readback_body(f->d);
    cross_queue_timeline_body(f->d);
    untracked_barrier_body(f->d, 100);
    if (de_gpu_get_caps(f->d)->base_tier_ok)
        bindless_dispatch_body(f->d);
    else
        printf("below the Base tier (%s): no bindless kernel\n",
               de_gpu_get_caps(f->d)->base_tier_reason);
    if (!copy_bc7_mips_body(f->d))
        printf("no BC formats: no BC7 round trip\n");
    cmd_fx_teardown(f);
}

/* ---------- Timestamps ---------- */

TEST_F(mtl, timestamp_frequency, cmd_fx) {
    const u64 freq = de_gpu_get_caps(f->d)->timestamp_frequency;
    ASSERT_GT(freq, 0u);
    de_test_mtl_timestamps ts;
    char err[512];
    const int r = de_test_mtl_sample_timestamps(f->d, &ts, err, sizeof err);
    if (r < 0)
        SKIP("the device cannot sample GPU timestamps at stage boundaries");
    ASSERT_TRUE(r == 1);
    const double counted = (double)(ts.end - ts.start) / (double)freq;
    printf("timestamps: counter %llu..%llu (%.3f ms at %llu Hz), command buffer %.3f ms, "
           "sampleTimestamps %llu..%llu\n",
           (unsigned long long)ts.start, (unsigned long long)ts.end, counted * 1e3,
           (unsigned long long)freq, ts.gpu_seconds * 1e3, (unsigned long long)ts.gpu_before,
           (unsigned long long)ts.gpu_after);
    EXPECT_GT(ts.end, ts.start);
    /* Counter samples count in the same clock as sampleTimestamps' GPU time... */
    EXPECT_GE(ts.start, ts.gpu_before);
    EXPECT_LE(ts.end, ts.gpu_after);
    /* ...and at caps.timestamp_frequency: the encoder takes most of the command buffer. */
    EXPECT_TRUE(counted <= ts.gpu_seconds * 1.05 + 0.0005);
    EXPECT_TRUE(counted >= ts.gpu_seconds * 0.5 - 0.0005);
}

/* ---------- Pure ---------- */

TEST(mtl, barrier_translation_table) {
    const u32 v = DE__MTL_STAGE_VERTEX, fr = DE__MTL_STAGE_FRAGMENT, o = DE__MTL_STAGE_OBJECT,
              me = DE__MTL_STAGE_MESH, all = DE__MTL_STAGES_ALL;
    EXPECT_EQ(de__mtl_access_stages(DE_ACCESS_NONE), 0u);
    EXPECT_EQ(de__mtl_access_stages(DE_ACCESS_SHADER_READ), all);
    EXPECT_EQ(de__mtl_access_stages(DE_ACCESS_SHADER_WRITE), all);
    EXPECT_EQ(de__mtl_access_stages(DE_ACCESS_RENDER_TARGET), fr);
    EXPECT_EQ(de__mtl_access_stages(DE_ACCESS_DEPTH_READ), fr);
    EXPECT_EQ(de__mtl_access_stages(DE_ACCESS_DEPTH_WRITE), fr);
    EXPECT_EQ(de__mtl_access_stages(DE_ACCESS_INDIRECT), v | o | me);
    EXPECT_EQ(de__mtl_access_stages(DE_ACCESS_COPY_SRC), 0u);
    EXPECT_EQ(de__mtl_access_stages(DE_ACCESS_COPY_DST), 0u);
    EXPECT_EQ(de__mtl_access_stages(DE_ACCESS_PRESENT), 0u);
    EXPECT_EQ(de__mtl_access_stages(DE_ACCESS_ACCEL_READ), all);
    EXPECT_EQ(de__mtl_access_stages(DE_ACCESS_ACCEL_WRITE), 0u);
    EXPECT_EQ(de__mtl_access_stages(DE_ACCESS_RENDER_TARGET | DE_ACCESS_INDIRECT), v | o | me | fr);

    /* The access pairs the GPU tests use (as on Vulkan and D3D12), on a device without mesh
     * stages and on one with them. */
    static const struct {
        u32 before, after, stages_before, stages_after;
    } k_pairs[] = {
        {DE_ACCESS_NONE, DE_ACCESS_RENDER_TARGET, 0, DE__MTL_STAGE_FRAGMENT},
        {DE_ACCESS_RENDER_TARGET, DE_ACCESS_COPY_SRC, DE__MTL_STAGE_FRAGMENT, 0},
        {DE_ACCESS_NONE, DE_ACCESS_COPY_DST, 0, 0},
        {DE_ACCESS_COPY_DST, DE_ACCESS_COPY_SRC, 0, 0},
        {DE_ACCESS_RENDER_TARGET, DE_ACCESS_PRESENT, DE__MTL_STAGE_FRAGMENT, 0},
        {DE_ACCESS_SHADER_WRITE, DE_ACCESS_SHADER_READ, DE__MTL_STAGES_ALL, DE__MTL_STAGES_ALL},
    };
    for (u32 i = 0; i < sizeof k_pairs / sizeof k_pairs[0]; ++i) {
        de_gpu_barrier b;
        memset(&b, 0, sizeof b);
        b.texture.id = 1;
        b.before = k_pairs[i].before;
        b.after = k_pairs[i].after;
        u32 bs = 99, as = 99;
        de__mtl_barrier_stages(&b, 1, all, &bs, &as);
        EXPECT_EQ(bs, k_pairs[i].stages_before);
        EXPECT_EQ(as, k_pairs[i].stages_after);
        de__mtl_barrier_stages(&b, 1, v | fr, &bs, &as); /* no object/mesh stages */
        EXPECT_EQ(bs, k_pairs[i].stages_before & (v | fr));
        EXPECT_EQ(as, k_pairs[i].stages_after & (v | fr));
    }
    /* Several barriers: the union of their stages. */
    de_gpu_barrier two[2];
    memset(two, 0, sizeof two);
    two[0].texture.id = 1;
    two[0].before = DE_ACCESS_RENDER_TARGET;
    two[0].after = DE_ACCESS_SHADER_READ;
    two[1].buffer.id = 2;
    two[1].before = DE_ACCESS_COPY_DST;
    two[1].after = DE_ACCESS_INDIRECT;
    u32 bs = 0, as = 0;
    de__mtl_barrier_stages(two, 2, all, &bs, &as);
    EXPECT_EQ(bs, fr);
    EXPECT_EQ(as, all);

    /* Buffer <-> texture copy regions: BC rows are rows of 4x4 blocks, sizes in texels. */
    de__gpu_texture t;
    memset(&t, 0, sizeof t);
    t.dim = DE_TEX_2D;
    t.format = DE_FMT_BC7_UNORM;
    t.width = t.height = 64;
    t.depth_or_layers = 1;
    t.mip_count = 4;
    de__mtl_copy_region r = de__mtl_copy_region_of(&t, 2, 0, 256);
    EXPECT_EQ(r.width, 16u);
    EXPECT_EQ(r.height, 16u);
    EXPECT_EQ(r.depth, 1u);
    EXPECT_EQ(r.slice, 0u);
    EXPECT_EQ(r.bytes_per_row, 256u);
    EXPECT_EQ(r.bytes_per_image, 256u * 4u);
    t.width = t.height = 8;
    r = de__mtl_copy_region_of(&t, 2, 0, 256); /* 2x2: one partial block row */
    EXPECT_EQ(r.width, 2u);
    EXPECT_EQ(r.height, 2u);
    EXPECT_EQ(r.bytes_per_image, 256u);
    t.format = DE_FMT_RGBA8_UNORM;
    t.dim = DE_TEX_2D_ARRAY;
    t.width = t.height = 128;
    t.depth_or_layers = 4;
    r = de__mtl_copy_region_of(&t, 0, 3, 512);
    EXPECT_EQ(r.slice, 3u);
    EXPECT_EQ(r.bytes_per_image, 512u * 128u);
    t.dim = DE_TEX_3D;
    t.format = DE_FMT_R8_UNORM;
    t.depth_or_layers = 8;
    r = de__mtl_copy_region_of(&t, 1, 5, 256);
    EXPECT_EQ(r.slice, 0u);
    EXPECT_EQ(r.depth, 4u);
    EXPECT_EQ(r.width, 64u);
    EXPECT_EQ(r.bytes_per_image, 256u * 64u);
}
