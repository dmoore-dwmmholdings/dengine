/*
 * reload_game_v1.c: the game of the hot reload tests (DENG-93, SPEC §10.1), version 1.
 *
 * One source for every version; reload_game_v2.c sets the macros of version 2 and includes this
 * file. fixtures/CMakeLists.txt builds five libraries with de_add_game():
 *   reload_game_v1   DeTestCounter { i32 value; }, the Update system adds 1 to every value.
 *   reload_game_v2   DeTestCounter { i32 value; f32 scale; }, adds 10 (reload_game_v2.c).
 *   reload_game_v3   version 1 with api_version DE_GAME_API_VERSION + 1 (RELOAD_GAME_BAD_VERSION).
 *   reload_game_v4   version 2 with a `migrate` hook that sets scale to 0.5, adds 100.
 *   reload_game_v5   version 2 whose register_types registers a broken type (an error).
 * Each also has the resource DeTestTotals (v1 { i64 sum; }, v2 adds `u32 frames`), which the
 * system updates, so a resource is re-laid out too.
 *
 * The state (reload_game.h) lives in de_engine.game_state, allocated by `init` from the engine's
 * allocator, so it survives every reload; the first library spawns the entities. The library
 * keeps nothing in static storage that a reload would need. It logs from every callback, so the
 * log thread formats `__FILE__` strings of every library (the host flushes the log before it
 * closes one).
 */
#include "reload_game.h"

#include <stddef.h>
#include <string.h>

#ifndef RELOAD_GAME_VERSION
#define RELOAD_GAME_VERSION 1u
#endif
#ifndef RELOAD_GAME_INCREMENT
#define RELOAD_GAME_INCREMENT 1
#endif
#ifndef RELOAD_GAME_NAME
#if defined(RELOAD_GAME_BAD_VERSION)
#define RELOAD_GAME_NAME "reload_game_v3"
#else
#define RELOAD_GAME_NAME "reload_game_v1"
#endif
#endif

/* ---- Types ---------------------------------------------------------------------------------- */

typedef struct DeTestCounter {
    i32 value;
#if defined(RELOAD_GAME_HAS_SCALE)
    f32 scale;
#endif
} DeTestCounter;

typedef struct DeTestTotals {
    i64 sum;
#if defined(RELOAD_GAME_HAS_SCALE)
    u32 frames;
#endif
} DeTestTotals;

/* Field names, units and tooltips are in this library's read-only data: the registry must copy
 * them (the test reads them after the library is closed). */
static const de_field_info k_counter_fields[] = {
    {.name = "value",
     .offset = offsetof(DeTestCounter, value),
     .size = sizeof(i32),
     .kind = DE_FK_I32,
     .flags = DE_FIELD_DEFAULT,
     .attrs = {.units = "count", .tooltip = "counted by " RELOAD_GAME_NAME}},
#if defined(RELOAD_GAME_HAS_SCALE)
    {.name = "scale",
     .offset = offsetof(DeTestCounter, scale),
     .size = sizeof(f32),
     .kind = DE_FK_F32,
     .flags = DE_FIELD_DEFAULT,
     .attrs = {.tooltip = "added in version 2"}},
#endif
};

static const de_field_info k_totals_fields[] = {
    {.name = "sum",
     .offset = offsetof(DeTestTotals, sum),
     .size = sizeof(i64),
     .kind = DE_FK_I64,
     .flags = DE_FIELD_DEFAULT},
#if defined(RELOAD_GAME_HAS_SCALE)
    {.name = "frames",
     .offset = offsetof(DeTestTotals, frames),
     .size = sizeof(u32),
     .kind = DE_FK_U32,
     .flags = DE_FIELD_DEFAULT},
#endif
};

static const de_enum_value k_mode_values[] = {{"Idle", 0}, {"Counting", 1}};

/* A construct hook, so the component's hooks point into this library. */
static void counter_construct(void *obj) {
    memset(obj, 0, sizeof(DeTestCounter));
}

