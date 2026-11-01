#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "de_test.h"
#include <dengine/de_debug.h>
#include <dengine/de_shader.h>
#include "render/post/tonemap.h"
#include "render/graph/rg_internal.h"
#include "render/renderer_internal.h"
#include "render/views_internal.h"
#include "render/components_internal.h"
#include "render/world/render_world.h"
#include "gpu/shader/shader_internal.h"
#include "asset/hot_reload_internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WAIT_NS (120ull * 1000000000ull)
static const f32 inputs[16][4] = {{0, 0, 0, 1},
                                  {0.0001f, 0.0001f, 0.0001f, 1},
                                  {0.01f, 0.01f, 0.01f, 1},
                                  {0.18f, 0.18f, 0.18f, 1},
                                  {0.76f, 0.76f, 0.76f, 1},
                                  {1, 1, 1, 1},
                                  {4, 4, 4, 1},
                                  {16, 16, 16, 1},
                                  {1, 0, 0, 1},
                                  {0, 1, 0, 1},
                                  {0, 0, 1, 1},
                                  {8, 2, 0.1f, 1},
                                  {0.01f, 0.1f, 0.001f, 1},
                                  {0.6f, 0.3f, 0.1f, 1},
                                  {0, 0.2f, 0.4f, 1},
                                  {100, 20, 2, 1}};

static double clamp01(double x) {
    return fmin(1, fmax(0, x));
}
static void matrix(double c[3], const double m[9]) {
    double out[3];
    for (u32 i = 0; i < 3; ++i)
        out[i] = m[3 * i] * c[0] + m[3 * i + 1] * c[1] + m[3 * i + 2] * c[2];
    memcpy(c, out, sizeof out);
}
/* Independent double-precision scalar references; GPU output includes half-float storage. */
static void reference(u32 which, const f32 input[4], bool punchy, double c[3]) {
    for (u32 k = 0; k < 3; ++k)
        c[k] = input[k];
    if (which == DE_TM_AGX) {
        const double a[9] = {.842479062253094,  .0784335999999992, .0792237451477643,
                             .0423282422610123, .878468636469772,  .0791661274605434,
                             .0423756549057051, .0784336,          .879142973793104};
        const double b[9] = {1.19687900512017,   -.0980208811401368, -.0990297440797205,
                             -.0528968517574562, 1.15190312990417,   -.0989611768448433,
                             -.0529716355144438, -.0980434501171241, 1.15107367264116};
        matrix(c, a);
        for (u32 k = 0; k < 3; ++k) {
            double x = clamp01((log2(fmax(c[k], 1e-10)) + 12.47393) / 16.5);
            c[k] = fmax(0, 15.5 * pow(x, 6) - 40.14 * pow(x, 5) + 31.96 * pow(x, 4) -
                               6.868 * pow(x, 3) + .4298 * x * x + .1191 * x - .00232);
        }
        if (punchy) {
            const double y = .2126 * c[0] + .7152 * c[1] + .0722 * c[2];
            for (u32 k = 0; k < 3; ++k)
                c[k] = fmax(0, y + 1.4 * (pow(c[k], 1.35) - y));
        }
        matrix(c, b);
        for (u32 k = 0; k < 3; ++k)
            c[k] = clamp01(pow(fmax(c[k], 0), 2.2));
    } else if (which == DE_TM_ACES) {
        const double a[9] = {.59719, .35458, .04823, .07600, .90834,
                             .01566, .02840, .13383, .83777};
        const double b[9] = {1.60475, -.53108, -.07367, -.10208, 1.10813,
                             -.00605, -.00327, -.07276, 1.07602};
        matrix(c, a);
        for (u32 k = 0; k < 3; ++k)
            c[k] = (c[k] * (c[k] + .0245786) - .000090537) /
                   (c[k] * (.983729 * c[k] + .432951) + .238081);
        matrix(c, b);
        for (u32 k = 0; k < 3; ++k)
            c[k] = clamp01(c[k]);
    } else if (which == DE_TM_PBR_NEUTRAL) {
        const double x = fmin(c[0], fmin(c[1], c[2]));
        const double offset = x < .08 ? x - 6.25 * x * x : .04;
        for (u32 k = 0; k < 3; ++k)
            c[k] -= offset;
        const double peak = fmax(c[0], fmax(c[1], c[2]));
        if (peak >= .76) {
            const double compressed = 1 - .24 * .24 / (peak + .24 - .76);
            const double g = 1 - 1 / (.15 * (peak - compressed) + 1);
            for (u32 k = 0; k < 3; ++k)
                c[k] = (1 - g) * c[k] * compressed / peak + g * compressed;
        }
    } else if (which == DE_TM_REINHARD) {
        const double y = .2126 * c[0] + .7152 * c[1] + .0722 * c[2];
        const double mapped = y * (1 + y / 16) / (1 + y);
        for (u32 k = 0; k < 3; ++k)
            c[k] = y > 0 ? clamp01(c[k] * mapped / y) : 0;
    } else
        for (u32 k = 0; k < 3; ++k)
            c[k] = clamp01(c[k]);
}
static f32 half(u16 h) {
    const int sign = (h & 0x8000) ? -1 : 1;
    const u32 exp = (h >> 10) & 31, mant = h & 1023;
    if (exp == 31)
        return mant ? NAN : sign * INFINITY;
    return (f32)(sign *
                 ldexp(exp ? 1.0 + mant / 1024.0 : mant / 1024.0, exp ? (int)exp - 15 : -14));
}

