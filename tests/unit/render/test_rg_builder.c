/* test_rg_builder.c: render graph builder (DENG-171): declarations, versions and dependency
 * edges, culling, validation, no heap after warm-up, the Graphviz dump. Imported resources come
 * from a null-backend device; the builder itself never touches the GPU. */
#include "de_test.h"

#include <dengine/de_gpu.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_render.h>

#include "de_alloc_guard.h"
#include "render/graph/rg_internal.h"

#include <stdio.h>
#include <string.h>

#ifndef DE_RG_TEST_OUT_DIR
#error "DE_RG_TEST_OUT_DIR must be defined by tests/unit/render/CMakeLists.txt"
#endif

typedef struct rg_fx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_arena *arena;
    de_rg *g;
    de_gpu_device *dev;
    de_gpu_texture swapchain; /* stands in for a swapchain image */
} rg_fx;

static void rg_fx_setup(rg_fx *f) {
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_RENDER);
    ASSERT_EQ(de_arena_create(&(de_arena_desc){.name = "rg_test"}, &f->arena), DE_OK);
    ASSERT_EQ(de__rg_create(&(de__rg_desc){.allocator = &f->alloc, .name = "test"}, &f->g), DE_OK);
    ASSERT_EQ(de_gpu_device_create(&(de_gpu_device_desc){.backend = DE_GPU_NULL}, &f->dev), DE_OK);
    f->swapchain = de_gpu_texture_create(
        f->dev, &(de_gpu_texture_desc){.format = DE_FMT_BGRA8_UNORM,
                                       .width = 64,
                                       .height = 64,
                                       .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_COPY_DST,
                                       .name = "swapchain"});
    ASSERT_TRUE(DE_HANDLE_VALID(f->swapchain));
    ASSERT_EQ(de__rg_begin(f->g, f->arena), DE_OK);
    de_error_clear();
}

static void rg_fx_teardown(rg_fx *f) {
    if (f->g)
        de__rg_reset(f->g);
    de__rg_destroy(f->g);
    if (f->dev) {
        if (DE_HANDLE_VALID(f->swapchain))
            de_gpu_texture_destroy(f->dev, f->swapchain);
        de_gpu_device_destroy(f->dev);
    }
    if (f->arena)
        de_arena_destroy(f->arena);
    if (f->alloc.alloc)
        EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0);
}

static de_gpu_texture_desc color_desc(const char *name) {
    return (de_gpu_texture_desc){.format = DE_FMT_RGBA16_FLOAT,
                                 .width = 64,
                                 .height = 64,
                                 .usage =
                                     DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED | DE_USAGE_STORAGE,
                                 .name = name};
}

static u32 res_index(rg_fx *f, de_rg_resource r) {
    return de__rg_resource_index(f->g, r);
}

/* Index of the access of pass `p` on `r` with the given direction, or DE__RG_NONE. */
static u32 find_access(rg_fx *f, const de_rg_pass *p, de_rg_resource r, bool write) {
    u32 ri = res_index(f, r);
    for (u32 a = p->access_first; a != DE__RG_NONE; a = f->g->accesses[a].pass_next)
        if (f->g->accesses[a].resource == ri && f->g->accesses[a].write == write)
            return a;
    return DE__RG_NONE;
}

static bool has_edge(rg_fx *f, const de_rg_pass *to, u32 from, de__rg_edge_kind kind) {
    for (u32 e = to->edge_first; e < to->edge_first + to->edge_count; ++e)
        if (f->g->edges[e].from == from && f->g->edges[e].kind == kind)
            return true;
    return false;
}

/* Expects the calling thread's error to mention every needle. */
#define EXPECT_ERROR_HAS(...)                                                                      \
    do {                                                                                           \
        const de_error *e_ = de_error_last();                                                      \
        ASSERT_TRUE(e_ != NULL);                                                                   \
        const char *needles_[] = {__VA_ARGS__};                                                    \
        for (size_t i_ = 0; i_ < sizeof needles_ / sizeof needles_[0]; ++i_) {                     \
            if (!strstr(e_->message, needles_[i_]))                                                \
                fprintf(stderr, "missing \"%s\" in: %s\n", needles_[i_], e_->message);             \
            EXPECT_TRUE(strstr(e_->message, needles_[i_]) != NULL);                                \
        }                                                                                          \
    } while (0)

/* ---------- Execute callbacks ---------- */

typedef struct exec_log {
    u32 calls;
    u32 order[16];
    const de_rg_pass *passes[16];
    de_rg_resource tex_res;
    de_gpu_texture tex_seen;
    de_rg_resource buf_res;
    de_gpu_buffer buf_seen;
} exec_log;

static void record_exec(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    (void)cmd;
    exec_log *l = (exec_log *)user;
    if (l->calls < 16)
        l->passes[l->calls] = pass;
    l->order[l->calls < 16 ? l->calls : 15] = pass->index;
    ++l->calls;
    if (DE_HANDLE_VALID(l->tex_res))
        l->tex_seen = de_rg_texture(pass, l->tex_res);
    if (DE_HANDLE_VALID(l->buf_res))
        l->buf_seen = de_rg_buffer(pass, l->buf_res);
}

