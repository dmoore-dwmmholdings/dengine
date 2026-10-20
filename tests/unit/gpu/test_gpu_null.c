/* test_gpu_null.c — de_gpu front-end on the null backend: devices, backend selection, object
 * lifetime, bindless recycling and deferred destruction (DENG-281). */
#include "de_test.h"

#include <dengine/de_gpu.h>

#include "gpu/gpu_backend.h"
#include "gpu/texture_lease_internal.h"

#include <string.h>

static const char *errmsg(void) {
    const de_error *e = de_error_last();
    return e ? e->message : "";
}

/* A null device on a tracking allocator, so every test also checks for leaks. */
typedef struct null_fx {
    de_tracking_allocator tracker;
    de_allocator alloc;
    de_gpu_device *d;
} null_fx;

static void null_fx_setup(null_fx *f) {
    f->alloc = de_tracking_allocator_init(&f->tracker, NULL, DE_MEM_RENDER);
    de_gpu_device_desc desc = {.backend = DE_GPU_NULL, .allocator = &f->alloc};
    ASSERT_EQ(de_gpu_device_create(&desc, &f->d), DE_OK);
}
static void null_fx_teardown(null_fx *f) {
    de_gpu_device_destroy(f->d);
    de_tracking_stats st = de_tracking_allocator_stats(&f->tracker);
    EXPECT_EQ(st.blocks_live, 0u);
    EXPECT_EQ(st.bytes_live, 0u);
}

static de_gpu_buffer make_buffer(de_gpu_device *d, de_gpu_memory mem) {
    de_gpu_buffer_desc bd = {.size = 64, .usage = DE_USAGE_STORAGE, .memory = mem, .name = "b"};
    return de_gpu_buffer_create(d, &bd);
}

TEST_F(gpu, texture_generation_leases_survive_owner_destroy_until_last_packet_release, null_fx) {
    de_gpu_texture t = de_gpu_texture_create(
        f->d,
        &(de_gpu_texture_desc){
            .width = 4, .height = 4, .format = DE_FMT_RGBA8_UNORM, .usage = DE_USAGE_SAMPLED});
    ASSERT_TRUE(t.id);
    u32 descriptor = de_gpu_texture_bindless(f->d, t, 0, false);
    ASSERT_TRUE(de__gpu_texture_pin(f->d, t));
    ASSERT_TRUE(de__gpu_texture_pin(f->d, t));
    de_gpu_texture_destroy(f->d, t);
    EXPECT_FALSE(de__gpu_texture_pin(f->d, t)); /* No new owner after destruction starts. */
    de_gpu_wait_idle(f->d);
    EXPECT_TRUE(de__gpu_get_texture(f->d, t) != NULL);
    EXPECT_EQ(de_gpu_texture_bindless(f->d, t, 0, false), descriptor);
    de__gpu_texture_unpin(f->d, t);
    de_gpu_wait_idle(f->d);
    EXPECT_TRUE(de__gpu_get_texture(f->d, t) != NULL);
    de__gpu_texture_unpin(f->d, t);
    EXPECT_TRUE(de__gpu_get_texture(f->d, t) == NULL);
    de_gpu_wait_idle(f->d);
    EXPECT_EQ(de__gpu_live_count(f->d, DE__GPU_TEXTURE), 0u);
    EXPECT_EQ(de__gpu_deferred_count(f->d), 0u);
}

