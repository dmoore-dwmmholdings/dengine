#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "de_test.h"
#include "gpu/gpu_backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
TEST(rt_pipeline, sbt_layout) {
    de__gpu_sbt_layout l;
    ASSERT_EQ(de__gpu_sbt_layout_make(32, 32, 64, 2, 3, &l), DE_OK);
    EXPECT_EQ(l.raygen_size, 64u);
    EXPECT_EQ(l.miss_offset, 64u);
    EXPECT_EQ(l.miss_size, 64u);
    EXPECT_EQ(l.hit_offset, 128u);
    EXPECT_EQ(l.hit_size, 96u);
    EXPECT_EQ(l.bytes, 256u);
    ASSERT_EQ(de__gpu_sbt_layout_make(24, 32, 64, 2, 3, &l), DE_OK);
    EXPECT_EQ(l.stride, 32u);
    ASSERT_EQ(de__gpu_sbt_layout_make(16, 16, 16, 2, 3, &l), DE_OK);
    EXPECT_EQ(l.bytes, 96u);
    EXPECT_EQ(de__gpu_sbt_layout_make(32, 3, 64, 2, 3, &l), DE_ERR_INVALID_ARGUMENT);
    de_error_clear();
}
TEST(rt_pipeline, desc_validation) {
    de_gpu_device *d = NULL;
    ASSERT_EQ(de_gpu_device_create(&(de_gpu_device_desc){.backend = DE_GPU_NULL}, &d), DE_OK);
    de_gpu_pipeline_desc p = {.kind = DE_PIPE_RAYTRACING};
    EXPECT_FALSE(de_gpu_pipeline_create(d, &p).id);
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    de_error_clear();
    const u32 code = 1;
    p.raygen = (de_gpu_shader_blob){&code, 4, "raygen"};
    EXPECT_FALSE(de_gpu_pipeline_create(d, &p).id);
    EXPECT_EQ(de_error_last()->result, DE_ERR_UNSUPPORTED);
    de_error_clear();
    de_gpu_device_destroy(d);
}
#if DE_HAS_GPU_METAL
static void submit(de_gpu_device *d, de_gpu_cmd *c) {
    ASSERT_EQ(
        de_gpu_queue_submit(d, DE_QUEUE_COMPUTE, &(de_gpu_submit){.cmds = &c, .cmd_count = 1}),
        DE_OK);
    de_gpu_wait_idle(d);
    ASSERT_FALSE(de_gpu_device_is_lost(d));
}
static void metal_hit_groups(bool multi_geometry) {
    setenv("MTL_DEBUG_LAYER", "1", 1);
    de_tracking_allocator track;
    de_allocator alloc = de_tracking_allocator_init(&track, NULL, DE_MEM_RENDER);
    de_gpu_device *d = NULL;
    ASSERT_EQ(de_gpu_device_create(&(de_gpu_device_desc){.backend = DE_GPU_METAL,
                                                         .debug_layer = true,
                                                         .allocator = &alloc,
                                                         .pipeline_cache_dir = ""},
                                   &d),
              DE_OK);
    if (!de_gpu_get_caps(d)->rt_pipelines) {
        de_gpu_device_destroy(d);
        SKIP("Metal device has no RT function-table support");
    }
    FILE *file = fopen(DE_RT_TEST_METALLIB, "rb");
    ASSERT_TRUE(file);
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);
    void *code = malloc((size_t)size);
    ASSERT_TRUE(code);
    ASSERT_EQ(fread(code, 1, (size_t)size, file), (size_t)size);
    fclose(file);
    de_gpu_shader_blob miss[2] = {{code, (u64)size, "miss0"}, {code, (u64)size, "miss1"}};
    de_gpu_rt_hit_group groups[3] = {{{code, (u64)size, "closest0"}, {0}},
                                     {{code, (u64)size, "closest1"}, {code, (u64)size, "alpha"}},
                                     {{code, (u64)size, "closest2"}, {0}}};
    if (multi_geometry) {
        groups[0].closest_hit.entry = "closest_geometry";
        groups[0].any_hit = (de_gpu_shader_blob){code, (u64)size, "alpha_accept"};
        groups[1].any_hit.entry = "alpha_reject";
    }
    de_gpu_rt_desc rt = {.raygen = {code, (u64)size, multi_geometry ? "raygen_geometry" : "raygen"},
                         .miss = miss,
                         .miss_count = 2,
                         .hit_groups = groups,
                         .hit_group_count = 3,
                         .max_recursion = 1,
                         .payload_bytes = 64,
                         .attribute_bytes = 8};
    de_gpu_pipeline_desc pd = {.kind = DE_PIPE_RAYTRACING,
                               .rt = &rt,
                               .push_constant_bytes = 12,
                               .name = "S17 native SBT acceptance"};
    rt.payload_bytes = 65;
    EXPECT_FALSE(de_gpu_pipeline_create(d, &pd).id);
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    de_error_clear();
    rt.payload_bytes = 64;
    rt.max_recursion = 2;
    EXPECT_FALSE(de_gpu_pipeline_create(d, &pd).id);
    EXPECT_EQ(de_error_last()->result, DE_ERR_UNSUPPORTED);
    de_error_clear();
    rt.max_recursion = 1;
    de_gpu_pipeline pipeline = de_gpu_pipeline_create(d, &pd);
    ASSERT_TRUE(pipeline.id);
    free(code);
    const float vertices[] = {-.5f, -.5f, 0, .5f, -.5f, 0, .5f,  .5f, 0,
                              -.5f, -.5f, 0, .5f, .5f,  0, -.5f, .5f, 0};
    de_gpu_buffer v = de_gpu_buffer_create(
        d, &(de_gpu_buffer_desc){.size = sizeof vertices * (multi_geometry ? 2u : 1u),
                                 .usage = DE_USAGE_ACCEL_INPUT,
                                 .memory = DE_MEM_UPLOAD});
    memcpy(de_gpu_buffer_map(d, v), vertices, sizeof vertices);
    if (multi_geometry) {
        float *second = (float *)de_gpu_buffer_map(d, v) + sizeof vertices / sizeof vertices[0];
        memcpy(second, vertices, sizeof vertices);
        for (u32 i = 0; i < 6; ++i)
            second[i * 3] += 1;
    }
    de_gpu_blas_geometry geometry[2] = {{.vertices = v,
                                         .vertex_count = 6,
                                         .vertex_stride = 12,
                                         .vertex_format = DE_FMT_RGB32_FLOAT,
                                         .opaque = false}};
    geometry[1] = geometry[0];
    geometry[1].vertex_offset = sizeof vertices;
    de_gpu_accel blas =
        de_gpu_accel_create(d, &(de_gpu_accel_desc){.geometries = geometry,
                                                    .geometry_count = multi_geometry ? 2u : 1u});
    de_gpu_accel tlas =
        de_gpu_accel_create(d, &(de_gpu_accel_desc){.is_tlas = true, .max_instances = 3});
    de_gpu_buffer instances =
        de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = 3 * sizeof(de_gpu_tlas_instance),
                                                      .usage = DE_USAGE_ACCEL_INPUT,
                                                      .memory = DE_MEM_UPLOAD});
    de_gpu_tlas_instance *in = de_gpu_buffer_map(d, instances);
    memset(in, 0, 3 * sizeof *in);
    for (u32 i = 0; i < 3; ++i) {
        in[i].transform[0] = in[i].transform[5] = in[i].transform[10] = 1;
        in[i].transform[3] = (float)i * (multi_geometry ? 3.f : 2.f);
        in[i].mask = 255;
        in[i].blas = blas;
        in[i].custom_index = 100 + i;
        in[i].sbt_offset = i;
    }
    de_gpu_buffer output = de_gpu_buffer_create(
        d, &(de_gpu_buffer_desc){.size = 28, .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC});
    de_gpu_buffer readback = de_gpu_buffer_create(
        d,
        &(de_gpu_buffer_desc){.size = 28, .usage = DE_USAGE_COPY_DST, .memory = DE_MEM_READBACK});
    de_gpu_buffer pixels = de_gpu_buffer_create(
        d, &(de_gpu_buffer_desc){.size = 256, .usage = DE_USAGE_COPY_SRC, .memory = DE_MEM_UPLOAD});
    u8 *rgba = de_gpu_buffer_map(d, pixels);
    memset(rgba, 255, 256);
    rgba[3] = 0;
    de_gpu_texture texture = de_gpu_texture_create(
        d, &(de_gpu_texture_desc){.width = 2,
                                  .height = 1,
                                  .format = DE_FMT_RGBA8_UNORM,
                                  .usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST});
    de_gpu_cmd *cmd = de_gpu_cmd_begin(d, DE_QUEUE_COMPUTE, "S17 RT scene build");
    de_gpu_barrier b = {.texture = texture, .after = DE_ACCESS_COPY_DST};
    de_gpu_cmd_barriers(cmd, &b, 1);
    de_gpu_cmd_copy_buffer_to_texture(cmd, texture, 0, 0, pixels, 0, 256);
    b.before = DE_ACCESS_COPY_DST;
    b.after = DE_ACCESS_SHADER_READ;
    de_gpu_cmd_barriers(cmd, &b, 1);
    de_gpu_cmd_build_blas(cmd, blas, false);
    de_gpu_cmd_build_tlas(cmd, tlas, instances, 3, false);
    submit(d, cmd);
    const u32 pc[] = {de_gpu_buffer_bindless(d, output), de_gpu_accel_bindless(d, tlas),
                      de_gpu_texture_bindless(d, texture, 0, false)};
    de_tracking_stats warm = {0};
    for (u32 frame = 0; frame < 9; ++frame) {
        cmd = de_gpu_cmd_begin(d, DE_QUEUE_COMPUTE, "S17 dispatch rays");
        b = (de_gpu_barrier){.buffer = output,
                             .before = frame ? DE_ACCESS_COPY_SRC : DE_ACCESS_NONE,
                             .after = DE_ACCESS_SHADER_WRITE};
        de_gpu_cmd_barriers(cmd, &b, 1);
        de_gpu_cmd_set_pipeline(cmd, pipeline);
        de_gpu_cmd_push_constants(cmd, pc, sizeof pc);
        de_gpu_cmd_dispatch_rays(cmd, 7, 1, 1);
        b.before = DE_ACCESS_SHADER_WRITE;
        b.after = DE_ACCESS_COPY_SRC;
        de_gpu_cmd_barriers(cmd, &b, 1);
        de_gpu_cmd_copy_buffer(cmd, readback, 0, output, 0, 28);
        submit(d, cmd);
        const u32 *data = de_gpu_buffer_map(d, readback);
        const u32 single_expected[] = {10, 11, 12, 20, 21, 20, 11};
        const u32 multi_expected[] = {10, 20, 20, 12, 12, 20, 20};
        const u32 *expected = multi_geometry ? multi_expected : single_expected;
        for (u32 i = 0; i < 7; ++i) {
            printf("RT ray%u=%u expected%u\n", i, data[i], expected[i]);
            EXPECT_EQ(data[i], expected[i]);
        }
        if (!frame)
            warm = de_tracking_allocator_stats(&track);
    }
    de_tracking_stats end = de_tracking_allocator_stats(&track);
    EXPECT_EQ(end.alloc_count, warm.alloc_count);
    EXPECT_EQ(end.realloc_count, warm.realloc_count);
    de_gpu_pipeline_destroy(d, pipeline);
    de_gpu_accel_destroy(d, tlas);
    de_gpu_accel_destroy(d, blas);
    de_gpu_texture_destroy(d, texture);
    de_gpu_buffer buffers[] = {v, instances, output, readback, pixels};
    for (u32 i = 0; i < 5; ++i)
        de_gpu_buffer_destroy(d, buffers[i]);
    de_gpu_device_destroy(d);
    EXPECT_EQ(de_tracking_allocator_stats(&track).blocks_live, 0u);
}
TEST(rt_pipeline, metal_hit_groups_misses_and_any_hit_alpha) {
    metal_hit_groups(false);
}
TEST(rt_pipeline, metal_two_geometries_select_distinct_any_hit_groups) {
    metal_hit_groups(true);
}
#endif