#if defined(RELOAD_GAME_MIGRATE)
/* Runs after the host copied the fields that match by name and kind. */
static bool counter_migrate(void *obj, u32 from_version, const void *old_data,
                            const de_field_info *old_fields, u32 old_field_count) {
    (void)old_data;
    (void)old_fields;
    (void)old_field_count;
    DeTestCounter *c = (DeTestCounter *)obj;
    if (from_version >= RELOAD_GAME_VERSION)
        return false;
    c->scale = 0.5f;
    return true;
}
#endif

/* ---- State ---------------------------------------------------------------------------------- */

static de_log_category game_log(void) {
    return de_log_register_category("game", DE_LOG_INFO);
}

static reload_game_state *state_of(de_engine *e) {
    reload_game_state *s = (reload_game_state *)e->game_state;
    return (s != NULL && s->magic == RELOAD_GAME_STATE_MAGIC) ? s : NULL;
}

/* ---- Callbacks ------------------------------------------------------------------------------ */

static de_result game_init(de_engine *e) {
    reload_game_state *s = (reload_game_state *)DE_ALLOC(
        e->allocator, sizeof(reload_game_state), DE_ALIGNOF(reload_game_state), DE_MEM_GENERAL);
    if (s == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, RELOAD_GAME_NAME ": no memory for the state");
    memset(s, 0, sizeof *s);
    s->magic = RELOAD_GAME_STATE_MAGIC;
    s->inits = 1;
    e->game_state = s;
    DE_INFO(game_log(), RELOAD_GAME_NAME ": init");
    return DE_OK;
}

static void game_shutdown(de_engine *e) {
    reload_game_state *s = state_of(e);
    if (s == NULL)
        return;
    s->shutdowns++;
    DE_INFO(game_log(), RELOAD_GAME_NAME ": shutdown after %u loads", s->loads);
    /* The test reads the state after shutdown; it frees it (de_engine.game_state). */
}

static void game_register_types(de_engine *e, de_type_registry *types) {
    de_type_desc d;
    memset(&d, 0, sizeof d);
    d.name = "DeTestCounter";
    d.size = sizeof(DeTestCounter);
    d.align = DE_ALIGNOF(DeTestCounter);
    d.version = RELOAD_GAME_VERSION;
    d.fields = k_counter_fields;
    d.field_count = (u32)(sizeof k_counter_fields / sizeof k_counter_fields[0]);
    d.hooks.construct = counter_construct;
#if defined(RELOAD_GAME_MIGRATE)
    d.hooks.migrate = counter_migrate;
#endif
    d.is_pod = true;
    const de_type_id counter = de_type_register(types, &d);

    memset(&d, 0, sizeof d);
    d.name = "DeTestTotals";
    d.size = sizeof(DeTestTotals);
    d.align = DE_ALIGNOF(DeTestTotals);
    d.version = RELOAD_GAME_VERSION;
    d.fields = k_totals_fields;
    d.field_count = (u32)(sizeof k_totals_fields / sizeof k_totals_fields[0]);
    d.is_pod = true;
    (void)de_type_register(types, &d);

    (void)de_enum_register(types, "DeTestMode", k_mode_values, 2u, false);

#if defined(RELOAD_GAME_BAD_TYPES)
    /* A field past the end of its type: de_type_register refuses it and sets the error context,
     * which makes the host refuse the library. */
    static const de_field_info k_bad_fields[] = {
        {.name = "oops", .offset = 8, .size = 4, .kind = DE_FK_I32, .flags = DE_FIELD_DEFAULT}};
    memset(&d, 0, sizeof d);
    d.name = "DeTestBroken";
    d.size = 4;
    d.align = 4;
    d.fields = k_bad_fields;
    d.field_count = 1;
    d.is_pod = true;
    (void)de_type_register(types, &d);
#endif

    /* An existing component keeps its id; the host re-lays it out after this returns. */
    if (DE_HANDLE_VALID(counter))
        (void)de_component_register(e->world, counter, 0);
}