typedef struct lease_thread {
    de_gpu_device *device;
    de_gpu_texture texture;
    de_atomic_u32 ready, release;
    bool pinned;
} lease_thread;
static i32 release_packet_texture(void *user) {
    lease_thread *s = user;
    s->pinned = de__gpu_texture_pin(s->device, s->texture);
    de_atomic_store_u32(&s->ready, 1, DE_ATOMIC_RELEASE);
    u64 end = de_time_now_ns() + 2000000000ULL;
    while (!de_atomic_load_u32(&s->release, DE_ATOMIC_ACQUIRE) && de_time_now_ns() < end)
        de_sleep_ns(50000);
    if (s->pinned)
        de__gpu_texture_unpin(s->device, s->texture);
    return 0;
}
TEST_F(gpu, texture_packet_leases_release_concurrently_with_owner_destruction, null_fx) {
    lease_thread s = {.device = f->d};
    s.texture = de_gpu_texture_create(f->d, &(de_gpu_texture_desc){.width = 4,
                                                                   .height = 4,
                                                                   .format = DE_FMT_RGBA8_UNORM,
                                                                   .usage = DE_USAGE_SAMPLED});
    ASSERT_TRUE(de__gpu_texture_pin(f->d, s.texture));
    de_atomic_init_u32(&s.ready, 0);
    de_atomic_init_u32(&s.release, 0);
    de_thread *thread = NULL;
    ASSERT_EQ(
        de_thread_create(release_packet_texture, &s, "packet lease", DE_THREAD_NORMAL, 0, &thread),
        DE_OK);
    u64 end = de_time_now_ns() + 2000000000ULL;
    while (!de_atomic_load_u32(&s.ready, DE_ATOMIC_ACQUIRE) && de_time_now_ns() < end)
        de_sleep_ns(50000);
    EXPECT_EQ(de_atomic_load_u32(&s.ready, DE_ATOMIC_ACQUIRE), 1u);
    de_gpu_texture_destroy(f->d, s.texture);
    EXPECT_TRUE(de__gpu_get_texture(f->d, s.texture) != NULL);
    de_atomic_store_u32(&s.release, 1, DE_ATOMIC_RELEASE);
    de__gpu_texture_unpin(f->d, s.texture);
    de_thread_join(thread);
    EXPECT_TRUE(s.pinned);
    EXPECT_TRUE(de__gpu_get_texture(f->d, s.texture) == NULL);
    de_gpu_wait_idle(f->d);
    EXPECT_EQ(de__gpu_live_count(f->d, DE__GPU_TEXTURE), 0u);
}

/* Submits an empty command list on `q`; returns the queue's new internal timeline value. */
static u64 submit_empty(de_gpu_device *d, de_gpu_queue q) {
    de_gpu_cmd *c = de_gpu_cmd_begin(d, q, "empty");
    ASSERT_TRUE(c != NULL);
    de_gpu_submit s = {.cmds = &c, .cmd_count = 1};
    ASSERT_EQ(de_gpu_queue_submit(d, q, &s), DE_OK);
    return de_atomic_load_u64(&d->submitted[q], DE_ATOMIC_RELAXED);
}

/* ---------- Devices and backend selection ---------- */

