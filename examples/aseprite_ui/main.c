/* Live Aseprite metadata -> retained UI. DENG-687 / DIDE-R164.AC1. */
#include <dengine/dengine.h>
#include "capture.h"
#include <string.h>

static struct {
    de_load_handle load;
    de_anim_clip clip;
    de_texture atlas;
    de_entity panels[2];
    de_component image;
} app;

/* No copied asset pointers survive the frame. Requery metadata after a reload, and derive
 * UVs from the trimmed atlas rectangle rather than assuming the atlas equals the canvas.
 * This example's opaque slice is wholly within the trim; transparent/outside slices need
 * clipping or an untrimmed import before drawing them as a rectangular UI image. */
static void refresh_panels(de_system_ctx *ctx) {
    de_engine *e = ctx->user;
    sprite_ui_capture_poll();
    de_sprite_frame_info frame;
    de_sprite_slice_info slice;
    if (!de_anim_sprite_frame(e->anim, app.clip, 0, &frame) || !frame.has_geometry ||
        !frame.trim[2] || !frame.trim[3] ||
        !de_anim_sprite_slice(e->anim, app.clip, 0, de_name_from("panel"), &slice))
        return;
    if (!app.atlas.id)
        app.atlas = de_asset_texture(e->assets, de_asset_id_from("aseprite_ui/panel#atlas"));
    f32 du = (frame.uv_rect.z - frame.uv_rect.x) / (f32)frame.trim[2];
    f32 dv = (frame.uv_rect.w - frame.uv_rect.y) / (f32)frame.trim[3];
    f32 x = frame.uv_rect.x + ((f32)slice.origin.x - (f32)frame.trim[0]) * du;
    f32 y = frame.uv_rect.y + ((f32)slice.origin.y - (f32)frame.trim[1]) * dv;
    de_vec4 uv = {x, y, x + (f32)slice.width * du, y + (f32)slice.height * dv};
    for (u32 i = 0; i < 2; ++i) {
        DeUiImage *image = de_entity_get(e->world, app.panels[i], app.image);
        if (image)
            *image =
                (DeUiImage){.texture = app.atlas,
                            .uv_rect = uv,
                            .nine_slice = slice.has_nine_slice ? slice.nine_slice : (de_vec4){0},
                            .tint = {1, 1, 1, 1}};
    }
}

static de_entity panel(de_engine *e, de_entity root, de_vec4 rect) {
    de_entity entity = de_entity_spawn(e->world);
    de_component node = de_component_find(e->world, "DeUiNode");
    de_component anchor = de_component_find(e->world, "DeUiAnchor");
    DeUiNode *n = de_entity_add(e->world, entity, node);
    n->width = (de_ui_len){DE_UI_PX, rect.z};
    n->height = (de_ui_len){DE_UI_PX, rect.w};
    n->shrink = 0;
    (void)de_entity_set_parent(e->world, entity, root);
    *(DeUiAnchor *)de_entity_add(e->world, entity, anchor) = (DeUiAnchor){
        .offset_min = {rect.x, rect.y}, .offset_max = {rect.x + rect.z, rect.y + rect.w}};
    (void)de_entity_add(e->world, entity, app.image);
    return entity;
}

