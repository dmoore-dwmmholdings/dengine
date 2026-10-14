/* test_rg_compile.c: render graph compiler (DENG-172): schedule, async compute fences, transient
 * aliasing, barrier plans (golden lists from the null backend's command log), the compiled-graph
 * cache, and no heap allocation on cache hits. Everything runs on a null-backend device. */
#include "de_test.h"

#include <dengine/de_debug.h>
#include <dengine/de_gpu.h>
#include <dengine/de_memory.h>
#include <dengine/de_render.h>

#include "de_alloc_guard.h"
#include "gpu/gpu_backend.h"
#include "render/graph/rg_internal.h"

#include <stdio.h>
#include <string.h>

typedef struct cfx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_arena *arena;
    de_rg *g;
    de_gpu_device *dev;
    de_gpu_texture swapchain[2]; /* two images, as a swapchain hands out */
    de_gpu_texture history;      /* an imported sampled texture */
    de_gpu_fence fence[DE_QUEUE_COUNT_];
    u64 fence_base[DE_QUEUE_COUNT_];
} cfx;

static void cfx_setup(cfx *f) {
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_RENDER);
    ASSERT_EQ(de_arena_create(&(de_arena_desc){.name = "rg_compile_test"}, &f->arena), DE_OK);
    ASSERT_EQ(de_gpu_device_create(&(de_gpu_device_desc){.backend = DE_GPU_NULL}, &f->dev), DE_OK);
    ASSERT_EQ(
        de__rg_create(&(de__rg_desc){.allocator = &f->alloc, .device = f->dev, .name = "t"}, &f->g),
        DE_OK);
    for (int i = 0; i < 2; ++i) {
        f->swapchain[i] = de_gpu_texture_create(
            f->dev, &(de_gpu_texture_desc){.format = DE_FMT_BGRA8_UNORM,
                                           .width = 64,
                                           .height = 64,
                                           .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_COPY_DST,
                                           .name = "swapchain"});
        ASSERT_TRUE(DE_HANDLE_VALID(f->swapchain[i]));
    }
    f->history = de_gpu_texture_create(
        f->dev, &(de_gpu_texture_desc){.format = DE_FMT_RGBA16_FLOAT,
                                       .width = 64,
                                       .height = 64,
                                       .usage = DE_USAGE_SAMPLED | DE_USAGE_STORAGE,
                                       .name = "history"});
    ASSERT_TRUE(DE_HANDLE_VALID(f->history));
    for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q) {
        f->fence[q] = de_gpu_fence_create(f->dev, 0);
        ASSERT_TRUE(DE_HANDLE_VALID(f->fence[q]));
    }
    de_error_clear();
}

static void cfx_teardown(cfx *f) {
    if (f->g) {
        de__rg_reset(f->g);
        de__rg_destroy(f->g);
    }
    if (f->dev) {
        de_gpu_wait_idle(f->dev);
        for (int i = 0; i < 2; ++i)
            if (DE_HANDLE_VALID(f->swapchain[i]))
                de_gpu_texture_destroy(f->dev, f->swapchain[i]);
        if (DE_HANDLE_VALID(f->history))
            de_gpu_texture_destroy(f->dev, f->history);
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

static void frame_begin(cfx *f) {
    de__rg_reset(f->g);
    de_arena_reset(f->arena);
    ASSERT_EQ(de__rg_begin(f->g, f->arena), DE_OK);
}

static void frame_compile(cfx *f) {
    de_result r = de__rg_finalize(f->g);
    if (r != DE_OK)
        fprintf(stderr, "finalize: %s\n", de_error_last() ? de_error_last()->message : "?");
    ASSERT_EQ(r, DE_OK);
    r = de__rg_compile(f->g);
    if (r != DE_OK)
        fprintf(stderr, "compile: %s\n", de_error_last() ? de_error_last()->message : "?");
    ASSERT_EQ(r, DE_OK);
    ASSERT_TRUE(f->g->plan != NULL);
}

static de_rg_resource new_color(de_rg *g, const char *name) {
    const de_gpu_texture_desc d = {.format = DE_FMT_RGBA16_FLOAT,
                                   .width = 64,
                                   .height = 64,
                                   .usage =
                                       DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED | DE_USAGE_STORAGE,
                                   .name = name};
    return de_rg_create_texture(g, &d);
}

/* Records and submits the frame's plan as DENG-173 will: one command list per batch, each step's
 * PRE barriers, its pass, its POST barriers; timeline fences per queue (signal n -> base + n). */
static void run_plan(cfx *f) {
    const de__rg_plan *p = f->g->plan;
    ASSERT_TRUE(p != NULL);
    for (u32 bi = 0; bi < p->batch_count; ++bi) {
        const de__rg_batch *b = &p->batches[bi];
        char name[32];
        snprintf(name, sizeof name, "b%u", bi);
        de_gpu_cmd *cmd = de_gpu_cmd_begin(f->dev, b->queue, name);
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
            waits[nw++] = (de_gpu_wait){f->fence[x->queue], f->fence_base[x->queue] + x->signal};
        }
        de_gpu_submit sub = {.cmds = &cmd, .cmd_count = 1, .waits = waits, .wait_count = nw};
        if (b->signal) {
            sub.signal = f->fence[b->queue];
            sub.signal_value = f->fence_base[b->queue] + b->signal;
        }
        ASSERT_EQ(de_gpu_queue_submit(f->dev, b->queue, &sub), DE_OK);
    }
    for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q)
        f->fence_base[q] += p->signal_count[q];
}

/* The name of the graph resource whose physical handle a barrier names. */
static const char *phys_name(cfx *f, const de_gpu_barrier *b) {
    for (u32 i = 0; i < f->g->resource_count; ++i) {
        const de__rg_resource *r = &f->g->resources[i];
        if (r->kind == DE__RG_TEXTURE && b->texture.id && r->texture.id == b->texture.id)
            return r->name;
        if (r->kind == DE__RG_BUFFER && b->buffer.id && r->buffer.id == b->buffer.id)
            return r->name;
    }
    return "?";
}

