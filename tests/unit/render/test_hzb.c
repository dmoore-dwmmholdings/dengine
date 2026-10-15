/*
 * test_hzb.c: the HZB build and the occlusion test (DENG-233). Executable test_render_occlusion;
 * CTest names render_occlusion.hzb.<test>_<backend>.
 *
 *   build_matches_cpu_npot  random depths of odd and non-power-of-two sizes: every texel of every
 *                           mip equals a CPU min-reduction (bit for bit; min is exact), and a max
 *                           reduction for the op parameter (S15 DENG-339)
 *   single_pass_vs_multi    the grouped 6-mips-per-dispatch path and one dispatch per mip give
 *                           the same texels
 *   occlusion_conservative_random  random boxes, a tilted camera: the HZB of every box (the
 *                           culling-off reference depth, a CPU ray cast); phase 2 tests every box
 *                           against it (a cut, so phase 1 draws nothing); no box it rejects owns a
 *                           pixel of the reference (the nearest-box id per pixel), and it rejects
 *                           some
 */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "de_test.h"
#include "occlusion_fixture.h"

#include "render/graph/rg_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WAIT_NS (10ull * 1000000000ull)

static u32 align_up(u32 v, u32 a) {
    return (v + a - 1u) / a * a;
}

/* ---------- CPU reference ---------- */

static f32 op_of(f32 a, f32 b, u32 op) {
    return op == DE__VIS_HZB_OP_MAX ? (a > b ? a : b) : (a < b ? a : b);
}

/* Mips of the HZB of `src` (sw x sh), each mip tightly packed, in `out` (allocated). */
static f32 *cpu_hzb(const f32 *src, u32 sw, u32 sh, u32 op, u32 *w0, u32 *h0, u32 *mips) {
    de__vis_hzb_size(sw, sh, w0, h0, mips);
    size_t total = 0;
    for (u32 m = 0; m < *mips; ++m) {
        const u32 w = (*w0 >> m) ? (*w0 >> m) : 1u, h = (*h0 >> m) ? (*h0 >> m) : 1u;
        total += (size_t)w * h;
    }
    f32 *out = (f32 *)malloc(total * sizeof(f32));
    ASSERT_TRUE(out != NULL);
    for (u32 y = 0; y < *h0; ++y) {
        const u32 y0 = y * sh / *h0;
        u32 y1 = ((y + 1u) * sh + *h0 - 1u) / *h0;
        y1 = (y1 < sh ? y1 : sh) - 1u;
        for (u32 x = 0; x < *w0; ++x) {
            const u32 x0 = x * sw / *w0;
            u32 x1 = ((x + 1u) * sw + *w0 - 1u) / *w0;
            x1 = (x1 < sw ? x1 : sw) - 1u;
            f32 v = src[(size_t)y0 * sw + x0];
            for (u32 yy = y0; yy <= y1; ++yy)
                for (u32 xx = x0; xx <= x1; ++xx)
                    v = op_of(v, src[(size_t)yy * sw + xx], op);
            out[(size_t)y * *w0 + x] = v;
        }
    }
    size_t prev = 0, at = (size_t)*w0 * *h0;
    for (u32 m = 1; m < *mips; ++m) {
        const u32 pw = (*w0 >> (m - 1u)) ? (*w0 >> (m - 1u)) : 1u;
        const u32 ph = (*h0 >> (m - 1u)) ? (*h0 >> (m - 1u)) : 1u;
        const u32 w = (*w0 >> m) ? (*w0 >> m) : 1u, h = (*h0 >> m) ? (*h0 >> m) : 1u;
        for (u32 y = 0; y < h; ++y) {
            for (u32 x = 0; x < w; ++x) {
                const u32 ax = 2u * x < pw ? 2u * x : pw - 1u,
                          bx = 2u * x + 1u < pw ? 2u * x + 1u : pw - 1u;
                const u32 ay = 2u * y < ph ? 2u * y : ph - 1u,
                          by = 2u * y + 1u < ph ? 2u * y + 1u : ph - 1u;
                const f32 *p = out + prev;
                f32 v = p[(size_t)ay * pw + ax];
                v = op_of(v, p[(size_t)ay * pw + bx], op);
                v = op_of(v, p[(size_t)by * pw + ax], op);
                v = op_of(v, p[(size_t)by * pw + bx], op);
                out[at + (size_t)y * w + x] = v;
            }
        }
        prev = at;
        at += (size_t)w * h;
    }
    return out;
}

