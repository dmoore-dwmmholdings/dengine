/* DENG-645: finite capture through public engine APIs only. No private game data.
 * Native runs are explicitly opt-in; null runs verify lifecycle, never pixel correctness. */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
#include <dengine/dengine.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { WIDTH = 64, HEIGHT = 48, PITCH = 256, FRAMES = 4 };
typedef struct probe {
    de_engine *engine;
    de_gpu_backend backend;
    de_tracking_allocator tracking;
    de_allocator allocator;
    de_gpu_texture target, uploaded;
    de_gpu_buffer readback;
    de_gpu_fence fence;
    de_view view;
    de_rg_resource color, source, destination;
    atomic_uint hooks, passes, gpu_leak_warnings, invalid_hook_info;
    u32 failure_stage, shutdowns, actual_backend;
    bool pipelined, intentional, hooked, pixels_ok, asset_pixels_ok, asset_ready;
    de_result init_result, capture_result;
    const char *output;
    u64 image_hash;
} probe;
static probe state;

static de_result windowless(void) {
    if (!state.engine->offscreen || DE_HANDLE_VALID(state.engine->window) ||
        !de_platform_headless() || de_display_count())
        return DE_FAIL(DE_ERR_INTERNAL, "offscreen fixture unexpectedly has video/window state");
    return DE_OK;
}
static void log_sink(const de_log_record *record, void *user) {
    (void)user;
    if (de_str_find(record->message, DE_STR("de_gpu_device_destroy:")) != DE_STR_NPOS &&
        de_str_find(record->message, DE_STR("still alive")) != DE_STR_NPOS)
        atomic_fetch_add_explicit(&state.gpu_leak_warnings, 1u, memory_order_relaxed);
}
static de_result fail_after(u32 stage) {
    if (state.failure_stage != stage)
        return DE_OK;
    state.intentional = true;
    return DE_FAIL(DE_ERR_CANCELLED, "intentional offscreen init failure after stage %u", stage);
}
static void clear_pass(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    probe *p = user;
    de_gpu_attachment attachment = {.texture = de_rg_texture(pass, p->color),
                                    .clear = true,
                                    .store = true,
                                    .clear_color = {0.25f, 0.5f, 0.75f, 1.0f}};
    de_gpu_cmd_begin_render(cmd, &attachment, 1, NULL);
    de_gpu_cmd_end_render(cmd);
    atomic_fetch_add_explicit(&p->passes, 1u, memory_order_relaxed);
}
static void asset_copy_pass(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    probe *p = user;
    de_gpu_cmd_copy_texture_to_buffer(cmd, de_rg_buffer(pass, p->destination), 0, PITCH,
                                      de_rg_texture(pass, p->source), 0, 0);
}
static void view_hook(de_rg *graph, de_view view, void *user) {
    probe *p = user;
    if (view.id != p->view.id)
        return;
    de_render_hook_info info;
    if (!de_view_hook_info(p->engine->renderer, view, &info) || info.width != WIDTH ||
        info.height != HEIGHT || !info.extract_frame)
        atomic_fetch_add_explicit(&p->invalid_hook_info, 1u, memory_order_relaxed);
    /* Only this render thread uses these per-graph handles. Pass callbacks execute before
     * another graph is built; main-thread cleanup first removes/drains this hook. */
    p->color = de_view_color(p->engine->renderer, view);
    de_rg_pass *pass =
        de_rg_add_pass(graph, "offscreen.fixture.clear", DE_QUEUE_GRAPHICS, clear_pass, p);
    de_rg_write(pass, p->color, DE_ACCESS_RENDER_TARGET);
    p->source = de_rg_import_texture(graph, p->uploaded, DE_ACCESS_SHADER_READ);
    p->destination = de_rg_import_buffer(graph, p->readback, DE_ACCESS_COPY_DST);
    pass = de_rg_add_pass(graph, "offscreen.fixture.asset-readback", DE_QUEUE_GRAPHICS,
                          asset_copy_pass, p);
    de_rg_read(pass, p->source, DE_ACCESS_COPY_SRC);
    de_rg_write(pass, p->destination, DE_ACCESS_COPY_DST);
    de_rg_pass_side_effect(pass);
    de_rg_export(graph, p->source, DE_ACCESS_SHADER_READ);
    atomic_fetch_add_explicit(&p->hooks, 1u, memory_order_relaxed);
}
static void custom_cleanup(de_engine *e) {
    if (state.hooked) {
        de_result r = de_render_remove_hook(e->renderer, DE_HOOK_UI, view_hook, &state);
        if (r != DE_OK) {
            state.capture_result = r;
            /* Do not race recording after a failed drain. Host teardown first stops the
             * render bridge, then releases the device; leftover-object warnings fail us. */
            return;
        }
        state.hooked = false;
    }
    if (DE_HANDLE_VALID(state.view))
        de_view_destroy(e->renderer, state.view);
    if (e->gpu) {
        de_gpu_wait_idle(e->gpu);
        de_gpu_fence_destroy(e->gpu, state.fence);
        de_gpu_buffer_destroy(e->gpu, state.readback);
        de_gpu_texture_destroy(e->gpu, state.target);
    }
    state.view = (de_view){0};
    state.fence = (de_gpu_fence){0};
    state.readback = (de_gpu_buffer){0};
    state.target = (de_gpu_texture){0};
}
static de_result initialize(de_engine *e) {
    DE_TRY(windowless());
    DE_TRY(de_gpu_device_create(&(de_gpu_device_desc){.backend = state.backend,
                                                      .debug_layer = true,
                                                      .gpu_validation = true,
                                                      .allocator = &state.allocator,
                                                      .pipeline_cache_dir = ""},
                                &e->gpu));
    state.actual_backend = (u32)de_gpu_get_caps(e->gpu)->backend;
    if (state.actual_backend != (u32)state.backend)
        return DE_FAIL(DE_ERR_INTERNAL, "fixture device backend differs from request");
    DE_TRY(fail_after(1));
    DE_TRY(de_renderer_create(&(de_renderer_desc){.device = e->gpu,
                                                  .assets = e->assets,
                                                  .types = e->types,
                                                  .allocator = &state.allocator},
                              &e->renderer));
    DE_TRY(fail_after(2));
    de_renderer_set_settings(e->renderer, &(de_render_settings){.render_scale = 1.0f});
    de_renderer_attach(e->renderer, e->world);
    DE_TRY(de_cvar_set(de_cvar_find("r.pipelined"), state.pipelined ? "1" : "0"));
    DE_TRY(de_asset_manifest_load(e->assets, "game:/offscreen-assets.toml"));
    de_asset_id id = de_asset_id_from("offscreen/checker");
    de_load_handle load = de_asset_load(e->assets, id, DE_LOAD_DEFAULT);
    const u64 deadline = de_time_now_ns() + 120ULL * 1000000000ULL;
    for (;;) {
        de_renderer_pump_uploads(e->renderer);
        de_load_info info;
        de_load_state status = de_asset_wait(e->assets, load, 1000000);
        if (status == DE_LOAD_READY)
            break;
        if (status == DE_LOAD_FAILED || status == DE_LOAD_PARTIAL || status == DE_LOAD_INVALID) {
            de_asset_status(e->assets, load, &info);
            return DE_FAIL(DE_ERR_IO, "fixture texture failed: %s", info.first_error_msg);
        }
        if (de_time_now_ns() >= deadline)
            return DE_FAIL(DE_ERR_TIMEOUT, "fixture texture upload timed out");
        de_app_event events[8];
        (void)de_platform_pump(events, 8);
    }
    de_texture texture = de_asset_texture(e->assets, id);
    u32 width = 0, height = 0;
    if (!de_render_texture_size(e->renderer, texture, &width, &height) || width != 2 || height != 2)
        return DE_FAIL(DE_ERR_INTERNAL, "fixture texture dimensions differ");
    state.uploaded = de_render_texture_gpu(e->renderer, texture);
    state.asset_ready = DE_HANDLE_VALID(state.uploaded);
    if (!state.asset_ready)
        return DE_FAIL(DE_ERR_INTERNAL, "fixture uploaded texture unavailable");
    DE_TRY(fail_after(3));
    state.target = de_gpu_texture_create(
        e->gpu, &(de_gpu_texture_desc){.format = DE_FMT_RGBA8_UNORM,
                                       .width = WIDTH,
                                       .height = HEIGHT,
                                       .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED |
                                                DE_USAGE_COPY_SRC,
                                       .name = "offscreen fixture target"});
    state.readback =
        de_gpu_buffer_create(e->gpu, &(de_gpu_buffer_desc){.size = 512 + PITCH * HEIGHT,
                                                           .usage = DE_USAGE_COPY_DST,
                                                           .memory = DE_MEM_READBACK,
                                                           .name = "offscreen fixture readback"});
    state.fence = de_gpu_fence_create(e->gpu, 0);
    if (!DE_HANDLE_VALID(state.target) || !DE_HANDLE_VALID(state.readback) ||
        !DE_HANDLE_VALID(state.fence))
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "fixture capture resources unavailable");
    state.view = de_view_create(
        e->renderer, &(de_view_desc){.target = state.target, .name = "offscreen.fixture"});
    if (!DE_HANDLE_VALID(state.view))
        return DE_FAIL(DE_ERR_INTERNAL, "fixture view unavailable");
    de_component components[] = {de_component_find(e->world, "DeCamera"),
                                 de_builtin_components(e->world).transform};
    de_entity camera;
    DE_TRY(de_entity_spawn_with(e->world, components, 2, &camera));
    DeCamera *c = de_entity_get(e->world, camera, components[0]);
    c->view = state.view;
    c->exposure_ev100 = 0;
    de_render_add_hook(e->renderer, DE_HOOK_UI, view_hook, &state);
    state.hooked = true;
    return fail_after(4);
}
static de_result init(de_engine *e) {
    state.engine = e;
    state.init_result = initialize(e);
    if (state.init_result != DE_OK)
        custom_cleanup(e); /* shutdown is not called when init fails. */
    return state.init_result;
}
static de_result capture(de_engine *e) {
    DE_TRY(windowless());
    DE_TRY(de_render_remove_hook(e->renderer, DE_HOOK_UI, view_hook, &state));
    state.hooked = false;
    DE_TRY(de_renderer_wait_frame(e->renderer, FRAMES));
    de_gpu_cmd *cmd = de_gpu_cmd_begin(e->gpu, DE_QUEUE_GRAPHICS, "offscreen fixture capture");
    if (!cmd)
        return DE_ERR_INTERNAL;
    de_gpu_barrier barrier = {
        .texture = state.target, .before = DE_ACCESS_SHADER_READ, .after = DE_ACCESS_COPY_SRC};
    de_gpu_cmd_barriers(cmd, &barrier, 1);
    de_gpu_cmd_copy_texture_to_buffer(cmd, state.readback, 512, PITCH, state.target, 0, 0);
    DE_TRY(de_gpu_queue_submit(
        e->gpu, DE_QUEUE_GRAPHICS,
        &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1, .signal = state.fence, .signal_value = 1}));
    DE_TRY(de_gpu_fence_wait(e->gpu, state.fence, 1, 30ULL * 1000000000ULL));
    const u8 *bytes = de_gpu_buffer_map(e->gpu, state.readback);
    if (!bytes)
        return DE_FAIL(DE_ERR_INTERNAL, "fixture readback unavailable");
    /* checker.png is an independent2x2 RGB fixture: black/white, white/black. */
    static const u8 checker[] = {0,   0,   0,   255, 255, 255, 255, 255,
                                 255, 255, 255, 255, 0,   0,   0,   255};
    state.asset_pixels_ok = !memcmp(bytes, checker, 8) && !memcmp(bytes + PITCH, checker + 8, 8);
    bool native = state.backend != DE_GPU_NULL;
    u64 hash = 14695981039346656037ULL;
    u32 nonzero = 0;
    bool ordered_color = true;
    FILE *file = NULL;
    if (state.output && native) {
#if defined(_WIN32)
        if (fopen_s(&file, state.output, "wb"))
            file = NULL;
#else
        file = fopen(state.output, "wb");
#endif
    }
    if (state.output && native && !file)
        return DE_FAIL(DE_ERR_IO, "cannot create fixture capture");
    bool ok = true;
    for (u32 y = 0; y < HEIGHT; ++y) {
        const u8 *row = bytes + 512 + y * PITCH;
        /* The full-target blue-biased clear must reach readback, not merely some nonzero
         * background. Tone/output conversion preserves this widely separated channel order. */
        for (u32 x = 0; x < WIDTH; ++x)
            if (!(row[x * 4] < row[x * 4 + 1] && row[x * 4 + 1] < row[x * 4 + 2]) ||
                row[x * 4 + 3] != 255)
                ordered_color = false;
        for (u32 x = 0; x < WIDTH * 4; ++x) {
            hash = (hash ^ row[x]) * 1099511628211ULL;
            if (x % 4 != 3 && row[x])
                ++nonzero;
        }
        if (file && fwrite(row, 1, WIDTH * 4, file) != WIDTH * 4)
            ok = false;
    }
    if (file && fclose(file))
        ok = false;
    state.image_hash = hash;
    state.pixels_ok = native && nonzero && ordered_color && state.asset_pixels_ok;
    if (!ok || (native && !state.pixels_ok))
        return DE_FAIL(DE_ERR_IO, "fixture output/texture readback failed");
    return DE_OK;
}
static void shutdown(de_engine *e) {
    ++state.shutdowns;
    state.capture_result = capture(e);
    custom_cleanup(e);
}
static void types(de_engine *e, de_type_registry *t) {
    (void)e;
    (void)t;
}
static void systems(de_engine *e, de_world *w) {
    (void)e;
    (void)w;
}
static int option_error(void) {
    fprintf(stderr, "usage: de-offscreen-capture --backend null|metal|vulkan|d3d12 --pipeline 0|1 "
                    "--fail-after 0..4 --scratch <existing-directory> [--output <rgba-file>] "
                    "[--allow-native]\n");
    return 2;
}
int main(int argc, char **argv) {
    bool allow_native = false;
    const char *scratch = NULL;
    state.backend = DE_GPU_NULL;
    state.capture_result = DE_OK;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--allow-native")) {
            allow_native = true;
            continue;
        }
        if (i + 1 == argc)
            return option_error();
        const char *option = argv[i], *value = argv[++i];
        if (!strcmp(option, "--scratch"))
            scratch = value;
        else if (!strcmp(option, "--output"))
            state.output = value;
        else if (!strcmp(option, "--pipeline") && (!strcmp(value, "0") || !strcmp(value, "1")))
            state.pipelined = *value == '1';
        else if (!strcmp(option, "--fail-after") && strlen(value) == 1 && *value >= '0' &&
                 *value <= '4')
            state.failure_stage = (u32)(*value - '0');
        else if (!strcmp(option, "--backend")) {
            if (!strcmp(value, "null"))
                state.backend = DE_GPU_NULL;
            else if (!strcmp(value, "metal"))
                state.backend = DE_GPU_METAL;
            else if (!strcmp(value, "vulkan"))
                state.backend = DE_GPU_VULKAN;
            else if (!strcmp(value, "d3d12"))
                state.backend = DE_GPU_D3D12;
            else
                return option_error();
        } else
            return option_error();
    }
    if (!scratch || (state.backend != DE_GPU_NULL && !allow_native))
        return option_error();
    /* Defense against accidental video startup, not Space/activation acceptance evidence. */