/* The null backend's command log as text: one line per barrier call or submit. */
static void log_text(cfx *f, char *out, size_t cap) {
    size_t n = 0;
    out[0] = 0;
    const u32 count = de__gpu_null_log_count(f->dev);
    for (u32 i = 0; i < count; ++i) {
        const de__gpu_null_entry *e = de__gpu_null_log_entry(f->dev, i);
        if (e->op == DE__GPU_NULL_OP_SUBMIT) {
            n += (size_t)snprintf(out + n, cap - n, "submit %s %s", de__rg_queue_name(e->queue),
                                  e->list);
            const de_gpu_wait *w = de__gpu_null_log_waits(f->dev, e);
            for (u32 k = 0; k < e->data_count; ++k)
                for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q)
                    if (w[k].fence.id == f->fence[q].id)
                        n += (size_t)snprintf(out + n, cap - n, " wait %s:%llu",
                                              de__rg_queue_name((de_gpu_queue)q),
                                              (unsigned long long)w[k].value);
            if (e->a[2])
                n +=
                    (size_t)snprintf(out + n, cap - n, " signal %llu", (unsigned long long)e->a[3]);
            n += (size_t)snprintf(out + n, cap - n, "\n");
            continue;
        }
        n += (size_t)snprintf(out + n, cap - n, "%s %s:", de__rg_queue_name(e->queue), e->list);
        const de_gpu_barrier *b = de__gpu_null_log_barriers(f->dev, e);
        for (u32 k = 0; k < e->data_count; ++k) {
            char before[96], after[96];
            n += (size_t)snprintf(out + n, cap - n, "%s %s %s->%s", k ? ";" : "",
                                  phys_name(f, &b[k]),
                                  de__rg_access_str(b[k].before, before, sizeof before),
                                  de__rg_access_str(b[k].after, after, sizeof after));
            if (b[k].mip_first || b[k].mip_count)
                n += (size_t)snprintf(out + n, cap - n, " mips %u+%u", b[k].mip_first,
                                      b[k].mip_count);
            if (b[k].layer_first || b[k].layer_count)
                n += (size_t)snprintf(out + n, cap - n, " layers %u+%u", b[k].layer_first,
                                      b[k].layer_count);
            if (b[k].src_queue != b[k].dst_queue)
                n +=
                    (size_t)snprintf(out + n, cap - n, " %s->%s", de__rg_queue_name(b[k].src_queue),
                                     de__rg_queue_name(b[k].dst_queue));
        }
        n += (size_t)snprintf(out + n, cap - n, "\n");
    }
}

static void expect_log(cfx *f, const char *golden) {
    static char text[8192];
    log_text(f, text, sizeof text);
    if (strcmp(text, golden) != 0)
        fprintf(stderr, "command log:\n%s\nexpected:\n%s\n", text, golden);
    EXPECT_STREQ(text, golden);
}

/* ---------- Schedule ---------- */

/* A 40-pass graph with branches, reconvergence, culled passes and every queue. */
static void build_mixed(cfx *f) {
    de_rg *g = f->g;
    de_rg_resource sc = de_rg_import_texture(g, f->swapchain[0], DE_ACCESS_NONE);
    de_rg_resource t[8];
    for (u32 i = 0; i < 8; ++i) {
        char name[16];
        snprintf(name, sizeof name, "t%u", i);
        t[i] = de_rg_create_texture(
            g, &(de_gpu_texture_desc){.format = DE_FMT_RGBA16_FLOAT,
                                      .width = 32u << (i % 3),
                                      .height = 32,
                                      .usage = DE_USAGE_SAMPLED | DE_USAGE_STORAGE |
                                               DE_USAGE_COPY_SRC | DE_USAGE_COPY_DST,
                                      .name = name});
    }
    for (u32 p = 0; p < 40; ++p) {
        char name[16];
        snprintf(name, sizeof name, "p%u", p);
        const de_gpu_queue q = p % 7 == 3   ? DE_QUEUE_COPY
                               : p % 3 == 1 ? DE_QUEUE_COMPUTE
                                            : DE_QUEUE_GRAPHICS;
        de_rg_pass *ps = de_rg_add_pass(g, name, q, NULL, NULL);
        /* Passes 0-7 write t0-t7 first, so every later read has a producer. */
        const u32 w = p < 8u ? p : (p * 5u + 1u) % 8u, r = (p * 3u) % 8u;
        if (q == DE_QUEUE_COPY) {
            if (p >= 8u)
                de_rg_read(ps, t[r], DE_ACCESS_COPY_SRC);
            if (r != w || p < 8u)
                de_rg_write(ps, t[w], DE_ACCESS_COPY_DST);
        } else {
            if (p >= 8u)
                de_rg_read(ps, t[r], DE_ACCESS_SHADER_READ);
            de_rg_write(ps, t[w], DE_ACCESS_SHADER_WRITE);
        }
        if (p % 11 == 10)
            de_rg_pass_side_effect(ps);
    }
    de_rg_pass *present = de_rg_add_pass(g, "present", DE_QUEUE_GRAPHICS, NULL, NULL);
    de_rg_read(present, t[2], DE_ACCESS_SHADER_READ);
    de_rg_write(present, sc, DE_ACCESS_RENDER_TARGET);
    de_rg_export(g, sc, DE_ACCESS_PRESENT);
}

/* A plain copy of a plan's schedule and barriers, to compare two compilations. */
typedef struct plan_copy {
    u32 step_count, batch_count, wait_count, barrier_count;
    u32 pass[64];
    u32 queue[64];
    de__rg_batch batches[64];
    de__rg_wait waits[128];
    de__rg_cbarrier barriers[512];
} plan_copy;

static void copy_plan(const de__rg_plan *p, plan_copy *c) {
    memset(c, 0, sizeof *c);
    ASSERT_LE(p->step_count, 64u);
    ASSERT_LE(p->batch_count, 64u);
    ASSERT_LE(p->wait_count, 128u);
    ASSERT_LE(p->barrier_count, 512u);
    c->step_count = p->step_count;
    c->batch_count = p->batch_count;
    c->wait_count = p->wait_count;
    c->barrier_count = p->barrier_count;
    for (u32 s = 0; s < p->step_count; ++s) {
        c->pass[s] = p->steps[s].pass;
        c->queue[s] = p->steps[s].queue;
    }
    memcpy(c->batches, p->batches, sizeof *p->batches * p->batch_count);
    if (p->wait_count)
        memcpy(c->waits, p->waits, sizeof *p->waits * p->wait_count);
    memcpy(c->barriers, p->barriers, sizeof *p->barriers * p->barrier_count);
}