TEST_F(gpu, null_create_destroy, null_fx) {
    const de_gpu_caps *caps = de_gpu_get_caps(f->d);
    ASSERT_TRUE(caps != NULL);
    EXPECT_EQ(caps->backend, DE_GPU_NULL);
    EXPECT_STREQ(caps->device_name, "null");
    EXPECT_TRUE(caps->base_tier_ok);
    EXPECT_STREQ(caps->base_tier_reason, "");
    EXPECT_EQ(caps->max_push_constant_bytes, DE_GPU_MAX_PUSH_CONSTANT_BYTES);
    EXPECT_EQ(caps->timestamp_frequency, (u64)1000000000u);
    EXPECT_EQ(caps->raytracing, DE_RT_NONE);

    /* 100k buffers, every other one mappable. */
    enum { N = 100000 };
    static de_gpu_buffer first[N];
    for (u32 i = 0; i < N; ++i) {
        first[i] = make_buffer(f->d, (i & 1u) ? DE_MEM_UPLOAD : DE_MEM_GPU_ONLY);
        ASSERT_TRUE(DE_HANDLE_VALID(first[i]));
    }
    EXPECT_EQ(de__gpu_live_count(f->d, DE__GPU_BUFFER), (u32)N);
    EXPECT_EQ(de__gpu_bindless_count(f->d, DE__GPU_BINDLESS_BUFFER), (u32)N);
    /* Every bindless index is distinct and not 0. */
    EXPECT_EQ(de_gpu_buffer_bindless(f->d, first[0]), 1u);
    EXPECT_EQ(de_gpu_buffer_bindless(f->d, first[N - 1]), (u32)N);
    /* Mapped memory is real CPU memory. */
    u8 *p = (u8 *)de_gpu_buffer_map(f->d, first[1]);
    ASSERT_TRUE(p != NULL);
    p[0] = 7;
    p[63] = 9;
    EXPECT_EQ(((u8 *)de_gpu_buffer_map(f->d, first[1]))[63], 9u);

    for (u32 i = 0; i < N; ++i)
        de_gpu_buffer_destroy(f->d, first[i]);
    /* Destroyed handles are invalid at once, before the GPU is done. */
    EXPECT_EQ(de__gpu_live_count(f->d, DE__GPU_BUFFER), 0u);
    de_error_clear();
    EXPECT_TRUE(de_gpu_buffer_map(f->d, first[1]) == NULL);
    EXPECT_TRUE(strstr(errmsg(), "stale") != NULL);
    de_gpu_wait_idle(f->d);
    EXPECT_EQ(de__gpu_deferred_count(f->d), 0u);
    EXPECT_EQ(de__gpu_bindless_count(f->d, DE__GPU_BINDLESS_BUFFER), 0u);

    /* Second round: slots and bindless indices are recycled; old handles stay stale. */
    static de_gpu_buffer second[N];
    for (u32 i = 0; i < N; ++i) {
        second[i] = make_buffer(f->d, DE_MEM_UPLOAD);
        ASSERT_TRUE(DE_HANDLE_VALID(second[i]));
        ASSERT_LT(de__gpu_handle_index(second[i].id), (u32)N);
        ASSERT_LE(de_gpu_buffer_bindless(f->d, second[i]), (u32)N);
    }
    de__gpu_pool_stats st = de__gpu_pool_get_stats(&f->d->pools[DE__GPU_BUFFER]);
    EXPECT_EQ(st.used, (u32)N);
    for (u32 i = 0; i < N; i += 997) {
        de_error_clear();
        EXPECT_EQ(de_gpu_buffer_bindless(f->d, first[i]), DE_GPU_BINDLESS_INVALID);
        ASSERT_TRUE(de_error_last() != NULL);
        EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
        EXPECT_TRUE(strstr(errmsg(), "is stale") != NULL);
        de_gpu_buffer_destroy(f->d, first[i]); /* a stale destroy changes nothing */
    }
    EXPECT_EQ(de__gpu_live_count(f->d, DE__GPU_BUFFER), (u32)N);
    for (u32 i = 0; i < N; ++i)
        de_gpu_buffer_destroy(f->d, second[i]);
    de_gpu_wait_idle(f->d);
    EXPECT_EQ(de__gpu_live_count(f->d, DE__GPU_BUFFER), 0u);
}

TEST(gpu, default_backend_selection) {
    const de_gpu_backend platform = de_gpu_platform_default_backend();
#if defined(_WIN32)
    EXPECT_EQ(platform, DE_GPU_D3D12);
#elif defined(__APPLE__)
    EXPECT_EQ(platform, DE_GPU_METAL);
#else
    EXPECT_EQ(platform, DE_GPU_VULKAN);
#endif
    EXPECT_STREQ(de_gpu_backend_name(platform), platform == DE_GPU_METAL   ? "metal"
                                                : platform == DE_GPU_D3D12 ? "d3d12"
                                                                           : "vulkan");
    EXPECT_STREQ(de_gpu_backend_name(DE_GPU_NULL), "null");
    EXPECT_STREQ(de_gpu_backend_name(DE_GPU_BACKEND_COUNT_), "unknown");
    EXPECT_TRUE(de__gpu_find_backend(DE_GPU_NULL) == &de__gpu_null_vtable);
    EXPECT_TRUE(de__gpu_find_backend(DE_GPU_DEFAULT) == de__gpu_find_backend(platform));

    /* (desc){0} and a NULL desc pick the platform default. Until that backend is in the build
     * (macOS: DENG-287) the create fails with DE_ERR_UNSUPPORTED and names it. */
    de_gpu_device *d = NULL;
    de_result r = de_gpu_device_create(&(de_gpu_device_desc){0}, &d);
    if (de__gpu_find_backend(platform) == NULL) {
        EXPECT_EQ(r, DE_ERR_UNSUPPORTED);
        EXPECT_TRUE(d == NULL);
        EXPECT_TRUE(strstr(errmsg(), de_gpu_backend_name(platform)) != NULL);
        EXPECT_TRUE(strstr(errmsg(), "platform default") != NULL);
        EXPECT_EQ(de_gpu_device_create(NULL, &d), DE_ERR_UNSUPPORTED);
    } else if (r == DE_OK) {
        EXPECT_EQ(de_gpu_get_caps(d)->backend, platform);
        de_gpu_device_destroy(d);
    } /* else: the backend is built but this machine cannot run it (no GPU on a CI runner) */

    /* With a backend registered for the platform default, (desc){0} uses it. */
    de__gpu_register_backend(platform, &de__gpu_null_vtable);
    d = NULL;
    ASSERT_EQ(de_gpu_device_create(&(de_gpu_device_desc){0}, &d), DE_OK);
    EXPECT_EQ(de_gpu_get_caps(d)->backend, platform); /* the backend that was asked for */
    EXPECT_STREQ(de_gpu_get_caps(d)->device_name, "null");
    de_gpu_device_destroy(d);
    de__gpu_register_backend(platform, NULL); /* back to the built-in table */
    EXPECT_TRUE(de__gpu_find_backend(platform) != &de__gpu_null_vtable);

    /* DE_GPU_DEFAULT cannot be registered; out-of-range backends are refused. */
    de__gpu_register_backend(DE_GPU_DEFAULT, &de__gpu_null_vtable);
    EXPECT_TRUE(de__gpu_find_backend(DE_GPU_DEFAULT) == de__gpu_find_backend(platform));
    EXPECT_EQ(de_gpu_device_create(&(de_gpu_device_desc){.backend = (de_gpu_backend)99}, &d),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_gpu_device_create(NULL, NULL), DE_ERR_INVALID_ARGUMENT);
}

