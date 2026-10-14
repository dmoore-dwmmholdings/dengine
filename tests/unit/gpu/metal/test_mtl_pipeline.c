/* test_mtl_pipeline.c — Metal pipelines, draws, dispatches, indirect command buffers and the
 * binary archive (DENG-325). CTest names: gpu_metal.mtl.<test>.
 *
 * The GPU tests use the public de_gpu API and DENG-323's shared S10 pipeline tests
 * (tests/unit/gpu/data/pipelines/: Slang shaders cooked to metallib through the S09 importer,
 * inputs and expected values), with the names of the Vulkan tests (test_vk_pipeline.c):
 *   - pipeline_triangle_readback: vertex pulling, push-constant color, back-face culling (front =
 *     counter-clockwise: the Metal winding check), reverse-Z depth test; three pixels. Then the
 *     pipeline is destroyed while its submission is in flight (deferred destruction).
 *   - pipeline_compute_bindless: a compute shader reads and writes buffers through bindless
 *     indices on the compute queue; the buffer reads back exactly.
 *   - draw_indirect_count: a compute pass writes 3 draw records and count 2; through the
 *     indirect command buffer exactly two draws appear (and the pass was split once). Then the
 *     same records without a count buffer: all three. Then two counted draws in one pass.
 *   - mesh_pipeline: object + mesh + fragment functions draw the triangle test's first triangle,
 *     directly and through the indirect variant.
 *   - pipelines_forced_use_heap: the triangle, indirect-count and mesh tests on a device with
 *     gpu.metal.force_use_heap (useHeaps: per encoder, the macOS 14 residency path).
 *   - push_constant_limit: 128 bytes of push constants arrive intact; 132 bytes, or fewer bytes
 *     than the shader uses, are refused.
 *   - pipeline_validation: wrong stage, not a metallib, missing threadgroup sizes, blend and cull
 *     modes.
 *   - draw_indexed_scissor: draw_indexed with u32 and u16 index buffers (first_index > 0), a
 *     scissor that starts left of the target (cut) and a viewport; the pipeline and push
 *     constants carry over into the next render pass.
 *   - mtl_archive.binary_archive_roundtrip / binary_archive_corrupt: the persistent pipeline
 *     cache. Their suite is not `mtl`, so they run without shader validation (test_mtl_cmd.c turns
 *     it on for --filter=mtl.*), which leaves a device without an archive
 *     (binary_archive_off_with_shader_validation checks that).
 *   - scissor_rect, archive_file_name: pure.
 * Metal API validation (MTL_DEBUG_LAYER=1, test_mtl_device.c) and shader validation
 * (test_mtl_cmd.c, for mtl.* tests on the host) are on: a validation error aborts the test. Without
 * a Metal device, or without the shader importer in the build, the GPU tests SKIP with the reason.
 */
#include "de_test.h"

#include <dengine/de_debug.h>
#include <dengine/de_gpu.h>
#include <dengine/de_platform.h>

#include "gpu/metal/mtl_internal.h"
#include "pipeline_shaders.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WAIT_NS ((u64)60 * 1000 * 1000 * 1000)
#define SIZE    DE_PIPE_TEST_SIZE
#define PITCH   (DE_PIPE_TEST_SIZE * 4u)
#define TARGET  DE__SHADER_TARGET_METALLIB

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

/* ---------- Fixture ---------- */

typedef struct pipe_fx {
    de_tracking_allocator tracker;
    de_allocator alloc;
    de_gpu_device *d;
    char skip[1200];
    char err[1100];
} pipe_fx;

static void need_shaders(pipe_fx *f) {
    char why[300];
    if (!de_pipe_test_cook(TARGET, why, sizeof why)) {
        snprintf(f->skip, sizeof f->skip, "shared pipeline shaders not cooked: %s", why);
        SKIP(f->skip);
    }
}

static de_pipe_test_shader shader(const char *file, const char *entry) {
    de_pipe_test_shader s;
    if (!de_pipe_test_get(TARGET, file, entry, &s)) {
        printf("missing cooked shader %s:%s\n", file, entry);
        ASSERT_TRUE(false);
    }
    return s;
}

/* A Metal device on the fixture's tracking allocator; `cache_dir` "" = no pipeline cache. Skips
 * without a Metal device or below the Base tier (CI's paravirtual GPU). */
static de_gpu_device *make_device(pipe_fx *f, const char *cache_dir) {
    de_gpu_device_desc desc = {.backend = DE_GPU_METAL,
                               .debug_layer = true,
                               .allocator = &f->alloc,
                               .pipeline_cache_dir = cache_dir};
    de_gpu_device *d = NULL;
    de_result r = de_gpu_device_create(&desc, &d);
    if (r == DE_ERR_UNSUPPORTED) {
        snprintf(f->skip, sizeof f->skip, "no Metal device: %s", errchain(f->err, sizeof f->err));
        SKIP(f->skip);
    }
    ASSERT_EQ(r, DE_OK);
    const de_gpu_caps *caps = de_gpu_get_caps(d);
    if (!caps->base_tier_ok) {
        snprintf(f->skip, sizeof f->skip, "the Metal device is below the Base tier: %s",
                 caps->base_tier_reason);
        de_gpu_device_destroy(d);
        SKIP(f->skip);
    }
    return d;
}

static void pipe_fx_setup(pipe_fx *f) {
    f->alloc = de_tracking_allocator_init(&f->tracker, NULL, DE_MEM_RENDER);
}

static void pipe_fx_teardown(pipe_fx *f) {
    de_gpu_device_destroy(f->d);
    f->d = NULL;
    de_tracking_stats st = de_tracking_allocator_stats(&f->tracker);
    EXPECT_EQ(st.blocks_live, 0u);
    EXPECT_EQ(st.bytes_live, 0u);
}

static void open_device(pipe_fx *f) {
    need_shaders(f);
    f->d = make_device(f, "");
    printf("device: \"%s\"\n", de_gpu_get_caps(f->d)->device_name);
}

/* ---------- Helpers ---------- */

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

static de_gpu_fence submit(de_gpu_device *d, de_gpu_queue q, de_gpu_cmd *c) {
    de_gpu_fence fence = de_gpu_fence_create(d, 0);
    ASSERT_TRUE(DE_HANDLE_VALID(fence));
    de_gpu_submit s = {.cmds = &c, .cmd_count = 1, .signal = fence, .signal_value = 1};
    ASSERT_EQ(de_gpu_queue_submit(d, q, &s), DE_OK);
    return fence;
}

static void finish(de_gpu_device *d, de_gpu_fence fence) {
    ASSERT_EQ(de_gpu_fence_wait(d, fence, 1, WAIT_NS), DE_OK);
    de_gpu_fence_destroy(d, fence);
}

static void submit_wait(de_gpu_device *d, de_gpu_queue q, de_gpu_cmd *c) {
    finish(d, submit(d, q, c));
}

