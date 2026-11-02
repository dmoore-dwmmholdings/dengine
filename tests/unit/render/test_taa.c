#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "de_test.h"
#include "render/post/jitter.h"
#include "render/upscale/upscaler.h"
#include <dengine/de_platform.h>
#include <math.h>
#include <string.h>
#if DE_TAA_SCENE_TEST
#include <dengine/de_asset.h>
#include <dengine/de_ecs.h>
#include <dengine/de_task.h>
#include "render/renderer_internal.h"
#include "render/views_internal.h"
#include "render/scene/gpu_scene.h"
#include "render/passes/vis/vis_stages.h"
#include "render/passes/ssr/ssr_internal.h"
#include "render/world/render_world.h"
#include "render/graph/rg_internal.h"
#include "loader_test_util.h"
#include "dpak_writer.h"
#include "flip.h"
#include <stdio.h>
#include <stdlib.h>
#endif

TEST(taa, shared_halton_table_and_period) {
    static const f32 expected[8][2] = {{0, -1.f / 6},       {-.25f, 1.f / 6},   {.25f, -7.f / 18},
                                       {-.375f, -1.f / 18}, {.125f, 5.f / 18},  {-.125f, -5.f / 18},
                                       {.375f, 1.f / 18},   {-.4375f, 7.f / 18}};
    for (u32 i = 0; i < 8; ++i) {
        de_vec2 sample = de__jitter_sample(i, 8);
        EXPECT_FLOAT_EQ(sample.x, expected[i][0], 1e-7);
        EXPECT_FLOAT_EQ(sample.y, expected[i][1], 1e-7);
        de_vec2 repeat = de__jitter_sample(i + 8, 8);
        EXPECT_MEMEQ(&sample, &repeat, sizeof sample);
    }
    EXPECT_EQ(de__jitter_phase_count(1920, 1080, 1920, 1080), 8u);
    EXPECT_EQ(de__jitter_phase_count(960, 540, 1920, 1080), 32u);
    EXPECT_EQ(de__jitter_phase_count(480, 270, 1920, 1080), 64u);
    EXPECT_EQ(de__jitter_phase_count(1, 1, UINT32_MAX, UINT32_MAX), 64u);
    EXPECT_EQ(de__jitter_phase_count(0, 1, 1920, 1080), 0u);
    de_vec2 invalid = de__jitter_sample(UINT64_MAX, 0);
    EXPECT_FLOAT_EQ(invalid.x, 0, 0);
    EXPECT_FLOAT_EQ(invalid.y, 0, 0);
    for (u32 phase = 8; phase <= 64; ++phase)
        for (u32 i = 0; i < phase; ++i) {
            de_vec2 s = de__jitter_sample(i, phase);
            EXPECT_TRUE(s.x >= -.5f && s.x < .5f && s.y >= -.5f && s.y < .5f);
        }
}
TEST(taa, homogeneous_projection_and_depth) {
    const de_mat4 matrices[2] = {de_m4_perspective_rz(1, 1.5f, .1f),
                                 de_m4_ortho_rz(-3, 3, -2, 2, .1f, 1000)};
    const de_vec2 jitter = {.25f, -.375f};
    for (u32 i = 0; i < 2; ++i) {
        de_mat4 p = de__jitter_projection(matrices[i], jitter, 640, 360);
        de_vec4 point = {.2f, .3f, -3, 1};
        de_vec4 a = de_m4_mul_v4(&matrices[i], point), b = de_m4_mul_v4(&p, point);
        EXPECT_FLOAT_EQ((b.x / b.w - a.x / a.w) * 320, -jitter.x, 1e-5);
        EXPECT_FLOAT_EQ((b.y / b.w - a.y / a.w) * -180, -jitter.y, 1e-5);
        EXPECT_FLOAT_EQ(a.z, b.z, 0);
        EXPECT_FLOAT_EQ(a.w, b.w, 0);
    }
}
TEST(taa, backend_selection_and_mip_bias) {
    const u32 vendors = (1u << DE_UPSCALE_FSR) | (1u << DE_UPSCALE_METALFX);
    EXPECT_EQ(de__upscaler_select(DE_UPSCALE_AUTO, DE_GPU_METAL, vendors), DE_UPSCALE_METALFX);
    EXPECT_EQ(de__upscaler_select(DE_UPSCALE_AUTO, DE_GPU_VULKAN, vendors), DE_UPSCALE_FSR);
    EXPECT_EQ(de__upscaler_select(DE_UPSCALE_AUTO, DE_GPU_D3D12, vendors), DE_UPSCALE_FSR);
    EXPECT_EQ(de__upscaler_select(DE_UPSCALE_AUTO, DE_GPU_NULL, vendors), DE_UPSCALE_TAA);
    EXPECT_EQ(de__upscaler_select(DE_UPSCALE_AUTO, DE_GPU_METAL, 0), DE_UPSCALE_TAA);
    EXPECT_EQ(de__upscaler_select(DE_UPSCALE_METALFX, DE_GPU_VULKAN, vendors), DE_UPSCALE_TAA);
    EXPECT_EQ(de__upscaler_select(DE_UPSCALE_FSR, DE_GPU_METAL, vendors), DE_UPSCALE_TAA);
    EXPECT_EQ(de__upscaler_select(DE_UPSCALE_NONE, DE_GPU_METAL, vendors), DE_UPSCALE_NONE);
    EXPECT_FLOAT_EQ(de__upscaler_mip_bias((de__upscale_size){32, 32, 64, 64}, .25f), -1.25, 0);
    EXPECT_FLOAT_EQ(de__upscaler_mip_bias((de__upscale_size){32, 32, 32, 32}, 1), 0, 0);
    EXPECT_FLOAT_EQ(de__upscaler_mip_bias((de__upscale_size){32, 32, 64, 64}, NAN), -1, 0);
}

/* Keep unavailable Metal fixtures out of MSVC's optimizer: an inlined unconditional
 * fixture skip makes the subsequent test body unreachable under /W4 /WX. */
