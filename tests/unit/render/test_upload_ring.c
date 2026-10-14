/*
 * test_upload_ring.c: the staging ring and the per-frame copy list of the render asset types
 * (DENG-105, SPEC §11.4), on the null backend, through the uploader's internal API
 * (render/loaders/upload_ring.h) without an asset system. CTest names
 * render_loaders.upload_ring.<test>.
 *
 * The null backend completes a fence at submit; de__uploader_test_hold_fence makes the uploader
 * see it as late. Buffers here are GPU_ONLY targets of null devices (copies are no-ops), so these
 * tests check the bookkeeping: positions, records, frames, the byte cap, retirement and
 * allocations. The byte-exact copies are checked on real devices in test_render_loaders.c.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "de_test.h"

#include <dengine/de_base.h>
#include <dengine/de_debug.h>
#include <dengine/de_gpu.h>
#include <dengine/de_memory.h>

#include "loader_test_util.h"
#include "render/loaders/upload_ring.h"

#include <stdlib.h>
#include <string.h>

#define KIB ((u64)1 << 10)

typedef struct ring_fx {
    de_tracking_allocator track;     /* the uploader's */
    de_tracking_allocator dev_track; /* the device's */
    de_allocator alloc, dev_alloc;
    de_gpu_device *d;
    de_render_uploader u;
    bool inited;
    u8 *blob; /* source bytes of every upload */
    u64 blob_size;
} ring_fx;

static void ring_fx_setup(ring_fx *f) {
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_RENDER);
    f->dev_alloc = de_tracking_allocator_init(&f->dev_track, NULL, DE_MEM_RENDER);
    ASSERT_EQ(de_gpu_device_create(
                  &(de_gpu_device_desc){.backend = DE_GPU_NULL, .allocator = &f->dev_alloc}, &f->d),
              DE_OK);
    f->blob_size = 2u << 20;
    f->blob = (u8 *)malloc((size_t)f->blob_size);
    for (u64 i = 0; i < f->blob_size; ++i)
        f->blob[i] = lt_pattern(i, 0, 0);
}

static void ring_fx_teardown(ring_fx *f) {
    if (f->inited)
        de__uploader_fini(&f->u);
    if (f->d != NULL) {
        de_gpu_wait_idle(f->d);
        de_gpu_device_destroy(f->d);
    }
    free(f->blob);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).bytes_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->dev_track).bytes_live, 0u);
}

static void ring_init(ring_fx *f, u64 ring_bytes, u64 cap) {
    memset(&f->u, 0, sizeof f->u);
    f->u.alloc = &f->alloc;
    f->u.dev = f->d;
    ASSERT_EQ(de__uploader_init(&f->u, ring_bytes, cap), DE_OK);
    f->inited = true;
}

static de_gpu_buffer target(ring_fx *f, u64 size) {
    de_gpu_buffer b = de_gpu_buffer_create(
        f->d, &(de_gpu_buffer_desc){.size = size,
                                    .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_DST,
                                    .memory = DE_MEM_GPU_ONLY,
                                    .name = "ring test target"});
    EXPECT_TRUE(DE_HANDLE_VALID(b));
    return b;
}

/* An upload of `pieces` pieces of `piece` bytes into buffer `b`, submitted. */
static de__upload *submit_buffer(ring_fx *f, de_gpu_buffer b, u32 pieces, u64 piece) {
    de__upload *up = de__upload_new(&f->u, pieces);
    EXPECT_TRUE(up != NULL);
    up->targets[0].buffer = b;
    up->target_count = 1;
    for (u32 i = 0; i < pieces; ++i) {
        up->pieces[i].blob_off = (u64)i * piece;
        up->pieces[i].size = piece;
        up->pieces[i].dst_off = (u64)i * piece;
    }
    EXPECT_EQ(de__upload_layout(&f->u, up), DE_OK);
    EXPECT_EQ(de__upload_submit(&f->u, up, f->blob), DE_OK);
    return up;
}