TEST_F(rg_compile, stable_schedule, cfx) {
    static plan_copy a, b;
    frame_begin(f);
    build_mixed(f);
    frame_compile(f);
    const de__rg_plan *p = f->g->plan;
    copy_plan(p, &a);
    /* Declaration order of the live passes, between the prologue and the epilogue. */
    EXPECT_EQ(p->steps[0].pass, DE__RG_NONE);
    EXPECT_EQ(p->steps[p->step_count - 1u].pass, DE__RG_NONE);
    u32 prev = 0, live = 0;
    for (u32 s = 1; s + 1u < p->step_count; ++s) {
        EXPECT_TRUE(s == 1 || p->steps[s].pass > prev);
        prev = p->steps[s].pass;
        EXPECT_FALSE(f->g->passes[prev]->culled);
        ++live;
    }
    EXPECT_EQ(live, f->g->pass_count - f->g->culled_count);
    EXPECT_GT(f->g->culled_count, 0u);
    EXPECT_GT(p->signal_count[DE_QUEUE_COMPUTE], 0u);

    /* A second graph (own cache, own pool) compiles the same declarations to the same plan. */
    de_rg *g2 = NULL;
    ASSERT_EQ(de__rg_create(&(de__rg_desc){.allocator = &f->alloc, .device = f->dev}, &g2), DE_OK);
    de_rg *g1 = f->g;
    de__rg_reset(g1);
    de_arena_reset(f->arena);
    f->g = g2;
    ASSERT_EQ(de__rg_begin(g2, f->arena), DE_OK);
    build_mixed(f);
    frame_compile(f);
    copy_plan(f->g->plan, &b);
    de__rg_reset(g2);
    de__rg_destroy(g2);
    f->g = g1;
    EXPECT_EQ(a.step_count, b.step_count);
    EXPECT_EQ(a.batch_count, b.batch_count);
    EXPECT_EQ(a.wait_count, b.wait_count);
    EXPECT_EQ(a.barrier_count, b.barrier_count);
    EXPECT_MEMEQ(&a, &b, sizeof a);
}

/* G1 renders A, C (compute) writes B on its own, G2 reads both: with async compute the compute
 * pass overlaps G1 and G2 waits for it once. */
static void build_async(cfx *f, bool chain, de_rg_pass **out_c) {
    de_rg *g = f->g;
    de_rg_resource sc = de_rg_import_texture(g, f->swapchain[0], DE_ACCESS_NONE);
    de_rg_resource a = de_rg_create_texture(
        g, &(de_gpu_texture_desc){.format = DE_FMT_RGBA16_FLOAT,
                                  .width = 64,
                                  .height = 64,
                                  .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED,
                                  .name = "A"});
    de_rg_resource b = new_color(g, "B");
    de_rg_pass *g1 = de_rg_add_pass(g, "G1", DE_QUEUE_GRAPHICS, NULL, NULL);
    de_rg_write(g1, a, DE_ACCESS_RENDER_TARGET);
    de_rg_pass *c = de_rg_add_pass(g, "C", DE_QUEUE_COMPUTE, NULL, NULL);
    if (chain)
        de_rg_read(c, a, DE_ACCESS_SHADER_READ);
    de_rg_write(c, b, DE_ACCESS_SHADER_WRITE);
    de_rg_pass *g2 = de_rg_add_pass(g, "G2", DE_QUEUE_GRAPHICS, NULL, NULL);
    de_rg_read(g2, a, DE_ACCESS_SHADER_READ);
    de_rg_read(g2, b, DE_ACCESS_SHADER_READ);
    de_rg_write(g2, sc, DE_ACCESS_RENDER_TARGET);
    de_rg_export(g, sc, DE_ACCESS_PRESENT);
    if (out_c)
        *out_c = c;
}

static u32 in_frame_waits(const de__rg_plan *p) {
    u32 n = 0;
    for (u32 i = 0; i < p->wait_count; ++i)
        n += p->waits[i].signal != 0;
    return n;
}

static u32 total_signals(const de__rg_plan *p) {
    return p->signal_count[0] + p->signal_count[1] + p->signal_count[2];
}

TEST_F(rg_compile, async_compute_fences, cfx) {
    ASSERT_TRUE(de_gpu_get_caps(f->dev)->async_compute);
    frame_begin(f);
    de_rg_pass *c = NULL;
    build_async(f, false, &c);
    frame_compile(f);
    const de__rg_plan *p = f->g->plan;
    EXPECT_TRUE(p->async_compute);
    const u32 cs = p->step_of_pass[c->index];
    EXPECT_EQ(p->steps[cs].queue, DE_QUEUE_COMPUTE);
    /* One wait (G2 waits for C) and one signal (after C) in the frame. */
    EXPECT_EQ(in_frame_waits(p), 1u);
    EXPECT_EQ(total_signals(p), 1u);
    EXPECT_EQ(p->signal_count[DE_QUEUE_COMPUTE], 1u);
    /* Plus the frame-entry join of the compute queue (signal 0: graphics' last signal before). */
    EXPECT_EQ(p->wait_count, 2u);
    const de__rg_batch *cb = &p->batches[p->steps[cs].batch];
    EXPECT_EQ(cb->queue, DE_QUEUE_COMPUTE);
    ASSERT_EQ(cb->wait_count, 1u);
    EXPECT_EQ(p->waits[cb->wait_first].queue, DE_QUEUE_GRAPHICS);
    EXPECT_EQ(p->waits[cb->wait_first].signal, 0u);
    EXPECT_EQ(cb->signal, 1u);
    /* G2 starts a graphics batch that waits for compute signal 1. */
    const u32 g2s = cs + 1u;
    const de__rg_batch *gb = &p->batches[p->steps[g2s].batch];
    EXPECT_EQ(gb->queue, DE_QUEUE_GRAPHICS);
    EXPECT_EQ(p->batch_steps[gb->step_first], g2s);
    ASSERT_EQ(gb->wait_count, 1u);
    EXPECT_EQ(p->waits[gb->wait_first].queue, DE_QUEUE_COMPUTE);
    EXPECT_EQ(p->waits[gb->wait_first].signal, 1u);
    EXPECT_EQ(p->batch_count, 3u);
    run_plan(f);

    /* C reads G1's output too: one wait and one signal per queue transition (two each). */
    frame_begin(f);
    build_async(f, true, &c);
    frame_compile(f);
    p = f->g->plan;
    EXPECT_EQ(in_frame_waits(p), 2u);
    EXPECT_EQ(total_signals(p), 2u);
    EXPECT_EQ(p->signal_count[DE_QUEUE_GRAPHICS], 1u);
    EXPECT_EQ(p->signal_count[DE_QUEUE_COMPUTE], 1u);
    EXPECT_EQ(p->wait_count, 2u); /* the compute queue's in-frame wait implies the entry join */
    run_plan(f);
}

TEST_F(rg_compile, async_fallback_graphics, cfx) {
    de__rg_set_async_compute(f->g, false);
    for (int chain = 0; chain < 2; ++chain) {
        frame_begin(f);
        de_rg_pass *c = NULL;
        build_async(f, chain != 0, &c);
        frame_compile(f);
        const de__rg_plan *p = f->g->plan;
        EXPECT_FALSE(p->async_compute);
        /* The compute pass runs on graphics at its place, with no fences at all. */
        EXPECT_EQ(p->steps[p->step_of_pass[c->index]].queue, DE_QUEUE_GRAPHICS);
        EXPECT_EQ(p->step_of_pass[c->index], 2u);
        EXPECT_EQ(p->batch_count, 1u);
        EXPECT_EQ(p->wait_count, 0u);
        EXPECT_EQ(total_signals(p), 0u);
        for (u32 i = 0; i < p->barrier_count; ++i)
            EXPECT_EQ(p->barriers[i].src_queue, p->barriers[i].dst_queue);
        run_plan(f);
    }
    /* Allowing it again changes the key: a new plan with the compute queue. */
    de__rg_set_async_compute(f->g, true);
    frame_begin(f);
    build_async(f, false, NULL);
    frame_compile(f);
    EXPECT_TRUE(f->g->plan->async_compute);
    EXPECT_EQ(de__rg_compile_stats(f->g).compiles, 3u);
}

