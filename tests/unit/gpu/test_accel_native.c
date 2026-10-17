/* Real API proof. Unsupported devices skip explicitly; API_ONLY implementations are exercised. */
#include "de_test.h"
#include "gpu/gpu_accel_internal.h"
#include "pipeline_shaders.h"
#include <stdio.h>
#include <string.h>
#if DE_HAS_GPU_VULKAN
#include "gpu/vulkan/vk_internal.h"
#endif
#if DE_HAS_GPU_D3D12
#include "gpu/d3d12/d3d12_internal.h"
#include "gpu/d3d12/d3d12_agility_exports.h"
#endif

typedef struct native_fx {
    de_gpu_device *d;
    de_gpu_accel blas, tlas;
    de_gpu_buffer vertices, indices, instances, output, readback;
    de_gpu_pipeline pipeline;
    de_gpu_fence fence;
    u64 value;
    bool queried;
} native_fx;
static void native_fx_setup(native_fx *f) {
#if defined(_WIN32)
    const de_gpu_backend backend = DE_GPU_D3D12;
    const de__shader_target target = DE__SHADER_TARGET_DXIL;
#else
    const de_gpu_backend backend = DE_GPU_VULKAN;
    const de__shader_target target = DE__SHADER_TARGET_SPIRV;
#endif
    de_gpu_device_desc dd = {
        .backend = backend, .debug_layer = true, .gpu_validation = true, .pipeline_cache_dir = ""};
    de_result r = de_gpu_device_create(&dd, &f->d);
    if (r == DE_ERR_UNSUPPORTED && backend == DE_GPU_VULKAN) {
        dd.adapter = 1;
        r = de_gpu_device_create(&dd, &f->d);
    }
    if (r != DE_OK)
        SKIP("native AS runtime unavailable on this host");
    if (!de_gpu_get_caps(f->d)->ray_query)
        SKIP("native device has no functional ray-query API (needs RT runner)");
    printf("AS API runtime: %s; RT support=%d (API_ONLY is exercised)\n",
           de_gpu_get_caps(f->d)->device_name, (int)de_gpu_get_caps(f->d)->raytracing);
    char why[512] = {0};
    ASSERT_TRUE(de_pipe_test_cook(target, why, sizeof why));
    de_pipe_test_shader sh;
    ASSERT_TRUE(de_pipe_test_get(target, "accel_query.slang", "cs_main", &sh));
    f->pipeline =
        de_gpu_pipeline_create(f->d, &(de_gpu_pipeline_desc){.kind = DE_PIPE_COMPUTE,
                                                             .cs = sh.blob,
                                                             .push_constant_bytes = 8,
                                                             .threadgroup_size = {1, 1, 1}});
    ASSERT_TRUE(f->pipeline.id != 0);
    f->vertices = de_gpu_buffer_create(
        f->d,
        &(de_gpu_buffer_desc){.size = 48, .usage = DE_USAGE_ACCEL_INPUT, .memory = DE_MEM_UPLOAD});
    f->indices = de_gpu_buffer_create(
        f->d,
        &(de_gpu_buffer_desc){.size = 12, .usage = DE_USAGE_ACCEL_INPUT, .memory = DE_MEM_UPLOAD});
    f->instances = de_gpu_buffer_create(
        f->d,
        &(de_gpu_buffer_desc){.size = 60, .usage = DE_USAGE_ACCEL_INPUT, .memory = DE_MEM_UPLOAD});
    f->output = de_gpu_buffer_create(
        f->d, &(de_gpu_buffer_desc){.size = 4, .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC});
    f->readback = de_gpu_buffer_create(
        f->d,
        &(de_gpu_buffer_desc){.size = 4, .usage = DE_USAGE_COPY_DST, .memory = DE_MEM_READBACK});
    ASSERT_TRUE(f->vertices.id && f->indices.id && f->instances.id && f->output.id &&
                f->readback.id);
    const f32 triangle[9] = {-1, -1, 2, 1, -1, 2, 0, 1, 2};
    const u16 indices[4] = {0, 1, 2, 0};
    memcpy(de_gpu_buffer_map(f->d, f->vertices), triangle, sizeof triangle);
    memcpy(de_gpu_buffer_map(f->d, f->indices), indices, sizeof indices);
    de_gpu_blas_geometry geometry = {.vertices = f->vertices,
                                     .vertex_stride = 12,
                                     .vertex_count = 3,
                                     .vertex_format = DE_FMT_RGB32_FLOAT,
                                     .indices = f->indices,
                                     .index_count = 3,
                                     .index_u16 = true,
                                     .opaque = true};
    f->blas = de_gpu_accel_create(f->d, &(de_gpu_accel_desc){.geometries = &geometry,
                                                             .geometry_count = 1,
                                                             .allow_update = true,
                                                             .allow_compaction = true});
    f->tlas = de_gpu_accel_create(
        f->d, &(de_gpu_accel_desc){.is_tlas = true, .max_instances = 1, .allow_update = true});
    ASSERT_TRUE(f->blas.id && f->tlas.id);
    de_gpu_tlas_instance instance = {
        .transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}, .mask = 255, .blas = f->blas};
    memcpy(de_gpu_buffer_map(f->d, f->instances), &instance, sizeof instance);
    f->fence = de_gpu_fence_create(f->d, 0);
    ASSERT_TRUE(f->fence.id != 0);
}
static void native_fx_teardown(native_fx *f) {
    if (!f->d)
        return;
    de_gpu_wait_idle(f->d);
#if DE_HAS_GPU_VULKAN
    if (f->d->caps.backend == DE_GPU_VULKAN)
        EXPECT_EQ(de__gpu_vk_validation_messages(f->d, NULL), 0u);
#endif
#if DE_HAS_GPU_D3D12
    if (f->d->caps.backend == DE_GPU_D3D12)
        EXPECT_EQ(de__d3d12_debug_error_count(f->d), 0u);
#endif
    de_gpu_device_destroy(f->d);
}
static void finish(native_fx *f, de_gpu_cmd *c) {
    de_gpu_submit s = {.cmds = &c, .cmd_count = 1, .signal = f->fence, .signal_value = ++f->value};
    ASSERT_EQ(de_gpu_queue_submit(f->d, DE_QUEUE_COMPUTE, &s), DE_OK);
    ASSERT_EQ(de_gpu_fence_wait(f->d, f->fence, f->value, 30000000000ull), DE_OK);
}
static f32 trace(native_fx *f, de_gpu_cmd *c) {
    de_gpu_barrier barriers[3] = {
        {.accel = f->tlas, .before = DE_ACCESS_ACCEL_WRITE, .after = DE_ACCESS_ACCEL_READ},
        {.buffer = f->output,
         .before = f->queried ? DE_ACCESS_COPY_SRC : DE_ACCESS_NONE,
         .after = DE_ACCESS_SHADER_WRITE},
        {.buffer = f->readback,
         .before = f->queried ? DE_ACCESS_COPY_DST : DE_ACCESS_NONE,
         .after = DE_ACCESS_COPY_DST}};
    de_gpu_cmd_barriers(c, barriers, 3);
    de_gpu_cmd_set_pipeline(c, f->pipeline);
    const u32 pc[2] = {de_gpu_accel_bindless(f->d, f->tlas),
                       de_gpu_buffer_bindless(f->d, f->output)};
    de_gpu_cmd_push_constants(c, pc, sizeof pc);
    de_gpu_cmd_dispatch(c, 1, 1, 1);
    barriers[0] = (de_gpu_barrier){
        .buffer = f->output, .before = DE_ACCESS_SHADER_WRITE, .after = DE_ACCESS_COPY_SRC};
    de_gpu_cmd_barriers(c, barriers, 1);
    de_gpu_cmd_copy_buffer(c, f->readback, 0, f->output, 0, 4);
    finish(f, c);
    f->queried = true;
    f32 result;
    memcpy(&result, de_gpu_buffer_map(f->d, f->readback), 4);
    return result;
}
TEST_F(accel_native, test_accel_ray_query_hit, native_fx) {
    de_gpu_cmd *c = de_gpu_cmd_begin(f->d, DE_QUEUE_COMPUTE, "known triangle");
    de_gpu_cmd_build_blas(c, f->blas, false);
    de_gpu_cmd_build_tlas(c, f->tlas, f->instances, 1, false);
    f32 t = trace(f, c);
    EXPECT_TRUE(t > 1.999f && t < 2.001f);
    /* A second submission exercises command/scratch reuse and fixed resource states. */
    c = de_gpu_cmd_begin(f->d, DE_QUEUE_COMPUTE, "same triangle again");
    de_gpu_cmd_build_tlas(c, f->tlas, f->instances, 1, true);
    t = trace(f, c);
    EXPECT_TRUE(t > 1.999f && t < 2.001f);
}
TEST_F(accel_native, test_accel_u32_padded_float3, native_fx) {
    const f32 positions[12] = {-1, -1, 2, 99, 1, -1, 2, 99, 0, 1, 2, 99};
    const u32 indices[3] = {0, 1, 2};
    memcpy(de_gpu_buffer_map(f->d, f->vertices), positions, sizeof positions);
    memcpy(de_gpu_buffer_map(f->d, f->indices), indices, sizeof indices);
    de_gpu_blas_geometry g = {.vertices = f->vertices,
                              .vertex_stride = 16,
                              .vertex_count = 3,
                              .vertex_format = DE_FMT_RGB32_FLOAT,
                              .indices = f->indices,
                              .index_count = 3,
                              .opaque = true};
    de_gpu_accel b =
        de_gpu_accel_create(f->d, &(de_gpu_accel_desc){.geometries = &g, .geometry_count = 1});
    ASSERT_TRUE(b.id != 0);
    ((de_gpu_tlas_instance *)de_gpu_buffer_map(f->d, f->instances))->blas = b;
    de_gpu_cmd *c = de_gpu_cmd_begin(f->d, DE_QUEUE_COMPUTE, "u32 padded positions");
    de_gpu_cmd_build_blas(c, b, false);
    de_gpu_cmd_build_tlas(c, f->tlas, f->instances, 1, false);
    f32 t = trace(f, c);
    EXPECT_TRUE(t > 1.999f && t < 2.001f);
}
TEST_F(accel_native, test_accel_refit_moves_hit, native_fx) {
    de_gpu_cmd *c = de_gpu_cmd_begin(f->d, DE_QUEUE_COMPUTE, "before refit");
    de_gpu_cmd_build_blas(c, f->blas, false);
    de_gpu_cmd_build_tlas(c, f->tlas, f->instances, 1, false);
    f32 t = trace(f, c);
    EXPECT_TRUE(t > 1.999f && t < 2.001f);
    f32 *v = de_gpu_buffer_map(f->d, f->vertices);
    v[2] = v[5] = v[8] = 4;
    c = de_gpu_cmd_begin(f->d, DE_QUEUE_COMPUTE, "moved triangle refit");
    de_gpu_cmd_build_blas(c, f->blas, true);
    de_gpu_cmd_build_tlas(c, f->tlas, f->instances, 1, true);
    t = trace(f, c);
    EXPECT_TRUE(t > 3.999f && t < 4.001f);
}
TEST_F(accel_native, test_accel_compaction, native_fx) {
    de_gpu_cmd *c = de_gpu_cmd_begin(f->d, DE_QUEUE_COMPUTE, "build and compact size");
    de_gpu_cmd_build_blas(c, f->blas, false);
    de_gpu_cmd_accel_emit_compacted_size(c, f->blas);
    u64 bytes = 0;
    EXPECT_FALSE(de_gpu_accel_compacted_size(f->d, f->blas, &bytes));
    de_gpu_cmd_build_tlas(c, f->tlas, f->instances, 1, false);
    f32 before = trace(f, c);
    EXPECT_TRUE(before > 1.999f && before < 2.001f);
    ASSERT_TRUE(de_gpu_accel_compacted_size(f->d, f->blas, &bytes));
    EXPECT_TRUE(bytes && bytes <= de__gpu_get_accel(f->d, f->blas)->sizes.accel_bytes);
    c = de_gpu_cmd_begin(f->d, DE_QUEUE_COMPUTE, "compact copy");
    de_gpu_accel copy = de_gpu_cmd_accel_compact(c, f->blas);
    ASSERT_TRUE(copy.id != 0);
    finish(f, c);
    de_gpu_accel_destroy(f->d, f->blas);
    ((de_gpu_tlas_instance *)de_gpu_buffer_map(f->d, f->instances))->blas = copy;
    c = de_gpu_cmd_begin(f->d, DE_QUEUE_COMPUTE, "compacted triangle");
    de_gpu_cmd_build_tlas(c, f->tlas, f->instances, 1, true);
    f32 after = trace(f, c);
    EXPECT_TRUE(after > 1.999f && after < 2.001f);
    EXPECT_EQ(memcmp(&before, &after, 4), 0);
}
TEST_F(accel_native, test_accel_instance_layout_gpu, native_fx) {
    de_gpu_buffer in = de_gpu_buffer_create(
        f->d,
        &(de_gpu_buffer_desc){.size = 180, .usage = DE_USAGE_ACCEL_INPUT, .memory = DE_MEM_UPLOAD});
    de_gpu_buffer table = de_gpu_buffer_create(
        f->d,
        &(de_gpu_buffer_desc){.size = 64, .usage = DE_USAGE_STORAGE, .memory = DE_MEM_UPLOAD});
    de_gpu_buffer out = de_gpu_buffer_create(
        f->d, &(de_gpu_buffer_desc){.size = 192, .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC});
    de_gpu_buffer read = de_gpu_buffer_create(
        f->d,
        &(de_gpu_buffer_desc){.size = 192, .usage = DE_USAGE_COPY_DST, .memory = DE_MEM_READBACK});
    ASSERT_TRUE(in.id && table.id && out.id && read.id);
    de_gpu_tlas_instance records[3] = {{0}};
    de__gpu_accel_address addresses[4] = {{0}};
    addresses[1] = (de__gpu_accel_address){.handle = 0x100001, .address = 0x12345678abcdef00ull};
    addresses[2] = (de__gpu_accel_address){.handle = 0x200002, .address = 0xfedcba9876543200ull};
    u8 expected[192] = {0};
    for (u32 i = 0; i < 3; ++i) {
        for (u32 j = 0; j < 12; ++j)
            records[i].transform[j] = (f32)(i * 12 + j);
        records[i].custom_index = 10 + i;
        records[i].mask = 0xab;
        records[i].sbt_offset = 20 + i;
        records[i].flags = 5;
        records[i].blas.id = i == 0 ? 0x100001 : i == 1 ? 0x200002 : 0x100002;
        /* Independent byte reference, not the production CPU translator. */
        memcpy(expected + i * 64, records[i].transform, 48);
        const u32 custom = (10 + i) | (i < 2 ? 0xab000000u : 0), sbt = (20 + i) | 0x05000000u;
        const u64 address = i < 2 ? addresses[i + 1].address : 0;
        memcpy(expected + i * 64 + 48, &custom, 4);
        memcpy(expected + i * 64 + 52, &sbt, 4);
        memcpy(expected + i * 64 + 56, &address, 8);
    }
    memcpy(de_gpu_buffer_map(f->d, in), records, 180);
    memcpy(de_gpu_buffer_map(f->d, table), addresses, 64);
    de_gpu_cmd *c = de_gpu_cmd_begin(f->d, DE_QUEUE_COMPUTE, "instance ABI translation");
    de_gpu_barrier b = {.buffer = out, .before = DE_ACCESS_NONE, .after = DE_ACCESS_SHADER_WRITE};
    de_gpu_cmd_barriers(c, &b, 1);
    ASSERT_TRUE(f->d->vt->cmd_accel_translate(c, de__gpu_get_buffer(f->d, in),
                                              de__gpu_get_buffer(f->d, out),
                                              de__gpu_get_buffer(f->d, table), 3, 4));
    b.before = DE_ACCESS_SHADER_WRITE;
    b.after = DE_ACCESS_COPY_SRC;
    de_gpu_cmd_barriers(c, &b, 1);
    de_gpu_cmd_copy_buffer(c, read, 0, out, 0, 192);
    finish(f, c);
    EXPECT_EQ(memcmp(expected, de_gpu_buffer_map(f->d, read), 192), 0);
}