#if DE_HAS_GPU_METAL
#define W           32u
#define H           32u
#define COLOR_PITCH (W * 16u)
#define COLOR_BYTES (COLOR_PITCH * H)
#define AUX_PITCH   256u
#define AUX_BYTES   (AUX_PITCH * H)
#define WAIT_NS     (120ull * 1000000000ull)
typedef struct taa_fx {
    de_gpu_device *device;
    de_renderer *renderer;
    de_tracking_allocator tracking;
    de_allocator allocator;
    void *state;
    de_gpu_texture inputs[4], output;
    de_gpu_buffer upload, readback;
    de_gpu_fence fence;
    de__upscale_history history;
    u64 frame;
    f32 pre_exposure, previous_pre_exposure, sharpen;
    f32 pixels[W * H * 4], ages[W * H * 4];
} taa_fx;
static void taa_fx_setup(taa_fx *f) {
    f->allocator = de_tracking_allocator_init(&f->tracking, NULL, DE_MEM_RENDER);
}
static void taa_fx_teardown(taa_fx *f) {
    if (f->device)
        de_gpu_wait_idle(f->device);
    de__taa_backend()->destroy(f->state);
    if (f->renderer)
        de_renderer_destroy(f->renderer);
    if (f->device) {
        for (u32 i = 0; i < 4; ++i)
            de_gpu_texture_destroy(f->device, f->inputs[i]);
        de_gpu_texture_destroy(f->device, f->output);
        de_gpu_buffer_destroy(f->device, f->upload);
        de_gpu_buffer_destroy(f->device, f->readback);
        de_gpu_fence_destroy(f->device, f->fence);
        de_gpu_device_destroy(f->device);
    }
    EXPECT_EQ(de_tracking_allocator_stats(&f->tracking).bytes_live, 0u);
}
static void open_taa(taa_fx *f) {
#if !DE_HAS_GPU_METAL
    (void)f;
    SKIP("Metal runtime requires macOS");
#else
    ASSERT_EQ(de_gpu_device_create(
                  &(de_gpu_device_desc){.backend = DE_GPU_METAL, .debug_layer = true}, &f->device),
              DE_OK);
    ASSERT_EQ(
        de_renderer_create(&(de_renderer_desc){.device = f->device, .allocator = &f->allocator},
                           &f->renderer),
        DE_OK);
    ASSERT_EQ(de__taa_backend()->init(f->renderer, &f->state), DE_OK);
    ASSERT_EQ(de__taa_backend()->resize(f->state, (de__upscale_size){W, H, W, H}, &f->history),
              DE_OK);
    const de_gpu_format formats[4] = {DE_FMT_RGBA32_FLOAT, DE_FMT_R32_FLOAT, DE_FMT_RG32_FLOAT,
                                      DE_FMT_R32_FLOAT};
    for (u32 i = 0; i < 4; ++i) {
        f->inputs[i] = de_gpu_texture_create(
            f->device, &(de_gpu_texture_desc){.width = W,
                                              .height = H,
                                              .format = formats[i],
                                              .usage = DE_USAGE_COPY_DST | DE_USAGE_SAMPLED});
        ASSERT_TRUE(f->inputs[i].id);
    }
    f->output = de_gpu_texture_create(
        f->device, &(de_gpu_texture_desc){.width = W,
                                          .height = H,
                                          .format = DE_FMT_RGBA32_FLOAT,
                                          .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC});
    f->upload =
        de_gpu_buffer_create(f->device, &(de_gpu_buffer_desc){.size = COLOR_BYTES + 3 * AUX_BYTES,
                                                              .usage = DE_USAGE_COPY_SRC,
                                                              .memory = DE_MEM_UPLOAD});
    f->readback = de_gpu_buffer_create(f->device, &(de_gpu_buffer_desc){.size = 2 * COLOR_BYTES,
                                                                        .usage = DE_USAGE_COPY_DST,
                                                                        .memory = DE_MEM_READBACK});
    f->fence = de_gpu_fence_create(f->device, 0);
    ASSERT_TRUE(f->output.id && f->upload.id && f->readback.id && f->fence.id);
#endif
}
static void fill(taa_fx *f, bool gradient, f32 offset, f32 depth, de_vec2 motion) {
    u8 *bytes = de_gpu_buffer_map(f->device, f->upload);
    memset(bytes, 0, COLOR_BYTES + 3 * AUX_BYTES);
    for (u32 y = 0; y < H; ++y)
        for (u32 x = 0; x < W; ++x) {
            f32 *color = (f32 *)(bytes + y * COLOR_PITCH) + x * 4;
            color[0] = color[1] = color[2] = gradient ? ((f32)x + offset) / (W - 1) : offset;
            color[3] = 1;
            ((f32 *)(bytes + COLOR_BYTES + y * AUX_PITCH))[x] = depth;
            f32 *velocity = (f32 *)(bytes + COLOR_BYTES + AUX_BYTES + y * AUX_PITCH) + x * 2;
            velocity[0] = motion.x;
            velocity[1] = motion.y;
        }
}
static void step(taa_fx *f, bool reset) {
    de_gpu_cmd *cmd = de_gpu_cmd_begin(f->device, DE_QUEUE_GRAPHICS, "TAA component qualification");
    ASSERT_TRUE(cmd);
    const u32 write = (u32)(f->frame & 1), read = 1 - write;
    for (u32 i = 0; i < 4; ++i) {
        de_gpu_barrier b = {.texture = f->inputs[i],
                            .before = f->frame ? DE_ACCESS_SHADER_READ : DE_ACCESS_NONE,
                            .after = DE_ACCESS_COPY_DST};
        de_gpu_cmd_barriers(cmd, &b, 1);
        de_gpu_cmd_copy_buffer_to_texture(cmd, f->inputs[i], 0, 0, f->upload,
                                          i ? COLOR_BYTES + (i - 1) * AUX_BYTES : 0,
                                          i ? AUX_PITCH : COLOR_PITCH);
        b.before = DE_ACCESS_COPY_DST;
        b.after = DE_ACCESS_SHADER_READ;
        de_gpu_cmd_barriers(cmd, &b, 1);
    }
    de_gpu_barrier barriers[5] = {{.texture = f->history.color[write],
                                   .before = f->frame > 1 ? DE_ACCESS_SHADER_READ : DE_ACCESS_NONE,
                                   .after = DE_ACCESS_SHADER_WRITE},
                                  {.texture = f->history.depth[write],
                                   .before = f->frame > 1 ? DE_ACCESS_SHADER_READ : DE_ACCESS_NONE,
                                   .after = DE_ACCESS_SHADER_WRITE},
                                  {.texture = f->history.color[read],
                                   .before = f->frame ? DE_ACCESS_SHADER_READ : DE_ACCESS_NONE,
                                   .after = DE_ACCESS_SHADER_READ},
                                  {.texture = f->history.depth[read],
                                   .before = f->frame ? DE_ACCESS_SHADER_READ : DE_ACCESS_NONE,
                                   .after = DE_ACCESS_SHADER_READ},
                                  {.texture = f->output,
                                   .before = f->frame ? DE_ACCESS_COPY_SRC : DE_ACCESS_NONE,
                                   .after = DE_ACCESS_SHADER_WRITE}};
    de_gpu_cmd_barriers(cmd, barriers, 5);
    de__upscale_inputs in = {.color = f->inputs[0],
                             .depth = f->inputs[1],
                             .motion = f->inputs[2],
                             .reactive = f->inputs[3],
                             .history_color = f->history.color[read],
                             .history_depth = f->history.depth[read],
                             .size = {W, H, W, H},
                             .pre_exposure = f->pre_exposure,
                             .previous_pre_exposure = f->previous_pre_exposure,
                             .sharpen = f->sharpen,
                             .reset = reset,
                             .linear_depth = true};
    de__upscale_outputs out = {f->output, f->history.color[write], f->history.depth[write]};
    de__taa_backend()->dispatch(f->state, cmd, &in, &out);
    de_gpu_barrier b = {
        .texture = f->output, .before = DE_ACCESS_SHADER_WRITE, .after = DE_ACCESS_COPY_SRC};
    de_gpu_cmd_barriers(cmd, &b, 1);
    de_gpu_cmd_copy_texture_to_buffer(cmd, f->readback, 0, COLOR_PITCH, f->output, 0, 0);
    b.texture = f->history.color[write];
    de_gpu_cmd_barriers(cmd, &b, 1);
    de_gpu_cmd_copy_texture_to_buffer(cmd, f->readback, COLOR_BYTES, COLOR_PITCH,
                                      f->history.color[write], 0, 0);
    b.before = DE_ACCESS_COPY_SRC;
    b.after = DE_ACCESS_SHADER_READ;
    de_gpu_cmd_barriers(cmd, &b, 1);
    b.texture = f->history.depth[write];
    b.before = DE_ACCESS_SHADER_WRITE;
    de_gpu_cmd_barriers(cmd, &b, 1);
    const u64 serial = ++f->frame;
    ASSERT_EQ(de_gpu_queue_submit(
                  f->device, DE_QUEUE_GRAPHICS,
                  &(de_gpu_submit){
                      .cmds = &cmd, .cmd_count = 1, .signal = f->fence, .signal_value = serial}),
              DE_OK);
    ASSERT_EQ(de_gpu_fence_wait(f->device, f->fence, serial, WAIT_NS), DE_OK);
    const u8 *readback = de_gpu_buffer_map(f->device, f->readback);
    memcpy(f->pixels, readback, COLOR_BYTES);
    memcpy(f->ages, readback + COLOR_BYTES, COLOR_BYTES);
}
TEST_F(taa, native_history_reprojection_rejection_and_resize, taa_fx) {
    open_taa(f);
    fill(f, false, .2f, 2, (de_vec2){0});
    step(f, true);
    EXPECT_FLOAT_EQ(f->pixels[(16 * W + 16) * 4], .2, 1e-6);
    for (u32 i = 0; i < 10; ++i)
        step(f, false);
    EXPECT_FLOAT_EQ(f->ages[(16 * W + 16) * 4 + 3], 11, 0);
    const de_tracking_stats before = de_tracking_allocator_stats(&f->tracking);
    const de__upscale_history previous = f->history;
    for (u32 i = 0; i < 16; ++i) {
        ASSERT_EQ(de__taa_backend()->resize(f->state, (de__upscale_size){W, H, W, H}, &f->history),
                  DE_OK);
        step(f, false);
    }
    EXPECT_MEMEQ(&f->history, &previous, sizeof previous);
    EXPECT_EQ(de_tracking_allocator_stats(&f->tracking).alloc_count, before.alloc_count);
    fill(f, true, 0, 2, (de_vec2){0});
    step(f, true);
    fill(f, true, -1, 2, (de_vec2){2.f / W, 0});
    step(f, false);
    EXPECT_FLOAT_EQ(f->pixels[(16 * W + 16) * 4], 15.f / (W - 1), 1e-5);
    /* Newly revealed background must replace the old surface, including its accumulated age. */
    fill(f, false, .8f, 8, (de_vec2){0});
    step(f, false);
    EXPECT_FLOAT_EQ(f->pixels[(16 * W + 16) * 4], .8, 1e-6);
    EXPECT_FLOAT_EQ(f->ages[(16 * W + 16) * 4 + 3], 1, 0);
    fill(f, false, .1f, 8, (de_vec2){0});
    step(f, true);
    EXPECT_FLOAT_EQ(f->pixels[(16 * W + 16) * 4], .1, 1e-6);
    EXPECT_FLOAT_EQ(f->ages[(16 * W + 16) * 4 + 3], 1, 0);
    EXPECT_EQ(de__taa_backend()->resize(f->state, (de__upscale_size){0, H, W, H}, &f->history),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_MEMEQ(&f->history, &previous, sizeof previous);
    /* Dynamic internal scale must not recreate output-resolution history. */
    ASSERT_EQ(
        de__taa_backend()->resize(f->state, (de__upscale_size){W / 2, H / 2, W, H}, &f->history),
        DE_OK);
    EXPECT_MEMEQ(&f->history, &previous, sizeof previous);
    ASSERT_EQ(
        de__taa_backend()->resize(f->state, (de__upscale_size){W, H, W * 2, H * 2}, &f->history),
        DE_OK);
    EXPECT_NE(f->history.color[0].id, previous.color[0].id);
    EXPECT_NE(f->history.depth[1].id, previous.depth[1].id);
}
TEST_F(taa, native_pre_exposure_reactive_and_sharpen_contract, taa_fx) {
    open_taa(f);
    fill(f, true, 0, 2, (de_vec2){0});
    step(f, true);
    fill(f, true, 0, 2, (de_vec2){0});
    f32 *upload = de_gpu_buffer_map(f->device, f->upload);
    for (u32 i = 0; i < W * H; ++i)
        for (u32 c = 0; c < 3; ++c)
            upload[i * 4 + c] *= 2;
    f->pre_exposure = 2;
    f->previous_pre_exposure = 1;
    step(f, false);
    EXPECT_FLOAT_EQ(f->pixels[(16 * W + 16) * 4], 32.f / (W - 1), 1e-5);
    f->pre_exposure = f->previous_pre_exposure = 1;
    fill(f, false, .2f, 2, (de_vec2){0});
    step(f, true);
    for (u32 i = 0; i < 16; ++i)
        step(f, false);
    fill(f, false, 0, 2, (de_vec2){0});
    u8 *bytes = de_gpu_buffer_map(f->device, f->upload);
    for (u32 c = 0; c < 3; ++c)
        ((f32 *)(bytes + 16 * COLOR_PITCH))[16 * 4 + c] = .8f;
    for (u32 y = 0; y < H; ++y)
        for (u32 x = 0; x < W; ++x)
            ((f32 *)(bytes + COLOR_BYTES + 2 * AUX_BYTES + y * AUX_PITCH))[x] = 1;
    step(f, false);
    const f32 reactive = f->pixels[(16 * W + 16) * 4];
    step(f, true);
    const f32 reset = f->pixels[(16 * W + 16) * 4];
    EXPECT_FLOAT_EQ(reactive, reset,
                    1e-6); /* Reactive 1 is a complete current-frame replacement. */
    f->sharpen = 1;
    step(f, true);
    EXPECT_TRUE(f->pixels[(16 * W + 16) * 4] > reset + .1f);
    EXPECT_FLOAT_EQ(f->ages[(16 * W + 16) * 4], reset, 1e-6); /* Sharpen stays out of history. */
}

#if DE_TAA_SCENE_TEST
/* Two real renderer views of the same loaded geometry: a temporal 64px view and a spatial
 * 256px view. The latter supplies an independent 4x4 (16 sample) box-filter reference. */
#define SCENE_SIZE 64u
#define REF_SIZE   (SCENE_SIZE * 4u)
typedef struct taa_scene {
    de_tracking_allocator tracking;
    de_allocator allocator;
    de_task_system *tasks;
    de_type_registry *types;
    de_asset_system *assets;
    de_world *world;
    de_gpu_device *device;
    de_renderer *renderer;
    de_load_handle load;
    de_gpu_texture targets[2];
    de_gpu_buffer readbacks[2];
    de_gpu_fence fence;
    de_view views[2];
    de_entity cameras[2], object;
    de_mesh mesh;
    de_material material;
    de_builtin_component_ids bi;
    de_component transform, geometry, camera;
    u64 serial;
    u32 projection_checks;
    de_vec2 previous_jitter;
    char pack[128];
    u8 image[SCENE_SIZE * SCENE_SIZE * 4], reference[REF_SIZE * REF_SIZE * 4];
} taa_scene;
static void taa_scene_setup(taa_scene *f) {
    de_fp_env_init_thread();
    f->allocator = de_tracking_allocator_init(&f->tracking, NULL, DE_MEM_RENDER);
    ASSERT_EQ(de_fs_mount("taa", DE_TAA_TMP, DE_MOUNT_WRITABLE, 0), DE_OK);
}
static void taa_scene_teardown(taa_scene *f) {
    if (f->device)
        de_gpu_wait_idle(f->device);
    if (f->assets && f->load.id)
        de_asset_release(f->assets, f->load);
    if (f->assets)
        de_asset_system_destroy(f->assets);
    de_renderer_destroy(f->renderer);
    if (f->world)
        de_world_destroy(f->world);
    if (f->device) {
        for (u32 i = 0; i < 2; ++i) {
            de_gpu_texture_destroy(f->device, f->targets[i]);
            de_gpu_buffer_destroy(f->device, f->readbacks[i]);
        }
        de_gpu_fence_destroy(f->device, f->fence);
        de_gpu_device_destroy(f->device);
    }
    if (f->types)
        de_type_registry_destroy(f->types);
    if (f->tasks)
        de_task_system_destroy(f->tasks);
    if (f->pack[0])
        EXPECT_EQ(de_file_delete(f->pack), DE_OK);
    (void)de_cvar_set(de_cvar_find("r.pipelined"), "1");
    (void)de_cvar_set(de_cvar_find("r.taa.sharpen"), "0");
    (void)de_cvar_set(de_cvar_find("r.taa.mip_bias"), "0");
    EXPECT_EQ(de_tracking_allocator_stats(&f->tracking).bytes_live, 0u);
    EXPECT_EQ(de_fs_unmount("taa", DE_TAA_TMP), DE_OK);
}
#if DE_HAS_GPU_METAL
static lt_blob scene_plane(u64 material) {
    lt_blob b = lt_dmsh(4, material, 0);
    for (u32 c = 0; c < 3; ++c) {
        de_cooked_put_f32(b.data + 128 + c * 4, 0);
        de_cooked_put_f32(b.data + 140 + c * 4, 1.f / 65535);
    }
    de_cooked_put_f32(b.data + 160, 1.f / 65535);
    de_cooked_put_f32(b.data + 164, 1.f / 65535);
    const u32 streams = de_cooked_get_u32(b.data + 56);
    for (u32 s = 0; s < 3; ++s) {
        const u8 *record = b.data + streams + s * DE_COOKED_MESH_STREAM_SIZE;
        const u32 offset = de_cooked_get_u32(record + 8), bytes = de_cooked_get_u32(record + 12);
        memset(b.data + offset, 0, bytes);
        for (u32 i = 0; i < 4; ++i) {
            const u16 values[4] = {(i & 1) ? 65535 : 0, (i & 2) ? 65535 : 0, 32768, 0};
            if (s == 1)
                continue;
            for (u32 c = 0; c < (s == 0 ? 4u : 2u); ++c) {
                const u32 at = offset + i * (s == 0 ? 8 : 4) + c * 2;
                b.data[at] = (u8)values[c];
                b.data[at + 1] = (u8)(values[c] >> 8);
            }
        }
    }
    const u8 indices[6] = {0, 1, 2, 2, 1, 3};
    for (u32 i = 0; i < 6; ++i)
        de_cooked_put_u32(b.data + de_cooked_get_u32(b.data + 84) + i * 4, indices[i]);
    memcpy(b.data + de_cooked_get_u32(b.data + 80), indices, sizeof indices);
    de_cooked_put_f32(b.data + de_cooked_get_u32(b.data + 72) + 28, 2);
    return b;
}
static void scene_assets(taa_scene *f) {
    const de_asset_id mesh_id = de_asset_id_from("taa/plane"),
                      mat_id = de_asset_id_from("taa/white");
    lt_blob mesh = scene_plane(mat_id.hash), mat = lt_dmat(NULL, NULL, 0, 1);
    snprintf(f->pack, sizeof f->pack, "taa:/scene-%llu.dpak", (unsigned long long)de_time_now_ns());
    dpak_writer *writer = NULL;
    ASSERT_EQ(dpak_writer_create(&(dpak_writer_desc){.include_names = true}, &writer), DE_OK);
    ASSERT_EQ(dpak_writer_add(writer, mesh_id.hash, "mesh", mesh.data, mesh.size, &mat_id.hash, 1,
                              "taa/plane"),
              DE_OK);
    ASSERT_EQ(
        dpak_writer_add(writer, mat_id.hash, "material", mat.data, mat.size, NULL, 0, "taa/white"),
        DE_OK);
    const u64 ids[] = {mesh_id.hash, mat_id.hash};
    ASSERT_EQ(dpak_writer_add_bundle(writer, "taa", ids, 2, NULL, 0, false, 0), DE_OK);
    ASSERT_EQ(dpak_writer_finish(writer, f->pack), DE_OK);
    dpak_writer_destroy(writer);
    lt_free(&mesh);
    lt_free(&mat);
    ASSERT_EQ(de_asset_pack_mount(f->assets, f->pack, 0), DE_OK);
    f->load = de_asset_load_bundle(f->assets, "taa", 0);
    de_load_state state = DE_LOAD_LOADING;
    const u64 deadline = de_time_now_ns() + WAIT_NS;
    while ((state == DE_LOAD_LOADING || state == DE_LOAD_PENDING) && de_time_now_ns() < deadline) {
        de_renderer_pump_uploads(f->renderer);
        state = de_asset_wait(f->assets, f->load, 1000000);
    }
    ASSERT_EQ(state, DE_LOAD_READY);
    f->mesh = de_asset_mesh(f->assets, mesh_id);
    f->material = de_material_create_instance(f->renderer, de_asset_material(f->assets, mat_id));
    ASSERT_TRUE(f->mesh.id && f->material.id);
    de_standard_material params;
    ASSERT_EQ(de_material_get_params(f->renderer, f->material, &params), DE_OK);
    params.unlit = true;
    params.alpha_mode = DE_COOKED_ALPHA_OPAQUE;
    params.base_color = (de_vec4){.6f, .6f, .6f, 1};
    ASSERT_EQ(de_material_set_params(f->renderer, f->material, &params), DE_OK);
}
#endif
static de_entity scene_object(taa_scene *f, de_dvec3 position, de_vec3 scale, f32 angle) {
    const de_component fields[] = {f->transform, f->geometry};
    de_entity entity = DE_ENTITY_NULL;
    ASSERT_EQ(de_entity_spawn_with(f->world, fields, 2, &entity), DE_OK);
    DeTransform *t = de_entity_get(f->world, entity, f->transform);
    *t = de_transform_identity();
    t->position = position;
    t->scale = scale;
    t->rotation = (de_quat){0, 0, sinf(angle * .5f), cosf(angle * .5f)};
    DeMeshRenderer *m = de_entity_get(f->world, entity, f->geometry);
    m->mesh = f->mesh;
    m->materials[0] = f->material;
    return entity;
}
static void open_scene(taa_scene *f, f32 scale) {
#if !DE_HAS_GPU_METAL
    (void)f;
    (void)scale;
    SKIP("Metal scene qualification requires macOS");
#else
    ASSERT_EQ(de_task_system_create(
                  &(de_task_system_desc){.allocator = &f->allocator, .worker_count = 2}, &f->tasks),
              DE_OK);
    ASSERT_EQ(de_type_registry_create(&f->allocator, &f->types), DE_OK);
    ASSERT_EQ(de_asset_system_create(&(de_asset_system_desc){.allocator = &f->allocator,
                                                             .tasks = f->tasks,
                                                             .io_threads = 1,
                                                             .cache_dir = "taa:/cache",
                                                             .types = f->types},
                                     &f->assets),
              DE_OK);
    ASSERT_EQ(de_gpu_device_create(&(de_gpu_device_desc){.backend = DE_GPU_METAL,
                                                         .allocator = &f->allocator,
                                                         .debug_layer = true},
                                   &f->device),
              DE_OK);
    ASSERT_EQ(de_renderer_create(&(de_renderer_desc){.device = f->device,
                                                     .assets = f->assets,
                                                     .types = f->types,
                                                     .allocator = &f->allocator},
                                 &f->renderer),
              DE_OK);
    ASSERT_EQ(
        de_world_create(
            &(de_world_desc){.allocator = &f->allocator, .types = f->types, .tasks = f->tasks},
            &f->world),
        DE_OK);
    de_renderer_attach(f->renderer, f->world);
    ASSERT_EQ(de_cvar_set(de_cvar_find("r.pipelined"), "0"), DE_OK);
    de_render_settings settings = {
        .render_scale = 1, .upscaler = DE_UPSCALE_TAA, .path_max_bounces = 1};
    de_renderer_set_settings(f->renderer, &settings);
    f->bi = de_builtin_components(f->world);
    f->transform = f->bi.transform;
    f->geometry = de_component_find(f->world, "DeMeshRenderer");
    f->camera = de_component_find(f->world, "DeCamera");
    scene_assets(f);
    for (u32 i = 0; i < 2; ++i) {
        const u32 size = i ? REF_SIZE : SCENE_SIZE;
        f->targets[i] = de_gpu_texture_create(
            f->device, &(de_gpu_texture_desc){.width = size,
                                              .height = size,
                                              .format = DE_FMT_RGBA8_UNORM,
                                              .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED |
                                                       DE_USAGE_COPY_SRC});
        f->readbacks[i] =
            de_gpu_buffer_create(f->device, &(de_gpu_buffer_desc){.size = (u64)size * size * 4,
                                                                  .usage = DE_USAGE_COPY_DST,
                                                                  .memory = DE_MEM_READBACK});
        ASSERT_TRUE(f->targets[i].id && f->readbacks[i].id);
        f->views[i] =
            de_view_create(f->renderer, &(de_view_desc){.target = f->targets[i],
                                                        .name = i ? "taa.reference16" : "taa.test",
                                                        .order = (i32)i});
        ASSERT_TRUE(f->views[i].id);
        if (i)
            settings.upscaler = DE_UPSCALE_NONE;
        settings.render_scale = i ? 1 : scale;
        de_view_set_settings(f->renderer, f->views[i], &settings);
        const de_component fields[] = {f->transform, f->camera};
        ASSERT_EQ(de_entity_spawn_with(f->world, fields, 2, &f->cameras[i]), DE_OK);
        DeTransform *t = de_entity_get(f->world, f->cameras[i], f->transform);
        *t = de_transform_identity();
        t->position.z = 5;
        DeCamera *c = de_entity_get(f->world, f->cameras[i], f->camera);
        c->tonemapper = de_tonemapper_builtin(f->renderer, DE_TM_NONE);
        c->view = f->views[i];
        c->projection = DE_PROJ_ORTHOGRAPHIC;
        c->ortho_height = 4;
        c->exposure_ev100 = 0;
    }
    f->fence = de_gpu_fence_create(f->device, 0);
    ASSERT_TRUE(f->fence.id);
#endif
}
static void scene_frame(taa_scene *f, bool read) {
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_POST_UPDATE, 1.f / 60, 0), DE_OK);
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_EXTRACT, 1.f / 60, 0), DE_OK);
    ASSERT_EQ(de__render_extract_and_kick(f->renderer, f->world), DE_OK);
    de__render_world_wait_idle(f->renderer);
    if (!read)
        return;
    de_gpu_cmd *cmd = de_gpu_cmd_begin(f->device, DE_QUEUE_GRAPHICS, "TAA scene readback");
    ASSERT_TRUE(cmd);
    for (u32 i = 0; i < 2; ++i) {
        de_gpu_barrier b = {
            .texture = f->targets[i], .before = DE_ACCESS_SHADER_READ, .after = DE_ACCESS_COPY_SRC};
        de_gpu_cmd_barriers(cmd, &b, 1);
        de_gpu_cmd_copy_texture_to_buffer(cmd, f->readbacks[i], 0, (i ? REF_SIZE : SCENE_SIZE) * 4,
                                          f->targets[i], 0, 0);
        b.before = DE_ACCESS_COPY_SRC;
        b.after = DE_ACCESS_SHADER_READ;
        de_gpu_cmd_barriers(cmd, &b, 1);
    }
    const u64 serial = ++f->serial;
    ASSERT_EQ(de_gpu_queue_submit(
                  f->device, DE_QUEUE_GRAPHICS,
                  &(de_gpu_submit){
                      .cmds = &cmd, .cmd_count = 1, .signal = f->fence, .signal_value = serial}),
              DE_OK);
    ASSERT_EQ(de_gpu_fence_wait(f->device, f->fence, serial, WAIT_NS), DE_OK);
    memcpy(f->image, de_gpu_buffer_map(f->device, f->readbacks[0]), sizeof f->image);
    memcpy(f->reference, de_gpu_buffer_map(f->device, f->readbacks[1]), sizeof f->reference);
}
static u8 scene_srgb(f32 c) {
    c = fminf(1, fmaxf(0, c));
    return (u8)lroundf(255 * (c <= .0031308f ? 12.92f * c : 1.055f * powf(c, 1.f / 2.4f) - .055f));
}
static f64 scene_flip(taa_scene *f) {
    u8 ref[SCENE_SIZE * SCENE_SIZE * 3], image[SCENE_SIZE * SCENE_SIZE * 3];
    f32 errors[SCENE_SIZE * SCENE_SIZE];
    u32 covered = 0;
    for (u32 y = 0; y < SCENE_SIZE; ++y)
        for (u32 x = 0; x < SCENE_SIZE; ++x) {
            const u32 p = y * SCENE_SIZE + x;
            for (u32 c = 0; c < 3; ++c) {
                u32 sum = 0;
                for (u32 sy = 0; sy < 4; ++sy)
                    for (u32 sx = 0; sx < 4; ++sx)
                        sum += f->reference[((y * 4 + sy) * REF_SIZE + x * 4 + sx) * 4 + c];
                ref[p * 3 + c] = scene_srgb((f32)sum / (16 * 255));
                image[p * 3 + c] = scene_srgb((f32)f->image[p * 4 + c] / 255);
            }
            covered += f->image[p * 4] > 80;
        }
    EXPECT_GT(covered, 16u); /* Reject a clear-only/no-geometry false pass. */
    de_flip_image a = {ref, SCENE_SIZE, SCENE_SIZE, 3, 0};
    de_flip_image b = {image, SCENE_SIZE, SCENE_SIZE, 3, 0};
    ASSERT_TRUE(de_flip_ldr(&a, &b, 0, errors));
    de_flip_stats stats;
    ASSERT_TRUE(de_flip_stats_compute(errors, SCENE_SIZE * SCENE_SIZE, &stats));
    ASSERT_EQ(de_file_write_all_atomic("taa:/image.rgba", f->image, sizeof f->image), DE_OK);
    ASSERT_EQ(de_file_write_all_atomic("taa:/reference.rgba", f->reference, sizeof f->reference),
              DE_OK);
    return stats.mean;
}
TEST_F(taa, rendered_static_scene_reference16_and_edge_stability, taa_scene) {
    open_scene(f, 1);
    f->object = scene_object(f, (de_dvec3){-.9, -.8, 0}, (de_vec3){1.7f, 1.3f, 1}, .37f);
    for (u32 i = 0; i < 96; ++i)
        scene_frame(f, false);
    scene_frame(f, true);
    const f64 error = scene_flip(f);
    printf("TAA rendered scene 16xSSAA FLIP mean %.9g\n", error);
    EXPECT_TRUE(error <= .04);
    /* Measure every partially covered reference edge pixel through two complete periods. */
    f64 sum[SCENE_SIZE * SCENE_SIZE] = {0}, sum2[SCENE_SIZE * SCENE_SIZE] = {0};
    for (u32 frame = 0; frame < 16; ++frame) {
        scene_frame(f, true);
        for (u32 i = 0; i < SCENE_SIZE * SCENE_SIZE; ++i) {
            const f64 value = (f64)f->image[i * 4] / 255;
            sum[i] += value;
            sum2[i] += value * value;
        }
    }
    f64 worst = 0;
    u32 edges = 0;
    for (u32 y = 1; y + 1 < SCENE_SIZE; ++y)
        for (u32 x = 1; x + 1 < SCENE_SIZE; ++x) {
            u32 reference = 0;
            for (u32 sy = 0; sy < 4; ++sy)
                for (u32 sx = 0; sx < 4; ++sx)
                    reference += f->reference[((y * 4 + sy) * REF_SIZE + x * 4 + sx) * 4];
            if (reference > 16 * 10 && reference < 16 * 115) {
                const u32 i = y * SCENE_SIZE + x;
                worst = fmax(worst, sqrt(fmax(0, sum2[i] / 16 - (sum[i] / 16) * (sum[i] / 16))));
                ++edges;
            }
        }
    printf("TAA rendered static edges %u max temporal stddev %.9g\n", edges, worst);
    EXPECT_GT(edges, 30u);
    EXPECT_TRUE(worst < .01);
}
TEST_F(taa, rendered_half_scale_one_pixel_lines, taa_scene) {
    open_scene(f, .5f);
    de_entity lines[7];
    for (u32 i = 0; i < 7; ++i)
        lines[i] = scene_object(f, (de_dvec3){-1.5 + (f64)i * .4, -1.2, 0},
                                (de_vec3){4.f / SCENE_SIZE, 2.4f, 1}, .19f);
    for (u32 i = 0; i < 128; ++i)
        scene_frame(f, false);
    scene_frame(f, true);
    const f64 error = scene_flip(f);
    printf("TAAU half-scale one-pixel lines 16xSSAA FLIP mean %.9g\n", error);
    EXPECT_TRUE(error <= .04);
    /* Static color rejection must preserve legitimate colored subpixel coverage too. */
    de_material colored[3];
    for (u32 c = 0; c < 3; ++c) {
        colored[c] = de_material_create_instance(f->renderer, f->material);
        ASSERT_TRUE(colored[c].id);
        de_standard_material params;
        ASSERT_EQ(de_material_get_params(f->renderer, colored[c], &params), DE_OK);
        params.base_color = (de_vec4){c == 0 ? .6f : 0, c == 1 ? .6f : 0, c == 2 ? .6f : 0, 1};
        ASSERT_EQ(de_material_set_params(f->renderer, colored[c], &params), DE_OK);
    }
    for (u32 i = 0; i < 7; ++i) {
        DeMeshRenderer *mesh = de_entity_get(f->world, lines[i], f->geometry);
        ASSERT_TRUE(mesh);
        mesh->materials[0] = colored[i % 3];
    }
    for (u32 i = 0; i < 128; ++i)
        scene_frame(f, false);
    scene_frame(f, true);
    const f64 colored_error = scene_flip(f);
    printf("TAAU half-scale RGB one-pixel lines 16xSSAA FLIP mean %.9g\n", colored_error);
    EXPECT_TRUE(colored_error <= .04);
}
static de__gs_view scene_constants(taa_scene *f, u32 index) {
    de__gs_view constants;
    const u32 slot = (f->views[index].id & 255u) - 1u;
    de__gpu_scene_debug_pack(f->renderer->gpu_scene, DE__GS_VIEWS, slot, &constants);
    return constants;
}
static void verify_static_projection(de__view_frame *frame, void *user) {
    taa_scene *f = user;
    if (frame->view.id != f->views[0].id)
        return;
    const de__vis_context *context = de__vis_frame_context(de__views_vis(f->renderer), frame->view,
                                                           frame->graph, frame->frame_index);
    ASSERT_TRUE(context != NULL);
    EXPECT_EQ(context->occlusion_reset, f->projection_checks == 0);
    const de_vec2 previous = f->projection_checks ? f->previous_jitter : frame->jitter;
    EXPECT_FLOAT_EQ(context->raster_history_uv_offset.x,
                    (frame->jitter.x - previous.x) / frame->width, 1e-7);
    EXPECT_FLOAT_EQ(context->raster_history_uv_offset.y,
                    (frame->jitter.y - previous.y) / frame->height, 1e-7);
    f->previous_jitter = frame->jitter;
    ++f->projection_checks;
}
TEST_F(taa, rendered_view_jitter_resets_motion_and_steady_allocations, taa_scene) {
    open_scene(f, .5f);
    ASSERT_EQ(de__view_register_stage(f->renderer, DE__VIEW_SLOT_POST, 1, "test.camera_policy",
                                      verify_static_projection, f),
              DE_OK);
    (void)scene_object(f, (de_dvec3){-.8, -.8, 0}, (de_vec3){1.6f, 1.6f, 1}, .17f);
    for (u32 i = 0; i < 8; ++i) {
        scene_frame(f, false);
        const de__gs_view v = scene_constants(f, 0), reference = scene_constants(f, 1);
        const de_vec2 expected = de__jitter_sample(i, 32);
        EXPECT_FLOAT_EQ(v.jitter.v[0], expected.x, 1e-7);
        EXPECT_FLOAT_EQ(v.jitter.v[1], expected.y, 1e-7);
        EXPECT_FLOAT_EQ(v.viewport.v[0], SCENE_SIZE / 2, 0);
        EXPECT_FLOAT_EQ(v.temporal.v[0], -1, 0);
        EXPECT_FLOAT_EQ(v.temporal.v[1], SCENE_SIZE, 0);
        EXPECT_MEMEQ(v.unjittered_view_proj, v.prev_unjittered_view_proj,
                     sizeof v.unjittered_view_proj);
        EXPECT_FLOAT_EQ(reference.jitter.v[0], 0, 0);
        EXPECT_FLOAT_EQ(reference.jitter.v[1], 0, 0);
        EXPECT_MEMEQ(reference.view_proj, reference.unjittered_view_proj,
                     sizeof reference.view_proj);
    }
    EXPECT_EQ(f->projection_checks, 8u);
    de__view_unregister_stage(f->renderer, verify_static_projection, f);
    de_view_frame_info metadata;
    ASSERT_TRUE(de_view_layout_info(f->renderer, f->views[0], &metadata));
    EXPECT_EQ(metadata.internal_width, SCENE_SIZE / 2);
    EXPECT_EQ(metadata.output_width, SCENE_SIZE);
    for (u32 i = 0; i < 24; ++i)
        scene_frame(f, false);
    const de_tracking_stats before = de_tracking_allocator_stats(&f->tracking);
    for (u32 i = 0; i < 32; ++i)
        scene_frame(f, false);
    EXPECT_EQ(de_tracking_allocator_stats(&f->tracking).alloc_count, before.alloc_count);
    const de_render_stats *stats = de_renderer_stats(f->renderer);
    u32 temporal_passes = 0;
    for (u32 i = 0; i < stats->pass_count; ++i)
        temporal_passes += !strcmp(stats->pass_names[i], "post.taa");
    EXPECT_EQ(temporal_passes, 1u); /* The spatial reference view has no temporal pass. */
    /* Bias changes alter the sampled texture content; restart accumulation without resizing. */
    scene_frame(f, false);
    ASSERT_EQ(de_cvar_set(de_cvar_find("r.taa.mip_bias"), ".25"), DE_OK);
    scene_frame(f, false);
    de__gs_view biased = scene_constants(f, 0);
    EXPECT_FLOAT_EQ(biased.temporal.v[0], -1.25, 0);
    EXPECT_FLOAT_EQ(biased.jitter.v[0], 0, 0);
    EXPECT_FLOAT_EQ(biased.jitter.v[1], -1.f / 6, 1e-7);
    scene_frame(f, false);
    biased = scene_constants(f, 0);
    EXPECT_FLOAT_EQ(biased.jitter.v[0], -.25, 0);
    ASSERT_EQ(de_cvar_set(de_cvar_find("r.taa.mip_bias"), "0"), DE_OK);
    scene_frame(f, false);
    biased = scene_constants(f, 0);
    EXPECT_FLOAT_EQ(biased.temporal.v[0], -1, 0);
    EXPECT_FLOAT_EQ(biased.jitter.v[0], 0, 0);
    de_render_settings settings = de_renderer_get_settings(f->renderer);
    settings.upscaler = DE_UPSCALE_NONE;
    de_view_set_settings(f->renderer, f->views[0], &settings);
    scene_frame(f, false);
    de__gs_view v = scene_constants(f, 0);
    EXPECT_FLOAT_EQ(v.jitter.v[0], 0, 0);
    EXPECT_FLOAT_EQ(v.jitter.v[1], 0, 0);
    EXPECT_MEMEQ(v.unjittered_view_proj, v.prev_unjittered_view_proj,
                 sizeof v.unjittered_view_proj);
    settings.upscaler = DE_UPSCALE_TAA;
    de_view_set_settings(f->renderer, f->views[0], &settings);
    scene_frame(f, false);
    v = scene_constants(f, 0);
    EXPECT_FLOAT_EQ(v.jitter.v[0], 0, 0);
    EXPECT_FLOAT_EQ(v.jitter.v[1], -1.f / 6, 1e-7);
    /* Large camera cut resets the phase even though the output size is unchanged. */
    DeTransform *camera = de_entity_get(f->world, f->cameras[0], f->transform);
    camera->position.x += 2;
    scene_frame(f, false);
    v = scene_constants(f, 0);
    EXPECT_FLOAT_EQ(v.jitter.v[0], 0, 0);
    EXPECT_FLOAT_EQ(v.jitter.v[1], -1.f / 6, 1e-7);
}
static void scene_disocclusion(taa_scene *f, f32 scale) {
    open_scene(f, scale);
    f->object = scene_object(f, (de_dvec3){-.9, -.8, 0}, (de_vec3){1.2f, 1.5f, 1}, 0);
    for (u32 i = 0; i < 96; ++i)
        scene_frame(f, false);
    scene_frame(f, true);
    bool foreground[SCENE_SIZE * SCENE_SIZE] = {0};
    for (u32 i = 0; i < SCENE_SIZE * SCENE_SIZE; ++i)
        foreground[i] =
            f->image[i * 4] > 1; /* Include antialiased silhouette pixels, not just interiors. */
    DeTransform *object = de_entity_get(f->world, f->object, f->transform);
    object->position.x += 2.0;
    for (u32 i = 0; i < 3; ++i)
        scene_frame(f, false);
    scene_frame(f, true);
    u32 revealed = 0, worst = 0;
    for (u32 y = 0; y < SCENE_SIZE; ++y)
        for (u32 x = 0; x < SCENE_SIZE; ++x) {
            const u32 i = y * SCENE_SIZE + x;
            if (!foreground[i])
                continue;
            const u32 reference = f->reference[((y * 4 + 2) * REF_SIZE + x * 4 + 2) * 4];
            if (reference)
                continue;
            ++revealed;
            if (f->image[i * 4] > worst)
                worst = f->image[i * 4];
        }
    printf("TAA scale %.2f disocclusion after four frames: %u revealed pixels, max trail %u/255\n",
           scale, revealed, worst);
    EXPECT_GT(revealed, 150u);
    EXPECT_LE(worst, 1u);
}
TEST_F(taa, rendered_light_and_layer_policy_resets_are_view_local, taa_scene) {
    open_scene(f, 1);
    de_render_settings settings = de_renderer_get_settings(f->renderer);
    settings.upscaler = DE_UPSCALE_TAA;
    de_view_set_settings(f->renderer, f->views[1], &settings);
    (void)scene_object(f, (de_dvec3){-.8, -.8, 0}, (de_vec3){1.6f, 1.6f, 1}, .17f);
    for (u32 i = 0; i < 3; ++i)
        scene_frame(f, false);
    ASSERT_EQ(de_view_set_light_mask(f->renderer, f->views[0], 0), DE_OK);
    scene_frame(f, false);
    de__gs_view a = scene_constants(f, 0), b = scene_constants(f, 1);
    de_vec2 expected = de__jitter_sample(3, 8);
    EXPECT_FLOAT_EQ(a.jitter.v[0], 0, 0);
    EXPECT_FLOAT_EQ(a.jitter.v[1], -1.f / 6, 1e-7);
    EXPECT_FLOAT_EQ(b.jitter.v[0], expected.x, 1e-7);
    EXPECT_FLOAT_EQ(b.jitter.v[1], expected.y, 1e-7);
    de_view_set_layer_mask(f->renderer, f->views[0], 2);
    scene_frame(f, false);
    a = scene_constants(f, 0);
    b = scene_constants(f, 1);
    expected = de__jitter_sample(4, 8);
    EXPECT_FLOAT_EQ(a.jitter.v[0], 0, 0);
    EXPECT_FLOAT_EQ(a.jitter.v[1], -1.f / 6, 1e-7);
    EXPECT_FLOAT_EQ(b.jitter.v[0], expected.x, 1e-7);
    EXPECT_FLOAT_EQ(b.jitter.v[1], expected.y, 1e-7);
}
TEST_F(taa, rendered_half_scale_with_screen_history, taa_scene) {
    open_scene(f, .5f);
    de_render_settings settings = de_renderer_get_settings(f->renderer);
    settings.render_scale = .5f;
    settings.ao = settings.reflections = DE_QUALITY_HIGH;
    de_view_set_settings(f->renderer, f->views[0], &settings);
    (void)scene_object(f, (de_dvec3){-.8, -.8, 0}, (de_vec3){1.6f, 1.6f, 1}, .17f);
    for (u32 i = 0; i < 32; ++i) {
        scene_frame(f, false);
        de__ssr_info info;
        ASSERT_TRUE(de__ssr_view_info(f->renderer->ssr, f->views[0], &info));
        EXPECT_EQ(info.width, SCENE_SIZE / 2);
        EXPECT_EQ(info.height, SCENE_SIZE / 2);
        EXPECT_EQ(info.history_valid, i != 0);
        EXPECT_EQ(info.dispatches, 2u);
        EXPECT_TRUE(info.output.id != 0);
    }
    scene_frame(f, true);
    EXPECT_TRUE(scene_flip(f) <= .04);
}
TEST_F(taa, rendered_disocclusion_four_frames, taa_scene) {
    scene_disocclusion(f, 1);
}
TEST_F(taa, rendered_half_scale_disocclusion_four_frames, taa_scene) {
    scene_disocclusion(f, .5f);
}
TEST_F(taa, rendered_half_scale_static_material_change_rejects_history, taa_scene) {
    open_scene(f, .5f);
    (void)scene_object(f, (de_dvec3){-1, -1, 0}, (de_vec3){2, 2, 1}, 0);
    for (u32 i = 0; i < 96; ++i)
        scene_frame(f, false);
    de_standard_material params;
    ASSERT_EQ(de_material_get_params(f->renderer, f->material, &params), DE_OK);
    const u32 masks[] = {1, 3, 4, 7}; /* white -> red -> yellow -> blue -> white */
    for (u32 step = 0; step < sizeof masks / sizeof masks[0]; ++step) {
        const u32 mask = masks[step];
        params.base_color =
            (de_vec4){mask & 1 ? .6f : 0, mask & 2 ? .6f : 0, mask & 4 ? .6f : 0, 1};
        ASSERT_EQ(de_material_set_params(f->renderer, f->material, &params), DE_OK);
        for (u32 i = 0; i < 3; ++i)
            scene_frame(f, false);
        scene_frame(f, true);
        u32 stale = 0;
        for (u32 i = 0; i < SCENE_SIZE * SCENE_SIZE; ++i) {
            u32 peak = 0;
            for (u32 c = 0; c < 3; ++c)
                if ((mask & (1u << c)) && f->image[i * 4 + c] > peak)
                    peak = f->image[i * 4 + c];
            for (u32 c = 0; c < 3; ++c) {
                const u32 error =
                    (u32)abs((int)f->image[i * 4 + c] - ((mask & (1u << c)) ? (int)peak : 0));
                if (error > stale)
                    stale = error;
            }
        }
        for (u32 y = 24; y < 40; ++y)
            for (u32 x = 24; x < 40; ++x)
                for (u32 c = 0; c < 3; ++c) {
                    const u32 actual = f->image[(y * SCENE_SIZE + x) * 4 + c];
                    const u32 reference =
                        f->reference[((y * 4 + 2) * REF_SIZE + x * 4 + 2) * 4 + c];
                    if (mask & (1u << c))
                        EXPECT_GT(reference, 100u);
                    EXPECT_TRUE(abs((int)actual - (int)reference) <= 1);
                }
        printf("TAAU material mask %u after four frames: chromatic residue %u/255\n", mask, stale);
        EXPECT_LE(stale, 1u);
    }
}
/* Exercise the vendor contract without pretending a vendor SDK is available: this adapter
 * deliberately hides a real TAA backend's history and supplies its own history barriers. */