/* ---------- Aliasing ---------- */

static bool null_placement(cfx *f, de_rg_resource r, de__gpu_heap **heap, u64 *off) {
    const de__rg_resource *res = &f->g->resources[de__rg_resource_index(f->g, r)];
    return de__gpu_null_placement(
        f->dev, res->kind == DE__RG_TEXTURE ? DE__GPU_TEXTURE : DE__GPU_BUFFER,
        res->kind == DE__RG_TEXTURE ? res->texture.id : res->buffer.id, heap, off);
}

TEST_F(rg_alias, disjoint_share, cfx) {
    de_rg *g = f->g;
    frame_begin(f);
    de_rg_resource sc = de_rg_import_texture(g, f->swapchain[0], DE_ACCESS_NONE);
    de_rg_resource a = new_color(g, "A");
    de_rg_resource b = new_color(g, "B");
    de_rg_resource c = new_color(g, "C"); /* lives all frame */
    de_rg_resource e = new_color(g, "E"); /* exported: never aliased */
    de_rg_pass *p0 = de_rg_add_pass(g, "p0", DE_QUEUE_GRAPHICS, NULL, NULL);
    de_rg_write(p0, a, DE_ACCESS_RENDER_TARGET);
    de_rg_write(p0, c, DE_ACCESS_SHADER_WRITE);
    de_rg_pass *p1 = de_rg_add_pass(g, "p1", DE_QUEUE_GRAPHICS, NULL, NULL);
    de_rg_read(p1, a, DE_ACCESS_SHADER_READ);
    de_rg_write(p1, e, DE_ACCESS_SHADER_WRITE);
    de_rg_pass *p2 = de_rg_add_pass(g, "p2", DE_QUEUE_GRAPHICS, NULL, NULL);
    de_rg_read(p2, e, DE_ACCESS_SHADER_READ);
    de_rg_write(p2, b, DE_ACCESS_RENDER_TARGET);
    de_rg_pass *p3 = de_rg_add_pass(g, "p3", DE_QUEUE_GRAPHICS, NULL, NULL);
    de_rg_read(p3, b, DE_ACCESS_SHADER_READ);
    de_rg_read(p3, c, DE_ACCESS_SHADER_READ);
    de_rg_write(p3, sc, DE_ACCESS_RENDER_TARGET);
    de_rg_export(g, sc, DE_ACCESS_PRESENT);
    de_rg_export(g, e, DE_ACCESS_SHADER_READ);
    frame_compile(f);
    const de__rg_plan *p = f->g->plan;
    ASSERT_EQ(p->transient_count, 4u);
    const de__rg_transient *ta = &p->transients[p->transient_of[de__rg_resource_index(g, a)]];
    const de__rg_transient *tb = &p->transients[p->transient_of[de__rg_resource_index(g, b)]];
    const de__rg_transient *tc = &p->transients[p->transient_of[de__rg_resource_index(g, c)]];
    const de__rg_transient *te = &p->transients[p->transient_of[de__rg_resource_index(g, e)]];
    /* A [p0, p1] and B [p2, p3] share memory; C and E overlap both and share nothing. */
    EXPECT_EQ(ta->mem_class, tb->mem_class);
    EXPECT_EQ(ta->offset, tb->offset);
    EXPECT_TRUE(ta->aliased && tb->aliased);
    EXPECT_FALSE(tc->aliased);
    EXPECT_FALSE(te->aliased);
    EXPECT_TRUE(tc->offset >= ta->offset + ta->size || tc->offset + tc->size <= ta->offset);
    EXPECT_EQ(p->class_bytes[ta->mem_class], 3u * ta->size);

    /* The physical placements say the same: two textures, one heap address. */
    de__gpu_heap *ha = NULL, *hb = NULL, *hc = NULL;
    u64 oa = 0, ob = 0, oc = 0;
    ASSERT_TRUE(null_placement(f, a, &ha, &oa));
    ASSERT_TRUE(null_placement(f, b, &hb, &ob));
    ASSERT_TRUE(null_placement(f, c, &hc, &oc));
    EXPECT_NE(g->resources[de__rg_resource_index(g, a)].texture.id,
              g->resources[de__rg_resource_index(g, b)].texture.id);
    EXPECT_EQ(ha, hb);
    EXPECT_EQ(oa, ob);
    EXPECT_NE(oa, oc);

    /* B's first use discards (NONE before), which orders it after A on the queue. */
    const de__rg_step *s2 = &p->steps[p->step_of_pass[p2->index]];
    bool found = false;
    for (u32 i = 0; i < s2->barrier_count[DE__RG_PRE]; ++i) {
        const de__rg_cbarrier *x = &p->barriers[s2->barrier_first[DE__RG_PRE] + i];
        if (x->resource == de__rg_resource_index(g, b)) {
            EXPECT_EQ(x->before, (u32)DE_ACCESS_NONE);
            EXPECT_EQ(x->after, (u32)DE_ACCESS_RENDER_TARGET);
            found = true;
        }
    }
    EXPECT_TRUE(found);
    run_plan(f);
}

/* Random graphs: xorshift64* with a fixed seed. */
static u32 rnd(u64 *s) {
    *s ^= *s >> 12;
    *s ^= *s << 25;
    *s ^= *s >> 27;
    return (u32)((*s * 2685821657736338717ull) >> 32);
}

typedef struct rand_graph {
    u32 nt;
    de_rg_resource t[10];
} rand_graph;