/* Flushes until `up` is done (at most `max` flushes); the number of flushes. */
static u32 flush_until_done(ring_fx *f, de__upload *up, u32 max) {
    for (u32 n = 1; n <= max; ++n) {
        de_render_uploader_flush(&f->u);
        if (de__upload_poll(&f->u, up) == DE_OK)
            return n;
    }
    return 0;
}

/* ---- upload_ring.wrap -------------------------------------------------------------------------
 *
 * 20 KiB uploads through a 64 KiB ring, one per frame: each reservation goes where the last one
 * ended until the end of the ring, then wraps to offset 0 (never splitting itself); retired space
 * is reused, nothing spills, and the ring is empty after the last fence. */
TEST_F(upload_ring, wrap, ring_fx) {
    ring_init(f, 64 * KIB, 0);
    de_gpu_buffer b = target(f, 20 * KIB);
    u64 prev_off = 0;
    u32 wraps = 0;
    for (u32 i = 0; i < 12; ++i) {
        de__upload *up = submit_buffer(f, b, 1, 20 * KIB);
        ASSERT_TRUE(up->in_ring);
        if (i > 0 && up->ring_off < prev_off)
            wraps++;
        EXPECT_TRUE(up->ring_off + 20 * KIB <= 64 * KIB); /* one contiguous range */
        prev_off = up->ring_off;
        EXPECT_LE(de_render_uploader_stats(&f->u).ring_used, 64 * KIB);
        EXPECT_EQ(flush_until_done(f, up, 4), 1u);
        de__upload_release(&f->u, up);
    }
    EXPECT_GE(wraps, 3u);
    de_render_upload_stats st = de_render_uploader_stats(&f->u);
    EXPECT_EQ(st.spilled, 0u);
    EXPECT_EQ(st.frames, 12u);
    EXPECT_EQ(st.bytes, 12u * 20u * KIB);
    de_render_uploader_flush(&f->u); /* retires the last frame */
    st = de_render_uploader_stats(&f->u);
    EXPECT_EQ(st.ring_used, 0u);
    EXPECT_EQ(st.ring_records, 0u);
    /* An empty ring starts again at offset 0. */
    de__upload *up = submit_buffer(f, b, 1, 20 * KIB);
    EXPECT_EQ(up->ring_off, 0u);
    EXPECT_EQ(flush_until_done(f, up, 4), 1u);
    de__upload_release(&f->u, up);
    de_gpu_buffer_destroy(f->d, b);
}

/* ---- upload_ring.split_large ------------------------------------------------------------------
 *
 * An upload of 10 x 32 KiB through a 64 KiB ring cannot be staged whole: it spills to the heap,
 * and the flush moves two pieces per frame into the ring as the previous frame retires. An upload
 * queued behind the spill spills too and finishes after it (order). A single copy larger than the
 * ring is refused. */
TEST_F(upload_ring, split_large, ring_fx) {
    ring_init(f, 64 * KIB, 0);
    de_gpu_buffer big = target(f, 320 * KIB), small = target(f, 8 * KIB);
    de__upload *a = submit_buffer(f, big, 10, 32 * KIB);
    EXPECT_FALSE(a->in_ring);
    de__upload *b = submit_buffer(f, small, 1, 8 * KIB);
    EXPECT_FALSE(b->in_ring); /* behind a spill */
    EXPECT_EQ(de_render_uploader_stats(&f->u).spilled, 2u);
    u32 frames = 0;
    bool a_done = false, b_before_a = false;
    while (frames < 20 && de__upload_poll(&f->u, b) != DE_OK) {
        de_render_uploader_flush(&f->u);
        frames++;
        de_render_upload_stats st = de_render_uploader_stats(&f->u);
        EXPECT_LE(st.last_frame_bytes, 64 * KIB);
        EXPECT_LE(st.ring_used, 64 * KIB);
        const bool ad = de__upload_poll(&f->u, a) == DE_OK;
        if (!ad && de__upload_poll(&f->u, b) == DE_OK)
            b_before_a = true;
        a_done = a_done || ad;
    }
    EXPECT_TRUE(a_done);
    EXPECT_FALSE(b_before_a);
    EXPECT_EQ(a->next_piece, 10u);
    de_render_upload_stats st = de_render_uploader_stats(&f->u);
    EXPECT_EQ(st.bytes, 328 * KIB);
    EXPECT_GE(st.frames, 5u); /* 320 KiB through 64 KiB */
    EXPECT_LE(frames, 7u);
    de__upload_release(&f->u, a);
    de__upload_release(&f->u, b);

    /* One copy that can never fit. */
    de__upload *up = de__upload_new(&f->u, 1);
    ASSERT_TRUE(up != NULL);
    up->targets[0].buffer = big;
    up->target_count = 1;
    up->pieces[0].size = 65 * KIB;
    EXPECT_EQ(de__upload_layout(&f->u, up), DE_ERR_UNSUPPORTED);
    de__upload_release(&f->u, up);
    de_gpu_buffer_destroy(f->d, big);
    de_gpu_buffer_destroy(f->d, small);
}

