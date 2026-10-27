#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
/* Explicit opt-in: never create a native device during ordinary CPU UI tests. */
#include "ui_test_util.h"
#include "ui/widgets/advanced_internal.h"
#include "render/world/render_world.h"
#include <stdio.h>
#include <stdlib.h>

static de_entity panel_root(ui_fx *f) {
    de_entity root = ui_node(f, DE_ENTITY_NULL);
    de_component panel = de_component_find(f->world, "DeUiWorldPanel");
    ASSERT_TRUE(panel.id);
    *(DeUiWorldPanel *)de_entity_add(f->world, root, panel) =
        (DeUiWorldPanel){.size_m = {2, 1}, .pixels_per_meter = 100, .depth_test = true};
    return root;
}
static const u8 *world_native_capture(ui_fx *f, de_gpu_texture target, de_gpu_buffer buffer) {
    ui_frame(f);
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_EXTRACT, 0, 0), DE_OK);
    ASSERT_EQ(de__render_extract_and_kick(f->renderer, f->world), DE_OK);
    de_gpu_wait_idle(f->device);
    de_gpu_cmd *cmd = de_gpu_cmd_begin(f->device, DE_QUEUE_GRAPHICS, "world UI native readback");
    de_gpu_barrier barrier = {
        .texture = target, .before = DE_ACCESS_SHADER_READ, .after = DE_ACCESS_COPY_SRC};
    de_gpu_cmd_barriers(cmd, &barrier, 1);
    de_gpu_cmd_copy_texture_to_buffer(cmd, buffer, 0, 512 * 4, target, 0, 0);
    barrier.before = DE_ACCESS_COPY_SRC;
    barrier.after = DE_ACCESS_SHADER_READ;
    de_gpu_cmd_barriers(cmd, &barrier, 1);
    ASSERT_EQ(de_gpu_queue_submit(f->device, DE_QUEUE_GRAPHICS,
                                  &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
              DE_OK);
    de_gpu_wait_idle(f->device);
    return de_gpu_buffer_map(f->device, buffer);
}