static void build_random(cfx *f, u64 seed, rand_graph *rg) {
    de_rg *g = f->g;
    u64 s = seed;
    rg->nt = 2u + rnd(&s) % 9u;
    bool written[10] = {false};
    for (u32 i = 0; i < rg->nt; ++i) {
        char name[16];
        snprintf(name, sizeof name, "t%u", i);
        if (rnd(&s) % 5u == 0) {
            rg->t[i] =
                de_rg_create_buffer(g, &(de_gpu_buffer_desc){.size = 256u * (1u + rnd(&s) % 64u),
                                                             .usage = DE_USAGE_STORAGE,
                                                             .name = name});
        } else {
            const u32 size = 16u << (rnd(&s) % 5u);
            rg->t[i] = de_rg_create_texture(
                g, &(de_gpu_texture_desc){.format =
                                              rnd(&s) % 2u ? DE_FMT_RGBA8_UNORM : DE_FMT_R32_FLOAT,
                                          .width = size,
                                          .height = size,
                                          .mip_count = 1u + rnd(&s) % 3u,
                                          .usage = DE_USAGE_SAMPLED | DE_USAGE_STORAGE,
                                          .name = name});
        }
        if (rnd(&s) % 10u == 0)
            de_rg_export(g, rg->t[i], DE_ACCESS_SHADER_READ);
    }
    const u32 np = 2u + rnd(&s) % 19u;
    for (u32 p = 0; p < np; ++p) {
        char name[16];
        snprintf(name, sizeof name, "p%u", p);
        de_rg_pass *ps = de_rg_add_pass(
            g, name, rnd(&s) % 3u == 0 ? DE_QUEUE_COMPUTE : DE_QUEUE_GRAPHICS, NULL, NULL);
        if (rnd(&s) % 4u)
            de_rg_pass_side_effect(ps);
        for (u32 k = 0; k < 2; ++k) {
            const u32 r = rnd(&s) % rg->nt;
            if (written[r])
                de_rg_read(ps, rg->t[r], DE_ACCESS_SHADER_READ);
        }
        const u32 w = rnd(&s) % rg->nt;
        de_rg_write(ps, rg->t[w], DE_ACCESS_SHADER_WRITE);
        written[w] = true;
    }
}

/* Steps known complete when each batch starts, re-derived from the plan's waits alone. */
static void known_complete(const de__rg_plan *p, u32 *known) {
    for (u32 bi = 0; bi < p->batch_count; ++bi) {
        const de__rg_batch *b = &p->batches[bi];
        u32 k = 0;
        for (u32 pj = bi; pj-- > 0;)
            if (p->batches[pj].queue == b->queue) {
                k |= known[pj];
                break;
            }
        for (u32 w = 0; w < b->wait_count; ++w) {
            const de__rg_wait *x = &p->waits[b->wait_first + w];
            if (x->signal == 0)
                continue;
            for (u32 bj = 0; bj < bi; ++bj) {
                const de__rg_batch *o = &p->batches[bj];
                if (o->queue != x->queue)
                    continue;
                for (u32 i = 0; i < o->step_count; ++i)
                    k |= 1u << p->batch_steps[o->step_first + i];
                k |= known[bj];
                if (o->signal == x->signal)
                    break;
            }
        }
        known[bi] = k;
    }
}

TEST_F(rg_alias, random_graphs_property, cfx) {
    const u64 seed0 = 0xDE172A11A5ull;
    u32 shared = 0, cross_queue = 0;
    for (u32 iter = 0; iter < 1000; ++iter) {
        const u64 seed = seed0 + iter * 0x9E3779B97F4A7C15ull;
        de__rg_set_async_compute(f->g, iter % 4u != 0);
        frame_begin(f);
        rand_graph rg;
        build_random(f, seed, &rg);
        frame_compile(f);
        const de__rg_plan *p = f->g->plan;
        ASSERT_LE(p->step_count, 32u);
        u32 known[64];
        ASSERT_LE(p->batch_count, 64u);
        known_complete(p, known);
        /* Lifetime per transient: first and last step over every queue. */
        u32 first[10], last[10];
        bool used[10];
        for (u32 i = 0; i < rg.nt; ++i) {
            const u32 ri = de__rg_resource_index(f->g, rg.t[i]);
            const de__rg_resource *r = &f->g->resources[ri];
            used[i] = (r->flags & DE__RG_RES_USED) != 0;
            first[i] = UINT32_MAX;
            last[i] = 0;
            for (u32 a = r->access_first; a != DE__RG_NONE; a = f->g->accesses[a].res_next) {
                const u32 s = p->step_of_pass[f->g->accesses[a].pass];
                if (s == DE__RG_NONE)
                    continue;
                first[i] = s < first[i] ? s : first[i];
                last[i] = s > last[i] ? s : last[i];
            }
            if (r->flags & DE__RG_RES_EXPORTED)
                last[i] = p->step_count - 1u;
        }
        for (u32 i = 0; i < rg.nt; ++i) {
            if (!used[i])
                continue;
            const u32 ri = de__rg_resource_index(f->g, rg.t[i]);
            const de__rg_transient *ti = &p->transients[p->transient_of[ri]];
            ASSERT_LE(ti->offset + ti->size, p->class_bytes[ti->mem_class]);
            for (u32 j = 0; j < rg.nt; ++j) {
                if (j == i || !used[j])
                    continue;
                const u32 rj = de__rg_resource_index(f->g, rg.t[j]);
                const de__rg_transient *tj = &p->transients[p->transient_of[rj]];
                if (ti->mem_class != tj->mem_class || ti->offset >= tj->offset + tj->size ||
                    tj->offset >= ti->offset + ti->size)
                    continue;
                /* Shared bytes: both aliasable textures, disjoint lifetimes, and the earlier one
                 * completes before the later one starts on every queue. */
                const bool ok_kind = !ti->buffer && !tj->buffer &&
                                     !(f->g->resources[ri].flags & DE__RG_RES_EXPORTED) &&
                                     !(f->g->resources[rj].flags & DE__RG_RES_EXPORTED);
                const bool disjoint = last[i] < first[j] || last[j] < first[i];
                bool ordered = true;
                if (disjoint && i < j) {
                    const u32 e = last[i] < first[j] ? i : j, l = e == i ? j : i;
                    const u32 er = de__rg_resource_index(f->g, rg.t[e]);
                    const u32 lr = de__rg_resource_index(f->g, rg.t[l]);
                    const de__rg_resource *R = &f->g->resources[er];
                    const de__rg_resource *L = &f->g->resources[lr];
                    for (u32 a = R->access_first; a != DE__RG_NONE;
                         a = f->g->accesses[a].res_next) {
                        const u32 sa = p->step_of_pass[f->g->accesses[a].pass];
                        if (sa == DE__RG_NONE)
                            continue;
                        for (u32 b2 = L->access_first; b2 != DE__RG_NONE;
                             b2 = f->g->accesses[b2].res_next) {
                            const u32 sb = p->step_of_pass[f->g->accesses[b2].pass];
                            if (sb == DE__RG_NONE)
                                continue;
                            const bool same = p->steps[sa].queue == p->steps[sb].queue;
                            if (same)
                                ordered = ordered && sa < sb;
                            else
                                ordered = ordered && ((known[p->steps[sb].batch] >> sa) & 1u) != 0;
                            cross_queue += same ? 0u : 1u;
                        }
                    }
                }
                if (!ok_kind || !disjoint || !ordered)
                    fprintf(stderr,
                            "random graph seed 0x%llx (iteration %u): t%u [%u, %u] and t%u "
                            "[%u, %u] share memory (kind ok %d, disjoint %d, ordered %d)\n",
                            (unsigned long long)seed, iter, i, first[i], last[i], j, first[j],
                            last[j], ok_kind, disjoint, ordered);
                ASSERT_TRUE(ok_kind && disjoint && ordered);
                shared += i < j;
            }
        }
        run_plan(f);
    }
    /* The generator exercises aliasing, also across queues. */
    EXPECT_GT(shared, 100u);
    EXPECT_GT(cross_queue, 10u);
}