/* ---- upload_ring.fence_retire -----------------------------------------------------------------
 *
 * Ring space is reused only after the copy fence passed the frame that read it: while the GPU is
 * "late" (held at 0) the first upload stays BUSY and its 40 KiB stay reserved, so a second 40 KiB
 * upload cannot be staged nor recorded; once the fence moves, the next flush retires the space and
 * records the second one. */
TEST_F(upload_ring, fence_retire, ring_fx) {
    ring_init(f, 64 * KIB, 0);
    de_gpu_buffer b = target(f, 40 * KIB);
    de__uploader_test_hold_fence(&f->u, 0);
    de__upload *a = submit_buffer(f, b, 1, 40 * KIB);
    EXPECT_TRUE(a->in_ring);
    de_render_uploader_flush(&f->u);
    EXPECT_EQ(de_render_uploader_stats(&f->u).submitted, 1u);
    EXPECT_EQ(de__upload_poll(&f->u, a), DE_ERR_BUSY);
    de__upload *c = submit_buffer(f, b, 1, 40 * KIB);
    EXPECT_FALSE(c->in_ring); /* no room: spilled */
    for (u32 i = 0; i < 3; ++i) {
        de_render_uploader_flush(&f->u);
        de_render_upload_stats st = de_render_uploader_stats(&f->u);
        EXPECT_EQ(st.frames, 1u); /* nothing recorded: no space retired */
        EXPECT_EQ(st.ring_used, 40 * KIB);
        EXPECT_EQ(st.completed, 0u);
        EXPECT_EQ(de__upload_poll(&f->u, a), DE_ERR_BUSY);
        EXPECT_EQ(de__upload_poll(&f->u, c), DE_ERR_BUSY);
    }
    de__uploader_test_hold_fence(&f->u, UINT64_MAX);
    EXPECT_EQ(de__upload_poll(&f->u, a), DE_OK);
    de_render_uploader_flush(&f->u); /* retires a's 40 KiB, records c */
    de_render_upload_stats st = de_render_uploader_stats(&f->u);
    EXPECT_EQ(st.frames, 2u);
    EXPECT_EQ(st.ring_used, 40 * KIB);
    EXPECT_EQ(de__upload_poll(&f->u, c), DE_OK);
    de__upload_release(&f->u, a);
    de__upload_release(&f->u, c);
    de_gpu_buffer_destroy(f->d, b);
}

/* ---- upload_ring.frame_cap --------------------------------------------------------------------
 *
 * One flush copies at most the cap: 10 x 16 KiB with a 64 KiB cap take three frames (4, 4, 2
 * pieces); a single piece above the cap still goes, alone. The cvar asset.upload_mb_per_frame sets
 * the cap when the desc does not. */
