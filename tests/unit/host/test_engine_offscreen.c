/* CPU lifecycle checks: SDL video must never start, including before game init.
 * Real GPU/render/readback and native Space checks belong to the separate GPU fixture. */
#include "de_test.h"
#include "host/engine_internal.h"
#include <dengine/de_debug.h>
#include <dengine/de_gpu.h>
#include <dengine/de_render.h>
#include <stdatomic.h>
#include <string.h>
#if !defined(DE_HEADLESS)
#include <SDL3/SDL.h>
#endif

TEST(engine_offscreen, incompatible_modes_refuse_before_platform) {
    de_engine *e = (de_engine *)(uintptr_t)1;
    EXPECT_EQ(de__engine_create(&(de__engine_desc){.headless = true, .offscreen = true}, &e),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(e == NULL);
    EXPECT_TRUE(de_platform_headless());
}

#if defined(DE_HEADLESS)
TEST(engine_offscreen, server_build_refuses_gpu_mode) {
    de_engine *e = NULL;
    EXPECT_EQ(de__engine_create(&(de__engine_desc){.offscreen = true}, &e), DE_ERR_UNSUPPORTED);
    EXPECT_TRUE(e == NULL);
}
#else
static void expect_no_video(void) {
    EXPECT_EQ(SDL_WasInit(SDL_INIT_VIDEO), 0u);
    EXPECT_TRUE(de_platform_headless()); /* Video absence, not the host's server policy. */
    EXPECT_EQ(de_display_count(), 0u);
    de_window window = {0};
    EXPECT_EQ(de_window_create(&(de_window_desc){.width = 64, .height = 64}, &window),
              DE_ERR_UNSUPPORTED);
    EXPECT_FALSE(DE_HANDLE_VALID(window));
    EXPECT_EQ(de_window_create(&(de_window_desc){.hidden = true}, &window), DE_ERR_UNSUPPORTED);
    EXPECT_EQ(de_platform_cursor_show(false), DE_ERR_UNSUPPORTED);
    EXPECT_EQ(de_platform_mouse_relative((de_window){0}, true), DE_ERR_UNSUPPORTED);
    de_error_clear();
}

typedef struct offscreen_fx {
    de_engine *engine;
    bool platform;
    de_tracking_allocator track;
    de_allocator allocator;
} offscreen_fx;
static void offscreen_fx_setup(offscreen_fx *f) {
    memset(f, 0, sizeof *f);
    f->allocator = de_tracking_allocator_init(&f->track, NULL, DE_MEM_GENERAL);
}
static void offscreen_fx_teardown(offscreen_fx *f) {
    de__engine_destroy(f->engine);
    if (f->platform)
        de_platform_shutdown();
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).bytes_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0u);
}

TEST_F(engine_offscreen, owns_events_only_platform_without_server_policy, offscreen_fx) {
    de__game_config c;
    de__game_config_defaults(&c);
    c.task_workers = 1;
    ASSERT_EQ(de__engine_create(
                  &(de__engine_desc){.allocator = &f->allocator, .config = &c, .offscreen = true},
                  &f->engine),
              DE_OK);
    EXPECT_TRUE(f->engine->offscreen);
    EXPECT_FALSE(de__engine_from(f->engine)->headless);
    EXPECT_FALSE(DE_HANDLE_VALID(f->engine->window));
    EXPECT_TRUE(f->engine->world != NULL && f->engine->assets != NULL);
    EXPECT_TRUE(f->engine->input != NULL && f->engine->text != NULL);
    expect_no_video();
}

TEST_F(engine_offscreen, borrows_events_only_platform_without_stopping_it, offscreen_fx) {
    ASSERT_EQ(de_platform_init(&(de_platform_desc){.flags = DE_PLATFORM_HEADLESS}), DE_OK);
    f->platform = true;
    de__game_config c;
    de__game_config_defaults(&c);
    c.task_workers = 1;
    ASSERT_EQ(de__engine_create(
                  &(de__engine_desc){.allocator = &f->allocator, .config = &c, .offscreen = true},
                  &f->engine),
              DE_OK);
    EXPECT_FALSE(de__engine_from(f->engine)->owns_platform);
    EXPECT_FALSE(de__engine_from(f->engine)->headless);
    de__engine_destroy(f->engine);
    f->engine = NULL;
    EXPECT_EQ(de_platform_init(NULL), DE_ERR_BUSY);
    de_error_clear();
    expect_no_video();
}

