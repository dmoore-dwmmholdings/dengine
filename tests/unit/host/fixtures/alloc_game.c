/*
 * alloc_game.c: the fixture game of the zero-alloc frame test (DENG-97, SPEC §5.3).
 *
 * It runs the per-frame engine paths a game uses, so test_zero_alloc_frames.c can check that
 * none of them touches the heap once the game is warm:
 *
 *   Startup      (exclusive)    spawns ALLOC_GAME_ENTITIES entities with AgPos, AgVel, AgLife
 *   FixedUpdate  (deterministic) de_query_par_each over AgPos/AgVel, thread scratch per range,
 *                                a filtered Debug log line per range
 *   Update       send            de_event_send of ALLOC_GAME_EVENTS_PER_FRAME AgHit events
 *   Update       receive         reads them back (de_event_next) and damages AgLife
 *   Update       toggle          command buffer: adds the AgMarked tag to ALLOC_GAME_MARKED fixed
 *                                entities on even frames and removes it on odd frames
 *   Update       arena           (main thread) pushes ALLOC_GAME_ARENA_BYTES on the frame arena
 *   PostUpdate   marked          counts the AgMarked rows
 *   Last         frame           (main thread) a filtered Debug line every frame, an Info line
 *                                every ALLOC_GAME_INFO_EVERY frames, and every
 *                                ALLOC_GAME_CVAR_EVERY frames a new value for the int cvar
 *                                `alloc_game.wave` (as the console sets it), so the crash
 *                                bundle's once-per-second cvar snapshot (DENG-96) has a change
 *                                to format
 *
 * Built with ALLOC_GUARD_SELFTEST, the Last system also makes one DE_ALLOC (and DE_FREE) per
 * frame through a tracking allocator with the memory tag ALLOC_GAME_SELFTEST_TAG, which the
 * guard must catch and name.
 *
 * Deterministic: integer math only, events and commands in a fixed order, per-range partial
 * sums reduced in range order. The same fixture can later back a determinism checksum run.
 * Every first-use allocation (component tables, archetype edges, event buffers, command buffer
 * pages, query caches) happens in the first frames, before the test's guarded window.
 */
#include "alloc_game.h"

#include <dengine/de_debug.h>
#include <dengine/de_ecs.h>
#include <dengine/de_memory.h>
#include <dengine/de_reflect.h>

#include <string.h>

/* ---- Types ---------------------------------------------------------------------------------- */

typedef struct AgPos {
    i32 x, y;
} AgPos;

typedef struct AgVel {
    i32 x, y;
} AgVel;

typedef struct AgLife {
    i32 hp;
    u32 hits;
} AgLife;

typedef struct AgMarked {
    i32 unused; /* a tag: registered with DE_COMP_TAG, so it has no column */
} AgMarked;

typedef struct AgHit {
    u32 target; /* index into the marked set */
    i32 amount;
} AgHit;

/* par_each ranges of the FixedUpdate system: ceil(ALLOC_GAME_ENTITIES / ALLOC_GAME_GRAIN). */
#define AG_MAX_RANGES 64u
_Static_assert((ALLOC_GAME_ENTITIES + ALLOC_GAME_GRAIN - 1u) / ALLOC_GAME_GRAIN <= AG_MAX_RANGES,
               "too many par_each ranges");

/* Per-range partial results: each range writes only its own slot, so no atomics are needed, and
 * the reduction in range order is the same for every worker count. */
typedef struct ag_range {
    u64 rows;
    u64 scratch_uses;
    u64 sum;
} ag_range;

typedef struct ag_state {
    de_engine *engine;
    de_log_category log;
    de_component c_pos, c_vel, c_life, c_marked;
    de_event_type ev_hit;
    de_event_reader hit_reader;
    de_entity marked[ALLOC_GAME_MARKED];
    ag_range ranges[AG_MAX_RANGES];
    alloc_game_stats stats;
    de_cvar wave;     /* alloc_game.wave */
    u32 frame_parity; /* frame number of the Update toggle: even adds, odd removes */
#if defined(ALLOC_GUARD_SELFTEST)
    de_tracking_allocator selftest_tracking;
    de_allocator selftest_alloc;
#endif
} ag_state;

static alloc_game_stats g_last_stats;

const alloc_game_stats *alloc_game_last_stats(void) {
    return &g_last_stats;
}

static ag_state *state_of(de_system_ctx *ctx) {
    return (ag_state *)ctx->user;
}

/* ---- Startup: spawn ------------------------------------------------------------------------- */

