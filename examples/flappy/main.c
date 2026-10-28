/* flappy: a Flappy Bird clone as a host game. The rules are Sky Hop's (../flappy_bird/game.h).
 * Space, Up or a click flaps; P pauses; R restarts; Esc quits.
 *
 * The level is a scene (flappy.scene, DENG-648): a camera, the sky, rail and ground, the bird,
 * the pipes and clouds, as entities with DeTransform and DeSprite. The game state is a reflected
 * component (FlappyState) on its own entity; each frame `flappy.present` moves the scene's
 * entities to match it (they are found by the FlappyBird, FlappyPipe and FlappyCloud tags). The
 * score and the messages are UI text.
 *
 * In a DIDE engine session the editor opens flappy.scene itself (the hierarchy, the inspector
 * and the scene view show it, and Save writes it back), so the game leaves it out. The game's
 * systems are held while the editor is in Edit mode, so the level stays as authored until Play. */
#include <dengine/dengine.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../flappy_bird/game.h"

#define STAGE_W 320.f
#define STAGE_H 400.f
#define PPM     32.f /* stage pixels per metre */

static const de_vec4 INK = {.09f, .11f, .15f, 1}, WHITE = {1, 1, 1, 1};

/* The reflected names of Sky Hop's types. */
typedef game FlappyState;
typedef pipe_pair FlappyPipePair;

/* Scene components. `Name` is what DIDE's hierarchy shows. */
typedef struct Name {
    char value[32];
} Name;
typedef struct FlappyBird {
    f32 flap_tilt; /* radians the bird noses up while rising */
} FlappyBird;
typedef struct FlappyPipe {
    u32 pair; /* index in FlappyState.pipes */
    u32 part; /* 0 top body, 1 top cap, 2 bottom body, 3 bottom cap */
} FlappyPipe;
typedef struct FlappyCloud {
    u32 index;
} FlappyCloud;

static struct {
    de_entity state;
    struct {
        de_component node, anchor, style, text, transform, sprite, state, name, bird, pipe, cloud;
    } c;
    de_query birds, pipes, clouds;
    bool level_spawned;
    de_load_handle font_load, art_load, level_load;
    de_font font;
    de_entity stage, score, panel, message;
    char score_text[64], message_text[160];
} app;

/* Stage pixels (y down) to world metres in the X-Z plane (Z up), the stage centred on X = 0. */
static f64 world_x(f32 px) {
    return (px - STAGE_W * .5f) / PPM;
}
static f64 world_z(f32 py) {
    return (STAGE_H - py) / PPM;
}

/* The palette is sRGB; the UI writes its colours unconverted into an sRGB swapchain, which
 * encodes them again, so they go in linear (DENG-621). */
static f32 linear(f32 c) {
    return c <= .04045f ? c / 12.92f : powf((c + .055f) / 1.055f, 2.4f);
}

static de_entity node(de_engine *e, de_entity parent, de_vec4 color, f32 radius) {
    color = (de_vec4){linear(color.x), linear(color.y), linear(color.z), color.w};
    de_entity n = de_entity_spawn(e->world);
    (void)de_entity_add(e->world, n, app.c.node);
    (void)de_entity_add(e->world, n, app.c.anchor);
    if (parent.bits)
        (void)de_entity_set_parent(e->world, n, parent);
    *(DeUiStyle *)de_entity_add(e->world, n, app.c.style) =
        (DeUiStyle){.background = color, .corner_radius = radius, .opacity = 1};
    return n;
}

/* Places `n` in stage pixels (the stage is 320x400, scaled to the window height). */
static void place(de_engine *e, de_entity n, f32 x, f32 y, f32 w, f32 h) {
    DeUiAnchor *a = de_entity_get(e->world, n, app.c.anchor);
    a->offset_min = (de_vec2){x, y};
    a->offset_max = (de_vec2){x + w, y + h};
}