/* ---------- One GPU build ---------- */

typedef struct hzb_run {
    ocx *f;
    de_gpu_texture src;
    de_gpu_buffer upload, rb;
    u32 sw, sh, pitch;
    u32 w0, h0, mips;
    u64 mip_off[DE__VIS_HZB_MAX_MIPS];
    u32 mip_pitch[DE__VIS_HZB_MAX_MIPS];
    de_rg_resource src_res, hzb_res;
} hzb_run;

static void upload_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const hzb_run *r = (const hzb_run *)user;
    de_gpu_cmd_copy_buffer_to_texture(cmd, de_rg_texture(pass, r->src_res), 0, 0, r->upload, 0,
                                      r->pitch);
}

static void readback_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const hzb_run *r = (const hzb_run *)user;
    for (u32 m = 0; m < r->mips; ++m)
        de_gpu_cmd_copy_texture_to_buffer(cmd, r->rb, r->mip_off[m], r->mip_pitch[m],
                                          de_rg_texture(pass, r->hzb_res), m, 0);
}

/* Builds the HZB of `src` on the GPU and returns its mips tightly packed (allocated). */
static f32 *gpu_hzb(ocx *f, const f32 *src, u32 sw, u32 sh, u32 op, bool single) {
    hzb_run r;
    memset(&r, 0, sizeof r);
    r.f = f;
    r.sw = sw;
    r.sh = sh;
    r.pitch = align_up(sw * 4u, 256u);
    de__vis_hzb_size(sw, sh, &r.w0, &r.h0, &r.mips);
    r.src = de_gpu_texture_create(
        f->dev, &(de_gpu_texture_desc){.format = DE_FMT_R32_FLOAT,
                                       .width = sw,
                                       .height = sh,
                                       .usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST,
                                       .name = "hzb test source"});
    r.upload = de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = (u64)r.pitch * sh,
                                                                  .usage = DE_USAGE_COPY_SRC,
                                                                  .memory = DE_MEM_UPLOAD,
                                                                  .name = "hzb test upload"});
    u64 total = 0;
    for (u32 m = 0; m < r.mips; ++m) {
        const u32 w = (r.w0 >> m) ? (r.w0 >> m) : 1u, h = (r.h0 >> m) ? (r.h0 >> m) : 1u;
        r.mip_pitch[m] = align_up(w * 4u, 256u);
        r.mip_off[m] = total;
        total = (total + (u64)r.mip_pitch[m] * h + 511u) & ~(u64)511u;
    }
    r.rb = de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = total,
                                                              .usage = DE_USAGE_COPY_DST,
                                                              .memory = DE_MEM_READBACK,
                                                              .name = "hzb test readback"});
    ASSERT_TRUE(DE_HANDLE_VALID(r.src) && DE_HANDLE_VALID(r.upload) && DE_HANDLE_VALID(r.rb));
    u8 *up = (u8 *)de_gpu_buffer_map(f->dev, r.upload);
    for (u32 y = 0; y < sh; ++y)
        memcpy(up + (size_t)y * r.pitch, src + (size_t)y * sw, (size_t)sw * 4u);

    /* One frame. The HZB is a transient of the graph, so the graph knows its mips: an import's
     * mip count is unknown to it, and the state changes of a fresh import (NONE -> ...) would
     * also produce barriers past its last mip, which the backends apply to real mips. */
    /* Its own graph per build (kept until teardown): a new size makes a new plan, and a graph
     * whose transient pool grows destroys the old placements right after the last frame's fence,
     * before Metal's debug layer has seen that command buffer complete. */
    de_rg *g = ocx_graph(f, "hzb test");
    const u64 n = f->frames++;
    de_arena_reset(f->arena);
    ASSERT_EQ(de__rg_begin(g, f->arena), DE_OK);
    r.src_res = de_rg_import_texture(g, r.src, DE_ACCESS_NONE);
    r.hzb_res = de_rg_create_texture(
        g, &(de_gpu_texture_desc){.format = DE_FMT_R32_FLOAT,
                                  .width = r.w0,
                                  .height = r.h0,
                                  .mip_count = r.mips,
                                  .usage = DE_USAGE_STORAGE | DE_USAGE_SAMPLED | DE_USAGE_COPY_SRC,
                                  .name = "hzb test"});
    ASSERT_TRUE(DE_HANDLE_VALID(r.hzb_res));
    de_rg_pass *p = de_rg_add_pass(g, "test.upload", DE_QUEUE_GRAPHICS, upload_execute, &r);
    de_rg_write(p, r.src_res, DE_ACCESS_COPY_DST);
    const de__vis_hzb_target t = {
        .resource = r.hzb_res, .width = r.w0, .height = r.h0, .mips = r.mips};
    de__vis_hzb_add_passes(f->vis, g, r.src_res, sw, sh, &t, op, single);
    p = de_rg_add_pass(g, "test.readback", DE_QUEUE_GRAPHICS, readback_execute, &r);
    de_rg_pass_side_effect(p);
    de_rg_read(p, r.hzb_res, DE_ACCESS_COPY_SRC);
    de_rg_write(p, de_rg_import_buffer(g, r.rb, DE_ACCESS_NONE), DE_ACCESS_COPY_DST);
    /* Back to the shader-write state before the command list ends: Vulkan validation (lavapipe)
     * checks a command buffer's storage-image descriptor accesses against each image's layout
     * at the end of that command buffer, not at the access. */
    p = de_rg_add_pass(g, "test.restore", DE_QUEUE_GRAPHICS, NULL, NULL);
    de_rg_pass_side_effect(p);
    de_rg_read(p, r.hzb_res, DE_ACCESS_SHADER_READ);
    de_rg_write(p, r.hzb_res, DE_ACCESS_SHADER_WRITE);
    de_result res = de__rg_finalize(g);
    if (res == DE_OK)
        res = de__rg_compile(g);
    if (res == DE_OK)
        res =
            de__rg_execute(g, &(de__rg_execute_desc){
                                  .frame_index = n, .frame_fence = f->fence, .frame_value = n + 1});
    EXPECT_EQ(res, DE_OK);
    if (res != DE_OK)
        printf("  %s\n", de_error_last() ? de_error_last()->message : "?");
    EXPECT_EQ(de_gpu_fence_wait(f->dev, f->fence, n + 1, WAIT_NS), DE_OK);
    de__rg_reset(g);

    size_t texels = 0;
    for (u32 m = 0; m < r.mips; ++m) {
        const u32 w = (r.w0 >> m) ? (r.w0 >> m) : 1u, h = (r.h0 >> m) ? (r.h0 >> m) : 1u;
        texels += (size_t)w * h;
    }
    f32 *out = (f32 *)malloc(texels * sizeof(f32));
    ASSERT_TRUE(out != NULL);
    const u8 *m8 = (const u8 *)de_gpu_buffer_map(f->dev, r.rb);
    size_t at = 0;
    for (u32 m = 0; m < r.mips; ++m) {
        const u32 w = (r.w0 >> m) ? (r.w0 >> m) : 1u, h = (r.h0 >> m) ? (r.h0 >> m) : 1u;
        for (u32 y = 0; y < h; ++y)
            memcpy(out + at + (size_t)y * w, m8 + r.mip_off[m] + (size_t)y * r.mip_pitch[m],
                   (size_t)w * 4u);
        at += (size_t)w * h;
    }
    ocx_keep_texture(f, r.src); /* destroyed at teardown (occlusion_fixture.h, keep_tex) */
    ocx_keep_buffer(f, r.upload);
    ocx_keep_buffer(f, r.rb);
    return out;
}

