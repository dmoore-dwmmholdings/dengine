/* rhi_basic.c — the first golden scenes: the S10 RHI features through de_gpu (DENG-329).
 *
 *   rhi_clear               an RGBA16F target cleared to an HDR color (the tone-mapped readback).
 *   rhi_triangle            four triangles with interpolated vertex colors, depth-tested
 *                           (reverse-Z) and back-face culled: one is hidden in part, one culled.
 *   rhi_textured_quad       a rotated quad sampling a 32x32 texture through bindless indices,
 *                           linear filtering, repeat addressing.
 *   rhi_compute_gradient    a compute shader writes the image into a buffer; a copy puts it in
 *                           the target.
 *   rhi_indirect_count      a compute shader writes 4 indexed draw records and the count 3;
 *                           de_gpu_cmd_draw_indexed_indirect draws 3 of the 4 triangles.
 *   rhi_mesh_triangle       amplification + mesh shader hexagon fan (requires mesh_shaders).
 *   rhi_selftest_perturbed  rhi_triangle with every vertex color off by 8/255, compared with the
 *                           rhi_triangle reference: it must fail (scenes.toml expect_fail).
 *
 * Shaders: tests/golden/shaders/. Buffers the shaders read are GPU_ONLY storage buffers filled by
 * a copy (golden.h). Settings are fixed; nothing depends on time.
 */
#include "golden.h"

#include <string.h>

/* ---------- rhi_clear ---------- */

DE_GOLDEN_SCENE(rhi_clear) {
    /* HDR blue (4.0) shows the tone mapping of RGBA16F targets in the readback. */
    const float clear[4] = {0.25f, 0.5f, 4.0f, 1.0f};
    de_golden_begin_render(ctx, clear, (de_gpu_texture){0});
    de_golden_end_render(ctx);
}

/* ---------- rhi_triangle (and its perturbed self test) ---------- */

typedef struct draw_data {
    float bias[4];
    u32 verts, first;
} draw_data;