typedef struct count_ctx {
    i64 sum;
} count_ctx;

static void count_chunk(const de_query_chunk *ch, void *user) {
    DeTestCounter *c = (DeTestCounter *)ch->columns[0];
    count_ctx *cc = (count_ctx *)user;
    for (u32 i = 0; i < ch->count; ++i) {
        c[i].value += RELOAD_GAME_INCREMENT;
        cc->sum += RELOAD_GAME_INCREMENT;
    }
}

static void game_count(de_system_ctx *ctx) {
    de_engine *e = (de_engine *)ctx->user;
    count_ctx cc = {0};
    de_query_each(ctx->world, ctx->queries[0], count_chunk, &cc);
    const de_resource_id rid = de_resource_find(ctx->world, de_type_find(e->types, "DeTestTotals"));
    DeTestTotals *t = (DeTestTotals *)de_resource_get(ctx->world, rid);
    if (t != NULL) {
        t->sum += cc.sum;
#if defined(RELOAD_GAME_HAS_SCALE)
        t->frames++;
#endif
    }
}

static void game_register_systems(de_engine *e, de_world *w) {
    reload_game_state *s = state_of(e);
    const de_component counter = de_component_find(w, "DeTestCounter");
    const de_type_id totals = de_type_find(e->types, "DeTestTotals");
    if (s == NULL || !DE_HANDLE_VALID(counter) || !DE_HANDLE_VALID(totals)) {
        DE_ERROR(game_log(), RELOAD_GAME_NAME ": register_systems without state or types");
        return;
    }
    if (!s->spawned) {
        for (u32 i = 0; i < RELOAD_GAME_ENTITIES; ++i) {
            const de_entity ent = de_entity_spawn(w);
            DeTestCounter *c = (DeTestCounter *)de_entity_add(w, ent, counter);
            if (c != NULL)
                c->value = (i32)(i * 100u);
            s->entities[i] = ent;
        }
        (void)de_resource_insert(w, totals, NULL);
        s->spawned = 1;
    }

    const de_query_term term = {counter, DE_WRITE};
    const de_query_desc q = {&term, 1, NULL, 0};
    const de_resource_access res = {de_resource_find(w, totals), DE_WRITE};
    de_system_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.name = RELOAD_GAME_SYSTEM;
    sd.schedule = DE_SCHEDULE_UPDATE;
    sd.fn = game_count;
    sd.user = e;
    sd.queries = &q;
    sd.query_count = 1;
    sd.resources = &res;
    sd.resource_count = 1;
    if (!DE_HANDLE_VALID(de_system_add(w, &sd)))
        DE_ERROR(game_log(), RELOAD_GAME_NAME ": de_system_add failed");
    s->loads++;
    s->version = RELOAD_GAME_VERSION;
    DE_INFO(game_log(), RELOAD_GAME_NAME ": register_systems (load %u)", s->loads);
}

static void game_on_unload(de_engine *e) {
    reload_game_state *s = state_of(e);
    if (s != NULL)
        s->unloads++;
    DE_INFO(game_log(), RELOAD_GAME_NAME ": on_unload");
}

static void game_on_reload(de_engine *e) {
    reload_game_state *s = state_of(e);
    if (s != NULL)
        s->reloads++;
    DE_INFO(game_log(), RELOAD_GAME_NAME ": on_reload");
}

static const de_game_api g_api = {
#if defined(RELOAD_GAME_BAD_VERSION)
    .api_version = DE_GAME_API_VERSION + 1,
#else
    .api_version = DE_GAME_API_VERSION,
#endif
    .name = RELOAD_GAME_NAME,
    .init = game_init,
    .shutdown = game_shutdown,
    .register_types = game_register_types,
    .register_systems = game_register_systems,
    .on_unload = game_on_unload,
    .on_reload = game_on_reload,
};

DE_GAME_EXPORT const de_game_api *de_game_get_api(void);
DE_GAME_EXPORT const de_game_api *de_game_get_api(void) {
    return &g_api;
}