/* ---------- Bindless and deferred destruction ---------- */

TEST_F(gpu, bindless_alloc_recycle_after_fence, null_fx) {
    de__gpu_null_set_manual_completion(f->d, true);
    de_gpu_buffer a = make_buffer(f->d, DE_MEM_GPU_ONLY);
    const u32 ia = de_gpu_buffer_bindless(f->d, a);
    ASSERT_NE(ia, DE_GPU_BINDLESS_INVALID);

    const u64 v = submit_empty(f->d, DE_QUEUE_GRAPHICS); /* in flight: not completed */
    de_gpu_buffer_destroy(f->d, a);
    de__gpu_collect(f->d);
    EXPECT_EQ(de__gpu_deferred_count(f->d), 1u);
    EXPECT_EQ(de__gpu_bindless_count(f->d, DE__GPU_BINDLESS_BUFFER), 1u);

    /* While the GPU may still read index ia, a new buffer gets another one. */
    de_gpu_buffer b = make_buffer(f->d, DE_MEM_GPU_ONLY);
    const u32 ib = de_gpu_buffer_bindless(f->d, b);
    EXPECT_NE(ib, ia);

    de__gpu_null_complete(f->d, DE_QUEUE_GRAPHICS, v);
    EXPECT_EQ(de__gpu_collect(f->d), 1u);
    EXPECT_EQ(de__gpu_deferred_count(f->d), 0u);
    EXPECT_EQ(de__gpu_bindless_count(f->d, DE__GPU_BINDLESS_BUFFER), 1u);
    de_gpu_buffer c = make_buffer(f->d, DE_MEM_GPU_ONLY);
    EXPECT_EQ(de_gpu_buffer_bindless(f->d, c), ia); /* recycled after the fence */

    /* Textures: the sampled index and every storage-mip index wait for the fence too. */
    de_gpu_texture_desc td = {.format = DE_FMT_RGBA8_UNORM,
                              .width = 64,
                              .height = 64,
                              .mip_count = 3,
                              .usage = DE_USAGE_SAMPLED | DE_USAGE_STORAGE,
                              .name = "t"};
    de_gpu_texture t = de_gpu_texture_create(f->d, &td);
    ASSERT_TRUE(DE_HANDLE_VALID(t));
    const u32 sampled = de_gpu_texture_bindless(f->d, t, 0, false);
    const u32 mip2 = de_gpu_texture_bindless(f->d, t, 2, true);
    EXPECT_EQ(de__gpu_bindless_count(f->d, DE__GPU_BINDLESS_STORAGE_TEXTURE), 3u);
    const u64 v2 = submit_empty(f->d, DE_QUEUE_COMPUTE);
    de_gpu_texture_destroy(f->d, t);
    de__gpu_collect(f->d);
    EXPECT_EQ(de__gpu_bindless_count(f->d, DE__GPU_BINDLESS_SAMPLED_TEXTURE), 1u);
    EXPECT_EQ(de__gpu_bindless_count(f->d, DE__GPU_BINDLESS_STORAGE_TEXTURE), 3u);
    de__gpu_null_complete(f->d, DE_QUEUE_COMPUTE, v2);
    de__gpu_collect(f->d);
    EXPECT_EQ(de__gpu_bindless_count(f->d, DE__GPU_BINDLESS_SAMPLED_TEXTURE), 0u);
    EXPECT_EQ(de__gpu_bindless_count(f->d, DE__GPU_BINDLESS_STORAGE_TEXTURE), 0u);
    de_gpu_texture t2 = de_gpu_texture_create(f->d, &td);
    EXPECT_EQ(de_gpu_texture_bindless(f->d, t2, 0, false), sampled);
    /* LIFO: the last storage index freed (mip 2's) is handed out first, to mip 0. */
    EXPECT_EQ(de_gpu_texture_bindless(f->d, t2, 0, true), mip2);
    (void)b;
}

