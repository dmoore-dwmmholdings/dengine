/* test_gpu_heap.c — device memory heaps on the null backend (src/gpu/memory/gpu_heap.c,
 * DENG-282): growth and spare release, dedicated heaps, statistics, transient regions with
 * placed resources. CTest names: gpu.gpu_heap.<test>. */
#include "de_test.h"

#include <dengine/de_gpu.h>
#include <dengine/de_platform.h>

#include "gpu/gpu_backend.h"

#include <string.h>

#define KIB ((u64)1 << 10)
#define MIB ((u64)1 << 20)

static const char *errmsg(void) {
    const de_error *e = de_error_last();
    return e ? e->message : "";
}

/* A null device on a tracking allocator, so every test also checks for leaks. */
typedef struct heap_fx {
    de_tracking_allocator tracker;
    de_allocator alloc;
    de_gpu_device *d;
} heap_fx;

static void heap_fx_setup(heap_fx *f) {
    f->alloc = de_tracking_allocator_init(&f->tracker, NULL, DE_MEM_RENDER);
    de_gpu_device_desc desc = {.backend = DE_GPU_NULL, .allocator = &f->alloc};
    ASSERT_EQ(de_gpu_device_create(&desc, &f->d), DE_OK);
}
static void heap_fx_teardown(heap_fx *f) {
    de_gpu_device_destroy(f->d);
    de_tracking_stats st = de_tracking_allocator_stats(&f->tracker);
    EXPECT_EQ(st.blocks_live, 0u);
    EXPECT_EQ(st.bytes_live, 0u);
}

static de__gpu_mem_stats class_stats(de_gpu_device *d, u32 cls) {
    de__gpu_mem_stats st;
    memset(&st, 0, sizeof st);
    EXPECT_EQ(de__gpu_mem_class_stats(d, cls, &st), DE_OK);
    return st;
}

static u32 null_live_heaps(de_gpu_device *d) {
    u32 live;
    de__gpu_null_heap_counts(d, &live, NULL, NULL);
    return live;
}

static u64 null_heap_bytes(de_gpu_device *d) {
    u64 bytes;
    de__gpu_null_heap_counts(d, NULL, &bytes, NULL);
    return bytes;
}

static u32 null_created_heaps(de_gpu_device *d) {
    u32 created;
    de__gpu_null_heap_counts(d, NULL, NULL, &created);
    return created;
}

/* ---------- Classes ---------- */