#if defined(_WIN32)
    if (_putenv_s("SDL_VIDEO_DRIVER", "dengine-offscreen-no-video") ||
        _putenv_s("SDL_VIDEODRIVER", "dengine-offscreen-no-video"))
        return 2;
#else
    if (setenv("SDL_VIDEO_DRIVER", "dengine-offscreen-no-video", 1) ||
        setenv("SDL_VIDEODRIVER", "dengine-offscreen-no-video", 1))
        return 2;
#endif
    de_fs_desc fs = {.game_dir = DE_OFFSCREEN_DATA_DIR, .user_dir = scratch, .cache_dir = scratch};
    if (de_fs_init(&fs) != DE_OK || de_log_init(NULL) != DE_OK)
        return 1;
    de_log_add_sink(log_sink, NULL);
    state.allocator = de_tracking_allocator_init(&state.tracking, NULL, DE_MEM_RENDER);
    de_game_api api = {.api_version = DE_GAME_API_VERSION,
                       .name = "offscreen_capture",
                       .init = init,
                       .shutdown = shutdown,
                       .register_types = types,
                       .register_systems = systems};
    char *args[] = {"offscreen_capture", "--frames", "4"};
    int host = de_host_run(&(de_host_desc){.static_api = &api,
                                           .offscreen = true,
                                           .game_toml = "game:/offscreen.toml",
                                           .argc = 3,
                                           .argv = args});
    de_log_flush();
    de_tracking_stats tracked = de_tracking_allocator_stats(&state.tracking);
    u32 hooks = atomic_load_explicit(&state.hooks, memory_order_relaxed);
    u32 passes = atomic_load_explicit(&state.passes, memory_order_relaxed);
    u32 leaks = atomic_load_explicit(&state.gpu_leak_warnings, memory_order_relaxed);
    u32 invalid_hooks = atomic_load_explicit(&state.invalid_hook_info, memory_order_relaxed);
    bool ok = state.failure_stage
                  ? host == 1 && state.intentional && !state.shutdowns
                  : host == 0 && state.shutdowns == 1 && state.capture_result == DE_OK &&
                        hooks == FRAMES && passes == FRAMES;
    ok = ok && !tracked.bytes_live && !tracked.blocks_live && !leaks && !invalid_hooks;
    printf("{\"ok\":%s,\"backend\":%u,\"actual_backend\":%u,\"pipeline\":%u,\"failure_stage\":%u,"
           "\"intentional\":%s,\"host_exit\":%d,\"init_result\":%d,\"capture_result\":%d,"
           "\"shutdowns\":%u,\"hooks\":%u,\"passes\":%u,\"width\":%u,\"height\":%u,"
           "\"native_pixels\":%s,\"asset_pixels\":%s,\"asset_ready\":%s,\"image_hash\":\"%016llx\","
           "\"tracked_bytes\":%llu,\"tracked_blocks\":%llu,\"gpu_leak_warnings\":%u,"
           "\"invalid_hook_info\":%u}\n",
           ok ? "true" : "false", (unsigned)state.backend, state.actual_backend, state.pipelined,
           state.failure_stage, state.intentional ? "true" : "false", host, (int)state.init_result,
           (int)state.capture_result, state.shutdowns, hooks, passes, WIDTH, HEIGHT,
           state.pixels_ok ? "true" : "false", state.asset_pixels_ok ? "true" : "false",
           state.asset_ready ? "true" : "false", (unsigned long long)state.image_hash,
           (unsigned long long)tracked.bytes_live, (unsigned long long)tracked.blocks_live, leaks,
           invalid_hooks);
    de_log_remove_sink(log_sink, NULL);
    de_log_shutdown();
    de_fs_shutdown();
    return ok ? 0 : 1;
}