TEST_F(gpu, deferred_destroy_order, null_fx) {
    de__gpu_null_set_manual_completion(f->d, true);
    const u64 g1 = submit_empty(f->d, DE_QUEUE_GRAPHICS);
    de_gpu_buffer x = make_buffer(f->d, DE_MEM_GPU_ONLY);
    de_gpu_buffer y = make_buffer(f->d, DE_MEM_GPU_ONLY);
    de_gpu_buffer z = make_buffer(f->d, DE_MEM_GPU_ONLY);
    de_gpu_buffer_destroy(f->d, x); /* waits for graphics g1 */
    const u64 c1 = submit_empty(f->d, DE_QUEUE_COMPUTE);
    de_gpu_buffer_destroy(f->d, y); /* waits for graphics g1 and compute c1 */
    const u64 g2 = submit_empty(f->d, DE_QUEUE_GRAPHICS);
    de_gpu_buffer_destroy(f->d, z); /* waits for graphics g2 and compute c1 */
    EXPECT_EQ(de__gpu_deferred_count(f->d), 3u);

    de__gpu_null_complete(f->d, DE_QUEUE_GRAPHICS, g1);
    EXPECT_EQ(de__gpu_collect(f->d), 1u); /* x only: y also waits for compute */
    EXPECT_EQ(de__gpu_deferred_count(f->d), 2u);
    de__gpu_null_complete(f->d, DE_QUEUE_COMPUTE, c1);
    EXPECT_EQ(de__gpu_collect(f->d), 1u); /* y; z waits for graphics g2 */
    EXPECT_EQ(de__gpu_deferred_count(f->d), 1u);
    /* The copy queue never ran anything: its value 0 is always complete. */
    de__gpu_null_complete(f->d, DE_QUEUE_GRAPHICS, g2);
    EXPECT_EQ(de__gpu_collect(f->d), 1u);
    EXPECT_EQ(de__gpu_deferred_count(f->d), 0u);
    EXPECT_EQ(de__gpu_pool_get_stats(&f->d->pools[DE__GPU_BUFFER]).free, 3u);

    /* The released slots are reused in release order: x, y, z. */
    de_gpu_buffer n1 = make_buffer(f->d, DE_MEM_GPU_ONLY);
    de_gpu_buffer n2 = make_buffer(f->d, DE_MEM_GPU_ONLY);
    EXPECT_EQ(de__gpu_handle_index(n1.id), de__gpu_handle_index(x.id));
    EXPECT_EQ(de__gpu_handle_index(n2.id), de__gpu_handle_index(y.id));

    /* wait_idle flushes whatever is still waiting. */
    submit_empty(f->d, DE_QUEUE_COPY);
    de_gpu_buffer_destroy(f->d, n1);
    de_gpu_buffer_destroy(f->d, n2);
    EXPECT_EQ(de__gpu_deferred_count(f->d), 2u);
    de_gpu_wait_idle(f->d);
    EXPECT_EQ(de__gpu_deferred_count(f->d), 0u);
}

/* ---------- Fences, submits, command-list recycling ---------- */

