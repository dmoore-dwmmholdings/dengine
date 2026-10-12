/* scenario_spawn_move.c: the ECS determinism scenario (DENG-353). See scenario_spawn_move.h.
 *
 * SPEC §5.7: de_dmath only (no <math.h>), random numbers from a de_rng seeded from the run's
 * seed and kept in a snapshot component, no hash-map iteration (except the test-only injection),
 * every iteration in query order. Each de_rng call is its own statement, so the draw order never
 * depends on the compiler's argument evaluation order. Built with DE_DETERMINISTIC_FLAGS. */
#include "scenario_spawn_move.h"

#include <dengine/de_math.h>

#include "core/de_hashmap.h"

#include <string.h>

/* ---- Components (registered in this order: ids 1..6) ------------------------------------- */

typedef struct DPos {
    f32 x, y, z;
} DPos;
typedef struct DVel {
    f32 x, y, z;
} DVel;
typedef struct DLife {
    u32 ticks_left;
    u32 generation;
} DLife;
typedef struct DPhase {
    f32 phase, freq;
} DPhase;
typedef struct DKick {
    f32 impulse;
    u32 count;
} DKick;
/* The spawner's random stream lives in the world (a snapshot component), so a snapshot restore
 * rewinds it with everything else. */
typedef struct DSpawner {
    u64 rng_state, rng_inc;
    u64 spawned;
} DSpawner;

typedef struct spawn_move_state {
    de_component pos, vel, life, phase, kick, spawner;
    de_query wind; /* W DVel, for the tick callback */
} spawn_move_state;

det_inject det_ecs_inject = {DET_INJECT_NONE, 0};

#define TWO_PI   6.2831855f
#define WALL     60.0f
#define GRAVITY  9.8f
#define MAX_GEN  2u
#define CHILD_AT 30u

/* ---- spawn: de_rng-driven spawns through the system buffer -------------------------------- */

static f32 rng_signed(de_rng *r, f32 scale) {
    const f32 u = de_rng_f32(r);
    return (u - 0.5f) * scale;
}

static void spawn_chunk(const de_query_chunk *ch, void *user) {
    de_system_ctx *ctx = (de_system_ctx *)user;
    const spawn_move_state *st = (const spawn_move_state *)ctx->user;
    DSpawner *sp = (DSpawner *)ch->columns[0];
    for (u32 i = 0; i < ch->count; ++i) {
        de_rng rng = {sp[i].rng_state, sp[i].rng_inc};
        const u32 n = de_rng_range(&rng, 0, 12);
        for (u32 k = 0; k < n; ++k) {
            const de_entity e = de_cmd_spawn(ctx->commands);
            DPos *p = (DPos *)de_cmd_add(ctx->commands, e, st->pos);
            DVel *v = (DVel *)de_cmd_add(ctx->commands, e, st->vel);
            DLife *l = (DLife *)de_cmd_add(ctx->commands, e, st->life);
            if (p == NULL || v == NULL || l == NULL)
                return;
            p->x = rng_signed(&rng, 2.0f * WALL);
            p->y = rng_signed(&rng, 2.0f * WALL);
            p->z = de_rng_f32(&rng) * 10.0f;
            v->x = rng_signed(&rng, 20.0f);
            v->y = rng_signed(&rng, 20.0f);
            v->z = rng_signed(&rng, 4.0f);
            l->ticks_left = de_rng_range(&rng, 20, 240);
            l->generation = 0;
            if ((de_rng_u32(&rng) & 1u) != 0) {
                DPhase *ph = (DPhase *)de_cmd_add(ctx->commands, e, st->phase);
                if (ph == NULL)
                    return;
                ph->phase = de_rng_f32(&rng) * TWO_PI;
                ph->freq = 0.5f + de_rng_f32(&rng) * 4.0f;
            }
        }
        sp[i].rng_state = rng.state;
        sp[i].rng_inc = rng.inc;
        sp[i].spawned += n;
    }
}