static de_gpu_buffer upload(de_gpu_device *d, const void *data, u64 size, u32 usage, u32 stride,
                            const char *name) {
    de_gpu_buffer b = de_gpu_buffer_create(
        d,
        &(de_gpu_buffer_desc){
            .size = size, .usage = usage, .memory = DE_MEM_UPLOAD, .stride = stride, .name = name});
    ASSERT_TRUE(DE_HANDLE_VALID(b));
    void *p = de_gpu_buffer_map(d, b);
    ASSERT_TRUE(p != NULL);
    memcpy(p, data, (size_t)size);
    return b;
}

typedef struct target {
    de_gpu_texture color, depth;
    de_gpu_buffer readback;
    u8 *pixels;
} target;

static target make_target(de_gpu_device *d, bool depth) {
    target t;
    memset(&t, 0, sizeof t);
    t.color = de_gpu_texture_create(
        d, &(de_gpu_texture_desc){.format = DE_FMT_RGBA8_UNORM,
                                  .width = SIZE,
                                  .height = SIZE,
                                  .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_COPY_SRC,
                                  .name = "pipeline target"});
    ASSERT_TRUE(DE_HANDLE_VALID(t.color));
    if (depth) {
        t.depth = de_gpu_texture_create(d, &(de_gpu_texture_desc){.format = DE_FMT_D32_FLOAT,
                                                                  .width = SIZE,
                                                                  .height = SIZE,
                                                                  .usage = DE_USAGE_DEPTH,
                                                                  .name = "pipeline depth"});
        ASSERT_TRUE(DE_HANDLE_VALID(t.depth));
    }
    t.readback = de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = (u64)PITCH * SIZE,
                                                               .usage = DE_USAGE_COPY_DST,
                                                               .memory = DE_MEM_READBACK,
                                                               .name = "pipeline readback"});
    ASSERT_TRUE(DE_HANDLE_VALID(t.readback));
    t.pixels = (u8 *)de_gpu_buffer_map(d, t.readback);
    ASSERT_TRUE(t.pixels != NULL);
    memset(t.pixels, 0xEE, (size_t)PITCH * SIZE);
    return t;
}

static void free_target(de_gpu_device *d, target *t) {
    de_gpu_texture_destroy(d, t->color);
    de_gpu_texture_destroy(d, t->depth);
    de_gpu_buffer_destroy(d, t->readback);
}

static void begin_target(de_gpu_cmd *c, const target *t) {
    de_gpu_barrier b[2] = {tex_barrier(t->color, DE_ACCESS_NONE, DE_ACCESS_RENDER_TARGET),
                           tex_barrier(t->depth, DE_ACCESS_NONE, DE_ACCESS_DEPTH_WRITE)};
    de_gpu_cmd_barriers(c, b, DE_HANDLE_VALID(t->depth) ? 2u : 1u);
    de_gpu_attachment color = {
        .texture = t->color, .clear = true, .clear_color = {DE_PIPE_TEST_CLEAR_F}, .store = true};
    de_gpu_attachment depth = {.texture = t->depth, .clear = true, .clear_depth = 0.0f};
    de_gpu_cmd_begin_render(c, &color, 1, DE_HANDLE_VALID(t->depth) ? &depth : NULL);
}

static void end_target(de_gpu_cmd *c, const target *t) {
    de_gpu_cmd_end_render(c);
    de_gpu_barrier b = tex_barrier(t->color, DE_ACCESS_RENDER_TARGET, DE_ACCESS_COPY_SRC);
    de_gpu_cmd_barriers(c, &b, 1);
    de_gpu_cmd_copy_texture_to_buffer(c, t->readback, 0, PITCH, t->color, 0, 0);
}

static const u8 *pixel_at(const target *t, u32 x, u32 y) {
    return t->pixels + (size_t)y * PITCH + (size_t)x * 4u;
}

static void expect_pixels(const target *t, const de_pipe_test_pixel *px, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        const u8 *p = pixel_at(t, px[i].x, px[i].y);
        if (memcmp(p, px[i].rgba, 4) != 0)
            printf("pixel (%u, %u): got %u %u %u %u, expected %u %u %u %u\n", px[i].x, px[i].y,
                   p[0], p[1], p[2], p[3], px[i].rgba[0], px[i].rgba[1], px[i].rgba[2],
                   px[i].rgba[3]);
        EXPECT_MEMEQ(p, px[i].rgba, 4);
    }
}

typedef struct draw_data {
    f32 color[4];
    u32 positions;
    u32 first;
} draw_data;

static u32 max_u32(u32 a, u32 b) {
    return a > b ? a : b;
}

static de_gpu_pipeline triangle_pipeline(de_gpu_device *d, bool depth) {
    de_pipe_test_shader vs = shader("triangle.slang", "vs_main");
    de_pipe_test_shader ps = shader("triangle.slang", "ps_main");
    de_gpu_pipeline_desc pd = {.kind = DE_PIPE_GRAPHICS,
                               .vs = vs.blob,
                               .ps = ps.blob,
                               .color_formats = {DE_FMT_RGBA8_UNORM},
                               .color_count = 1,
                               .depth_format = depth ? DE_FMT_D32_FLOAT : DE_FMT_UNKNOWN,
                               .cull = DE_CULL_BACK,
                               .depth_test = depth,
                               .depth_write = depth,
                               .push_constant_bytes =
                                   max_u32(vs.push_constant_bytes, ps.push_constant_bytes),
                               .name = "triangle"};
    de_gpu_pipeline p = de_gpu_pipeline_create(d, &pd);
    if (!DE_HANDLE_VALID(p)) {
        char e[600];
        printf("triangle pipeline: %s\n", errchain(e, sizeof e));
    }
    ASSERT_TRUE(DE_HANDLE_VALID(p));
    return p;
}

static de_gpu_pipeline compute_pipeline(de_gpu_device *d, const char *file, const char *name) {
    de_pipe_test_shader cs = shader(file, "cs_main");
    de_gpu_pipeline_desc pd = {
        .kind = DE_PIPE_COMPUTE,
        .cs = cs.blob,
        .push_constant_bytes = cs.push_constant_bytes,
        .threadgroup_size = {cs.threadgroup[0], cs.threadgroup[1], cs.threadgroup[2]},
        .name = name};
    de_gpu_pipeline p = de_gpu_pipeline_create(d, &pd);
    if (!DE_HANDLE_VALID(p)) {
        char e[600];
        printf("%s pipeline: %s\n", name, errchain(e, sizeof e));
    }
    ASSERT_TRUE(DE_HANDLE_VALID(p));
    return p;
}