static void show(de_engine *e, de_entity n, bool shown) {
    const DeUiNode *now = de_entity_get_const(e->world, n, app.c.node);
    de_ui_display display = shown ? DE_UI_FLEX : DE_UI_NONE;
    if (now->display != display)
        ((DeUiNode *)de_entity_get(e->world, n, app.c.node))->display = display;
}

static void label(de_engine *e, de_entity n, const char *str, f32 size) {
    *(DeUiText *)de_entity_add(e->world, n, app.c.text) = (DeUiText){
        .literal = {str, strlen(str)},
        .align = DE_ALIGN_CENTER,
        .style = {.font = app.font, .size_px = size, .line_height = 1.25f, .color = WHITE}};
}

static game *state(de_engine *e) {
    return (game *)de_entity_get(e->world, app.state, app.c.state);
}

static const game *state_const(de_engine *e) {
    return (const game *)de_entity_get_const(e->world, app.state, app.c.state);
}

static void control(de_system_ctx *ctx) {
    de_engine *e = ctx->user;
    de_input *in = e->input;
    game *g = state(e);
    if (g == NULL)
        return;
    if (de_key_pressed(in, DE_KEY_ESCAPE))
        de_engine_request_quit(e);
    if (de_key_pressed(in, DE_KEY_P))
        game_pause(g);
    if (de_key_pressed(in, DE_KEY_R))
        game_reset(g);
    if (de_key_pressed(in, DE_KEY_SPACE) || de_key_pressed(in, DE_KEY_UP) ||
        de_mouse_pressed(in, DE_MOUSE_LEFT))
        game_flap(g);
}

static void simulate(de_system_ctx *ctx) {
    game *g = state(ctx->user);
    if (g != NULL)
        game_tick(g, ctx->dt);
}

/* Spawns the level once it (and the textures it names) has loaded. */
static void spawn(de_system_ctx *ctx) {
    de_engine *e = ctx->user;
    if (app.level_load.id == 0 || app.level_spawned)
        return;
    de_load_info info = {0};
    const de_load_state st = de_asset_status(e->assets, app.level_load, &info);
    if (st == DE_LOAD_PENDING || st == DE_LOAD_LOADING)
        return;
    app.level_spawned = true;
    const de_asset_id level = de_asset_id_from("flappy/level");
    if (st != DE_LOAD_READY)
        DE_ERROR(DE_LOG_CAT_GENERAL, "flappy level failed to load: %s", info.first_error_msg);
    else if (de_prefab_instantiate(e->assets, e->world, de_asset_scene(e->assets, level),
                                   DE_ENTITY_NULL, NULL) != DE_OK)
        DE_ERROR(DE_LOG_CAT_GENERAL, "flappy level: %s", de_error_last()->message);
}

/* ---------- Presentation: the scene follows the state ---------- */

static void place_bird(const de_query_chunk *chunk, void *user) {
    const game *g = user;
    DeTransform *t = chunk->columns[0];
    const FlappyBird *b = chunk->columns[1];
    for (u32 i = 0; i < chunk->count; ++i) {
        /* The bird bobs while waiting, and noses up while rising. */
        f32 y = g->mode == READY ? g->y + sinf(g->time * 5) * 4 : g->y;
        f32 tilt = g->mode == READY ? 0 : fmaxf(-b[i].flap_tilt, fminf(1.2f, g->velocity * .0025f));
        t[i].position.x = world_x(BIRD_X);
        t[i].position.z = world_z(y);
        t[i].rotation = de_quat_axis_angle((de_vec3){0, 1, 0}, tilt);
    }
}