static void run_all(rg_fx *f) {
    for (u32 i = 0; i < f->g->pass_count; ++i)
        de__rg_execute_pass(f->g, i, NULL);
}

/* ---------- Declarations ---------- */

TEST_F(rg_builder, add_pass_resources, rg_fx) {
    de_rg *g = f->g;
    de_rg_resource hdr =
        de_rg_create_texture(g, &(de_gpu_texture_desc){.dim = DE_TEX_2D_ARRAY,
                                                       .format = DE_FMT_RGBA16_FLOAT,
                                                       .width = 128,
                                                       .height = 128,
                                                       .depth_or_layers = 4,
                                                       .mip_count = 3,
                                                       .usage = DE_USAGE_SAMPLED | DE_USAGE_STORAGE,
                                                       .name = "hdr"});
    de_rg_resource cube = de_rg_create_texture(
        g, &(de_gpu_texture_desc){.dim = DE_TEX_CUBE,
                                  .format = DE_FMT_RGBA8_UNORM,
                                  .width = 32,
                                  .height = 32,
                                  .usage = DE_USAGE_SAMPLED | DE_USAGE_RENDER_TARGET});
    de_rg_resource buf = de_rg_create_buffer(
        g, &(de_gpu_buffer_desc){.size = 4096, .usage = DE_USAGE_STORAGE, .name = "counts"});
    de_rg_resource sc = de_rg_import_texture(g, f->swapchain, DE_ACCESS_PRESENT);
    ASSERT_TRUE(DE_HANDLE_VALID(hdr));
    ASSERT_TRUE(DE_HANDLE_VALID(cube));
    ASSERT_TRUE(DE_HANDLE_VALID(buf));
    ASSERT_TRUE(DE_HANDLE_VALID(sc));
    ASSERT_EQ(g->resource_count, 4u);

    /* Importing the same texture again gives the same resource. */
    de_rg_resource sc2 = de_rg_import_texture(g, f->swapchain, DE_ACCESS_NONE);
    EXPECT_EQ(sc2.id, sc.id);
    EXPECT_EQ(g->resource_count, 4u);

    const de__rg_resource *rh = &g->resources[res_index(f, hdr)];
    EXPECT_EQ(rh->kind, DE__RG_TEXTURE);
    EXPECT_STREQ(rh->name, "hdr");
    EXPECT_TRUE(rh->tex.transient);
    EXPECT_EQ(rh->mip_count, 3u);
    EXPECT_EQ(rh->layer_count, 4u);
    const de__rg_resource *rc = &g->resources[res_index(f, cube)];
    EXPECT_EQ(rc->layer_count, 6u);
    EXPECT_EQ(rc->mip_count, 1u);
    EXPECT_STREQ(rc->name, "texture#1");
    const de__rg_resource *rs = &g->resources[res_index(f, sc)];
    EXPECT_TRUE((rs->flags & DE__RG_RES_IMPORTED) != 0);
    EXPECT_EQ(rs->initial_access, (u32)DE_ACCESS_PRESENT);
    EXPECT_EQ(rs->texture.id, f->swapchain.id);
    de_rg_set_name(g, sc, "backbuffer");
    EXPECT_STREQ(g->resources[res_index(f, sc)].name, "backbuffer");

    exec_log log = {.tex_res = sc, .buf_res = buf};
    de_rg_pass *p0 = de_rg_add_pass(g, "fill", DE_QUEUE_COMPUTE, record_exec, &log);
    ASSERT_TRUE(p0 != NULL);
    de_rg_write(p0, buf, DE_ACCESS_SHADER_WRITE);
    de_rg_write(p0, hdr, DE_ACCESS_SHADER_WRITE);
    de_rg_pass *p1 = de_rg_add_pass(g, "blit", DE_QUEUE_GRAPHICS, record_exec, &log);
    de_rg_read(p1, hdr, DE_ACCESS_SHADER_READ);
    de_rg_read(p1, buf, DE_ACCESS_SHADER_READ);
    de_rg_read(p1, hdr, DE_ACCESS_COPY_SRC); /* merges with the first read */
    de_rg_write(p1, sc, DE_ACCESS_RENDER_TARGET);
    EXPECT_EQ(p0->access_count, 2u);
    EXPECT_EQ(p1->access_count, 3u);
    u32 a = find_access(f, p1, hdr, false);
    ASSERT_NE(a, DE__RG_NONE);
    EXPECT_EQ(g->accesses[a].access, (u32)(DE_ACCESS_SHADER_READ | DE_ACCESS_COPY_SRC));
    EXPECT_STREQ(p1->name, "blit");
    EXPECT_EQ(p1->queue, DE_QUEUE_GRAPHICS);
    EXPECT_EQ(p1->index, 1u);
    EXPECT_EQ(g->pass_count, 2u);

    ASSERT_EQ(de__rg_finalize(g), DE_OK);
    EXPECT_EQ(g->culled_count, 0u);
    /* Declarations after finalize are refused. */
    de_error_clear();
    EXPECT_FALSE(DE_HANDLE_VALID(
        de_rg_create_buffer(g, &(de_gpu_buffer_desc){.size = 16, .usage = DE_USAGE_STORAGE})));
    EXPECT_ERROR_HAS("not open for declarations", "finalized");

    /* Inside execute, the pass sees the physical resources it declared. */
    de_error_clear();
    run_all(f);
    EXPECT_EQ(log.calls, 2u);
    EXPECT_EQ(log.order[0], 0u);
    EXPECT_EQ(log.order[1], 1u);
    EXPECT_EQ(log.tex_seen.id, f->swapchain.id); /* from p1, the last call */
    EXPECT_EQ(log.buf_seen.id, 0u);              /* transients are bound by DENG-172 */

    /* A new frame: last frame's handles are stale. */
    de__rg_reset(g);
    ASSERT_EQ(de__rg_begin(g, f->arena), DE_OK);
    EXPECT_EQ(g->pass_count, 0u);
    EXPECT_EQ(de__rg_resource_index(g, hdr), DE__RG_NONE);
    de_error_clear();
    de_rg_pass *q = de_rg_add_pass(g, "stale", DE_QUEUE_GRAPHICS, NULL, NULL);
    de_rg_read(q, hdr, DE_ACCESS_SHADER_READ);
    EXPECT_ERROR_HAS("stale", "de_rg_read");
    EXPECT_EQ(q->access_count, 0u);
    /* A bad queue is refused in every build. */
    EXPECT_TRUE(de_rg_add_pass(g, "bad", (de_gpu_queue)7, NULL, NULL) == NULL);
    EXPECT_EQ(de__rg_finalize(g), DE_ERR_INVALID_ARGUMENT);
}

