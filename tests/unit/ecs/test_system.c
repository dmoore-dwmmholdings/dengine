/* test_system.c: systems: add / remove, enable / disable, the system context, main-thread systems
 * and the Debug/Dev access and determinism checks (DENG-350; SPEC §7.3, §5.7).
 *
 * The checks panic (DE_PANIC). The death tests catch the panic with a panic hook that longjmps
 * out, on a fresh thread each time (a thread that left a panic by longjmp still counts as
 * panicking), on a world without a task system, so the system runs on that thread. */
#include "ecs_test_util.h"

#include "ecs/query_internal.h"
#include "ecs/system_internal.h"
#include "platform/de_atomic.h"

#include <dengine/de_platform.h>

#include <setjmp.h>
#include <stdio.h>

#if defined(DE_DEBUG) || defined(DE_DEV)
#define CHECKS_ON 1
#else
#define CHECKS_ON 0
#endif

static de_task_system *make_ts(u32 workers) {
    de_fp_env_init_thread();
    de_task_system_desc d;
    memset(&d, 0, sizeof d);
    d.worker_count = workers;
    d.background_worker_count = 1;
    de_task_system *ts = NULL;
    ASSERT_EQ(de_task_system_create(&d, &ts), DE_OK);
    return ts;
}

/* Recreates the fixture's world on `ts` (may be NULL). */
static void rebuild_world(ecs_fx *f, de_task_system *ts) {
    ecs_fx_destroy_world(f);
    de_world_desc d;
    memset(&d, 0, sizeof d);
    d.tasks = ts;
    ecs_fx_create_world(f, &d);
}

static void count_fn(de_system_ctx *ctx) {
    de_atomic_fetch_add_u32((de_atomic_u32 *)ctx->user, 1u, DE_ATOMIC_RELAXED);
}

static u32 count_of(de_atomic_u32 *c) {
    return de_atomic_load_u32(c, DE_ATOMIC_RELAXED);
}

static bool error_has(const char *needle) {
    const de_error *e = de_error_last();
    return e != NULL && strstr(e->message, needle) != NULL;
}

typedef struct immediate_add_observer {
    de_entity entity;
    de_component component;
    u32 added, changed;
} immediate_add_observer;
static void immediate_add_once(de_system_ctx *ctx) {
    immediate_add_observer *s = ctx->user;
    if (!de_entity_has(ctx->world, s->entity, s->component))
        ASSERT_TRUE(de_entity_add(ctx->world, s->entity, s->component));
}
static void observe_immediate_add(de_system_ctx *ctx) {
    immediate_add_observer *s = ctx->user;
    s->added = de_query_count(ctx->world, ctx->queries[0]);
    s->changed = de_query_count(ctx->world, ctx->queries[1]);
}
TEST_F(system, immediate_add_visible_to_later_system_in_same_run_only, ecs_fx) {
    immediate_add_observer state = {.component = f->pos};
    state.entity = de_entity_spawn(f->w);
    ASSERT_TRUE(state.entity.bits);
    const char *before[] = {"observe_add"};
    ASSERT_TRUE(de_system_add(f->w, &(de_system_desc){.name = "add_once",
                                                      .schedule = DE_SCHEDULE_UPDATE,
                                                      .fn = immediate_add_once,
                                                      .user = &state,
                                                      .flags = DE_SYSTEM_EXCLUSIVE,
                                                      .before = before,
                                                      .before_count = 1})
                    .id);
    de_query_term term = {f->pos, DE_READ};
    de_query_filter added = {f->pos, DE_ADDED}, changed = {f->pos, DE_CHANGED};
    de_query_desc queries[] = {
        {.terms = &term, .term_count = 1, .filters = &added, .filter_count = 1},
        {.terms = &term, .term_count = 1, .filters = &changed, .filter_count = 1}};
    ASSERT_TRUE(de_system_add(f->w, &(de_system_desc){.name = "observe_add",
                                                      .schedule = DE_SCHEDULE_UPDATE,
                                                      .fn = observe_immediate_add,
                                                      .user = &state,
                                                      .queries = queries,
                                                      .query_count = 2})
                    .id);
    ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_UPDATE, 0, 0), DE_OK);
    EXPECT_EQ(state.added, 1u);
    EXPECT_EQ(state.changed, 1u);
    ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_UPDATE, 0, 0), DE_OK);
    EXPECT_EQ(state.added, 0u);
    EXPECT_EQ(state.changed, 0u);
}

