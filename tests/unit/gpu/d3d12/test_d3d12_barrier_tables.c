/* test_d3d12_barrier_tables.c — the D3D12 barrier translation (DENG-286), on every build host.
 * CTest name: gpu_d3d12.d3d12.barrier_translation_table
 * (DENG-284's name for the Vulkan table test).
 *
 * Pure functions of src/gpu/d3d12/d3d12_barriers.c (compiled into the test where the backend is
 * not built): de_gpu_access -> enhanced sync / access / layout per queue type, normalization of
 * combined accesses, the access pairs DENG-284's GPU tests use (the same pairs Vulkan and Metal
 * check), subresource ranges, queue-ownership transfers through COMMON, and the legacy
 * D3D12_RESOURCE_STATES translation with tracked states. The D3D12 numbers are the copies in
 * d3d12_internal.h, which the Windows build checks against d3d12.h.
 */
#include "de_test.h"

#include <dengine/de_gpu.h>

#include "gpu/d3d12/d3d12_internal.h"

#include <string.h>

#define SYNC(x)   DE__D3D12_SYNC_##x
#define ACC(x)    DE__D3D12_ACCESS_##x
#define LAYOUT(x) DE__D3D12_LAYOUT_##x
#define STATE(x)  DE__D3D12_STATE_##x

static de_gpu_barrier tex_barrier(u32 before, u32 after) {
    de_gpu_barrier b;
    memset(&b, 0, sizeof b);
    b.texture.id = 1;
    b.before = before;
    b.after = after;
    return b;
}

static de_gpu_barrier buf_barrier(u32 before, u32 after) {
    de_gpu_barrier b;
    memset(&b, 0, sizeof b);
    b.buffer.id = 1;
    b.before = before;
    b.after = after;
    return b;
}