static void sys_spawn(de_system_ctx *ctx) {
    ag_state *s = state_of(ctx);
    de_world *w = ctx->world;
    const de_component comps[3] = {s->c_pos, s->c_vel, s->c_life};
    for (u32 i = 0; i < ALLOC_GAME_ENTITIES; ++i) {
        de_entity e;
        if (de_entity_spawn_with(w, comps, 3, &e) != DE_OK)
            return; /* the test sees stats.entities short */
        AgPos *p = (AgPos *)de_entity_get(w, e, s->c_pos);
        AgVel *v = (AgVel *)de_entity_get(w, e, s->c_vel);
        AgLife *l = (AgLife *)de_entity_get(w, e, s->c_life);
        p->x = (i32)(i % 100u) * 16;
        p->y = (i32)(i / 100u) * 16;
        v->x = (i32)(i % 7u) - 3;
        v->y = (i32)(i % 5u) - 2;
        l->hp = 1000;
        l->hits = 0;
        if (i < ALLOC_GAME_MARKED)
            s->marked[i] = e;
        s->stats.entities++;
    }
}

/* ---- FixedUpdate: par_each with scratch ----------------------------------------------------- */

static void move_chunk(const de_query_chunk *chunk, void *user) {
    ag_state *s = (ag_state *)user;
    AgPos *pos = (AgPos *)chunk->columns[0];
    const AgVel *vel = (const AgVel *)chunk->columns[1];
    ag_range *r = &s->ranges[chunk->range < AG_MAX_RANGES ? chunk->range : AG_MAX_RANGES - 1u];

    /* Thread scratch: the step is computed into a temporary array first. */
    de_scratch sc = de_scratch_begin(NULL, 0);
    AgPos *next = DE_ARENA_PUSH(sc.arena, AgPos, chunk->count);
    for (u32 i = 0; i < chunk->count; ++i) {
        /* Wrap into [0, 4096) so the values stay bounded over any number of frames. */
        next[i].x = (pos[i].x + vel[i].x) & 4095;
        next[i].y = (pos[i].y + vel[i].y) & 4095;
    }
    u64 sum = 0;
    for (u32 i = 0; i < chunk->count; ++i) {
        pos[i] = next[i];
        sum += (u64)(u32)pos[i].x + (u64)(u32)pos[i].y;
    }
    de_scratch_end(sc);

    r->rows += chunk->count;
    r->scratch_uses++;
    r->sum += sum;
    DE_LOG(DE_LOG_DEBUG, s->log, "move range %u: %u rows", chunk->range, chunk->count);
}

static void sys_move(de_system_ctx *ctx) {
    ag_state *s = state_of(ctx);
    memset(s->ranges, 0, sizeof s->ranges);
    de_query_par_each(ctx->world, ctx->queries[0], ALLOC_GAME_GRAIN, move_chunk, s);
    u64 sum = 0;
    for (u32 k = 0; k < AG_MAX_RANGES; ++k) {
        s->stats.fixed_rows += s->ranges[k].rows;
        s->stats.scratch_uses += s->ranges[k].scratch_uses;
        sum += s->ranges[k].sum;
    }
    s->stats.checksum = sum;
    s->stats.fixed_steps++;
}

/* ---- Update: events ------------------------------------------------------------------------- */

static void sys_send(de_system_ctx *ctx) {
    ag_state *s = state_of(ctx);
    for (u32 i = 0; i < ALLOC_GAME_EVENTS_PER_FRAME; ++i) {
        const AgHit hit = {(u32)((s->stats.events_sent + i) % ALLOC_GAME_MARKED), 1};
        de_event_send(ctx->world, s->ev_hit, &hit);
    }
    s->stats.events_sent += ALLOC_GAME_EVENTS_PER_FRAME;
}

static void sys_receive(de_system_ctx *ctx) {
    ag_state *s = state_of(ctx);
    const AgHit *hit;
    while ((hit = (const AgHit *)de_event_next(ctx->world, &s->hit_reader)) != NULL) {
        AgLife *l = (AgLife *)de_entity_get(ctx->world, s->marked[hit->target], s->c_life);
        if (l != NULL) {
            l->hp -= hit->amount;
            l->hits++;
        }
        s->stats.events_read++;
    }
    s->stats.events_missed = de_event_reader_missed(&s->hit_reader);
}

/* ---- Update: command buffer tag toggle ------------------------------------------------------ */

static void sys_toggle(de_system_ctx *ctx) {
    ag_state *s = state_of(ctx);
    if (ctx->commands == NULL)
        return;
    const bool add = (s->frame_parity++ & 1u) == 0u;
    for (u32 i = 0; i < ALLOC_GAME_MARKED; ++i) {
        if (add)
            (void)de_cmd_add(ctx->commands, s->marked[i], s->c_marked);
        else
            de_cmd_remove(ctx->commands, s->marked[i], s->c_marked);
    }
    s->stats.tag_commands += ALLOC_GAME_MARKED;
}

/* ---- Update: frame arena (main thread: pushing on the frame arena is not thread-safe) ------- */

