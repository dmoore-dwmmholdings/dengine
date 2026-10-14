/* test_rg_gpu.c: render graph compiler smoke test on real GPU backends (DENG-172).
 *
 * A compiled plan runs as the recorder (DENG-173) will run it: one command list per batch, each
 * step's barriers around its pass, timeline fences per queue. The graph clears three transient
 * render targets and copies each into a readback buffer: the first copy runs on the copy queue
 * (an ownership transfer each way and a fence pair), and the third target is aliased onto the
 * second's memory. The third target is also copied into a transient buffer and from there into a
 * fourth readback (DENG-414): the transient buffer keeps its placement, so the next frame's write
 * must wait for this frame's read. Four frames are submitted back to back (cache hits) before
 * one wait; the readbacks must hold the clear colours.
 * Metal runs with its API validation layer, Vulkan with validation and synchronization validation
 * (errors counted; on CI: lavapipe), D3D12 with the debug layer. Each backend SKIPs when it is not
 * built or has no device. */
#include "de_test.h"

#include <dengine/de_gpu.h>
#include <dengine/de_memory.h>
#include <dengine/de_render.h>

#include "render/graph/rg_internal.h"

#if DE_HAS_GPU_VULKAN
#include "gpu/vulkan/vk_internal.h"
#endif
#if defined(_WIN32) && DE_HAS_GPU_D3D12
#include "gpu/d3d12/d3d12_internal.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RT_SIZE  64u
#define RB_PITCH (RT_SIZE * 4u)
#define RB_BYTES ((u64)RB_PITCH * RT_SIZE)

typedef struct gfx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_arena *arena;
    de_gpu_device *dev;
    de_rg *g;
    de_gpu_buffer rb[4];
    de_gpu_fence fence[DE_QUEUE_COUNT_];
    u64 base[DE_QUEUE_COUNT_];
} gfx;

static void gfx_setup(gfx *f) {
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_RENDER);
    ASSERT_EQ(de_arena_create(&(de_arena_desc){.name = "rg_gpu_test"}, &f->arena), DE_OK);
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

static void gfx_teardown(gfx *f) {
    if (f->g) {
        de__rg_reset(f->g);
        de__rg_destroy(f->g);
    }
    if (f->dev) {
        de_gpu_wait_idle(f->dev);
        EXPECT_EQ(validation_errors(f->dev), 0u);
        for (u32 i = 0; i < 4; ++i)
            if (DE_HANDLE_VALID(f->rb[i]))
                de_gpu_buffer_destroy(f->dev, f->rb[i]);
        for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q)
            if (DE_HANDLE_VALID(f->fence[q]))
                de_gpu_fence_destroy(f->dev, f->fence[q]);
        de_gpu_device_destroy(f->dev);
    }
    if (f->arena)
        de_arena_destroy(f->arena);
    if (f->alloc.alloc)
        EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0);
}

static bool backend_built(de_gpu_backend b) {
    switch (b) {
#if DE_HAS_GPU_METAL
    case DE_GPU_METAL:
        return true;
#endif
#if DE_HAS_GPU_VULKAN
    case DE_GPU_VULKAN:
        return true;
#endif
#if DE_HAS_GPU_D3D12
    case DE_GPU_D3D12:
        return true;
#endif
    default:
        return false;
    }
}