TEST_F(upload_ring, frame_cap, ring_fx) {
    ring_init(f, 1024 * KIB, 64 * KIB);
    de_gpu_buffer b = target(f, 1024 * KIB);
    de__upload *ups[10];
    for (u32 i = 0; i < 10; ++i)
        ups[i] = submit_buffer(f, b, 1, 16 * KIB);
    const u32 want[3] = {4, 4, 2};
    for (u32 fr = 0; fr < 3; ++fr) {
        de_render_uploader_flush(&f->u);
        de_render_upload_stats st = de_render_uploader_stats(&f->u);
        EXPECT_EQ(st.last_frame_pieces, want[fr]);
        EXPECT_LE(st.last_frame_bytes, 64 * KIB);
    }
    for (u32 i = 0; i < 10; ++i) {
        EXPECT_EQ(de__upload_poll(&f->u, ups[i]), DE_OK);
        de__upload_release(&f->u, ups[i]);
    }
    de__upload *big = submit_buffer(f, b, 1, 100 * KIB);
    de_render_uploader_flush(&f->u);
    EXPECT_EQ(de_render_uploader_stats(&f->u).last_frame_bytes, 100 * KIB);
    EXPECT_EQ(de__upload_poll(&f->u, big), DE_OK);
    de__upload_release(&f->u, big);

    /* The cvar: 1 MiB per frame. */
    f->u.frame_cap = 0;
    de_cvar cv = de_cvar_find("asset.upload_mb_per_frame");
    ASSERT_TRUE(DE_HANDLE_VALID(cv));
    EXPECT_EQ(de_cvar_int(cv), 64);
    ASSERT_EQ(de_cvar_set(cv, "1"), DE_OK);
    de__upload *m[3];
    for (u32 i = 0; i < 3; ++i)
        m[i] = submit_buffer(f, b, 1, 600 * KIB);
    for (u32 fr = 0; fr < 3; ++fr) {
        de_render_uploader_flush(&f->u);
        EXPECT_EQ(de_render_uploader_stats(&f->u).last_frame_pieces, 1u);
    }
    ASSERT_EQ(de_cvar_set(cv, "64"), DE_OK);
    for (u32 i = 0; i < 3; ++i) {
        EXPECT_EQ(de__upload_poll(&f->u, m[i]), DE_OK);
        de__upload_release(&f->u, m[i]);
    }
    de_gpu_buffer_destroy(f->d, b);
}

/* ---- upload_ring.no_heap_after_warmup ---------------------------------------------------------
 *
 * The flush allocates nothing: ring records and release barriers are fixed arrays sized at init,
 * and the scratch is in the uploader. After a warm-up round (staged and spilled uploads), 50
 * frames that copy staged and spilled uploads and record the acquire barriers make no allocation
 * through the uploader's allocator. */
TEST_F(upload_ring, no_heap_after_warmup, ring_fx) {
    ring_init(f, 128 * KIB, 0);
    de_gpu_buffer b = target(f, 256 * KIB);
    for (u32 round = 0; round < 2; ++round) {
        de__upload *ups[6];
        for (u32 i = 0; i < 6; ++i) /* the last ones spill (6 x 40 KiB > 128 KiB) */
            ups[i] = submit_buffer(f, b, 4, 10 * KIB);
        const de_tracking_stats before = de_tracking_allocator_stats(&f->track);
        for (u32 fr = 0; fr < 50; ++fr) {
            de_render_uploader_flush(&f->u);
            de_gpu_cmd *c = de_gpu_cmd_begin(f->d, DE_QUEUE_GRAPHICS, "acquire");
            ASSERT_TRUE(c != NULL);
            de_gpu_wait w;
            (void)de_render_uploader_acquire(&f->u, c, &w);
            ASSERT_EQ(de_gpu_queue_submit(f->d, DE_QUEUE_GRAPHICS,
                                          &(de_gpu_submit){.cmds = &c, .cmd_count = 1}),
                      DE_OK);
        }
        const de_tracking_stats after = de_tracking_allocator_stats(&f->track);
        if (round == 1) {
            EXPECT_EQ(after.alloc_count, before.alloc_count);
            EXPECT_EQ(after.realloc_count, before.realloc_count);
        }
        EXPECT_GE(de_render_uploader_stats(&f->u).spilled, 1u);
        for (u32 i = 0; i < 6; ++i) {
            EXPECT_EQ(de__upload_poll(&f->u, ups[i]), DE_OK);
            de__upload_release(&f->u, ups[i]);
        }
    }
    de_gpu_buffer_destroy(f->d, b);
}