typedef struct tmfx {
    de_gpu_device *dev;
    de_renderer *r;
    de_arena *arena;
    de_rg *graph;
    de_gpu_texture src;
    de_gpu_buffer upload, readback;
    de_gpu_fence fence;
    de_asset_system *assets;
    de_load_handle load;
    de_world *world;
    de_type_registry *types;
    de_gpu_texture target;
    u64 frame;
} tmfx;
static void tmfx_setup(tmfx *f) {
    (void)f;
}
static void close_assets(tmfx *f) {
    if (!f->assets)
        return;
    if (f->load.id)
        de_asset_release(f->assets, f->load);
    f->load = (de_load_handle){0};
    de_asset_system_destroy(f->assets);
    f->assets = NULL;
    de_fs_unmount("tmtest", DE_TONEMAP_TEST_TMP);
    de_fs_unmount("tmtest", DE_TONEMAP_SHADER_DIR);
}
static void tmfx_teardown(tmfx *f) {
    if (f->dev)
        de_gpu_wait_idle(f->dev);
    if (f->graph)
        de__rg_destroy(f->graph);
    de_arena_destroy(f->arena);
    if (f->r)
        de_renderer_destroy(f->r);
    if (f->world)
        de_world_destroy(f->world);
    if (f->types)
        de_type_registry_destroy(f->types);
    close_assets(f);
    if (f->dev) {
        de_gpu_texture_destroy(f->dev, f->src);
        de_gpu_texture_destroy(f->dev, f->target);
        de_gpu_buffer_destroy(f->dev, f->upload);
        de_gpu_buffer_destroy(f->dev, f->readback);
        de_gpu_fence_destroy(f->dev, f->fence);
        de_gpu_device_destroy(f->dev);
    }
}
static void open_fixture(tmfx *f, bool assets) {
#if !DE_HAS_GPU_METAL
    (void)f;
    (void)assets;
    SKIP("Metal runtime qualification needs macOS");
#else
    setenv("MTL_DEBUG_LAYER", "1", 0);
    ASSERT_EQ(de_gpu_device_create(
                  &(de_gpu_device_desc){.backend = DE_GPU_METAL, .debug_layer = true}, &f->dev),
              DE_OK);
    if (assets) {
        ASSERT_EQ(de_fs_mount("tmtest", DE_TONEMAP_SHADER_DIR, 0, 0), DE_OK);
        ASSERT_EQ(de_fs_mount("tmtest", DE_TONEMAP_TEST_TMP, DE_MOUNT_WRITABLE, 1), DE_OK);
        ASSERT_EQ(de_asset_system_create(&(de_asset_system_desc){.mode = DE_ASSET_MODE_SOURCE,
                                                                 .io_threads = 1,
                                                                 .hot_reload = true},
                                         &f->assets),
                  DE_OK);
        ASSERT_EQ(de_shader_register_type(f->assets, NULL), DE_OK);
    }
    ASSERT_EQ(de_renderer_create(&(de_renderer_desc){.device = f->dev, .assets = f->assets}, &f->r),
              DE_OK);
    ASSERT_EQ(de_arena_create(&(de_arena_desc){.name = "tonemap test"}, &f->arena), DE_OK);
    ASSERT_EQ(de__rg_create(&(de__rg_desc){.device = f->dev, .name = "tonemap test"}, &f->graph),
              DE_OK);
    f->fence = de_gpu_fence_create(f->dev, 0);
    f->src = de_gpu_texture_create(
        f->dev, &(de_gpu_texture_desc){.width = 16,
                                       .height = 1,
                                       .format = DE_FMT_RGBA32_FLOAT,
                                       .usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST,
                                       .name = "tonemap input"});
    f->upload = de_gpu_buffer_create(
        f->dev,
        &(de_gpu_buffer_desc){.size = 256, .usage = DE_USAGE_COPY_SRC, .memory = DE_MEM_UPLOAD});
    f->readback = de_gpu_buffer_create(
        f->dev,
        &(de_gpu_buffer_desc){.size = 256, .usage = DE_USAGE_COPY_DST, .memory = DE_MEM_READBACK});
    ASSERT_TRUE(f->fence.id && f->src.id && f->upload.id && f->readback.id);
#endif
}
typedef struct copy_work {
    tmfx *f;
    de_rg_resource resource;
} copy_work;
static void upload(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const copy_work *p = user;
    de_gpu_cmd_copy_buffer_to_texture(cmd, de_rg_texture(pass, p->resource), 0, 0, p->f->upload, 0,
                                      256);
}
static void readback(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const copy_work *p = user;
    de_gpu_cmd_copy_texture_to_buffer(cmd, p->f->readback, 0, 256, de_rg_texture(pass, p->resource),
                                      0, 0);
}
static void run(tmfx *f, de_tonemapper mapper, const f32 params[8], bool hdr, f32 out[16][4]) {
    de__rg_reset(f->graph);
    de_arena_reset(f->arena);
    ASSERT_EQ(de__rg_begin(f->graph, f->arena), DE_OK);
    memcpy(de_gpu_buffer_map(f->dev, f->upload), inputs, sizeof inputs);
    const de_rg_resource src =
        de_rg_import_texture(f->graph, f->src, f->frame ? DE_ACCESS_SHADER_READ : DE_ACCESS_NONE);
    copy_work up = {f, src};
    de_rg_pass *p = de_rg_add_pass(f->graph, "test.upload", DE_QUEUE_GRAPHICS, upload, &up);
    de_rg_read(p, de_rg_import_buffer(f->graph, f->upload, DE_ACCESS_COPY_SRC), DE_ACCESS_COPY_SRC);
    de_rg_write(p, src, DE_ACCESS_COPY_DST);
    const de_rg_resource dst =
        de__tonemap_add(f->r, f->graph, f->arena, src, 16, 1, mapper, params, 1, hdr, NULL);
    ASSERT_TRUE(dst.id);
    copy_work rb = {f, dst};
    p = de_rg_add_pass(f->graph, "test.readback", DE_QUEUE_GRAPHICS, readback, &rb);
    de_rg_read(p, dst, DE_ACCESS_COPY_SRC);
    de_rg_write(p, de_rg_import_buffer(f->graph, f->readback, DE_ACCESS_NONE), DE_ACCESS_COPY_DST);
    de_rg_pass_side_effect(p);
    de_rg_export(f->graph, src, DE_ACCESS_SHADER_READ);
    ASSERT_EQ(de__rg_finalize(f->graph), DE_OK);
    ASSERT_EQ(de__rg_compile(f->graph), DE_OK);
    ASSERT_EQ(de__rg_execute(f->graph, &(de__rg_execute_desc){.frame_index = f->frame,
                                                              .frame_fence = f->fence,
                                                              .frame_value = f->frame + 1}),
              DE_OK);
    ASSERT_EQ(de_gpu_fence_wait(f->dev, f->fence, ++f->frame, WAIT_NS), DE_OK);
    const u16 *bytes = de_gpu_buffer_map(f->dev, f->readback);
    for (u32 i = 0; i < 16; ++i)
        for (u32 c = 0; c < 4; ++c)
            out[i][c] = half(bytes[4 * i + c]);
}
TEST_F(tonemap, builtin_reference_values, tmfx) {
    open_fixture(f, false);
    for (u32 which = 0; which < 5; ++which) {
        f32 out[16][4];
        run(f, de_tonemapper_builtin(f->r, (de_builtin_tonemapper)which), NULL, false, out);
        for (u32 i = 0; i < 16; ++i) {
            double ref[3];
            reference(which, inputs[i], false, ref);
            for (u32 c = 0; c < 3; ++c)
                EXPECT_FLOAT_EQ(out[i][c], ref[c], 1e-3);
            EXPECT_FLOAT_EQ(out[i][3], 1, 0);
        }
        if (which == DE_TM_PBR_NEUTRAL) {
            /* Below compression: zero-min colors unchanged, gray retains reference flare removal.
             */
            EXPECT_FLOAT_EQ(out[14][1], inputs[14][1], 1e-4);
            EXPECT_FLOAT_EQ(out[14][2], inputs[14][2], 1e-4);
            EXPECT_FLOAT_EQ(out[3][0], 0.14, 1e-3); /* Half-float output, reference fit budget. */
        }
    }
    f32 out[16][4], params[8] = {1};
    run(f, (de_tonemapper){0}, params, false, out);
    for (u32 i = 0; i < 16; ++i) {
        double ref[3];
        reference(DE_TM_AGX, inputs[i], true, ref);
        for (u32 c = 0; c < 3; ++c)
            EXPECT_FLOAT_EQ(out[i][c], ref[c], 1e-3);
    }
    run(f, de_tonemapper_builtin(f->r, DE_TM_NONE), NULL, true, out);
    EXPECT_FLOAT_EQ(out[7][0], 16, 0); /* HDR None is passthrough. */
}