static f32 *random_depth(u32 w, u32 h, u64 seed) {
    f32 *d = (f32 *)malloc((size_t)w * h * sizeof(f32));
    ASSERT_TRUE(d != NULL);
    u64 x = seed | 1u;
    for (size_t i = 0; i < (size_t)w * h; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        d[i] = (f32)((x >> 11) % 1000000u) / 1000000.0f;
    }
    return d;
}

static const u32 k_sizes[][2] = {{1280, 720}, {1001, 333}, {37, 17},  {1, 1},
                                 {3, 2049},   {640, 1},    {4096, 64}};
#define SIZE_COUNT (sizeof k_sizes / sizeof k_sizes[0])

/* Every mip of the GPU HZB equals the CPU's, bit for bit. Returns the mismatches. */
static u32 compare(const f32 *gpu, const f32 *cpu, u32 w0, u32 h0, u32 mips, const char *what) {
    u32 bad = 0;
    size_t at = 0;
    for (u32 m = 0; m < mips; ++m) {
        const u32 w = (w0 >> m) ? (w0 >> m) : 1u, h = (h0 >> m) ? (h0 >> m) : 1u;
        for (size_t i = 0; i < (size_t)w * h; ++i) {
            if (memcmp(&gpu[at + i], &cpu[at + i], 4) != 0) {
                if (bad++ < 4)
                    printf("  %s mip %u texel %zu: gpu %.9g, cpu %.9g\n", what, m, i,
                           (double)gpu[at + i], (double)cpu[at + i]);
            }
        }
        at += (size_t)w * h;
    }
    return bad;
}