typedef struct opaque_taa {
    de_renderer *renderer;
    void *native;
    de__upscale_history history;
    u32 frame;
} opaque_taa;
static u32 opaque_dispatches, opaque_destroys, opaque_inits;
static bool opaque_last_reset;
static de_result opaque_init(de_renderer *renderer, void **out) {
    *out = NULL;
    opaque_taa *state = DE_NEW(renderer->alloc, opaque_taa, DE_MEM_RENDER);
    if (!state)
        return DE_ERR_OUT_OF_MEMORY;
    *state = (opaque_taa){.renderer = renderer};
    de_result result = de__taa_backend()->init(renderer, &state->native);
    if (result != DE_OK) {
        DE_FREE(renderer->alloc, state, sizeof *state);
        return result;
    }
    *out = state;
    ++opaque_inits;
    return DE_OK;
}
static de_result opaque_resize(void *user, de__upscale_size size, de__upscale_history *out) {
    opaque_taa *state = user;
    const de__upscale_history previous = state->history;
    de_result result = de__taa_backend()->resize(state->native, size, &state->history);
    if (result != DE_OK)
        return result;
    if (memcmp(&state->history, &previous, sizeof previous))
        state->frame = 0;
    *out = (de__upscale_history){0};
    return DE_OK;
}
static void opaque_dispatch(void *user, de_gpu_cmd *cmd, const de__upscale_inputs *inputs,
                            const de__upscale_outputs *outputs) {
    opaque_taa *state = user;
    opaque_last_reset = inputs->reset;
    EXPECT_TRUE(inputs->color.id && inputs->depth.id && inputs->motion.id && outputs->color.id);
    EXPECT_EQ(inputs->history_color.id, 0u);
    EXPECT_EQ(inputs->history_depth.id, 0u);
    EXPECT_EQ(outputs->history_color.id, 0u);
    EXPECT_EQ(outputs->history_depth.id, 0u);
    EXPECT_FLOAT_EQ(inputs->delta_seconds, 1.f / 60, 1e-7);
    const u32 write = state->frame & 1u, read = 1u - write;
    de_gpu_barrier barriers[4] = {
        {.texture = state->history.color[write],
         .before = state->frame > 1 ? DE_ACCESS_SHADER_READ : DE_ACCESS_NONE,
         .after = DE_ACCESS_SHADER_WRITE},
        {.texture = state->history.depth[write],
         .before = state->frame > 1 ? DE_ACCESS_SHADER_READ : DE_ACCESS_NONE,
         .after = DE_ACCESS_SHADER_WRITE},
        {.texture = state->history.color[read],
         .before = state->frame ? DE_ACCESS_SHADER_READ : DE_ACCESS_NONE,
         .after = DE_ACCESS_SHADER_READ},
        {.texture = state->history.depth[read],
         .before = state->frame ? DE_ACCESS_SHADER_READ : DE_ACCESS_NONE,
         .after = DE_ACCESS_SHADER_READ}};
    de_gpu_cmd_barriers(cmd, barriers, 4);
    de__upscale_inputs in = *inputs;
    de__upscale_outputs out = *outputs;
    in.history_color = state->history.color[read];
    in.history_depth = state->history.depth[read];
    out.history_color = state->history.color[write];
    out.history_depth = state->history.depth[write];
    de__taa_backend()->dispatch(state->native, cmd, &in, &out);
    for (u32 i = 0; i < 2; ++i) {
        barriers[i].before = DE_ACCESS_SHADER_WRITE;
        barriers[i].after = DE_ACCESS_SHADER_READ;
    }
    de_gpu_cmd_barriers(cmd, barriers, 2);
    ++state->frame;
    ++opaque_dispatches;
}
static void opaque_destroy(void *user) {
    opaque_taa *state = user;
    de__taa_backend()->destroy(state->native);
    DE_FREE(state->renderer->alloc, state, sizeof *state);
    ++opaque_destroys;
}
TEST_F(taa, rendered_opaque_vendor_history_graph_contract, taa_scene) {
    open_scene(f, 1);
    opaque_dispatches = opaque_destroys = 0;
    static const de__upscaler_backend opaque = {"Test opaque TAA", DE_UPSCALE_METALFX,
                                                opaque_init,       opaque_resize,
                                                opaque_dispatch,   opaque_destroy};
    ASSERT_EQ(de__upscalers_register(f->renderer, &opaque), DE_OK);
    de_render_settings settings = de_renderer_get_settings(f->renderer);
    settings.upscaler = DE_UPSCALE_AUTO;
    de_view_set_settings(f->renderer, f->views[0], &settings);
    (void)scene_object(f, (de_dvec3){-.8, -.8, 0}, (de_vec3){1.6f, 1.6f, 1}, .17f);
    for (u32 i = 0; i < 32; ++i)
        scene_frame(f, false);
    scene_frame(f, true);
    EXPECT_EQ(opaque_dispatches, 33u);
    EXPECT_EQ(opaque_destroys, 0u);
    EXPECT_TRUE(scene_flip(f) <= .04);
    de__upscalers_forget(f->renderer, f->views[0]);
    EXPECT_EQ(opaque_destroys, 1u);
    scene_frame(f, false);
    EXPECT_EQ(opaque_dispatches, 34u);
    de_view_destroy(f->renderer, f->views[0]);
    for (u32 i = 0; i < 8; ++i)
        scene_frame(f, false);
    EXPECT_EQ(opaque_destroys, 2u);
}
static u32 solid_forward_gather(const de_forward_context *context, de_forward_item *out,
                                u32 capacity, void *user) {
    (void)context;
    (void)user;
    if (!capacity)
        return 0;
    out[0] = (de_forward_item){.entity = 1};
    return 1;
}
static void solid_forward_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const de_rg_resource *target = user;
    const de_gpu_attachment color = {.texture = de_rg_texture(pass, *target),
                                     .clear = true,
                                     .clear_color = {.6f, .6f, .6f, 1},
                                     .store = true};
    de_gpu_cmd_begin_render(cmd, &color, 1, NULL);
    de_gpu_cmd_end_render(cmd);
}
static de_result solid_forward_build(de_rg *graph, const de_forward_context *context,
                                     const de_forward_item *item, void *user) {
    (void)item;
    (void)user;
    de_rg_resource *target = de_render_forward_alloc(context, sizeof *target, 8);
    if (!target)
        return DE_ERR_OUT_OF_MEMORY;
    *target = de_render_forward_color(context);
    de_rg_pass *pass = de_rg_add_pass(graph, "taa.forward_only_fixture", DE_QUEUE_GRAPHICS,
                                      solid_forward_execute, target);
    if (!pass)
        return DE_ERR_OUT_OF_MEMORY;
    de_render_forward_declare(pass, context, false);
    return DE_OK;
}
TEST_F(taa, rendered_forward_only_and_empty_views, taa_scene) {
    open_scene(f, .5f);
    opaque_dispatches = opaque_destroys = opaque_inits = 0;
    static const de__upscaler_backend opaque = {
        "Test forward-only TAA", DE_UPSCALE_METALFX, opaque_init,
        opaque_resize,           opaque_dispatch,    opaque_destroy};
    ASSERT_EQ(de__upscalers_register(f->renderer, &opaque), DE_OK);
    de_render_settings settings = de_renderer_get_settings(f->renderer);
    settings.render_scale = .5f;
    settings.upscaler = DE_UPSCALE_AUTO;
    de_view_set_settings(f->renderer, f->views[0], &settings);
    for (u32 i = 0; i < 3; ++i)
        scene_frame(f, false);
    EXPECT_EQ(opaque_inits, 0u); /* Empty views must not allocate histories or native contexts. */
    ASSERT_EQ(de_render_add_forward_participant(
                  f->renderer, &(de_forward_participant_desc){.max_items = 1,
                                                              .gather = solid_forward_gather,
                                                              .build = solid_forward_build}),
              DE_OK);
    for (u32 i = 0; i < 3; ++i)
        scene_frame(f, false);
    scene_frame(f, true);
    EXPECT_EQ(opaque_inits, 1u);
    EXPECT_EQ(opaque_dispatches, 4u);
    de__gs_view sc = scene_constants(f, 0);
    de_vec2 expected = de__jitter_sample(3, 32);
    EXPECT_FLOAT_EQ(sc.jitter.v[0], expected.x, 1e-7);
    EXPECT_FLOAT_EQ(sc.jitter.v[1], expected.y, 1e-7);
    for (u32 i = 0; i < SCENE_SIZE * SCENE_SIZE; ++i)
        EXPECT_TRUE(f->image[i * 4] >= 152 && f->image[i * 4] <= 154);
    ASSERT_EQ(de_render_remove_forward_participant(f->renderer, solid_forward_gather, NULL), DE_OK);
    scene_frame(f, false);
    EXPECT_EQ(opaque_dispatches, 4u);
}
static void solid_hdr_stage(de__view_frame *frame, void *user) {
    (void)user;
    de_rg_resource *target = de_arena_push(frame->graph->arena, sizeof *target, 8);
    ASSERT_TRUE(target);
    *target = de_rg_create_texture(
        frame->graph, &(de_gpu_texture_desc){.width = frame->width,
                                             .height = frame->height,
                                             .format = DE_FMT_RGBA16_FLOAT,
                                             .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED,
                                             .name = "taa.custom_hdr_fixture"});
    ASSERT_TRUE(target->id);
    de_rg_pass *pass = de_rg_add_pass(frame->graph, "taa.custom_hdr_fixture", DE_QUEUE_GRAPHICS,
                                      solid_forward_execute, target);
    ASSERT_TRUE(pass);
    de_rg_write(pass, *target, DE_ACCESS_RENDER_TARGET);
    frame->color = *target;
    frame->color_is_scene_linear = true;
    frame->color_format = DE_FMT_RGBA16_FLOAT;
}
TEST_F(taa, rendered_custom_hdr_without_geometry_and_gap_reset, taa_scene) {
    open_scene(f, .5f);
    opaque_dispatches = opaque_destroys = opaque_inits = 0;
    static const de__upscaler_backend opaque = {"Test custom HDR TAA", DE_UPSCALE_METALFX,
                                                opaque_init,           opaque_resize,
                                                opaque_dispatch,       opaque_destroy};
    ASSERT_EQ(de__upscalers_register(f->renderer, &opaque), DE_OK);
    de_render_settings settings = de_renderer_get_settings(f->renderer);
    settings.render_scale = .5f;
    settings.upscaler = DE_UPSCALE_AUTO;
    de_view_set_settings(f->renderer, f->views[0], &settings);
    ASSERT_EQ(de__view_register_stage(f->renderer, DE__VIEW_SLOT_POST, 5, "taa.custom_hdr_fixture",
                                      solid_hdr_stage, NULL),
              DE_OK);
    for (u32 i = 0; i < 3; ++i)
        scene_frame(f, false);
    scene_frame(f, true);
    EXPECT_EQ(opaque_inits, 1u);
    EXPECT_EQ(opaque_dispatches, 4u);
    EXPECT_FALSE(opaque_last_reset);
    for (u32 i = 0; i < SCENE_SIZE * SCENE_SIZE; ++i)
        EXPECT_TRUE(f->image[i * 4] >= 152 && f->image[i * 4] <= 154);
    de__view_unregister_stage(f->renderer, solid_hdr_stage, NULL);
    scene_frame(f, false);
    EXPECT_EQ(opaque_dispatches, 4u);
    ASSERT_EQ(de__view_register_stage(f->renderer, DE__VIEW_SLOT_POST, 5, "taa.custom_hdr_fixture",
                                      solid_hdr_stage, NULL),
              DE_OK);
    scene_frame(f, false);
    EXPECT_EQ(opaque_inits, 1u);
    EXPECT_EQ(opaque_dispatches, 5u);
    EXPECT_TRUE(opaque_last_reset);
    de__view_unregister_stage(f->renderer, solid_hdr_stage, NULL);
}
static u32 refused_inits;
static de_result refused_init(de_renderer *renderer, void **out) {
    (void)renderer;
    *out = NULL;
    ++refused_inits;
    return DE_FAIL(DE_ERR_UNSUPPORTED, "test native upscaler refused this device");
}
TEST_F(taa, rendered_native_init_failure_falls_back_once, taa_scene) {
    open_scene(f, .5f);
    refused_inits = opaque_destroys = 0;
    static const de__upscaler_backend refused = {"Unsupported native upscaler",
                                                 DE_UPSCALE_METALFX,
                                                 refused_init,
                                                 opaque_resize,
                                                 opaque_dispatch,
                                                 opaque_destroy};
    ASSERT_EQ(de__upscalers_register(f->renderer, &refused), DE_OK);
    de_render_settings settings = de_renderer_get_settings(f->renderer);
    settings.render_scale = .5f;
    settings.upscaler = DE_UPSCALE_AUTO;
    de_view_set_settings(f->renderer, f->views[0], &settings);
    (void)scene_object(f, (de_dvec3){-.8, -.8, 0}, (de_vec3){1.6f, 1.6f, 1}, .17f);
    for (u32 i = 0; i < 64; ++i)
        scene_frame(f, false);
    scene_frame(f, true);
    EXPECT_EQ(refused_inits, 1u);
    EXPECT_EQ(opaque_destroys, 0u); /* An unsuccessful init owns nothing to destroy. */
    EXPECT_TRUE(scene_flip(f) <= .04);
}
static bool fail_next_resize;
static de_result retry_resize(void *state, de__upscale_size size, de__upscale_history *out) {
    if (fail_next_resize) {
        fail_next_resize = false;
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "test temporal resize failure");
    }
    return opaque_resize(state, size, out);
}
TEST_F(taa, rendered_failed_resize_does_not_publish_history, taa_scene) {
    open_scene(f, .5f);
    opaque_dispatches = opaque_destroys = opaque_inits = 0;
    fail_next_resize = true;
    static const de__upscaler_backend retry = {"Retry native resize", DE_UPSCALE_METALFX,
                                               opaque_init,           retry_resize,
                                               opaque_dispatch,       opaque_destroy};
    ASSERT_EQ(de__upscalers_register(f->renderer, &retry), DE_OK);
    de_render_settings settings = de_renderer_get_settings(f->renderer);
    settings.render_scale = .5f;
    settings.upscaler = DE_UPSCALE_AUTO;
    de_view_set_settings(f->renderer, f->views[0], &settings);
    (void)scene_object(f, (de_dvec3){-.8, -.8, 0}, (de_vec3){1.6f, 1.6f, 1}, .17f);
    scene_frame(f, false);
    EXPECT_EQ(opaque_inits, 1u);
    EXPECT_EQ(opaque_dispatches, 0u);
    scene_frame(f, false);
    EXPECT_EQ(opaque_inits, 1u);
    EXPECT_EQ(opaque_dispatches, 1u);
    EXPECT_TRUE(opaque_last_reset);
    const de__gs_view sc = scene_constants(f, 0);
    EXPECT_FLOAT_EQ(sc.jitter.v[0], 0, 0);
    EXPECT_FLOAT_EQ(sc.jitter.v[1], -1.f / 6, 1e-7);
    scene_frame(f, true);
    EXPECT_EQ(opaque_dispatches, 2u);
    EXPECT_FALSE(opaque_last_reset);
    u32 covered = 0;
    for (u32 i = 0; i < SCENE_SIZE * SCENE_SIZE; ++i)
        covered += f->image[i * 4] > 80;
    EXPECT_GT(covered, 16u);
}
typedef struct temporal_arena_probe {
    taa_scene *fixture;
    de_arena *limited, *original;
    bool failed;
} temporal_arena_probe;
static void limit_temporal_arena(de__view_frame *frame, void *user) {
    temporal_arena_probe *probe = user;
    if (frame->view.id != probe->fixture->views[0].id)
        return;
    ASSERT_FALSE(frame->graph->oom);
    probe->original = frame->graph->arena;
    frame->graph->arena = probe->limited;
}
static void restore_temporal_arena(de__view_frame *frame, void *user) {
    temporal_arena_probe *probe = user;
    if (frame->view.id != probe->fixture->views[0].id)
        return;
    probe->failed = frame->graph->oom;
    frame->graph->arena = probe->original;
}
TEST_F(taa, rendered_frame_arena_failure_refuses_unfiltered_frame, taa_scene) {
    open_scene(f, .5f);
    opaque_dispatches = opaque_destroys = opaque_inits = 0;
    static const de__upscaler_backend opaque = {"Test temporal arena", DE_UPSCALE_METALFX,
                                                opaque_init,           opaque_resize,
                                                opaque_dispatch,       opaque_destroy};
    ASSERT_EQ(de__upscalers_register(f->renderer, &opaque), DE_OK);
    de_render_settings settings = de_renderer_get_settings(f->renderer);
    settings.render_scale = .5f;
    settings.upscaler = DE_UPSCALE_AUTO;
    de_view_set_settings(f->renderer, f->views[0], &settings);
    (void)scene_object(f, (de_dvec3){-.8, -.8, 0}, (de_vec3){1.6f, 1.6f, 1}, .17f);
    temporal_arena_probe probe = {.fixture = f};
    ASSERT_EQ(de_arena_create(&(de_arena_desc){.reserve_bytes = 65536,
                                               .commit_step = 65536,
                                               .name = "temporal allocation fault"},
                              &probe.limited),
              DE_OK);
    const de_arena_info info = de_arena_get_info(probe.limited);
    ASSERT_TRUE(de_arena_push(probe.limited, info.reserved - info.used, 1));
    ASSERT_EQ(de__view_register_stage(f->renderer, DE__VIEW_SLOT_POST, 9, "test.temporal_limit",
                                      limit_temporal_arena, &probe),
              DE_OK);
    ASSERT_EQ(de__view_register_stage(f->renderer, DE__VIEW_SLOT_POST, 11, "test.temporal_restore",
                                      restore_temporal_arena, &probe),
              DE_OK);
    scene_frame(f, false);
    EXPECT_TRUE(probe.failed);
    EXPECT_EQ(opaque_dispatches, 0u);
    de__view_unregister_stage(f->renderer, limit_temporal_arena, &probe);
    de__view_unregister_stage(f->renderer, restore_temporal_arena, &probe);
    de_arena_destroy(probe.limited);
    scene_frame(f, false);
    EXPECT_EQ(opaque_inits, 1u);
    EXPECT_EQ(opaque_dispatches, 1u);
    EXPECT_TRUE(opaque_last_reset);
}
#endif