static bool mesh_desc(de_gpu_pipeline_desc *pd) {
    de_pipe_test_shader as, ms, ps;
    if (!de_pipe_test_get(TARGET, "mesh.slang", "as_main", &as) ||
        !de_pipe_test_get(TARGET, "mesh.slang", "ms_main", &ms) ||
        !de_pipe_test_get(TARGET, "mesh.slang", "ps_main", &ps))
        return false;
    *pd = (de_gpu_pipeline_desc){
        .kind = DE_PIPE_MESH,
        .as = as.blob,
        .ms = ms.blob,
        .ps = ps.blob,
        .color_formats = {DE_FMT_RGBA8_UNORM},
        .color_count = 1,
        .cull = DE_CULL_BACK,
        .push_constant_bytes = max_u32(max_u32(as.push_constant_bytes, ms.push_constant_bytes),
                                       ps.push_constant_bytes),
        .threadgroup_size = {ms.threadgroup[0], ms.threadgroup[1], ms.threadgroup[2]},
        .object_threadgroup_size = {as.threadgroup[0], as.threadgroup[1], as.threadgroup[2]},
        .name = "mesh"};
    return true;
}

static draw_data triangle_data(de_gpu_device *d, de_gpu_buffer pos, u32 color, u32 first) {
    draw_data dd;
    memcpy(dd.color, de_pipe_test_triangle_color[color], sizeof dd.color);
    dd.positions = de_gpu_buffer_bindless(d, pos);
    dd.first = first;
    return dd;
}

/* ---------- Triangle ---------- */

static void triangle_body(de_gpu_device *d) {
    de_gpu_pipeline pipe = triangle_pipeline(d, true);
    de_gpu_buffer pos = upload(d, de_pipe_test_triangle_pos, sizeof de_pipe_test_triangle_pos,
                               DE_USAGE_STORAGE, 16, "triangle positions");
    target t = make_target(d, true);

    /* Twice: the second pass reuses the recycled list. On the second, the pipeline is destroyed
     * while the submission is in flight: deferred destruction keeps it until the GPU is done. */
    for (u32 pass = 0; pass < 2; ++pass) {
        de_error_clear();
        de_gpu_cmd *c = de_gpu_cmd_begin(d, DE_QUEUE_GRAPHICS, "triangle");
        ASSERT_TRUE(c != NULL);
        begin_target(c, &t);
        de_gpu_cmd_set_pipeline(c, pipe);
        for (u32 k = 0; k < 3; ++k) {
            draw_data dd = triangle_data(d, pos, k, 3u * k);
            de_gpu_cmd_push_constants(c, &dd, sizeof dd);
            de_gpu_cmd_draw(c, 3, 1, 0, 0);
        }
        end_target(c, &t);
        EXPECT_TRUE(de_error_last() == NULL);
        de_gpu_fence fence = submit(d, DE_QUEUE_GRAPHICS, c);
        if (pass == 1) {
            de_gpu_pipeline_destroy(d, pipe);
            EXPECT_GE(de__gpu_deferred_count(d), 1u);
        }
        finish(d, fence);
        expect_pixels(&t, de_pipe_test_triangle_expect, 3);
        memset(t.pixels, 0xEE, (size_t)PITCH * SIZE);
    }
    free_target(d, &t);
    de_gpu_buffer_destroy(d, pos);
    de_gpu_wait_idle(d);
    EXPECT_EQ(de__gpu_live_count(d, DE__GPU_PIPELINE), 0u);
}

TEST_F(mtl, pipeline_triangle_readback, pipe_fx) {
    open_device(f);
    triangle_body(f->d);
}

/* ---------- Compute ---------- */

TEST_F(mtl, pipeline_compute_bindless, pipe_fx) {
    open_device(f);
    enum { N = DE_PIPE_TEST_COMPUTE_COUNT };
    static u32 src[N];
    for (u32 i = 0; i < N; ++i)
        src[i] = DE_PIPE_TEST_COMPUTE_SRC(i);
    de_gpu_pipeline pipe = compute_pipeline(f->d, "compute.slang", "compute");
    de_gpu_buffer in = upload(f->d, src, sizeof src, DE_USAGE_STORAGE, 4, "compute src");
    de_gpu_buffer out = de_gpu_buffer_create(
        f->d, &(de_gpu_buffer_desc){.size = sizeof src,
                                    .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                    .stride = 4,
                                    .name = "compute dst"});
    de_gpu_buffer rb = de_gpu_buffer_create(f->d, &(de_gpu_buffer_desc){.size = sizeof src,
                                                                        .usage = DE_USAGE_COPY_DST,
                                                                        .memory = DE_MEM_READBACK,
                                                                        .name = "compute rb"});
    ASSERT_TRUE(DE_HANDLE_VALID(out) && DE_HANDLE_VALID(rb));
    u32 *res = (u32 *)de_gpu_buffer_map(f->d, rb);
    memset(res, 0, sizeof src);

    de_error_clear();
    de_gpu_cmd *c = de_gpu_cmd_begin(f->d, DE_QUEUE_COMPUTE, "compute");
    ASSERT_TRUE(c != NULL);
    de_gpu_barrier b = buf_barrier(out, DE_ACCESS_NONE, DE_ACCESS_SHADER_WRITE);
    de_gpu_cmd_barriers(c, &b, 1);
    de_gpu_cmd_set_pipeline(c, pipe);
    const u32 params[4] = {de_gpu_buffer_bindless(f->d, in), de_gpu_buffer_bindless(f->d, out), N,
                           DE_PIPE_TEST_COMPUTE_SEED};
    de_gpu_cmd_push_constants(c, params, sizeof params);
    de_gpu_cmd_dispatch(c, (N + 63u) / 64u, 1, 1);
    b = buf_barrier(out, DE_ACCESS_SHADER_WRITE, DE_ACCESS_COPY_SRC);
    de_gpu_cmd_barriers(c, &b, 1);
    de_gpu_cmd_copy_buffer(c, rb, 0, out, 0, sizeof src);
    EXPECT_TRUE(de_error_last() == NULL);
    submit_wait(f->d, DE_QUEUE_COMPUTE, c);

    u32 bad = 0;
    for (u32 i = 0; i < N; ++i)
        if (res[i] != DE_PIPE_TEST_COMPUTE_DST(i)) {
            if (bad++ == 0)
                printf("element %u: 0x%08x, expected 0x%08x\n", i, res[i],
                       DE_PIPE_TEST_COMPUTE_DST(i));
        }
    EXPECT_EQ(bad, 0u);

    /* The same through dispatch_indirect (x, y, z from a buffer). */
    memset(res, 0, sizeof src);
    const u32 groups[3] = {(N + 63u) / 64u, 1, 1};
    de_gpu_buffer ia = upload(f->d, groups, sizeof groups, DE_USAGE_INDIRECT, 0, "dispatch args");
    c = de_gpu_cmd_begin(f->d, DE_QUEUE_COMPUTE, "compute indirect");
    ASSERT_TRUE(c != NULL);
    b = buf_barrier(out, DE_ACCESS_COPY_SRC, DE_ACCESS_SHADER_WRITE);
    de_gpu_cmd_barriers(c, &b, 1);
    de_gpu_cmd_set_pipeline(c, pipe);
    const u32 params2[4] = {params[0], params[1], N, DE_PIPE_TEST_COMPUTE_SEED + 1u};
    de_gpu_cmd_push_constants(c, params2, sizeof params2);
    de_gpu_cmd_dispatch_indirect(c, ia, 0);
    b = buf_barrier(out, DE_ACCESS_SHADER_WRITE, DE_ACCESS_COPY_SRC);
    de_gpu_cmd_barriers(c, &b, 1);
    de_gpu_cmd_copy_buffer(c, rb, 0, out, 0, sizeof src);
    submit_wait(f->d, DE_QUEUE_COMPUTE, c);
    bad = 0;
    for (u32 i = 0; i < N; ++i)
        if (res[i] != DE_PIPE_TEST_COMPUTE_DST(i) + 1u)
            ++bad;
    EXPECT_EQ(bad, 0u);

    de_gpu_buffer_destroy(f->d, ia);
    de_gpu_buffer_destroy(f->d, in);
    de_gpu_buffer_destroy(f->d, out);
    de_gpu_buffer_destroy(f->d, rb);
    de_gpu_pipeline_destroy(f->d, pipe);
}