TEST_F(gpu, fences_and_command_recycling, null_fx) {
    de_gpu_fence fence = de_gpu_fence_create(f->d, 3);
    ASSERT_TRUE(DE_HANDLE_VALID(fence));
    EXPECT_EQ(de_gpu_fence_value(f->d, fence), 3u);

    de_gpu_cmd *c = de_gpu_cmd_begin(f->d, DE_QUEUE_GRAPHICS, "frame");
    ASSERT_TRUE(c != NULL);
    de_gpu_submit s = {.cmds = &c, .cmd_count = 1, .signal = fence, .signal_value = 10};
    ASSERT_EQ(de_gpu_queue_submit(f->d, DE_QUEUE_GRAPHICS, &s), DE_OK);
    EXPECT_EQ(de_gpu_fence_value(f->d, fence), 10u); /* null: complete at submit */
    EXPECT_EQ(de_gpu_fence_wait(f->d, fence, 10, 0), DE_OK);
    EXPECT_EQ(de_gpu_fence_wait(f->d, fence, 11, 0), DE_ERR_TIMEOUT);
    EXPECT_EQ(de_gpu_fence_wait(f->d, fence, 11, 1000000), DE_ERR_TIMEOUT); /* 1 ms */

    /* The submitted list was recycled (its submit completed): the next begin reuses it. */
    de_gpu_cmd *c2 = de_gpu_cmd_begin(f->d, DE_QUEUE_GRAPHICS, "frame2");
    EXPECT_TRUE(c2 == c);
    /* Resubmitting a list is refused. */
    de_gpu_submit s2 = {
        .cmds = &c2, .cmd_count = 1, .waits = &(de_gpu_wait){fence, 10}, .wait_count = 1};
    ASSERT_EQ(de_gpu_queue_submit(f->d, DE_QUEUE_GRAPHICS, &s2), DE_OK);
    EXPECT_EQ(de_gpu_queue_submit(f->d, DE_QUEUE_GRAPHICS, &s2), DE_ERR_INVALID_ARGUMENT);

    /* With completion held back the list stays in flight and a new one is made. */
    de__gpu_null_set_manual_completion(f->d, true);
    de_gpu_cmd *c3 = de_gpu_cmd_begin(f->d, DE_QUEUE_GRAPHICS, "held");
    EXPECT_TRUE(c3 == c);
    de_gpu_submit s3 = {.cmds = &c3, .cmd_count = 1};
    ASSERT_EQ(de_gpu_queue_submit(f->d, DE_QUEUE_GRAPHICS, &s3), DE_OK);
    de_gpu_cmd *c4 = de_gpu_cmd_begin(f->d, DE_QUEUE_GRAPHICS, "new");
    EXPECT_TRUE(c4 != NULL && c4 != c3);
    /* A list begun on one queue cannot go to another. */
    de_gpu_submit s4 = {.cmds = &c4, .cmd_count = 1};
    EXPECT_EQ(de_gpu_queue_submit(f->d, DE_QUEUE_COMPUTE, &s4), DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(strstr(errmsg(), "was begun on queue") != NULL);
    ASSERT_EQ(de_gpu_queue_submit(f->d, DE_QUEUE_GRAPHICS, &s4), DE_OK);
    de__gpu_null_set_manual_completion(f->d, false);

    /* Fence destruction is deferred like any object. */
    de_gpu_fence_destroy(f->d, fence);
    EXPECT_EQ(de_gpu_fence_value(f->d, fence), 0u);
    de_gpu_wait_idle(f->d);
    EXPECT_EQ(de__gpu_live_count(f->d, DE__GPU_FENCE), 0u);
}

TEST_F(gpu, texture_views_and_samplers, null_fx) {
    de_gpu_texture_desc td = {.format = DE_FMT_RGBA16_FLOAT,
                              .width = 256,
                              .height = 128,
                              .mip_count = 4,
                              .usage = DE_USAGE_SAMPLED | DE_USAGE_STORAGE,
                              .name = "hdr"};
    de_gpu_texture t = de_gpu_texture_create(f->d, &td);
    ASSERT_TRUE(DE_HANDLE_VALID(t));
    u32 storage[4];
    for (u32 m = 0; m < 4; ++m) {
        storage[m] = de_gpu_texture_bindless(f->d, t, m, true);
        EXPECT_NE(storage[m], DE_GPU_BINDLESS_INVALID);
        for (u32 k = 0; k < m; ++k)
            EXPECT_NE(storage[m], storage[k]);
    }
    /* Sampled and storage indices come from separate spaces: both start at 1. */
    EXPECT_EQ(de_gpu_texture_bindless(f->d, t, 0, false), 1u);
    EXPECT_EQ(storage[0], 1u);
    EXPECT_EQ(de_gpu_texture_bindless(f->d, t, 4, true), DE_GPU_BINDLESS_INVALID);
    EXPECT_TRUE(strstr(errmsg(), "mip 4, but the texture has 4") != NULL);
    EXPECT_EQ(de_gpu_texture_bindless(f->d, t, 1, false), DE_GPU_BINDLESS_INVALID);
    EXPECT_TRUE(strstr(errmsg(), "covers every mip") != NULL);

    /* A sampled-only texture has no storage view; a cube defaults to 6 layers. */
    de_gpu_texture_desc cd = {.dim = DE_TEX_CUBE,
                              .format = DE_FMT_BC7_SRGB,
                              .width = 64,
                              .height = 64,
                              .usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST};
    de_gpu_texture cube = de_gpu_texture_create(f->d, &cd);
    ASSERT_TRUE(DE_HANDLE_VALID(cube));
    EXPECT_EQ(de__gpu_get_texture(f->d, cube)->depth_or_layers, 6u);
    EXPECT_EQ(de_gpu_texture_bindless(f->d, cube, 0, true), DE_GPU_BINDLESS_INVALID);
    EXPECT_TRUE(strstr(errmsg(), "lacks DE_USAGE_STORAGE") != NULL);

    /* Samplers: (desc){0} is valid; each gets its own index. */
    de_gpu_sampler s0 = de_gpu_sampler_create(f->d, &(de_gpu_sampler_desc){0});
    de_gpu_sampler s1 = de_gpu_sampler_create(
        f->d, &(de_gpu_sampler_desc){.min = DE_FILTER_LINEAR, .max_aniso = 16.0f});
    ASSERT_TRUE(DE_HANDLE_VALID(s0) && DE_HANDLE_VALID(s1));
    EXPECT_EQ(de_gpu_sampler_bindless(f->d, s0), 1u);
    EXPECT_EQ(de_gpu_sampler_bindless(f->d, s1), 2u);
    de_gpu_sampler_destroy(f->d, s0);
    /* Leave the rest alive: device destroy releases them (and warns). */
}

TEST_F(gpu, unsupported_features, null_fx) {
    de_error_clear();
    de_gpu_accel a = de_gpu_accel_create(f->d, &(de_gpu_accel_desc){.is_tlas = true});
    EXPECT_FALSE(DE_HANDLE_VALID(a));
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_gpu_accel_bindless(f->d, a), DE_GPU_BINDLESS_INVALID);

    de_gpu_swapchain sc =
        de_gpu_swapchain_create(f->d, &(de_gpu_swapchain_desc){.window = {.id = 1}});
    EXPECT_FALSE(DE_HANDLE_VALID(sc));
    EXPECT_EQ(de_error_last()->result, DE_ERR_UNSUPPORTED);
    EXPECT_TRUE(strstr(errmsg(), "null backend has no swapchain") != NULL);

    de_gpu_cmd *c = de_gpu_cmd_begin(f->d, DE_QUEUE_COMPUTE, "rt");
    ASSERT_TRUE(c != NULL);
    de_gpu_cmd_dispatch_rays(c, 1, 1, 1);
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(strstr(errmsg(), "no ray-tracing pipeline is bound") != NULL);
    de_gpu_submit s = {.cmds = &c, .cmd_count = 1};
    EXPECT_EQ(de_gpu_queue_submit(f->d, DE_QUEUE_COMPUTE, &s), DE_OK);

    /* Query heaps and timestamps record on the null backend. */
    de_gpu_query_heap q =
        de_gpu_query_heap_create(f->d, &(de_gpu_query_heap_desc){.count = 8, .name = "ts"});
    ASSERT_TRUE(DE_HANDLE_VALID(q));
    de_gpu_query_heap_destroy(f->d, q);
}

