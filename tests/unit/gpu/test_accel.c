#include "de_test.h"
#include "gpu/gpu_accel_internal.h"
#include <string.h>

typedef struct accel_fx {
    de_tracking_allocator tracker;
    de_allocator alloc;
    de_gpu_device *d;
    de_gpu_buffer vertices, indices, instances;
    de_gpu_blas_geometry geometry;
    de_gpu_accel_desc desc;
} accel_fx;
static void accel_fx_setup(accel_fx *f) {
    f->alloc = de_tracking_allocator_init(&f->tracker, NULL, DE_MEM_RENDER);
    ASSERT_EQ(de_gpu_device_create(
                  &(de_gpu_device_desc){.backend = DE_GPU_NULL, .allocator = &f->alloc}, &f->d),
              DE_OK);
    f->vertices = de_gpu_buffer_create(
        f->d,
        &(de_gpu_buffer_desc){.size = 48, .usage = DE_USAGE_ACCEL_INPUT, .memory = DE_MEM_UPLOAD});
    f->indices = de_gpu_buffer_create(
        f->d,
        &(de_gpu_buffer_desc){.size = 12, .usage = DE_USAGE_ACCEL_INPUT, .memory = DE_MEM_UPLOAD});
    f->instances = de_gpu_buffer_create(
        f->d,
        &(de_gpu_buffer_desc){.size = 180, .usage = DE_USAGE_ACCEL_INPUT, .memory = DE_MEM_UPLOAD});
    f->geometry = (de_gpu_blas_geometry){.vertices = f->vertices,
                                         .vertex_stride = 16,
                                         .vertex_count = 3,
                                         .vertex_format = DE_FMT_RGB32_FLOAT,
                                         .indices = f->indices,
                                         .index_count = 3,
                                         .opaque = true};
    f->desc = (de_gpu_accel_desc){.geometries = &f->geometry,
                                  .geometry_count = 1,
                                  .allow_update = true,
                                  .allow_compaction = true};
}
static void accel_fx_teardown(accel_fx *f) {
    de_gpu_device_destroy(f->d);
    EXPECT_EQ(de_tracking_allocator_stats(&f->tracker).blocks_live, 0u);
}
static void submit(de_gpu_device *d, de_gpu_cmd *c) {
    ASSERT_EQ(
        de_gpu_queue_submit(d, DE_QUEUE_COMPUTE, &(de_gpu_submit){.cmds = &c, .cmd_count = 1}),
        DE_OK);
}
static void expect_invalid(void) {
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    de_error_clear();
}
TEST_F(accel, desc_validation, accel_fx) {
    EXPECT_EQ(de_gpu_accel_create(f->d, &(de_gpu_accel_desc){0}).id, 0u);
    expect_invalid();
    de_gpu_accel_sizes sizes = de_gpu_accel_query_sizes(f->d, &f->desc);
    EXPECT_TRUE(sizes.accel_bytes && sizes.build_scratch_bytes && sizes.update_scratch_bytes);
    f->geometry.vertex_stride = 8;
    EXPECT_EQ(de_gpu_accel_create(f->d, &f->desc).id, 0u);
    expect_invalid();
    f->geometry.vertex_stride = 16;
    f->geometry.vertex_offset = 8;
    EXPECT_EQ(de_gpu_accel_create(f->d, &f->desc).id, 0u);
    expect_invalid();
    f->geometry.vertex_offset = 0;
    f->geometry.index_count = 4;
    EXPECT_EQ(de_gpu_accel_create(f->d, &f->desc).id, 0u);
    expect_invalid();
    f->geometry.index_count = 3;
    f->geometry.index_u16 = true;
    de_gpu_accel a = de_gpu_accel_create(f->d, &f->desc);
    ASSERT_TRUE(a.id != 0);
    EXPECT_EQ(de_gpu_accel_bindless(f->d, a), DE_GPU_BINDLESS_INVALID);
    expect_invalid();
    de_gpu_cmd *c = de_gpu_cmd_begin(f->d, DE_QUEUE_COMPUTE, "validation");
    de_gpu_cmd_build_blas(c, a, true);
    expect_invalid();
    de_gpu_cmd_build_blas(c, a, false);
    de_gpu_cmd_build_blas(c, a, true);
    /* The caller's descriptor storage can change: creation owns an immutable copy. */
    f->geometry.index_count = 6;
    de_gpu_cmd_build_blas(c, a, true);
    EXPECT_TRUE(de_error_last() == NULL);
    de_gpu_cmd_build_tlas(c, a, f->instances, 1, false);
    expect_invalid();
    submit(f->d, c);
}
TEST_F(accel, refit_validation, accel_fx) {
    f->desc.allow_update = false;
    de_gpu_accel a = de_gpu_accel_create(f->d, &f->desc);
    de_gpu_accel t = de_gpu_accel_create(
        f->d, &(de_gpu_accel_desc){.is_tlas = true, .max_instances = 3, .allow_update = true});
    ASSERT_TRUE(a.id && t.id);
    EXPECT_TRUE(de_gpu_accel_bindless(f->d, t) != 0);
    de_gpu_cmd *c = de_gpu_cmd_begin(f->d, DE_QUEUE_COMPUTE, "refit");
    de_gpu_cmd_build_blas(c, a, false);
    de_gpu_cmd_build_blas(c, a, true);
    expect_invalid();
    de_gpu_cmd_build_tlas(c, t, f->instances, 2, false);
    de_gpu_cmd_build_tlas(c, t, f->instances, 1, true);
    expect_invalid();
    de_gpu_cmd_build_tlas(c, t, f->instances, 4, false);
    expect_invalid();
    de_gpu_cmd_build_tlas(c, t, f->instances, 2, true);
    EXPECT_TRUE(de_error_last() == NULL);
    de_gpu_buffer_destroy(f->d, f->vertices);
    de_gpu_cmd_build_blas(c, a, false);
    expect_invalid();
    submit(f->d, c);
}
TEST_F(accel, handles, accel_fx) {
    de__gpu_null_set_manual_completion(f->d, true);
    de_gpu_accel a = de_gpu_accel_create(f->d, &f->desc);
    de_gpu_cmd *c = de_gpu_cmd_begin(f->d, DE_QUEUE_COMPUTE, "handles");
    de_gpu_cmd_build_blas(c, a, false);
    submit(f->d, c);
    de_gpu_accel_destroy(f->d, a);
    EXPECT_TRUE(de__gpu_get_accel(f->d, a) == NULL);
    EXPECT_EQ(de__gpu_pool_get_stats(&f->d->pools[DE__GPU_ACCEL]).retired, 1u);
    de_gpu_accel b = de_gpu_accel_create(f->d, &f->desc);
    EXPECT_TRUE(de__gpu_handle_index(a.id) != de__gpu_handle_index(b.id));
    de__gpu_null_complete(f->d, DE_QUEUE_COMPUTE, 1);
    de__gpu_collect(f->d);
    de_gpu_accel next = de_gpu_accel_create(f->d, &f->desc);
    EXPECT_EQ(de__gpu_handle_index(next.id), de__gpu_handle_index(a.id));
    EXPECT_TRUE(next.id != a.id);
    c = de_gpu_cmd_begin(f->d, DE_QUEUE_COMPUTE, "stale");
    de_gpu_cmd_build_blas(c, a, false);
    expect_invalid();
    submit(f->d, c);
}
TEST_F(accel, compaction_fence, accel_fx) {
    de__gpu_null_set_manual_completion(f->d, true);
    de_gpu_accel a = de_gpu_accel_create(f->d, &f->desc);
    u64 bytes = 999;
    EXPECT_FALSE(de_gpu_accel_compacted_size(f->d, a, &bytes));
    EXPECT_EQ(bytes, 0u);
    de_gpu_cmd *c = de_gpu_cmd_begin(f->d, DE_QUEUE_COMPUTE, "compact");
    de_gpu_cmd_build_blas(c, a, false);
    de_gpu_cmd_accel_emit_compacted_size(c, a);
    EXPECT_FALSE(de_gpu_accel_compacted_size(f->d, a, &bytes));
    submit(f->d, c);
    EXPECT_FALSE(de_gpu_accel_compacted_size(f->d, a, &bytes));
    de__gpu_null_complete(f->d, DE_QUEUE_COMPUTE, 1);
    EXPECT_TRUE(de_gpu_accel_compacted_size(f->d, a, &bytes));
    EXPECT_TRUE(bytes && bytes <= de__gpu_get_accel(f->d, a)->sizes.accel_bytes);
    c = de_gpu_cmd_begin(f->d, DE_QUEUE_COMPUTE, "compact copy");
    de_gpu_accel copy = de_gpu_cmd_accel_compact(c, a);
    ASSERT_TRUE(copy.id != 0);
    EXPECT_EQ(de__gpu_get_accel(f->d, copy)->sizes.accel_bytes, bytes);
    submit(f->d, c);
    de_gpu_accel_destroy(f->d, a);
    EXPECT_TRUE(de__gpu_get_accel(f->d, copy) != NULL);
}
TEST(accel, instance_layout) {
    de__gpu_accel_address table[4] = {{0}};
    de_gpu_tlas_instance input[3] = {{0}};
    de__gpu_native_instance output[3];
    table[1] = (de__gpu_accel_address){.handle = 0x100001, .address = 0x12345678abcdef00ull};
    table[2] = (de__gpu_accel_address){.handle = 0x200002, .address = 0xfedcba9876543200ull};
    for (u32 i = 0; i < 3; ++i) {
        for (u32 j = 0; j < 12; ++j)
            input[i].transform[j] = (f32)(i * 12 + j);
        input[i].custom_index = 123 + i;
        input[i].mask = 0xab;
        input[i].sbt_offset = 345 + i;
        input[i].flags = 5;
        input[i].blas.id = i == 0 ? 0x100001 : i == 1 ? 0x200002 : 0x100002; /* stale generation */
        de__gpu_accel_translate(&input[i], &output[i], table, 4);
        EXPECT_EQ(memcmp(input[i].transform, output[i].transform, 48), 0);
        EXPECT_EQ(output[i].custom_mask, 123u + i + (i < 2 ? 0xab000000u : 0));
        EXPECT_EQ(output[i].sbt_flags, 345u + i + 0x05000000u);
    }
    EXPECT_EQ(output[0].address, 0x12345678abcdef00ull);
    EXPECT_EQ(output[1].address, 0xfedcba9876543200ull);
    EXPECT_EQ(output[2].address, 0u);
    input[2].blas.id = 0xffffffff;
    de__gpu_accel_translate(&input[2], &output[2], table, 4);
    EXPECT_EQ(output[2].address, 0u);
    EXPECT_EQ(output[2].custom_mask >> 24, 0u);
}