static void start(gfx *f, de_gpu_backend b) {
    static char why[512];
    if (!backend_built(b)) {
        snprintf(why, sizeof why, "the %s backend is not in this build", de_gpu_backend_name(b));
        SKIP(why);
    }
#if defined(__APPLE__)
    if (b == DE_GPU_METAL) /* Metal API validation (only on Apple, where setenv is declared) */
        setenv("MTL_DEBUG_LAYER", "1", 0);
#endif
    de_gpu_device_desc desc = {.backend = b,
                               .debug_layer = true,
                               .gpu_validation = b == DE_GPU_VULKAN,
                               .pipeline_cache_dir = ""};
    de_result r = de_gpu_device_create(&desc, &f->dev);
    if (r == DE_ERR_UNSUPPORTED && b == DE_GPU_VULKAN) {
        desc.adapter = 1;
        r = de_gpu_device_create(&desc, &f->dev);
    }
    if (r != DE_OK) {
        f->dev = NULL;
        snprintf(why, sizeof why, "no usable %s device: %.400s", de_gpu_backend_name(b),
                 de_error_last() ? de_error_last()->message : "?");
        SKIP(why);
    }
    printf("device: \"%s\" (%s), async compute %d\n", de_gpu_get_caps(f->dev)->device_name,
           de_gpu_backend_name(b), de_gpu_get_caps(f->dev)->async_compute);
    ASSERT_EQ(de__rg_create(
                  &(de__rg_desc){.allocator = &f->alloc, .device = f->dev, .name = "smoke"}, &f->g),
              DE_OK);
    for (u32 i = 0; i < 4; ++i) {
        f->rb[i] = de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = RB_BYTES,
                                                                      .usage = DE_USAGE_COPY_DST,
                                                                      .memory = DE_MEM_READBACK,
                                                                      .name = "readback"});
        ASSERT_TRUE(DE_HANDLE_VALID(f->rb[i]));
    }
    for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q) {
        f->fence[q] = de_gpu_fence_create(f->dev, 0);
        ASSERT_TRUE(DE_HANDLE_VALID(f->fence[q]));
    }
}

/* ---------- The graph ---------- */

typedef struct clear_pass {
    de_rg_resource target;
    f32 color[4];
} clear_pass;

typedef struct copy_pass {
    de_rg_resource src, dst;
} copy_pass;

static void exec_clear(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const clear_pass *c = (const clear_pass *)user;
    de_gpu_attachment a = {.texture = de_rg_texture(pass, c->target), .clear = true, .store = true};
    memcpy(a.clear_color, c->color, sizeof a.clear_color);
    de_gpu_cmd_begin_render(cmd, &a, 1, NULL);
    de_gpu_cmd_end_render(cmd);
}

static void exec_copy(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const copy_pass *c = (const copy_pass *)user;
    de_gpu_cmd_copy_texture_to_buffer(cmd, de_rg_buffer(pass, c->dst), 0, RB_PITCH,
                                      de_rg_texture(pass, c->src), 0, 0);
}

typedef struct buffer_copy_pass {
    de_rg_resource src, dst;
} buffer_copy_pass;

static void exec_copy_buffer(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const buffer_copy_pass *c = (const buffer_copy_pass *)user;
    de_gpu_cmd_copy_buffer(cmd, de_rg_buffer(pass, c->dst), 0, de_rg_buffer(pass, c->src), 0,
                           RB_BYTES);
}

/* clears[3] / copies[3] as below, plus C -> transient buffer -> the fourth readback. */
static void build(gfx *f, clear_pass *clears, copy_pass *copies, buffer_copy_pass *via,
                  de_rg_resource *staging) {
    de_rg *g = f->g;
    static const f32 colors[3][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}};
    static const char *const names[3] = {"A", "B", "C"};
    for (u32 i = 0; i < 3; ++i) {
        const de_gpu_texture_desc td = {.format = DE_FMT_RGBA8_UNORM,
                                        .width = RT_SIZE,
                                        .height = RT_SIZE,
                                        .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_COPY_SRC,
                                        .name = names[i]};
        clears[i].target = de_rg_create_texture(g, &td);
        memcpy(clears[i].color, colors[i], sizeof colors[i]);
        copies[i].src = clears[i].target;
        copies[i].dst = de_rg_import_buffer(g, f->rb[i], DE_ACCESS_NONE);
        char name[16];
        snprintf(name, sizeof name, "clear%s", names[i]);
        de_rg_pass *c = de_rg_add_pass(g, name, DE_QUEUE_GRAPHICS, exec_clear, &clears[i]);
        de_rg_write(c, clears[i].target, DE_ACCESS_RENDER_TARGET);
        snprintf(name, sizeof name, "copy%s", names[i]);
        /* A's readback runs on the copy queue: a transfer each way. */
        de_rg_pass *k = de_rg_add_pass(g, name, i == 0 ? DE_QUEUE_COPY : DE_QUEUE_GRAPHICS,
                                       exec_copy, &copies[i]);
        de_rg_read(k, copies[i].src, DE_ACCESS_COPY_SRC);
        de_rg_write(k, copies[i].dst, DE_ACCESS_COPY_DST);
    }
    *staging =
        de_rg_create_buffer(g, &(de_gpu_buffer_desc){.size = RB_BYTES,
                                                     .usage = DE_USAGE_COPY_SRC | DE_USAGE_COPY_DST,
                                                     .name = "staging"});
    copies[3].src = clears[2].target;
    copies[3].dst = *staging;
    de_rg_pass *k = de_rg_add_pass(g, "stageC", DE_QUEUE_GRAPHICS, exec_copy, &copies[3]);
    de_rg_read(k, copies[3].src, DE_ACCESS_COPY_SRC);
    de_rg_write(k, copies[3].dst, DE_ACCESS_COPY_DST);
    via->src = *staging;
    via->dst = de_rg_import_buffer(g, f->rb[3], DE_ACCESS_NONE);
    de_rg_pass *v = de_rg_add_pass(g, "copyStaging", DE_QUEUE_GRAPHICS, exec_copy_buffer, via);
    de_rg_read(v, via->src, DE_ACCESS_COPY_SRC);
    de_rg_write(v, via->dst, DE_ACCESS_COPY_DST);
}