/* ---------- Indirect count (ICB) ---------- */

static void indirect_count_body(de_gpu_device *d) {
    de_gpu_pipeline args_pipe = compute_pipeline(d, "indirect_args.slang", "indirect args");
    de_gpu_pipeline draw_pipe = triangle_pipeline(d, false);
    de_gpu_buffer pos = upload(d, de_pipe_test_indirect_pos, sizeof de_pipe_test_indirect_pos,
                               DE_USAGE_STORAGE, 16, "indirect positions");
    u16 idx[10] = {0};
    memcpy(idx, de_pipe_test_indirect_indices, sizeof de_pipe_test_indirect_indices);
    de_gpu_buffer ib = upload(d, idx, sizeof idx, DE_USAGE_INDEX, 0, "indirect indices");
    de_gpu_buffer args =
        de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = DE_PIPE_TEST_INDIRECT_RECORDS * 20u,
                                                      .usage = DE_USAGE_STORAGE | DE_USAGE_INDIRECT,
                                                      .stride = 4,
                                                      .name = "indirect args"});
    de_gpu_buffer count =
        de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = 16,
                                                      .usage = DE_USAGE_STORAGE | DE_USAGE_INDIRECT,
                                                      .stride = 4,
                                                      .name = "indirect count"});
    ASSERT_TRUE(DE_HANDLE_VALID(args) && DE_HANDLE_VALID(count));
    target t = make_target(d, false);

    /* Pass 0: with the count buffer (2 of 3 records). Pass 1: without it (all 3). Pass 2: two
     * counted draws in one render pass (two splits; the second reuses the ICB further on). */
    for (u32 pass = 0; pass < 3; ++pass) {
        de_error_clear();
        de_gpu_cmd *c = de_gpu_cmd_begin(d, DE_QUEUE_GRAPHICS, "indirect count");
        ASSERT_TRUE(c != NULL);
        de_gpu_barrier b[2] = {buf_barrier(args, DE_ACCESS_NONE, DE_ACCESS_SHADER_WRITE),
                               buf_barrier(count, DE_ACCESS_NONE, DE_ACCESS_SHADER_WRITE)};
        de_gpu_cmd_barriers(c, b, 2);
        de_gpu_cmd_set_pipeline(c, args_pipe);
        const u32 params[4] = {de_gpu_buffer_bindless(d, args), de_gpu_buffer_bindless(d, count),
                               DE_PIPE_TEST_INDIRECT_RECORDS, DE_PIPE_TEST_INDIRECT_COUNT};
        de_gpu_cmd_push_constants(c, params, sizeof params);
        de_gpu_cmd_dispatch(c, 1, 1, 1);
        b[0] = buf_barrier(args, DE_ACCESS_SHADER_WRITE, DE_ACCESS_INDIRECT);
        b[1] = buf_barrier(count, DE_ACCESS_SHADER_WRITE, DE_ACCESS_INDIRECT);
        de_gpu_cmd_barriers(c, b, 2);
        begin_target(c, &t);
        de_gpu_cmd_set_pipeline(c, draw_pipe);
        draw_data dd = triangle_data(d, pos, 0, 0);
        de_gpu_cmd_push_constants(c, &dd, sizeof dd);
        de_gpu_cmd_set_index_buffer(c, ib, 0, true);
        de_gpu_cmd_draw_indexed_indirect(c, args, 0, pass != 1 ? count : (de_gpu_buffer){0}, 0,
                                         DE_PIPE_TEST_INDIRECT_RECORDS);
        if (pass == 2)
            de_gpu_cmd_draw_indexed_indirect(c, args, 0, count, 0, DE_PIPE_TEST_INDIRECT_RECORDS);
        const u32 splits = ((mtl_cmd *)de__gpu_cmd_payload(c))->draw.splits;
        EXPECT_EQ(splits, pass == 0 ? 1u : pass == 1 ? 0u : 2u);
        end_target(c, &t);
        EXPECT_TRUE(de_error_last() == NULL);
        submit_wait(d, DE_QUEUE_GRAPHICS, c);
        if (pass != 1) {
            expect_pixels(&t, de_pipe_test_indirect_expect, 3);
        } else {
            de_pipe_test_pixel all[3];
            memcpy(all, de_pipe_test_indirect_expect, sizeof all);
            memcpy(all[2].rgba, all[0].rgba, 4); /* the third draw ran too */
            expect_pixels(&t, all, 3);
        }
        memset(t.pixels, 0xEE, (size_t)PITCH * SIZE);
    }
    free_target(d, &t);
    de_gpu_buffer_destroy(d, pos);
    de_gpu_buffer_destroy(d, ib);
    de_gpu_buffer_destroy(d, args);
    de_gpu_buffer_destroy(d, count);
    de_gpu_pipeline_destroy(d, args_pipe);
    de_gpu_pipeline_destroy(d, draw_pipe);
}

TEST_F(mtl, draw_indirect_count, pipe_fx) {
    open_device(f);
    indirect_count_body(f->d);
}

/* ---------- Mesh ---------- */