/* ---------- Versions and edges ---------- */

TEST_F(rg_builder, versions_edges, rg_fx) {
    de_rg *g = f->g;
    /* write -> read -> write -> read on one transient. */
    de_gpu_texture_desc d = color_desc("t");
    de_rg_resource t = de_rg_create_texture(g, &d);
    de_rg_pass *p[4];
    p[0] = de_rg_add_pass(g, "w1", DE_QUEUE_GRAPHICS, NULL, NULL);
    de_rg_write(p[0], t, DE_ACCESS_RENDER_TARGET);
    p[1] = de_rg_add_pass(g, "r1", DE_QUEUE_COMPUTE, NULL, NULL);
    de_rg_read(p[1], t, DE_ACCESS_SHADER_READ);
    p[2] = de_rg_add_pass(g, "w2", DE_QUEUE_COMPUTE, NULL, NULL);
    de_rg_write(p[2], t, DE_ACCESS_SHADER_WRITE);
    p[3] = de_rg_add_pass(g, "r2", DE_QUEUE_GRAPHICS, NULL, NULL);
    de_rg_read(p[3], t, DE_ACCESS_SHADER_READ);
    for (int i = 0; i < 4; ++i)
        de_rg_pass_side_effect(p[i]);

    /* HZB-style chain on one texture: pass i reads mip i-1 and writes mip i. */
    de_rg_resource hzb =
        de_rg_create_texture(g, &(de_gpu_texture_desc){.format = DE_FMT_R32_FLOAT,
                                                       .width = 64,
                                                       .height = 64,
                                                       .mip_count = 4,
                                                       .usage = DE_USAGE_SAMPLED | DE_USAGE_STORAGE,
                                                       .name = "hzb"});
    de_rg_pass *h[4];
    h[0] = de_rg_add_pass(g, "hzb0", DE_QUEUE_COMPUTE, NULL, NULL);
    de_rg_write_sub(h[0], hzb, (de_rg_subresource){.mip_first = 0, .mip_count = 1},
                    DE_ACCESS_SHADER_WRITE);
    for (u32 i = 1; i < 4; ++i) {
        h[i] = de_rg_add_pass(g, "hzb_down", DE_QUEUE_COMPUTE, NULL, NULL);
        de_rg_read_sub(h[i], hzb, (de_rg_subresource){.mip_first = i - 1, .mip_count = 1},
                       DE_ACCESS_SHADER_READ);
        de_rg_write_sub(h[i], hzb, (de_rg_subresource){.mip_first = i, .mip_count = 1},
                        DE_ACCESS_SHADER_WRITE);
    }
    de_rg_export(g, hzb, DE_ACCESS_SHADER_READ);

    /* Read-modify-write declared with both: depth test + write. */
    de_rg_resource depth = de_rg_create_texture(
        g, &(de_gpu_texture_desc){
               .format = DE_FMT_D32_FLOAT, .width = 64, .height = 64, .usage = DE_USAGE_DEPTH});
    de_rg_pass *z0 = de_rg_add_pass(g, "prepass", DE_QUEUE_GRAPHICS, NULL, NULL);
    de_rg_write(z0, depth, DE_ACCESS_DEPTH_WRITE);
    de_rg_pass *z1 = de_rg_add_pass(g, "opaque", DE_QUEUE_GRAPHICS, NULL, NULL);
    de_rg_read(z1, depth, DE_ACCESS_DEPTH_READ);
    de_rg_write(z1, depth, DE_ACCESS_DEPTH_READ | DE_ACCESS_DEPTH_WRITE);
    de_rg_export(g, depth, DE_ACCESS_NONE);

    ASSERT_EQ(de__rg_finalize(g), DE_OK);
    EXPECT_EQ(g->culled_count, 0u);

    /* Versions: each write makes a new one; a read sees the latest. */
    EXPECT_EQ(g->resources[res_index(f, t)].version_count, 2u);
    EXPECT_EQ(g->accesses[find_access(f, p[0], t, true)].version, 1u);
    EXPECT_EQ(g->accesses[find_access(f, p[1], t, false)].version, 1u);
    EXPECT_EQ(g->accesses[find_access(f, p[2], t, true)].version, 2u);
    EXPECT_EQ(g->accesses[find_access(f, p[3], t, false)].version, 2u);

    /* Edges of the chain. */
    EXPECT_EQ(p[0]->edge_count, 0u);
    EXPECT_EQ(p[1]->edge_count, 1u);
    EXPECT_TRUE(has_edge(f, p[1], 0, DE__RG_EDGE_RAW));
    EXPECT_EQ(p[2]->edge_count, 2u);
    EXPECT_TRUE(has_edge(f, p[2], 0, DE__RG_EDGE_WAW));
    EXPECT_TRUE(has_edge(f, p[2], 1, DE__RG_EDGE_WAR));
    EXPECT_EQ(p[3]->edge_count, 1u);
    EXPECT_TRUE(has_edge(f, p[3], 2, DE__RG_EDGE_RAW));
    /* Incoming edges are sorted by source pass. */
    EXPECT_LT(g->edges[p[2]->edge_first].from, g->edges[p[2]->edge_first + 1u].from);

    /* HZB: each level depends only on the level before it (distinct subresources). */
    EXPECT_EQ(h[0]->edge_count, 0u);
    for (u32 i = 1; i < 4; ++i) {
        EXPECT_EQ(h[i]->edge_count, 1u);
        EXPECT_TRUE(has_edge(f, h[i], h[i - 1]->index, DE__RG_EDGE_RAW));
    }
    EXPECT_EQ(g->resources[res_index(f, hzb)].version_count, 4u);

    /* RMW: the read sees the prepass's version, the write makes a new one after it. */
    EXPECT_EQ(g->accesses[find_access(f, z1, depth, false)].version, 1u);
    EXPECT_EQ(g->accesses[find_access(f, z1, depth, true)].version, 2u);
    EXPECT_EQ(z1->edge_count, 2u);
    EXPECT_TRUE(has_edge(f, z1, z0->index, DE__RG_EDGE_RAW));
    EXPECT_TRUE(has_edge(f, z1, z0->index, DE__RG_EDGE_WAW));

    /* Live ranges of the resources. */
    const de__rg_resource *rt = &g->resources[res_index(f, t)];
    EXPECT_EQ(rt->first_pass, 0u);
    EXPECT_EQ(rt->last_pass, 3u);
    EXPECT_TRUE((rt->flags & DE__RG_RES_USED) != 0);
}