static void spawn_fn(de_system_ctx *ctx) {
    de_query_each(ctx->world, ctx->queries[0], spawn_chunk, ctx);
}

/* ---- move: par_each integration with walls and a floor ------------------------------------ */

static void move_chunk(const de_query_chunk *ch, void *user) {
    const f32 dt = *(const f32 *)user;
    DPos *p = (DPos *)ch->columns[0];
    DVel *v = (DVel *)ch->columns[1];
    for (u32 i = 0; i < ch->count; ++i) {
        v[i].z -= GRAVITY * dt;
        p[i].x += v[i].x * dt;
        p[i].y += v[i].y * dt;
        p[i].z += v[i].z * dt;
        if (p[i].x > WALL || p[i].x < -WALL) {
            p[i].x = p[i].x > 0.0f ? WALL : -WALL;
            v[i].x = -v[i].x;
        }
        if (p[i].y > WALL || p[i].y < -WALL) {
            p[i].y = p[i].y > 0.0f ? WALL : -WALL;
            v[i].y = -v[i].y;
        }
        if (p[i].z < 0.0f) {
            p[i].z = 0.0f;
            v[i].z = -v[i].z * 0.75f;
        }
    }
}

static void move_fn(de_system_ctx *ctx) {
    const f32 dt = ctx->dt;
    de_query_par_each(ctx->world, ctx->queries[0], 64, move_chunk, (void *)&dt);
}

/* ---- age: par_each countdown; despawns and child spawns from the ranges ------------------- */

static void age_chunk(const de_query_chunk *ch, void *user) {
    const spawn_move_state *st = (const spawn_move_state *)user;
    DLife *l = (DLife *)ch->columns[0];
    for (u32 i = 0; i < ch->count; ++i) {
        if (l[i].ticks_left > 0)
            --l[i].ticks_left;
        if (l[i].ticks_left == 0) {
            de_cmd_despawn(ch->commands, ch->entities[i]);
        } else if (l[i].ticks_left == CHILD_AT && l[i].generation < MAX_GEN) {
            const u32 idx = de_entity_index(ch->entities[i]);
            const de_entity c = de_cmd_spawn(ch->commands);
            DLife *cl = (DLife *)de_cmd_add(ch->commands, c, st->life);
            DPos *cp = (DPos *)de_cmd_add(ch->commands, c, st->pos);
            DVel *cv = (DVel *)de_cmd_add(ch->commands, c, st->vel);
            if (cl == NULL || cp == NULL || cv == NULL)
                return;
            cl->ticks_left = 40u + (idx * 7u) % 90u;
            cl->generation = l[i].generation + 1u;
            cp->x = (f32)(idx % 41u) - 20.0f;
            cp->y = (f32)(idx % 37u) - 18.0f;
            cp->z = 5.0f;
            cv->x = (f32)(idx % 5u) - 2.0f;
            cv->y = (f32)(idx % 3u) - 1.0f;
            cv->z = 3.0f;
        }
    }
}

static void age_fn(de_system_ctx *ctx) {
    de_query_par_each(ctx->world, ctx->queries[0], 48, age_chunk, ctx->user);
}

/* ---- phase: de_dmath oscillators; a kick when the sine changes sign ----------------------- */

typedef struct phase_args {
    const spawn_move_state *st;
    f32 dt;
} phase_args;

static void phase_chunk(const de_query_chunk *ch, void *user) {
    const phase_args *a = (const phase_args *)user;
    DPhase *ph = (DPhase *)ch->columns[0];
    for (u32 i = 0; i < ch->count; ++i) {
        const f32 before = de_dsin(ph[i].phase);
        ph[i].phase = de_dfmod(ph[i].phase + ph[i].freq * a->dt, TWO_PI);
        const f32 after = de_dsin(ph[i].phase);
        if ((before < 0.0f) != (after < 0.0f)) {
            DKick *k = (DKick *)de_cmd_add(ch->commands, ch->entities[i], a->st->kick);
            if (k == NULL)
                return;
            k->impulse = 2.0f * de_dcos(ph[i].phase) + after;
            k->count = (u32)(ch->first_row + i) % 7u;
        }
    }
}