/* ---- system.add_remove ---------------------------------------------------------------------- */

TEST_F(system, add_remove, ecs_fx) {
    de_atomic_u32 calls;
    de_atomic_init_u32(&calls, 0);
    de_system_desc d;
    memset(&d, 0, sizeof d);
    d.name = "a";
    d.schedule = DE_SCHEDULE_UPDATE;
    d.fn = count_fn;
    d.user = &calls;

    /* Refusals. */
    EXPECT_FALSE(DE_HANDLE_VALID(de_system_add(NULL, &d)));
    EXPECT_FALSE(DE_HANDLE_VALID(de_system_add(f->w, NULL)));
    de_system_desc bad = d;
    bad.fn = NULL;
    EXPECT_FALSE(DE_HANDLE_VALID(de_system_add(f->w, &bad)));
    EXPECT_TRUE(error_has("no fn"));
    bad = d;
    bad.schedule = DE_SCHEDULE_COUNT_;
    EXPECT_FALSE(DE_HANDLE_VALID(de_system_add(f->w, &bad)));
    bad = d;
    bad.flags = 1u << 7;
    EXPECT_FALSE(DE_HANDLE_VALID(de_system_add(f->w, &bad)));
    bad = d;
    bad.query_count = 1;
    EXPECT_FALSE(DE_HANDLE_VALID(de_system_add(f->w, &bad)));
    const de_resource_access bad_res = {{7u}, DE_READ};
    bad = d;
    bad.resources = &bad_res;
    bad.resource_count = 1;
    EXPECT_FALSE(DE_HANDLE_VALID(de_system_add(f->w, &bad)));
    const de_event_type bad_ev = {3u};
    bad = d;
    bad.writes_events = &bad_ev;
    bad.writes_event_count = 1;
    EXPECT_FALSE(DE_HANDLE_VALID(de_system_add(f->w, &bad)));
    const char *null_name[1] = {NULL};
    bad = d;
    bad.before = null_name;
    bad.before_count = 1;
    EXPECT_FALSE(DE_HANDLE_VALID(de_system_add(f->w, &bad)));
    const de_query_term bad_term = {{99u}, DE_READ};
    const de_query_desc bad_q = {&bad_term, 1, NULL, 0};
    bad = d;
    bad.queries = &bad_q;
    bad.query_count = 1;
    EXPECT_FALSE(DE_HANDLE_VALID(de_system_add(f->w, &bad)));
    EXPECT_TRUE(error_has("query 0"));

    /* A system with two queries: its access set. */
    const de_query_term t0[] = {
        {f->pos, DE_READ}, {f->vel, DE_WRITE}, {f->health, DE_OPTIONAL_READ}};
    const de_query_filter f0[] = {{f->enemy, DE_WITH}, {f->rot, DE_CHANGED}};
    const de_query_term t1[] = {{f->vel, DE_READ}, {f->pos, DE_READ}};
    const de_query_desc qs[2] = {{t0, 3, f0, 2}, {t1, 2, NULL, 0}};
    d.queries = qs;
    d.query_count = 2;
    const de_system a = de_system_add(f->w, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(a));
    EXPECT_TRUE(de_system_enabled(f->w, a));
    ecs_system *sa = ecs_system_get(f->w, a);
    ASSERT_TRUE(sa != NULL);
    EXPECT_STREQ(sa->name, "a");
    EXPECT_EQ(sa->query_count, 2u);
    EXPECT_TRUE(ecs_query_get(f->w, sa->queries[0]) != NULL);
    EXPECT_TRUE(ecs_query_get(f->w, sa->queries[1]) != NULL);
    /* pos read, vel write (a write wins), health read (optional), rot read (CHANGED); no enemy. */
    ASSERT_EQ(sa->access.component_count, 4u);
    const u32 ids[4] = {f->pos.id, f->vel.id, f->health.id, f->rot.id};
    const u32 wr[4] = {0, 1, 0, 0};
    for (u32 i = 0; i < 4; ++i) {
        EXPECT_EQ(sa->access.components[i].id, ids[i]);
        EXPECT_EQ(sa->access.components[i].write, wr[i]);
    }
    EXPECT_TRUE(ecs_access_find(sa->access.components, 4, f->enemy.id) == NULL);
    const de_query q0 = sa->queries[0];

    /* The name is taken. */
    EXPECT_FALSE(DE_HANDLE_VALID(de_system_add(f->w, &d)));
    EXPECT_EQ(de_error_last()->result, DE_ERR_ALREADY_EXISTS);

    d.queries = NULL;
    d.query_count = 0;
    d.name = "b";
    const de_system b = de_system_add(f->w, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(b));
    d.name = NULL; /* unnamed systems are fine */
    const de_system u = de_system_add(f->w, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(u));
    EXPECT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_UPDATE, 0.0f, 0.0f), DE_OK);
    EXPECT_EQ(count_of(&calls), 3u);
    EXPECT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_FIXED_UPDATE, 0.0f, 0.0f), DE_OK);
    EXPECT_EQ(count_of(&calls), 3u);

    /* Remove: the handle is stale, its queries are gone, it no longer runs. */
    de_system_remove(f->w, a);
    EXPECT_TRUE(ecs_system_get(f->w, a) == NULL);
    EXPECT_FALSE(de_system_enabled(f->w, a));
    EXPECT_TRUE(ecs_query_get(f->w, q0) == NULL);
    de_system_remove(f->w, a); /* stale: ignored */
    de_system_remove(NULL, b);
    EXPECT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_UPDATE, 0.0f, 0.0f), DE_OK);
    EXPECT_EQ(count_of(&calls), 5u);

    /* The name is free again; the slot comes back with a new generation. */
    d.name = "a";
    const de_system a2 = de_system_add(f->w, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(a2));
    EXPECT_NE(a2.id, a.id);
    EXPECT_EQ(a2.id & ECS_SYSTEM_INDEX_MASK, a.id & ECS_SYSTEM_INDEX_MASK);
    EXPECT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_UPDATE, 0.0f, 0.0f), DE_OK);
    EXPECT_EQ(count_of(&calls), 8u);

    EXPECT_EQ(de_world_run_schedule(NULL, DE_SCHEDULE_UPDATE, 0.0f, 0.0f), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_COUNT_, 0.0f, 0.0f), DE_ERR_INVALID_ARGUMENT);
    /* The world frees the rest (the fixture checks for leaks). */
}