static void write_shader(u32 variant) {
    char text[1400];
    snprintf(
        text, sizeof text,
        "import \"tonemap/itonemapper\";\nimport \"tonemap/kernel\";\n"
        "public struct Custom : ITonemapper { public float3 apply(float3 c, TonemapParams p, "
        "OutputInfo o) { return float3(%u.0) * 0.125 + p.data0.x + p.data0.y + p.data1.z; } }\n"
        "[shader(\"compute\")] [numthreads(%u,8,1)] void cs_tonemap(uint3 id : "
        "SV_DispatchThreadID, uniform TonemapPassParams pc) { de_tonemap_pixel<Custom>(id,pc); }\n",
        variant, variant == 3 ? 4 : 8);
    ASSERT_EQ(de_file_write_all_atomic("tmtest:/custom.slang", text, strlen(text)), DE_OK);
}
static void reload(tmfx *f, de_shader shader, u32 variant) {
    u32 before = 0, after = 0;
    ASSERT_TRUE(de__shader_loaded_version(f->assets, shader, &before));
    write_shader(variant);
    const u64 deadline = de_time_now_ns() + WAIT_NS;
    do {
        de_asset_system_update(f->assets, NULL);
        if (de__shader_loaded_version(f->assets, shader, &after) && after != before)
            break;
        de_sleep_ns(10000000);
    } while (de_time_now_ns() < deadline);
    if (after == before) {
        de__asset_reload_stats stats;
        de__asset_reload_get_stats(f->assets, &stats);
        char error[1024];
        de_asset_last_error(f->assets, de_asset_id_from("tonemap/custom"), error, sizeof error);
        fprintf(stderr,
                "reload %u timed out: watches=%u changes=%llu queued=%llu started=%llu "
                "swapped=%llu failed=%llu error=%s\n",
                variant, stats.watches, (unsigned long long)stats.changes,
                (unsigned long long)stats.queued, (unsigned long long)stats.started,
                (unsigned long long)stats.swapped, (unsigned long long)stats.failed, error);
    }
    ASSERT_NE(after, before);
}