TEST_F(gpu_heap, classes, heap_fx) {
    ASSERT_EQ(de__gpu_mem_class_count(f->d), 3u);
    const de__gpu_mem_class_desc *c = de__gpu_mem_class_get(f->d, DE__GPU_MEM_CLASS_UPLOAD);
    ASSERT_TRUE(c != NULL);
    EXPECT_STREQ(c->name, "upload");
    EXPECT_EQ(c->memory, DE_MEM_UPLOAD);
    EXPECT_EQ(c->block_size, DE__GPU_HEAP_DEFAULT_BLOCK_SIZE);
    EXPECT_EQ(c->dedicated_threshold, DE__GPU_HEAP_DEFAULT_DEDICATED_SIZE);
    EXPECT_TRUE(de__gpu_mem_class_get(f->d, 3) == NULL);

    /* A backend sub-class (e.g. D3D12 tier-1 render targets). */
    de__gpu_mem_class_desc rt = {
        .memory = DE_MEM_GPU_ONLY, .name = "render_targets", .block_size = 1000, .backend_bits = 7};
    u32 cls = 0;
    ASSERT_EQ(de__gpu_mem_add_class(f->d, &rt, &cls), DE_OK);
    EXPECT_EQ(cls, 3u);
    c = de__gpu_mem_class_get(f->d, cls);
    EXPECT_STREQ(c->name, "render_targets");
    EXPECT_EQ(c->block_size, (u64)1024); /* rounded to 256 */
    EXPECT_EQ(c->backend_bits, 7u);
    de__gpu_mem_class_desc unnamed = {.memory = DE_MEM_READBACK};
    ASSERT_EQ(de__gpu_mem_add_class(f->d, &unnamed, &cls), DE_OK);
    EXPECT_STREQ(de__gpu_mem_class_get(f->d, cls)->name, "readback.4");
    while (de__gpu_mem_class_count(f->d) < DE__GPU_MEM_CLASS_MAX)
        ASSERT_EQ(de__gpu_mem_add_class(f->d, &unnamed, &cls), DE_OK);
    EXPECT_EQ(de__gpu_mem_add_class(f->d, &unnamed, &cls), DE_ERR_OUT_OF_MEMORY);

    /* Configure a class before it has heaps; refused afterwards. */
    de__gpu_mem_class_desc small = {.memory = DE_MEM_GPU_ONLY, .block_size = MIB};
    ASSERT_EQ(de__gpu_mem_configure_class(f->d, DE__GPU_MEM_CLASS_GPU_ONLY, &small), DE_OK);
    EXPECT_STREQ(de__gpu_mem_class_get(f->d, 0)->name, "gpu_only");
    de__gpu_alloc a;
    ASSERT_EQ(de__gpu_mem_alloc(f->d, 0, 1000, 0, DE_MEM_RENDER, &a), DE_OK);
    EXPECT_EQ(de__gpu_mem_configure_class(f->d, 0, &small), DE_ERR_INVALID_ARGUMENT);
    de__gpu_mem_free(f->d, &a);

    /* Argument errors. */
    EXPECT_EQ(de__gpu_mem_alloc(f->d, 99, 256, 0, 0, &a), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de__gpu_mem_alloc(f->d, 0, 0, 0, 0, &a), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de__gpu_mem_alloc(f->d, 0, 256, 768, 0, &a), DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(strstr(errmsg(), "power of two") != NULL);
    EXPECT_TRUE(a.heap == NULL);
    de__gpu_mem_free(f->d, &a); /* zero record: ignored */
}

/* ---------- Growth and spare release ---------- */

TEST_F(gpu_heap, grow_and_release, heap_fx) {
    const u32 G = DE__GPU_MEM_CLASS_GPU_ONLY;
    /* 48 MiB each (below the 64 MiB dedicated threshold): five fit one 256 MiB heap. */
    enum { N = 16 };
    de__gpu_alloc a[N];
    for (u32 i = 0; i < N; ++i) {
        ASSERT_EQ(de__gpu_mem_alloc(f->d, G, 48 * MIB, 64 * KIB, DE_MEM_RENDER, &a[i]), DE_OK);
        EXPECT_FALSE(a[i].dedicated);
        EXPECT_EQ(a[i].offset % (64 * KIB), (u64)0);
        EXPECT_EQ(a[i].mem_class, (u16)G);
        EXPECT_EQ(a[i].heap_index, a[i].heap->index);
    }
    de__gpu_mem_stats st = class_stats(f->d, G);
    EXPECT_EQ(st.heaps, 4u); /* ceil(16 / 5) */
    EXPECT_EQ(st.dedicated_heaps, 0u);
    EXPECT_EQ(st.reserved, 4 * DE__GPU_HEAP_DEFAULT_BLOCK_SIZE);
    EXPECT_EQ(st.used, N * 48 * MIB);
    EXPECT_EQ(null_live_heaps(f->d), 4u);
    EXPECT_EQ(null_heap_bytes(f->d), 4 * DE__GPU_HEAP_DEFAULT_BLOCK_SIZE);

    /* Free everything: one heap stays as the spare, the others go back to the backend. */
    for (u32 i = 0; i < N; ++i) {
        de__gpu_mem_free(f->d, &a[i]);
        EXPECT_TRUE(a[i].heap == NULL);
        st = class_stats(f->d, G);
        EXPECT_LE(st.empty_heaps, 1u);
    }
    st = class_stats(f->d, G);
    EXPECT_EQ(st.heaps, 1u);
    EXPECT_EQ(st.empty_heaps, 1u);
    EXPECT_EQ(st.used, (u64)0);
    EXPECT_EQ(st.free_blocks, 1u); /* the spare is one free block again */
    EXPECT_EQ(st.largest_free, DE__GPU_HEAP_DEFAULT_BLOCK_SIZE);
    EXPECT_EQ(null_live_heaps(f->d), 1u);

    /* The spare is reused before a new heap is created. */
    const u32 created = null_created_heaps(f->d);
    de__gpu_alloc b;
    ASSERT_EQ(de__gpu_mem_alloc(f->d, G, MIB, 0, DE_MEM_RENDER, &b), DE_OK);
    EXPECT_EQ(null_created_heaps(f->d), created);
    st = class_stats(f->d, G);
    EXPECT_EQ(st.empty_heaps, 0u);
    de__gpu_mem_free(f->d, &b);

    /* Classes are independent: each keeps its own spare. */
    ASSERT_EQ(de__gpu_mem_alloc(f->d, DE__GPU_MEM_CLASS_UPLOAD, MIB, 0, DE_MEM_RENDER, &b), DE_OK);
    de__gpu_mem_free(f->d, &b);
    EXPECT_EQ(class_stats(f->d, DE__GPU_MEM_CLASS_UPLOAD).empty_heaps, 1u);
    EXPECT_EQ(class_stats(f->d, G).empty_heaps, 1u);
    EXPECT_EQ(null_live_heaps(f->d), 2u);

    /* A class with small blocks grows a heap large enough for a bigger request. */
    de__gpu_mem_class_desc small = {.memory = DE_MEM_GPU_ONLY, .block_size = MIB};
    u32 cls;
    ASSERT_EQ(de__gpu_mem_add_class(f->d, &small, &cls), DE_OK);
    ASSERT_EQ(de__gpu_mem_alloc(f->d, cls, 5 * MIB, 64 * KIB, DE_MEM_RENDER, &b), DE_OK);
    EXPECT_FALSE(b.dedicated);
    EXPECT_EQ(b.heap->size, 5 * MIB);
    de__gpu_alloc c;
    ASSERT_EQ(de__gpu_mem_alloc(f->d, cls, 100 * KIB, 0, DE_MEM_RENDER, &c), DE_OK);
    EXPECT_EQ(class_stats(f->d, cls).heaps, 2u); /* the 5 MiB heap is exactly full */
    de__gpu_mem_free(f->d, &b);
    de__gpu_mem_free(f->d, &c);
    EXPECT_EQ(class_stats(f->d, cls).heaps, 1u);
}

/* ---------- Dedicated heaps ---------- */

TEST_F(gpu_heap, dedicated, heap_fx) {
    const u32 G = DE__GPU_MEM_CLASS_GPU_ONLY;
    const u32 heaps0 = null_live_heaps(f->d);
    /* Exactly the threshold: sub-allocated. One byte more: a dedicated heap. */
    de__gpu_alloc at, above;
    ASSERT_EQ(de__gpu_mem_alloc(f->d, G, 64 * MIB, 0, DE_MEM_RENDER, &at), DE_OK);
    EXPECT_FALSE(at.dedicated);
    ASSERT_EQ(de__gpu_mem_alloc(f->d, G, 64 * MIB + 1, 0, DE_MEM_RENDER, &above), DE_OK);
    EXPECT_TRUE(above.dedicated);
    EXPECT_EQ(above.offset, (u64)0);
    EXPECT_EQ(above.size, 64 * MIB + 256);
    EXPECT_TRUE(above.block == NULL);
    EXPECT_TRUE(above.heap->dedicated);
    EXPECT_EQ(above.heap->size, 64 * MIB + 256);
    de__gpu_mem_stats st = class_stats(f->d, G);
    EXPECT_EQ(st.heaps, 1u);
    EXPECT_EQ(st.dedicated_heaps, 1u);
    EXPECT_EQ(st.reserved, DE__GPU_HEAP_DEFAULT_BLOCK_SIZE + 64 * MIB + 256);
    EXPECT_EQ(null_live_heaps(f->d), heaps0 + 2);

    /* 300 MiB (larger than a block heap): dedicated, sized to the request. */
    de__gpu_alloc big;
    ASSERT_EQ(de__gpu_mem_alloc(f->d, G, 300 * MIB, 64 * KIB, DE_MEM_ASSET, &big), DE_OK);
    EXPECT_TRUE(big.dedicated);
    EXPECT_EQ(null_heap_bytes(f->d), DE__GPU_HEAP_DEFAULT_BLOCK_SIZE + 364 * MIB + 256);

    /* Freeing a dedicated allocation releases its heap at once (it is never a spare). */
    de__gpu_mem_free(f->d, &above);
    de__gpu_mem_free(f->d, &big);
    st = class_stats(f->d, G);
    EXPECT_EQ(st.dedicated_heaps, 0u);
    EXPECT_EQ(null_live_heaps(f->d), heaps0 + 1);
    de__gpu_mem_free(f->d, &at);
    st = class_stats(f->d, G);
    EXPECT_EQ(st.heaps, 1u);
    EXPECT_EQ(st.empty_heaps, 1u);
    EXPECT_EQ(st.reserved, DE__GPU_HEAP_DEFAULT_BLOCK_SIZE);

    /* A class with a lower threshold. */
    de__gpu_mem_class_desc low = {.memory = DE_MEM_UPLOAD, .dedicated_threshold = MIB};
    u32 cls;
    ASSERT_EQ(de__gpu_mem_add_class(f->d, &low, &cls), DE_OK);
    de__gpu_alloc u;
    ASSERT_EQ(de__gpu_mem_alloc(f->d, cls, 2 * MIB, 0, DE_MEM_RENDER, &u), DE_OK);
    EXPECT_TRUE(u.dedicated);
    EXPECT_EQ(de__gpu_mem_kind_reserved(f->d, DE_MEM_UPLOAD), 2 * MIB);
    de__gpu_mem_free(f->d, &u);
    EXPECT_EQ(de__gpu_mem_kind_reserved(f->d, DE_MEM_UPLOAD), (u64)0);
}

/* ---------- Statistics ---------- */

typedef struct shadow_alloc {
    de__gpu_alloc a;
    u64 size; /* requested */
} shadow_alloc;

TEST_F(gpu_heap, stats, heap_fx) {
    /* A fourth class with 8 MiB blocks and a 4 MiB threshold, so heaps come and go often. */
    de__gpu_mem_class_desc sub = {.memory = DE_MEM_GPU_ONLY,
                                  .name = "sub",
                                  .block_size = 8 * MIB,
                                  .dedicated_threshold = 4 * MIB};
    u32 sub_cls;
    ASSERT_EQ(de__gpu_mem_add_class(f->d, &sub, &sub_cls), DE_OK);
    const u32 classes = de__gpu_mem_class_count(f->d);
    const u32 tags[3] = {DE_MEM_RENDER, DE_MEM_ASSET, DE_MEM_UI};

    enum { SLOTS = 512, OPS = 40000 };
    static shadow_alloc slot[SLOTS];
    memset(slot, 0, sizeof slot);
    u64 used_cls[DE__GPU_MEM_CLASS_MAX] = {0};
    u32 count_cls[DE__GPU_MEM_CLASS_MAX] = {0};
    u64 used_tag[DE_MEM_TAG_MAX] = {0};
    u64 rs = 99;
    for (u32 op = 0; op < OPS; ++op) {
        rs = rs * 6364136223846793005ull + 1442695040888963407ull;
        shadow_alloc *s = &slot[(rs >> 33) % SLOTS];
        if (s->a.heap) {
            used_cls[s->a.mem_class] -= s->a.size;
            --count_cls[s->a.mem_class];
            used_tag[s->a.tag] -= s->a.size;
            de__gpu_mem_free(f->d, &s->a);
        } else {
            const u32 cls = (u32)((rs >> 13) % classes);
            const u64 size = 256 + ((rs >> 20) % (6 * MIB));
            const u64 align = (u64)1 << (8 + (rs >> 45) % 9); /* 256 B .. 64 KiB */
            const u32 tag = tags[(rs >> 50) % 3];
            ASSERT_EQ(de__gpu_mem_alloc(f->d, cls, size, align, tag, &s->a), DE_OK);
            EXPECT_EQ(s->a.offset % align, (u64)0);
            EXPECT_EQ(s->a.size, (size + 255) & ~(u64)255);
            used_cls[cls] += s->a.size;
            ++count_cls[cls];
            used_tag[tag] += s->a.size;
            s->size = size;
        }
        /* Every class's stats after every operation. */
        u64 reserved_all = 0, used_kind[3] = {0}, reserved_kind[3] = {0};
        for (u32 c = 0; c < classes; ++c) {
            de__gpu_mem_stats st = class_stats(f->d, c);
            ASSERT_EQ(st.used, used_cls[c]);
            ASSERT_EQ(st.allocations, count_cls[c]);
            ASSERT_EQ(st.free, st.reserved - st.used);
            ASSERT_LE(st.empty_heaps, 1u);
            ASSERT_LE(st.largest_free, st.free);
            reserved_all += st.reserved;
            const de_gpu_memory k = de__gpu_mem_class_get(f->d, c)->memory;
            used_kind[k] += st.used;
            reserved_kind[k] += st.reserved;
        }
        ASSERT_EQ(null_heap_bytes(f->d), reserved_all);
        for (u32 k = 0; k < 3; ++k) {
            ASSERT_EQ(de__gpu_mem_kind_used(f->d, (de_gpu_memory)k), used_kind[k]);
            ASSERT_EQ(de__gpu_mem_kind_reserved(f->d, (de_gpu_memory)k), reserved_kind[k]);
        }
        for (u32 t = 0; t < 3; ++t)
            ASSERT_EQ(de__gpu_mem_tag_used(f->d, tags[t]), used_tag[tags[t]]);
    }
    EXPECT_GT(class_stats(f->d, sub_cls).reserved, (u64)0);
    for (u32 i = 0; i < SLOTS; ++i)
        de__gpu_mem_free(f->d, &slot[i].a);
    for (u32 c = 0; c < classes; ++c) {
        de__gpu_mem_stats st = class_stats(f->d, c);
        EXPECT_EQ(st.used, (u64)0);
        EXPECT_EQ(st.allocations, 0u);
        EXPECT_LE(st.heaps, 1u);
        EXPECT_EQ(st.dedicated_heaps, 0u);
        EXPECT_EQ(st.free_blocks, st.heaps); /* fully coalesced */
        EXPECT_TRUE(st.fragmentation == 0.0f);
    }
    for (u32 t = 0; t < 3; ++t)
        EXPECT_EQ(de__gpu_mem_tag_used(f->d, tags[t]), (u64)0);
    EXPECT_EQ(de__gpu_mem_tag_used(f->d, DE_MEM_TAG_MAX), (u64)0);
}

/* ---------- Threads ---------- */

typedef struct mt_ctx {
    de_gpu_device *d;
    u32 seed;
    u32 failures;
} mt_ctx;

static i32 mt_main(void *user) {
    mt_ctx *c = (mt_ctx *)user;
    de__gpu_alloc a[64];
    memset(a, 0, sizeof a);
    u64 rs = c->seed;
    for (u32 i = 0; i < 20000; ++i) {
        rs = rs * 6364136223846793005ull + 1442695040888963407ull;
        de__gpu_alloc *s = &a[(rs >> 33) % 64];
        if (s->heap) {
            de__gpu_mem_free(c->d, s);
        } else if (de__gpu_mem_alloc(c->d, (u32)((rs >> 20) % 3), 256 + (rs >> 40) % (2 * MIB), 0,
                                     DE_MEM_RENDER, s) != DE_OK) {
            ++c->failures;
        }
    }
    for (u32 i = 0; i < 64; ++i)
        de__gpu_mem_free(c->d, &a[i]);
    return 0;
}

TEST_F(gpu_heap, threads, heap_fx) {
    enum { T = 8 };
    mt_ctx ctx[T];
    de_thread *threads[T];
    for (u32 i = 0; i < T; ++i) {
        ctx[i] = (mt_ctx){.d = f->d, .seed = 1000 + i};
        ASSERT_EQ(
            de_thread_create(mt_main, &ctx[i], "gpu-heap-mt", DE_THREAD_NORMAL, 0, &threads[i]),
            DE_OK);
    }
    for (u32 i = 0; i < T; ++i) {
        de_thread_join(threads[i]);
        EXPECT_EQ(ctx[i].failures, 0u);
    }
    for (u32 c = 0; c < 3; ++c) {
        de__gpu_mem_stats st = class_stats(f->d, c);
        EXPECT_EQ(st.used, (u64)0);
        EXPECT_LE(st.heaps, 1u);
    }
}

/* ---------- Transient regions ---------- */

TEST_F(gpu_heap, transient_place_null, heap_fx) {
    const u32 G = DE__GPU_MEM_CLASS_GPU_ONLY;
    de__gpu_transient_region *r = NULL;
    ASSERT_EQ(de__gpu_transient_region_create(f->d, G, 48 * MIB, &r), DE_OK);
    ASSERT_TRUE(r != NULL);
    EXPECT_EQ(de__gpu_transient_region_size(r), 48 * MIB);
    EXPECT_EQ(de__gpu_transient_region_class(r), G);
    const de__gpu_alloc *ra = de__gpu_transient_region_alloc(r);
    EXPECT_EQ(ra->offset % DE__GPU_TRANSIENT_REGION_ALIGN, (u64)0);
    EXPECT_EQ(class_stats(f->d, G).used, 48 * MIB);

    /* Requirements of a 1080p RGBA16F target. */
    de_gpu_texture_desc td = {.format = DE_FMT_RGBA16_FLOAT,
                              .width = 1920,
                              .height = 1080,
                              .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED,
                              .name = "hdr"};
    de__gpu_mem_req req;
    ASSERT_EQ(de__gpu_texture_requirements(f->d, &td, &req), DE_OK);
    EXPECT_EQ(req.align, 64 * KIB);
    EXPECT_GE(req.size, (u64)1920 * 1080 * 8);
    EXPECT_EQ(req.class_mask, 1u << G);

    /* Two textures aliasing the same memory, and a buffer after them. */
    de_gpu_texture t0 = de__gpu_place_transient_texture(r, 0, &td);
    ASSERT_TRUE(DE_HANDLE_VALID(t0));
    td.name = "bloom";
    de_gpu_texture t1 = de__gpu_place_transient_texture(r, 0, &td);
    ASSERT_TRUE(DE_HANDLE_VALID(t1));
    de_gpu_buffer_desc bd = {.size = 1000, .usage = DE_USAGE_STORAGE, .name = "scratch"};
    const u64 boff = req.size;
    de_gpu_buffer b0 = de__gpu_place_transient_buffer(r, boff, &bd);
    ASSERT_TRUE(DE_HANDLE_VALID(b0));
    EXPECT_EQ(de__gpu_transient_region_placed(r), 3u);
    EXPECT_NE(de_gpu_buffer_bindless(f->d, b0), DE_GPU_BINDLESS_INVALID);
    EXPECT_NE(de_gpu_texture_bindless(f->d, t1, 0, false), DE_GPU_BINDLESS_INVALID);
    EXPECT_TRUE(de__gpu_get_texture(f->d, t0)->transient);

    /* The null backend recorded the heap and offset of each placement. */
    de__gpu_heap *heap = NULL;
    u64 off = 0;
    ASSERT_TRUE(de__gpu_null_placement(f->d, DE__GPU_TEXTURE, t0.id, &heap, &off));
    EXPECT_TRUE(heap == ra->heap);
    EXPECT_EQ(off, ra->offset);
    ASSERT_TRUE(de__gpu_null_placement(f->d, DE__GPU_TEXTURE, t1.id, &heap, &off));
    EXPECT_EQ(off, ra->offset);
    ASSERT_TRUE(de__gpu_null_placement(f->d, DE__GPU_BUFFER, b0.id, &heap, &off));
    EXPECT_TRUE(heap == ra->heap);
    EXPECT_EQ(off, ra->offset + boff);
    /* An ordinary resource is not placed. */
    de_gpu_buffer plain = de_gpu_buffer_create(f->d, &bd);
    EXPECT_FALSE(de__gpu_null_placement(f->d, DE__GPU_BUFFER, plain.id, &heap, &off));
    de_gpu_buffer_destroy(f->d, plain);

    /* Bad placements: alignment, range, class, desc. */
    EXPECT_FALSE(DE_HANDLE_VALID(de__gpu_place_transient_texture(r, 256, &td)));
    EXPECT_TRUE(strstr(errmsg(), "not aligned to 65536 bytes") != NULL);
    EXPECT_FALSE(DE_HANDLE_VALID(de__gpu_place_transient_texture(r, 48 * MIB - 64 * KIB, &td)));
    EXPECT_TRUE(strstr(errmsg(), "end past the region") != NULL);
    EXPECT_FALSE(DE_HANDLE_VALID(de__gpu_place_transient_buffer(r, ~(u64)0 - 255, &bd)));
    EXPECT_TRUE(strstr(errmsg(), "end past the region") != NULL);
    de_gpu_buffer_desc up = {
        .size = 256, .usage = DE_USAGE_COPY_SRC, .memory = DE_MEM_UPLOAD, .name = "up"};
    EXPECT_FALSE(DE_HANDLE_VALID(de__gpu_place_transient_buffer(r, 0, &up)));
    EXPECT_TRUE(strstr(errmsg(), "memory class \"gpu_only\"") != NULL);
    de_gpu_buffer_desc zero = {.usage = DE_USAGE_STORAGE};
    EXPECT_FALSE(DE_HANDLE_VALID(de__gpu_place_transient_buffer(r, 0, &zero)));
    EXPECT_FALSE(DE_HANDLE_VALID(de__gpu_place_transient_buffer(NULL, 0, &bd)));
    EXPECT_EQ(de__gpu_transient_region_placed(r), 3u);

    /* An UPLOAD region holds mappable placed buffers. */
    de__gpu_transient_region *ur = NULL;
    ASSERT_EQ(de__gpu_transient_region_create(f->d, DE__GPU_MEM_CLASS_UPLOAD, MIB, &ur), DE_OK);
    de_gpu_buffer ub = de__gpu_place_transient_buffer(ur, 256, &up);
    ASSERT_TRUE(DE_HANDLE_VALID(ub));
    EXPECT_TRUE(de_gpu_buffer_map(f->d, ub) != NULL);
    de__gpu_transient_region_destroy(ur); /* freed once ub is reclaimed */
    EXPECT_GT(class_stats(f->d, DE__GPU_MEM_CLASS_UPLOAD).used, (u64)0);
    de_gpu_buffer_destroy(f->d, ub);
    de_gpu_wait_idle(f->d);
    EXPECT_EQ(class_stats(f->d, DE__GPU_MEM_CLASS_UPLOAD).used, (u64)0);

    /* Destroying the region while resources live keeps its memory until the GPU is done with
     * the last of them. */
    de__gpu_null_set_manual_completion(f->d, true);
    de_gpu_cmd *c = de_gpu_cmd_begin(f->d, DE_QUEUE_GRAPHICS, "use");
    ASSERT_TRUE(c != NULL);
    de_gpu_submit s = {.cmds = &c, .cmd_count = 1};
    ASSERT_EQ(de_gpu_queue_submit(f->d, DE_QUEUE_GRAPHICS, &s), DE_OK);
    de__gpu_transient_region_destroy(r);
    EXPECT_FALSE(DE_HANDLE_VALID(de__gpu_place_transient_buffer(r, 0, &bd))); /* destroyed */
    EXPECT_TRUE(strstr(errmsg(), "the region is destroyed") != NULL);
    de_gpu_texture_destroy(f->d, t0);
    de_gpu_texture_destroy(f->d, t1);
    de_gpu_buffer_destroy(f->d, b0);
    de__gpu_collect(f->d);
    EXPECT_EQ(class_stats(f->d, G).used, 48 * MIB); /* the GPU still has the submission */
    EXPECT_EQ(de__gpu_transient_region_placed(r), 3u);
    de__gpu_null_complete(f->d, DE_QUEUE_GRAPHICS, ~(u64)0 >> 1);
    de__gpu_collect(f->d);
    EXPECT_EQ(class_stats(f->d, G).used, (u64)0); /* region freed with its last resource */
    de__gpu_null_set_manual_completion(f->d, false);

    /* A region nobody destroys is freed by device destroy (the fixture checks for leaks). */
    de__gpu_transient_region *leak = NULL;
    ASSERT_EQ(de__gpu_transient_region_create(f->d, G, MIB, &leak), DE_OK);
    de_gpu_texture lt = de__gpu_place_transient_texture(
        leak, 0,
        &(de_gpu_texture_desc){
            .format = DE_FMT_RGBA8_UNORM, .width = 64, .height = 64, .usage = DE_USAGE_SAMPLED});
    EXPECT_TRUE(DE_HANDLE_VALID(lt));
    /* And a plain allocation nobody frees. */
    de__gpu_alloc stray;
    ASSERT_EQ(de__gpu_mem_alloc(f->d, G, MIB, 0, DE_MEM_RENDER, &stray), DE_OK);
}

TEST(gpu_heap, unsupported_backend) {
    /* A backend without heap callbacks: allocation and placement report DE_ERR_UNSUPPORTED. */
    de__gpu_backend_vtable vt = de__gpu_null_vtable;
    vt.heap_create = NULL;
    vt.heap_destroy = NULL;
    vt.buffer_requirements = NULL;
    vt.texture_requirements = NULL;
    de__gpu_register_backend(DE_GPU_NULL, &vt);
    de_gpu_device *d = NULL;
    de_result r = de_gpu_device_create(&(de_gpu_device_desc){.backend = DE_GPU_NULL}, &d);
    de__gpu_register_backend(DE_GPU_NULL, NULL);
    ASSERT_EQ(r, DE_OK);
    de__gpu_alloc a;
    EXPECT_EQ(de__gpu_mem_alloc(d, 0, 256, 0, DE_MEM_RENDER, &a), DE_ERR_UNSUPPORTED);
    EXPECT_TRUE(strstr(errmsg(), "has no heaps") != NULL);
    de__gpu_transient_region *reg = NULL;
    EXPECT_EQ(de__gpu_transient_region_create(d, 0, MIB, &reg), DE_ERR_UNSUPPORTED);
    EXPECT_TRUE(reg == NULL);
    de__gpu_mem_req req;
    de_gpu_buffer_desc bd = {.size = 256, .usage = DE_USAGE_STORAGE};
    EXPECT_EQ(de__gpu_buffer_requirements(d, &bd, &req), DE_ERR_UNSUPPORTED);
    de_gpu_device_destroy(d);
}