TEST_F(accel, scratch_reuse_no_allocations, accel_fx) {
    de_gpu_cmd *c = de_gpu_cmd_begin(f->d, DE_QUEUE_COMPUTE, "warm scratch");
    ASSERT_TRUE(de__gpu_accel_scratch_get(c, 256, 0) != NULL);
    ASSERT_TRUE(de__gpu_accel_scratch_get(c, 512, 0) != NULL);
    submit(f->d, c);
    const u64 before = de_tracking_allocator_stats(&f->tracker).alloc_count;
    for (u32 i = 0; i < 8; ++i) {
        c = de_gpu_cmd_begin(f->d, DE_QUEUE_COMPUTE, "reuse scratch");
        ASSERT_TRUE(de__gpu_accel_scratch_get(c, 256, 0) != NULL);
        ASSERT_TRUE(de__gpu_accel_scratch_get(c, 512, 0) != NULL);
        submit(f->d, c);
    }
    EXPECT_EQ(de_tracking_allocator_stats(&f->tracker).alloc_count, before);
}
TEST_F(accel, failed_create_unwinds, accel_fx) {
    /* Warm the handle chunk, then fail geometry ownership allocation. */
    de_gpu_accel first = de_gpu_accel_create(f->d, &f->desc);
    ASSERT_TRUE(first.id != 0);
    de_gpu_accel_destroy(f->d, first);
    de_gpu_wait_idle(f->d);
    const de_tracking_stats before = de_tracking_allocator_stats(&f->tracker);
    de_tracking_allocator_fail_after(&f->tracker, 0);
    EXPECT_EQ(de_gpu_accel_create(f->d, &f->desc).id, 0u);
    de_tracking_allocator_fail_after(&f->tracker, -1);
    EXPECT_EQ(de_tracking_allocator_stats(&f->tracker).blocks_live, before.blocks_live);
    EXPECT_EQ(de__gpu_live_count(f->d, DE__GPU_ACCEL), 0u);
}