static void sys_arena(de_system_ctx *ctx) {
    ag_state *s = state_of(ctx);
    de_arena *a = de_frame_arena();
    if (a == NULL)
        return;
    u32 *words = DE_ARENA_PUSH(a, u32, ALLOC_GAME_ARENA_BYTES / sizeof(u32));
    if (words == NULL)
        return;
    for (u32 i = 0; i < ALLOC_GAME_ARENA_BYTES / sizeof(u32); ++i)
        words[i] = i ^ (u32)s->stats.frames;
    s->stats.arena_bytes += ALLOC_GAME_ARENA_BYTES;
}

/* ---- PostUpdate: count the tagged rows ------------------------------------------------------ */

static void sys_marked(de_system_ctx *ctx) {
    ag_state *s = state_of(ctx);
    s->stats.marked_rows += de_query_count(ctx->world, ctx->queries[0]);
}

/* ---- Last: frame counter and logging -------------------------------------------------------- */

static void sys_frame(de_system_ctx *ctx) {
    ag_state *s = state_of(ctx);
    const u64 f = s->stats.frames++;
    DE_LOG(DE_LOG_DEBUG, s->log, "frame %llu", (unsigned long long)f); /* filtered */
    if (f % ALLOC_GAME_INFO_EVERY == 0u) {
        DE_INFO(s->log, "alloc_game: frame %llu, %llu rows moved, %llu hits read",
                (unsigned long long)f, (unsigned long long)s->stats.fixed_rows,
                (unsigned long long)s->stats.events_read);
        s->stats.info_lines++;
    }
    if (f % ALLOC_GAME_CVAR_EVERY == 0u) {
        /* The text a console line would carry; formatted by hand (no C library call). */
        char text[24];
        u64 v = f / ALLOC_GAME_CVAR_EVERY;
        char *p = text + sizeof text;
        *--p = '\0';
        do {
            *--p = (char)('0' + (int)(v % 10u));
            v /= 10u;
        } while (v != 0u);
        if (de_cvar_set(s->wave, p) == DE_OK)
            s->stats.cvar_sets++;
    }
#if defined(ALLOC_GUARD_SELFTEST)
    /* One engine allocation per frame, with its own tag: the guard must catch and name it. */
    void *p = DE_ALLOC(&s->selftest_alloc, 64, 16, 0u);
    if (p != NULL) {
        DE_FREE(&s->selftest_alloc, p, 64);
        s->stats.selftest_allocs++;
    }
#endif
}

/* ---- Game callbacks ------------------------------------------------------------------------- */

static de_result ag_init(de_engine *e) {
    ag_state *s = DE_NEW(e->allocator, ag_state, DE_MEM_GENERAL);
    if (s == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "alloc_game: no memory for the game state");
    memset(s, 0, sizeof *s);
    s->engine = e;
    s->log = de_log_register_category("alloc_game", DE_LOG_INFO);
    const de_cvar_desc wave = {
        .name = "alloc_game.wave",
        .help = "alloc_game fixture: changed every few frames (zero-alloc test)",
        .type = DE_CVAR_INT,
    };
    s->wave = de_cvar_register(&wave);
#if defined(ALLOC_GUARD_SELFTEST)
    const u32 tag = de_mem_register_tag(ALLOC_GAME_SELFTEST_TAG);
    s->selftest_alloc = de_tracking_allocator_init(&s->selftest_tracking, NULL, tag);
#endif
    e->game_state = s;
    memset(&g_last_stats, 0, sizeof g_last_stats);
    return DE_OK;
}

static void ag_shutdown(de_engine *e) {
    ag_state *s = (ag_state *)e->game_state;
    if (s == NULL)
        return;
    g_last_stats = s->stats;
    e->game_state = NULL;
    DE_FREE(e->allocator, s, sizeof *s);
}