static void place_pipe(const de_query_chunk *chunk, void *user) {
    const game *g = user;
    DeTransform *t = chunk->columns[0];
    DeSprite *s = chunk->columns[1];
    const FlappyPipe *p = chunk->columns[2];
    for (u32 i = 0; i < chunk->count; ++i) {
        if (p[i].pair >= PIPE_COUNT)
            continue;
        const pipe_pair *pair = &g->pipes[p[i].pair];
        f32 top = pair->gap - GAP * .5f, bottom = pair->gap + GAP * .5f, cy = 0, h = 0;
        switch (p[i].part) {
        case 0: cy = (48 + top) * .5f, h = top - 48; break;
        case 1: cy = top - 7; break;
        case 2: cy = (bottom + FLOOR) * .5f, h = FLOOR - bottom; break;
        default: cy = bottom + 7; break;
        }
        t[i].position.x = world_x(pair->x + PIPE_W * .5f);
        t[i].position.z = world_z(cy);
        if (h > 0)
            s[i].size.y = h / PPM;
    }
}

static void place_cloud(const de_query_chunk *chunk, void *user) {
    const game *g = user;
    DeTransform *t = chunk->columns[0];
    const FlappyCloud *c = chunk->columns[1];
    for (u32 i = 0; i < chunk->count; ++i) {
        f32 n = (f32)c[i].index, w = 64 + 14 * n, span = STAGE_W + 90;
        f32 x = fmodf(40 + n * 130 - g->time * (8 + 4 * n), span);
        if (x < -90)
            x += span;
        t[i].position.x = world_x(x + w * .5f);
        t[i].position.z = world_z(70 + n * 52 + 11);
    }
}

static void present(de_system_ctx *ctx) {
    de_engine *e = ctx->user;
    const game *g = state_const(e);
    if (g == NULL)
        return;
    de_query_each(e->world, app.birds, place_bird, (void *)g);
    de_query_each(e->world, app.pipes, place_pipe, (void *)g);
    de_query_each(e->world, app.clouds, place_cloud, (void *)g);
}

static void display(de_system_ctx *ctx) {
    de_engine *e = ctx->user;
    const game *g = state_const(e);
    if (g == NULL)
        return;
    snprintf(app.score_text, sizeof app.score_text, "Score %u    Best %u", g->score, g->best);
    label(e, app.score, app.score_text, 18);
    const char *msg = NULL;
    if (g->mode == READY)
        msg = "FLAPPY\nSpace, Up or click to flap";
    else if (g->mode == PAUSED)
        msg = "Paused\nP to resume";
    else if (g->mode == DEAD) {
        snprintf(app.message_text, sizeof app.message_text,
                 "Crashed!\nScore %u - Best %u\nFlap to retry", g->score, g->best);
        msg = app.message_text;
    }
    show(e, app.panel, msg != NULL);
    if (msg)
        label(e, app.message, msg, 17);
}

static de_result wait_ready(de_engine *e, de_load_handle h, const char *what) {
    const de_load_state st = de_asset_wait(e->assets, h, 60000000000ULL);
    if (st == DE_LOAD_READY)
        return DE_OK;
    de_load_info info = {0};
    (void)de_asset_status(e->assets, h, &info);
    return DE_FAIL(DE_ERR_IO, "flappy %s failed to load (state %d, %u/%u ready, %u failed): %s",
                   what, (int)st, info.assets_ready, info.assets_total, info.assets_failed,
                   info.first_error_msg[0] ? info.first_error_msg : "no error message");
}

static de_result init(de_engine *e) {
    memset(&app, 0, sizeof app);
    (void)game_self_check; /* Sky Hop's logic test; its own target runs it. */
    /* Vulkan: the D3D12 renderer presents black frames on hardware adapters for now (DENG-619).
     * game.toml asks for a Vulkan window surface to match. */
    DE_TRY(de_gpu_device_create(&(de_gpu_device_desc){.backend = DE_GPU_VULKAN}, &e->gpu));
    DE_TRY(de_renderer_create(
        &(de_renderer_desc){
            .device = e->gpu, .window = e->window, .assets = e->assets, .types = e->types},
        &e->renderer));
    de_renderer_attach(e->renderer, e->world);
    de_render_settings settings = de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_LOW);
    settings.ao = settings.gi = settings.reflections = settings.shadows = settings.volumetrics =
        DE_QUALITY_OFF;
    /* Full resolution: the UI is drawn before the upscaler, in output pixels (DENG-620). */
    settings.upscaler = DE_UPSCALE_NONE;
    settings.render_scale = 1;
    settings.dynamic_res_target_ms = 0;
    de_renderer_set_settings(e->renderer, &settings);
    DE_TRY(de_text_attach(e->text, e->renderer, e->world));