static void phase_fn(de_system_ctx *ctx) {
    const phase_args a = {(const spawn_move_state *)ctx->user, ctx->dt};
    de_query_par_each(ctx->world, ctx->queries[0], 32, phase_chunk, (void *)&a);
}

/* ---- kick: serial; applies and removes kicks ---------------------------------------------- */

static void kick_chunk(const de_query_chunk *ch, void *user) {
    const spawn_move_state *st = (const spawn_move_state *)user;
    DVel *v = (DVel *)ch->columns[0];
    const DKick *k = (const DKick *)ch->columns[1];
    for (u32 i = 0; i < ch->count; ++i) {
        v[i].z += k[i].impulse;
        v[i].x += (f32)k[i].count * 0.125f;
        de_cmd_remove(ch->commands, ch->entities[i], st->kick);
    }
}

static void kick_fn(de_system_ctx *ctx) {
    de_query_each(ctx->world, ctx->queries[0], kick_chunk, ctx->user);
}

/* ---- tick callback: the wind ------------------------------------------------------------- */

typedef struct wind_args {
    u32 rank;
} wind_args;

static void wind_apply(DVel *v, u32 rank) {
    v->x += 0.25f * (f32)(rank % 5u) - 0.5f;
    v->y -= 0.125f * (f32)(rank % 3u);
}

static void wind_chunk(const de_query_chunk *ch, void *user) {
    wind_args *a = (wind_args *)user;
    DVel *v = (DVel *)ch->columns[0];
    for (u32 i = 0; i < ch->count; ++i)
        wind_apply(&v[i], a->rank++);
}

typedef struct collect_args {
    de_hashmap *map;
    bool ok;
} collect_args;

static void collect_chunk(const de_query_chunk *ch, void *user) {
    collect_args *a = (collect_args *)user;
    for (u32 i = 0; i < ch->count && a->ok; ++i) {
        const u64 key = ch->entities[i].bits;
        const u32 value = 0;
        a->ok = de_hashmap_put(a->map, de_hash64(&key, sizeof key, 0), &key, &value) == DE_OK;
    }
}

/* Test-only: the same wind, but ranks in hash-map iteration order (SPEC §5.7 rule 5 broken). */
static void wind_hashmap_order(de_world *w, const spawn_move_state *st) {
    de_hashmap map;
    de_hashmap_desc d;
    memset(&d, 0, sizeof d);
    d.key_size = sizeof(u64);
    d.value_size = sizeof(u32);
    if (de_hashmap_init(&map, &d) != DE_OK)
        return;
    collect_args c = {&map, true};
    de_query_each(w, st->wind, collect_chunk, &c);
    u32 rank = 0;
    for (de_hashmap_iter it = de_hashmap_iter_begin(&map); de_hashmap_iter_next(&it);) {
        de_entity e;
        memcpy(&e.bits, it.key, sizeof e.bits);
        DVel *v = (DVel *)de_entity_get(w, e, st->vel);
        if (v != NULL)
            wind_apply(v, rank++);
    }
    de_hashmap_free(&map);
}

static void spawn_move_tick(de_world *w, u64 tick_index, void *state) {
    const spawn_move_state *st = (const spawn_move_state *)state;
    if (tick_index % DET_ECS_WIND_PERIOD != 0)
        return;
    if (det_ecs_inject.kind == DET_INJECT_HASHMAP_ORDER && tick_index >= det_ecs_inject.from_tick) {
        wind_hashmap_order(w, st);
        return;
    }
    wind_args a = {0};
    de_query_each(w, st->wind, wind_chunk, &a);
}

/* ---- setup ------------------------------------------------------------------------------- */

static de_component register_component(de_world *w, de_type_id t) {
    return de_component_register(w, t, DE_COMP_SNAPSHOT);
}