/* ---- system.enable_disable ------------------------------------------------------------------ */

typedef struct changed_counter {
    u32 runs;
    u32 rows;
} changed_counter;

static void changed_count_chunk(const de_query_chunk *ch, void *user) {
    ((changed_counter *)user)->rows += ch->count;
}

static void changed_count_fn(de_system_ctx *ctx) {
    changed_counter *c = (changed_counter *)ctx->user;
    ++c->runs;
    c->rows = 0;
    de_query_each(ctx->world, ctx->queries[0], changed_count_chunk, c);
}

TEST_F(system, enable_disable, ecs_fx) {
    de_atomic_u32 calls;
    de_atomic_init_u32(&calls, 0);
    de_system_desc d;
    memset(&d, 0, sizeof d);
    d.name = "counter";
    d.schedule = DE_SCHEDULE_UPDATE;
    d.fn = count_fn;
    d.user = &calls;
    const de_system s = de_system_add(f->w, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(s));

    changed_counter cc = {0, 0};
    const de_query_term t = {f->pos, DE_READ};
    const de_query_filter fl = {f->pos, DE_CHANGED};
    const de_query_desc q = {&t, 1, &fl, 1};
    d.name = "changed";
    d.fn = changed_count_fn;
    d.user = &cc;
    d.queries = &q;
    d.query_count = 1;
    const de_system c = de_system_add(f->w, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(c));

    de_entity e[5];
    for (u32 i = 0; i < 5; ++i) {
        e[i] = de_entity_spawn(f->w);
        ASSERT_TRUE(de_entity_add(f->w, e[i], f->pos) != NULL);
    }
    ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_UPDATE, 0.0f, 0.0f), DE_OK);
    EXPECT_EQ(count_of(&calls), 1u);
    EXPECT_EQ(cc.rows, 5u); /* first run: everything */
    const ecs_schedule_state *sc = &f->w->systems->schedules[DE_SCHEDULE_UPDATE];
    const u64 builds = sc->builds;
    const de_task_graph *graph = sc->graph;

    de_system_set_enabled(f->w, s, false);
    de_system_set_enabled(f->w, c, false);
    EXPECT_FALSE(de_system_enabled(f->w, s));
    de_entity_mark_changed(f->w, e[1], f->pos);
    ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_UPDATE, 0.0f, 0.0f), DE_OK);
    de_entity_mark_changed(f->w, e[3], f->pos);
    ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_UPDATE, 0.0f, 0.0f), DE_OK);
    EXPECT_EQ(count_of(&calls), 1u);
    EXPECT_EQ(cc.runs, 1u);
    EXPECT_EQ(sc->builds, builds); /* disabling does not change the graph */
    EXPECT_TRUE(sc->graph == graph);

    /* Enabled again: the changes made while it was off are reported once. */
    de_system_set_enabled(f->w, s, true);
    de_system_set_enabled(f->w, c, true);
    ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_UPDATE, 0.0f, 0.0f), DE_OK);
    EXPECT_EQ(count_of(&calls), 2u);
    EXPECT_EQ(cc.runs, 2u);
    EXPECT_EQ(cc.rows, 2u);
    ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_UPDATE, 0.0f, 0.0f), DE_OK);
    EXPECT_EQ(cc.rows, 0u);
    EXPECT_EQ(sc->builds, builds);
    de_system_set_enabled(NULL, s, false); /* ignored */
    de_system_set_enabled(f->w, (de_system){12345u}, false);
}

