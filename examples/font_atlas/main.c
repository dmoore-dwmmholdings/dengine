/* DENG-689 / DIDE-R165: a real text owner with baked and generated runtime atlas pages.
 * Run with --headless; inspection does not need a GPU or a native game window. */
#include <dengine/dengine.h>
#include <string.h>
static struct {
    de_load_handle load;
    de_font latin, cjk;
    bool requested;
} app;
static de_result init(de_engine *engine) {
    memset(&app, 0, sizeof app);
    const char *manifest = "dev:/assets.toml";
    if (!de_path_exists(manifest)) {
        DE_TRY(de_fs_mount("fontdemo", FONT_ATLAS_DEMO_ROOT, DE_MOUNT_READ_ONLY, 0));
        manifest = "fontdemo:/examples/font_atlas/assets.toml";
    }
    DE_TRY(de_asset_manifest_load(engine->assets, manifest));
    app.latin = de_asset_font(engine->assets, de_asset_id_from("font_atlas/latin"));
    app.cjk = de_asset_font(engine->assets, de_asset_id_from("font_atlas/cjk"));
    de_text_set_fallbacks(engine->text, app.latin, &app.cjk, 1);
    app.load = de_asset_load_bundle(engine->assets, "font_atlas", DE_LOAD_PRIORITY_HIGH);
    return app.load.id ? DE_OK : DE_ERR_NOT_FOUND;
}
static void generate(de_system_ctx *ctx) {
    de_engine *engine = ctx->user;
    if (app.requested || de_asset_status(engine->assets, app.load, NULL) != DE_LOAD_READY)
        return;
    /* Ä exists in the source face but is outside the baked ASCII set. Shaping requests its
     * real background glyph generation. The next text updates integrate a runtime page. */
    de_text_style style = {
        .font = app.latin, .size_px = 12, .color = {1, 1, 1, 1}, .language = "en"};
    de_text_layout *layout = NULL;
    if (de_text_layout_build(
            engine->text,
            &(de_text_layout_desc){.utf8 = de_str_from_cstr("AÄ一丁"), .style = &style}, NULL,
            &layout) == DE_OK) {
        de_text_layout_free(layout);
        app.requested = true;
    }
}
static void systems(de_engine *engine, de_world *world) {
    (void)de_system_add(world,
                        &(de_system_desc){.name = "font_atlas_demo.generate",
                                          .schedule = DE_SCHEDULE_POST_UPDATE,
                                          .fn = generate,
                                          .user = engine,
                                          .flags = DE_SYSTEM_MAIN_THREAD | DE_SYSTEM_EXCLUSIVE});
}
static void types(de_engine *engine, de_type_registry *registry) {
    (void)engine;
    (void)registry;
}
static void shutdown(de_engine *engine) {
    if (app.load.id)
        de_asset_release(engine->assets, app.load);
}
DE_GAME_EXPORT const de_game_api *de_game_get_api(void);
DE_GAME_EXPORT const de_game_api *de_game_get_api(void) {
    static const de_game_api api = {.api_version = DE_GAME_API_VERSION,
                                    .name = "font_atlas_demo",
                                    .init = init,
                                    .shutdown = shutdown,
                                    .register_types = types,
                                    .register_systems = systems};
    return &api;
}