/* Position (clip space, y up) and color per vertex. */
static const float k_tri_verts[12][2][4] = {
    /* 0: in front (z 0.6), drawn first */
    {{-0.2f, -0.9f, 0.6f, 1.0f}, {1.0f, 0.9f, 0.1f, 1.0f}},
    {{0.9f, 0.2f, 0.6f, 1.0f}, {0.1f, 0.9f, 0.9f, 1.0f}},
    {{-0.6f, 0.5f, 0.6f, 1.0f}, {0.9f, 0.1f, 0.8f, 1.0f}},
    /* 1: behind it (z 0.3), drawn second: depth test hides its overlap */
    {{-0.8f, -0.7f, 0.3f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
    {{0.7f, -0.6f, 0.3f, 1.0f}, {0.0f, 1.0f, 0.0f, 1.0f}},
    {{-0.1f, 0.8f, 0.3f, 1.0f}, {0.0f, 0.0f, 1.0f, 1.0f}},
    /* 2: between the two (z 0.45) */
    {{0.1f, 0.1f, 0.45f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
    {{0.6f, 0.4f, 0.45f, 1.0f}, {0.5f, 0.5f, 0.5f, 1.0f}},
    {{0.2f, 0.7f, 0.45f, 1.0f}, {0.2f, 0.2f, 0.2f, 1.0f}},
    /* 3: clockwise: culled */
    {{0.5f, -0.5f, 0.9f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
    {{0.95f, -0.5f, 0.9f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
    {{0.9f, -0.9f, 0.9f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
};

/* A GRAPHICS pipeline of triangle.slang for the target format. */
static de_gpu_pipeline triangle_pipeline(de_golden_ctx *ctx, bool depth, de_gpu_cull cull) {
    de_golden_shader vs, ps;
    if (!de_golden_get_shader(ctx, "triangle.slang", "vs_main", &vs) ||
        !de_golden_get_shader(ctx, "triangle.slang", "ps_main", &ps))
        return (de_gpu_pipeline){0};
    de_gpu_pipeline_desc pd;
    memset(&pd, 0, sizeof pd);
    pd.kind = DE_PIPE_GRAPHICS;
    pd.vs = vs.blob;
    pd.ps = ps.blob;
    pd.color_formats[0] = ctx->format;
    pd.color_count = 1;
    pd.depth_format = depth ? DE_FMT_D32_FLOAT : DE_FMT_UNKNOWN;
    pd.depth_test = pd.depth_write = depth;
    pd.cull = cull;
    pd.push_constant_bytes = vs.push_constant_bytes > ps.push_constant_bytes
                                 ? vs.push_constant_bytes
                                 : ps.push_constant_bytes;
    pd.name = "golden triangle";
    return de_golden_pipeline(ctx, &pd);
}

static void triangles(de_golden_ctx *ctx, float bias) {
    de_gpu_pipeline pipe = triangle_pipeline(ctx, true, DE_CULL_BACK);
    de_gpu_buffer vb =
        de_golden_storage_buffer(ctx, k_tri_verts, sizeof k_tri_verts, 16, 0, "triangle vertices");
    de_gpu_texture depth = de_golden_depth_target(ctx);
    if (de_golden_stopped(ctx))
        return;
    const float clear[4] = {0.1f, 0.1f, 0.15f, 1.0f};
    de_golden_begin_render(ctx, clear, depth);
    de_gpu_cmd_set_pipeline(ctx->cmd, pipe);
    for (u32 t = 0; t < 4; ++t) {
        draw_data dd = {{bias, bias, bias, 0.0f}, de_gpu_buffer_bindless(ctx->device, vb), t * 3u};
        de_gpu_cmd_push_constants(ctx->cmd, &dd, sizeof dd);
        de_gpu_cmd_draw(ctx->cmd, 3, 1, 0, 0);
    }
    de_golden_end_render(ctx);
}

DE_GOLDEN_SCENE(rhi_triangle) {
    triangles(ctx, 0.0f);
}

DE_GOLDEN_SCENE(rhi_selftest_perturbed) {
    triangles(ctx, 8.0f / 255.0f);
}

/* ---------- rhi_textured_quad ---------- */

DE_GOLDEN_SCENE(rhi_textured_quad) {
    /* 32x32: 8x8-texel cells in four flat colors. Flat cells keep the reference PNG small; the
     * cell edges show the linear filtering, the repeat (uv 0..2) the address mode. */
    static const u8 k_colors[4][3] = {{230, 90, 40}, {40, 60, 200}, {240, 200, 40}, {110, 50, 170}};
    u8 px[32 * 32 * 4];
    for (u32 y = 0; y < 32; ++y) {
        for (u32 x = 0; x < 32; ++x) {
            u8 *p = px + (y * 32u + x) * 4u;
            memcpy(p, k_colors[((x / 8u) + (y / 8u) * 3u) % 4u], 3);
            p[3] = 255;
        }
    }
    de_golden_shader vs, ps;
    if (!de_golden_get_shader(ctx, "quad.slang", "vs_main", &vs) ||
        !de_golden_get_shader(ctx, "quad.slang", "ps_main", &ps))
        return;
    de_gpu_pipeline_desc pd;
    memset(&pd, 0, sizeof pd);
    pd.kind = DE_PIPE_GRAPHICS;
    pd.vs = vs.blob;
    pd.ps = ps.blob;
    pd.color_formats[0] = ctx->format;
    pd.color_count = 1;
    pd.cull = DE_CULL_NONE;
    pd.push_constant_bytes = vs.push_constant_bytes > ps.push_constant_bytes
                                 ? vs.push_constant_bytes
                                 : ps.push_constant_bytes;
    pd.name = "golden quad";
    de_gpu_pipeline pipe = de_golden_pipeline(ctx, &pd);
    de_gpu_texture tex = de_golden_texture_rgba8(ctx, 32, 32, px, "golden quad texture");
    de_gpu_sampler smp = de_golden_sampler(ctx, &(de_gpu_sampler_desc){.min = DE_FILTER_LINEAR,
                                                                       .mag = DE_FILTER_LINEAR,
                                                                       .mip = DE_FILTER_NEAREST,
                                                                       .u = DE_ADDR_WRAP,
                                                                       .v = DE_ADDR_WRAP,
                                                                       .w = DE_ADDR_WRAP});
    if (de_golden_stopped(ctx))
        return;
    const float clear[4] = {0.05f, 0.05f, 0.05f, 1.0f};
    de_golden_begin_render(ctx, clear, (de_gpu_texture){0});
    de_gpu_cmd_set_pipeline(ctx->cmd, pipe);
    const struct {
        float rot[2];
        float scale;
        u32 tex, smp;
    } pc = {{0.93969262f, 0.34202015f}, /* 20 degrees */
            0.7f,
            de_gpu_texture_bindless(ctx->device, tex, 0, false),
            de_gpu_sampler_bindless(ctx->device, smp)};
    de_gpu_cmd_push_constants(ctx->cmd, &pc, sizeof pc);
    de_gpu_cmd_draw(ctx->cmd, 6, 1, 0, 0);
    de_golden_end_render(ctx);
}

/* ---------- rhi_compute_gradient ---------- */

static de_gpu_pipeline compute_pipeline(de_golden_ctx *ctx, const char *file) {
    de_golden_shader cs;
    if (!de_golden_get_shader(ctx, file, "cs_main", &cs))
        return (de_gpu_pipeline){0};
    de_gpu_pipeline_desc pd;
    memset(&pd, 0, sizeof pd);
    pd.kind = DE_PIPE_COMPUTE;
    pd.cs = cs.blob;
    memcpy(pd.threadgroup_size, cs.threadgroup, sizeof pd.threadgroup_size);
    pd.push_constant_bytes = cs.push_constant_bytes;
    pd.name = file;
    return de_golden_pipeline(ctx, &pd);
}

static de_gpu_barrier buffer_barrier(de_gpu_buffer b, u32 before, u32 after) {
    de_gpu_barrier x;
    memset(&x, 0, sizeof x);
    x.buffer = b;
    x.before = before;
    x.after = after;
    return x;
}

DE_GOLDEN_SCENE(rhi_compute_gradient) {
    const u32 pitch = ctx->width * 4u;
    if (ctx->format != DE_FMT_RGBA8_UNORM || pitch % 256u != 0) {
        de_golden_fail(ctx, "needs an rgba8 target whose width is a multiple of 64");
        return;
    }
    de_gpu_pipeline pipe = compute_pipeline(ctx, "gradient.slang");
    de_gpu_buffer buf = de_golden_storage_buffer(ctx, NULL, (u64)pitch * ctx->height, 4,
                                                 DE_USAGE_COPY_SRC, "gradient texels");
    if (de_golden_stopped(ctx))
        return;
    de_gpu_cmd_set_pipeline(ctx->cmd, pipe);
    const u32 params[4] = {de_gpu_buffer_bindless(ctx->device, buf), ctx->width, ctx->height, 32};
    de_gpu_cmd_push_constants(ctx->cmd, params, sizeof params);
    de_gpu_cmd_dispatch(ctx->cmd, (ctx->width + 7u) / 8u, (ctx->height + 7u) / 8u, 1);
    de_gpu_barrier b = buffer_barrier(buf, DE_ACCESS_SHADER_WRITE, DE_ACCESS_COPY_SRC);
    de_gpu_cmd_barriers(ctx->cmd, &b, 1);
    de_golden_target_barrier(ctx, DE_ACCESS_COPY_DST);
    de_gpu_cmd_copy_buffer_to_texture(ctx->cmd, ctx->target, 0, 0, buf, 0, pitch);
}

/* ---------- rhi_indirect_count ---------- */

#define INDIRECT_RECORDS 4u
#define INDIRECT_COUNT   3u

/* One triangle per quadrant (counter-clockwise); the fourth, top right, is past the count. */
static const float k_quad_tris[12][2][4] = {
    {{-0.9f, 0.1f, 0.5f, 1.0f}, {1.0f, 0.2f, 0.2f, 1.0f}},
    {{-0.1f, 0.1f, 0.5f, 1.0f}, {1.0f, 0.8f, 0.2f, 1.0f}},
    {{-0.5f, 0.9f, 0.5f, 1.0f}, {1.0f, 0.2f, 0.6f, 1.0f}},
    {{-0.9f, -0.9f, 0.5f, 1.0f}, {0.2f, 1.0f, 0.2f, 1.0f}},
    {{-0.1f, -0.9f, 0.5f, 1.0f}, {0.2f, 1.0f, 0.8f, 1.0f}},
    {{-0.5f, -0.1f, 0.5f, 1.0f}, {0.6f, 1.0f, 0.2f, 1.0f}},
    {{0.1f, -0.9f, 0.5f, 1.0f}, {0.2f, 0.2f, 1.0f, 1.0f}},
    {{0.9f, -0.9f, 0.5f, 1.0f}, {0.2f, 0.8f, 1.0f, 1.0f}},
    {{0.5f, -0.1f, 0.5f, 1.0f}, {0.6f, 0.2f, 1.0f, 1.0f}},
    {{0.1f, 0.1f, 0.5f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
    {{0.9f, 0.1f, 0.5f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
    {{0.5f, 0.9f, 0.5f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
};

DE_GOLDEN_SCENE(rhi_indirect_count) {
    de_gpu_pipeline args_pipe = compute_pipeline(ctx, "indirect_args.slang");
    de_gpu_pipeline draw_pipe = triangle_pipeline(ctx, false, DE_CULL_NONE);
    de_gpu_buffer vb =
        de_golden_storage_buffer(ctx, k_quad_tris, sizeof k_quad_tris, 16, 0, "indirect vertices");
    u16 idx[INDIRECT_RECORDS * 3u];
    for (u16 i = 0; i < INDIRECT_RECORDS * 3u; ++i)
        idx[i] = i;
    de_gpu_buffer ib =
        de_golden_upload_buffer(ctx, idx, sizeof idx, DE_USAGE_INDEX, "indirect indices");
    de_gpu_buffer args = de_golden_storage_buffer(ctx, NULL, INDIRECT_RECORDS * 20u, 4,
                                                  DE_USAGE_INDIRECT, "indirect args");
    de_gpu_buffer count =
        de_golden_storage_buffer(ctx, NULL, 16, 4, DE_USAGE_INDIRECT, "indirect count");
    if (de_golden_stopped(ctx))
        return;
    de_gpu_cmd_set_pipeline(ctx->cmd, args_pipe);
    const u32 params[4] = {de_gpu_buffer_bindless(ctx->device, args),
                           de_gpu_buffer_bindless(ctx->device, count), INDIRECT_RECORDS,
                           INDIRECT_COUNT};
    de_gpu_cmd_push_constants(ctx->cmd, params, sizeof params);
    de_gpu_cmd_dispatch(ctx->cmd, 1, 1, 1);
    de_gpu_barrier b[2] = {buffer_barrier(args, DE_ACCESS_SHADER_WRITE, DE_ACCESS_INDIRECT),
                           buffer_barrier(count, DE_ACCESS_SHADER_WRITE, DE_ACCESS_INDIRECT)};
    de_gpu_cmd_barriers(ctx->cmd, b, 2);
    const float clear[4] = {0.15f, 0.1f, 0.1f, 1.0f};
    de_golden_begin_render(ctx, clear, (de_gpu_texture){0});
    de_gpu_cmd_set_pipeline(ctx->cmd, draw_pipe);
    draw_data dd = {{0, 0, 0, 0}, de_gpu_buffer_bindless(ctx->device, vb), 0};
    de_gpu_cmd_push_constants(ctx->cmd, &dd, sizeof dd);
    de_gpu_cmd_set_index_buffer(ctx->cmd, ib, 0, true);
    de_gpu_cmd_draw_indexed_indirect(ctx->cmd, args, 0, count, 0, INDIRECT_RECORDS);
    de_golden_end_render(ctx);
}

/* ---------- rhi_mesh_triangle ---------- */

DE_GOLDEN_SCENE(rhi_mesh_triangle) {
    de_golden_shader as, ms, ps;
    if (!de_golden_get_shader(ctx, "mesh.slang", "as_main", &as) ||
        !de_golden_get_shader(ctx, "mesh.slang", "ms_main", &ms) ||
        !de_golden_get_shader(ctx, "mesh.slang", "ps_main", &ps))
        return;
    de_gpu_pipeline_desc pd;
    memset(&pd, 0, sizeof pd);
    pd.kind = DE_PIPE_MESH;
    pd.as = as.blob;
    pd.ms = ms.blob;
    pd.ps = ps.blob;
    memcpy(pd.threadgroup_size, ms.threadgroup, sizeof pd.threadgroup_size);
    memcpy(pd.object_threadgroup_size, as.threadgroup, sizeof pd.object_threadgroup_size);
    pd.color_formats[0] = ctx->format;
    pd.color_count = 1;
    pd.cull = DE_CULL_BACK;
    u32 bytes = as.push_constant_bytes;
    bytes = ms.push_constant_bytes > bytes ? ms.push_constant_bytes : bytes;
    bytes = ps.push_constant_bytes > bytes ? ps.push_constant_bytes : bytes;
    pd.push_constant_bytes = bytes;
    pd.name = "golden mesh";
    de_gpu_pipeline pipe = de_golden_pipeline(ctx, &pd);
    if (de_golden_stopped(ctx))
        return;
    const float clear[4] = {0.1f, 0.12f, 0.1f, 1.0f};
    de_golden_begin_render(ctx, clear, (de_gpu_texture){0});
    de_gpu_cmd_set_pipeline(ctx->cmd, pipe);
    const struct {
        float bias[4];
        float scale;
    } pc = {{0, 0, 0, 0}, 0.8f};
    de_gpu_cmd_push_constants(ctx->cmd, &pc, sizeof pc);
    de_gpu_cmd_dispatch_mesh(ctx->cmd, 1, 1, 1);
    de_golden_end_render(ctx);
}