/* ---------- Culling ---------- */

/* Three producers, one compose pass into the swapchain; returns the producers' resources. */
static void build_three_producers(rg_fx *f, exec_log *log, de_rg_pass *prod[3],
                                  de_rg_resource out[3], de_rg_pass **compose) {
    de_rg *g = f->g;
    static const char *const names[3] = {"scene", "debug_overlay", "unused_blur"};
    for (int i = 0; i < 3; ++i) {
        de_gpu_texture_desc d = color_desc(names[i]);
        out[i] = de_rg_create_texture(g, &d);
        prod[i] = de_rg_add_pass(g, names[i], DE_QUEUE_GRAPHICS, record_exec, log);
        de_rg_write(prod[i], out[i], DE_ACCESS_RENDER_TARGET);
    }
    de_rg_resource sc = de_rg_import_texture(g, f->swapchain, DE_ACCESS_NONE);
    *compose = de_rg_add_pass(g, "compose", DE_QUEUE_GRAPHICS, record_exec, log);
    de_rg_read(*compose, out[0], DE_ACCESS_SHADER_READ);
    de_rg_write(*compose, sc, DE_ACCESS_RENDER_TARGET);
    de_rg_export(g, sc, DE_ACCESS_PRESENT);
}

TEST_F(rg_builder, cull_unused, rg_fx) {
    exec_log log = {0};
    de_rg_pass *prod[3], *compose;
    de_rg_resource out[3];
    build_three_producers(f, &log, prod, out, &compose);
    ASSERT_EQ(de__rg_finalize(f->g), DE_OK);

    EXPECT_FALSE(prod[0]->culled);
    EXPECT_TRUE(prod[1]->culled);
    EXPECT_TRUE(prod[2]->culled);
    EXPECT_FALSE(compose->culled);
    EXPECT_EQ(f->g->culled_count, 2u);
    /* Transients that only culled passes use are never allocated. */
    EXPECT_TRUE((f->g->resources[res_index(f, out[0])].flags & DE__RG_RES_USED) != 0);
    EXPECT_TRUE((f->g->resources[res_index(f, out[1])].flags & DE__RG_RES_USED) == 0);
    EXPECT_TRUE((f->g->resources[res_index(f, out[2])].flags & DE__RG_RES_USED) == 0);
    EXPECT_EQ(f->g->resources[res_index(f, out[1])].first_pass, DE__RG_NONE);
    /* Culled passes have no edges and never execute. */
    EXPECT_EQ(prod[1]->edge_count, 0u);
    EXPECT_EQ(compose->edge_count, 1u);
    EXPECT_TRUE(has_edge(f, compose, prod[0]->index, DE__RG_EDGE_RAW));
    run_all(f);
    EXPECT_EQ(log.calls, 2u);
    EXPECT_EQ(log.order[0], prod[0]->index);
    EXPECT_EQ(log.order[1], compose->index);

    /* An overwrite makes the earlier writer of an import dead too. */
    de__rg_reset(f->g);
    de_arena_reset(f->arena);
    ASSERT_EQ(de__rg_begin(f->g, f->arena), DE_OK);
    de_rg_resource sc = de_rg_import_texture(f->g, f->swapchain, DE_ACCESS_NONE);
    de_rg_pass *clear = de_rg_add_pass(f->g, "clear", DE_QUEUE_GRAPHICS, NULL, NULL);
    de_rg_write(clear, sc, DE_ACCESS_RENDER_TARGET);
    de_rg_pass *full = de_rg_add_pass(f->g, "fullscreen", DE_QUEUE_GRAPHICS, NULL, NULL);
    de_rg_write(full, sc, DE_ACCESS_RENDER_TARGET);
    de_rg_pass *blend = de_rg_add_pass(f->g, "ui_blend", DE_QUEUE_GRAPHICS, NULL, NULL);
    de_rg_read(blend, sc, DE_ACCESS_RENDER_TARGET); /* blends onto the existing contents */
    de_rg_write(blend, sc, DE_ACCESS_RENDER_TARGET);
    ASSERT_EQ(de__rg_finalize(f->g), DE_OK);
    EXPECT_TRUE(clear->culled);
    EXPECT_FALSE(full->culled);
    EXPECT_FALSE(blend->culled);
}