/* ---- system.ctx_fields ---------------------------------------------------------------------- */

typedef struct ctx_probe {
    de_system_ctx seen[4];
    u32 n;
    de_commands *chunk_commands;
    u32 q_counts[2];
} ctx_probe;

static void probe_chunk(const de_query_chunk *ch, void *user) {
    ((ctx_probe *)user)->chunk_commands = ch->commands;
}

static void probe_fn(de_system_ctx *ctx) {
    ctx_probe *p = (ctx_probe *)ctx->user;
    if (p->n < 4)
        p->seen[p->n++] = *ctx;
    p->q_counts[0] = de_query_count(ctx->world, ctx->queries[0]);
    p->q_counts[1] = de_query_count(ctx->world, ctx->queries[1]);
    de_query_each(ctx->world, ctx->queries[0], probe_chunk, p);
}

static void outside_chunk(const de_query_chunk *ch, void *user) {
    *(bool *)user = ch->commands == NULL;
}

TEST_F(system, ctx_fields, ecs_fx) {
    for (u32 i = 0; i < 3; ++i) {
        const de_entity e = de_entity_spawn(f->w);
        de_entity_add(f->w, e, f->pos);
        if (i == 0)
            de_entity_add(f->w, e, f->vel);
    }
    ctx_probe p;
    memset(&p, 0, sizeof p);
    const de_query_term tp = {f->pos, DE_READ};
    const de_query_term tv = {f->vel, DE_READ};
    const de_query_desc qs[2] = {{&tp, 1, NULL, 0}, {&tv, 1, NULL, 0}};
    de_system_desc d;
    memset(&d, 0, sizeof d);
    d.name = "probe";
    d.schedule = DE_SCHEDULE_FIXED_UPDATE;
    d.fn = probe_fn;
    d.user = &p;
    d.queries = qs;
    d.query_count = 2;
    const de_system s = de_system_add(f->w, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(s));
    const ecs_system *is = ecs_system_get(f->w, s);

    EXPECT_EQ(de_world_tick(f->w), 0u);
    for (u32 i = 0; i < 3; ++i)
        ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_FIXED_UPDATE, 0.25f, 0.5f), DE_OK);
    ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_UPDATE, 0.0f, 0.0f), DE_OK);
    EXPECT_EQ(de_world_tick(f->w), 3u); /* only FIXED_UPDATE moves the world tick */
    ASSERT_EQ(p.n, 3u);
    for (u32 i = 0; i < 3; ++i) {
        const de_system_ctx *c = &p.seen[i];
        EXPECT_TRUE(c->world == f->w);
        EXPECT_TRUE(c->commands != NULL);
        EXPECT_TRUE(c->queries == is->queries);
        EXPECT_FLOAT_EQ(c->dt, 0.25f, 0.0);
        EXPECT_FLOAT_EQ(c->alpha, 0.5f, 0.0);
        EXPECT_EQ(c->tick, (u64)i);
        EXPECT_TRUE(c->user == &p);
    }
    /* Queries in declared order. */
    EXPECT_EQ(p.q_counts[0], 3u);
    EXPECT_EQ(p.q_counts[1], 1u);
    /* de_query_each inside a system hands out the system's own buffer. */
    EXPECT_TRUE(p.chunk_commands == p.seen[2].commands);
    /* Outside a system the chunk has no buffer. */
    bool null_outside = false;
    de_query_each(f->w, is->queries[0], outside_chunk, &null_outside);
    EXPECT_TRUE(null_outside);
}

