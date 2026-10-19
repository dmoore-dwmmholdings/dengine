#include "de_test.h"
#include <dengine/de_gpu.h>
#include <dengine/de_memory.h>
#include <dengine/de_debug.h>
#include "render/denoise/denoise_internal.h"
#include "render/graph/rg_internal.h"
#include "render/renderer_internal.h"
#include "render/passes/vis/gbuffer.h"
#include "render/scene/gpu_scene.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static bool cvar_set(const char *name, const char *value) {
    de_cvar cv = de_cvar_find(name);
    return cv.id && de_cvar_set(cv, value) == DE_OK;
}

#define W      64u
#define H      32u
#define PIXELS (W * H)
typedef struct dn_fixture {
    de_tracking_allocator tracking, device_tracking;
    de_allocator alloc, device_alloc;
    de_renderer renderer;
    de_gpu_device *device;
    de__denoise *owner;
    de_arena *arena;
    de_rg *graph;
    de_gpu_texture signal, depth, normal, motion, material, key;
    de_gpu_buffer upload, readback;
    float data[6][PIXELS * 4];
    bool use_key, use_hit;
    de_denoise_result result;
    de_rg_resource identity;
    u64 frame;
    u32 width, height, channels, iterations;
    de_denoise_clamp clamp;
} dn_fixture;
static de_gpu_texture make_texture(dn_fixture *f, de_gpu_format format) {
    return de_gpu_texture_create(
        f->device,
        &(de_gpu_texture_desc){.width = W,
                               .height = H,
                               .depth_or_layers = 1,
                               .mip_count = 1,
                               .format = format,
                               .usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST | DE_USAGE_COPY_SRC});
}
static void dn_fixture_setup(dn_fixture *f) {
#if !DE_HAS_GPU_METAL
    SKIP("native Metal denoiser acceptance requires Metal");
#endif
    f->alloc = de_tracking_allocator_init(&f->tracking, NULL, DE_MEM_RENDER);
    f->device_alloc = de_tracking_allocator_init(&f->device_tracking, NULL, DE_MEM_RENDER);
    ASSERT_EQ(de_gpu_device_create(&(de_gpu_device_desc){.backend = DE_GPU_METAL,
                                                         .debug_layer = true,
                                                         .allocator = &f->device_alloc},
                                   &f->device),
              DE_OK);
    f->renderer.dev = f->device;
    f->renderer.alloc = &f->alloc;
    ASSERT_EQ(de__denoise_create(&f->renderer, &f->owner), DE_OK);
    ASSERT_EQ(de_arena_create(&(de_arena_desc){.name = "denoiser acceptance"}, &f->arena), DE_OK);
    ASSERT_EQ(de__rg_create(&(de__rg_desc){.allocator = &f->alloc, .device = f->device}, &f->graph),
              DE_OK);
    f->signal = make_texture(f, DE_FMT_RGBA32_FLOAT);
    f->depth = make_texture(f, DE_FMT_R32_FLOAT);
    f->normal = make_texture(f, DE_FMT_RGBA32_FLOAT);
    f->motion = make_texture(f, DE_FMT_RG32_FLOAT);
    f->material = make_texture(f, DE_FMT_RG32_FLOAT);
    f->key = make_texture(f, DE_FMT_RG32_FLOAT);
    f->upload = de_gpu_buffer_create(f->device, &(de_gpu_buffer_desc){.size = 6 * PIXELS * 16,
                                                                      .usage = DE_USAGE_COPY_SRC,
                                                                      .memory = DE_MEM_UPLOAD});
    f->readback = de_gpu_buffer_create(f->device, &(de_gpu_buffer_desc){.size = 131072,
                                                                        .usage = DE_USAGE_COPY_DST,
                                                                        .memory = DE_MEM_READBACK});
    ASSERT_TRUE(f->signal.id && f->depth.id && f->normal.id && f->motion.id && f->material.id &&
                f->key.id && f->upload.id && f->readback.id);
    f->channels = 3;
    f->iterations = 3;
    f->width = W;
    f->height = H;
    const f32 n[3] = {0, 0, 1};
    u32 packed = de__gb1_pack(n, .5f);
    for (u32 i = 0; i < PIXELS; ++i) {
        f->data[0][i * 4] = f->data[0][i * 4 + 1] = f->data[0][i * 4 + 2] = 1;
        f->data[0][i * 4 + 3] = 1;
        f->data[1][i] = 2;
        f->data[2][i * 4] = (packed & 1023) / 1023.f;
        f->data[2][i * 4 + 1] = ((packed >> 10) & 1023) / 1023.f;
        f->data[2][i * 4 + 2] = ((packed >> 20) & 1023) / 1023.f;
        f->data[2][i * 4 + 3] = (packed >> 30) / 3.f;
        f->data[4][i * 2] = 11;
    }
}
static void dn_fixture_teardown(dn_fixture *f) {
    (void)cvar_set("r.denoise.reset", "0");
    if (f->device)
        de_gpu_wait_idle(f->device);
    de__denoise_destroy(f->owner);
    if (f->graph)
        de__rg_destroy(f->graph);
    if (f->arena)
        de_arena_destroy(f->arena);
    if (f->device) {
        de_gpu_texture_destroy(f->device, f->signal);
        de_gpu_texture_destroy(f->device, f->depth);
        de_gpu_texture_destroy(f->device, f->normal);
        de_gpu_texture_destroy(f->device, f->motion);
        de_gpu_texture_destroy(f->device, f->material);
        de_gpu_texture_destroy(f->device, f->key);
        de_gpu_buffer_destroy(f->device, f->upload);
        de_gpu_buffer_destroy(f->device, f->readback);
        de_gpu_device_destroy(f->device);
    }
}
static void upload(dn_fixture *f) {
    const de_gpu_texture textures[] = {f->signal, f->depth,    f->normal,
                                       f->motion, f->material, f->key};
    const u32 channels[] = {4, 1, 4, 2, 2, 2};
    u8 *mapped = de_gpu_buffer_map(f->device, f->upload);
    de_gpu_cmd *cmd =
        de_gpu_cmd_begin(f->device, DE_QUEUE_GRAPHICS, "Canonical encoded denoiser input upload");
    for (u32 i = 0; i < 6; ++i) {
        u64 offset = (u64)i * PIXELS * 16;
        memcpy(mapped + offset, f->data[i], PIXELS * channels[i] * 4);
        de_gpu_barrier b = {
            .texture = textures[i], .before = DE_ACCESS_SHADER_READ, .after = DE_ACCESS_COPY_DST};
        de_gpu_cmd_barriers(cmd, &b, 1);
        de_gpu_cmd_copy_buffer_to_texture(cmd, textures[i], 0, 0, f->upload, offset,
                                          W * channels[i] * 4);
        b.before = DE_ACCESS_COPY_DST;
        b.after = DE_ACCESS_SHADER_READ;
        de_gpu_cmd_barriers(cmd, &b, 1);
    }
    ASSERT_EQ(de_gpu_queue_submit(f->device, DE_QUEUE_GRAPHICS,
                                  &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
              DE_OK);
    de_gpu_wait_idle(f->device);
}
static void begin(dn_fixture *f) {
    de__rg_reset(f->graph);
    de_arena_reset(f->arena);
    ASSERT_EQ(de__rg_begin(f->graph, f->arena), DE_OK);
    ++f->frame;
}
static de_denoise_result build(dn_fixture *f, de_view view, u64 signal, bool reset, bool spatial,
                               de_rg_resource identity) {
    de_denoise_frame frame = {
        .graph = f->graph,
        .view = view,
        .frame = f->frame,
        .reset = reset,
        .inputs = {.width = f->width, .height = f->height, .linear_depth = true}};
    frame.inputs.signal = de_rg_import_texture(f->graph, f->signal, DE_ACCESS_SHADER_READ);
    frame.inputs.depth = de_rg_import_texture(f->graph, f->depth, DE_ACCESS_SHADER_READ);
    frame.inputs.normal = de_rg_import_texture(f->graph, f->normal, DE_ACCESS_SHADER_READ);
    frame.inputs.motion = de_rg_import_texture(f->graph, f->motion, DE_ACCESS_SHADER_READ);
    frame.inputs.material =
        identity.id ? identity : de_rg_import_texture(f->graph, f->material, DE_ACCESS_SHADER_READ);
    if (f->use_hit)
        frame.inputs.hit_distance = frame.inputs.depth;
    if (f->use_key)
        frame.inputs.validation_key = de_rg_import_texture(f->graph, f->key, DE_ACCESS_SHADER_READ);
    de_denoise_ctx *ctx = NULL;
    ASSERT_EQ(denoise_begin(f->owner, &frame,
                            &(de_denoise_signal_desc){.identity = signal,
                                                      .format = DE_FMT_RGBA32_FLOAT,
                                                      .channels = f->channels,
                                                      .clamp = f->clamp,
                                                      .iterations = f->iterations,
                                                      .hit_distance_scale = f->use_hit ? 1.f : 0.f,
                                                      .luminance_scale = 100},
                            &ctx),
              DE_OK);
    de_denoise_result r = denoise_add_reproject(ctx);
    r = denoise_add_temporal(ctx, r);
    if (spatial)
        r = denoise_add_atrous(ctx, r);
    ASSERT_TRUE(r.color.id && r.history_length.id && r.validity.id);
    return r;
}
static void readback(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    dn_fixture *f = user;
    const de_rg_resource resources[] = {f->result.color, f->result.history_length,
                                        f->result.validity, f->result.variance};
    const u32 offsets[] = {0, 32768, 40960, 49152}, pitches[] = {W * 16, W * 4, 256, W * 4};
    if (f->identity.id) {
        de_gpu_texture t = de_rg_texture(pass, f->identity);
        de_gpu_cmd_copy_texture_to_buffer(cmd, f->readback, 65536, W * 8, t, 0, 0);
        de_gpu_barrier b = {
            .texture = t, .before = DE_ACCESS_COPY_SRC, .after = DE_ACCESS_SHADER_READ};
        de_gpu_cmd_barriers(cmd, &b, 1);
    }
    for (u32 i = 0; i < 4; ++i) {
        de_gpu_texture t = de_rg_texture(pass, resources[i]);
        de_gpu_cmd_copy_texture_to_buffer(cmd, f->readback, offsets[i], pitches[i], t, 0, 0);
        de_gpu_barrier b = {
            .texture = t, .before = DE_ACCESS_COPY_SRC, .after = DE_ACCESS_SHADER_READ};
        de_gpu_cmd_barriers(cmd, &b, 1);
    }
}
static void run(dn_fixture *f, de_denoise_result result) {
    f->result = result;
    de_rg_pass *p =
        de_rg_add_pass(f->graph, "Independent denoiser readback", DE_QUEUE_GRAPHICS, readback, f);
    if (f->identity.id)
        de_rg_read(p, f->identity, DE_ACCESS_COPY_SRC);
    de_rg_read(p, result.color, DE_ACCESS_COPY_SRC);
    de_rg_read(p, result.history_length, DE_ACCESS_COPY_SRC);
    de_rg_read(p, result.validity, DE_ACCESS_COPY_SRC);
    de_rg_read(p, result.variance, DE_ACCESS_COPY_SRC);
    de_rg_write(p, de_rg_import_buffer(f->graph, f->readback, DE_ACCESS_COPY_DST),
                DE_ACCESS_COPY_DST);
    de_rg_pass_side_effect(p);
    ASSERT_EQ(de__rg_finalize(f->graph), DE_OK);
    ASSERT_EQ(de__rg_compile(f->graph), DE_OK);
    ASSERT_EQ(de__rg_execute(f->graph, &(de__rg_execute_desc){.frame_index = f->frame}), DE_OK);
    de_gpu_wait_idle(f->device);
}
static float *color(dn_fixture *f) {
    return de_gpu_buffer_map(f->device, f->readback);
}
static float age(dn_fixture *f, u32 x, u32 y) {
    return *(float *)((u8 *)color(f) + 32768 + y * W * 4 + x * 4);
}
TEST(denoise_framework, motion_convention) {
    de_vec2 previous = de__denoise_previous_uv((de_vec2){.5f, .5f}, (de_vec2){.25f, -.5f});
    EXPECT_FLOAT_EQ(previous.x, .375, 0);
    EXPECT_FLOAT_EQ(previous.y, .25, 0);
    previous = de__denoise_previous_uv((de_vec2){.5f, .5f}, (de_vec2){-.25f, .5f});
    EXPECT_FLOAT_EQ(previous.x, .625, 0);
    EXPECT_FLOAT_EQ(previous.y, .75, 0);
}
TEST_F(denoise_framework, reproject_validation, dn_fixture) {
    upload(f);
    begin(f);
    run(f, build(f, (de_view){1}, 1, false, false, (de_rg_resource){0}));
    begin(f);
    run(f, build(f, (de_view){1}, 1, false, false, (de_rg_resource){0}));
    EXPECT_FLOAT_EQ(age(f, 32, 16), 1, 0);
    /* Distinct material instances with identical Standard-model/GB1 data. */
    f->data[4][(16 * W + 30) * 2] = 12;
    f->data[1][16 * W + 31] = 3;
    const f32 n[3] = {1, 0, 0};
    u32 packed = de__gb1_pack(n, .5f);
    u32 at = (16 * W + 32) * 4;
    f->data[2][at] = (packed & 1023) / 1023.f;
    f->data[2][at + 1] = ((packed >> 10) & 1023) / 1023.f;
    f->data[2][at + 3] = (packed >> 30) / 3.f;
    upload(f);
    begin(f);
    run(f, build(f, (de_view){1}, 1, false, false, (de_rg_resource){0}));
    EXPECT_FLOAT_EQ(age(f, 30, 16), 0, 0);
    EXPECT_FLOAT_EQ(age(f, 31, 16), 0, 0);
    EXPECT_FLOAT_EQ(age(f, 32, 16), 0, 0);
    EXPECT_FLOAT_EQ(age(f, 40, 16), 2, 0);
    /* Signed motion pulls prior per-pixel identities from opposite sides. */
    for (u32 i = 0; i < PIXELS; ++i)
        f->data[4][i * 2] = (float)(i % W + 1);
    upload(f);
    begin(f);
    run(f, build(f, (de_view){1}, 1, true, false, (de_rg_resource){0}));
    f->data[3][(16 * W + 20) * 2] = 2.f / W;
    f->data[4][(16 * W + 20) * 2] = 20;
    f->data[3][(16 * W + 40) * 2] = -2.f / W;
    f->data[4][(16 * W + 40) * 2] = 42;
    upload(f);
    begin(f);
    run(f, build(f, (de_view){1}, 1, false, false, (de_rg_resource){0}));
    EXPECT_FLOAT_EQ(age(f, 20, 16), 1, 0);
    EXPECT_FLOAT_EQ(age(f, 40, 16), 1, 0);
}
static u32 hash32(u32 x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    return x ^ (x >> 16);
}
TEST_F(denoise_framework, temporal_converge, dn_fixture) {
    const u32 frames = 64;
    double source_var = 0;
    float sums[PIXELS] = {0};
    for (u32 frame = 0; frame < frames; ++frame) {
        for (u32 i = 0; i < PIXELS; ++i) {
            float noise = ((double)hash32(i + frame * PIXELS) / 4294967296.0 - .5) * 2;
            sums[i] += 1 + noise;
            source_var += noise * noise;
            for (u32 c = 0; c < 3; ++c)
                f->data[0][i * 4 + c] = 1 + noise;
        }
        upload(f);
        begin(f);
        run(f, build(f, (de_view){1}, 1, false, false, (de_rg_resource){0}));
    }
    double variance = 0, mean = 0, error = 0;
    float *out = color(f);
    for (u32 i = 0; i < PIXELS; ++i) {
        mean += out[i * 4];
        variance += (out[i * 4] - 1) * (out[i * 4] - 1);
        error = fmax(error, fabs(out[i * 4] - sums[i] / frames));
    }
    variance /= PIXELS;
    mean /= PIXELS;
    source_var /= PIXELS * frames;
    printf("Denoise temporal mean %.9g variance %.9g bound %.9g CPU error %.9g\\n", mean, variance,
           source_var / frames, error);
    EXPECT_TRUE(error < 2e-6);
    EXPECT_TRUE(variance <= source_var / frames);
}
TEST_F(denoise_framework, atrous_edge, dn_fixture) {
    double before = 0;
    for (u32 y = 0; y < H; ++y)
        for (u32 x = 0; x < W; ++x) {
            u32 i = y * W + x;
            float mean = x < 32 ? 1 : 3, noise = ((double)hash32(i) / 4294967296.0 - .5) * .8;
            f->data[1][i] = x < 32 ? 2 : 3;
            for (u32 c = 0; c < 3; ++c)
                f->data[0][i * 4 + c] = mean + noise;
            if (x > 4 && x < 26)
                before += noise * noise;
        }
    upload(f);
    begin(f);
    run(f, build(f, (de_view){1}, 1, true, true, (de_rg_resource){0}));
    double after = 0, max_bleed = 0;
    float *out = color(f);
    for (u32 y = 0; y < H; ++y)
        for (u32 x = 0; x < W; ++x) {
            u32 i = y * W + x;
            double mean = x < 32 ? 1 : 3;
            if (x > 4 && x < 26)
                after += (out[i * 4] - mean) * (out[i * 4] - mean);
            if (x == 30 || x == 33)
                max_bleed = fmax(max_bleed, fabs(out[i * 4] - mean));
        }
    printf("Denoise a-trous flat reduction %.9gx edge error %.9g\\n", before / after, max_bleed);
    EXPECT_TRUE(before / after >= 8);
    EXPECT_TRUE(max_bleed < .4);
    float baseline[PIXELS];
    for (u32 i = 0; i < PIXELS; ++i)
        baseline[i] = out[i * 4];
    for (u32 y = 0; y < H; ++y)
        for (u32 x = 32; x < W; ++x)
            for (u32 c = 0; c < 3; ++c)
                f->data[0][(y * W + x) * 4 + c] += 27;
    upload(f);
    begin(f);
    run(f, build(f, (de_view){1}, 1, true, true, (de_rg_resource){0}));
    out = color(f);
    /* Changing only the far side by 27 cannot affect pixels >1px from the edge. */
    for (u32 y = 0; y < H; ++y)
        for (u32 x = 0; x < 31; ++x)
            EXPECT_FLOAT_EQ(out[(y * W + x) * 4], baseline[y * W + x], 0);
}
TEST_F(denoise_framework, reset_resize_views_signals, dn_fixture) {
    upload(f);
    for (u32 frame = 0; frame < 24; ++frame) {
        begin(f);
        (void)build(f, (de_view){1}, 2, false, false, (de_rg_resource){0});
        (void)build(f, (de_view){2}, 1, false, false, (de_rg_resource){0});
        run(f, build(f, (de_view){1}, 1, false, false, (de_rg_resource){0}));
    }
    EXPECT_FLOAT_EQ(age(f, 32, 16), 23, 0);
    de_tracking_stats cpu = de_tracking_allocator_stats(&f->tracking),
                      gpu = de_tracking_allocator_stats(&f->device_tracking);
    for (u32 frame = 0; frame < 8; ++frame) {
        begin(f);
        (void)build(f, (de_view){1}, 2, false, false, (de_rg_resource){0});
        (void)build(f, (de_view){2}, 1, false, false, (de_rg_resource){0});
        run(f, build(f, (de_view){1}, 1, false, false, (de_rg_resource){0}));
    }
    de_tracking_stats cpu_after = de_tracking_allocator_stats(&f->tracking),
                      gpu_after = de_tracking_allocator_stats(&f->device_tracking);
    EXPECT_EQ(cpu.alloc_count, cpu_after.alloc_count);
    EXPECT_EQ(cpu.realloc_count, cpu_after.realloc_count);
    EXPECT_EQ(gpu.alloc_count, gpu_after.alloc_count);
    EXPECT_EQ(gpu.realloc_count, gpu_after.realloc_count);
    ASSERT_TRUE(cvar_set("r.denoise.reset", "1"));
    begin(f);
    (void)build(f, (de_view){1}, 2, false, false, (de_rg_resource){0});
    (void)build(f, (de_view){2}, 1, false, false, (de_rg_resource){0});
    run(f, build(f, (de_view){1}, 1, false, false, (de_rg_resource){0}));
    EXPECT_FLOAT_EQ(age(f, 32, 16), 0, 0);
    begin(f);
    run(f, build(f, (de_view){1}, 2, true, false, (de_rg_resource){0}));
    EXPECT_FLOAT_EQ(age(f, 32, 16), 0, 0);
    begin(f);
    run(f, build(f, (de_view){1}, 1, false, false,
                 (de_rg_resource){
                     0})); /* Signal1 skipped a frame: framework invalidates continuity. */
    EXPECT_FLOAT_EQ(age(f, 32, 16), 0, 0);
    de__denoise_forget(f->owner, (de_view){1});
    begin(f);
    run(f, build(f, (de_view){257}, 1, false, false, (de_rg_resource){0}));
    EXPECT_FLOAT_EQ(age(f, 32, 16), 0, 0);
    f->width = 32;
    f->height = 16;
    begin(f);
    run(f, build(f, (de_view){257}, 1, false, false, (de_rg_resource){0}));
    EXPECT_FLOAT_EQ(age(f, 16, 8), 0, 0);
}

static de_gpu_buffer raw(dn_fixture *f, const void *data, u32 bytes) {
    de_gpu_buffer b = de_gpu_buffer_create(
        f->device,
        &(de_gpu_buffer_desc){.size = bytes, .usage = DE_USAGE_STORAGE, .memory = DE_MEM_UPLOAD});
    ASSERT_TRUE(b.id);
    memcpy(de_gpu_buffer_map(f->device, b), data, bytes);
    return b;
}
TEST_F(denoise_framework, canonical_material_lookup, dn_fixture) {
    /* Canonical raw visibility rows include padding; IDs address DeVisMeshlet entries,
     * then actual instance/submesh material slots. No model id participates. */
    u32 ids[128 * H];
    for (u32 y = 0; y < H; ++y)
        for (u32 x = 0; x < 128; ++x)
            ids[y * 128 + x] = x < 32 ? 0 : 128;
    u32 visible[4] = {0, 0, 1, 0}, record[16] = {0};
    de_gpu_buffer records = raw(f, record, sizeof record);
    de__gs_mesh mesh = {0};
    mesh.meshlets = de_gpu_buffer_bindless(f->device, records);
    de__gs_instance instance[2] = {0};
    instance[0].materials[0].v[0] = 3;
    instance[1].materials[0].v[0] = 7;
    de_gpu_buffer meshes = raw(f, &mesh, sizeof mesh),
                  instances = raw(f, instance, sizeof instance);
    de__gs_view scene = {0};
    scene.instances = de_gpu_buffer_bindless(f->device, instances);
    scene.meshes = de_gpu_buffer_bindless(f->device, meshes);
    scene.instance_count = 2;
    scene.mesh_count = 1;
    de_gpu_buffer tables = raw(f, &scene, sizeof scene), entries = raw(f, visible, sizeof visible),
                  visibility = raw(f, ids, sizeof ids);
    upload(f);
    for (u32 frame = 0; frame < 3; ++frame) {
        if (frame == 2) {
            instance[1].materials[0].v[0] = 3;
            memcpy(de_gpu_buffer_map(f->device, instances), instance, sizeof instance);
        }
        begin(f);
        f->identity = denoise_add_material_identity(
            f->owner, f->graph, W, H,
            de_rg_import_buffer(f->graph, visibility, DE_ACCESS_SHADER_READ),
            de_rg_import_buffer(f->graph, entries, DE_ACCESS_SHADER_READ),
            de_rg_import_buffer(f->graph, tables, DE_ACCESS_SHADER_READ), 0, 512);
        ASSERT_TRUE(f->identity.id);
        run(f, build(f, (de_view){1}, 1, false, false, f->identity));
        float *material = (float *)((u8 *)color(f) + 65536);
        EXPECT_FLOAT_EQ(material[(16 * W + 20) * 2], 3, 0);
        EXPECT_FLOAT_EQ(material[(16 * W + 40) * 2], frame == 2 ? 3 : 7, 0);
        if (frame == 2) {
            EXPECT_FLOAT_EQ(age(f, 20, 16), 2, 0);
            EXPECT_FLOAT_EQ(age(f, 40, 16), 0, 0);
        }
    }
    de_gpu_buffer buffers[] = {records, meshes, instances, tables, entries, visibility};
    for (u32 i = 0; i < 6; ++i)
        de_gpu_buffer_destroy(f->device, buffers[i]);
}

TEST_F(denoise_framework, clamp_and_determinism, dn_fixture) {
    float baseline[PIXELS * 4];
    for (u32 mode = 1; mode <= 2; ++mode) {
        f->clamp = (de_denoise_clamp)mode;
        for (u32 i = 0; i < PIXELS; ++i)
            for (u32 c = 0; c < 3; ++c)
                f->data[0][i * 4 + c] = 100;
        upload(f);
        begin(f);
        run(f, build(f, (de_view){1}, mode, true, false, (de_rg_resource){0}));
        for (u32 i = 0; i < PIXELS; ++i)
            for (u32 c = 0; c < 3; ++c)
                f->data[0][i * 4 + c] = 1;
        upload(f);
        begin(f);
        run(f, build(f, (de_view){1}, mode, false, false, (de_rg_resource){0}));
        EXPECT_FLOAT_EQ(color(f)[(16 * W + 32) * 4], 1, 1e-6);
    }
    begin(f);
    run(f, build(f, (de_view){1}, 3, true, true, (de_rg_resource){0}));
    memcpy(baseline, color(f), sizeof baseline);
    begin(f);
    run(f, build(f, (de_view){1}, 3, true, true, (de_rg_resource){0}));
    EXPECT_EQ(memcmp(baseline, color(f), sizeof baseline), 0);
}

TEST_F(denoise_framework, variance_respects_material_and_light_identity_boundaries, dn_fixture) {
    for (u32 mode = 0; mode < 3; ++mode) {
        f->use_key = mode != 0;
        for (u32 y = 0; y < H; ++y)
            for (u32 x = 0; x < W; ++x) {
                u32 i = y * W + x;
                f32 value = (x < W / 2 ? 1.f : 3.f) + (hash32(i) % 101) / 250.f;
                for (u32 c = 0; c < 3; ++c)
                    f->data[0][i * 4 + c] = value;
                f->data[4][i * 2] = 11;
                f->data[4][i * 2 + 1] = mode == 0 && x >= W / 2 ? 2 : 1;
                f->data[5][i * 2] = mode == 1 && x >= W / 2 ? 31 : 17;
                f->data[5][i * 2 + 1] = mode == 2 && x >= W / 2 ? 2 : 1;
            }
        upload(f);
        begin(f);
        run(f, build(f, (de_view){1}, 1, true, true, (de_rg_resource){0}));
        f32 baseline_variance[PIXELS], baseline_color[PIXELS];
        memcpy(baseline_variance, (u8 *)color(f) + 49152, sizeof baseline_variance);
        for (u32 i = 0; i < PIXELS; ++i)
            baseline_color[i] = color(f)[i * 4];
        /* Independent sample variance on the left boundary has no samples from
         * the other identity, despite matching geometric depth and normals. */
        f64 sum = 0, squared = 0;
        u32 count = 0;
        for (u32 y = 14; y <= 18; ++y)
            for (u32 x = 29; x < 32; ++x) {
                f64 value = f->data[0][(y * W + x) * 4];
                sum += value;
                squared += value * value;
                ++count;
            }
        f64 expected = squared / count - (sum / count) * (sum / count);
        EXPECT_FLOAT_EQ(baseline_variance[16 * W + 31], expected, 2e-5);
        for (u32 y = 0; y < H; ++y)
            for (u32 x = W / 2; x < W; ++x)
                for (u32 c = 0; c < 3; ++c)
                    f->data[0][(y * W + x) * 4 + c] += 27;
        upload(f);
        begin(f);
        run(f, build(f, (de_view){1}, 1, true, true, (de_rg_resource){0}));
        f32 *variance = (f32 *)((u8 *)color(f) + 49152);
        f32 variance_delta = 0, color_delta = 0;
        for (u32 y = 0; y < H; ++y)
            for (u32 x = 0; x < W / 2; ++x) {
                u32 i = y * W + x;
                variance_delta = fmaxf(variance_delta, fabsf(variance[i] - baseline_variance[i]));
                color_delta = fmaxf(color_delta, fabsf(color(f)[i * 4] - baseline_color[i]));
            }
        printf("identity boundary mode%u: variance delta%g filtered delta%g\n", mode,
               variance_delta, color_delta);
        EXPECT_FLOAT_EQ(variance_delta, 0, 0);
        EXPECT_FLOAT_EQ(color_delta, 0, 0);
    }
}

TEST_F(denoise_framework, luminance_clamp_recovers_black_and_near_black_history, dn_fixture) {
    f->clamp = DE_DENOISE_CLAMP_LUMINANCE;
    const f32 old_values[] = {0, 1e-30f};
    const f32 lit[3] = {1, .25f, .5f};
    for (u32 sample = 0; sample < 2; ++sample) {
        for (u32 i = 0; i < PIXELS; ++i)
            for (u32 c = 0; c < 3; ++c)
                f->data[0][i * 4 + c] = old_values[sample];
        upload(f);
        for (u32 frame = 0; frame < 16; ++frame) {
            begin(f);
            run(f, build(f, (de_view){1}, 91 + sample, frame == 0, false, (de_rg_resource){0}));
        }
        EXPECT_TRUE(age(f, 32, 16) > 0);
        for (u32 i = 0; i < PIXELS; ++i)
            for (u32 c = 0; c < 3; ++c)
                f->data[0][i * 4 + c] = lit[c];
        upload(f);
        begin(f);
        run(f, build(f, (de_view){1}, 91 + sample, false, false, (de_rg_resource){0}));
        /* A uniform current neighborhood has a single allowed luminance. Recover
         * from history with no usable chroma using the current chromatic signal. */
        f32 maximum = 0;
        for (u32 i = 0; i < PIXELS; ++i)
            for (u32 c = 0; c < 3; ++c)
                maximum = fmaxf(maximum, fabsf(color(f)[i * 4 + c] - lit[c]));
        printf("black-history sample%u maximum clamped signal error: %g\n", sample, maximum);
        EXPECT_FLOAT_EQ(maximum, 0, 1e-6);
    }
}

TEST_F(denoise_framework, scalar_pair_clamp_does_not_rescale_visibility_with_distance, dn_fixture) {
    f->channels = 2;
    f->clamp = DE_DENOISE_CLAMP_LUMINANCE;
    for (u32 i = 0; i < PIXELS; ++i) {
        f->data[0][i * 4] = .25f;
        f->data[0][i * 4 + 1] = 8;
        f->data[0][i * 4 + 2] = 0;
    }
    upload(f);
    for (u32 frame = 0; frame < 16; ++frame) {
        begin(f);
        run(f, build(f, (de_view){1}, 99, frame == 0, false, (de_rg_resource){0}));
    }
    for (u32 i = 0; i < PIXELS; ++i)
        f->data[0][i * 4 + 1] = .1f;
    upload(f);
    begin(f);
    run(f, build(f, (de_view){1}, 99, false, false, (de_rg_resource){0}));
    f32 visibility_error = 0, distance_error = 0;
    for (u32 i = 0; i < PIXELS; ++i) {
        visibility_error = fmaxf(visibility_error, fabsf(color(f)[i * 4] - .25f));
        distance_error = fmaxf(distance_error, fabsf(color(f)[i * 4 + 1] - .1f));
    }
    printf("scalar pair clamp: visibility error%g distance error%g\n", visibility_error,
           distance_error);
    EXPECT_FLOAT_EQ(visibility_error, 0, 1e-6);
    EXPECT_FLOAT_EQ(distance_error, 0, 1e-6);
}
TEST_F(denoise_framework, scalar_pair_variance_and_weights_ignore_auxiliary_distance, dn_fixture) {
    f->channels = 2;
    for (u32 i = 0; i < PIXELS; ++i) {
        f->data[0][i * 4] = .4f + (hash32(i) % 101) / 500.f;
        f->data[0][i * 4 + 1] = 1;
        f->data[0][i * 4 + 2] = 0;
    }
    upload(f);
    begin(f);
    run(f, build(f, (de_view){1}, 100, true, true, (de_rg_resource){0}));
    f32 original[PIXELS], variance[PIXELS];
    for (u32 i = 0; i < PIXELS; ++i)
        original[i] = color(f)[i * 4];
    memcpy(variance, (u8 *)color(f) + 49152, sizeof variance);
    for (u32 i = 0; i < PIXELS; ++i)
        f->data[0][i * 4 + 1] = 1 + (i % W >= W / 2 ? 27 : 0);
    upload(f);
    begin(f);
    run(f, build(f, (de_view){1}, 100, true, true, (de_rg_resource){0}));
    f32 color_delta = 0, variance_delta = 0;
    const f32 *current_variance = (const f32 *)((u8 *)color(f) + 49152);
    for (u32 i = 0; i < PIXELS; ++i) {
        color_delta = fmaxf(color_delta, fabsf(original[i] - color(f)[i * 4]));
        variance_delta = fmaxf(variance_delta, fabsf(variance[i] - current_variance[i]));
    }
    printf("scalar pair auxiliary change: visibility delta%g variance delta%g\n", color_delta,
           variance_delta);
    EXPECT_FLOAT_EQ(color_delta, 0, 0);
    EXPECT_FLOAT_EQ(variance_delta, 0, 0);
}

static void scalar_pair_bounded_support(dn_fixture *f, u32 iterations) {
    f->channels = 2;
    f->iterations = iterations;
    f->use_hit = true; /* Flat depth2 doubles as a constant two-pixel support guide. */
    for (u32 i = 0; i < PIXELS; ++i) {
        f->data[0][i * 4] = .4f + (hash32(i) % 101) / 500.f;
        f->data[0][i * 4 + 1] = 1;
        f->data[0][i * 4 + 2] = 0;
    }
    upload(f);
    begin(f);
    run(f, build(f, (de_view){1}, 101, true, true, (de_rg_resource){0}));
    f32 baseline[H];
    for (u32 y = 0; y < H; ++y)
        baseline[y] = color(f)[(y * W + 31) * 4];
    /* Column34 is three pixels from the measured column31, outside both the
     * declared two-pixel support and its local variance neighborhood. */
    for (u32 y = 0; y < H; ++y)
        for (u32 x = 34; x < W; ++x)
            f->data[0][(y * W + x) * 4] += .3f;
    upload(f);
    begin(f);
    run(f, build(f, (de_view){1}, 101, true, true, (de_rg_resource){0}));
    f32 maximum = 0;
    for (u32 y = 0; y < H; ++y)
        maximum = fmaxf(maximum, fabsf(color(f)[(y * W + 31) * 4] - baseline[y]));
    printf("two-pixel AO support, %u iterations: outside-neighbor influence%g\n", iterations,
           maximum);
    EXPECT_FLOAT_EQ(maximum, 0, 0);
}

TEST_F(denoise_framework, scalar_pair_spatial_guide_bounds_sample_support, dn_fixture) {
    scalar_pair_bounded_support(f, 1);
    scalar_pair_bounded_support(f, 3);
    scalar_pair_bounded_support(f, 5);
}

TEST_F(denoise_framework, changing_signal_channel_semantics_resets_history, dn_fixture) {
    upload(f);
    begin(f);
    run(f, build(f, (de_view){1}, 102, false, false, (de_rg_resource){0}));
    begin(f);
    run(f, build(f, (de_view){1}, 102, false, false, (de_rg_resource){0}));
    EXPECT_FLOAT_EQ(age(f, 32, 16), 1, 0);
    f->channels = 2;
    begin(f);
    run(f, build(f, (de_view){1}, 102, false, false, (de_rg_resource){0}));
    EXPECT_FLOAT_EQ(age(f, 32, 16), 0, 0);
    begin(f);
    run(f, build(f, (de_view){1}, 102, false, false, (de_rg_resource){0}));
    EXPECT_FLOAT_EQ(age(f, 32, 16), 1, 0);
    f->channels = 3;
    begin(f);
    run(f, build(f, (de_view){1}, 102, false, false, (de_rg_resource){0}));
    EXPECT_FLOAT_EQ(age(f, 32, 16), 0, 0);
}