static void build_matches_cpu_npot(ocx *f, de_gpu_backend b) {
    ocx_open(f, b, 16u);
    for (u32 k = 0; k < SIZE_COUNT; ++k) {
        const u32 sw = k_sizes[k][0], sh = k_sizes[k][1];
        f32 *src = random_depth(sw, sh, 77u + k);
        for (u32 op = 0; op < 2; ++op) {
            if (op == DE__VIS_HZB_OP_MAX && k != 1)
                continue; /* the max op once: the parameter works */
            u32 w0 = 0, h0 = 0, mips = 0;
            f32 *cpu = cpu_hzb(src, sw, sh, op, &w0, &h0, &mips);
            f32 *gpu = gpu_hzb(f, src, sw, sh, op, true);
            char what[64];
            snprintf(what, sizeof what, "%ux%u op %u", sw, sh, op);
            EXPECT_EQ(compare(gpu, cpu, w0, h0, mips, what), 0u);
            free(cpu);
            free(gpu);
        }
        free(src);
    }
}

static void single_pass_vs_multi(ocx *f, de_gpu_backend b) {
    ocx_open(f, b, 16u);
    for (u32 k = 0; k < SIZE_COUNT; ++k) {
        const u32 sw = k_sizes[k][0], sh = k_sizes[k][1];
        f32 *src = random_depth(sw, sh, 1234u + k);
        u32 w0 = 0, h0 = 0, mips = 0;
        f32 *cpu = cpu_hzb(src, sw, sh, DE__VIS_HZB_OP_MIN, &w0, &h0, &mips);
        f32 *one = gpu_hzb(f, src, sw, sh, DE__VIS_HZB_OP_MIN, true);
        f32 *multi = gpu_hzb(f, src, sw, sh, DE__VIS_HZB_OP_MIN, false);
        char what[64];
        snprintf(what, sizeof what, "%ux%u single vs multi", sw, sh);
        EXPECT_EQ(compare(one, multi, w0, h0, mips, what), 0u);
        snprintf(what, sizeof what, "%ux%u multi vs cpu", sw, sh);
        EXPECT_EQ(compare(multi, cpu, w0, h0, mips, what), 0u);
        free(cpu);
        free(one);
        free(multi);
        free(src);
    }
}

/* ---------- Conservativeness ---------- */

static u64 g_rng;
static f32 rndf(f32 lo, f32 hi) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return lo + (hi - lo) * (f32)((g_rng >> 11) % 1000000u) / 1000000.0f;
}