static de_result init(de_engine *e) {
    memset(&app, 0, sizeof app);
#if defined(__APPLE__)
    const de_gpu_backend backend = DE_GPU_METAL;
#else
    const de_gpu_backend backend = DE_GPU_VULKAN;
#endif
    DE_TRY(de_gpu_device_create(&(de_gpu_device_desc){.backend = backend}, &e->gpu));
    DE_TRY(de_renderer_create(
        &(de_renderer_desc){
            .device = e->gpu, .window = e->window, .assets = e->assets, .types = e->types},
        &e->renderer));
    de_renderer_attach(e->renderer, e->world);
    de_render_settings settings = de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_LOW);
    settings.ao = settings.gi = settings.reflections = settings.shadows = settings.volumetrics =
        DE_QUALITY_OFF;
    settings.upscaler = DE_UPSCALE_NONE;
    settings.render_scale = 1;
    settings.dynamic_res_target_ms = 0;
    de_renderer_set_settings(e->renderer, &settings);

    /* --dev-dir permits a writable project copy for DIDE tests. The default example is
     * mounted read-only: applying settings in the IDE still writes its ordinary source file. */
    const char *manifest = "dev:/assets.toml";
    if (!de_path_exists(manifest)) {
        DE_TRY(de_fs_mount("spriteui", ASEPRITE_UI_ROOT, DE_MOUNT_READ_ONLY, 0));
        manifest = "spriteui:/assets.toml";
    }
    DE_TRY(de_asset_manifest_load(e->assets, manifest));
    app.clip = de_anim_clip_asset(e->anim, de_asset_id_from("aseprite_ui/panel"));
    app.load = de_asset_load_bundle(e->assets, "panels", DE_LOAD_PRIORITY_HIGH);
    if (!app.load.id)
        return DE_ERR_NOT_FOUND;

    DE_TRY(de_ui_create(&(de_ui_desc){.renderer = e->renderer,
                                      .assets = e->assets,
                                      .input = e->input,
                                      .window = e->window,
                                      .reference_height = 480,
                                      .max_nodes = 8},
                        &e->ui));
    de_ui_attach(e->ui, e->world);
    app.image = de_component_find(e->world, "DeUiImage");
    de_component node = de_component_find(e->world, "DeUiNode");
    de_entity root = de_entity_spawn(e->world);
    *(DeUiNode *)de_entity_add(e->world, root, node) =
        (DeUiNode){.width = {DE_UI_PERCENT, 100}, .height = {DE_UI_PERCENT, 100}};
    *(DeUiStyle *)de_entity_add(e->world, root, de_component_find(e->world, "DeUiStyle")) =
        (DeUiStyle){.background = {.025f, .035f, .05f, 1}, .opacity = 1};
    app.panels[0] = panel(e, root, (de_vec4){32, 32, 280, 160});
    app.panels[1] = panel(e, root, (de_vec4){360, 32, 160, 280});

    de_builtin_component_ids ids = de_builtin_components(e->world);
    de_entity camera = de_entity_spawn(e->world);
    *(DeTransform *)de_entity_add(e->world, camera, ids.transform) = de_transform_identity();
    DeCamera *c = de_entity_add(e->world, camera, de_component_find(e->world, "DeCamera"));
    c->exposure_ev100 = 0;
    return sprite_ui_capture_init(e);
}

static void systems(de_engine *e, de_world *world) {
    static const char *before[] = {"ui.layout"};
    /* Presentation runs in Edit mode too; Update is paused by the editor service. */
    (void)de_system_add(world,
                        &(de_system_desc){.name = "aseprite_ui.refresh",
                                          .schedule = DE_SCHEDULE_POST_UPDATE,
                                          .fn = refresh_panels,
                                          .user = e,
                                          .before = before,
                                          .before_count = 1,
                                          .flags = DE_SYSTEM_MAIN_THREAD | DE_SYSTEM_EXCLUSIVE});
}
static void shutdown(de_engine *e) {
    sprite_ui_capture_shutdown();
    if (app.load.id)
        de_asset_release(e->assets, app.load);
}
static void types(de_engine *e, de_type_registry *registry) {
    (void)e;
    (void)registry;
}
DE_GAME_EXPORT const de_game_api *de_game_get_api(void);
DE_GAME_EXPORT const de_game_api *de_game_get_api(void) {
    static const de_game_api api = {.api_version = DE_GAME_API_VERSION,
                                    .name = "aseprite_ui",
                                    .init = init,
                                    .shutdown = shutdown,
                                    .register_types = types,
                                    .register_systems = systems};
    return &api;
}