/* ---- system.main_thread_flag ---------------------------------------------------------------- */

typedef struct thread_probe {
    de_atomic_u32 runs;
    de_atomic_u32 off_main; /* runs not on frame worker 0 */
} thread_probe;

static void main_probe_fn(de_system_ctx *ctx) {
    thread_probe *p = (thread_probe *)ctx->user;
    de_atomic_fetch_add_u32(&p->runs, 1u, DE_ATOMIC_RELAXED);
    if (de_task_worker_index() != 0)
        de_atomic_fetch_add_u32(&p->off_main, 1u, DE_ATOMIC_RELAXED);
}

static void busy_fn(de_system_ctx *ctx) {
    (void)ctx;
    const u64 t0 = de_time_now_ns();
    while (de_time_now_ns() - t0 < 20000u) {
    }
}

TEST_F(system, main_thread_flag, ecs_fx) {
    de_task_system *ts = make_ts(4);
    rebuild_world(f, ts);
    thread_probe p;
    de_atomic_init_u32(&p.runs, 0);
    de_atomic_init_u32(&p.off_main, 0);
    de_system_desc d;
    memset(&d, 0, sizeof d);
    d.schedule = DE_SCHEDULE_UPDATE;
    /* Frame systems around it keep the workers busy, so a worker would take it if it could. */
    char names[6][8];
    for (u32 i = 0; i < 6; ++i) {
        snprintf(names[i], sizeof names[i], "busy%u", i);
        d.name = names[i];
        d.fn = busy_fn;
        ASSERT_TRUE(DE_HANDLE_VALID(de_system_add(f->w, &d)));
        if (i == 2) {
            de_system_desc m = d;
            m.name = "main";
            m.fn = main_probe_fn;
            m.user = &p;
            m.flags = DE_SYSTEM_MAIN_THREAD;
            ASSERT_TRUE(DE_HANDLE_VALID(de_system_add(f->w, &m)));
        }
    }
    for (u32 run = 0; run < 200u; ++run)
        ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_UPDATE, 0.0f, 0.0f), DE_OK);
    EXPECT_EQ(de_atomic_load_u32(&p.runs, DE_ATOMIC_RELAXED), 200u);
    EXPECT_EQ(de_atomic_load_u32(&p.off_main, DE_ATOMIC_RELAXED), 0u);

    /* Stats come from the graph node (Debug/Dev). */
    de_task_node_stats st;
    const ecs_system *ms = NULL;
    for (u32 i = 0; i < f->w->systems->slots.count; ++i) {
        const ecs_system *s = DE_ARRAY_AT(&f->w->systems->slots, ecs_system_slot, i).sys;
        if (s != NULL && s->name != NULL && strcmp(s->name, "main") == 0)
            ms = s;
    }
    ASSERT_TRUE(ms != NULL);
    const de_result r = de_system_get_stats(f->w, ms->handle, &st);
    if (CHECKS_ON) {
        ASSERT_EQ(r, DE_OK);
        EXPECT_EQ(st.runs, 200u);
        EXPECT_EQ(st.last_worker, 0u);
    } else {
        EXPECT_EQ(r, DE_ERR_UNSUPPORTED);
    }
    EXPECT_EQ(de_system_get_stats(f->w, (de_system){0}, &st), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_system_get_stats(f->w, ms->handle, NULL), DE_ERR_INVALID_ARGUMENT);

    ecs_fx_destroy_world(f); /* before the task system its graphs run on */
    de_task_system_destroy(ts);
}

/* ---- Death-test harness --------------------------------------------------------------------- */

static jmp_buf g_panic_jmp;
static int g_panic_calls;
static char g_panic_msg[512];

static void longjmp_hook(const de_panic_info *info, void *user) {
    (void)user;
    ++g_panic_calls;
    snprintf(g_panic_msg, sizeof g_panic_msg, "%s", info->message);
    longjmp(g_panic_jmp, 1);
}