static void occlusion_conservative_random(ocx *f, de_gpu_backend b) {
    ocx_open(f, b, 1024u);
    /* A tilted camera: no box face is perpendicular to the view. */
    f->target = (de_vec3){0.2f, 1.0f, 0.1f};
    g_rng = 0xC0FFEEull;
    u32 n = 0;
    for (u32 i = 0; i < 40; ++i, ++n) /* large occluders */
        ocx_set_box(f, n, (de_vec3){rndf(-14, 14), rndf(10, 20), rndf(-6, 6)},
                    (de_vec3){rndf(1, 4), rndf(0.3f, 2), rndf(1, 4)});
    for (u32 i = 0; i < 760; ++i, ++n) /* boxes everywhere behind and between them */
        ocx_set_box(f, n, (de_vec3){rndf(-45, 45), rndf(8, 90), rndf(-22, 22)},
                    (de_vec3){rndf(0.2f, 1), rndf(0.2f, 1), rndf(0.2f, 1)});
    /* Frame 0 uploads the scene; frame 1 is the test: a cut, so phase 1 draws nothing and phase 2
     * tests every box against the HZB of the reference depth (every box drawn). */
    ocx_raycast(f, NULL);
    ASSERT_TRUE(ocx_frame(f));
    ASSERT_TRUE(ocx_cvar("r.occlusion_reset", "1"));
    ASSERT_TRUE(ocx_frame(f));
    EXPECT_EQ(f->item_count[0], 0u);
    bool *owns = (bool *)calloc(n, sizeof(bool));
    ASSERT_TRUE(owns != NULL);
    for (u32 i = 0; i < f->width * f->height; ++i)
        if (f->id_cpu[i] != UINT32_MAX)
            owns[f->id_cpu[i]] = true;
    const de__vis_cull_view v = ocx_cpu_view(f);
    u32 in_frustum = 0, culled = 0, wrong = 0, visible_px_owners = 0;
    for (u32 s = 0; s < n; ++s) {
        visible_px_owners += owns[s];
        if (!ocx_in_frustum(f, &v, s))
            continue;
        ++in_frustum;
        if (ocx_drawn(f, 1, s))
            continue;
        ++culled;
        if (owns[s]) {
            if (wrong++ < 8)
                printf("  box %u was culled by occlusion but owns pixels of the reference\n", s);
        }
    }
    printf("%u boxes in the frustum, %u own pixels, %u culled by occlusion\n", in_frustum,
           visible_px_owners, culled);
    EXPECT_EQ(wrong, 0u);
    EXPECT_GT(culled, in_frustum / 10u); /* the HZB rejects something */
    /* Every box that owns a pixel was drawn, and the counts agree. */
    EXPECT_EQ(f->counters[1][DE__VIS_COUNTER_INSTANCES_VISIBLE / 4u], in_frustum - culled);
    free(owns);
}

#define HZB_TESTS(sfx, backend)                                                                    \
    TEST_F(hzb, build_matches_cpu_npot_##sfx, ocx) {                                               \
        build_matches_cpu_npot(f, backend);                                                        \
    }                                                                                              \
    TEST_F(hzb, single_pass_vs_multi_##sfx, ocx) {                                                 \
        single_pass_vs_multi(f, backend);                                                          \
    }                                                                                              \
    TEST_F(hzb, occlusion_conservative_random_##sfx, ocx) {                                        \
        occlusion_conservative_random(f, backend);                                                 \
    }

HZB_TESTS(metal, DE_GPU_METAL)
HZB_TESTS(vulkan, DE_GPU_VULKAN)
HZB_TESTS(d3d12, DE_GPU_D3D12)

/* Sizes: the previous power of two (at least 1) and a full chain. */
TEST(hzb, size_rule) {
    u32 w = 0, h = 0, m = 0;
    de__vis_hzb_size(1280, 720, &w, &h, &m);
    EXPECT_EQ(w, 1024u);
    EXPECT_EQ(h, 512u);
    EXPECT_EQ(m, 11u);
    de__vis_hzb_size(1, 1, &w, &h, &m);
    EXPECT_EQ(w, 1u);
    EXPECT_EQ(h, 1u);
    EXPECT_EQ(m, 1u);
    de__vis_hzb_size(2049, 3, &w, &h, &m);
    EXPECT_EQ(w, 2048u);
    EXPECT_EQ(h, 2u);
    EXPECT_EQ(m, 12u);
    de__vis_hzb_size(1024, 1024, &w, &h, &m);
    EXPECT_EQ(w, 1024u);
    EXPECT_EQ(m, 11u);
}