TEST_F(rg_builder, side_effect_kept, rg_fx) {
    exec_log log = {0};
    de_rg_pass *prod[3], *compose;
    de_rg_resource out[3];
    build_three_producers(f, &log, prod, out, &compose);
    /* A readback pass of the unused blur: it has effects outside the graph. */
    de_rg_pass *readback = de_rg_add_pass(f->g, "readback", DE_QUEUE_COPY, record_exec, &log);
    de_rg_read(readback, out[2], DE_ACCESS_COPY_SRC);
    de_rg_pass_side_effect(readback);
    /* A pass whose only output is an exported transient. */
    de_gpu_texture_desc hd = color_desc("history");
    de_rg_resource history = de_rg_create_texture(f->g, &hd);
    de_rg_pass *taa = de_rg_add_pass(f->g, "taa", DE_QUEUE_COMPUTE, record_exec, &log);
    de_rg_write(taa, history, DE_ACCESS_SHADER_WRITE);
    de_rg_export(f->g, history, DE_ACCESS_SHADER_READ);
    /* A side-effect pass with no resources at all. */
    de_rg_pass *marker = de_rg_add_pass(f->g, "gpu_counter", DE_QUEUE_GRAPHICS, record_exec, &log);
    de_rg_pass_side_effect(marker);
    ASSERT_EQ(de__rg_finalize(f->g), DE_OK);

    EXPECT_FALSE(readback->culled);
    EXPECT_FALSE(prod[2]->culled); /* feeds the side-effect pass */
    EXPECT_TRUE(prod[1]->culled);
    EXPECT_FALSE(taa->culled);
    EXPECT_FALSE(marker->culled);
    EXPECT_FALSE(compose->culled);
    EXPECT_EQ(f->g->culled_count, 1u);
    EXPECT_TRUE(has_edge(f, readback, prod[2]->index, DE__RG_EDGE_RAW));
    EXPECT_TRUE((f->g->resources[res_index(f, history)].flags & DE__RG_RES_EXPORTED) != 0);
    run_all(f);
    EXPECT_EQ(log.calls, 6u);
}

/* ---------- Validation ---------- */