typedef struct view_input {
    tmfx *f;
    de_view view;
    de_entity camera;
    bool uploaded;
    bool scene_linear;
} view_input;
static void scene_input(de__view_frame *vf, void *user) {
    view_input *state = user;
    if (vf->view.id != state->view.id)
        return;
    copy_work *work = de_arena_push(vf->graph->arena, sizeof *work, 16);
    ASSERT_TRUE(work != NULL);
    work->f = state->f;
    work->resource = de_rg_import_texture(vf->graph, state->f->src,
                                          state->uploaded ? DE_ACCESS_SHADER_READ : DE_ACCESS_NONE);
    de_rg_pass *pass =
        de_rg_add_pass(vf->graph, "test.scene_linear", DE_QUEUE_GRAPHICS, upload, work);
    de_rg_read(pass, de_rg_import_buffer(vf->graph, state->f->upload, DE_ACCESS_COPY_SRC),
               DE_ACCESS_COPY_SRC);
    de_rg_write(pass, work->resource, DE_ACCESS_COPY_DST);
    vf->color = work->resource;
    vf->color_is_scene_linear = state->scene_linear;
    vf->color_format = DE_FMT_RGBA32_FLOAT;
    state->uploaded = true;
}
static void open_view(tmfx *f, de_tonemapper mapper, const f32 params[8], view_input *state) {
    ASSERT_EQ(de_type_registry_create(NULL, &f->types), DE_OK);
    ASSERT_EQ(de_world_create(&(de_world_desc){.types = f->types}, &f->world), DE_OK);
    de_renderer_attach(f->r, f->world);
    const de_cvar pipelined = de_cvar_find("r.pipelined");
    ASSERT_EQ(de_cvar_set(pipelined, "0"), DE_OK);
    const de_render_settings settings = de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_HIGH);
    de_renderer_set_settings(f->r, &settings);
    f->target = de_gpu_texture_create(
        f->dev, &(de_gpu_texture_desc){.format = DE_FMT_RGBA16_FLOAT,
                                       .width = 16,
                                       .height = 1,
                                       .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED |
                                                DE_USAGE_COPY_SRC});
    ASSERT_TRUE(f->target.id);
    const de_view view = de_view_create(f->r, &(de_view_desc){.target = f->target});
    ASSERT_TRUE(view.id);
    const DeRenderComponents *ids = de__render_components(f->world);
    const de_builtin_component_ids bi = de_builtin_components(f->world);
    const de_component components[] = {bi.transform, ids->camera};
    de_entity entity = DE_ENTITY_NULL;
    ASSERT_EQ(de_entity_spawn_with(f->world, components, 2, &entity), DE_OK);
    DeCamera *camera = de_entity_get(f->world, entity, ids->camera);
    camera->view = view;
    camera->tonemapper = mapper;
    memcpy(camera->tonemap_params, params, sizeof camera->tonemap_params);
    *state = (view_input){.f = f, .view = view, .camera = entity, .scene_linear = true};
    memcpy(de_gpu_buffer_map(f->dev, f->upload), inputs, sizeof inputs);
    ASSERT_EQ(de__view_register_stage(f->r, DE__VIEW_SLOT_LIGHTING, 1, "test.scene_linear",
                                      scene_input, state),
              DE_OK);
}
static void render_view(tmfx *f, f32 out[16][4]) {
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_POST_UPDATE, 1.0f / 60, 0), DE_OK);
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_EXTRACT, 1.0f / 60, 0), DE_OK);
    ASSERT_EQ(de__render_extract_and_kick(f->r, f->world), DE_OK);
    de_gpu_wait_idle(f->dev);
    de__rg_reset(f->graph);
    de_arena_reset(f->arena);
    ASSERT_EQ(de__rg_begin(f->graph, f->arena), DE_OK);
    copy_work work = {f, de_rg_import_texture(f->graph, f->target, DE_ACCESS_SHADER_READ)};
    de_rg_pass *pass =
        de_rg_add_pass(f->graph, "test.target_readback", DE_QUEUE_GRAPHICS, readback, &work);
    de_rg_read(pass, work.resource, DE_ACCESS_COPY_SRC);
    de_rg_write(pass, de_rg_import_buffer(f->graph, f->readback, DE_ACCESS_NONE),
                DE_ACCESS_COPY_DST);
    de_rg_pass_side_effect(pass);
    de_rg_export(f->graph, work.resource, DE_ACCESS_SHADER_READ);
    ASSERT_EQ(de__rg_finalize(f->graph), DE_OK);
    ASSERT_EQ(de__rg_compile(f->graph), DE_OK);
    ASSERT_EQ(de__rg_execute(f->graph, &(de__rg_execute_desc){.frame_index = f->frame,
                                                              .frame_fence = f->fence,
                                                              .frame_value = f->frame + 1}),
              DE_OK);
    ASSERT_EQ(de_gpu_fence_wait(f->dev, f->fence, ++f->frame, WAIT_NS), DE_OK);
    const u16 *bytes = de_gpu_buffer_map(f->dev, f->readback);
    for (u32 i = 0; i < 16; ++i)
        for (u32 c = 0; c < 4; ++c)
            out[i][c] = half(bytes[4 * i + c]);
}
static void custom_roundtrip(tmfx *f, bool assets_first) {
#if !DE_HAS_COOKER
    SKIP("Custom source reload requires a Dev/Debug build with the cooker");
#endif
    open_fixture(f, true);
    write_shader(1);
    const char *manifest =
        "[assets]\n\"tonemap/custom\"={type=\"shader\",source=\"custom.slang\"}\n";
    ASSERT_EQ(de_file_write_all_atomic("tmtest:/assets.toml", manifest, strlen(manifest)), DE_OK);
    ASSERT_EQ(de_asset_manifest_load(f->assets, "tmtest:/assets.toml"), DE_OK);
    const de_asset_id id = de_asset_id_from("tonemap/custom");
    f->load = de_asset_load(f->assets, id, 0);
    de_load_info info = {0};
    const de_load_state state = de_asset_wait(f->assets, f->load, WAIT_NS);
    de_asset_status(f->assets, f->load, &info);
    if (state != DE_LOAD_READY)
        fprintf(stderr, "custom shader load: %s\n", info.first_error_msg);
    ASSERT_EQ(state, DE_LOAD_READY);
    const de_shader shader = de_asset_shader(f->assets, id);
    EXPECT_FALSE(de_tonemapper_register(f->r, "bad", shader, 33).id);
    EXPECT_FALSE(de_tonemapper_register(f->r, "agx", shader, 4).id);
    EXPECT_FALSE(de_tonemapper_register(f->r, "unloaded", (de_shader){0}, 0).id);
    const de_tonemapper tm = de_tonemapper_register(f->r, "custom", shader, 4);
    ASSERT_TRUE(tm.id);
    EXPECT_FALSE(de_tonemapper_register(f->r, "custom", shader, 4).id);
    f32 pixels[16][4], params[8] = {0.25f, 99, 99, 99, 99, 99, 99, 99};
    de_asset_system_update(f->assets, NULL);
    de_sleep_ns(200000000u); /* Let the native watch settle, as in the asset watcher test. */
    view_input view_state;
    open_view(f, tm, params, &view_state);
    render_view(f, pixels);
    EXPECT_FLOAT_EQ(pixels[0][0], 0.375f, 0);
    reload(f, shader, 2);
    render_view(f, pixels);
    EXPECT_FLOAT_EQ(pixels[0][0], 0.5f, 0);
    reload(f, shader, 3); /* Compiles, but wrong threadgroup: retain the last good pipeline. */
    EXPECT_FALSE(de_tonemapper_register(f->r, "wronggroup", shader, 4).id);
    render_view(f, pixels);
    EXPECT_FLOAT_EQ(pixels[0][0], 0.5f, 0);
    reload(f, shader, 4);
    render_view(f, pixels);
    EXPECT_FLOAT_EQ(pixels[0][0], 0.75f, 0);
    const DeRenderComponents *ids = de__render_components(f->world);
    DeCamera *camera = de_entity_get(f->world, view_state.camera, ids->camera);
    camera->tonemapper = (de_tonemapper){0};
    memset(camera->tonemap_params, 0, sizeof camera->tonemap_params);
    render_view(f, pixels);
    for (u32 i = 0; i < 16; ++i) {
        double ref[3];
        reference(DE_TM_AGX, inputs[i], false, ref);
        for (u32 c = 0; c < 3; ++c)
            EXPECT_FLOAT_EQ(pixels[i][c], ref[c], 1e-3);
    }
    ASSERT_EQ(
        de_view_set_display(f->r, view_state.view,
                            &(de_view_display){.disabled_features = DE_VIEW_DISABLE_POST_EFFECTS}),
        DE_OK);
    render_view(f, pixels);
    EXPECT_FLOAT_EQ(pixels[3][0], 0.18, 1e-3);
    EXPECT_FLOAT_EQ(pixels[7][0], 1, 0); /* Post-disabled still performs SDR output clamping. */
    ASSERT_EQ(de_view_set_display(f->r, view_state.view, NULL), DE_OK);
    view_state.scene_linear = false; /* Diagnostic replacements bypass tonemapping. */
    render_view(f, pixels);
    EXPECT_FLOAT_EQ(pixels[7][0], 16, 0);
    for (u32 i = 0; i < 26; ++i) {
        char name[32];
        snprintf(name, sizeof name, "copy%u", i);
        ASSERT_TRUE(de_tonemapper_register(f->r, name, shader, 0).id);
    }
    EXPECT_FALSE(de_tonemapper_register(f->r, "full", shader, 0).id);
    if (assets_first) {
        /* The public renderer lifetime contract destroys the asset system first, while all
         * 27 custom entries still own their shader pins. Teardown must not touch that system. */
        close_assets(f);
        EXPECT_FALSE(de_tonemapper_register(f->r, "after-shutdown", shader, 0).id);
    }
}
TEST_F(tonemap, custom_registration_reload_and_retained_pipeline, tmfx) {
    custom_roundtrip(f, false);
}
TEST_F(tonemap, asset_system_destroyed_before_registered_mapper, tmfx) {
    custom_roundtrip(f, true);
}