TEST(gpu, device_destroy_releases_leaked_objects) {
    de_tracking_allocator tracker;
    de_allocator alloc = de_tracking_allocator_init(&tracker, NULL, DE_MEM_RENDER);
    de_gpu_device *d = NULL;
    ASSERT_EQ(de_gpu_device_create(
                  &(de_gpu_device_desc){.backend = DE_GPU_NULL, .allocator = &alloc}, &d),
              DE_OK);
    de__gpu_null_set_manual_completion(d, true);
    for (u32 i = 0; i < 1000; ++i)
        make_buffer(d, DE_MEM_UPLOAD);
    de_gpu_texture_create(d, &(de_gpu_texture_desc){.format = DE_FMT_D32_FLOAT,
                                                    .width = 8,
                                                    .usage = DE_USAGE_DEPTH | DE_USAGE_SAMPLED});
    de_gpu_fence_create(d, 0);
    de_gpu_cmd_begin(d, DE_QUEUE_COPY, "never submitted");
    submit_empty(d, DE_QUEUE_GRAPHICS);
    de_gpu_buffer_destroy(d, make_buffer(d, DE_MEM_GPU_ONLY)); /* still deferred */
    de_gpu_device_destroy(d);
    EXPECT_EQ(de_tracking_allocator_stats(&tracker).blocks_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&tracker).bytes_live, 0u);
    de_gpu_device_destroy(NULL);
}

