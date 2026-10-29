/* DENG-691: real cooked multi-mesh metadata. No model is placed into the game world;
 * inspection requests trigger an ordinary asynchronous asset load. */
#include <dengine/dengine.h>
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
        DE_TRY(de_fs_mount("modeldemo", MODEL_INSPECTION_ROOT, DE_MOUNT_READ_ONLY, 0));
        manifest = "modeldemo:/examples/model_inspection/assets.toml";
    }
    return de_asset_manifest_load(engine->assets, manifest);
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
    (void)engine;
}
DE_GAME_EXPORT const de_game_api *de_game_get_api(void);
DE_GAME_EXPORT const de_game_api *de_game_get_api(void) {
    static const de_game_api api = {.api_version = DE_GAME_API_VERSION,
                                    .name = "model_inspection",
                                    .init = init,
                                    .register_types = types,
                                    .register_systems = systems,
                                    .shutdown = shutdown};
    return &api;
}