static void ag_register_types(de_engine *e, de_type_registry *types) {
    ag_state *s = (ag_state *)e->game_state;
    static const de_field_info pos_fields[] = {
        DE_FIELD(AgPos, x, DE_FK_I32, DE_FIELD_DEFAULT),
        DE_FIELD(AgPos, y, DE_FK_I32, DE_FIELD_DEFAULT),
    };
    static const de_field_info vel_fields[] = {
        DE_FIELD(AgVel, x, DE_FK_I32, DE_FIELD_DEFAULT),
        DE_FIELD(AgVel, y, DE_FK_I32, DE_FIELD_DEFAULT),
    };
    static const de_field_info life_fields[] = {
        DE_FIELD(AgLife, hp, DE_FK_I32, DE_FIELD_DEFAULT),
        DE_FIELD(AgLife, hits, DE_FK_U32, DE_FIELD_DEFAULT),
    };
    static const de_field_info marked_fields[] = {
        DE_FIELD(AgMarked, unused, DE_FK_I32, DE_FIELD_DEFAULT),
    };
    static const de_field_info hit_fields[] = {
        DE_FIELD(AgHit, target, DE_FK_U32, DE_FIELD_DEFAULT),
        DE_FIELD(AgHit, amount, DE_FK_I32, DE_FIELD_DEFAULT),
    };
    de_world *w = e->world;
    s->c_pos = de_component_register(
        w, de_type_register(types, &DE_TYPE_DESC(AgPos, 1, pos_fields)), DE_COMP_SNAPSHOT);
    s->c_vel = de_component_register(
        w, de_type_register(types, &DE_TYPE_DESC(AgVel, 1, vel_fields)), DE_COMP_SNAPSHOT);
    s->c_life = de_component_register(
        w, de_type_register(types, &DE_TYPE_DESC(AgLife, 1, life_fields)), DE_COMP_SNAPSHOT);
    s->c_marked = de_component_register(
        w, de_type_register(types, &DE_TYPE_DESC(AgMarked, 1, marked_fields)), DE_COMP_TAG);
    s->ev_hit = de_event_register(w, de_type_register(types, &DE_TYPE_DESC(AgHit, 1, hit_fields)));
    s->hit_reader = de_event_reader_create(w, s->ev_hit);
}

static void ag_register_systems(de_engine *e, de_world *w) {
    ag_state *s = (ag_state *)e->game_state;

    de_system_desc spawn = {
        .name = "alloc_game.spawn",
        .schedule = DE_SCHEDULE_STARTUP,
        .fn = sys_spawn,
        .user = s,
        .flags = DE_SYSTEM_EXCLUSIVE,
    };
    (void)de_system_add(w, &spawn);

    const de_query_term move_terms[2] = {{s->c_pos, DE_WRITE}, {s->c_vel, DE_READ}};
    const de_query_desc move_q = {.terms = move_terms, .term_count = 2};
    de_system_desc move = {
        .name = "alloc_game.move",
        .schedule = DE_SCHEDULE_FIXED_UPDATE,
        .fn = sys_move,
        .user = s,
        .flags = DE_SYSTEM_DETERMINISTIC,
        .queries = &move_q,
        .query_count = 1,
    };
    (void)de_system_add(w, &move);

    de_system_desc send = {
        .name = "alloc_game.send",
        .schedule = DE_SCHEDULE_UPDATE,
        .fn = sys_send,
        .user = s,
        .writes_events = &s->ev_hit,
        .writes_event_count = 1,
    };
    (void)de_system_add(w, &send);

    const de_query_term life_term = {s->c_life, DE_WRITE};
    const de_query_desc life_q = {.terms = &life_term, .term_count = 1};
    static const char *const after_send[] = {"alloc_game.send"};
    de_system_desc receive = {
        .name = "alloc_game.receive",
        .schedule = DE_SCHEDULE_UPDATE,
        .fn = sys_receive,
        .user = s,
        .queries = &life_q,
        .query_count = 1,
        .reads_events = &s->ev_hit,
        .reads_event_count = 1,
        .after = after_send,
        .after_count = 1,
    };
    (void)de_system_add(w, &receive);

    de_system_desc toggle = {
        .name = "alloc_game.toggle",
        .schedule = DE_SCHEDULE_UPDATE,
        .fn = sys_toggle,
        .user = s,
    };
    (void)de_system_add(w, &toggle);

    de_system_desc arena = {
        .name = "alloc_game.arena",
        .schedule = DE_SCHEDULE_UPDATE,
        .fn = sys_arena,
        .user = s,
        .flags = DE_SYSTEM_MAIN_THREAD,
    };
    (void)de_system_add(w, &arena);

    const de_query_term marked_term = {s->c_marked, DE_READ};
    const de_query_desc marked_q = {.terms = &marked_term, .term_count = 1};
    de_system_desc marked = {
        .name = "alloc_game.marked",
        .schedule = DE_SCHEDULE_POST_UPDATE,
        .fn = sys_marked,
        .user = s,
        .queries = &marked_q,
        .query_count = 1,
    };
    (void)de_system_add(w, &marked);

    de_system_desc frame = {
        .name = "alloc_game.frame",
        .schedule = DE_SCHEDULE_LAST,
        .fn = sys_frame,
        .user = s,
        .flags = DE_SYSTEM_MAIN_THREAD,
    };
    (void)de_system_add(w, &frame);
}

const de_game_api *alloc_game_api(void) {
    static const de_game_api api = {
        .api_version = DE_GAME_API_VERSION,
        .name = "alloc_game",
        .init = ag_init,
        .shutdown = ag_shutdown,
        .register_types = ag_register_types,
        .register_systems = ag_register_systems,
    };
    return &api;
}