static void run_plan(gfx *f) {
    const de__rg_plan *p = f->g->plan;
    for (u32 bi = 0; bi < p->batch_count; ++bi) {
        const de__rg_batch *b = &p->batches[bi];
        de_gpu_cmd *cmd = de_gpu_cmd_begin(f->dev, b->queue, "rg_smoke");
        ASSERT_TRUE(cmd != NULL);
        for (u32 k = 0; k < b->step_count; ++k) {
            const u32 s = p->batch_steps[b->step_first + k];
            de__rg_record_barriers(f->g, s, DE__RG_PRE, cmd);
            if (p->steps[s].pass != DE__RG_NONE)
                de__rg_execute_pass(f->g, p->steps[s].pass, cmd);
            de__rg_record_barriers(f->g, s, DE__RG_POST, cmd);
        }
        de_gpu_wait waits[8];
        u32 nw = 0;
        for (u32 w = 0; w < b->wait_count && nw < 8; ++w) {
            const de__rg_wait *x = &p->waits[b->wait_first + w];
            waits[nw++] = (de_gpu_wait){f->fence[x->queue], f->base[x->queue] + x->signal};
        }
        de_gpu_submit sub = {.cmds = &cmd, .cmd_count = 1, .waits = waits, .wait_count = nw};
        if (b->signal) { /* signal n of the frame = base + n; wait 0 = the last before it */
            sub.signal = f->fence[b->queue];
            sub.signal_value = f->base[b->queue] + b->signal;
        }
        ASSERT_EQ(de_gpu_queue_submit(f->dev, b->queue, &sub), DE_OK);
    }
    for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q)
        f->base[q] += p->signal_count[q];
}

/* The barrier before `pass` on `resource` (its first use), or NULL. */
static const de__rg_cbarrier *first_barrier(const de_rg *g, const de_rg_pass *pass,
                                            de_rg_resource resource) {
    const de__rg_plan *p = g->plan;
    const de__rg_step *s = &p->steps[p->step_of_pass[pass->index]];
    const u32 ri = de__rg_resource_index(g, resource);
    for (u32 i = 0; i < s->barrier_count[DE__RG_PRE]; ++i) {
        const de__rg_cbarrier *b = &p->barriers[s->barrier_first[DE__RG_PRE] + i];
        if (b->resource == ri)
            return b;
    }
    return NULL;
}

