/*
 * shader_game.c: a game that ships its own .slang shader (DENG-789).
 *
 * Built with de_add_game(ASSETS assets.toml) (tests/unit/host/fixtures/CMakeLists.txt). It does
 * what a game with a custom post pass does (VCC's CRT pass): it registers the `shader` type,
 * loads its manifest from game:/, requests its shader and takes the vertex and pixel code. Debug
 * and Dev cook tint.slang in-process; Release and Shipping read the game.dpak that de_add_game
 * cooked at build time, which the host mounts from game:/. The CTest host_game_shader
 * (tests/unit/host/CMakeLists.txt) checks the log line
 *   shader_game: shader ready (spirv vs <n> ps <n>[, metallib vs <n> ps <n>])
 */
#include <dengine/de_asset.h>
#include <dengine/de_game.h>
#include <dengine/de_platform.h>
#include <dengine/de_shader.h>
#include <stdio.h>
#include <string.h>

#define SHADER_GAME_ID "shaders/shader_game_tint"
#define SHADER_GAME_TIMEOUT_NS (90ull * 1000000000ull)

static de_log_category g_log;
static de_load_handle g_load;
static u64 g_start;
static bool g_done;

/* The vertex and pixel code for `backend`, as "<name> vs <n> ps <n>"; false when missing. */
static bool code_for(de_asset_system *a, de_shader sh, de_gpu_backend backend, const char *name,
                     char *out, size_t cap) {
    de_gpu_shader_blob vs, ps;
    de_shader_reflection refl;
    if (de_shader_get_code(a, sh, backend, 0, "vs_main", &vs, &refl) != DE_OK ||
        refl.stage != DE_SHADER_STAGE_VERTEX || vs.size == 0 ||
        de_shader_get_code(a, sh, backend, 0, "ps_main", &ps, NULL) != DE_OK || ps.size == 0) {
        const de_error *e = de_error_last();
        DE_ERROR(g_log, "shader_game: FAILED: no %s code: %s", name,
                 e != NULL ? e->message : "?");
        return false;
    }
    snprintf(out, cap, "%s vs %llu ps %llu", name, (unsigned long long)vs.size,
             (unsigned long long)ps.size);
    return true;
}

static void finish(de_engine *e, de_load_state st) {
    g_done = true;
    if (st != DE_LOAD_READY) {
        de_load_info info;
        (void)de_asset_status(e->assets, g_load, &info);
        DE_ERROR(g_log, "shader_game: FAILED: shader not loaded (state %d): %s", (int)st,
                 st == DE_LOAD_PENDING || st == DE_LOAD_LOADING ? "timed out"
                                                                : info.first_error_msg);
        return;
    }
    de_shader sh = de_asset_shader(e->assets, de_asset_id_from(SHADER_GAME_ID));
    char spirv[96], metal[96] = "";
    if (!code_for(e->assets, sh, DE_GPU_VULKAN, "spirv", spirv, sizeof spirv))
        return;
#if defined(__APPLE__)
    char m[96];
    if (!code_for(e->assets, sh, DE_GPU_METAL, "metallib", m, sizeof m))
        return;
    snprintf(metal, sizeof metal, ", %s", m);
#endif
    DE_INFO(g_log, "shader_game: shader ready (%s%s)", spirv, metal);
}

static void game_update(de_system_ctx *ctx) {
    de_engine *e = (de_engine *)ctx->user;
    if (!g_done) {
        de_load_state st = de_asset_status(e->assets, g_load, NULL);
        bool final = st != DE_LOAD_PENDING && st != DE_LOAD_LOADING;
        if (!final && de_time_now_ns() - g_start < SHADER_GAME_TIMEOUT_NS)
            return;
        finish(e, st);
    }
    de_engine_request_quit(e);
}

static de_result game_init(de_engine *e) {
    g_log = de_log_register_category("game", DE_LOG_INFO);
    /* The renderer does not register the type (de_shader.h): a game that loads shaders does. */
    DE_TRY(de_shader_register_type(e->assets, NULL));
    DE_TRY(de_asset_manifest_load(e->assets, "game:/assets.toml"));
    g_load = de_asset_load(e->assets, de_asset_id_from(SHADER_GAME_ID), 0);
    if (!DE_HANDLE_VALID(g_load))
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "shader_game: de_asset_load failed");
    g_start = de_time_now_ns();
    return DE_OK;
}

static void game_register_types(de_engine *e, de_type_registry *types) {
    (void)e;
    (void)types;
}

static void game_register_systems(de_engine *e, de_world *w) {
    de_system_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.name = "shader_game.update";
    sd.schedule = DE_SCHEDULE_UPDATE;
    sd.fn = game_update;
    sd.user = e;
    sd.flags = DE_SYSTEM_MAIN_THREAD;
    if (!DE_HANDLE_VALID(de_system_add(w, &sd)))
        DE_ERROR(g_log, "shader_game: de_system_add failed");
}

static void game_shutdown(de_engine *e) {
    if (DE_HANDLE_VALID(g_load))
        de_asset_release(e->assets, g_load);
}

static const de_game_api g_api = {
    .api_version = DE_GAME_API_VERSION,
    .name = "shader_game",
    .init = game_init,
    .shutdown = game_shutdown,
    .register_types = game_register_types,
    .register_systems = game_register_systems,
};

DE_GAME_EXPORT const de_game_api *de_game_get_api(void);
DE_GAME_EXPORT const de_game_api *de_game_get_api(void) {
    return &g_api;
}