/* ---------- Threads ---------- */

typedef struct mt_ctx {
    de_gpu_device *d;
    u32 seed;
    u32 failures;
} mt_ctx;

/* Each thread creates, uses and destroys buffers and textures and submits on its own queue
 * (mixed with the other threads), checking that every live handle still resolves to its own
 * object and every destroyed one is stale. */
static i32 mt_main(void *user) {
    mt_ctx *x = (mt_ctx *)user;
    enum { LIVE = 32, ROUNDS = 4000 };
    de_gpu_buffer live[LIVE];
    memset(live, 0, sizeof live);
    u32 r = x->seed;
    for (u32 i = 0; i < ROUNDS; ++i) {
        r = r * 1664525u + 1013904223u;
        u32 slot = (r >> 8) % LIVE;
        if (live[slot].id) {
            u32 *p = (u32 *)de_gpu_buffer_map(x->d, live[slot]);
            if (!p || p[0] != live[slot].id)
                ++x->failures;
            de_gpu_buffer_destroy(x->d, live[slot]);
            if (de_gpu_buffer_map(x->d, live[slot]) != NULL)
                ++x->failures;
            live[slot].id = 0;
        } else {
            de_gpu_buffer_desc bd = {
                .size = 16, .usage = DE_USAGE_STORAGE, .memory = DE_MEM_UPLOAD};
            live[slot] = de_gpu_buffer_create(x->d, &bd);
            u32 *p = (u32 *)de_gpu_buffer_map(x->d, live[slot]);
            if (!p)
                ++x->failures;
            else
                p[0] = live[slot].id;
        }
        if ((i & 63u) == 0) {
            de_gpu_queue q = (de_gpu_queue)((r >> 4) % DE_QUEUE_COUNT_);
            de_gpu_cmd *c = de_gpu_cmd_begin(x->d, q, "mt");
            de_gpu_submit s = {.cmds = &c, .cmd_count = c ? 1u : 0u};
            if (!c || de_gpu_queue_submit(x->d, q, &s) != DE_OK)
                ++x->failures;
        }
    }
    for (u32 i = 0; i < LIVE; ++i)
        de_gpu_buffer_destroy(x->d, live[i]);
    return 0;
}

TEST_F(gpu, threads_create_destroy_submit, null_fx) {
    enum { T = 8 };
    de_thread *threads[T];
    mt_ctx ctx[T];
    for (u32 i = 0; i < T; ++i) {
        ctx[i] = (mt_ctx){.d = f->d, .seed = 0x9E3779B9u * (i + 1u)};
        ASSERT_EQ(de_thread_create(mt_main, &ctx[i], "gpu-mt", DE_THREAD_NORMAL, 0, &threads[i]),
                  DE_OK);
    }
    for (u32 i = 0; i < T; ++i) {
        de_thread_join(threads[i]);
        EXPECT_EQ(ctx[i].failures, 0u);
    }
    de_gpu_wait_idle(f->d);
    EXPECT_EQ(de__gpu_live_count(f->d, DE__GPU_BUFFER), 0u);
    EXPECT_EQ(de__gpu_deferred_count(f->d), 0u);
    EXPECT_EQ(de__gpu_bindless_count(f->d, DE__GPU_BINDLESS_BUFFER), 0u);
}