#if DE_HAS_COOKER
    /* Debug and Dev: the checkout's sources, cooked in-process. */
    DE_TRY(de_fs_mount("flappy", FLAPPY_ROOT, DE_MOUNT_READ_ONLY, 0));
    DE_TRY(de_asset_manifest_load(e->assets, "flappy:/examples/flappy/assets.toml"));
#elif !defined(DE_SHIPPING)
    /* Release has no cooker: it loads flappy.dpak beside the executable, which the flappy_pack
     * target cooks in a Dev build (Publish puts it there). Shipping mounts game:/'s packs itself. */
    if (de_asset_pack_mount(e->assets, "game:/flappy.dpak", 0) != DE_OK) {
        DE_ERROR(DE_LOG_CAT_GENERAL, "flappy.dpak: %s (build flappy_pack in Dev and copy it here)",
                 de_error_last()->message);
        return DE_ERR_NOT_FOUND;
    }
#endif
    de_asset_id font = de_asset_id_from("flappy/font");
    app.font = de_asset_font(e->assets, font);
    app.font_load = de_asset_load(e->assets, font, 0);
    /* The sprites' textures: the scene names them, whoever spawns it. Textures finish over the
     * first frames (their GPU uploads go with frames), so nothing waits for them here. */
    app.art_load = de_asset_load_bundle(e->assets, "flappy_art", 0);
    DE_TRY(wait_ready(e, app.font_load, "font"));
    de_text_update(e->text);

    DE_TRY(de_ui_create(&(de_ui_desc){.renderer = e->renderer,
                                      .text = e->text,
                                      .input = e->input,
                                      .window = e->window,
                                      .max_nodes = 32,
                                      .reference_height = STAGE_H},
                        &e->ui));
    de_ui_attach(e->ui, e->world);
    app.c.node = de_component_find(e->world, "DeUiNode");
    app.c.anchor = de_component_find(e->world, "DeUiAnchor");
    app.c.style = de_component_find(e->world, "DeUiStyle");
    app.c.text = de_component_find(e->world, "DeUiText");
    app.c.transform = de_component_find(e->world, "DeTransform");
    app.c.sprite = de_component_find(e->world, "DeSprite");

    /* The UI is only text over the scene: a clear stage, the score, the message panel. */
    app.stage = node(e, DE_ENTITY_NULL, (de_vec4){0}, 0);
    *(DeUiAnchor *)de_entity_get(e->world, app.stage, app.c.anchor) =
        (DeUiAnchor){.anchor_min = {.5f, .5f},
                     .anchor_max = {.5f, .5f},
                     .offset_min = {-STAGE_W / 2, -STAGE_H / 2},
                     .offset_max = {STAGE_W / 2, STAGE_H / 2}};
    app.score = node(e, app.stage, (de_vec4){0}, 0);
    place(e, app.score, 0, 12, STAGE_W, 28);
    app.panel = node(e, app.stage, (de_vec4){INK.x, INK.y, INK.z, .88f}, 10);
    place(e, app.panel, 40, 210, STAGE_W - 80, 96);
    app.message = node(e, app.panel, (de_vec4){0}, 0);
    *(DeUiAnchor *)de_entity_get(e->world, app.message, app.c.anchor) =
        (DeUiAnchor){.anchor_max = {1, 1}, .offset_min = {8, 14}, .offset_max = {-8, -8}};
    return DE_OK;
}

static void shutdown(de_engine *e) {
    if (app.level_load.id)
        de_asset_release(e->assets, app.level_load);
    if (app.art_load.id)
        de_asset_release(e->assets, app.art_load);
    if (app.font_load.id)
        de_asset_release(e->assets, app.font_load);
}