static void popup_native_compare(const u8 *before, const u8 *after, const char *name) {
    u32 inside_changed = 0, outside_changed = 0;
    for (u32 y = 0; y < 512; ++y)
        for (u32 x = 0; x < 512; ++x) {
            size_t at = ((size_t)y * 512 + x) * 4;
            bool changed = false;
            for (u32 c = 0; c < 3; ++c)
                changed |= abs((int)before[at + c] - (int)after[at + c]) > 2;
            if (y < 120 || y >= 392)
                outside_changed += changed;
            else
                inside_changed += changed;
        }
    printf("%s: inside changed=%u, outside panel guard changed=%u\n", name, inside_changed,
           outside_changed);
    if (strcmp(name, "baseline") != 0)
        EXPECT_TRUE(inside_changed > 1000);
    EXPECT_EQ(outside_changed, 0u);
    const char *directory = getenv("DE_UI_POPUP_EVIDENCE_DIR");
    if (directory && *directory) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s.ppm", directory, name);
        FILE *out = fopen(path, "wb");
        ASSERT_TRUE(out);
        fprintf(out, "P6\n512 512\n255\n");
        for (u32 i = 0; i < 512 * 512; ++i)
            ASSERT_EQ(fwrite(after + (size_t)i * 4, 1, 3, out), 3u);
        ASSERT_EQ(fclose(out), 0);
    }
}
TEST_F(ui_popup_native, generated_widgets_stay_on_world_panel, ui_fx) {
    const char *requested = getenv("DE_UI_POPUP_NATIVE_BACKEND");
    if (!requested || !*requested)
        SKIP("Set DE_UI_POPUP_NATIVE_BACKEND explicitly for supervised/remote native testing");
    de_gpu_backend backend;
#if DE_HAS_GPU_D3D12
    if (strcmp(requested, "d3d12") == 0)
        backend = DE_GPU_D3D12;
    else
#endif
#if DE_HAS_GPU_VULKAN
        if (strcmp(requested, "vulkan") == 0)
        backend = DE_GPU_VULKAN;
    else
#endif
#if DE_HAS_GPU_METAL
        if (strcmp(requested, "metal") == 0)
        backend = DE_GPU_METAL;
    else
#endif
    {
        FAIL("Requested native backend not compiled");
        return;
    }
    de_ui_destroy(f->ui);
    f->ui = NULL;
    if (de_gpu_device_create(&(de_gpu_device_desc){.backend = backend,
                                                   .debug_layer = true,
                                                   .pipeline_cache_dir = ""},
                             &f->device) != DE_OK)
        FAIL("Requested native GPU unavailable");
    ASSERT_EQ(de_renderer_create(&(de_renderer_desc){.device = f->device, .types = f->types},
                                 &f->renderer),
              DE_OK);
    de_renderer_attach(f->renderer, f->world);
    ASSERT_EQ(de_cvar_set(de_cvar_find("r.pipelined"), "0"), DE_OK);
    de_render_settings settings = de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_LOW);
    settings.ao = settings.gi = settings.reflections = settings.shadows = DE_QUALITY_OFF;
    de_renderer_set_settings(f->renderer, &settings);
    de_gpu_texture target = de_gpu_texture_create(
        f->device, &(de_gpu_texture_desc){.width = 512,
                                          .height = 512,
                                          .format = DE_FMT_RGBA8_UNORM,
                                          .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED |
                                                   DE_USAGE_COPY_SRC});
    de_view view =
        de_view_create(f->renderer, &(de_view_desc){.target = target, .name = "world-ui-unit"});
    ASSERT_EQ(de_ui_create(&(de_ui_desc){.renderer = f->renderer,
                                         .view = view,
                                         .reference_height = 256,
                                         .max_nodes = 64},
                           &f->ui),
              DE_OK);
    de_ui_attach(f->ui, f->world);
    de_entity root = panel_root(f);
    DeUiStyle *style = de_entity_add(f->world, root, f->ui->ids.style);
    *style = (DeUiStyle){.background = {0, 1, 0, 1}, .opacity = 1};
    DeTransform transform = de_transform_identity();
    transform.position.z = -3;
    *(DeTransform *)de_entity_add(f->world, root, f->ui->hierarchy.transform) = transform;
    de_entity camera = de_entity_spawn(f->world);
    *(DeTransform *)de_entity_add(f->world, camera, f->ui->hierarchy.transform) =
        de_transform_identity();
    de_component camera_type = de_component_find(f->world, "DeCamera");
    *(DeCamera *)de_entity_add(f->world, camera, camera_type) = (DeCamera){
        .view = view, .projection = DE_PROJ_ORTHOGRAPHIC, .ortho_height = 2, .near_z = .1f};
    de_gpu_buffer buffer =
        de_gpu_buffer_create(f->device, &(de_gpu_buffer_desc){.size = 512 * 512 * 4,
                                                              .usage = DE_USAGE_COPY_DST,
                                                              .memory = DE_MEM_READBACK});

    de_ui_set_viewport(f->ui, 512, 512,
                       (de_window_insets){.left = 10, .top = 20, .right = 30, .bottom = 40});
    de_entity owner = ui_node(f, root);
    *(DeUiAnchor *)de_entity_add(f->world, owner, f->ui->ids.anchor) =
        (DeUiAnchor){.offset_min = {140, 60}, .offset_max = {190, 80}};
    DeUiDropdown *drop = de_entity_add(f->world, owner, f->ui->advanced->dropdown);
    drop->option_count = 2;
    (void)world_native_capture(f, target, buffer);
    const u8 *pixels = world_native_capture(f, target, buffer);
    u8 *original = malloc(512 * 512 * 4);
    ASSERT_TRUE(original);
    memcpy(original, pixels, 512 * 512 * 4);
    const u8 *center = original + ((size_t)256 * 512 + 256) * 4;
    ASSERT_TRUE(center[1] > 100 && center[0] < 20 && center[2] < 20);
    popup_native_compare(original, original, "baseline");
    de_ui__dropdown_submit(f->ui, owner);
    pixels = world_native_capture(f, target, buffer);
    popup_native_compare(original, pixels, "dropdown");
    de_ui__dropdown_cancel(f->ui, owner);
    de_entity_add(f->world, owner, f->ui->advanced->tooltip);
    ((DeUiInteractionState *)de_entity_add(f->world, owner, f->ui->ids.state))->focused = true;
    pixels = world_native_capture(f, target, buffer);
    popup_native_compare(original, pixels, "tooltip");
    free(original);
    de_gpu_buffer_destroy(f->device, buffer);
    de_gpu_texture_destroy(f->device, target);
    ASSERT_EQ(de_cvar_set(de_cvar_find("r.pipelined"), "1"), DE_OK);
}