static void mesh_body(de_gpu_device *d) {
    de_gpu_pipeline_desc pd;
    ASSERT_TRUE(mesh_desc(&pd));
    EXPECT_EQ(pd.threadgroup_size[0], 3u);
    EXPECT_EQ(pd.object_threadgroup_size[0], 1u);
    de_error_clear();
    de_gpu_pipeline pipe = de_gpu_pipeline_create(d, &pd);
    if (!de_gpu_get_caps(d)->mesh_shaders) {
        EXPECT_FALSE(DE_HANDLE_VALID(pipe));
        ASSERT_TRUE(de_error_last() != NULL);
        EXPECT_EQ(de_error_last()->result, DE_ERR_UNSUPPORTED);
        SKIP("no Metal 3 mesh shaders on this device: DE_ERR_UNSUPPORTED checked");
    }
    if (!DE_HANDLE_VALID(pipe)) {
        char e[600];
        printf("mesh pipeline: %s\n", errchain(e, sizeof e));
    }
    ASSERT_TRUE(DE_HANDLE_VALID(pipe));
    de_gpu_buffer pos = upload(d, de_pipe_test_triangle_pos, sizeof de_pipe_test_triangle_pos,
                               DE_USAGE_STORAGE, 16, "mesh positions");
    /* Two records: the second holds zero threadgroups (the contract past the count). */
    const u32 recs[6] = {1, 1, 1, 0, 0, 0};
    de_gpu_buffer args = upload(d, recs, sizeof recs, DE_USAGE_INDIRECT, 0, "mesh args");
    const u32 cnt[1] = {1};
    de_gpu_buffer count = upload(d, cnt, sizeof cnt, DE_USAGE_INDIRECT, 0, "mesh count");
    target t = make_target(d, false);
    /* Pass 0: de_gpu_cmd_dispatch_mesh. Pass 1: the indirect-count variant (max 2). */
    for (u32 pass = 0; pass < 2; ++pass) {
        de_error_clear();
        de_gpu_cmd *c = de_gpu_cmd_begin(d, DE_QUEUE_GRAPHICS, "mesh");
        ASSERT_TRUE(c != NULL);
        begin_target(c, &t);
        de_gpu_cmd_set_pipeline(c, pipe);
        draw_data dd = triangle_data(d, pos, 0, 0);
        de_gpu_cmd_push_constants(c, &dd, sizeof dd);
        if (pass == 0)
            de_gpu_cmd_dispatch_mesh(c, 1, 1, 1);
        else
            de_gpu_cmd_dispatch_mesh_indirect(c, args, 0, count, 0, 2);
        end_target(c, &t);
        EXPECT_TRUE(de_error_last() == NULL);
        submit_wait(d, DE_QUEUE_GRAPHICS, c);
        expect_pixels(&t, de_pipe_test_triangle_expect, 3);
        memset(t.pixels, 0xEE, (size_t)PITCH * SIZE);
    }
    free_target(d, &t);
    de_gpu_buffer_destroy(d, pos);
    de_gpu_buffer_destroy(d, args);
    de_gpu_buffer_destroy(d, count);
    de_gpu_pipeline_destroy(d, pipe);
}

TEST_F(mtl, mesh_pipeline, pipe_fx) {
    open_device(f);
    mesh_body(f->d);
}

/* The same draws on a device that makes the heaps resident with useHeaps: in every encoder (the
 * macOS 14 path, gpu.metal.force_use_heap): the ICB and its range buffer are named to Metal with
 * useResource: on both paths. One device per process (see test_mtl_cmd.c). */
TEST_F(mtl, pipelines_forced_use_heap, pipe_fx) {
    need_shaders(f);
    static const char k_force[] = "gpu.metal.force_use_heap = true\n";
    ASSERT_EQ(de_cvar_load_from_memory(k_force, sizeof k_force - 1), DE_OK);
    f->d = make_device(f, "");
    de_cvar_set(de_cvar_find("gpu.metal.force_use_heap"), "0"); /* later devices: the default */
    EXPECT_TRUE(de__mtl_residency_uses_heaps(f->d));
    triangle_body(f->d);
    indirect_count_body(f->d);
    mesh_body(f->d);
}

/* ---------- Push-constant limit ---------- */

TEST_F(mtl, push_constant_limit, pipe_fx) {
    open_device(f);
    EXPECT_EQ(de_gpu_get_caps(f->d)->max_push_constant_bytes, DE_GPU_MAX_PUSH_CONSTANT_BYTES);
    de_pipe_test_shader cs = shader("push_limit.slang", "cs_main");
    EXPECT_EQ(cs.push_constant_bytes, 128u);
    de_gpu_pipeline_desc pd = {
        .kind = DE_PIPE_COMPUTE,
        .cs = cs.blob,
        .push_constant_bytes = 132,
        .threadgroup_size = {cs.threadgroup[0], cs.threadgroup[1], cs.threadgroup[2]},
        .name = "push 132"};
    /* Over the limit: refused by the front-end. */
    de_error_clear();
    EXPECT_FALSE(DE_HANDLE_VALID(de_gpu_pipeline_create(f->d, &pd)));
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(strstr(de_error_last()->message, "limit is 128") != NULL);
    /* Fewer bytes than the shader uses: refused by the binding reflection. */
    pd.push_constant_bytes = 120;
    pd.name = "push 120";
    de_error_clear();
    EXPECT_FALSE(DE_HANDLE_VALID(de_gpu_pipeline_create(f->d, &pd)));
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    if (!strstr(de_error_last()->message, "uses 128 bytes of push constants"))
        printf("message: %s\n", de_error_last()->message);
    EXPECT_TRUE(strstr(de_error_last()->message, "uses 128 bytes of push constants") != NULL);
    /* Exactly 128: every byte arrives. */
    pd.push_constant_bytes = 128;
    pd.name = "push 128";
    de_error_clear();
    de_gpu_pipeline pipe = de_gpu_pipeline_create(f->d, &pd);
    ASSERT_TRUE(DE_HANDLE_VALID(pipe));
    de_gpu_buffer out = de_gpu_buffer_create(
        f->d, &(de_gpu_buffer_desc){.size = 128,
                                    .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                    .stride = 4,
                                    .name = "push out"});
    de_gpu_buffer rb = de_gpu_buffer_create(f->d, &(de_gpu_buffer_desc){.size = 128,
                                                                        .usage = DE_USAGE_COPY_DST,
                                                                        .memory = DE_MEM_READBACK,
                                                                        .name = "push rb"});
    ASSERT_TRUE(DE_HANDLE_VALID(out) && DE_HANDLE_VALID(rb));
    u32 *res = (u32 *)de_gpu_buffer_map(f->d, rb);
    memset(res, 0, 128);
    u32 words[32];
    for (u32 i = 0; i < 31; ++i)
        words[i] = DE_PIPE_TEST_PUSH_WORD(i);
    words[31] = de_gpu_buffer_bindless(f->d, out);
    de_gpu_cmd *c = de_gpu_cmd_begin(f->d, DE_QUEUE_GRAPHICS, "push limit");
    ASSERT_TRUE(c != NULL);
    de_gpu_barrier b = buf_barrier(out, DE_ACCESS_NONE, DE_ACCESS_SHADER_WRITE);
    de_gpu_cmd_barriers(c, &b, 1);
    de_gpu_cmd_set_pipeline(c, pipe);
#if DE__GPU_VALIDATE_COMMANDS
    u32 too_many[33] = {0};
    de_error_clear();
    de_gpu_cmd_push_constants(c, too_many, sizeof too_many);
    EXPECT_TRUE(de_error_last() != NULL && de_error_last()->result == DE_ERR_INVALID_ARGUMENT);
#endif
    de_error_clear();
    de_gpu_cmd_push_constants(c, words, sizeof words);
    de_gpu_cmd_dispatch(c, 1, 1, 1);
    b = buf_barrier(out, DE_ACCESS_SHADER_WRITE, DE_ACCESS_COPY_SRC);
    de_gpu_cmd_barriers(c, &b, 1);
    de_gpu_cmd_copy_buffer(c, rb, 0, out, 0, 128);
    EXPECT_TRUE(de_error_last() == NULL);
    submit_wait(f->d, DE_QUEUE_GRAPHICS, c);
    EXPECT_MEMEQ(res, words, sizeof words);
    de_gpu_buffer_destroy(f->d, out);
    de_gpu_buffer_destroy(f->d, rb);
    de_gpu_pipeline_destroy(f->d, pipe);
}