static de_system add_system(de_world *w, const char *name, de_system_fn fn, void *user,
                            const de_query_desc *q) {
    de_system_desc d;
    memset(&d, 0, sizeof d);
    d.name = name;
    d.schedule = DE_SCHEDULE_FIXED_UPDATE;
    d.flags = DE_SYSTEM_DETERMINISTIC;
    d.fn = fn;
    d.user = user;
    d.queries = q;
    d.query_count = 1;
    return de_system_add(w, &d);
}

static de_result spawn_move_setup(de_world *w, u64 seed, void *state) {
    spawn_move_state *st = (spawn_move_state *)state;
    de_type_registry *types = de_world_types(w);

    static const de_field_info k_pos[] = {
        DE_FIELD(DPos, x, DE_FK_F32, DE_FIELD_DEFAULT),
        DE_FIELD(DPos, y, DE_FK_F32, DE_FIELD_DEFAULT),
        DE_FIELD(DPos, z, DE_FK_F32, DE_FIELD_DEFAULT),
    };
    static const de_field_info k_vel[] = {
        DE_FIELD(DVel, x, DE_FK_F32, DE_FIELD_DEFAULT),
        DE_FIELD(DVel, y, DE_FK_F32, DE_FIELD_DEFAULT),
        DE_FIELD(DVel, z, DE_FK_F32, DE_FIELD_DEFAULT),
    };
    static const de_field_info k_life[] = {
        DE_FIELD(DLife, ticks_left, DE_FK_U32, DE_FIELD_DEFAULT),
        DE_FIELD(DLife, generation, DE_FK_U32, DE_FIELD_DEFAULT),
    };
    static const de_field_info k_phase[] = {
        DE_FIELD(DPhase, phase, DE_FK_F32, DE_FIELD_DEFAULT),
        DE_FIELD(DPhase, freq, DE_FK_F32, DE_FIELD_DEFAULT),
    };
    static const de_field_info k_kick[] = {
        DE_FIELD(DKick, impulse, DE_FK_F32, DE_FIELD_DEFAULT),
        DE_FIELD(DKick, count, DE_FK_U32, DE_FIELD_DEFAULT),
    };
    static const de_field_info k_spawner[] = {
        DE_FIELD(DSpawner, rng_state, DE_FK_U64, DE_FIELD_DEFAULT),
        DE_FIELD(DSpawner, rng_inc, DE_FK_U64, DE_FIELD_DEFAULT),
        DE_FIELD(DSpawner, spawned, DE_FK_U64, DE_FIELD_DEFAULT),
    };
    const de_type_id t_pos = de_type_register(types, &DE_TYPE_DESC(DPos, 1, k_pos));
    const de_type_id t_vel = de_type_register(types, &DE_TYPE_DESC(DVel, 1, k_vel));
    const de_type_id t_life = de_type_register(types, &DE_TYPE_DESC(DLife, 1, k_life));
    const de_type_id t_phase = de_type_register(types, &DE_TYPE_DESC(DPhase, 1, k_phase));
    const de_type_id t_kick = de_type_register(types, &DE_TYPE_DESC(DKick, 1, k_kick));
    const de_type_id t_spawner = de_type_register(types, &DE_TYPE_DESC(DSpawner, 1, k_spawner));

    st->pos = register_component(w, t_pos);
    st->vel = register_component(w, t_vel);
    st->life = register_component(w, t_life);
    st->phase = register_component(w, t_phase);
    st->kick = register_component(w, t_kick);
    st->spawner = register_component(w, t_spawner);
    if (!DE_HANDLE_VALID(st->pos) || !DE_HANDLE_VALID(st->vel) || !DE_HANDLE_VALID(st->life) ||
        !DE_HANDLE_VALID(st->phase) || !DE_HANDLE_VALID(st->kick) || !DE_HANDLE_VALID(st->spawner))
        return DE_FAIL_WRAP(DE_ERR_INVALID_ARGUMENT, "ecs_spawn_move: component registration");

    const de_query_term t_spawn[1] = {{st->spawner, DE_WRITE}};
    const de_query_term t_move[2] = {{st->pos, DE_WRITE}, {st->vel, DE_WRITE}};
    const de_query_term t_age[1] = {{st->life, DE_WRITE}};
    const de_query_term t_phase_terms[1] = {{st->phase, DE_WRITE}};
    const de_query_term t_kick_terms[2] = {{st->vel, DE_WRITE}, {st->kick, DE_READ}};
    const de_query_desc q_spawn = {t_spawn, 1, NULL, 0};
    const de_query_desc q_move = {t_move, 2, NULL, 0};
    const de_query_desc q_age = {t_age, 1, NULL, 0};
    const de_query_desc q_phase = {t_phase_terms, 1, NULL, 0};
    const de_query_desc q_kick = {t_kick_terms, 2, NULL, 0};
    /* spawn, move, phase and age do not conflict and run in parallel; kick writes DVel like
     * move, so it runs after it. Commands apply in run order (spawn, move, phase, kick, age):
     * age is registered last so its despawns apply after the kick adds and removes of the same
     * tick, and no command meets an entity that is already gone. */
    if (!DE_HANDLE_VALID(add_system(w, "det.spawn", spawn_fn, st, &q_spawn)) ||
        !DE_HANDLE_VALID(add_system(w, "det.move", move_fn, st, &q_move)) ||
        !DE_HANDLE_VALID(add_system(w, "det.phase", phase_fn, st, &q_phase)) ||
        !DE_HANDLE_VALID(add_system(w, "det.kick", kick_fn, st, &q_kick)) ||
        !DE_HANDLE_VALID(add_system(w, "det.age", age_fn, st, &q_age)))
        return DE_FAIL_WRAP(DE_ERR_INVALID_ARGUMENT, "ecs_spawn_move: system registration");

    const de_query_term t_wind[1] = {{st->vel, DE_WRITE}};
    const de_query_desc q_wind = {t_wind, 1, NULL, 0};
    st->wind = de_query_create(w, &q_wind);
    if (!DE_HANDLE_VALID(st->wind))
        return DE_FAIL_WRAP(DE_ERR_INVALID_ARGUMENT, "ecs_spawn_move: wind query");

    /* The spawner entity, then 64 start entities in a fixed integer pattern. */
    const de_rng rng = de_rng_seed(seed, 0x353u);
    de_entity spawner;
    de_result r = de_entity_spawn_with(w, &st->spawner, 1, &spawner);
    if (r != DE_OK)
        return r;
    DSpawner *sp = (DSpawner *)de_entity_get(w, spawner, st->spawner);
    sp->rng_state = rng.state;
    sp->rng_inc = rng.inc;
    sp->spawned = 0;

    const de_component start[3] = {st->pos, st->vel, st->life};
    for (u32 i = 0; i < 64u; ++i) {
        de_entity e;
        r = de_entity_spawn_with(w, start, 3, &e);
        if (r != DE_OK)
            return r;
        DPos *p = (DPos *)de_entity_get(w, e, st->pos);
        DVel *v = (DVel *)de_entity_get(w, e, st->vel);
        DLife *l = (DLife *)de_entity_get(w, e, st->life);
        p->x = (f32)(i % 8u) * 10.0f - 35.0f;
        p->y = (f32)(i / 8u) * 10.0f - 35.0f;
        p->z = (f32)(i % 3u);
        v->x = (f32)(i % 7u) - 3.0f;
        v->y = (f32)(i % 5u) - 2.0f;
        v->z = 0.0f;
        l->ticks_left = 100u + i * 5u;
        l->generation = 0;
    }
    return DE_OK;
}

const de_det_scenario det_ecs_spawn_move = {
    "ecs_spawn_move",         /* name */
    spawn_move_setup,         /* setup */
    spawn_move_tick,          /* tick */
    NULL,                     /* teardown: the world owns the query and the systems */
    1000u,                    /* ticks */
    sizeof(spawn_move_state), /* state_size */
    0.0f,                     /* dt: DE_DET_DEFAULT_DT */
    0u,                       /* checksum mask: DE_COMP_SNAPSHOT */
};
