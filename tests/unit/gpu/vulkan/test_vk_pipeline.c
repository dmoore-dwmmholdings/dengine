/* test_vk_pipeline.c — Vulkan pipelines, draws, dispatches and the pipeline cache (DENG-323).
 * CTest names: gpu_vulkan.vk.<test>.
 *
 * The GPU tests use the public de_gpu API and the shared S10 pipeline tests
 * (tests/unit/gpu/data/pipelines/: Slang shaders cooked through the S09 importer, inputs and
 * expected values), so DENG-324 (D3D12) and DENG-325 (Metal) mirror them with the same names and
 * values:
 *   - pipeline_triangle_readback: vertex pulling, push-constant color, back-face culling (front =
 *     counter-clockwise), reverse-Z depth test; three pixels read back.
 *   - pipeline_compute_bindless: a compute shader reads and writes buffers through bindless
 *     indices (on the compute queue); the buffer reads back exactly.
 *   - draw_indirect_count: a compute pass writes 3 draw records and count 2; exactly two draws
 *     appear. Then the same records without a count buffer: all three.
 *   - mesh_pipeline: amplification + mesh + pixel shaders draw the triangle test's first
 *     triangle; DE_ERR_UNSUPPORTED (and a skip) without mesh shaders.
 *   - push_constant_limit: 128 bytes of push constants arrive intact; 132 bytes, or fewer bytes
 *     than the shader uses, are refused.
 *   - pipeline_cache_roundtrip / pipeline_cache_corrupt: the persistent cache.
 * The device is the best Base-tier one, or adapter 1 forced when there is none (MoltenVK): the
 * forced device's bindless tables are smaller, which these tests do not mind. Every device has
 * the debug layer with GPU-assisted and synchronization validation; with
 * VK_LAYER_KHRONOS_validation installed the tests require zero validation errors. Without a driver,
 * or without the shader importer in the build, they SKIP with the reason.
 *
 * spirv_scan, pipeline_validation_pure, scissor_rect and pipeline_cache_check are pure (no
 * device): the SPIR-V reader, the scissor cut and the cache-file checks.
 */
#include "de_test.h"

#include <dengine/de_gpu.h>
#include <dengine/de_platform.h>

#include "gpu/vulkan/vk_internal.h"
#include "pipeline_shaders.h"

#include <stdio.h>
#include <string.h>

#define WAIT_NS ((u64)10 * 1000 * 1000 * 1000)
#define SIZE    DE_PIPE_TEST_SIZE
#define PITCH   (DE_PIPE_TEST_SIZE * 4u)

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
    bool forced;
    char skip[1200];
    char err[1100];
} pipe_fx;

/* SPIR-V for the shared shaders, or SKIP. */
static void need_shaders(pipe_fx *f) {
    char why[300];
    if (!de_pipe_test_cook(DE__SHADER_TARGET_SPIRV, why, sizeof why)) {
        snprintf(f->skip, sizeof f->skip, "shared pipeline shaders not cooked: %s", why);
        SKIP(f->skip);
    }
}

static de_pipe_test_shader shader(const char *file, const char *entry) {
    de_pipe_test_shader s;
    if (!de_pipe_test_get(DE__SHADER_TARGET_SPIRV, file, entry, &s)) {
        printf("missing cooked shader %s:%s\n", file, entry);
        ASSERT_TRUE(false);
    }
    return s;
}

/* A Vulkan device with validation on the fixture's tracking allocator: the best Base-tier device,
 * else adapter 1 forced. `cache_dir` is the pipeline cache directory ("" = none). */
static de_gpu_device *make_device(pipe_fx *f, const char *cache_dir) {
    de_gpu_device_desc desc = {.backend = DE_GPU_VULKAN,
                               .debug_layer = true,
                               .gpu_validation = true,
                               .allocator = &f->alloc,
                               .pipeline_cache_dir = cache_dir};
    de_gpu_device *d = NULL;
    de_result r = de_gpu_device_create(&desc, &d);
    if (r == DE_ERR_UNSUPPORTED) {
        desc.adapter = 1; /* not Base-tier capable (MoltenVK): force it */
        r = de_gpu_device_create(&desc, &d);
    }
    if (r == DE_ERR_UNSUPPORTED) {
        snprintf(f->skip, sizeof f->skip, "no usable Vulkan device: %s",
                 errchain(f->err, sizeof f->err));
        SKIP(f->skip);
    }
    ASSERT_EQ(r, DE_OK);
    f->forced = desc.adapter != 0;
    return d;
}

static void pipe_fx_setup(pipe_fx *f) {
    f->alloc = de_tracking_allocator_init(&f->tracker, NULL, DE_MEM_RENDER);
}

/* Destroys a device after checking its validation counter. */
static void end_device(de_gpu_device *d) {
    if (!d)
        return;
    u32 warnings = 0;
    EXPECT_EQ(de__gpu_vk_validation_messages(d, &warnings), 0u);
    de_gpu_device_destroy(d);
}

static void pipe_fx_teardown(pipe_fx *f) {
    end_device(f->d);
    f->d = NULL;
    de_tracking_stats st = de_tracking_allocator_stats(&f->tracker);
    EXPECT_EQ(st.blocks_live, 0u);
    EXPECT_EQ(st.bytes_live, 0u);
}