/* ---------- Barrier plans (golden lists from the null command log) ---------- */

TEST_F(rg_barriers, golden_rt_sample_storage, cfx) {
    de_rg *g = f->g;
    frame_begin(f);
    de_rg_resource sc = de_rg_import_texture(g, f->swapchain[0], DE_ACCESS_NONE);
    de_rg_set_name(g, sc, "swapchain");
    de_rg_resource color = new_color(g, "color");
    de_rg_resource blurred = new_color(g, "blurred");
    de_rg_pass *draw = de_rg_add_pass(g, "draw", DE_QUEUE_GRAPHICS, NULL, NULL);
    de_rg_write(draw, color, DE_ACCESS_RENDER_TARGET);
    de_rg_pass *sample = de_rg_add_pass(g, "sample", DE_QUEUE_GRAPHICS, NULL, NULL);
    de_rg_read(sample, color, DE_ACCESS_SHADER_READ);
    de_rg_write(sample, blurred, DE_ACCESS_SHADER_WRITE);
    de_rg_pass *post = de_rg_add_pass(g, "post", DE_QUEUE_GRAPHICS, NULL, NULL);
    de_rg_write(post, color, DE_ACCESS_SHADER_WRITE);
    de_rg_pass *comp = de_rg_add_pass(g, "composite", DE_QUEUE_GRAPHICS, NULL, NULL);
    de_rg_read(comp, color, DE_ACCESS_SHADER_READ);
    de_rg_read(comp, blurred, DE_ACCESS_SHADER_READ);
    de_rg_write(comp, sc, DE_ACCESS_RENDER_TARGET);
    de_rg_export(g, sc, DE_ACCESS_PRESENT);
    frame_compile(f);
    de__gpu_null_log_enable(f->dev, true);
    run_plan(f);
    de__gpu_null_log_enable(f->dev, false);
    expect_log(f, "graphics b0: color NONE->RENDER_TARGET\n"
                  "graphics b0: color RENDER_TARGET->SHADER_READ; blurred NONE->SHADER_WRITE\n"
                  "graphics b0: color SHADER_READ->SHADER_WRITE\n"
                  "graphics b0: swapchain NONE->RENDER_TARGET; color SHADER_WRITE->SHADER_READ;"
                  " blurred SHADER_WRITE->SHADER_READ\n"
                  "graphics b0: swapchain RENDER_TARGET->PRESENT\n"
                  "submit graphics b0\n");
    EXPECT_EQ(de__rg_end_access(g, sc), (u32)DE_ACCESS_PRESENT);
    EXPECT_EQ(de__rg_end_access(g, color), (u32)DE_ACCESS_NONE);

    /* A second frame on the other swapchain image is a cache hit with the new image bound. */
    frame_begin(f);
    sc = de_rg_import_texture(g, f->swapchain[1], DE_ACCESS_NONE);
    de_rg_set_name(g, sc, "swapchain1");
    color = new_color(g, "color");
    blurred = new_color(g, "blurred");
    draw = de_rg_add_pass(g, "draw", DE_QUEUE_GRAPHICS, NULL, NULL);
    de_rg_write(draw, color, DE_ACCESS_RENDER_TARGET);
    sample = de_rg_add_pass(g, "sample", DE_QUEUE_GRAPHICS, NULL, NULL);
    de_rg_read(sample, color, DE_ACCESS_SHADER_READ);
    de_rg_write(sample, blurred, DE_ACCESS_SHADER_WRITE);
    post = de_rg_add_pass(g, "post", DE_QUEUE_GRAPHICS, NULL, NULL);
    de_rg_write(post, color, DE_ACCESS_SHADER_WRITE);
    comp = de_rg_add_pass(g, "composite", DE_QUEUE_GRAPHICS, NULL, NULL);
    de_rg_read(comp, color, DE_ACCESS_SHADER_READ);
    de_rg_read(comp, blurred, DE_ACCESS_SHADER_READ);
    de_rg_write(comp, sc, DE_ACCESS_RENDER_TARGET);
    de_rg_export(g, sc, DE_ACCESS_PRESENT);
    frame_compile(f);
    EXPECT_EQ(de__rg_compile_stats(g).hits, 1u);
    de__gpu_null_log_enable(f->dev, true);
    run_plan(f);
    de__gpu_null_log_enable(f->dev, false);
    expect_log(f, "graphics b0: color NONE->RENDER_TARGET\n"
                  "graphics b0: color RENDER_TARGET->SHADER_READ; blurred NONE->SHADER_WRITE\n"
                  "graphics b0: color SHADER_READ->SHADER_WRITE\n"
                  "graphics b0: swapchain1 NONE->RENDER_TARGET; color SHADER_WRITE->SHADER_READ;"
                  " blurred SHADER_WRITE->SHADER_READ\n"
                  "graphics b0: swapchain1 RENDER_TARGET->PRESENT\n"
                  "submit graphics b0\n");
}

