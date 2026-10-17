/* test_vk_cmd.c — Vulkan command lists, barriers, copies and timeline fences (DENG-284).
 * CTest names: gpu_vulkan.vk.<test>.
 *
 * The GPU tests use only the public de_gpu API (plus the validation counter), so DENG-286 (D3D12)
 * and DENG-288 (Metal) can mirror them with the same names and expected values:
 *   - clear_readback: a 256x256 RGBA8 render target cleared to (0.2, 0.4, 0.6, 1.0) reads back as
 *     bytes (51, 102, 153, 255) in every texel.
 *   - copy_bc7_mips: a 64x64 BC7 texture with 4 mips, filled from a buffer (byte i of mip m's
 *     region = (u8)(i * 13 + 7 + m)), copied back byte for byte.
 *   - cross_queue_timeline: 64 KiB (byte i = (u8)(i * 7 + 3)) copied on the copy queue, read on
 *     the graphics queue after a fence wait submitted first (wait before signal).
 * They run on the best Base-tier device, or, when there is none (MoltenVK), on adapter 1 forced:
 * command lists, barriers and copies do not need the bindless tables. Every device has the debug
 * layer and GPU-assisted + synchronization validation on; with VK_LAYER_KHRONOS_validation
 * installed they require zero validation errors. Without a Vulkan driver they SKIP.
 *
 * barrier_translation_table is pure (no device): the de_gpu_access -> stage/access/layout table,
 * ownership transfers, subresource ranges and buffer/image copy regions.
 */
#include "de_test.h"

#include <dengine/de_gpu.h>
#include <dengine/de_platform.h>

#include "gpu/vulkan/vk_internal.h"

#include <stdio.h>
#include <string.h>

#define KIB ((u64)1 << 10)

/* The whole error chain ("outer: inner: ..."), for skip reasons. */
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

/* A Vulkan device with validation on a tracking allocator: the best Base-tier device, else
 * adapter 1 forced (these tests need no bindless table). */
typedef struct cmd_fx {
    de_tracking_allocator tracker;
    de_allocator alloc;
    de_gpu_device *d;
    char skip[1200];
    char err[1100];
} cmd_fx;

static void cmd_fx_setup(cmd_fx *f) {
    f->alloc = de_tracking_allocator_init(&f->tracker, NULL, DE_MEM_RENDER);
    de_gpu_device_desc desc = {.backend = DE_GPU_VULKAN,
                               .debug_layer = true,
                               .gpu_validation = true,
                               .allocator = &f->alloc};
    de_result r = de_gpu_device_create(&desc, &f->d);
    if (r == DE_ERR_UNSUPPORTED) {
        desc.adapter = 1; /* not Base-tier capable (MoltenVK): force it */
        r = de_gpu_device_create(&desc, &f->d);
    }
    if (r == DE_ERR_UNSUPPORTED) {
        snprintf(f->skip, sizeof f->skip, "no usable Vulkan device: %s",
                 errchain(f->err, sizeof f->err));
        SKIP(f->skip);
    }
    ASSERT_EQ(r, DE_OK);
    printf("device: \"%s\"%s; validation %s\n", de_gpu_get_caps(f->d)->device_name,
           desc.adapter ? " (forced: not Base-tier capable)" : "",
           de__gpu_vk_validation_enabled(f->d) ? "on (with synchronization validation)"
                                               : "NOT installed: not checked");
}

static void cmd_fx_teardown(cmd_fx *f) {
    if (f->d) {
        u32 warnings = 0;
        EXPECT_EQ(de__gpu_vk_validation_messages(f->d, &warnings), 0u);
        de_gpu_device_destroy(f->d);
    }
    de_tracking_stats st = de_tracking_allocator_stats(&f->tracker);
    EXPECT_EQ(st.blocks_live, 0u);
    EXPECT_EQ(st.bytes_live, 0u);
}

#define WAIT_NS ((u64)10 * 1000 * 1000 * 1000)

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

/* ---------- Clear + readback ---------- */