typedef struct death_case {
    de_world *w;
    de_schedule schedule;
    bool init_fp_env;
} death_case;

static i32 run_schedule_thread(void *p) {
    death_case *dc = (death_case *)p;
    if (dc->init_fp_env)
        de_fp_env_init_thread();
    if (setjmp(g_panic_jmp) == 0)
        (void)de_world_run_schedule(dc->w, dc->schedule, 0.0f, 0.0f);
    return 0;
}

/* Runs `s` of the fixture's world on a fresh thread; returns true if it panicked. The world is
 * left mid-schedule after a panic: the caller rebuilds it. */
static bool run_expect(ecs_fx *f, de_schedule s, bool init_fp_env) {
    const int before = g_panic_calls;
    g_panic_msg[0] = '\0';
    death_case dc = {f->w, s, init_fp_env};
    de_panic_set_hook(longjmp_hook, NULL);
    de_thread *t = NULL;
    if (de_thread_create(run_schedule_thread, &dc, "ecs-death", DE_THREAD_NORMAL, 0, &t) != DE_OK) {
        de_panic_set_hook(NULL, NULL);
        EXPECT_TRUE(!"de_thread_create failed");
        return false;
    }
    (void)de_thread_join(t);
    de_panic_set_hook(NULL, NULL);
    return g_panic_calls != before;
}

/* ---- system.debug_access_violation ---------------------------------------------------------- */

typedef enum access_op {
    OP_GET_CONST_VEL,  /* reads vel */
    OP_GET_POS,        /* writes pos (mutable get) */
    OP_GET_CONST_POS,  /* reads pos */
    OP_RESOURCE,       /* de_resource_get */
    OP_EVENT_SEND,     /* de_event_send */
    OP_EVENT_READ,     /* de_event_next */
    OP_OUTSIDE_QUERY,  /* iterates a query made outside (WRITE vel) */
    OP_SPAWN_IMMEDIATE /* de_entity_spawn (exclusive systems only) */
} access_op;

typedef struct access_case {
    access_op op;
    de_entity e;
    de_resource_id res;
    de_event_type ev;
    de_query outside;
    bool done;
} access_case;

static void nop_chunk(const de_query_chunk *ch, void *user) {
    (void)ch;
    (void)user;
}

static void access_fn(de_system_ctx *ctx) {
    access_case *c = (access_case *)ctx->user;
    de_world *w = ctx->world;
    const de_component pos = {1u}, vel = {2u};
    switch (c->op) {
    case OP_GET_CONST_VEL:
        (void)de_entity_get_const(w, c->e, vel);
        break;
    case OP_GET_POS:
        (void)de_entity_get(w, c->e, pos);
        break;
    case OP_GET_CONST_POS:
        (void)de_entity_get_const(w, c->e, pos);
        break;
    case OP_RESOURCE:
        (void)de_resource_get(w, c->res);
        break;
    case OP_EVENT_SEND: {
        const THealth h = {1.0f, 2.0f};
        de_event_send(w, c->ev, &h);
        break;
    }
    case OP_EVENT_READ: {
        de_event_reader r = de_event_reader_create(w, c->ev);
        while (de_event_next(w, &r) != NULL) {
        }
        break;
    }
    case OP_OUTSIDE_QUERY:
        de_query_each(w, c->outside, nop_chunk, NULL);
        break;
    case OP_SPAWN_IMMEDIATE:
        (void)de_entity_spawn(w);
        break;
    }
    c->done = true;
}

typedef struct access_setup {
    de_resource_id res;
    de_event_type ev;
    de_entity e;
} access_setup;

/* A fresh world with one entity {pos, vel}, a resource and an event type, and one system "acc"
 * that reads pos (query) and declares `res_count` / `ev_read` / `ev_write` / `flags`. */