TEST_F(rg_barriers, mip_chain, cfx) {
    de_rg *g = f->g;
    frame_begin(f);
    de_rg_resource hzb =
        de_rg_create_texture(g, &(de_gpu_texture_desc){.format = DE_FMT_R32_FLOAT,
                                                       .width = 64,
                                                       .height = 64,
                                                       .mip_count = 4,
                                                       .usage = DE_USAGE_STORAGE | DE_USAGE_SAMPLED,
                                                       .name = "hzb"});
    de_rg_pass *h0 = de_rg_add_pass(g, "hzb0", DE_QUEUE_GRAPHICS, NULL, NULL);
    de_rg_write_sub(h0, hzb, (de_rg_subresource){.mip_first = 0, .mip_count = 1},
                    DE_ACCESS_SHADER_WRITE);
    for (u32 m = 1; m < 4; ++m) {
        char name[8];
        snprintf(name, sizeof name, "hzb%u", m);
        de_rg_pass *h = de_rg_add_pass(g, name, DE_QUEUE_GRAPHICS, NULL, NULL);
        de_rg_read_sub(h, hzb, (de_rg_subresource){.mip_first = m - 1u, .mip_count = 1},
                       DE_ACCESS_SHADER_READ);
        de_rg_write_sub(h, hzb, (de_rg_subresource){.mip_first = m, .mip_count = 1},
                        DE_ACCESS_SHADER_WRITE);
    }
    /* The history import is read mip by mip, then written whole: open-ended ranges. */
    de_rg_resource hist = de_rg_import_texture(g, f->history, DE_ACCESS_SHADER_READ);
    de_rg_set_name(g, hist, "history");
    de_rg_pass *use = de_rg_add_pass(g, "cull", DE_QUEUE_GRAPHICS, NULL, NULL);
    de_rg_read(use, hzb, DE_ACCESS_SHADER_READ);
    de_rg_read_sub(use, hist, (de_rg_subresource){.mip_first = 0, .mip_count = 1},
                   DE_ACCESS_SHADER_READ);
    de_rg_write_sub(use, hist, (de_rg_subresource){.mip_first = 1}, DE_ACCESS_SHADER_WRITE);
    frame_compile(f);
    de__gpu_null_log_enable(f->dev, true);
    run_plan(f);
    de__gpu_null_log_enable(f->dev, false);
    /* Each level depends only on the one before; "+0" counts run to the last mip. The import
     * ends in one state (the last pass's access) for the next frame's import. */
    expect_log(f,
               "graphics b0: hzb NONE->SHADER_WRITE mips 0+1\n"
               "graphics b0: hzb SHADER_WRITE->SHADER_READ mips 0+1; hzb NONE->SHADER_WRITE mips "
               "1+1\n"
               "graphics b0: hzb SHADER_WRITE->SHADER_READ mips 1+1; hzb NONE->SHADER_WRITE mips "
               "2+1\n"
               "graphics b0: hzb SHADER_WRITE->SHADER_READ mips 2+1; hzb NONE->SHADER_WRITE mips "
               "3+0\n"
               "graphics b0: hzb SHADER_WRITE->SHADER_READ mips 3+0;"
               " history SHADER_READ->SHADER_WRITE mips 1+0\n"
               "graphics b0: history SHADER_READ->SHADER_READ|SHADER_WRITE mips 0+1;"
               " history SHADER_WRITE->SHADER_READ|SHADER_WRITE mips 1+0\n"
               "submit graphics b0\n");
    EXPECT_EQ(de__rg_end_access(g, hist), (u32)(DE_ACCESS_SHADER_READ | DE_ACCESS_SHADER_WRITE));
}

TEST_F(rg_barriers, queue_transfer, cfx) {
    de_rg *g = f->g;
    frame_begin(f);
    de_rg_resource hist = de_rg_import_texture(g, f->history, DE_ACCESS_SHADER_READ);
    de_rg_set_name(g, hist, "history");
    de_rg_resource args = de_rg_create_buffer(
        g, &(de_gpu_buffer_desc){
               .size = 4096, .usage = DE_USAGE_STORAGE | DE_USAGE_INDIRECT, .name = "args"});
    de_rg_resource sc = de_rg_import_texture(g, f->swapchain[0], DE_ACCESS_NONE);
    de_rg_set_name(g, sc, "swapchain");
    de_rg_pass *cull = de_rg_add_pass(g, "cull", DE_QUEUE_COMPUTE, NULL, NULL);
    de_rg_read(cull, hist, DE_ACCESS_SHADER_READ);
    de_rg_write(cull, args, DE_ACCESS_SHADER_WRITE);
    de_rg_pass *draw = de_rg_add_pass(g, "draw", DE_QUEUE_GRAPHICS, NULL, NULL);
    de_rg_read(draw, args, DE_ACCESS_INDIRECT);
    de_rg_write(draw, sc, DE_ACCESS_RENDER_TARGET);
    de_rg_export(g, sc, DE_ACCESS_PRESENT);
    frame_compile(f);
    de__gpu_null_log_enable(f->dev, true);
    run_plan(f);
    de__gpu_null_log_enable(f->dev, false);
    /* The import starts and ends the frame owned by graphics: the prologue releases it to
     * compute, cull acquires it and releases it back with its output, draw and the epilogue
     * acquire them. Each transfer is the same barrier on both queues. */
    expect_log(f, "graphics b0: history SHADER_READ->SHADER_READ graphics->compute\n"
                  "submit graphics b0 signal 1\n"
                  "compute b1: history SHADER_READ->SHADER_READ graphics->compute;"
                  " args NONE->SHADER_WRITE\n"
                  "compute b1: history SHADER_READ->SHADER_READ compute->graphics;"
                  " args SHADER_WRITE->INDIRECT compute->graphics\n"
                  "submit compute b1 wait graphics:1 signal 1\n"
                  "graphics b2: args SHADER_WRITE->INDIRECT compute->graphics;"
                  " swapchain NONE->RENDER_TARGET\n"
                  "graphics b2: history SHADER_READ->SHADER_READ compute->graphics;"
                  " swapchain RENDER_TARGET->PRESENT\n"
                  "submit graphics b2 wait compute:1\n");
    EXPECT_EQ(de__rg_end_access(g, hist), (u32)DE_ACCESS_SHADER_READ);

    /* A copy pass gets only copy states on its own queue. */
    frame_begin(f);
    hist = de_rg_import_texture(g, f->history, DE_ACCESS_SHADER_READ);
    de_rg_set_name(g, hist, "history");
    de_rg_resource dst = de_rg_create_buffer(
        g, &(de_gpu_buffer_desc){.size = 65536, .usage = DE_USAGE_COPY_DST, .name = "dst"});
    de_rg_pass *cp = de_rg_add_pass(g, "readback", DE_QUEUE_COPY, NULL, NULL);
    de_rg_read(cp, hist, DE_ACCESS_COPY_SRC);
    de_rg_write(cp, dst, DE_ACCESS_COPY_DST);
    de_rg_export(g, dst, DE_ACCESS_NONE);
    frame_compile(f);
    const de__rg_plan *p = f->g->plan;
    for (u32 s = 0; s < p->step_count; ++s) {
        if (p->steps[s].queue != DE_QUEUE_COPY)
            continue;
        for (u32 slot = 0; slot < 2u; ++slot)
            for (u32 i = 0; i < p->steps[s].barrier_count[slot]; ++i) {
                const de__rg_cbarrier *x = &p->barriers[p->steps[s].barrier_first[slot] + i];
                const u32 copy = DE_ACCESS_COPY_SRC | DE_ACCESS_COPY_DST;
                /* Same-queue barriers on copy: copy bits only. Transfers: the copy half's own
                 * side is a copy state. */
                if (x->src_queue == x->dst_queue) {
                    EXPECT_EQ(x->before & ~copy, 0u);
                    EXPECT_EQ(x->after & ~copy, 0u);
                } else if (x->dst_queue == DE_QUEUE_COPY) {
                    EXPECT_EQ(x->after & ~copy, 0u);
                } else {
                    EXPECT_EQ(x->before & ~copy, 0u);
                }
            }
    }
    EXPECT_EQ(p->signal_count[DE_QUEUE_COPY], 1u); /* the epilogue joins the copy queue */
    run_plan(f);
}

/* ---------- Cache ---------- */