static de_query query(de_world *w, de_component a, de_access aa, de_component b, de_access ba,
                      de_component c) {
    const de_query_term terms[3] = {{a, aa}, {b, ba}, {c, DE_READ}};
    return de_query_create(w, &(de_query_desc){.terms = terms, .term_count = c.id ? 3u : 2u});
}

static void systems(de_engine *e, de_world *w) {
    app.birds = query(w, app.c.transform, DE_WRITE, app.c.bird, DE_READ, (de_component){0});
    app.pipes = query(w, app.c.transform, DE_WRITE, app.c.sprite, DE_WRITE, app.c.pipe);
    app.clouds = query(w, app.c.transform, DE_WRITE, app.c.cloud, DE_READ, (de_component){0});
    (void)de_system_add(w, &(de_system_desc){.name = "flappy.spawn",
                                             .schedule = DE_SCHEDULE_PRE_UPDATE,
                                             .fn = spawn,
                                             .user = e,
                                             .flags = DE_SYSTEM_MAIN_THREAD | DE_SYSTEM_EXCLUSIVE});
    (void)de_system_add(w, &(de_system_desc){.name = "flappy.control",
                                             .schedule = DE_SCHEDULE_PRE_UPDATE,
                                             .fn = control,
                                             .user = e,
                                             .flags = DE_SYSTEM_MAIN_THREAD | DE_SYSTEM_EXCLUSIVE});
    (void)de_system_add(w, &(de_system_desc){.name = "flappy.simulate",
                                             .schedule = DE_SCHEDULE_FIXED_UPDATE,
                                             .fn = simulate,
                                             .user = e,
                                             .flags = DE_SYSTEM_MAIN_THREAD | DE_SYSTEM_EXCLUSIVE});
    (void)de_system_add(w, &(de_system_desc){.name = "flappy.present",
                                             .schedule = DE_SCHEDULE_UPDATE,
                                             .fn = present,
                                             .user = e,
                                             .flags = DE_SYSTEM_MAIN_THREAD | DE_SYSTEM_EXCLUSIVE});
    static const char *before[] = {"ui.layout"}; /* the UI module's layout system */
    (void)de_system_add(w, &(de_system_desc){.name = "flappy.display",
                                             .schedule = DE_SCHEDULE_POST_UPDATE,
                                             .fn = display,
                                             .user = e,
                                             .before = before,
                                             .before_count = 1,
                                             .flags = DE_SYSTEM_MAIN_THREAD | DE_SYSTEM_EXCLUSIVE});
}

static de_component component(de_engine *e, de_type_registry *registry, const de_type_desc *desc) {
    return de_component_register(e->world, de_type_register(registry, desc),
                                 DE_COMP_SNAPSHOT | DE_COMP_SERIALIZE);
}

