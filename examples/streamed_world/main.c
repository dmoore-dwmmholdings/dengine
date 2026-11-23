/* streamed_world: the smallest game with a streamed world (DENG-836).
 *
 * The manifest project/assets.toml declares [worlds.arena]: 6 cells of 256 m in a 3 x 2 grid,
 * cooked by `de-cook world` and committed under project/assets/worlds/arena. The game loads the
 * manifest and nothing else. In an editor session DIDE opens the world through the
 * world.streaming service (stream.open "arena") and moves the sources; run on its own the game
 * only shows an empty window.
 *
 * `--dev-dir <dir>` mounts a copy of project/ as dev:/ (the host does it), so a test can edit the
 * cells in a scratch copy. Without it the checkout's project/ is used. */
#include <dengine/dengine.h>

#if DE_HAS_COOKER
static de_result game_init(de_engine *e) {
    /* A renderer, so the editor can draw views and the stream overlay (as in flappy). */
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
    if (de_path_exists("dev:/assets.toml"))
        return de_asset_manifest_load(e->assets, "dev:/assets.toml");
    de_result r = de_fs_mount("streamed_world", STREAMED_WORLD_ROOT, DE_MOUNT_WRITABLE, 0);
    if (r != DE_OK)
        return r;
    return de_asset_manifest_load(e->assets, "streamed_world:/project/assets.toml");
}
#else
/* Release and Shipping have no cooker and the example has no pack: it only runs in Debug and
 * Dev. */
static de_result game_init(de_engine *e) {
    (void)e;
    DE_ERROR(DE_LOG_CAT_GENERAL, "streamed_world runs only in Debug and Dev builds");
    return DE_ERR_UNSUPPORTED;
}
#endif

static void game_register_types(de_engine *e, de_type_registry *types) {
    (void)e;
    (void)types;
}

static void game_register_systems(de_engine *e, de_world *w) {
    (void)e;
    (void)w;
}

static void game_shutdown(de_engine *e) {
    (void)e;
}

static void game_on_unload(de_engine *e) {
    (void)e;
}

static void game_on_reload(de_engine *e) {
    (void)e;
}

DE_GAME_EXPORT const de_game_api *de_game_get_api(void);
DE_GAME_EXPORT const de_game_api *de_game_get_api(void) {
    static const de_game_api api = {
        .api_version = DE_GAME_API_VERSION,
        .name = "streamed_world",
        .init = game_init,
        .shutdown = game_shutdown,
        .register_types = game_register_types,
        .register_systems = game_register_systems,
        .on_unload = game_on_unload,
        .on_reload = game_on_reload,
    };
    return &api;
}