static void build_simple(cfx *f, u32 image, u32 read_bits) {
    de_rg *g = f->g;
    de_rg_resource sc = de_rg_import_texture(g, f->swapchain[image], DE_ACCESS_NONE);
    de_rg_resource color = new_color(g, "color");
    de_rg_pass *draw = de_rg_add_pass(g, "draw", DE_QUEUE_GRAPHICS, NULL, NULL);
    de_rg_write(draw, color, DE_ACCESS_RENDER_TARGET);
    de_rg_pass *comp = de_rg_add_pass(g, "composite", DE_QUEUE_GRAPHICS, NULL, NULL);
    de_rg_read(comp, color, read_bits);
    de_rg_write(comp, sc, DE_ACCESS_RENDER_TARGET);
    de_rg_export(g, sc, DE_ACCESS_PRESENT);
}

TEST_F(rg_cache, hit_no_recompile, cfx) {
    de_gpu_texture placed = {0};
    for (u32 i = 0; i < 6; ++i) {
        frame_begin(f);
        build_simple(f, i % 2u, DE_ACCESS_SHADER_READ);
        frame_compile(f);
        /* The same placed transient every frame; this frame's swapchain image in the table. */
        const de_gpu_texture t = f->g->resources[1].texture;
        if (i == 0)
            placed = t;
        EXPECT_EQ(t.id, placed.id);
        const de__rg_step *last = &f->g->plan->steps[f->g->plan->step_count - 1u];
        ASSERT_EQ(last->barrier_count[DE__RG_PRE], 1u);
        EXPECT_EQ(f->g->phys[last->barrier_first[DE__RG_PRE]].texture.id, f->swapchain[i % 2u].id);
        run_plan(f);
    }
    de__rg_stats st = de__rg_compile_stats(f->g);
    EXPECT_EQ(st.compiles, 1u);
    EXPECT_EQ(st.hits, 5u);
    EXPECT_EQ(st.cached, 1u);
}

TEST_F(rg_cache, invalidate_on_change, cfx) {
    frame_begin(f);
    build_simple(f, 0, DE_ACCESS_SHADER_READ);
    frame_compile(f);
    const de__rg_plan *first = f->g->plan;
    /* One more access bit on one read: a different key, so a compile. */
    frame_begin(f);
    build_simple(f, 0, DE_ACCESS_SHADER_READ | DE_ACCESS_COPY_SRC);
    frame_compile(f);
    EXPECT_NE(f->g->plan, first);
    EXPECT_EQ(de__rg_compile_stats(f->g).compiles, 2u);
    /* Back to the first declarations: its plan is still cached. */
    frame_begin(f);
    build_simple(f, 1, DE_ACCESS_SHADER_READ);
    frame_compile(f);
    EXPECT_EQ(f->g->plan, first);
    de__rg_stats st = de__rg_compile_stats(f->g);
    EXPECT_EQ(st.compiles, 2u);
    EXPECT_EQ(st.hits, 1u);
    /* A renamed pass is a different pass set. */
    frame_begin(f);
    build_simple(f, 0, DE_ACCESS_SHADER_READ);
    f->g->passes[1]->name = "composite2";
    frame_compile(f);
    EXPECT_EQ(de__rg_compile_stats(f->g).compiles, 3u);
}

TEST_F(rg_cache, lru_evict, cfx) {
    de_cvar cv = de_cvar_find("r.rg_cache_size");
    if (!DE_HANDLE_VALID(cv)) { /* registered on the first compile */
        frame_begin(f);
        build_simple(f, 0, DE_ACCESS_SHADER_READ);
        frame_compile(f);
        cv = de_cvar_find("r.rg_cache_size");
    }
    ASSERT_TRUE(DE_HANDLE_VALID(cv));
    EXPECT_EQ(de_cvar_int(cv), 8);
    ASSERT_EQ(de_cvar_set(cv, "2"), DE_OK);
    const u32 bits[3] = {DE_ACCESS_SHADER_READ, DE_ACCESS_SHADER_READ | DE_ACCESS_COPY_SRC,
                         DE_ACCESS_COPY_SRC};
    const de__rg_stats s0 = de__rg_compile_stats(f->g);
    for (u32 k = 0; k < 3; ++k) { /* A, B, C: C evicts A */
        frame_begin(f);
        build_simple(f, 0, bits[k]);
        frame_compile(f);
    }
    de__rg_stats st = de__rg_compile_stats(f->g);
    EXPECT_EQ(st.cached, 2u);
    EXPECT_GE(st.evictions, s0.evictions + 1u);
    /* B is recent: a hit. A was evicted: compiled again (and evicts C, the LRU now). */
    frame_begin(f);
    build_simple(f, 0, bits[1]);
    frame_compile(f);
    EXPECT_EQ(de__rg_compile_stats(f->g).hits, st.hits + 1u);
    frame_begin(f);
    build_simple(f, 0, bits[0]);
    frame_compile(f);
    EXPECT_EQ(de__rg_compile_stats(f->g).compiles, st.compiles + 1u);
    frame_begin(f);
    build_simple(f, 0, bits[1]);
    frame_compile(f);
    EXPECT_EQ(de__rg_compile_stats(f->g).hits, st.hits + 2u);
    frame_begin(f);
    build_simple(f, 0, bits[2]);
    frame_compile(f);
    EXPECT_EQ(de__rg_compile_stats(f->g).compiles, st.compiles + 2u);
    EXPECT_EQ(de__rg_compile_stats(f->g).cached, 2u);
    ASSERT_EQ(de_cvar_set(cv, "8"), DE_OK);
}

TEST_F(rg_cache, no_heap_on_hit, cfx) {
    /* Warm up: the pool, the plan, the arena blocks and the builder's capacity hints. */
    for (u32 i = 0; i < 3; ++i) {
        frame_begin(f);
        build_mixed(f);
        frame_compile(f);
        run_plan(f);
    }
    const de_tracking_stats before = de_tracking_allocator_stats(&f->track);
    de_alloc_guard guard = {0};
    de_alloc_guard_begin(&guard);
    de_result r = DE_OK;
    for (u32 i = 0; i < 20 && r == DE_OK; ++i) {
        de__rg_reset(f->g);
        de_arena_reset(f->arena);
        r = de__rg_begin(f->g, f->arena);
        if (r == DE_OK) {
            build_mixed(f);
            r = de__rg_finalize(f->g);
        }
        if (r == DE_OK)
            r = de__rg_compile(f->g);
    }
    de_alloc_guard_end(&guard);
    ASSERT_EQ(r, DE_OK);
    DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
    const de_tracking_stats after = de_tracking_allocator_stats(&f->track);
    EXPECT_EQ(after.alloc_count, before.alloc_count);
    EXPECT_EQ(de__rg_compile_stats(f->g).compiles, 1u);
    EXPECT_EQ(de__rg_compile_stats(f->g).hits, 22u);
}