static access_setup setup_access(ecs_fx *f, access_case *c, bool declare_res, bool ev_read,
                                 bool ev_write, u32 flags) {
    rebuild_world(f, NULL);
    access_setup s;
    s.e = de_entity_spawn(f->w);
    de_entity_add(f->w, s.e, f->pos);
    de_entity_add(f->w, s.e, f->vel);
    s.res = de_resource_insert(f->w, f->t_health, NULL);
    s.ev = de_event_register(f->w, f->t_health);
    const de_query_term tw = {f->vel, DE_WRITE};
    const de_query_desc outside = {&tw, 1, NULL, 0};
    c->outside = de_query_create(f->w, &outside);
    c->e = s.e;
    c->res = s.res;
    c->ev = s.ev;
    c->done = false;

    const de_query_term tr = {f->pos, DE_READ};
    const de_query_desc q = {&tr, 1, NULL, 0};
    const de_resource_access ra = {s.res, DE_READ};
    de_system_desc d;
    memset(&d, 0, sizeof d);
    d.name = "acc";
    d.schedule = DE_SCHEDULE_UPDATE;
    d.fn = access_fn;
    d.user = c;
    d.flags = flags;
    d.queries = &q;
    d.query_count = 1;
    if (declare_res) {
        d.resources = &ra;
        d.resource_count = 1;
    }
    if (ev_read) {
        d.reads_events = &s.ev;
        d.reads_event_count = 1;
    }
    if (ev_write) {
        d.writes_events = &s.ev;
        d.writes_event_count = 1;
    }
    const de_system h = de_system_add(f->w, &d);
    (void)h;
    return s;
}

static void expect_access_panic(ecs_fx *f, access_op op, bool res, bool evr, bool evw,
                                const char *needle) {
    access_case c;
    memset(&c, 0, sizeof c);
    setup_access(f, &c, res, evr, evw, 0);
    c.op = op;
    EXPECT_TRUE(run_expect(f, DE_SCHEDULE_UPDATE, true));
    EXPECT_FALSE(c.done);
    EXPECT_TRUE(strstr(g_panic_msg, "'acc'") != NULL);
    if (strstr(g_panic_msg, needle) == NULL)
        printf("panic message '%s' lacks '%s'\n", g_panic_msg, needle);
    EXPECT_TRUE(strstr(g_panic_msg, needle) != NULL);
}

static void expect_access_ok(ecs_fx *f, access_op op, bool res, bool evr, bool evw, u32 flags) {
    access_case c;
    memset(&c, 0, sizeof c);
    setup_access(f, &c, res, evr, evw, flags);
    c.op = op;
    EXPECT_FALSE(run_expect(f, DE_SCHEDULE_UPDATE, true));
    EXPECT_TRUE(c.done);
}

TEST_F(system, debug_access_violation, ecs_fx) {
    if (!CHECKS_ON) {
        /* Release / Shipping: no checks, undeclared access just works. */
        expect_access_ok(f, OP_GET_CONST_VEL, false, false, false, 0);
        return;
    }
    /* Declared accesses pass. */
    expect_access_ok(f, OP_GET_CONST_POS, false, false, false, 0);
    expect_access_ok(f, OP_RESOURCE, true, false, false, 0);
    expect_access_ok(f, OP_EVENT_SEND, false, false, true, 0);
    expect_access_ok(f, OP_EVENT_READ, false, true, false, 0);
    expect_access_ok(f, OP_EVENT_READ, false, false, true, 0); /* a writer may read */
    /* Undeclared or under-declared accesses panic, naming the system and what it touched. */
    expect_access_panic(f, OP_GET_CONST_VEL, false, false, false, "TVel");
    expect_access_panic(f, OP_GET_POS, false, false, false, "read-only");
    expect_access_panic(f, OP_RESOURCE, false, false, false, "resource");
    expect_access_panic(f, OP_EVENT_SEND, false, true, false, "read-only");
    expect_access_panic(f, OP_EVENT_READ, false, false, false, "event type");
    expect_access_panic(f, OP_OUTSIDE_QUERY, false, false, false, "TVel");
    /* An exclusive system may touch anything, and make immediate structural changes. */
    expect_access_ok(f, OP_GET_POS, false, false, false, DE_SYSTEM_EXCLUSIVE);
    expect_access_ok(f, OP_OUTSIDE_QUERY, false, false, false, DE_SYSTEM_EXCLUSIVE);
    access_case c;
    memset(&c, 0, sizeof c);
    setup_access(f, &c, false, false, false, DE_SYSTEM_EXCLUSIVE);
    c.op = OP_SPAWN_IMMEDIATE;
    ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_UPDATE, 0.0f, 0.0f), DE_OK);
    EXPECT_TRUE(c.done);
    EXPECT_TRUE(de_entity_alive(f->w, (de_entity){((u64)1u << 32) | 1u}));
    /* A non-exclusive system may not (the structural assert). */
    setup_access(f, &c, false, false, false, 0);
    c.op = OP_SPAWN_IMMEDIATE;
    EXPECT_TRUE(run_expect(f, DE_SCHEDULE_UPDATE, true));
    EXPECT_TRUE(strstr(g_panic_msg, "in_schedule") != NULL);
    rebuild_world(f, NULL); /* the fixture destroys a clean world */
}