static void run_smoke(gfx *f, de_gpu_backend backend) {
    start(f, backend);
    /* Copies of the pass data must live until the GPU is done: one set per frame. */
    static clear_pass clears[4][3];
    static copy_pass copies[4][4];
    static buffer_copy_pass via[4];
    for (u32 frame = 0; frame < 4; ++frame) {
        de_rg_resource staging;
        de__rg_reset(f->g);
        de_arena_reset(f->arena);
        ASSERT_EQ(de__rg_begin(f->g, f->arena), DE_OK);
        build(f, clears[frame], copies[frame], &via[frame], &staging);
        ASSERT_EQ(de__rg_finalize(f->g), DE_OK);
        de_result r = de__rg_compile(f->g);
        if (r != DE_OK)
            fprintf(stderr, "compile: %s\n", de_error_last() ? de_error_last()->message : "?");
        ASSERT_EQ(r, DE_OK);
        const de__rg_plan *p = f->g->plan;
        /* C reuses B's memory (both graphics-only, B done before C starts). */
        const de__rg_transient *tb =
            &p->transients[p->transient_of[de__rg_resource_index(f->g, clears[frame][1].target)]];
        const de__rg_transient *tc =
            &p->transients[p->transient_of[de__rg_resource_index(f->g, clears[frame][2].target)]];
        EXPECT_EQ(tb->offset, tc->offset);
        EXPECT_TRUE(tc->aliased);
        EXPECT_GT(p->signal_count[DE_QUEUE_COPY], 0u);
        /* The staging buffer's first barrier: every buffer access before it on Vulkan, so this
         * frame's copy waits for the last frame's read of the same bytes; NONE elsewhere. */
        const de_rg_pass *stage = f->g->passes[f->g->pass_count - 2u];
        const de__rg_cbarrier *fb = first_barrier(f->g, stage, staging);
        ASSERT_TRUE(fb != NULL);
        EXPECT_EQ(fb->before,
                  backend == DE_GPU_VULKAN ? (u32)DE__RG_BUFFER_ACCESS_BITS : (u32)DE_ACCESS_NONE);
        EXPECT_EQ(fb->after, (u32)DE_ACCESS_COPY_DST);
        run_plan(f); /* no wait between frames: they overlap on the GPU */
    }
    de_gpu_wait_idle(f->dev);
    for (u32 i = 0; i < 4; ++i) {
        const u8 *px = (const u8 *)de_gpu_buffer_map(f->dev, f->rb[i]);
        ASSERT_TRUE(px != NULL);
        const u8 want[4][4] = {
            {255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {0, 0, 255, 255}};
        const u64 last = RB_BYTES - 4u;
        if (memcmp(px, want[i], 4) != 0 || memcmp(px + last, want[i], 4) != 0)
            fprintf(stderr, "readback %u: %u %u %u %u\n", i, px[0], px[1], px[2], px[3]);
        EXPECT_MEMEQ(px, want[i], 4);
        EXPECT_MEMEQ(px + last, want[i], 4);
    }
    const de__rg_stats st = de__rg_compile_stats(f->g);
    EXPECT_EQ(st.compiles, 1u);
    EXPECT_EQ(st.hits, 3u);
}

/* DENG-414: a frame of buffers only, on one queue: upload -> transient staging -> readback.
 * Nothing but the staging buffer's first barrier orders the next frame's write of it after this
 * frame's read (no texture barrier from NONE, no other queue), so synchronization validation on
 * Vulkan reports a write-after-read hazard if that barrier has no source scope. */
typedef struct reuse_frame {
    buffer_copy_pass in, out;
} reuse_frame;

static void run_buffer_reuse(gfx *f, de_gpu_backend backend) {
    start(f, backend);
    de_gpu_buffer up =
        de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = RB_BYTES,
                                                           .usage = DE_USAGE_COPY_SRC,
                                                           .memory = DE_MEM_UPLOAD,
                                                           .name = "upload"});
    ASSERT_TRUE(DE_HANDLE_VALID(up));
    u8 *src = (u8 *)de_gpu_buffer_map(f->dev, up);
    ASSERT_TRUE(src != NULL);
    for (u64 i = 0; i < RB_BYTES; ++i)
        src[i] = (u8)(i * 7u + 3u);
    static reuse_frame frames[8];
    for (u32 frame = 0; frame < 8; ++frame) {
        reuse_frame *rf = &frames[frame];
        de__rg_reset(f->g);
        de_arena_reset(f->arena);
        ASSERT_EQ(de__rg_begin(f->g, f->arena), DE_OK);
        de_rg *g = f->g;
        de_rg_resource u = de_rg_import_buffer(g, up, DE_ACCESS_COPY_SRC);
        de_rg_resource t = de_rg_create_buffer(
            g, &(de_gpu_buffer_desc){.size = RB_BYTES,
                                     .usage = DE_USAGE_COPY_SRC | DE_USAGE_COPY_DST,
                                     .name = "staging"});
        de_rg_resource r = de_rg_import_buffer(g, f->rb[3], DE_ACCESS_NONE);
        rf->in = (buffer_copy_pass){u, t};
        rf->out = (buffer_copy_pass){t, r};
        de_rg_pass *p1 = de_rg_add_pass(g, "fill", DE_QUEUE_GRAPHICS, exec_copy_buffer, &rf->in);
        de_rg_read(p1, u, DE_ACCESS_COPY_SRC);
        de_rg_write(p1, t, DE_ACCESS_COPY_DST);
        de_rg_pass *p2 = de_rg_add_pass(g, "drain", DE_QUEUE_GRAPHICS, exec_copy_buffer, &rf->out);
        de_rg_read(p2, t, DE_ACCESS_COPY_SRC);
        de_rg_write(p2, r, DE_ACCESS_COPY_DST);
        ASSERT_EQ(de__rg_finalize(g), DE_OK);
        ASSERT_EQ(de__rg_compile(g), DE_OK);
        const de__rg_cbarrier *fb = first_barrier(g, p1, t);
        ASSERT_TRUE(fb != NULL);
        EXPECT_EQ(fb->before,
                  backend == DE_GPU_VULKAN ? (u32)DE__RG_BUFFER_ACCESS_BITS : (u32)DE_ACCESS_NONE);
        EXPECT_EQ(g->plan->batch_count, 1u);
        run_plan(f); /* back to back: frame N+1's fill may overlap frame N's drain */
    }
    de_gpu_wait_idle(f->dev);
    const u8 *dst = (const u8 *)de_gpu_buffer_map(f->dev, f->rb[3]);
    ASSERT_TRUE(dst != NULL);
    EXPECT_MEMEQ(dst, src, (size_t)RB_BYTES);
    de__rg_reset(f->g);
    de_gpu_buffer_destroy(f->dev, up);
}