static void types(de_engine *e, de_type_registry *registry) {
    static const de_enum_value modes[] = {
        {"ready", READY}, {"playing", PLAYING}, {"dead", DEAD}, {"paused", PAUSED}};
    (void)de_enum_register(registry, "FlappyMode", modes, 4, false);
    static const de_field_info pair_fields[] = {
        DE_FIELD_EX(FlappyPipePair, x, DE_FK_F32, DE_FIELD_DEFAULT, {0, 0, "px", "Left edge", NULL, 0}),
        DE_FIELD_EX(FlappyPipePair, gap, DE_FK_F32, DE_FIELD_DEFAULT,
                    {GAP * .5, FLOOR - GAP * .5, "px", "Centre of the gap", NULL, 0}),
        DE_FIELD(FlappyPipePair, scored, DE_FK_BOOL, DE_FIELD_DEFAULT),
    };
    (void)de_type_register(registry, &DE_TYPE_DESC(FlappyPipePair, 1, pair_fields));
    static const de_field_info state_fields[] = {
        DE_FIELD_EX(FlappyState, mode, DE_FK_ENUM, DE_FIELD_DEFAULT, {0, 0, NULL, NULL, "FlappyMode", 0}),
        DE_FIELD_EX(FlappyState, y, DE_FK_F32, DE_FIELD_DEFAULT, {48 + RADIUS, FLOOR - RADIUS, "px", "Bird height (down)", NULL, 0}),
        DE_FIELD_EX(FlappyState, velocity, DE_FK_F32, DE_FIELD_DEFAULT, {-400, 400, "px/s", NULL, NULL, 0}),
        DE_FIELD_EX(FlappyState, time, DE_FK_F32, DE_FIELD_DEFAULT | DE_FIELD_READONLY, {0, 0, "s", NULL, NULL, 0}),
        DE_FIELD(FlappyState, score, DE_FK_U32, DE_FIELD_DEFAULT),
        DE_FIELD(FlappyState, best, DE_FK_U32, DE_FIELD_DEFAULT),
        DE_FIELD(FlappyState, random, DE_FK_U32, DE_FIELD_DEFAULT | DE_FIELD_EDITOR_HIDDEN),
        DE_FIELD_EX(FlappyState, pipes, DE_FK_FIXED_ARRAY, DE_FIELD_DEFAULT,
                    {0, 0, NULL, NULL, "FlappyPipePair", PIPE_COUNT}),
    };
    app.c.state = component(e, registry, &DE_TYPE_DESC(FlappyState, 1, state_fields));
    static const de_field_info name_fields[] = {DE_FIELD(Name, value, DE_FK_STRING, DE_FIELD_DEFAULT)};
    app.c.name = component(e, registry, &DE_TYPE_DESC(Name, 1, name_fields));
    static const de_field_info bird_fields[] = {
        DE_FIELD_EX(FlappyBird, flap_tilt, DE_FK_F32, DE_FIELD_DEFAULT, {0, 1.5, "rad", "Nose-up tilt while rising", NULL, 0}),
    };
    app.c.bird = component(e, registry, &DE_TYPE_DESC(FlappyBird, 1, bird_fields));
    static const de_field_info pipe_fields[] = {
        DE_FIELD_EX(FlappyPipe, pair, DE_FK_U32, DE_FIELD_DEFAULT, {0, PIPE_COUNT - 1, NULL, "Index in FlappyState.pipes", NULL, 0}),
        DE_FIELD_EX(FlappyPipe, part, DE_FK_U32, DE_FIELD_DEFAULT, {0, 3, NULL, "0 top, 1 top cap, 2 bottom, 3 bottom cap", NULL, 0}),
    };
    app.c.pipe = component(e, registry, &DE_TYPE_DESC(FlappyPipe, 1, pipe_fields));
    static const de_field_info cloud_fields[] = {DE_FIELD(FlappyCloud, index, DE_FK_U32, DE_FIELD_DEFAULT)};
    app.c.cloud = component(e, registry, &DE_TYPE_DESC(FlappyCloud, 1, cloud_fields));

    app.state = de_entity_spawn(e->world);
    game *g = (game *)de_entity_add(e->world, app.state, app.c.state);
    if (g != NULL)
        game_reset(g);
    *(Name *)de_entity_add(e->world, app.state, app.c.name) = (Name){"Game state"};

    /* The level: `flappy.spawn` spawns it once it has loaded. In an editor session DIDE opens
     * flappy.scene instead (DENG-647). */
    if (!de_engine_editor_session(e))
        app.level_load = de_asset_load(e->assets, de_asset_id_from("flappy/level"), 0);
}

DE_GAME_EXPORT const de_game_api *de_game_get_api(void) {
    static const de_game_api api = {.api_version = DE_GAME_API_VERSION,
                                    .name = "Flappy",
                                    .init = init,
                                    .shutdown = shutdown,
                                    .register_systems = systems,
                                    .register_types = types};
    return &api;
}
