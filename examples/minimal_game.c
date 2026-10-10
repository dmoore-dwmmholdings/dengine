/* minimal_game.c — shows the game API, asset loading hook/status, ECS events, and a system.
 * Sketch only: compiles against the headers; there is no engine implementation yet. */
#include <dengine/dengine.h>

typedef enum { STATE_LOADING, STATE_PLAYING } game_phase;

typedef struct game_state {
    de_load_handle level_load;
    game_phase     phase;
    de_component   c_spin;
    de_event_type  ev_asset;
    de_event_reader asset_reader;
} game_state;

typedef struct DeSpin { f32 speed_rad; } DeSpin;   /* game component */

static void spin_system(de_system_ctx *ctx);
static void loading_system(de_system_ctx *ctx);

static de_result game_init(de_engine *e) {
    static game_state gs;
    e->game_state = &gs;

    de_result r = de_asset_manifest_load(e->assets, "dev:/assets.toml");
    if (r != DE_OK) return r;

    /* The "hook": start loading a bundle; poll its status from a system. */
    gs.level_load = de_asset_load_bundle(e->assets, "level_01", DE_LOAD_PRIORITY_HIGH);
    gs.phase = STATE_LOADING;
    return DE_OK;
}

static void game_register_types(de_engine *e, de_type_registry *types) {
    game_state *gs = (game_state *)e->game_state;
    static const de_field_info spin_fields[] = {
        DE_FIELD(DeSpin, speed_rad, DE_FK_F32, DE_FIELD_DEFAULT | DE_FIELD_REPLICATE),
    };
    de_type_id t = de_type_register(types, &DE_TYPE_DESC(DeSpin, 1, spin_fields));
    gs->c_spin = de_component_register(e->world, t, DE_COMP_SNAPSHOT | DE_COMP_SERIALIZE);
}

static void game_register_systems(de_engine *e, de_world *w) {
    game_state *gs = (game_state *)e->game_state;

    de_system_desc loading = {
        .name = "game.loading", .schedule = DE_SCHEDULE_UPDATE,
        .fn = loading_system, .user = e, .flags = DE_SYSTEM_MAIN_THREAD,
    };
    de_system_add(w, &loading);

    /* Spin: FixedUpdate, deterministic, writes the transform-bearing component. */
    static de_query_term terms[1];
    terms[0] = (de_query_term){ gs->c_spin, DE_WRITE };
    static de_query_desc q;
    q = (de_query_desc){ .terms = terms, .term_count = 1 };
    de_system_desc spin = {
        .name = "game.spin", .schedule = DE_SCHEDULE_FIXED_UPDATE,
        .fn = spin_system, .user = e, .flags = DE_SYSTEM_DETERMINISTIC,
        .queries = &q, .query_count = 1,
    };
    de_system_add(w, &spin);
}

static void loading_system(de_system_ctx *ctx) {
    de_engine *e = (de_engine *)ctx->user;
    game_state *gs = (game_state *)e->game_state;
    if (gs->phase != STATE_LOADING) return;

    de_load_info info;
    switch (de_asset_status(e->assets, gs->level_load, &info)) {
    case DE_LOAD_READY:
        gs->phase = STATE_PLAYING;
        DE_INFO((de_log_category){0}, "level_01 ready: %u assets", info.assets_total);
        break;
    case DE_LOAD_PARTIAL:
    case DE_LOAD_FAILED:
        DE_ERROR((de_log_category){0}, "level_01 failed: %s", info.first_error_msg);
        gs->phase = STATE_PLAYING;   /* game decides how to handle partial loads */
        break;
    default:
        /* draw_progress_bar(info.progress); */
        break;
    }
}

typedef struct spin_job { f32 dt; } spin_job;

static void spin_chunk(const de_query_chunk *chunk, void *user) {
    const spin_job *job = (const spin_job *)user;
    DeSpin *spins = (DeSpin *)chunk->columns[0];
    for (u32 i = 0; i < chunk->count; ++i) {
        spins[i].speed_rad = de_dsin(spins[i].speed_rad + job->dt);   /* deterministic math */
    }
}

static void spin_system(de_system_ctx *ctx) {
    spin_job job = { ctx->dt };
    de_query_par_each(ctx->world, ctx->queries[0], 1024, spin_chunk, &job);
}

static void game_shutdown(de_engine *e) {
    game_state *gs = (game_state *)e->game_state;
    de_asset_release(e->assets, gs->level_load);
}

static void game_on_unload(de_engine *e) { (void)e; }
static void game_on_reload(de_engine *e) { (void)e; }

DE_GAME_EXPORT const de_game_api *de_game_get_api(void);
DE_GAME_EXPORT const de_game_api *de_game_get_api(void) {
    static const de_game_api api = {
        .api_version = DE_GAME_API_VERSION, .name = "minimal",
        .init = game_init, .shutdown = game_shutdown,
        .register_types = game_register_types, .register_systems = game_register_systems,
        .on_unload = game_on_unload, .on_reload = game_on_reload,
    };
    return &api;
}