TEST_F(rg_builder, validation_errors, rg_fx) {
#if !DE__RG_VALIDATE
    SKIP("declarations are validated in Debug and Dev only");
#else
    de_rg *g = f->g;
    de_gpu_texture_desc cd = color_desc("gbuffer");
    de_rg_resource color = de_rg_create_texture(g, &cd);
    de_rg_resource depth =
        de_rg_create_texture(g, &(de_gpu_texture_desc){.format = DE_FMT_D32_FLOAT,
                                                       .width = 64,
                                                       .height = 64,
                                                       .usage = DE_USAGE_DEPTH,
                                                       .name = "depth"});
    de_rg_resource args = de_rg_create_buffer(
        g, &(de_gpu_buffer_desc){
               .size = 256, .usage = DE_USAGE_INDIRECT | DE_USAGE_STORAGE, .name = "args"});

    /* Render-target write on the compute queue. */
    de_error_clear();
    de_rg_pass *cs = de_rg_add_pass(g, "cs", DE_QUEUE_COMPUTE, NULL, NULL);
    de_rg_write(cs, color, DE_ACCESS_RENDER_TARGET);
    EXPECT_ERROR_HAS("de_rg_write(\"cs\", \"gbuffer\")", "compute queue", "RENDER_TARGET");
    EXPECT_EQ(cs->access_count, 0u);
    /* Depth read on compute, shader read on copy. */
    de_error_clear();
    de_rg_read(cs, depth, DE_ACCESS_DEPTH_READ);
    EXPECT_ERROR_HAS("compute queue", "DEPTH_READ");
    de_error_clear();
    de_rg_pass *cp = de_rg_add_pass(g, "upload", DE_QUEUE_COPY, NULL, NULL);
    de_rg_read(cp, color, DE_ACCESS_SHADER_READ);
    EXPECT_ERROR_HAS("copy queue", "SHADER_READ");
    /* Write bits in a read, read-only bits in a write, PRESENT, texture access on a buffer. */
    de_error_clear();
    de_rg_pass *gp = de_rg_add_pass(g, "gfx", DE_QUEUE_GRAPHICS, NULL, NULL);
    de_rg_read(gp, color, DE_ACCESS_SHADER_WRITE);
    EXPECT_ERROR_HAS("SHADER_WRITE are write accesses", "de_rg_write");
    de_error_clear();
    de_rg_write(gp, color, DE_ACCESS_SHADER_READ);
    EXPECT_ERROR_HAS("no write access");
    de_error_clear();
    de_rg_write(gp, color, DE_ACCESS_PRESENT);
    EXPECT_ERROR_HAS("DE_ACCESS_PRESENT is a final state", "de_rg_export");
    de_error_clear();
    de_rg_write(gp, args, DE_ACCESS_RENDER_TARGET);
    EXPECT_ERROR_HAS("texture-only", "\"args\" is a buffer");
    de_error_clear();
    de_rg_read_sub(gp, color, (de_rg_subresource){.mip_first = 2}, DE_ACCESS_SHADER_READ);
    EXPECT_ERROR_HAS("mips from 2", "outside the texture (1 mips, 1 layers)");
    EXPECT_EQ(gp->access_count, 0u);
    EXPECT_EQ(g->error_count, 8u);

    /* Finalize checks: a read of a never-written transient, and a read-modify-write write
     * without the read. */
    de_rg_pass *early = de_rg_add_pass(g, "early_read", DE_QUEUE_GRAPHICS, NULL, NULL);
    de_rg_read(early, color, DE_ACCESS_SHADER_READ);
    de_rg_pass_side_effect(early);
    de_rg_pass *zp = de_rg_add_pass(g, "z_rmw", DE_QUEUE_GRAPHICS, NULL, NULL);
    de_rg_write(zp, depth, DE_ACCESS_DEPTH_READ | DE_ACCESS_DEPTH_WRITE);
    de_rg_pass_side_effect(zp);
    /* Correct declarations raise nothing. */
    de_rg_pass *ok = de_rg_add_pass(g, "fill", DE_QUEUE_GRAPHICS, NULL, NULL);
    de_rg_write(ok, color, DE_ACCESS_RENDER_TARGET);
    de_rg_pass *ok2 = de_rg_add_pass(g, "blend", DE_QUEUE_GRAPHICS, NULL, NULL);
    de_rg_read(ok2, color, DE_ACCESS_SHADER_READ);
    de_rg_write(ok2, color, DE_ACCESS_SHADER_READ | DE_ACCESS_RENDER_TARGET);
    de_rg_pass_side_effect(ok2);
    EXPECT_EQ(g->error_count, 8u);

    de_error_clear();
    EXPECT_EQ(de__rg_finalize(g), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(g->error_count, 10u);
    const de_error *e = de_error_last();
    ASSERT_TRUE(e != NULL);
    EXPECT_TRUE(strstr(e->message, "10 invalid declarations") != NULL);
    ASSERT_TRUE(e->cause != NULL);
    /* The last failure is the cause (each failure was logged to `render` as it happened). */
    EXPECT_TRUE(strstr(e->cause->message, "pass \"z_rmw\" writes \"depth\" with read access "
                                          "(DEPTH_READ) but does not declare the read") != NULL);

    /* de_rg_texture / de_rg_buffer outside an execute callback. */
    de_error_clear();
    EXPECT_FALSE(DE_HANDLE_VALID(de_rg_texture(ok2, color)));
    EXPECT_ERROR_HAS("de_rg_texture(\"blend\")", "outside the pass's execute callback");
    de_error_clear();
    EXPECT_FALSE(DE_HANDLE_VALID(de_rg_buffer(ok2, args)));
    EXPECT_ERROR_HAS("de_rg_buffer(\"blend\")", "outside");
    /* Inside it, a resource the pass did not declare is refused. */
    exec_log log = {.tex_res = depth};
    ok2->fn = record_exec;
    ok2->user = &log;
    de_error_clear();
    de__rg_execute_pass(g, ok2->index, NULL);
    EXPECT_EQ(log.calls, 1u);
    EXPECT_EQ(log.tex_seen.id, 0u);
    EXPECT_ERROR_HAS("did not declare", "\"depth\"");
    EXPECT_EQ(g->error_count, 10u); /* execute-time errors do not touch the graph */

    /* The never-written read on its own; reading an import before any write is fine. */
    de__rg_reset(g);
    de_arena_reset(f->arena);
    ASSERT_EQ(de__rg_begin(g, f->arena), DE_OK);
    de_rg_resource fresh = de_rg_create_texture(g, &cd);
    de_rg_resource sc = de_rg_import_texture(g, f->swapchain, DE_ACCESS_NONE);
    de_rg_pass *rd = de_rg_add_pass(g, "early_read", DE_QUEUE_GRAPHICS, NULL, NULL);
    de_rg_read(rd, fresh, DE_ACCESS_SHADER_READ);
    de_rg_read(rd, sc, DE_ACCESS_SHADER_READ);
    de_rg_pass_side_effect(rd);
    de_error_clear();
    EXPECT_EQ(de__rg_finalize(g), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(g->error_count, 1u);
    e = de_error_last();
    ASSERT_TRUE(e != NULL && e->cause != NULL);
    EXPECT_TRUE(strstr(e->message, "1 invalid declaration this frame") != NULL);
    EXPECT_TRUE(strstr(e->cause->message,
                       "render graph \"test\": pass \"early_read\" reads "
                       "transient \"gbuffer\" before any pass writes it") != NULL);
#endif
}

/* ---------- No heap after warm-up ---------- */

static void exec_nop(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    (void)cmd;
    (void)pass;
    (void)user;
}

/* 200 passes: a chain of transients (each pass reads the previous one), some side branches that
 * get culled, a couple of mip-ranged accesses, into the swapchain. */
static de_result build_200(rg_fx *f, u32 *culled) {
    de_rg *g = f->g;
    DE_TRY(de__rg_begin(g, f->arena));
    static const de_gpu_texture_desc d = {.format = DE_FMT_RGBA16_FLOAT,
                                          .width = 256,
                                          .height = 256,
                                          .mip_count = 2,
                                          .usage = DE_USAGE_SAMPLED | DE_USAGE_STORAGE |
                                                   DE_USAGE_RENDER_TARGET,
                                          .name = "chain"};
    de_rg_resource sc = de_rg_import_texture(g, f->swapchain, DE_ACCESS_NONE);
    de_rg_resource prev = {0};
    for (u32 i = 0; i < 199; ++i) {
        de_rg_resource t = de_rg_create_texture(g, &d);
        de_rg_pass *p =
            de_rg_add_pass(g, (i % 5 == 4) ? "branch" : "chain",
                           (i % 3) ? DE_QUEUE_COMPUTE : DE_QUEUE_GRAPHICS, exec_nop, NULL);
        if (DE_HANDLE_VALID(prev))
            de_rg_read_sub(p, prev, (de_rg_subresource){.mip_count = 1}, DE_ACCESS_SHADER_READ);
        de_rg_write(p, t, DE_ACCESS_SHADER_WRITE);
        if (i % 5 != 4) /* branch outputs are never read: culled */
            prev = t;
    }
    de_rg_pass *last = de_rg_add_pass(g, "present", DE_QUEUE_GRAPHICS, exec_nop, NULL);
    de_rg_read(last, prev, DE_ACCESS_SHADER_READ);
    de_rg_write(last, sc, DE_ACCESS_RENDER_TARGET);
    de_rg_export(g, sc, DE_ACCESS_PRESENT);
    DE_TRY(de__rg_finalize(g));
    *culled = g->culled_count;
    for (u32 i = 0; i < g->pass_count; ++i)
        de__rg_execute_pass(g, i, NULL);
    de__rg_reset(g);
    de_arena_reset(f->arena);
    return DE_OK;
}

TEST_F(rg_builder, no_heap_after_warmup, rg_fx) {
    de__rg_reset(f->g);
    de_arena_reset(f->arena);
    u32 culled = 0;
    for (int i = 0; i < 3; ++i)
        ASSERT_EQ(build_200(f, &culled), DE_OK);
    EXPECT_EQ(culled, 39u); /* every fifth chain pass is a branch nobody reads */
    EXPECT_EQ(f->g->hint_passes, 200u);
    de_tracking_stats before = de_tracking_allocator_stats(&f->track);

    de_alloc_guard guard = {0};
    de_alloc_guard_begin(&guard);
    de_result r = DE_OK;
    for (int i = 0; i < 20 && r == DE_OK; ++i)
        r = build_200(f, &culled);
    de_alloc_guard_end(&guard);
    ASSERT_EQ(r, DE_OK);
    DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
    de_tracking_stats after = de_tracking_allocator_stats(&f->track);
    EXPECT_EQ(after.alloc_count, before.alloc_count);
    EXPECT_EQ(after.realloc_count, before.realloc_count);
    EXPECT_EQ(culled, 39u);
}

/* ---------- Dot dump ---------- */

TEST_F(rg_builder, dot_dump, rg_fx) {
    exec_log log = {0};
    de_rg_pass *prod[3], *compose;
    de_rg_resource out[3];
    build_three_producers(f, &log, prod, out, &compose);
    de_rg_resource hzb =
        de_rg_create_texture(f->g, &(de_gpu_texture_desc){.format = DE_FMT_R32_FLOAT,
                                                          .width = 64,
                                                          .height = 64,
                                                          .mip_count = 2,
                                                          .usage = DE_USAGE_STORAGE,
                                                          .name = "hzb \"quoted\""});
    de_rg_pass *h = de_rg_add_pass(f->g, "hzb", DE_QUEUE_COMPUTE, NULL, NULL);
    de_rg_write_sub(h, hzb, (de_rg_subresource){.mip_first = 1, .mip_count = 1},
                    DE_ACCESS_SHADER_WRITE);
    de_rg_pass_side_effect(h);
    ASSERT_EQ(de__rg_finalize(f->g), DE_OK);

    de_strbuf b;
    de_strbuf_init(&b, &f->alloc);
    ASSERT_EQ(de__rg_dump_dot(f->g, &b), DE_OK);
    const char *s = de_strbuf_cstr(&b);
    static const char *const needles[] = {
        "digraph \"test\" {",
        "5 passes (2 culled)",
        "p0 [shape=box, label=\"0: scene\\ngraphics\", style=\"rounded,filled\"",
        ("p1 [shape=box, label=\"1: debug_overlay\\ngraphics\\nculled\", "
         "style=\"rounded,filled,dashed\", fillcolor=\"#eeeeee\""),
        "r0_v1 [shape=ellipse, label=\"scene v1\\ntransient texture\"]",
        "r1_v1 [shape=ellipse, label=\"debug_overlay v1\\ntransient texture\", style=dashed",
        "p0 -> r0_v1 [label=\"RENDER_TARGET\"]",
        "r0_v1 -> p3 [label=\"SHADER_READ\"]",
        "p1 -> r1_v1 [label=\"RENDER_TARGET\", style=dashed",
        "p3 -> r3_v1 [label=\"RENDER_TARGET\"]",
        "transient texture, exported", /* not set: the swapchain is imported */
        "hzb \\\"quoted\\\" v1 mips 1-1",
        "p4 -> r4_v1 [label=\"SHADER_WRITE mips 1-1\"]",
        "}\n",
    };
    for (size_t i = 0; i < sizeof needles / sizeof needles[0]; ++i) {
        bool want = i != 10;
        bool got = strstr(s, needles[i]) != NULL;
        if (got != want)
            fprintf(stderr, "dot dump %s \"%s\":\n%s\n", want ? "lacks" : "has", needles[i], s);
        EXPECT_EQ(got, want);
    }
    /* The swapchain's final version is an output: double border. */
    EXPECT_TRUE(strstr(s, "imported texture, exported\", peripheries=2") != NULL);
    de_strbuf_free(&b);

    /* r.rg_dump writes the next finalized graph to its path once, then clears. */
    de_cvar cv = de_cvar_find("r.rg_dump");
#if defined(DE_SHIPPING)
    EXPECT_FALSE(DE_HANDLE_VALID(cv));
    return;
#else
    ASSERT_TRUE(DE_HANDLE_VALID(cv));
    ASSERT_EQ(de_fs_mount("rgout", DE_RG_TEST_OUT_DIR, DE_MOUNT_WRITABLE, 0), DE_OK);
    ASSERT_EQ(de_cvar_set(cv, "rgout:/rg_dump_test.dot"), DE_OK);
    de__rg_reset(f->g);
    de_arena_reset(f->arena);
    ASSERT_EQ(de__rg_begin(f->g, f->arena), DE_OK);
    build_three_producers(f, &log, prod, out, &compose);
    ASSERT_EQ(de__rg_finalize(f->g), DE_OK);
    EXPECT_STREQ(de_cvar_string(cv), "");
    void *data = NULL;
    u64 size = 0;
    ASSERT_EQ(de_file_read_all("rgout:/rg_dump_test.dot", &f->alloc, &data, &size), DE_OK);
    EXPECT_TRUE(size > 0);
    EXPECT_TRUE(memcmp(data, "digraph \"test\"", 14) == 0);
    DE_FREE(&f->alloc, data, (size_t)size);
    EXPECT_EQ(de_fs_unmount("rgout", DE_RG_TEST_OUT_DIR), DE_OK);
#endif
}