/* The fixture's device, without a persistent pipeline cache. */
static void open_device(pipe_fx *f) {
    need_shaders(f);
    f->d = make_device(f, "");
    printf("device: \"%s\"%s; validation %s\n", de_gpu_get_caps(f->d)->device_name,
           f->forced ? " (forced: not Base-tier capable)" : "",
           de__gpu_vk_validation_enabled(f->d) ? "on" : "NOT installed: not checked");
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

static void submit_wait(de_gpu_device *d, de_gpu_queue q, de_gpu_cmd *c) {
    de_gpu_fence fence = de_gpu_fence_create(d, 0);
    ASSERT_TRUE(DE_HANDLE_VALID(fence));
    de_gpu_submit s = {.cmds = &c, .cmd_count = 1, .signal = fence, .signal_value = 1};
    ASSERT_EQ(de_gpu_queue_submit(d, q, &s), DE_OK);
    ASSERT_EQ(de_gpu_fence_wait(d, fence, 1, WAIT_NS), DE_OK);
    de_gpu_fence_destroy(d, fence);
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

/* A render target (+ readback buffer, + depth) for one render test. */
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

/* Barriers into rendering, begin_render with the shared clear values (depth 0: reverse-Z far). */
static void begin_target(de_gpu_cmd *c, const target *t) {
    de_gpu_barrier b[2] = {tex_barrier(t->color, DE_ACCESS_NONE, DE_ACCESS_RENDER_TARGET),
                           tex_barrier(t->depth, DE_ACCESS_NONE, DE_ACCESS_DEPTH_WRITE)};
    de_gpu_cmd_barriers(c, b, DE_HANDLE_VALID(t->depth) ? 2u : 1u);
    de_gpu_attachment color = {
        .texture = t->color, .clear = true, .clear_color = {DE_PIPE_TEST_CLEAR_F}, .store = true};
    de_gpu_attachment depth = {.texture = t->depth, .clear = true, .clear_depth = 0.0f};
    de_gpu_cmd_begin_render(c, &color, 1, DE_HANDLE_VALID(t->depth) ? &depth : NULL);
}

/* end_render and the copy to the readback buffer. */
static void end_target(de_gpu_cmd *c, const target *t) {
    de_gpu_cmd_end_render(c);
    de_gpu_barrier b = tex_barrier(t->color, DE_ACCESS_RENDER_TARGET, DE_ACCESS_COPY_SRC);
    de_gpu_cmd_barriers(c, &b, 1);
    de_gpu_cmd_copy_texture_to_buffer(c, t->readback, 0, PITCH, t->color, 0, 0);
}

static void expect_pixels(const target *t, const de_pipe_test_pixel *px, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        const u8 *p = t->pixels + (size_t)px[i].y * PITCH + (size_t)px[i].x * 4u;
        if (memcmp(p, px[i].rgba, 4) != 0)
            printf("pixel (%u, %u): got %u %u %u %u, expected %u %u %u %u\n", px[i].x, px[i].y,
                   p[0], p[1], p[2], p[3], px[i].rgba[0], px[i].rgba[1], px[i].rgba[2],
                   px[i].rgba[3]);
        EXPECT_MEMEQ(p, px[i].rgba, 4);
    }
}

/* Push constants of triangle.slang and mesh.slang. */
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

/* ---------- Triangle ---------- */

TEST_F(vk, pipeline_triangle_readback, pipe_fx) {
    open_device(f);
    de_gpu_pipeline pipe = triangle_pipeline(f->d, true);
    de_gpu_buffer pos = upload(f->d, de_pipe_test_triangle_pos, sizeof de_pipe_test_triangle_pos,
                               DE_USAGE_STORAGE, 16, "triangle positions");
    target t = make_target(f->d, true);

    /* Twice: the second pass reuses the recycled list, so the bindless set is bound again. */
    for (u32 pass = 0; pass < 2; ++pass) {
        de_error_clear();
        de_gpu_cmd *c = de_gpu_cmd_begin(f->d, DE_QUEUE_GRAPHICS, "triangle");
        ASSERT_TRUE(c != NULL);
        begin_target(c, &t);
        de_gpu_cmd_set_pipeline(c, pipe);
        for (u32 k = 0; k < 3; ++k) {
            draw_data dd;
            memcpy(dd.color, de_pipe_test_triangle_color[k], sizeof dd.color);
            dd.positions = de_gpu_buffer_bindless(f->d, pos);
            dd.first = 3u * k;
            de_gpu_cmd_push_constants(c, &dd, sizeof dd);
            de_gpu_cmd_draw(c, 3, 1, 0, 0);
        }
        end_target(c, &t);
        EXPECT_TRUE(de_error_last() == NULL);
        submit_wait(f->d, DE_QUEUE_GRAPHICS, c);
        expect_pixels(&t, de_pipe_test_triangle_expect, 3);
        memset(t.pixels, 0xEE, (size_t)PITCH * SIZE);
    }
    free_target(f->d, &t);
    de_gpu_buffer_destroy(f->d, pos);
    de_gpu_pipeline_destroy(f->d, pipe);
}

/* ---------- Compute ---------- */

TEST_F(vk, pipeline_compute_bindless, pipe_fx) {
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

    /* On the compute queue: its own family on most devices (MoltenVK too). */
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
    de_gpu_buffer_destroy(f->d, in);
    de_gpu_buffer_destroy(f->d, out);
    de_gpu_buffer_destroy(f->d, rb);
    de_gpu_pipeline_destroy(f->d, pipe);
}

/* ---------- Indirect count ---------- */

TEST_F(vk, draw_indirect_count, pipe_fx) {
    open_device(f);
    /* Without drawIndirectCount (MoltenVK) only the pass without a count buffer runs, and the
     * test then skips: the count is what it is about. */
    const bool has_count = de__vk(f->d)->phys.f12.drawIndirectCount != VK_FALSE;
    de_gpu_pipeline args_pipe = compute_pipeline(f->d, "indirect_args.slang", "indirect args");
    de_gpu_pipeline draw_pipe = triangle_pipeline(f->d, false);
    de_gpu_buffer pos = upload(f->d, de_pipe_test_indirect_pos, sizeof de_pipe_test_indirect_pos,
                               DE_USAGE_STORAGE, 16, "indirect positions");
    u16 idx[10] = {0};
    memcpy(idx, de_pipe_test_indirect_indices, sizeof de_pipe_test_indirect_indices);
    de_gpu_buffer ib = upload(f->d, idx, sizeof idx, DE_USAGE_INDEX, 0, "indirect indices");
    de_gpu_buffer args = de_gpu_buffer_create(
        f->d, &(de_gpu_buffer_desc){.size = DE_PIPE_TEST_INDIRECT_RECORDS * 20u,
                                    .usage = DE_USAGE_STORAGE | DE_USAGE_INDIRECT,
                                    .stride = 4,
                                    .name = "indirect args"});
    de_gpu_buffer count = de_gpu_buffer_create(
        f->d, &(de_gpu_buffer_desc){.size = 16,
                                    .usage = DE_USAGE_STORAGE | DE_USAGE_INDIRECT,
                                    .stride = 4,
                                    .name = "indirect count"});
    ASSERT_TRUE(DE_HANDLE_VALID(args) && DE_HANDLE_VALID(count));
    target t = make_target(f->d, false);

    /* Pass 0: with the count buffer (2 of 3 records). Pass 1: without it (all 3). */
    for (u32 pass = has_count ? 0u : 1u; pass < 2; ++pass) {
        de_error_clear();
        de_gpu_cmd *c = de_gpu_cmd_begin(f->d, DE_QUEUE_GRAPHICS, "indirect count");
        ASSERT_TRUE(c != NULL);
        de_gpu_barrier b[2] = {buf_barrier(args, DE_ACCESS_NONE, DE_ACCESS_SHADER_WRITE),
                               buf_barrier(count, DE_ACCESS_NONE, DE_ACCESS_SHADER_WRITE)};
        de_gpu_cmd_barriers(c, b, 2);
        de_gpu_cmd_set_pipeline(c, args_pipe);
        const u32 params[4] = {de_gpu_buffer_bindless(f->d, args),
                               de_gpu_buffer_bindless(f->d, count), DE_PIPE_TEST_INDIRECT_RECORDS,
                               DE_PIPE_TEST_INDIRECT_COUNT};
        de_gpu_cmd_push_constants(c, params, sizeof params);
        de_gpu_cmd_dispatch(c, 1, 1, 1);
        b[0] = buf_barrier(args, DE_ACCESS_SHADER_WRITE, DE_ACCESS_INDIRECT);
        b[1] = buf_barrier(count, DE_ACCESS_SHADER_WRITE, DE_ACCESS_INDIRECT);
        de_gpu_cmd_barriers(c, b, 2);
        begin_target(c, &t);
        de_gpu_cmd_set_pipeline(c, draw_pipe);
        draw_data dd;
        memcpy(dd.color, de_pipe_test_triangle_color[0], sizeof dd.color);
        dd.positions = de_gpu_buffer_bindless(f->d, pos);
        dd.first = 0;
        de_gpu_cmd_push_constants(c, &dd, sizeof dd);
        de_gpu_cmd_set_index_buffer(c, ib, 0, true);
        de_gpu_cmd_draw_indexed_indirect(c, args, 0, pass == 0 ? count : (de_gpu_buffer){0}, 0,
                                         DE_PIPE_TEST_INDIRECT_RECORDS);
        end_target(c, &t);
        EXPECT_TRUE(de_error_last() == NULL);
        submit_wait(f->d, DE_QUEUE_GRAPHICS, c);
        if (pass == 0) {
            expect_pixels(&t, de_pipe_test_indirect_expect, 3);
        } else {
            de_pipe_test_pixel all[3];
            memcpy(all, de_pipe_test_indirect_expect, sizeof all);
            memcpy(all[2].rgba, all[0].rgba, 4); /* the third draw ran too */
            expect_pixels(&t, all, 3);
        }
    }
    free_target(f->d, &t);
    de_gpu_buffer_destroy(f->d, pos);
    de_gpu_buffer_destroy(f->d, ib);
    de_gpu_buffer_destroy(f->d, args);
    de_gpu_buffer_destroy(f->d, count);
    de_gpu_pipeline_destroy(f->d, args_pipe);
    de_gpu_pipeline_destroy(f->d, draw_pipe);
    if (!has_count) {
        /* Recording with a count buffer is refused, and records nothing. */
        de_gpu_cmd *c = de_gpu_cmd_begin(f->d, DE_QUEUE_GRAPHICS, "no count");
        ASSERT_TRUE(c != NULL);
        de_gpu_pipeline p = triangle_pipeline(f->d, false);
        de_gpu_buffer a = de_gpu_buffer_create(
            f->d, &(de_gpu_buffer_desc){.size = 64, .usage = DE_USAGE_INDIRECT | DE_USAGE_INDEX});
        target t2 = make_target(f->d, false);
        begin_target(c, &t2);
        de_gpu_cmd_set_pipeline(c, p);
        de_gpu_cmd_set_index_buffer(c, a, 0, true);
        de_error_clear();
        de_gpu_cmd_draw_indexed_indirect(c, a, 0, a, 60, 1);
        EXPECT_TRUE(de_error_last() != NULL && de_error_last()->result == DE_ERR_UNSUPPORTED);
        end_target(c, &t2);
        submit_wait(f->d, DE_QUEUE_GRAPHICS, c);
        free_target(f->d, &t2);
        de_gpu_buffer_destroy(f->d, a);
        de_gpu_pipeline_destroy(f->d, p);
        SKIP("this Vulkan device has no drawIndirectCount: the indirect pass without a count ran "
             "(3 draws) and the count variant was refused with DE_ERR_UNSUPPORTED; the count pass "
             "needs a device with drawIndirectCount");
    }
}

/* ---------- Mesh ---------- */

TEST_F(vk, mesh_pipeline, pipe_fx) {
    open_device(f);
    de_pipe_test_shader as, ms, ps;
    memset(&as, 0, sizeof as);
    memset(&ms, 0, sizeof ms);
    memset(&ps, 0, sizeof ps);
    const bool cooked = de_pipe_test_get(DE__SHADER_TARGET_SPIRV, "mesh.slang", "as_main", &as) &&
                        de_pipe_test_get(DE__SHADER_TARGET_SPIRV, "mesh.slang", "ms_main", &ms) &&
                        de_pipe_test_get(DE__SHADER_TARGET_SPIRV, "mesh.slang", "ps_main", &ps);
    ASSERT_TRUE(cooked);
    de_gpu_pipeline_desc pd = {
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
    EXPECT_EQ(ms.threadgroup[0], 3u);
    EXPECT_EQ(as.threadgroup[0], 1u);
    de_error_clear();
    de_gpu_pipeline pipe = de_gpu_pipeline_create(f->d, &pd);
    if (!de_gpu_get_caps(f->d)->mesh_shaders) {
        EXPECT_FALSE(DE_HANDLE_VALID(pipe));
        ASSERT_TRUE(de_error_last() != NULL);
        EXPECT_EQ(de_error_last()->result, DE_ERR_UNSUPPORTED);
        SKIP("no VK_EXT_mesh_shader on this device: DE_ERR_UNSUPPORTED checked, the mesh draw "
             "needs hardware with mesh shaders");
    }
    ASSERT_TRUE(DE_HANDLE_VALID(pipe));
    de_gpu_buffer pos = upload(f->d, de_pipe_test_triangle_pos, sizeof de_pipe_test_triangle_pos,
                               DE_USAGE_STORAGE, 16, "mesh positions");
    const u32 one[3] = {1, 1, 1};
    de_gpu_buffer args = upload(f->d, one, sizeof one, DE_USAGE_INDIRECT, 0, "mesh args");
    const u32 cnt[1] = {1};
    de_gpu_buffer count = upload(f->d, cnt, sizeof cnt, DE_USAGE_INDIRECT, 0, "mesh count");
    target t = make_target(f->d, false);
    /* Pass 0: de_gpu_cmd_dispatch_mesh. Pass 1: the indirect-count variant. */
    for (u32 pass = 0; pass < 2; ++pass) {
        de_error_clear();
        de_gpu_cmd *c = de_gpu_cmd_begin(f->d, DE_QUEUE_GRAPHICS, "mesh");
        ASSERT_TRUE(c != NULL);
        begin_target(c, &t);
        de_gpu_cmd_set_pipeline(c, pipe);
        draw_data dd;
        memcpy(dd.color, de_pipe_test_triangle_color[0], sizeof dd.color);
        dd.positions = de_gpu_buffer_bindless(f->d, pos);
        dd.first = 0;
        de_gpu_cmd_push_constants(c, &dd, sizeof dd);
        if (pass == 0)
            de_gpu_cmd_dispatch_mesh(c, 1, 1, 1);
        else
            de_gpu_cmd_dispatch_mesh_indirect(c, args, 0, count, 0, 1);
        end_target(c, &t);
        EXPECT_TRUE(de_error_last() == NULL);
        submit_wait(f->d, DE_QUEUE_GRAPHICS, c);
        /* A only: the first two expected pixels, and the clear color at the third. */
        expect_pixels(&t, de_pipe_test_triangle_expect, 3);
    }
    free_target(f->d, &t);
    de_gpu_buffer_destroy(f->d, pos);
    de_gpu_buffer_destroy(f->d, args);
    de_gpu_buffer_destroy(f->d, count);
    de_gpu_pipeline_destroy(f->d, pipe);
}

/* ---------- Push-constant limit ---------- */

TEST_F(vk, push_constant_limit, pipe_fx) {
    open_device(f);
    EXPECT_EQ(de_gpu_get_caps(f->d)->max_push_constant_bytes, DE_GPU_MAX_PUSH_CONSTANT_BYTES);
    de_pipe_test_shader cs = shader("push_limit.slang", "cs_main");
    EXPECT_EQ(cs.push_constant_bytes, 128u);
    de_gpu_pipeline_desc pd = {
        .kind = DE_PIPE_COMPUTE, .cs = cs.blob, .push_constant_bytes = 132, .name = "push 132"};
    /* Over the limit: refused by the front-end. */
    de_error_clear();
    EXPECT_FALSE(DE_HANDLE_VALID(de_gpu_pipeline_create(f->d, &pd)));
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(strstr(de_error_last()->message, "limit is 128") != NULL);
    /* Fewer bytes than the shader uses: refused by the SPIR-V check. */
    pd.push_constant_bytes = 120;
    pd.name = "push 120";
    de_error_clear();
    EXPECT_FALSE(DE_HANDLE_VALID(de_gpu_pipeline_create(f->d, &pd)));
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
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
    /* More than the pipeline declares: the command is refused and skipped (Debug/Dev). */
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

/* ---------- Pipeline creation checks (device) ---------- */

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

TEST_F(vk, pipeline_validation, pipe_fx) {
    open_device(f);
    de_pipe_test_shader vs = shader("triangle.slang", "vs_main");
    de_pipe_test_shader ps = shader("triangle.slang", "ps_main");
    de_pipe_test_shader cs = shader("compute.slang", "cs_main");
    /* A vertex shader given as the compute stage. */
    de_gpu_pipeline_desc pd = {.kind = DE_PIPE_COMPUTE, .cs = vs.blob, .push_constant_bytes = 24};
    expect_refused(f->d, &pd, "execution model");
    /* A blob of a stage the kind does not use (front-end). */
    pd = (de_gpu_pipeline_desc){
        .kind = DE_PIPE_COMPUTE, .cs = cs.blob, .vs = vs.blob, .push_constant_bytes = 16};
    expect_refused(f->d, &pd, "has no vs stage");
    /* Not SPIR-V. */
    pd = (de_gpu_pipeline_desc){.kind = DE_PIPE_COMPUTE, .cs = cs.blob, .push_constant_bytes = 16};
    static const u32 junk[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    pd.cs = (de_gpu_shader_blob){junk, sizeof junk, "cs_main"};
    expect_refused(f->d, &pd, "not SPIR-V");
    /* A threadgroup size that is not the shader's (64, 1, 1). */
    pd = (de_gpu_pipeline_desc){.kind = DE_PIPE_COMPUTE,
                                .cs = cs.blob,
                                .push_constant_bytes = 16,
                                .threadgroup_size = {32, 1, 1}};
    expect_refused(f->d, &pd, "threadgroup size (32, 1, 1)");
    /* A pixel shader output without a color format. */
    pd = (de_gpu_pipeline_desc){.kind = DE_PIPE_GRAPHICS,
                                .vs = vs.blob,
                                .ps = ps.blob,
                                .push_constant_bytes = 24,
                                .depth_format = DE_FMT_D32_FLOAT,
                                .depth_test = true};
    expect_refused(f->d, &pd, "SV_Target0");
    /* The same pipeline with its color format, and an empty entry name (the first entry point),
     * is fine; so are all four blend modes and cull modes. */
    for (u32 blend = DE_BLEND_OPAQUE; blend <= DE_BLEND_ADD; ++blend) {
        pd.color_count = 1;
        pd.color_formats[0] = DE_FMT_RGBA8_UNORM;
        pd.blend[0] = (de_gpu_blend)blend;
        pd.cull = (de_gpu_cull)(blend % 3u);
        pd.vs.entry = blend == 0 ? "" : vs.blob.entry;
        de_error_clear();
        de_gpu_pipeline p = de_gpu_pipeline_create(f->d, &pd);
        EXPECT_TRUE(DE_HANDLE_VALID(p));
        de_gpu_pipeline_destroy(f->d, p);
    }
}

/* ---------- Pipeline cache ---------- */

static const char *cache_dir(const char *sub) {
    static bool mounted;
    if (!mounted) {
        ASSERT_EQ(de_fs_mount("pipecache", DE_PIPE_TEST_CACHE_DIR, DE_MOUNT_WRITABLE, 0), DE_OK);
        mounted = true;
    }
    static char buf[64];
    snprintf(buf, sizeof buf, "pipecache:/%s", sub);
    (void)de_dir_create(buf);
    return buf;
}

/* Creates the three shared pipelines; returns the time it took. */
static u64 create_pipelines(de_gpu_device *d, de_gpu_pipeline out[3]) {
    const u64 t0 = de_time_now_ns();
    out[0] = triangle_pipeline(d, true);
    out[1] = compute_pipeline(d, "compute.slang", "compute");
    out[2] = compute_pipeline(d, "indirect_args.slang", "indirect args");
    return de_time_now_ns() - t0;
}

static void destroy_pipelines(de_gpu_device *d, de_gpu_pipeline p[3]) {
    for (u32 i = 0; i < 3; ++i)
        de_gpu_pipeline_destroy(d, p[i]);
}

/* A device that creates the shared pipelines and saves its cache in `dir`. */
static void fill_cache(pipe_fx *f, const char *dir, char *path, size_t cap) {
    de_gpu_device *d = make_device(f, dir);
    const char *p = NULL;
    (void)de__gpu_vk_pipeline_cache_state(d, NULL, &p);
    snprintf(path, cap, "%s", p);
    de_gpu_pipeline pipes[3];
    (void)create_pipelines(d, pipes);
    destroy_pipelines(d, pipes);
    end_device(d);
}

TEST_F(vk, pipeline_cache_roundtrip, pipe_fx) {
    need_shaders(f);
    const char *dir = cache_dir("roundtrip");
    /* Start without a file. */
    de_gpu_device *d = make_device(f, dir);
    u64 loaded = 0;
    const char *p = NULL;
    u32 state = de__gpu_vk_pipeline_cache_state(d, &loaded, &p);
    char path[512];
    snprintf(path, sizeof path, "%s", p);
    printf("cache file: %s\n", path);
    ASSERT_TRUE(strstr(path, "pipecache:/roundtrip/vulkan-") == path);
    if (de_path_exists(path)) {
        de_gpu_device_destroy(d);
        ASSERT_EQ(de_file_delete(path), DE_OK);
        d = make_device(f, dir);
        state = de__gpu_vk_pipeline_cache_state(d, &loaded, NULL);
    }
    EXPECT_EQ(state, (u32)DE__VK_PIPELINE_CACHE_EMPTY);
    EXPECT_EQ(loaded, 0u);
    de_gpu_pipeline pipes[3];
    const u64 cold = create_pipelines(d, pipes);
    destroy_pipelines(d, pipes);
    end_device(d);

    /* The file now exists, with a header and Vulkan data. */
    void *file = NULL;
    u64 size = 0;
    ASSERT_EQ(de_file_read_all(path, NULL, &file, &size), DE_OK);
    EXPECT_GT(size, (u64)sizeof(de__vk_pipeline_cache_file) + 32u);
    DE_FREE(de_heap_allocator(), file, (size_t)size + 1u);

    /* Second run: the cache loads, and creation is (usually) faster. */
    d = make_device(f, dir);
    state = de__gpu_vk_pipeline_cache_state(d, &loaded, NULL);
    EXPECT_EQ(state, (u32)DE__VK_PIPELINE_CACHE_LOADED);
    EXPECT_GT(loaded, 0u);
    const u64 warm = create_pipelines(d, pipes);
    printf("pipeline creation: %.3f ms without the cache, %.3f ms with %llu cached bytes\n",
           (double)cold / 1e6, (double)warm / 1e6, (unsigned long long)loaded);
    destroy_pipelines(d, pipes);
    end_device(d);
}

/* Writes `bytes` to the cache file, then checks that a device ignores it and still works. */
static void expect_rejected(pipe_fx *f, const char *dir, const char *path, const void *bytes,
                            u64 size, const char *what) {
    printf("case: %s\n", what);
    ASSERT_EQ(de_file_write_all_atomic(path, bytes, size), DE_OK);
    de_gpu_device *d = make_device(f, dir);
    u64 loaded = 1;
    EXPECT_EQ(de__gpu_vk_pipeline_cache_state(d, &loaded, NULL),
              (u32)DE__VK_PIPELINE_CACHE_REJECTED);
    EXPECT_EQ(loaded, 0u);
    de_gpu_pipeline pipes[3];
    (void)create_pipelines(d, pipes);
    destroy_pipelines(d, pipes);
    end_device(d);
}

/* Counts warnings about the pipeline cache (log sink). */
static de_atomic_u32 g_cache_warnings;
static void cache_sink(const de_log_record *rec, void *user) {
    (void)user;
    if (rec->level == DE_LOG_WARN && rec->message.len > 0 &&
        strstr(rec->message.ptr, "pipeline cache") != NULL)
        de_atomic_fetch_add_u32(&g_cache_warnings, 1u, DE_ATOMIC_RELAXED);
}

TEST_F(vk, pipeline_cache_corrupt, pipe_fx) {
    need_shaders(f);
    const char *dir = cache_dir("corrupt");
    char path[512];
    fill_cache(f, dir, path, sizeof path);
    void *file = NULL;
    u64 size = 0;
    ASSERT_EQ(de_file_read_all(path, NULL, &file, &size), DE_OK);
    ASSERT_GT(size, (u64)sizeof(de__vk_pipeline_cache_file) + 32u);
    u8 *good = (u8 *)file;
    static u8 bad[1u << 20];
    ASSERT_LE(size, (u64)sizeof bad);

    /* Sinks only run with logging initialized (the log thread); shut it down at the end. */
    const bool log_started = de_log_init(&(de_log_desc){.no_console_sink = false}) == DE_OK;
    de_error_clear();
    de_log_add_sink(cache_sink, NULL);
    de_log_flush();
    const u32 warnings_before = de_atomic_load_u32(&g_cache_warnings, DE_ATOMIC_RELAXED);

    /* One byte of the Vulkan data flipped: the checksum catches it. */
    memcpy(bad, good, (size_t)size);
    bad[size - 7] ^= 0x5A;
    expect_rejected(f, dir, path, bad, size, "a flipped data byte");
    /* Truncated. */
    expect_rejected(f, dir, path, good, size - 16, "truncated");
    /* Not a cache file at all. */
    expect_rejected(f, dir, path, "hello, not a cache", 18, "garbage");
    /* A well-formed file whose Vulkan header names another pipelineCacheUUID (checksum fixed). */
    memcpy(bad, good, (size_t)size);
    de__vk_pipeline_cache_file h;
    memcpy(&h, bad, sizeof h);
    VkPipelineCacheHeaderVersionOne vh;
    memcpy(&vh, bad + sizeof h, sizeof vh);
    vh.pipelineCacheUUID[0] ^= 0xFF;
    memcpy(bad + sizeof h, &vh, sizeof vh);
    h.data_hash = de_hash64(bad + sizeof h, (size_t)h.data_size, DE__VK_PIPELINE_CACHE_MAGIC);
    memcpy(bad, &h, sizeof h);
    expect_rejected(f, dir, path, bad, size, "another driver's cache");

    de_log_flush();
    const u32 warnings = de_atomic_load_u32(&g_cache_warnings, DE_ATOMIC_RELAXED) - warnings_before;
    de_log_remove_sink(cache_sink, NULL);
    if (log_started)
        de_log_shutdown();
    printf("pipeline cache warnings logged: %u\n", warnings);
    EXPECT_EQ(warnings, 4u); /* one per bad file */
    /* The last device saved a good cache over the bad one. */
    void *again = NULL;
    u64 again_size = 0;
    ASSERT_EQ(de_file_read_all(path, NULL, &again, &again_size), DE_OK);
    u64 data_size = 0;
    char why[200];
    de_gpu_device *d = make_device(f, "");
    EXPECT_TRUE(de__vk_pipeline_cache_check(again, again_size, &de__vk(d)->phys.props.properties,
                                            &data_size, why, sizeof why) != NULL);
    end_device(d);
    DE_FREE(de_heap_allocator(), again, (size_t)again_size + 1u);
    DE_FREE(de_heap_allocator(), file, (size_t)size + 1u);
}

/* ---------- Pure tests ---------- */

TEST(vk, scissor_rect) {
    VkRect2D r = de__vk_scissor_rect(4, 8, 16, 32);
    EXPECT_EQ(r.offset.x, 4);
    EXPECT_EQ(r.offset.y, 8);
    EXPECT_EQ(r.extent.width, 16u);
    EXPECT_EQ(r.extent.height, 32u);
    r = de__vk_scissor_rect(-10, -3, 16, 2);
    EXPECT_EQ(r.offset.x, 0);
    EXPECT_EQ(r.offset.y, 0);
    EXPECT_EQ(r.extent.width, 6u);
    EXPECT_EQ(r.extent.height, 0u);
    r = de__vk_scissor_rect(INT32_MAX - 1, 0, 100, 1);
    EXPECT_EQ(r.offset.x, INT32_MAX - 1);
    EXPECT_EQ(r.extent.width, 1u);
}

/* de__vk_spirv_scan, printing the reason on failure. */
static de_result scan(const de_pipe_test_shader *s, const char *entry, de__vk_spirv_info *info) {
    char why[300] = "";
    de_result r = de__vk_spirv_scan(s->blob.code, s->blob.size, entry, de_heap_allocator(), info,
                                    why, sizeof why);
    if (r != DE_OK)
        printf("scan(\"%s\"): %s\n", entry ? entry : "(null)", why);
    return r;
}

TEST(vk, spirv_scan) {
    char why[300];
    if (!de_pipe_test_cook(DE__SHADER_TARGET_SPIRV, why, sizeof why))
        SKIP("shared pipeline shaders not cooked (no shader importer or Slang)");
    const de_allocator *a = de_heap_allocator();
    de__vk_spirv_info info;
    de_pipe_test_shader s;

    ASSERT_TRUE(de_pipe_test_get(DE__SHADER_TARGET_SPIRV, "compute.slang", "cs_main", &s));
    ASSERT_EQ(scan(&s, "cs_main", &info), DE_OK);
    EXPECT_EQ(info.model, (u32)DE__VK_SPIRV_GLCOMPUTE);
    EXPECT_EQ(info.local_size[0], 64u);
    EXPECT_EQ(info.local_size[1], 1u);
    EXPECT_EQ(info.local_size[2], 1u);
    EXPECT_EQ(info.push_constant_bytes, 16u);
    EXPECT_EQ(info.push_constant_bytes, s.push_constant_bytes); /* agrees with the importer */
    /* One entry point per cooked module: any name (and NULL) finds it, under its module name. */
    EXPECT_TRUE(info.entry[0] != '\0');
    ASSERT_EQ(scan(&s, NULL, &info), DE_OK);
    EXPECT_EQ(info.model, (u32)DE__VK_SPIRV_GLCOMPUTE);
    ASSERT_EQ(scan(&s, "other", &info), DE_OK);
    EXPECT_EQ(info.local_size[0], 64u);

    /* A hand-made module with two entry points: names must match. */
    static const u32 two[] = {
        0x07230203u,
        0x00010600u,
        0,
        3,
        0,
        (4u << 16) | 15u,
        DE__VK_SPIRV_GLCOMPUTE,
        1,
        0x00000061u, /* "a" */
        (4u << 16) | 15u,
        DE__VK_SPIRV_VERTEX,
        2,
        0x00006262u, /* "bb" */
        (6u << 16) | 16u,
        1,
        17,
        8,
        4,
        2, /* LocalSize 8 4 2 */
    };
    const de_pipe_test_shader m = {.blob = {two, sizeof two, NULL}};
    ASSERT_EQ(scan(&m, "bb", &info), DE_OK);
    EXPECT_EQ(info.model, (u32)DE__VK_SPIRV_VERTEX);
    EXPECT_STREQ(info.entry, "bb");
    EXPECT_EQ(info.local_size[0], 0u);
    ASSERT_EQ(scan(&m, NULL, &info), DE_OK);
    EXPECT_STREQ(info.entry, "a");
    EXPECT_EQ(info.local_size[0], 8u);
    EXPECT_EQ(info.local_size[1], 4u);
    EXPECT_EQ(info.local_size[2], 2u);
    EXPECT_EQ(de__vk_spirv_scan(two, sizeof two, "c", a, &info, why, sizeof why), DE_ERR_PARSE);
    EXPECT_TRUE(strstr(why, "no entry point \"c\" (it has \"a\", \"bb\")") != NULL);

    ASSERT_TRUE(de_pipe_test_get(DE__SHADER_TARGET_SPIRV, "push_limit.slang", "cs_main", &s));
    ASSERT_EQ(scan(&s, "cs_main", &info), DE_OK);
    EXPECT_EQ(info.push_constant_bytes, 128u);
    EXPECT_EQ(info.local_size[0], 32u);

    ASSERT_TRUE(de_pipe_test_get(DE__SHADER_TARGET_SPIRV, "triangle.slang", "vs_main", &s));
    ASSERT_EQ(scan(&s, "vs_main", &info), DE_OK);
    EXPECT_EQ(info.model, (u32)DE__VK_SPIRV_VERTEX);
    EXPECT_EQ(info.push_constant_bytes, s.push_constant_bytes);
    EXPECT_EQ(info.output_locations, 0u); /* SV_Position is a built-in */
    ASSERT_TRUE(de_pipe_test_get(DE__SHADER_TARGET_SPIRV, "triangle.slang", "ps_main", &s));
    ASSERT_EQ(scan(&s, "ps_main", &info), DE_OK);
    EXPECT_EQ(info.model, (u32)DE__VK_SPIRV_FRAGMENT);
    EXPECT_EQ(info.output_locations, 1u); /* SV_Target0 */

    if (de_pipe_test_get(DE__SHADER_TARGET_SPIRV, "mesh.slang", "ms_main", &s)) {
        ASSERT_EQ(scan(&s, "ms_main", &info), DE_OK);
        EXPECT_EQ(info.model, (u32)DE__VK_SPIRV_MESH_EXT);
        EXPECT_EQ(info.local_size[0], 3u);
        ASSERT_TRUE(de_pipe_test_get(DE__SHADER_TARGET_SPIRV, "mesh.slang", "as_main", &s));
        ASSERT_EQ(scan(&s, "as_main", &info), DE_OK);
        EXPECT_EQ(info.model, (u32)DE__VK_SPIRV_TASK_EXT);
    }

    /* Damaged modules: bad magic, a size that is not whole words, a truncated instruction. */
    ASSERT_TRUE(de_pipe_test_get(DE__SHADER_TARGET_SPIRV, "compute.slang", "cs_main", &s));
    static u8 copy[1u << 16];
    ASSERT_LE(s.blob.size, (u64)sizeof copy);
    memcpy(copy, s.blob.code, (size_t)s.blob.size);
    copy[0] ^= 1;
    EXPECT_EQ(de__vk_spirv_scan(copy, s.blob.size, NULL, a, &info, why, sizeof why), DE_ERR_PARSE);
    copy[0] ^= 1;
    EXPECT_EQ(de__vk_spirv_scan(copy, s.blob.size - 2, NULL, a, &info, why, sizeof why),
              DE_ERR_PARSE);
    /* The first instruction claims more words than the module has. */
    const u32 huge = 0xFFFF0000u | (copy[20] | (copy[21] << 8));
    memcpy(copy + 20, &huge, 4);
    EXPECT_EQ(de__vk_spirv_scan(copy, s.blob.size, NULL, a, &info, why, sizeof why), DE_ERR_PARSE);
    EXPECT_TRUE(strstr(why, "truncated") != NULL);
}

TEST(vk, pipeline_cache_check) {
    VkPhysicalDeviceProperties props;
    memset(&props, 0, sizeof props);
    props.vendorID = 0x106b;
    props.deviceID = 0x1234;
    for (u32 i = 0; i < VK_UUID_SIZE; ++i)
        props.pipelineCacheUUID[i] = (u8)(i * 9u + 1u);
    /* A file: our header, the Vulkan header, 40 bytes of driver data. */
    static u8
        file[sizeof(de__vk_pipeline_cache_file) + sizeof(VkPipelineCacheHeaderVersionOne) + 40u];
    VkPipelineCacheHeaderVersionOne vh;
    memset(&vh, 0, sizeof vh);
    vh.headerSize = sizeof vh;
    vh.headerVersion = VK_PIPELINE_CACHE_HEADER_VERSION_ONE;
    vh.vendorID = props.vendorID;
    vh.deviceID = props.deviceID;
    memcpy(vh.pipelineCacheUUID, props.pipelineCacheUUID, VK_UUID_SIZE);
    const size_t hs = sizeof(de__vk_pipeline_cache_file);
    memcpy(file + hs, &vh, sizeof vh);
    for (u32 i = 0; i < 40; ++i)
        file[hs + sizeof vh + i] = (u8)(i ^ 0x3C);
    de__vk_pipeline_cache_file h = {.magic = DE__VK_PIPELINE_CACHE_MAGIC,
                                    .version = DE__VK_PIPELINE_CACHE_VERSION,
                                    .data_size = sizeof file - hs,
                                    .vendor_id = props.vendorID,
                                    .device_id = props.deviceID};
    h.data_hash = de_hash64(file + hs, sizeof file - hs, DE__VK_PIPELINE_CACHE_MAGIC);
    memcpy(file, &h, hs);
    char why[200];
    u64 n = 0;
    const void *data = de__vk_pipeline_cache_check(file, sizeof file, &props, &n, why, sizeof why);
    EXPECT_TRUE(data == file + hs);
    EXPECT_EQ(n, (u64)(sizeof file - hs));

#define REJECT(bytes, size, needle)                                                                \
    do {                                                                                           \
        why[0] = '\0';                                                                             \
        EXPECT_TRUE(de__vk_pipeline_cache_check((bytes), (size), &props, &n, why, sizeof why) ==   \
                    NULL);                                                                         \
        EXPECT_EQ(n, 0u);                                                                          \
        if (!strstr(why, (needle)))                                                                \
            printf("reason \"%s\" lacks \"%s\"\n", why, (needle));                                 \
        EXPECT_TRUE(strstr(why, (needle)) != NULL);                                                \
    } while (0)

    REJECT(file, 10, "shorter");
    REJECT(file, sizeof file - 1, "truncated");
    static u8 bad[sizeof file];
    memcpy(bad, file, sizeof file);
    bad[sizeof bad - 1] ^= 1;
    REJECT(bad, sizeof bad, "checksum");
    memcpy(bad, file, sizeof file);
    bad[0] ^= 1;
    REJECT(bad, sizeof bad, "not a DEngine");
    /* Another device: our header names it. */
    props.deviceID = 0x9999;
    REJECT(file, sizeof file, "not this device");
    props.deviceID = 0x1234;
    /* Another driver build: the UUID differs. */
    props.pipelineCacheUUID[3] ^= 0xFF;
    REJECT(file, sizeof file, "another device or driver");
    props.pipelineCacheUUID[3] ^= 0xFF;
    /* A Vulkan header with a bad version (checksum fixed up). */
    memcpy(bad, file, sizeof file);
    VkPipelineCacheHeaderVersionOne v2 = vh;
    v2.headerVersion = (VkPipelineCacheHeaderVersion)7;
    memcpy(bad + hs, &v2, sizeof v2);
    de__vk_pipeline_cache_file h2 = h;
    h2.data_hash = de_hash64(bad + hs, sizeof bad - hs, DE__VK_PIPELINE_CACHE_MAGIC);
    memcpy(bad, &h2, hs);
    REJECT(bad, sizeof bad, "bad Vulkan cache header");
#undef REJECT
}
