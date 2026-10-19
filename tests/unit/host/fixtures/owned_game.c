/* DENG-510: ordinary game callbacks, using engine public headers only. */
#include "owned_game.h"
#include <dengine/dengine.h>
#include <string.h>
static de_tracking_allocator tracking;
static de_allocator allocator;
static owned_game_result result;
static u32 mode;
static de_engine *engine;
void owned_game_reset(u32 requested) {
    memset(&result, 0, sizeof result);
    mode = requested;
    allocator = de_tracking_allocator_init(&tracking, NULL, DE_MEM_GENERAL);
}
owned_game_result owned_game_get_result(void) {
    result.allocations = de_tracking_allocator_stats(&tracking);
    return result;
}
#if DE_HAS_RENDER && DE_HAS_GPU
static de_result probe_load(de_asset_load_ctx *ctx) {
    /* Keep an actual GPU allocation until asset destruction, after game.shutdown. */
    de_gpu_buffer *buffer = DE_NEW(&allocator, de_gpu_buffer, DE_MEM_ASSET);
    if (!buffer)
        return DE_ERR_OUT_OF_MEMORY;
    *buffer = de_gpu_buffer_create(
        engine->gpu,
        &(de_gpu_buffer_desc){.size = 64, .usage = DE_USAGE_STORAGE, .memory = DE_MEM_GPU_ONLY});
    if (!buffer->id) {
        DE_FREE(&allocator, buffer, sizeof *buffer);
        return DE_ERR_UNSUPPORTED;
    }
    de_asset_load_ctx_set_object(ctx, buffer, sizeof *buffer);
    return DE_OK;
}
static void probe_unload(void *object, void *user) {
    de_engine *e = user;
    /* ASan catches a freed renderer/device here; the calls also assert useful live state. */
    result.dependencies_alive =
        e->gpu && e->renderer && de_gpu_get_caps(e->gpu) && de_renderer_stats(e->renderer);
    de_gpu_buffer_destroy(e->gpu, *(de_gpu_buffer *)object);
    DE_FREE(&allocator, object, sizeof(de_gpu_buffer));
    ++result.unloaded;
    DE_INFO((de_log_category){0}, "owned_game: asset unload, dependencies %s",
            result.dependencies_alive ? "alive" : "dead");
}
#endif
static de_result init(de_engine *e) {
    owned_game_reset(mode);
    engine = e;
#if DE_HAS_RENDER && DE_HAS_GPU
    de_result r = de_gpu_device_create(&(de_gpu_device_desc){.allocator = &allocator}, &e->gpu);
    if (r != DE_OK) {
        result.unavailable = r == DE_ERR_UNSUPPORTED || r == DE_ERR_NOT_FOUND;
        if (result.unavailable)
            DE_INFO((de_log_category){0}, "owned_game: native GPU unavailable");
        return r;
    }
    if (mode == 1)
        return DE_FAIL(DE_ERR_INTERNAL, "owned game: intentional failure after GPU");
    if (mode == 5) {
        de_gpu_device_destroy(e->gpu);
        e->gpu = NULL;
        return DE_FAIL(DE_ERR_INTERNAL, "owned game: intentional failure after self-destroy");
    }
    DE_TRY(de_renderer_create(
        &(de_renderer_desc){
            .device = e->gpu, .assets = e->assets, .types = e->types, .allocator = &allocator},
        &e->renderer));
    if (mode == 2)
        return DE_FAIL(DE_ERR_INTERNAL, "owned game: intentional failure after renderer");
    de_renderer_attach(e->renderer, e->world);
    DE_TRY(de_text_attach(e->text, e->renderer, e->world));
#if DE_HAS_UI
    DE_TRY(de_ui_create(
        &(de_ui_desc){
            .allocator = &allocator, .renderer = e->renderer, .text = e->text, .input = e->input},
        &e->ui));
    de_ui_attach(e->ui, e->world);
    if (mode == 4) {
        de_ui_destroy(e->ui);
        e->ui = NULL;
    }
#endif
    if (mode == 3)
        return DE_FAIL(DE_ERR_INTERNAL, "owned game: intentional failure after attach");
    de_asset_type_desc probe = {.name = "host_owned_probe",
                                .load = probe_load,
                                .unload = probe_unload,
                                .user = e,
                                .main_thread_load = true};
    if (!de_asset_type_register(e->assets, &probe).id)
        return DE_ERR_INTERNAL;
    DE_TRY(de_asset_manifest_load(e->assets, "game:/owned_assets.toml"));
    de_load_handle load = de_asset_load(e->assets, de_asset_id_from("probe"), 0);
    if (de_asset_wait(e->assets, load, 30ULL * 1000000000) != DE_LOAD_READY)
        return DE_ERR_IO;
    /* Leave the request owned by the asset system: it must unload after shutdown. */
    de_component camera = de_component_find(e->world, "DeCamera");
    de_builtin_component_ids builtin = de_builtin_components(e->world);
    de_component components[] = {builtin.transform, camera};
    de_entity entity;
    DE_TRY(de_entity_spawn_with(e->world, components, 2, &entity));
    *(DeTransform *)de_entity_get(e->world, entity, builtin.transform) = de_transform_identity();
    return DE_OK;
#else
    result.unavailable = true;
    return DE_ERR_UNSUPPORTED;
#endif
}
static void update(de_system_ctx *ctx) {
    if (++result.frames == 3)
        de_engine_request_quit(ctx->user);
}
#if DE_HAS_RENDER && DE_HAS_GPU
static void copy_gpu_component(void *destination, const void *source) {
    memcpy(destination, source, sizeof(u32));
}
static void destruct_gpu_component(void *object) {
    (void)object;
    result.destructor_dependencies_alive = engine->gpu && engine->renderer &&
                                           de_gpu_get_caps(engine->gpu) &&
                                           de_renderer_stats(engine->renderer);
    ++result.destructed;
    DE_INFO((de_log_category){0}, "owned_game: component destruct, dependencies %s",
            result.destructor_dependencies_alive ? "alive" : "dead");
}
#endif
static void register_types(de_engine *e, de_type_registry *types) {
#if DE_HAS_RENDER && DE_HAS_GPU
    de_type_desc type = {.name = "DeOwnedGPU",
                         .size = sizeof(u32),
                         .align = DE_ALIGNOF(u32),
                         .version = 1,
                         .hooks = {.destruct = destruct_gpu_component, .copy = copy_gpu_component}};
    de_component component = de_component_register(e->world, de_type_register(types, &type), 0);
    de_entity entity = de_entity_spawn(e->world);
    de_entity_add(e->world, entity, component);
#else
    (void)e;
    (void)types;
#endif
}
static void register_systems(de_engine *e, de_world *world) {
    de_system_add(world, &(de_system_desc){.name = "owned_game.update",
                                           .fn = update,
                                           .user = e,
                                           .schedule = DE_SCHEDULE_UPDATE,
                                           .flags = DE_SYSTEM_MAIN_THREAD});
}
static void shutdown(de_engine *e) {
    (void)e;
    ++result.shutdowns; /* Leave transferred resources for the host. */
    DE_INFO((de_log_category){0}, "owned_game: shutdown after %u frames", result.frames);
}
DE_GAME_EXPORT const de_game_api *de_game_get_api(void) {
    static const de_game_api api = {.api_version = DE_GAME_API_VERSION,
                                    .name = "owned_game",
                                    .init = init,
                                    .shutdown = shutdown,
                                    .register_types = register_types,
                                    .register_systems = register_systems};
    return &api;
}