TEST_F(vk, clear_readback, cmd_fx) {
    enum { W = 256, H = 256, PITCH = W * 4 };
    de_gpu_texture tex = de_gpu_texture_create(
        f->d, &(de_gpu_texture_desc){.format = DE_FMT_RGBA8_UNORM,
                                     .width = W,
                                     .height = H,
                                     .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_COPY_SRC,
                                     .name = "clear target"});
    de_gpu_buffer rb = de_gpu_buffer_create(f->d, &(de_gpu_buffer_desc){.size = (u64)PITCH * H,
                                                                        .usage = DE_USAGE_COPY_DST,
                                                                        .memory = DE_MEM_READBACK,
                                                                        .name = "readback"});
    de_gpu_fence fence = de_gpu_fence_create(f->d, 0);
    ASSERT_TRUE(DE_HANDLE_VALID(tex) && DE_HANDLE_VALID(rb) && DE_HANDLE_VALID(fence));
    EXPECT_EQ(de_gpu_fence_value(f->d, fence), 0u);
    u8 *out = (u8 *)de_gpu_buffer_map(f->d, rb);
    ASSERT_TRUE(out != NULL);
    memset(out, 0xEE, (size_t)PITCH * H);

    /* Twice, so the second pass reuses the recycled command list and its pool. */
    for (u32 pass = 0; pass < 2; ++pass) {
        de_error_clear();
        de_gpu_cmd *c = de_gpu_cmd_begin(f->d, DE_QUEUE_GRAPHICS, "clear_readback");
        ASSERT_TRUE(c != NULL);
        de_gpu_barrier b = tex_barrier(tex, DE_ACCESS_NONE, DE_ACCESS_RENDER_TARGET);
        de_gpu_cmd_barriers(c, &b, 1);
        de_gpu_attachment color = {
            .texture = tex, .clear = true, .clear_color = {0.2f, 0.4f, 0.6f, 1.0f}, .store = true};
        de_gpu_cmd_begin_render(c, &color, 1, NULL);
        de_gpu_cmd_viewport(c, 0.0f, 0.0f, (f32)W, (f32)H);
        de_gpu_cmd_end_render(c);
        /* The readback buffer from its last use (pass 0's copy, waited for on the host): Vulkan
         * needs no barrier after that host wait, but synchronization validation (VVL 1.3.275,
         * CI's lavapipe) does not see timeline-semaphore host waits and reports a
         * WRITE_AFTER_WRITE hazard without it (DENG-426). */
        de_gpu_barrier bb[2] = {tex_barrier(tex, DE_ACCESS_RENDER_TARGET, DE_ACCESS_COPY_SRC),
                                buf_barrier(rb, DE_ACCESS_COPY_DST, DE_ACCESS_COPY_DST)};
        de_gpu_cmd_barriers(c, bb, 2);
        de_gpu_cmd_copy_texture_to_buffer(c, rb, 0, PITCH, tex, 0, 0);
        EXPECT_TRUE(de_error_last() == NULL); /* every command passed validation */
        submit_wait(f->d, DE_QUEUE_GRAPHICS, c, fence, pass + 1u);
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
    de_gpu_texture_destroy(f->d, tex);
    de_gpu_buffer_destroy(f->d, rb);
    de_gpu_fence_destroy(f->d, fence);
}

/* ---------- BC7 copy round trip ---------- */

/* Different VkBuffers can share a retired heap range. A host fence wait alone must not
 * leave synchronization validation seeing the previous alias's transfer write unbarriered. */
TEST_F(vk, readback_allocation_reuse, cmd_fx) {
    enum { BYTES = 4096, REPEATS = 8 };
    de_gpu_buffer upload = de_gpu_buffer_create(
        f->d,
        &(de_gpu_buffer_desc){.size = BYTES, .memory = DE_MEM_UPLOAD, .usage = DE_USAGE_COPY_SRC});
    de_gpu_fence fence = de_gpu_fence_create(f->d, 0);
    ASSERT_TRUE(upload.id && fence.id);
    u8 *source = de_gpu_buffer_map(f->d, upload);
    ASSERT_TRUE(source != NULL);
    uintptr_t addresses[REPEATS];
    bool reused = false;
    for (u32 pass = 0; pass < REPEATS; ++pass) {
        de__gpu_collect(f->d);
        de_gpu_buffer readback = de_gpu_buffer_create(
            f->d, &(de_gpu_buffer_desc){
                      .size = BYTES, .memory = DE_MEM_READBACK, .usage = DE_USAGE_COPY_DST});
        ASSERT_TRUE(readback.id);
        for (u32 i = 0; i < BYTES; ++i)
            source[i] = (u8)(i * 7u + pass * 31u);
        de_gpu_cmd *cmd = de_gpu_cmd_begin(f->d, DE_QUEUE_GRAPHICS, "readback allocation reuse");
        ASSERT_TRUE(cmd != NULL);
        de_gpu_barrier barrier = buf_barrier(readback, DE_ACCESS_NONE, DE_ACCESS_COPY_DST);
        de_gpu_cmd_barriers(cmd, &barrier, 1);
        de_gpu_cmd_copy_buffer(cmd, readback, 0, upload, 0, BYTES);
        submit_wait(f->d, DE_QUEUE_GRAPHICS, cmd, fence, pass + 1u);
        const void *mapped = de_gpu_buffer_map(f->d, readback);
        ASSERT_TRUE(mapped != NULL);
        EXPECT_MEMEQ(mapped, source, BYTES);
        addresses[pass] = (uintptr_t)mapped;
        for (u32 prior = 0; prior < pass; ++prior)
            reused |= addresses[pass] == addresses[prior];
        de_gpu_buffer_destroy(f->d, readback);
    }
    EXPECT_TRUE(reused);
    de_gpu_buffer_destroy(f->d, upload);
    de_gpu_fence_destroy(f->d, fence);
}

TEST_F(vk, copy_bc7_mips, cmd_fx) {
    enum { SIZE = 64, MIPS = 4, PITCH = 256 };
    de_gpu_texture tex = de_gpu_texture_create(
        f->d, &(de_gpu_texture_desc){.format = DE_FMT_BC7_UNORM,
                                     .width = SIZE,
                                     .height = SIZE,
                                     .mip_count = MIPS,
                                     .usage = DE_USAGE_COPY_SRC | DE_USAGE_COPY_DST,
                                     .name = "bc7"});
    if (!DE_HANDLE_VALID(tex) && de_error_last() && de_error_last()->result == DE_ERR_UNSUPPORTED)
        SKIP("this Vulkan device has no BC formats");
    ASSERT_TRUE(DE_HANDLE_VALID(tex));
    /* Mip m: (64 >> m) / 4 rows of blocks, 256-byte rows, regions at 512-byte offsets. */
    u64 offset[MIPS], rows[MIPS], row_bytes[MIPS], total = 0;
    for (u32 m = 0; m < MIPS; ++m) {
        offset[m] = total;
        rows[m] = (u64)(SIZE >> m) / 4u;
        row_bytes[m] = rows[m] * 16u; /* blocks per row = rows (square), 16 bytes each */
        total += (rows[m] * PITCH + 511u) & ~(u64)511u;
    }
    de_gpu_buffer up = de_gpu_buffer_create(f->d, &(de_gpu_buffer_desc){.size = total,
                                                                        .usage = DE_USAGE_COPY_SRC,
                                                                        .memory = DE_MEM_UPLOAD,
                                                                        .name = "bc7 upload"});
    de_gpu_buffer rb = de_gpu_buffer_create(f->d, &(de_gpu_buffer_desc){.size = total,
                                                                        .usage = DE_USAGE_COPY_DST,
                                                                        .memory = DE_MEM_READBACK,
                                                                        .name = "bc7 readback"});
    de_gpu_fence fence = de_gpu_fence_create(f->d, 0);
    ASSERT_TRUE(DE_HANDLE_VALID(up) && DE_HANDLE_VALID(rb) && DE_HANDLE_VALID(fence));
    u8 *src = (u8 *)de_gpu_buffer_map(f->d, up);
    u8 *dst = (u8 *)de_gpu_buffer_map(f->d, rb);
    ASSERT_TRUE(src && dst);
    for (u32 m = 0; m < MIPS; ++m)
        for (u64 i = 0; i < rows[m] * PITCH; ++i)
            src[offset[m] + i] = (u8)(i * 13u + 7u + m);
    memset(dst, 0, (size_t)total);

    de_gpu_cmd *c = de_gpu_cmd_begin(f->d, DE_QUEUE_GRAPHICS, "bc7 round trip");
    ASSERT_TRUE(c != NULL);
    de_gpu_barrier b = tex_barrier(tex, DE_ACCESS_NONE, DE_ACCESS_COPY_DST);
    de_gpu_cmd_barriers(c, &b, 1);
    for (u32 m = 0; m < MIPS; ++m)
        de_gpu_cmd_copy_buffer_to_texture(c, tex, m, 0, up, offset[m], PITCH);
    b = tex_barrier(tex, DE_ACCESS_COPY_DST, DE_ACCESS_COPY_SRC);
    de_gpu_cmd_barriers(c, &b, 1);
    for (u32 m = 0; m < MIPS; ++m)
        de_gpu_cmd_copy_texture_to_buffer(c, rb, offset[m], PITCH, tex, m, 0);
    submit_wait(f->d, DE_QUEUE_GRAPHICS, c, fence, 1);

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
    de_gpu_texture_destroy(f->d, tex);
    de_gpu_buffer_destroy(f->d, up);
    de_gpu_buffer_destroy(f->d, rb);
    de_gpu_fence_destroy(f->d, fence);
}

/* ---------- Cross-queue timeline ---------- */

TEST_F(vk, cross_queue_timeline, cmd_fx) {
    const u64 size = 64 * KIB;
    de_gpu_buffer up = de_gpu_buffer_create(f->d, &(de_gpu_buffer_desc){.size = size,
                                                                        .usage = DE_USAGE_COPY_SRC,
                                                                        .memory = DE_MEM_UPLOAD,
                                                                        .name = "xq upload"});
    de_gpu_buffer mid = de_gpu_buffer_create(
        f->d, &(de_gpu_buffer_desc){
                  .size = size, .usage = DE_USAGE_COPY_SRC | DE_USAGE_COPY_DST, .name = "xq gpu"});
    de_gpu_buffer rb = de_gpu_buffer_create(f->d, &(de_gpu_buffer_desc){.size = size,
                                                                        .usage = DE_USAGE_COPY_DST,
                                                                        .memory = DE_MEM_READBACK,
                                                                        .name = "xq readback"});
    de_gpu_fence copy_done = de_gpu_fence_create(f->d, 0);
    de_gpu_fence gfx_done = de_gpu_fence_create(f->d, 10);
    ASSERT_TRUE(DE_HANDLE_VALID(up) && DE_HANDLE_VALID(mid) && DE_HANDLE_VALID(rb) &&
                DE_HANDLE_VALID(copy_done) && DE_HANDLE_VALID(gfx_done));
    EXPECT_EQ(de_gpu_fence_value(f->d, gfx_done), 10u);
    u8 *src = (u8 *)de_gpu_buffer_map(f->d, up);
    u8 *dst = (u8 *)de_gpu_buffer_map(f->d, rb);
    ASSERT_TRUE(src && dst);
    for (u64 i = 0; i < size; ++i)
        src[i] = (u8)(i * 7u + 3u);
    memset(dst, 0, (size_t)size);

    /* The ownership transfer of `mid` from the copy queue to the graphics queue: the same
     * barrier on both sides (a no-op when both queues share a family). */
    de_gpu_barrier xfer = buf_barrier(mid, DE_ACCESS_COPY_DST, DE_ACCESS_COPY_SRC);
    xfer.src_queue = DE_QUEUE_COPY;
    xfer.dst_queue = DE_QUEUE_GRAPHICS;

    /* Graphics first: it waits for a value the copy queue has not been asked to signal yet. */
    de_gpu_cmd *g = de_gpu_cmd_begin(f->d, DE_QUEUE_GRAPHICS, "xq read");
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
    ASSERT_EQ(de_gpu_queue_submit(f->d, DE_QUEUE_GRAPHICS, &gs), DE_OK);
    /* Not done: its wait is not satisfied. Polling times out. */
    EXPECT_EQ(de_gpu_fence_wait(f->d, gfx_done, 11, 0), DE_ERR_TIMEOUT);
    EXPECT_EQ(de_gpu_fence_value(f->d, gfx_done), 10u);

    de_gpu_cmd *c = de_gpu_cmd_begin(f->d, DE_QUEUE_COPY, "xq upload");
    ASSERT_TRUE(c != NULL);
    de_gpu_barrier first = buf_barrier(mid, DE_ACCESS_NONE, DE_ACCESS_COPY_DST);
    de_gpu_cmd_barriers(c, &first, 1);
    de_gpu_cmd_copy_buffer(c, mid, 0, up, 0, size);
    de_gpu_cmd_barriers(c, &xfer, 1);
    de_gpu_submit cs = {.cmds = &c, .cmd_count = 1, .signal = copy_done, .signal_value = 1};
    ASSERT_EQ(de_gpu_queue_submit(f->d, DE_QUEUE_COPY, &cs), DE_OK);

    ASSERT_EQ(de_gpu_fence_wait(f->d, gfx_done, 11, WAIT_NS), DE_OK);
    EXPECT_EQ(de_gpu_fence_value(f->d, gfx_done), 11u);
    EXPECT_GE(de_gpu_fence_value(f->d, copy_done), 1u);
    u64 bad = 0;
    for (u64 i = 0; i < size; ++i)
        bad += dst[i] != (u8)(i * 7u + 3u);
    EXPECT_EQ(bad, (u64)0);

    /* Lists, waits and destruction recycle: everything is released after wait_idle. */
    de_gpu_buffer_destroy(f->d, up);
    de_gpu_buffer_destroy(f->d, mid);
    de_gpu_buffer_destroy(f->d, rb);
    de_gpu_fence_destroy(f->d, copy_done);
    de_gpu_fence_destroy(f->d, gfx_done);
    de_gpu_wait_idle(f->d);
    EXPECT_EQ(de__gpu_deferred_count(f->d), 0u);
}

/* ---------- Pure: the barrier table ---------- */

#define S2(x) VK_PIPELINE_STAGE_2_##x
#define A2(x) VK_ACCESS_2_##x
#define L(x)  VK_IMAGE_LAYOUT_##x

static de__vk_barrier_ctx graphics_ctx(void) {
    de__vk_barrier_ctx c;
    memset(&c, 0, sizeof c);
    c.queue_stages = ~(VkPipelineStageFlags2)0 & ~S2(RAY_TRACING_SHADER_BIT_KHR) &
                     ~S2(ACCELERATION_STRUCTURE_BUILD_BIT_KHR) & ~S2(TASK_SHADER_BIT_EXT) &
                     ~S2(MESH_SHADER_BIT_EXT);
    c.separate_depth_layouts = true;
    c.family[DE_QUEUE_GRAPHICS] = 0;
    c.family[DE_QUEUE_COMPUTE] = 1;
    c.family[DE_QUEUE_COPY] = 2;
    c.cmd_family = 0;
    c.cmd_queue = DE_QUEUE_GRAPHICS;
    return c;
}

static de__vk_barrier_ctx copy_ctx(void) {
    de__vk_barrier_ctx c = graphics_ctx();
    c.queue_stages = S2(ALL_TRANSFER_BIT) | S2(ALL_COMMANDS_BIT) | S2(HOST_BIT);
    c.cmd_family = 2;
    c.cmd_queue = DE_QUEUE_COPY;
    return c;
}

TEST(vk, barrier_translation_table) {
    const de__vk_barrier_ctx g = graphics_ctx();
    const VkPipelineStageFlags2 shaders =
        S2(PRE_RASTERIZATION_SHADERS_BIT) | S2(FRAGMENT_SHADER_BIT) | S2(COMPUTE_SHADER_BIT);
    de__vk_access_info a;

    /* One bit at a time, images (after side). */
    a = de__vk_access_info_of(&g, DE_ACCESS_RENDER_TARGET, true, false, false, false);
    EXPECT_EQ(a.stages, S2(COLOR_ATTACHMENT_OUTPUT_BIT));
    EXPECT_EQ(a.access, A2(COLOR_ATTACHMENT_READ_BIT) | A2(COLOR_ATTACHMENT_WRITE_BIT));
    EXPECT_EQ(a.layout, L(COLOR_ATTACHMENT_OPTIMAL));
    a = de__vk_access_info_of(&g, DE_ACCESS_SHADER_READ, true, false, false, false);
    EXPECT_EQ(a.stages, shaders);
    EXPECT_EQ(a.access, A2(SHADER_SAMPLED_READ_BIT) | A2(SHADER_STORAGE_READ_BIT));
    EXPECT_EQ(a.layout, L(SHADER_READ_ONLY_OPTIMAL)); /* bindless binding 0 */
    a = de__vk_access_info_of(&g, DE_ACCESS_SHADER_WRITE, true, false, false, false);
    EXPECT_EQ(a.stages, shaders);
    EXPECT_EQ(a.access, A2(SHADER_STORAGE_READ_BIT) | A2(SHADER_STORAGE_WRITE_BIT));
    EXPECT_EQ(a.layout, L(GENERAL)); /* bindless binding 1 */
    a = de__vk_access_info_of(&g, DE_ACCESS_DEPTH_WRITE, true, true, false, false);
    EXPECT_EQ(a.stages, S2(EARLY_FRAGMENT_TESTS_BIT) | S2(LATE_FRAGMENT_TESTS_BIT));
    EXPECT_EQ(a.access,
              A2(DEPTH_STENCIL_ATTACHMENT_READ_BIT) | A2(DEPTH_STENCIL_ATTACHMENT_WRITE_BIT));
    EXPECT_EQ(a.layout, L(DEPTH_ATTACHMENT_OPTIMAL));
    a = de__vk_access_info_of(&g, DE_ACCESS_DEPTH_WRITE, true, true, true, false);
    EXPECT_EQ(a.layout, L(DEPTH_STENCIL_ATTACHMENT_OPTIMAL)); /* depth + stencil format */
    a = de__vk_access_info_of(&g, DE_ACCESS_DEPTH_READ, true, true, false, false);
    EXPECT_EQ(a.access, A2(DEPTH_STENCIL_ATTACHMENT_READ_BIT));
    EXPECT_EQ(a.layout, L(DEPTH_READ_ONLY_OPTIMAL));
    a = de__vk_access_info_of(&g, DE_ACCESS_COPY_SRC, true, false, false, false);
    EXPECT_EQ(a.stages, S2(ALL_TRANSFER_BIT));
    EXPECT_EQ(a.access, A2(TRANSFER_READ_BIT));
    EXPECT_EQ(a.layout, L(TRANSFER_SRC_OPTIMAL));
    a = de__vk_access_info_of(&g, DE_ACCESS_COPY_DST, true, false, false, false);
    EXPECT_EQ(a.access, A2(TRANSFER_WRITE_BIT));
    EXPECT_EQ(a.layout, L(TRANSFER_DST_OPTIMAL));
    a = de__vk_access_info_of(&g, DE_ACCESS_PRESENT, true, false, false, false);
    EXPECT_EQ(a.stages, (VkPipelineStageFlags2)0);
    EXPECT_EQ(a.access, (VkAccessFlags2)0);
    EXPECT_EQ(a.layout, L(PRESENT_SRC_KHR));
    a = de__vk_access_info_of(&g, DE_ACCESS_NONE, true, false, false, false);
    EXPECT_EQ(a.stages, (VkPipelineStageFlags2)0);
    EXPECT_EQ(a.layout, L(UNDEFINED));

    /* Source side: NONE / PRESENT on a texture wait for every earlier command. */
    a = de__vk_access_info_of(&g, DE_ACCESS_NONE, true, false, false, true);
    EXPECT_EQ(a.stages, S2(ALL_COMMANDS_BIT));
    EXPECT_EQ(a.access, (VkAccessFlags2)0);
    a = de__vk_access_info_of(&g, DE_ACCESS_PRESENT, true, false, false, true);
    EXPECT_EQ(a.stages, S2(ALL_COMMANDS_BIT));
    EXPECT_EQ(a.layout, L(PRESENT_SRC_KHR));
    a = de__vk_access_info_of(&g, DE_ACCESS_NONE, false, false, false, true);
    EXPECT_EQ(a.stages, (VkPipelineStageFlags2)0); /* buffers: nothing to wait for */

    /* Buffers. */
    a = de__vk_access_info_of(&g, DE_ACCESS_SHADER_READ, false, false, false, false);
    EXPECT_EQ(a.stages, shaders | S2(VERTEX_ATTRIBUTE_INPUT_BIT) | S2(INDEX_INPUT_BIT));
    EXPECT_EQ(a.access, A2(SHADER_STORAGE_READ_BIT) | A2(UNIFORM_READ_BIT) |
                            A2(VERTEX_ATTRIBUTE_READ_BIT) | A2(INDEX_READ_BIT));
    a = de__vk_access_info_of(&g, DE_ACCESS_INDIRECT, false, false, false, false);
    EXPECT_EQ(a.stages, S2(DRAW_INDIRECT_BIT));
    EXPECT_EQ(a.access, A2(INDIRECT_COMMAND_READ_BIT));
    /* No acceleration structures on this device: the bits add nothing. */
    a = de__vk_access_info_of(&g, DE_ACCESS_ACCEL_READ, false, false, false, false);
    EXPECT_EQ(a.stages, (VkPipelineStageFlags2)0);

    /* Combinations: a common layout, the read-only depth layout, else GENERAL. */
    a = de__vk_access_info_of(&g, DE_ACCESS_SHADER_READ | DE_ACCESS_DEPTH_READ, true, true, false,
                              false);
    EXPECT_EQ(a.layout, L(DEPTH_READ_ONLY_OPTIMAL));
    EXPECT_EQ(a.stages, shaders | S2(EARLY_FRAGMENT_TESTS_BIT) | S2(LATE_FRAGMENT_TESTS_BIT));
    a = de__vk_access_info_of(&g, DE_ACCESS_SHADER_READ | DE_ACCESS_COPY_SRC, true, false, false,
                              false);
    EXPECT_EQ(a.layout, L(GENERAL));
    EXPECT_EQ(a.access,
              A2(SHADER_SAMPLED_READ_BIT) | A2(SHADER_STORAGE_READ_BIT) | A2(TRANSFER_READ_BIT));

    /* Without separate depth/stencil layouts the combined ones are used. */
    de__vk_barrier_ctx g2 = g;
    g2.separate_depth_layouts = false;
    a = de__vk_access_info_of(&g2, DE_ACCESS_DEPTH_WRITE, true, true, false, false);
    EXPECT_EQ(a.layout, L(DEPTH_STENCIL_ATTACHMENT_OPTIMAL));
    a = de__vk_access_info_of(&g2, DE_ACCESS_DEPTH_READ, true, true, false, false);
    EXPECT_EQ(a.layout, L(DEPTH_STENCIL_READ_ONLY_OPTIMAL));

    /* A copy queue drops the stages it cannot run (and their accesses), not the layout. */
    const de__vk_barrier_ctx cq = copy_ctx();
    a = de__vk_access_info_of(&cq, DE_ACCESS_SHADER_READ, true, false, false, false);
    EXPECT_EQ(a.stages, (VkPipelineStageFlags2)0);
    EXPECT_EQ(a.access, (VkAccessFlags2)0);
    EXPECT_EQ(a.layout, L(SHADER_READ_ONLY_OPTIMAL));
    a = de__vk_access_info_of(&cq, DE_ACCESS_COPY_DST, true, false, false, false);
    EXPECT_EQ(a.stages, S2(ALL_TRANSFER_BIT));

    /* The access pairs the GPU tests use (D3D12 and Metal mirror them). */
    /* Not static: the VK_*_2_* flags are static const variables in C, not constants. */
    const struct {
        u32 before, after;
        VkImageLayout old_layout, new_layout;
        VkPipelineStageFlags2 src, dst;
        VkAccessFlags2 src_access, dst_access;
    } k_pairs[] = {
        {DE_ACCESS_NONE, DE_ACCESS_RENDER_TARGET, L(UNDEFINED), L(COLOR_ATTACHMENT_OPTIMAL),
         S2(ALL_COMMANDS_BIT), S2(COLOR_ATTACHMENT_OUTPUT_BIT), 0,
         A2(COLOR_ATTACHMENT_READ_BIT) | A2(COLOR_ATTACHMENT_WRITE_BIT)},
        {DE_ACCESS_RENDER_TARGET, DE_ACCESS_COPY_SRC, L(COLOR_ATTACHMENT_OPTIMAL),
         L(TRANSFER_SRC_OPTIMAL), S2(COLOR_ATTACHMENT_OUTPUT_BIT), S2(ALL_TRANSFER_BIT),
         A2(COLOR_ATTACHMENT_READ_BIT) | A2(COLOR_ATTACHMENT_WRITE_BIT), A2(TRANSFER_READ_BIT)},
        {DE_ACCESS_NONE, DE_ACCESS_COPY_DST, L(UNDEFINED), L(TRANSFER_DST_OPTIMAL),
         S2(ALL_COMMANDS_BIT), S2(ALL_TRANSFER_BIT), 0, A2(TRANSFER_WRITE_BIT)},
        {DE_ACCESS_COPY_DST, DE_ACCESS_COPY_SRC, L(TRANSFER_DST_OPTIMAL), L(TRANSFER_SRC_OPTIMAL),
         S2(ALL_TRANSFER_BIT), S2(ALL_TRANSFER_BIT), A2(TRANSFER_WRITE_BIT), A2(TRANSFER_READ_BIT)},
        {DE_ACCESS_RENDER_TARGET, DE_ACCESS_PRESENT, L(COLOR_ATTACHMENT_OPTIMAL),
         L(PRESENT_SRC_KHR), S2(COLOR_ATTACHMENT_OUTPUT_BIT), 0,
         A2(COLOR_ATTACHMENT_READ_BIT) | A2(COLOR_ATTACHMENT_WRITE_BIT), 0},
        {DE_ACCESS_SHADER_WRITE, DE_ACCESS_SHADER_READ, L(GENERAL), L(SHADER_READ_ONLY_OPTIMAL),
         shaders, shaders, A2(SHADER_STORAGE_READ_BIT) | A2(SHADER_STORAGE_WRITE_BIT),
         A2(SHADER_SAMPLED_READ_BIT) | A2(SHADER_STORAGE_READ_BIT)},
    };
    de__vk_barrier_texture tex = {.image = (VkImage)(uintptr_t)0x1000,
                                  .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                                  .mip_count = 5,
                                  .layers = 6};
    for (u32 i = 0; i < sizeof k_pairs / sizeof k_pairs[0]; ++i) {
        de_gpu_barrier b;
        memset(&b, 0, sizeof b);
        b.texture.id = 1;
        b.before = k_pairs[i].before;
        b.after = k_pairs[i].after;
        VkImageMemoryBarrier2 ib;
        de__vk_translate_barrier(&g, &b, &tex, VK_NULL_HANDLE, &ib, NULL);
        EXPECT_EQ(ib.oldLayout, k_pairs[i].old_layout);
        EXPECT_EQ(ib.newLayout, k_pairs[i].new_layout);
        EXPECT_EQ(ib.srcStageMask, k_pairs[i].src);
        EXPECT_EQ(ib.dstStageMask, k_pairs[i].dst);
        EXPECT_EQ(ib.srcAccessMask, k_pairs[i].src_access);
        EXPECT_EQ(ib.dstAccessMask, k_pairs[i].dst_access);
        EXPECT_EQ(ib.srcQueueFamilyIndex, (u32)VK_QUEUE_FAMILY_IGNORED);
        EXPECT_EQ(ib.dstQueueFamilyIndex, (u32)VK_QUEUE_FAMILY_IGNORED);
        /* 0 counts: every mip and layer. */
        EXPECT_EQ(ib.subresourceRange.baseMipLevel, 0u);
        EXPECT_EQ(ib.subresourceRange.levelCount, 5u);
        EXPECT_EQ(ib.subresourceRange.layerCount, 6u);
        EXPECT_EQ(ib.subresourceRange.aspectMask, (VkImageAspectFlags)VK_IMAGE_ASPECT_COLOR_BIT);
    }

    /* Ranges. */
    {
        de_gpu_barrier b;
        memset(&b, 0, sizeof b);
        b.texture.id = 1;
        b.before = DE_ACCESS_COPY_DST;
        b.after = DE_ACCESS_SHADER_READ;
        b.mip_first = 2;
        b.layer_first = 1;
        b.layer_count = 3;
        VkImageMemoryBarrier2 ib;
        de__vk_translate_barrier(&g, &b, &tex, VK_NULL_HANDLE, &ib, NULL);
        EXPECT_EQ(ib.subresourceRange.baseMipLevel, 2u);
        EXPECT_EQ(ib.subresourceRange.levelCount, 3u);
        EXPECT_EQ(ib.subresourceRange.baseArrayLayer, 1u);
        EXPECT_EQ(ib.subresourceRange.layerCount, 3u);
        b.after = DE_ACCESS_NONE; /* no later use: the layout stays */
        de__vk_translate_barrier(&g, &b, &tex, VK_NULL_HANDLE, &ib, NULL);
        EXPECT_EQ(ib.newLayout, ib.oldLayout);
    }

    /* Queue-family ownership transfer, copy (family 2) -> graphics (family 0). */
    {
        de_gpu_barrier b;
        memset(&b, 0, sizeof b);
        b.buffer.id = 1;
        b.before = DE_ACCESS_COPY_DST;
        b.after = DE_ACCESS_COPY_SRC;
        b.src_queue = DE_QUEUE_COPY;
        b.dst_queue = DE_QUEUE_GRAPHICS;
        VkBufferMemoryBarrier2 rel, acq;
        de__vk_translate_barrier(&cq, &b, NULL, (VkBuffer)(uintptr_t)0x2000, NULL, &rel);
        EXPECT_EQ(rel.srcQueueFamilyIndex, 2u);
        EXPECT_EQ(rel.dstQueueFamilyIndex, 0u);
        EXPECT_EQ(rel.srcStageMask, S2(ALL_TRANSFER_BIT));
        EXPECT_EQ(rel.srcAccessMask, A2(TRANSFER_WRITE_BIT));
        EXPECT_EQ(rel.dstStageMask, (VkPipelineStageFlags2)0); /* release half */
        EXPECT_EQ(rel.dstAccessMask, (VkAccessFlags2)0);
        EXPECT_EQ(rel.size, (VkDeviceSize)VK_WHOLE_SIZE);
        de__vk_translate_barrier(&g, &b, NULL, (VkBuffer)(uintptr_t)0x2000, NULL, &acq);
        EXPECT_EQ(acq.srcQueueFamilyIndex, 2u);
        EXPECT_EQ(acq.dstQueueFamilyIndex, 0u);
        EXPECT_EQ(acq.srcStageMask, (VkPipelineStageFlags2)0); /* acquire half */
        EXPECT_EQ(acq.dstStageMask, S2(ALL_TRANSFER_BIT));
        EXPECT_EQ(acq.dstAccessMask, A2(TRANSFER_READ_BIT));
        /* Same family: no transfer. */
        de__vk_barrier_ctx same = g;
        same.family[DE_QUEUE_COPY] = 0;
        de__vk_translate_barrier(&same, &b, NULL, (VkBuffer)(uintptr_t)0x2000, NULL, &acq);
        EXPECT_EQ(acq.srcQueueFamilyIndex, (u32)VK_QUEUE_FAMILY_IGNORED);
        EXPECT_EQ(acq.srcStageMask, S2(ALL_TRANSFER_BIT));
    }

    /* DENG-105: an image handed from the copy queue to the graphics queue (the asset uploader's
     * release + acquire). Different families: both halves carry the same transition. One family:
     * the copy queue's barrier transitions, the graphics half keeps the layout it left. */
    {
        de_gpu_barrier b;
        memset(&b, 0, sizeof b);
        b.texture.id = 1;
        b.before = DE_ACCESS_COPY_DST;
        b.after = DE_ACCESS_SHADER_READ;
        b.src_queue = DE_QUEUE_COPY;
        b.dst_queue = DE_QUEUE_GRAPHICS;
        VkImageMemoryBarrier2 rel, acq;
        de__vk_translate_barrier(&cq, &b, &tex, VK_NULL_HANDLE, &rel, NULL);
        de__vk_translate_barrier(&g, &b, &tex, VK_NULL_HANDLE, &acq, NULL);
        EXPECT_EQ(rel.oldLayout, L(TRANSFER_DST_OPTIMAL));
        EXPECT_EQ(rel.newLayout, L(SHADER_READ_ONLY_OPTIMAL));
        EXPECT_EQ(acq.oldLayout, rel.oldLayout);
        EXPECT_EQ(acq.newLayout, rel.newLayout);
        de__vk_barrier_ctx same_g = g, same_c = cq;
        same_g.family[DE_QUEUE_COPY] = 0;
        same_c.family[DE_QUEUE_COPY] = 0;
        same_c.cmd_family = 0;
        de__vk_translate_barrier(&same_c, &b, &tex, VK_NULL_HANDLE, &rel, NULL);
        de__vk_translate_barrier(&same_g, &b, &tex, VK_NULL_HANDLE, &acq, NULL);
        EXPECT_EQ(rel.srcQueueFamilyIndex, (u32)VK_QUEUE_FAMILY_IGNORED);
        EXPECT_EQ(rel.oldLayout, L(TRANSFER_DST_OPTIMAL));
        EXPECT_EQ(rel.newLayout, L(SHADER_READ_ONLY_OPTIMAL));
        EXPECT_EQ(acq.srcQueueFamilyIndex, (u32)VK_QUEUE_FAMILY_IGNORED);
        EXPECT_EQ(acq.oldLayout, L(SHADER_READ_ONLY_OPTIMAL)); /* no second transition */
        EXPECT_EQ(acq.newLayout, L(SHADER_READ_ONLY_OPTIMAL));
    }

    /* Buffer <-> texture copy regions: row pitch in texels (BC: 4 per block). */
    {
        de__gpu_texture t;
        memset(&t, 0, sizeof t);
        t.dim = DE_TEX_2D;
        t.format = DE_FMT_BC7_UNORM;
        t.width = t.height = 64;
        t.depth_or_layers = 1;
        t.mip_count = 4;
        VkBufferImageCopy r;
        de__vk_buffer_image_copy(&t, VK_IMAGE_ASPECT_COLOR_BIT, 2, 0, 1024, 256, &r);
        EXPECT_EQ(r.bufferOffset, (VkDeviceSize)1024);
        EXPECT_EQ(r.bufferRowLength, 64u);
        EXPECT_EQ(r.imageExtent.width, 16u);
        EXPECT_EQ(r.imageExtent.height, 16u);
        EXPECT_EQ(r.imageSubresource.mipLevel, 2u);
        t.format = DE_FMT_RGBA8_UNORM;
        t.dim = DE_TEX_2D_ARRAY;
        t.depth_or_layers = 4;
        de__vk_buffer_image_copy(&t, VK_IMAGE_ASPECT_COLOR_BIT, 0, 3, 0, 512, &r);
        EXPECT_EQ(r.bufferRowLength, 128u);
        EXPECT_EQ(r.imageSubresource.baseArrayLayer, 3u);
        t.format = DE_FMT_D32_FLOAT_S8;
        de__vk_buffer_image_copy(&t, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0,
                                 0, 512, &r);
        EXPECT_EQ(r.imageSubresource.aspectMask, (VkImageAspectFlags)VK_IMAGE_ASPECT_DEPTH_BIT);
        t.dim = DE_TEX_3D;
        t.format = DE_FMT_R8_UNORM;
        t.depth_or_layers = 8;
        de__vk_buffer_image_copy(&t, VK_IMAGE_ASPECT_COLOR_BIT, 1, 5, 0, 256, &r);
        EXPECT_EQ(r.imageSubresource.baseArrayLayer, 0u);
        EXPECT_EQ(r.imageExtent.depth, 4u);
    }
}