/* ---- system.deterministic_fpenv_check ------------------------------------------------------- */

typedef struct det_case {
    bool iterate_map;
    bool done;
} det_case;

static void det_fn(de_system_ctx *ctx) {
    det_case *c = (det_case *)ctx->user;
    if (c->iterate_map) {
        de_hashmap m;
        const de_hashmap_desc hd = {.key_size = sizeof(u32), .value_size = sizeof(u32)};
        if (de_hashmap_init(&m, &hd) == DE_OK) {
            de_hashmap_iter it = de_hashmap_iter_begin(&m);
            while (de_hashmap_iter_next(&it)) {
            }
            de_hashmap_free(&m);
        }
    }
    c->done = true;
}

static void setup_det(ecs_fx *f, det_case *c, u32 flags, bool iterate_map) {
    rebuild_world(f, NULL);
    c->iterate_map = iterate_map;
    c->done = false;
    de_system_desc d;
    memset(&d, 0, sizeof d);
    d.name = "det";
    d.schedule = DE_SCHEDULE_FIXED_UPDATE;
    d.fn = det_fn;
    d.user = c;
    d.flags = flags;
    ASSERT_TRUE(DE_HANDLE_VALID(de_system_add(f->w, &d)));
}

static i32 fp_env_default_thread(void *p) {
    *(bool *)p = de_fp_env_check();
    return 0;
}

TEST_F(system, deterministic_fpenv_check, ecs_fx) {
    det_case c;
    /* Deterministic system on a thread with the deterministic FP environment: fine. */
    setup_det(f, &c, DE_SYSTEM_DETERMINISTIC, false);
    EXPECT_FALSE(run_expect(f, DE_SCHEDULE_FIXED_UPDATE, true));
    EXPECT_TRUE(c.done);
    /* A non-deterministic system may iterate a hash map. */
    setup_det(f, &c, 0, true);
    EXPECT_FALSE(run_expect(f, DE_SCHEDULE_FIXED_UPDATE, true));
    EXPECT_TRUE(c.done);
    if (!CHECKS_ON) {
        setup_det(f, &c, DE_SYSTEM_DETERMINISTIC, true);
        EXPECT_FALSE(run_expect(f, DE_SCHEDULE_FIXED_UPDATE, false));
        return;
    }
    /* A deterministic system iterating a hash map panics (after it ran). */
    setup_det(f, &c, DE_SYSTEM_DETERMINISTIC, true);
    EXPECT_TRUE(run_expect(f, DE_SCHEDULE_FIXED_UPDATE, true));
    EXPECT_TRUE(c.done);
    EXPECT_TRUE(strstr(g_panic_msg, "hash map") != NULL);
    EXPECT_TRUE(strstr(g_panic_msg, "'det'") != NULL);

    /* A thread with the OS default FP environment (no flush-to-zero) fails the check. */
    bool default_ok = true;
    de_thread *t = NULL;
    ASSERT_EQ(
        de_thread_create(fp_env_default_thread, &default_ok, "fpenv", DE_THREAD_NORMAL, 0, &t),
        DE_OK);
    (void)de_thread_join(t);
    if (default_ok) {
        SKIP("new threads already have the deterministic FP environment on this platform");
    }
    setup_det(f, &c, DE_SYSTEM_DETERMINISTIC, false);
    EXPECT_TRUE(run_expect(f, DE_SCHEDULE_FIXED_UPDATE, false));
    EXPECT_FALSE(c.done);
    EXPECT_TRUE(strstr(g_panic_msg, "FP environment") != NULL);
    /* A non-deterministic system does not care. */
    setup_det(f, &c, 0, false);
    EXPECT_FALSE(run_expect(f, DE_SCHEDULE_FIXED_UPDATE, false));
    EXPECT_TRUE(c.done);
}
