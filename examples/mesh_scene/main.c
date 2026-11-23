/* DENG-817: a small 3D mesh game for editor render-mode probes. It creates the GPU renderer and
 * loads the manifest; the editor's scene.load of mesh_scene.scene (torus + pyramid, camera, sun)
 * places the meshes. Run with --editor-service. */
#include <dengine/dengine.h>
static de_load_handle model_load;
static de_result init(de_engine *engine) {
#if defined(__APPLE__)
    const de_gpu_backend backend = DE_GPU_METAL;
#else
    const de_gpu_backend backend = DE_GPU_VULKAN;
#endif
    DE_TRY(de_gpu_device_create(&(de_gpu_device_desc){.backend = backend}, &engine->gpu));
    DE_TRY(de_renderer_create(&(de_renderer_desc){.device = engine->gpu,
                                                  .window = engine->window,
                                                  .assets = engine->assets,
                                                  .types = engine->types},
                              &engine->renderer));
    de_renderer_attach(engine->renderer, engine->world);
    de_render_settings settings = de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_LOW);
    settings.ao = settings.gi = settings.reflections = settings.shadows = settings.volumetrics =
        DE_QUALITY_OFF;
    settings.upscaler = DE_UPSCALE_NONE;
    settings.render_scale = 1;
    settings.dynamic_res_target_ms = 0;
    de_renderer_set_settings(engine->renderer, &settings);
    const char *manifest = "dev:/assets.toml";
    if (!de_path_exists(manifest)) {
        DE_TRY(de_fs_mount("meshdemo", MESH_SCENE_ROOT, DE_MOUNT_READ_ONLY, 0));
        manifest = "meshdemo:/examples/mesh_scene/assets.toml";
    }
    DE_TRY(de_asset_manifest_load(engine->assets, manifest));
    /* Keep the cooked model resident so scene.load meshes resolve to GPU meshes. */
    model_load = de_asset_load(engine->assets, de_asset_id_from("models/mesh"), 0);
    return DE_OK;
}
static void types(de_engine *engine, de_type_registry *registry) {
    (void)engine;
    (void)registry;
}
static void systems(de_engine *engine, de_world *world) {
    (void)engine;
    (void)world;
}
static void shutdown(de_engine *engine) {
    de_asset_release(engine->assets, model_load);
}
DE_GAME_EXPORT const de_game_api *de_game_get_api(void);
DE_GAME_EXPORT const de_game_api *de_game_get_api(void) {
    static const de_game_api api = {.api_version = DE_GAME_API_VERSION,
                                    .name = "mesh_scene",
                                    .init = init,
                                    .register_types = types,
                                    .register_systems = systems,
                                    .shutdown = shutdown};
    return &api;
}