TEST_F(rg_gpu, buffer_reuse_metal, gfx) {
    run_buffer_reuse(f, DE_GPU_METAL);
}
TEST_F(rg_gpu, buffer_reuse_vulkan, gfx) {
    run_buffer_reuse(f, DE_GPU_VULKAN);
}
TEST_F(rg_gpu, buffer_reuse_d3d12, gfx) {
    run_buffer_reuse(f, DE_GPU_D3D12);
}

/* DENG-414: only a buffer's first use on Vulkan names a source scope. */
TEST(rg_gpu, first_use_before) {
    EXPECT_EQ(de__rg_first_use_before(DE_GPU_VULKAN, DE__RG_BUFFER),
              (u32)DE__RG_BUFFER_ACCESS_BITS);
    EXPECT_EQ(de__rg_first_use_before(DE_GPU_VULKAN, DE__RG_TEXTURE), (u32)DE_ACCESS_NONE);
    EXPECT_EQ(de__rg_first_use_before(DE_GPU_D3D12, DE__RG_BUFFER), (u32)DE_ACCESS_NONE);
    EXPECT_EQ(de__rg_first_use_before(DE_GPU_METAL, DE__RG_BUFFER), (u32)DE_ACCESS_NONE);
    EXPECT_EQ(de__rg_first_use_before(DE_GPU_NULL, DE__RG_BUFFER), (u32)DE_ACCESS_NONE);
    /* Buffer bits only: nothing texture-only and no PRESENT. */
    EXPECT_EQ(DE__RG_BUFFER_ACCESS_BITS & (u32)(DE_ACCESS_RENDER_TARGET | DE_ACCESS_DEPTH_READ |
                                                DE_ACCESS_DEPTH_WRITE | DE_ACCESS_PRESENT),
              0u);
}

TEST_F(rg_gpu, smoke_metal, gfx) {
    run_smoke(f, DE_GPU_METAL);
}
TEST_F(rg_gpu, smoke_vulkan, gfx) {
    run_smoke(f, DE_GPU_VULKAN);
}
TEST_F(rg_gpu, smoke_d3d12, gfx) {
    run_smoke(f, DE_GPU_D3D12);
}