TEST(d3d12, barrier_translation_table) {
    const de_gpu_queue G = DE_QUEUE_GRAPHICS, C = DE_QUEUE_COMPUTE, X = DE_QUEUE_COPY;
    de__d3d12_access_info a;

    /* ---- One bit at a time, textures, destination side (graphics queue). ---- */
    a = de__d3d12_access_info_of(G, DE_ACCESS_RENDER_TARGET, true, false);
    EXPECT_EQ(a.sync, SYNC(RENDER_TARGET));
    EXPECT_EQ(a.access, ACC(RENDER_TARGET));
    EXPECT_EQ(a.layout, LAYOUT(RENDER_TARGET));
    a = de__d3d12_access_info_of(G, DE_ACCESS_SHADER_READ, true, false);
    EXPECT_EQ(a.sync, SYNC(ALL_SHADING));
    EXPECT_EQ(a.access, ACC(SHADER_RESOURCE));
    EXPECT_EQ(a.layout, LAYOUT(SHADER_RESOURCE));
    a = de__d3d12_access_info_of(G, DE_ACCESS_SHADER_WRITE, true, false);
    EXPECT_EQ(a.sync, SYNC(ALL_SHADING));
    EXPECT_EQ(a.access, ACC(UNORDERED_ACCESS));
    EXPECT_EQ(a.layout, LAYOUT(UNORDERED_ACCESS));
    a = de__d3d12_access_info_of(G, DE_ACCESS_DEPTH_WRITE, true, false);
    EXPECT_EQ(a.sync, SYNC(DEPTH_STENCIL));
    EXPECT_EQ(a.access, ACC(DEPTH_STENCIL_WRITE));
    EXPECT_EQ(a.layout, LAYOUT(DEPTH_STENCIL_WRITE));
    a = de__d3d12_access_info_of(G, DE_ACCESS_DEPTH_READ, true, false);
    EXPECT_EQ(a.sync, SYNC(DEPTH_STENCIL));
    EXPECT_EQ(a.access, ACC(DEPTH_STENCIL_READ));
    EXPECT_EQ(a.layout, LAYOUT(DEPTH_STENCIL_READ));
    a = de__d3d12_access_info_of(G, DE_ACCESS_COPY_SRC, true, false);
    EXPECT_EQ(a.sync, SYNC(COPY));
    EXPECT_EQ(a.access, ACC(COPY_SOURCE));
    EXPECT_EQ(a.layout, LAYOUT(COPY_SOURCE));
    a = de__d3d12_access_info_of(G, DE_ACCESS_COPY_DST, true, false);
    EXPECT_EQ(a.sync, SYNC(COPY));
    EXPECT_EQ(a.access, ACC(COPY_DEST));
    EXPECT_EQ(a.layout, LAYOUT(COPY_DEST));
    /* PRESENT: the COMMON layout, nothing waits after it; before it, wait for everything. */
    a = de__d3d12_access_info_of(G, DE_ACCESS_PRESENT, true, false);
    EXPECT_EQ(a.sync, SYNC(NONE));
    EXPECT_EQ(a.access, ACC(NO_ACCESS));
    EXPECT_EQ(a.layout, LAYOUT(COMMON));
    a = de__d3d12_access_info_of(G, DE_ACCESS_PRESENT, true, true);
    EXPECT_EQ(a.sync, SYNC(ALL));
    EXPECT_EQ(a.access, ACC(NO_ACCESS));
    EXPECT_EQ(a.layout, LAYOUT(COMMON));
    /* NONE: before = contents discarded (UNDEFINED) after everything; after = no later use. */
    a = de__d3d12_access_info_of(G, DE_ACCESS_NONE, true, true);
    EXPECT_EQ(a.sync, SYNC(ALL));
    EXPECT_EQ(a.access, ACC(NO_ACCESS));
    EXPECT_EQ(a.layout, LAYOUT(UNDEFINED));
    a = de__d3d12_access_info_of(G, DE_ACCESS_NONE, true, false);
    EXPECT_EQ(a.sync, SYNC(NONE));
    EXPECT_EQ(a.access, ACC(NO_ACCESS));

    /* ---- Buffers: no layout. ---- */
    a = de__d3d12_access_info_of(G, DE_ACCESS_SHADER_READ, false, false);
    EXPECT_EQ(a.sync, SYNC(ALL_SHADING) | SYNC(INDEX_INPUT));
    EXPECT_EQ(a.access,
              ACC(VERTEX_BUFFER) | ACC(CONSTANT_BUFFER) | ACC(INDEX_BUFFER) | ACC(SHADER_RESOURCE));
    EXPECT_EQ(a.layout, LAYOUT(UNDEFINED));
    a = de__d3d12_access_info_of(G, DE_ACCESS_INDIRECT, false, false);
    EXPECT_EQ(a.sync, SYNC(EXECUTE_INDIRECT));
    EXPECT_EQ(a.access, ACC(INDIRECT_ARGUMENT));
    a = de__d3d12_access_info_of(G, DE_ACCESS_ACCEL_WRITE, false, false);
    EXPECT_EQ(a.sync, SYNC(BUILD_RTAS));
    EXPECT_EQ(a.access, ACC(RTAS_WRITE));
    a = de__d3d12_access_info_of(G, DE_ACCESS_COPY_DST, false, true);
    EXPECT_EQ(a.sync, SYNC(COPY));
    EXPECT_EQ(a.access, ACC(COPY_DEST));

    /* ---- Combinations. ---- */
    EXPECT_EQ(de__d3d12_access_normalize(DE_ACCESS_RENDER_TARGET | DE_ACCESS_SHADER_READ),
              (u32)DE_ACCESS_RENDER_TARGET); /* a write excludes the rest */
    EXPECT_EQ(de__d3d12_access_normalize(DE_ACCESS_DEPTH_WRITE | DE_ACCESS_DEPTH_READ),
              (u32)DE_ACCESS_DEPTH_WRITE);
    EXPECT_EQ(de__d3d12_access_normalize(DE_ACCESS_SHADER_WRITE | DE_ACCESS_COPY_DST),
              (u32)DE_ACCESS_SHADER_WRITE);
    EXPECT_EQ(de__d3d12_access_normalize(DE_ACCESS_PRESENT | DE_ACCESS_COPY_SRC),
              (u32)DE_ACCESS_COPY_SRC);
    EXPECT_EQ(de__d3d12_access_normalize(DE_ACCESS_SHADER_READ | DE_ACCESS_COPY_SRC),
              (u32)(DE_ACCESS_SHADER_READ | DE_ACCESS_COPY_SRC));
    a = de__d3d12_access_info_of(G, DE_ACCESS_SHADER_READ | DE_ACCESS_COPY_SRC, true, false);
    EXPECT_EQ(a.layout, LAYOUT(GENERIC_READ));
    EXPECT_EQ(a.sync, SYNC(ALL_SHADING) | SYNC(COPY));
    EXPECT_EQ(a.access, ACC(SHADER_RESOURCE) | ACC(COPY_SOURCE));
    /* Sampling a depth buffer that is also bound read-only: the direct queue's generic read. */
    a = de__d3d12_access_info_of(G, DE_ACCESS_SHADER_READ | DE_ACCESS_DEPTH_READ, true, false);
    EXPECT_EQ(a.layout, LAYOUT(DIRECT_QUEUE_GENERIC_READ));
    EXPECT_EQ(a.access, ACC(SHADER_RESOURCE) | ACC(DEPTH_STENCIL_READ));
    EXPECT_EQ(a.sync, SYNC(ALL_SHADING) | SYNC(DEPTH_STENCIL));

    /* ---- Queue types. ---- */
    /* Copy queue: copy syncs and accesses only, textures always COMMON. */
    a = de__d3d12_access_info_of(X, DE_ACCESS_SHADER_READ, true, false);
    EXPECT_EQ(a.sync, SYNC(NONE));
    EXPECT_EQ(a.access, ACC(NO_ACCESS));
    EXPECT_EQ(a.layout, LAYOUT(COMMON));
    a = de__d3d12_access_info_of(X, DE_ACCESS_COPY_DST, true, false);
    EXPECT_EQ(a.sync, SYNC(COPY));
    EXPECT_EQ(a.access, ACC(COPY_DEST));
    EXPECT_EQ(a.layout, LAYOUT(COMMON));
    a = de__d3d12_access_info_of(X, DE_ACCESS_NONE, true, true);
    EXPECT_EQ(a.sync, SYNC(ALL));
    EXPECT_EQ(a.layout, LAYOUT(COMMON)); /* no UNDEFINED -> COMMON transition on a copy queue */
    /* Compute queue: no input assembler, raster or depth; ALL_SHADING is compute shading. */
    a = de__d3d12_access_info_of(C, DE_ACCESS_SHADER_READ, false, false);
    EXPECT_EQ(a.sync, SYNC(COMPUTE_SHADING));
    EXPECT_EQ(a.access, ACC(CONSTANT_BUFFER) | ACC(SHADER_RESOURCE));
    a = de__d3d12_access_info_of(C, DE_ACCESS_SHADER_WRITE, true, false);
    EXPECT_EQ(a.sync, SYNC(COMPUTE_SHADING));
    EXPECT_EQ(a.layout, LAYOUT(UNORDERED_ACCESS));
    a = de__d3d12_access_info_of(C, DE_ACCESS_SHADER_READ | DE_ACCESS_DEPTH_READ, true, false);
    EXPECT_EQ(a.layout, LAYOUT(GENERIC_READ));
    EXPECT_EQ(a.access, ACC(SHADER_RESOURCE));

    /* ---- The access pairs of DENG-284's GPU tests (Vulkan and Metal check the same). ---- */
    const de__d3d12_texture_shape shape = {.mips = 5, .layers = 6, .planes = 1, .rt_ds = true};
    static const struct {
        u32 before, after;
        u32 sync_before, sync_after, access_before, access_after, layout_before, layout_after;
        bool discard;
        u32 state_from, state_to; /* legacy, from the state the pair's `before` leaves */
    } k_pairs[] = {
        {DE_ACCESS_NONE, DE_ACCESS_RENDER_TARGET, SYNC(ALL), SYNC(RENDER_TARGET), ACC(NO_ACCESS),
         ACC(RENDER_TARGET), LAYOUT(UNDEFINED), LAYOUT(RENDER_TARGET), true, STATE(COMMON),
         STATE(RENDER_TARGET)},
        {DE_ACCESS_RENDER_TARGET, DE_ACCESS_COPY_SRC, SYNC(RENDER_TARGET), SYNC(COPY),
         ACC(RENDER_TARGET), ACC(COPY_SOURCE), LAYOUT(RENDER_TARGET), LAYOUT(COPY_SOURCE), false,
         STATE(RENDER_TARGET), STATE(COPY_SOURCE)},
        {DE_ACCESS_NONE, DE_ACCESS_COPY_DST, SYNC(ALL), SYNC(COPY), ACC(NO_ACCESS), ACC(COPY_DEST),
         LAYOUT(UNDEFINED), LAYOUT(COPY_DEST), false, STATE(COMMON), STATE(COPY_DEST)},
        {DE_ACCESS_COPY_DST, DE_ACCESS_COPY_SRC, SYNC(COPY), SYNC(COPY), ACC(COPY_DEST),
         ACC(COPY_SOURCE), LAYOUT(COPY_DEST), LAYOUT(COPY_SOURCE), false, STATE(COPY_DEST),
         STATE(COPY_SOURCE)},
        {DE_ACCESS_RENDER_TARGET, DE_ACCESS_PRESENT, SYNC(RENDER_TARGET), SYNC(NONE),
         ACC(RENDER_TARGET), ACC(NO_ACCESS), LAYOUT(RENDER_TARGET), LAYOUT(COMMON), false,
         STATE(RENDER_TARGET), STATE(COMMON)},
        {DE_ACCESS_SHADER_WRITE, DE_ACCESS_SHADER_READ, SYNC(ALL_SHADING), SYNC(ALL_SHADING),
         ACC(UNORDERED_ACCESS), ACC(SHADER_RESOURCE), LAYOUT(UNORDERED_ACCESS),
         LAYOUT(SHADER_RESOURCE), false, STATE(UNORDERED_ACCESS),
         STATE(NON_PIXEL_SHADER_RESOURCE) | STATE(PIXEL_SHADER_RESOURCE)},
    };
    for (u32 i = 0; i < sizeof k_pairs / sizeof k_pairs[0]; ++i) {
        const de_gpu_barrier b = tex_barrier(k_pairs[i].before, k_pairs[i].after);
        de__d3d12_enhanced e;
        ASSERT_TRUE(de__d3d12_translate_enhanced(G, &b, &shape, &e));
        EXPECT_EQ(e.sync_before, k_pairs[i].sync_before);
        EXPECT_EQ(e.sync_after, k_pairs[i].sync_after);
        EXPECT_EQ(e.access_before, k_pairs[i].access_before);
        EXPECT_EQ(e.access_after, k_pairs[i].access_after);
        EXPECT_EQ(e.layout_before, k_pairs[i].layout_before);
        EXPECT_EQ(e.layout_after, k_pairs[i].layout_after);
        EXPECT_EQ(e.discard, k_pairs[i].discard);
        /* 0 counts: every mip, layer and plane. */
        EXPECT_EQ(e.mip_first, 0u);
        EXPECT_EQ(e.mip_count, 5u);
        EXPECT_EQ(e.layer_first, 0u);
        EXPECT_EQ(e.layer_count, 6u);
        EXPECT_EQ(e.plane_count, 1u);
        /* Enhanced-barrier rules: SYNC_NONE goes with NO_ACCESS; UNDEFINED with NO_ACCESS. */
        if (e.sync_before == SYNC(NONE))
            EXPECT_EQ(e.access_before, ACC(NO_ACCESS));
        if (e.sync_after == SYNC(NONE))
            EXPECT_EQ(e.access_after, ACC(NO_ACCESS));
        if (e.layout_before == LAYOUT(UNDEFINED))
            EXPECT_EQ(e.access_before, ACC(NO_ACCESS));

        /* Legacy: from the state the previous use left, to the pair's state. */
        u32 sb = 0, sa = 0;
        EXPECT_EQ(de__d3d12_translate_legacy(G, &b, true, k_pairs[i].state_from, &sb, &sa),
                  DE__D3D12_LEGACY_TRANSITION);
        EXPECT_EQ(sb, k_pairs[i].state_from);
        EXPECT_EQ(sa, k_pairs[i].state_to);
    }
    /* The buffer pairs (cross_queue_timeline: NONE -> COPY_DST before the upload copy). */
    {
        const de_gpu_barrier b = buf_barrier(DE_ACCESS_NONE, DE_ACCESS_COPY_DST);
        de__d3d12_enhanced e;
        ASSERT_TRUE(de__d3d12_translate_enhanced(X, &b, NULL, &e));
        EXPECT_EQ(e.sync_before, SYNC(ALL));
        EXPECT_EQ(e.access_before, ACC(NO_ACCESS));
        EXPECT_EQ(e.sync_after, SYNC(COPY));
        EXPECT_EQ(e.access_after, ACC(COPY_DEST));
        EXPECT_EQ(e.layout_before, LAYOUT(UNDEFINED));
        EXPECT_EQ(e.layout_after, LAYOUT(UNDEFINED));
        EXPECT_FALSE(e.discard);
        u32 sb = 0, sa = 0;
        EXPECT_EQ(de__d3d12_translate_legacy(X, &b, false, STATE(COMMON), &sb, &sa),
                  DE__D3D12_LEGACY_TRANSITION);
        EXPECT_EQ(sb, STATE(COMMON));
        EXPECT_EQ(sa, STATE(COPY_DEST));
    }

    /* ---- Nothing to record. ---- */
    {
        de_gpu_barrier b = tex_barrier(DE_ACCESS_NONE, DE_ACCESS_NONE);
        de__d3d12_enhanced e;
        EXPECT_FALSE(de__d3d12_translate_enhanced(G, &b, &shape, &e));
        /* A buffer barrier that keeps UNORDERED_ACCESS is a UAV barrier in legacy terms; a read
         * that stays a read is nothing. */
        b = buf_barrier(DE_ACCESS_SHADER_WRITE, DE_ACCESS_SHADER_WRITE);
        u32 sb = 0, sa = 0;
        EXPECT_EQ(de__d3d12_translate_legacy(G, &b, false, STATE(UNORDERED_ACCESS), &sb, &sa),
                  DE__D3D12_LEGACY_UAV);
        EXPECT_EQ(sa, STATE(UNORDERED_ACCESS));
        b = tex_barrier(DE_ACCESS_COPY_SRC, DE_ACCESS_COPY_SRC);
        EXPECT_EQ(de__d3d12_translate_legacy(G, &b, true, STATE(COPY_SOURCE), &sb, &sa),
                  DE__D3D12_LEGACY_NOTHING);
        /* after NONE keeps the state (legacy) and the layout (enhanced). */
        b = tex_barrier(DE_ACCESS_COPY_DST, DE_ACCESS_NONE);
        EXPECT_EQ(de__d3d12_translate_legacy(G, &b, true, STATE(COPY_DEST), &sb, &sa),
                  DE__D3D12_LEGACY_NOTHING);
        EXPECT_EQ(sa, STATE(COPY_DEST));
        ASSERT_TRUE(de__d3d12_translate_enhanced(G, &b, &shape, &e));
        EXPECT_EQ(e.layout_before, LAYOUT(COPY_DEST));
        EXPECT_EQ(e.layout_after, LAYOUT(COPY_DEST));
        EXPECT_EQ(e.sync_after, SYNC(NONE));
        EXPECT_EQ(e.access_after, ACC(NO_ACCESS));
        /* Legacy uses the tracked state, not `before`: a buffer from an earlier submission has
         * decayed to COMMON, whatever the render graph last did with it. */
        b = buf_barrier(DE_ACCESS_COPY_DST, DE_ACCESS_COPY_SRC);
        EXPECT_EQ(de__d3d12_translate_legacy(G, &b, false, STATE(COMMON), &sb, &sa),
                  DE__D3D12_LEGACY_TRANSITION);
        EXPECT_EQ(sb, STATE(COMMON));
        EXPECT_EQ(sa, STATE(COPY_SOURCE));
    }

    /* ---- Subresource ranges. ---- */
    {
        de_gpu_barrier b = tex_barrier(DE_ACCESS_COPY_DST, DE_ACCESS_SHADER_READ);
        b.mip_first = 2;
        b.layer_first = 1;
        b.layer_count = 3;
        de__d3d12_enhanced e;
        ASSERT_TRUE(de__d3d12_translate_enhanced(G, &b, &shape, &e));
        EXPECT_EQ(e.mip_first, 2u);
        EXPECT_EQ(e.mip_count, 3u);
        EXPECT_EQ(e.layer_first, 1u);
        EXPECT_EQ(e.layer_count, 3u);
        /* A 3D texture is one layer, whatever the barrier says; depth-stencil has two planes. */
        const de__d3d12_texture_shape vol = {.mips = 4, .layers = 1, .planes = 2, .rt_ds = true};
        b.mip_first = 1;
        b.mip_count = 2;
        b.layer_first = 3;
        b.layer_count = 2;
        ASSERT_TRUE(de__d3d12_translate_enhanced(G, &b, &vol, &e));
        EXPECT_EQ(e.mip_first, 1u);
        EXPECT_EQ(e.mip_count, 2u);
        EXPECT_EQ(e.layer_first, 0u);
        EXPECT_EQ(e.layer_count, 1u);
        EXPECT_EQ(e.plane_count, 2u);
        EXPECT_EQ(de__d3d12_subresource(2, 3, 1, 5, 6), 2u + 3u * 5u + 30u);
        EXPECT_EQ(de__d3d12_subresource(0, 0, 0, 1, 1), 0u);
        /* Discard only on the first use of a render target or depth buffer. */
        const de__d3d12_texture_shape plain = {.mips = 1, .layers = 1, .planes = 1};
        b = tex_barrier(DE_ACCESS_NONE, DE_ACCESS_RENDER_TARGET);
        ASSERT_TRUE(de__d3d12_translate_enhanced(G, &b, &plain, &e));
        EXPECT_FALSE(e.discard);
        b = tex_barrier(DE_ACCESS_NONE, DE_ACCESS_DEPTH_WRITE);
        ASSERT_TRUE(de__d3d12_translate_enhanced(G, &b, &vol, &e));
        EXPECT_TRUE(e.discard);
        b = tex_barrier(DE_ACCESS_NONE, DE_ACCESS_SHADER_READ);
        ASSERT_TRUE(de__d3d12_translate_enhanced(G, &b, &shape, &e));
        EXPECT_FALSE(e.discard);
    }

    /* ---- Queue-ownership transfers go through COMMON. ---- */
    {
        /* cross_queue_timeline: a buffer written on the copy queue, read on the graphics queue. */
        de_gpu_barrier b = buf_barrier(DE_ACCESS_COPY_DST, DE_ACCESS_COPY_SRC);
        b.src_queue = DE_QUEUE_COPY;
        b.dst_queue = DE_QUEUE_GRAPHICS;
        EXPECT_EQ(de__d3d12_transfer_of(X, &b), DE__D3D12_TRANSFER_RELEASE);
        EXPECT_EQ(de__d3d12_transfer_of(G, &b), DE__D3D12_TRANSFER_ACQUIRE);
        EXPECT_EQ(de__d3d12_transfer_of(C, &b), DE__D3D12_TRANSFER_FOREIGN);
        de__d3d12_enhanced rel, acq, e;
        ASSERT_TRUE(de__d3d12_translate_enhanced(X, &b, NULL, &rel));
        EXPECT_EQ(rel.sync_before, SYNC(COPY));
        EXPECT_EQ(rel.access_before, ACC(COPY_DEST));
        EXPECT_EQ(rel.sync_after, SYNC(NONE)); /* release half */
        EXPECT_EQ(rel.access_after, ACC(NO_ACCESS));
        ASSERT_TRUE(de__d3d12_translate_enhanced(G, &b, NULL, &acq));
        EXPECT_EQ(acq.sync_before, SYNC(NONE)); /* acquire half */
        EXPECT_EQ(acq.access_before, ACC(NO_ACCESS));
        EXPECT_EQ(acq.sync_after, SYNC(COPY));
        EXPECT_EQ(acq.access_after, ACC(COPY_SOURCE));
        EXPECT_FALSE(de__d3d12_translate_enhanced(C, &b, NULL, &e)); /* a third queue */
        u32 sb = 0, sa = 0;
        EXPECT_EQ(de__d3d12_translate_legacy(X, &b, false, STATE(COPY_DEST), &sb, &sa),
                  DE__D3D12_LEGACY_TRANSITION);
        EXPECT_EQ(sb, STATE(COPY_DEST));
        EXPECT_EQ(sa, STATE(COMMON));
        /* The acquire starts from COMMON whatever this queue tracked (recorded first). */
        EXPECT_EQ(de__d3d12_translate_legacy(G, &b, false, STATE(RENDER_TARGET), &sb, &sa),
                  DE__D3D12_LEGACY_TRANSITION);
        EXPECT_EQ(sb, STATE(COMMON));
        EXPECT_EQ(sa, STATE(COPY_SOURCE));
        EXPECT_EQ(de__d3d12_translate_legacy(C, &b, false, STATE(COPY_DEST), &sb, &sa),
                  DE__D3D12_LEGACY_NOTHING);

        /* A texture from the graphics queue to the compute queue. */
        de_gpu_barrier t = tex_barrier(DE_ACCESS_SHADER_WRITE, DE_ACCESS_SHADER_READ);
        t.src_queue = DE_QUEUE_GRAPHICS;
        t.dst_queue = DE_QUEUE_COMPUTE;
        ASSERT_TRUE(de__d3d12_translate_enhanced(G, &t, &shape, &rel));
        EXPECT_EQ(rel.layout_before, LAYOUT(UNORDERED_ACCESS));
        EXPECT_EQ(rel.layout_after, LAYOUT(COMMON));
        EXPECT_EQ(rel.sync_after, SYNC(NONE));
        ASSERT_TRUE(de__d3d12_translate_enhanced(C, &t, &shape, &acq));
        EXPECT_EQ(acq.layout_before, LAYOUT(COMMON));
        EXPECT_EQ(acq.layout_after, LAYOUT(SHADER_RESOURCE));
        EXPECT_EQ(acq.sync_after, SYNC(COMPUTE_SHADING));
        EXPECT_EQ(acq.access_after, ACC(SHADER_RESOURCE));
        EXPECT_FALSE(acq.discard);
        /* To the copy queue: the texture is COMMON on both sides of the acquire. */
        t.dst_queue = DE_QUEUE_COPY;
        t.after = DE_ACCESS_COPY_SRC;
        ASSERT_TRUE(de__d3d12_translate_enhanced(X, &t, &shape, &acq));
        EXPECT_EQ(acq.layout_before, LAYOUT(COMMON));
        EXPECT_EQ(acq.layout_after, LAYOUT(COMMON));
        EXPECT_EQ(acq.access_after, ACC(COPY_SOURCE));
    }

    /* ---- Legacy states per queue type. ---- */
    EXPECT_EQ(de__d3d12_legacy_state_of(G, DE_ACCESS_SHADER_READ, false),
              STATE(VERTEX_AND_CONSTANT_BUFFER) | STATE(INDEX_BUFFER) |
                  STATE(NON_PIXEL_SHADER_RESOURCE) | STATE(PIXEL_SHADER_RESOURCE));
    EXPECT_EQ(de__d3d12_legacy_state_of(G, DE_ACCESS_SHADER_READ | DE_ACCESS_DEPTH_READ, true),
              STATE(DEPTH_READ) | STATE(NON_PIXEL_SHADER_RESOURCE) | STATE(PIXEL_SHADER_RESOURCE));
    EXPECT_EQ(de__d3d12_legacy_state_of(G, DE_ACCESS_INDIRECT, false), STATE(INDIRECT_ARGUMENT));
    EXPECT_EQ(de__d3d12_legacy_state_of(G, DE_ACCESS_PRESENT, true), STATE(COMMON));
    EXPECT_EQ(de__d3d12_legacy_state_of(G, DE_ACCESS_DEPTH_WRITE, true), STATE(DEPTH_WRITE));
    EXPECT_EQ(de__d3d12_legacy_state_of(X, DE_ACCESS_SHADER_READ, true), STATE(COMMON));
    EXPECT_EQ(de__d3d12_legacy_state_of(X, DE_ACCESS_COPY_SRC, true), STATE(COPY_SOURCE));
    EXPECT_EQ(de__d3d12_legacy_state_of(C, DE_ACCESS_RENDER_TARGET, true), STATE(COMMON));
    EXPECT_EQ(de__d3d12_legacy_state_of(C, DE_ACCESS_SHADER_READ, true),
              STATE(NON_PIXEL_SHADER_RESOURCE));
    EXPECT_EQ(de__d3d12_legacy_state_of(C, DE_ACCESS_SHADER_READ, false),
              STATE(VERTEX_AND_CONSTANT_BUFFER) | STATE(NON_PIXEL_SHADER_RESOURCE));
}