#else
#define TAA_METAL_TEST_REQUIRES_MACOS(name)                                                        \
    TEST(taa, name) {                                                                              \
        SKIP("Metal runtime qualification requires macOS");                                        \
    }
TAA_METAL_TEST_REQUIRES_MACOS(native_history_reprojection_rejection_and_resize)
TAA_METAL_TEST_REQUIRES_MACOS(native_pre_exposure_reactive_and_sharpen_contract)
#if DE_TAA_SCENE_TEST
TAA_METAL_TEST_REQUIRES_MACOS(rendered_static_scene_reference16_and_edge_stability)
TAA_METAL_TEST_REQUIRES_MACOS(rendered_half_scale_one_pixel_lines)
TAA_METAL_TEST_REQUIRES_MACOS(rendered_view_jitter_resets_motion_and_steady_allocations)
TAA_METAL_TEST_REQUIRES_MACOS(rendered_light_and_layer_policy_resets_are_view_local)
TAA_METAL_TEST_REQUIRES_MACOS(rendered_half_scale_with_screen_history)
TAA_METAL_TEST_REQUIRES_MACOS(rendered_disocclusion_four_frames)
TAA_METAL_TEST_REQUIRES_MACOS(rendered_half_scale_disocclusion_four_frames)
TAA_METAL_TEST_REQUIRES_MACOS(rendered_half_scale_static_material_change_rejects_history)
TAA_METAL_TEST_REQUIRES_MACOS(rendered_opaque_vendor_history_graph_contract)
TAA_METAL_TEST_REQUIRES_MACOS(rendered_forward_only_and_empty_views)
TAA_METAL_TEST_REQUIRES_MACOS(rendered_custom_hdr_without_geometry_and_gap_reset)
TAA_METAL_TEST_REQUIRES_MACOS(rendered_native_init_failure_falls_back_once)
TAA_METAL_TEST_REQUIRES_MACOS(rendered_failed_resize_does_not_publish_history)
TAA_METAL_TEST_REQUIRES_MACOS(rendered_frame_arena_failure_refuses_unfiltered_frame)
#endif
#undef TAA_METAL_TEST_REQUIRES_MACOS
#endif