/* ---------- Pipeline creation checks ---------- */

static void expect_refused(de_gpu_device *d, const de_gpu_pipeline_desc *pd, const char *needle) {
    de_error_clear();
    de_gpu_pipeline p = de_gpu_pipeline_create(d, pd);
    EXPECT_FALSE(DE_HANDLE_VALID(p));
    if (DE_HANDLE_VALID(p)) {
        de_gpu_pipeline_destroy(d, p);
        return;
    }
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    if (!strstr(de_error_last()->message, needle))
        printf("message \"%s\" lacks \"%s\"\n", de_error_last()->message, needle);
    EXPECT_TRUE(strstr(de_error_last()->message, needle) != NULL);
}

TEST_F(mtl, pipeline_validation, pipe_fx) {
    open_device(f);
    de_pipe_test_shader vs = shader("triangle.slang", "vs_main");
    de_pipe_test_shader ps = shader("triangle.slang", "ps_main");
    de_pipe_test_shader cs = shader("compute.slang", "cs_main");
    /* A vertex shader given as the compute stage. */
    de_gpu_pipeline_desc pd = {.kind = DE_PIPE_COMPUTE,
                               .cs = vs.blob,
                               .push_constant_bytes = 24,
                               .threadgroup_size = {64, 1, 1}};
    expect_refused(f->d, &pd, "is a vertex function, not a kernel function");
    /* A blob of a stage the kind does not use (front-end). */
    pd = (de_gpu_pipeline_desc){.kind = DE_PIPE_COMPUTE,
                                .cs = cs.blob,
                                .vs = vs.blob,
                                .push_constant_bytes = 16,
                                .threadgroup_size = {64, 1, 1}};
    expect_refused(f->d, &pd, "has no vs stage");
    /* Not a metallib. */
    static const u32 junk[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    pd = (de_gpu_pipeline_desc){.kind = DE_PIPE_COMPUTE,
                                .cs = {junk, sizeof junk, "cs_main"},
                                .push_constant_bytes = 16,
                                .threadgroup_size = {64, 1, 1}};
    expect_refused(f->d, &pd, "not a metallib");
    /* Metal needs the threadgroup size. */
    pd = (de_gpu_pipeline_desc){.kind = DE_PIPE_COMPUTE, .cs = cs.blob, .push_constant_bytes = 16};
    expect_refused(f->d, &pd, "needs threadgroup_size");
    /* More threads than a threadgroup holds. */
    pd.threadgroup_size[0] = 4096;
    pd.threadgroup_size[1] = pd.threadgroup_size[2] = 1;
    expect_refused(f->d, &pd, "threadgroup size (4096, 1, 1)");
    de_gpu_pipeline_desc md;
    if (mesh_desc(&md) && de_gpu_get_caps(f->d)->mesh_shaders) {
        memset(md.object_threadgroup_size, 0, sizeof md.object_threadgroup_size);
        expect_refused(f->d, &md, "needs object_threadgroup_size");
    }
    /* All four blend modes and three cull modes, with an empty entry name (the only function). */
    pd = (de_gpu_pipeline_desc){.kind = DE_PIPE_GRAPHICS,
                                .vs = vs.blob,
                                .ps = ps.blob,
                                .push_constant_bytes = 24,
                                .color_count = 1,
                                .color_formats = {DE_FMT_RGBA8_UNORM},
                                .depth_format = DE_FMT_D32_FLOAT,
                                .depth_test = true};
    for (u32 blend = DE_BLEND_OPAQUE; blend <= DE_BLEND_ADD; ++blend) {
        pd.blend[0] = (de_gpu_blend)blend;
        pd.cull = (de_gpu_cull)(blend % 3u);
        pd.vs.entry = blend == 0 ? "" : vs.blob.entry;
        de_error_clear();
        de_gpu_pipeline p = de_gpu_pipeline_create(f->d, &pd);
        if (!DE_HANDLE_VALID(p)) {
            char e[600];
            printf("blend %u: %s\n", blend, errchain(e, sizeof e));
        }
        EXPECT_TRUE(DE_HANDLE_VALID(p));
        de_gpu_pipeline_destroy(f->d, p);
    }
}

/* ---------- draw_indexed, scissor, viewport ---------- */

TEST_F(mtl, draw_indexed_scissor, pipe_fx) {
    open_device(f);
    de_gpu_pipeline pipe = triangle_pipeline(f->d, false);
    /* A full-target quad as two counter-clockwise triangles. */
    static const f32 quad[4][4] = {
        {-1.0f, -1.0f, 0.5f, 1.0f},
        {1.0f, -1.0f, 0.5f, 1.0f},
        {1.0f, 1.0f, 0.5f, 1.0f},
        {-1.0f, 1.0f, 0.5f, 1.0f},
    };
    static const u32 indices[8] = {7, 7, 0, 1, 2, 0, 2, 3}; /* first_index 2 */
    de_gpu_buffer pos = upload(f->d, quad, sizeof quad, DE_USAGE_STORAGE, 16, "quad");
    de_gpu_buffer ib = upload(f->d, indices, sizeof indices, DE_USAGE_INDEX, 0, "quad indices");
    /* u16 indices from first_index 3: a 6-byte offset into the buffer. */
    static const u16 indices16[10] = {9, 9, 9, 0, 1, 2, 0, 2, 3, 0};
    de_gpu_buffer ib16 =
        upload(f->d, indices16, sizeof indices16, DE_USAGE_INDEX, 0, "quad indices u16");
    target t = make_target(f->d, false);
    de_gpu_cmd *c = de_gpu_cmd_begin(f->d, DE_QUEUE_GRAPHICS, "indexed scissor");
    ASSERT_TRUE(c != NULL);
    de_error_clear();
    begin_target(c, &t);
    de_gpu_cmd_set_pipeline(c, pipe);
    draw_data dd = triangle_data(f->d, pos, 0, 0);
    de_gpu_cmd_push_constants(c, &dd, sizeof dd);
    /* Scissor from (-8, 16) to (24, 48): cut to x 0..24, y 16..48. */
    de_gpu_cmd_scissor(c, -8, 16, 32, 32);
    de_gpu_cmd_set_index_buffer(c, ib, 0, false);
    de_gpu_cmd_draw_indexed(c, 6, 1, 2, 0, 0);
    de_gpu_cmd_end_render(c);
    /* A second pass after a barrier: the pipeline and push constants carry over (new encoder);
     * the viewport is the right half, the scissor reset to the whole area. */
    de_gpu_barrier b = tex_barrier(t.color, DE_ACCESS_RENDER_TARGET, DE_ACCESS_RENDER_TARGET);
    de_gpu_cmd_barriers(c, &b, 1);
    de_gpu_attachment color = {.texture = t.color, .store = true};
    de_gpu_cmd_begin_render(c, &color, 1, NULL);
    dd = triangle_data(f->d, pos, 1, 0); /* red */
    de_gpu_cmd_push_constants(c, &dd, sizeof dd);
    de_gpu_cmd_viewport(c, 48.0f, 0.0f, 16.0f, 8.0f);
    de_gpu_cmd_set_index_buffer(c, ib16, 0, true);
    de_gpu_cmd_draw_indexed(c, 6, 1, 3, 0, 0);
    end_target(c, &t);
    EXPECT_TRUE(de_error_last() == NULL);
    submit_wait(f->d, DE_QUEUE_GRAPHICS, c);
    static const u8 green[4] = {0x00, 0xFF, 0x00, 0xFF}, red[4] = {0xFF, 0x00, 0x00, 0xFF},
                    clear[4] = {DE_PIPE_TEST_CLEAR_U8};
    const de_pipe_test_pixel px[] = {
        {0, 16, {0}},  {23, 47, {0}}, {24, 30, {0}}, {10, 15, {0}},
        {10, 48, {0}}, {50, 4, {0}},  {40, 4, {0}},  {50, 9, {0}},
    };
    const u8 *want[] = {green, green, clear, clear, clear, red, clear, clear};
    de_pipe_test_pixel exp[8];
    for (u32 i = 0; i < 8; ++i) {
        exp[i] = px[i];
        memcpy(exp[i].rgba, want[i], 4);
    }
    expect_pixels(&t, exp, 8);
    free_target(f->d, &t);
    de_gpu_buffer_destroy(f->d, pos);
    de_gpu_buffer_destroy(f->d, ib);
    de_gpu_buffer_destroy(f->d, ib16);
    de_gpu_pipeline_destroy(f->d, pipe);
}

/* ---------- Binary archive ---------- */

static const char *cache_dir(const char *sub) {
    static bool mounted;
    if (!mounted) {
        ASSERT_EQ(de_fs_mount("mtlcache", DE_MTL_TEST_CACHE_DIR, DE_MOUNT_WRITABLE, 0), DE_OK);
        mounted = true;
    }
    static char buf[64];
    snprintf(buf, sizeof buf, "mtlcache:/%s", sub);
    (void)de_dir_create(buf);
    return buf;
}

/* Creates the shared pipelines (and the mesh one where the archive takes it); the time taken. */
static u64 create_pipelines(de_gpu_device *d, de_gpu_pipeline out[4], u32 *count) {
    const u64 t0 = de_time_now_ns();
    out[0] = triangle_pipeline(d, true);
    out[1] = compute_pipeline(d, "compute.slang", "compute");
    out[2] = compute_pipeline(d, "indirect_args.slang", "indirect args");
    *count = 3;
    de_gpu_pipeline_desc md;
    if (de_gpu_get_caps(d)->mesh_shaders && mesh_desc(&md)) {
        out[3] = de_gpu_pipeline_create(d, &md);
        ASSERT_TRUE(DE_HANDLE_VALID(out[3]));
        *count = 4;
    }
    return de_time_now_ns() - t0;
}

static void destroy_pipelines(de_gpu_device *d, de_gpu_pipeline *p, u32 count) {
    for (u32 i = 0; i < count; ++i)
        de_gpu_pipeline_destroy(d, p[i]);
}

/* Pipelines the archive holds: the mesh one only from macOS 15 on (checked through a hit). */
static void run_pipelines(de_gpu_device *d, u64 *ns, u32 *count) {
    de_gpu_pipeline p[4];
    *ns = create_pipelines(d, p, count);
    destroy_pipelines(d, p, *count);
}

TEST_F(mtl_archive, binary_archive_roundtrip, pipe_fx) {
    need_shaders(f);
    const char *dir = cache_dir("roundtrip");
    de_gpu_device *d = make_device(f, dir);
    const char *p = NULL;
    u32 hits = 0, misses = 0;
    u32 state = de__mtl_archive_state(d, &hits, &misses, &p);
    char path[512];
    snprintf(path, sizeof path, "%s", p);
    printf("archive: %s\n", path);
    ASSERT_TRUE(strstr(path, "mtlcache:/roundtrip/metal-") == path);
    ASSERT_TRUE(strstr(path, ".metallib") != NULL);
    if (state != DE__MTL_ARCHIVE_EMPTY) {
        de_gpu_device_destroy(d);
        ASSERT_EQ(de_file_delete(path), DE_OK);
        d = make_device(f, dir);
        state = de__mtl_archive_state(d, NULL, NULL, NULL);
    }
    EXPECT_EQ(state, (u32)DE__MTL_ARCHIVE_EMPTY);
    u64 cold = 0;
    u32 n = 0;
    run_pipelines(d, &cold, &n);
    (void)de__mtl_archive_state(d, &hits, &misses, NULL);
    EXPECT_EQ(hits, 0u);
    EXPECT_GE(misses, 3u);
    de_gpu_device_destroy(d);
    ASSERT_TRUE(de_path_exists(path));

    /* Second run: the archive loads and every pipeline comes from it. */
    d = make_device(f, dir);
    EXPECT_EQ(de__mtl_archive_state(d, NULL, NULL, NULL), (u32)DE__MTL_ARCHIVE_LOADED);
    u64 warm = 0;
    run_pipelines(d, &warm, &n);
    (void)de__mtl_archive_state(d, &hits, &misses, NULL);
    printf("pipeline creation: %.3f ms compiled (%u), %.3f ms from the archive (%u hits, %u "
           "misses)\n",
           (double)cold / 1e6, n, (double)warm / 1e6, hits, misses);
    /* Every pipeline is a hit (the mesh one from macOS 15 on). The times are printed, not
     * compared: macOS also caches compiled shaders system-wide, so a "cold" creation is only
     * slow the first time a shader is seen (89 ms against 13 ms from the archive on an M5 Pro). */
    EXPECT_GE(hits, 3u);
    EXPECT_EQ(misses, 0u);
    EXPECT_LE(hits, n);
    de_gpu_device_destroy(d);

    /* The file deleted: the next device starts empty and writes it again. */
    ASSERT_EQ(de_file_delete(path), DE_OK);
    d = make_device(f, dir);
    EXPECT_EQ(de__mtl_archive_state(d, NULL, NULL, NULL), (u32)DE__MTL_ARCHIVE_EMPTY);
    run_pipelines(d, &warm, &n);
    de_gpu_device_destroy(d);
    EXPECT_TRUE(de_path_exists(path));

    /* No cache: "" */
    d = make_device(f, "");
    EXPECT_EQ(de__mtl_archive_state(d, NULL, NULL, NULL), (u32)DE__MTL_ARCHIVE_NONE);
    run_pipelines(d, &warm, &n);
    de_gpu_device_destroy(d);
}

/* Counts warnings about the pipeline cache (log sink). */
static de_atomic_u32 g_cache_warnings;
static void cache_sink(const de_log_record *rec, void *user) {
    (void)user;
    if (rec->level == DE_LOG_WARN && rec->message.len > 0 &&
        strstr(rec->message.ptr, "pipeline cache") != NULL)
        de_atomic_fetch_add_u32(&g_cache_warnings, 1u, DE_ATOMIC_RELAXED);
}

/* Writes `bytes` over the archive, then checks that a device ignores it, still creates and runs
 * pipelines, and writes a good archive back. */
static void expect_rejected(pipe_fx *f, const char *dir, const char *path, const void *bytes,
                            u64 size, const char *what) {
    printf("case: %s\n", what);
    ASSERT_EQ(de_file_write_all_atomic(path, bytes, size), DE_OK);
    de_gpu_device *d = make_device(f, dir);
    EXPECT_EQ(de__mtl_archive_state(d, NULL, NULL, NULL), (u32)DE__MTL_ARCHIVE_REJECTED);
    u64 ns = 0;
    u32 n = 0;
    run_pipelines(d, &ns, &n);
    de_gpu_device_destroy(d);
    d = make_device(f, dir);
    EXPECT_EQ(de__mtl_archive_state(d, NULL, NULL, NULL), (u32)DE__MTL_ARCHIVE_LOADED);
    de_gpu_device_destroy(d);
}

TEST_F(mtl_archive, binary_archive_corrupt, pipe_fx) {
    need_shaders(f);
    const char *dir = cache_dir("corrupt");
    de_gpu_device *d = make_device(f, dir);
    const char *p = NULL;
    (void)de__mtl_archive_state(d, NULL, NULL, &p);
    char path[512];
    snprintf(path, sizeof path, "%s", p);
    u64 ns = 0;
    u32 n = 0;
    run_pipelines(d, &ns, &n);
    de_gpu_device_destroy(d);
    void *file = NULL;
    u64 size = 0;
    ASSERT_EQ(de_file_read_all(path, NULL, &file, &size), DE_OK);
    ASSERT_GT(size, (u64)256);

    const bool log_started = de_log_init(&(de_log_desc){.no_console_sink = false}) == DE_OK;
    de_error_clear();
    de_log_add_sink(cache_sink, NULL);
    de_log_flush();
    const u32 warnings_before = de_atomic_load_u32(&g_cache_warnings, DE_ATOMIC_RELAXED);

    expect_rejected(f, dir, path, file, size / 2u, "truncated");
    expect_rejected(f, dir, path, "hello, not an archive", 21, "garbage");
    expect_rejected(f, dir, path, "", 0, "empty");

    de_log_flush();
    const u32 warnings = de_atomic_load_u32(&g_cache_warnings, DE_ATOMIC_RELAXED) - warnings_before;
    de_log_remove_sink(cache_sink, NULL);
    if (log_started)
        de_log_shutdown();
    printf("pipeline cache warnings logged: %u\n", warnings);
    EXPECT_EQ(warnings, 3u); /* one per bad file */
    DE_FREE(de_heap_allocator(), file, (size_t)size + 1u);
}

/* With shader validation on (the mtl.* tests on the host), the device has no archive. */
TEST_F(mtl, binary_archive_off_with_shader_validation, pipe_fx) {
    need_shaders(f);
    const char *sv = getenv("MTL_SHADER_VALIDATION");
    if (!sv || !sv[0] || strcmp(sv, "0") == 0)
        SKIP(
            "Metal shader validation is off in this process (CI): mtl_archive.* cover the archive");
    f->d = make_device(f, cache_dir("validation"));
    EXPECT_EQ(de__mtl_archive_state(f->d, NULL, NULL, NULL), (u32)DE__MTL_ARCHIVE_NONE);
    u64 ns = 0;
    u32 n = 0;
    run_pipelines(f->d, &ns, &n);
}

/* ---------- Pure ---------- */

TEST(mtl, scissor_rect) {
    u32 r[4];
    de__mtl_scissor_rect(4, 8, 16, 32, 64, 64, r);
    EXPECT_EQ(r[0], 4u);
    EXPECT_EQ(r[1], 8u);
    EXPECT_EQ(r[2], 16u);
    EXPECT_EQ(r[3], 32u);
    de__mtl_scissor_rect(-10, -3, 16, 2, 64, 64, r); /* cut left and above */
    EXPECT_EQ(r[0], 0u);
    EXPECT_EQ(r[1], 0u);
    EXPECT_EQ(r[2], 6u);
    EXPECT_EQ(r[3], 0u);
    de__mtl_scissor_rect(60, 50, 100, 100, 64, 64, r); /* cut right and below */
    EXPECT_EQ(r[0], 60u);
    EXPECT_EQ(r[1], 50u);
    EXPECT_EQ(r[2], 4u);
    EXPECT_EQ(r[3], 14u);
    de__mtl_scissor_rect(100, 100, 5, 5, 64, 64, r); /* outside: empty, inside the area */
    EXPECT_EQ(r[0], 64u);
    EXPECT_EQ(r[2], 0u);
    EXPECT_EQ(r[3], 0u);
    de__mtl_scissor_rect(INT32_MAX - 1, 0, UINT32_MAX, 1, 64, 64, r);
    EXPECT_EQ(r[2], 0u);
    de__mtl_scissor_rect(INT32_MIN, INT32_MIN, UINT32_MAX, UINT32_MAX, 64, 64, r);
    EXPECT_EQ(r[0], 0u);
    EXPECT_EQ(r[2], 64u);
    EXPECT_EQ(r[3], 64u);
}

TEST(mtl, archive_file_name) {
    char name[128];
    de__mtl_archive_file_name(0x1000002A5ull, "Apple M5 Pro", "25G83", name, sizeof name);
    EXPECT_STREQ(name, "metal-00000001000002a5-25G83.metallib");
    de__mtl_archive_file_name(0, "Apple M5 Pro", "25G83", name, sizeof name);
    EXPECT_STREQ(name, "metal-AppleM5Pro-25G83.metallib");
    de__mtl_archive_file_name(0, NULL, "", name, sizeof name);
    EXPECT_STREQ(name, "metal-device-unknown.metallib");
    de__mtl_archive_file_name(7, "x", "25/../G83", name, sizeof name);
    EXPECT_STREQ(name, "metal-0000000000000007-25G83.metallib");
}