static unsigned init_calls, shutdown_calls, extract_calls;
static bool fail_init, gpu_probe, pipelined_probe;
static atomic_uint render_hooks;
static void probe_render_hook(de_rg *graph, de_view view, void *user) {
    (void)graph;
    (void)view;
    (void)user;
    atomic_fetch_add_explicit(&render_hooks, 1u, memory_order_relaxed);
}
static de_result probe_init(de_engine *e) {
    ++init_calls;
    EXPECT_TRUE(e->offscreen);
    EXPECT_FALSE(DE_HANDLE_VALID(e->window));
    EXPECT_FALSE(de__engine_from(e)->headless);
    expect_no_video();
    if (gpu_probe) {
        /* The null backend records the real renderer/Extract/hook lifecycle without issuing
         * native GPU work. Pixel correctness and Metal lifecycle remain separate acceptance. */
        DE_TRY(de_gpu_device_create(
            &(de_gpu_device_desc){.backend = DE_GPU_NULL, .pipeline_cache_dir = ""}, &e->gpu));
        DE_TRY(de_renderer_create(
            &(de_renderer_desc){.device = e->gpu, .assets = e->assets, .types = e->types},
            &e->renderer));
        de_renderer_attach(e->renderer, e->world);
        DE_TRY(de_cvar_set(de_cvar_find("r.pipelined"), pipelined_probe ? "1" : "0"));
        const de_component camera = de_component_find(e->world, "DeCamera");
        const de_builtin_component_ids bi = de_builtin_components(e->world);
        de_entity entity = DE_ENTITY_NULL;
        de_component components[] = {camera, bi.transform};
        DE_TRY(de_entity_spawn_with(e->world, components, 2, &entity));
        de_render_add_hook(e->renderer, DE_HOOK_UI, probe_render_hook, NULL);
    }
    return fail_init ? DE_FAIL(DE_ERR_CANCELLED, "intentional offscreen init refusal") : DE_OK;
}
static void probe_shutdown(de_engine *e) {
    ++shutdown_calls;
    EXPECT_TRUE(e->offscreen);
    expect_no_video();
    if (gpu_probe) {
        EXPECT_EQ(de_renderer_wait_frame(e->renderer, 2), DE_OK);
        EXPECT_EQ(atomic_load_explicit(&render_hooks, memory_order_relaxed), 2u);
        EXPECT_TRUE(e->gpu != NULL && e->renderer != NULL);
    }
}
static void probe_extract(de_system_ctx *ctx) {
    (void)ctx;
    ++extract_calls;
    expect_no_video();
}
static void probe_systems(de_engine *e, de_world *w) {
    (void)e;
    de_system_desc d = {.name = "offscreen.extract",
                        .schedule = DE_SCHEDULE_EXTRACT,
                        .flags = DE_SYSTEM_MAIN_THREAD,
                        .fn = probe_extract};
    EXPECT_TRUE(DE_HANDLE_VALID(de_system_add(w, &d)));
}
static void probe_types(de_engine *e, de_type_registry *types) {
    (void)e;
    (void)types;
}
static void run_probe(bool cli, bool refused) {
    init_calls = shutdown_calls = extract_calls = 0;
    atomic_store_explicit(&render_hooks, 0u, memory_order_relaxed);
    fail_init = refused;
    de_game_api api = {.api_version = DE_GAME_API_VERSION,
                       .name = "offscreen_probe",
                       .init = probe_init,
                       .shutdown = probe_shutdown,
                       .register_types = probe_types,
                       .register_systems = probe_systems};
    char *args[] = {"offscreen_probe", "--offscreen", "--frames", "2"};
    de_fs_desc fs = {.game_dir = DE_HOST_TEST_DATA_DIR,
                     .user_dir = DE_HOST_TEST_TMP_DIR "/user",
                     .cache_dir = DE_HOST_TEST_TMP_DIR "/cache"};
    ASSERT_EQ(de_fs_init(&fs), DE_OK);
    de_host_desc desc = {.static_api = &api,
                         .offscreen = !cli,
                         .game_toml = "game:/host_run.toml",
                         .argc = 4,
                         .argv = args};
    if (!cli)
        args[1] = "unused";
    EXPECT_EQ(de_host_run(&desc), refused ? 1 : 0);
    EXPECT_EQ(init_calls, 1u);
    EXPECT_EQ(shutdown_calls, refused ? 0u : 1u);
    EXPECT_EQ(extract_calls, refused ? 0u : 2u);
    EXPECT_EQ(SDL_WasInit(SDL_INIT_VIDEO), 0u);
    de_fs_shutdown();
}
TEST(engine_offscreen, public_descriptor_runs_extract_without_video) {
    run_probe(false, false);
}
TEST(engine_offscreen, public_cli_selects_mode_before_game_init) {
    run_probe(true, false);
}
TEST(engine_offscreen, unavailable_video_driver_does_not_block_offscreen_startup) {
    /* Any accidental video startup fails safely rather than opening a test window. */
    ASSERT_TRUE(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dengine-no-such-video-driver"));
    run_probe(true, false);
    SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
}
TEST(engine_offscreen, failed_init_unwinds_without_video_or_shutdown_callback) {
    run_probe(false, true);
    run_probe(true, false); /* The failed run must not retain platform/signal/task ownership. */
}
TEST(engine_offscreen, null_gpu_renderer_hooks_and_extract_unpipelined) {
    gpu_probe = true;
    pipelined_probe = false;
    run_probe(false, false);
    gpu_probe = false;
}
TEST(engine_offscreen, null_gpu_renderer_hooks_and_extract_pipelined) {
    gpu_probe = true;
    pipelined_probe = true;
    run_probe(false, false);
    gpu_probe = false;
}
#endif
