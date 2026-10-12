/* test_change.c: change detection: added / changed ticks per row, DE_ADDED / DE_CHANGED filters,
 * write marking and tick-wrap clamping (DENG-347; SPEC §7.1, §7.2, §5.7).
 *
 * The tests drive the change window through the internal API, the way DENG-350's schedules will:
 * a simulated system takes this_run = ecs_change_tick_advance(), sets its query's window
 * (last_run, this_run] with ecs_query_set_change_window() and iterates. Its last_run is the
 * previous this_run (kept in the query's window, so ecs_check_change_ticks clamps it), or
 * this_run - ECS_MAX_CHANGE_AGE before its first run.
 *
 * CHANGE_WRAP_TRACE is the test vector of change.wrap_equivalence: an integer-only hash of what
 * every simulated system run saw. It must hold on every machine and for every start tick; a change
 * is a determinism bug, never a reason to update it. */
#include "ecs_test_util.h"

#include "ecs/change_internal.h"
#include "ecs/query_internal.h"
#include "platform/de_atomic.h"

#include <stdio.h>

/** Hash of every run's results in change.wrap_equivalence (see the file comment). */
#define CHANGE_WRAP_TRACE 0x61E1F53782987ECAull

#define SEEN_MAX 1024u

/* ---- Simulated systems ------------------------------------------------------------------------
 */

typedef struct seen_set {
    u32 n;
    bool overflow;
    de_entity e[SEEN_MAX];
} seen_set;

static void seen_fn(const de_query_chunk *ch, void *user) {
    seen_set *s = (seen_set *)user;
    for (u32 i = 0; i < ch->count; ++i) {
        if (s->n == SEEN_MAX) {
            s->overflow = true;
            return;
        }
        s->e[s->n++] = ch->entities[i];
    }
}

static bool seen_has(const seen_set *s, de_entity e) {
    for (u32 i = 0; i < s->n; ++i)
        if (s->e[i].bits == e.bits)
            return true;
    return false;
}

typedef struct sim_system {
    de_query q;
    bool ran;
    u32 this_run; /* tick of the last run */
} sim_system;

static sim_system sys_make(de_world *w, const de_query_term *t, u32 nt, const de_query_filter *fl,
                           u32 nf) {
    const de_query_desc d = {t, nt, fl, nf};
    sim_system s;
    memset(&s, 0, sizeof s);
    s.q = de_query_create(w, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(s.q));
    return s;
}

/* One system run: take a tick, set the window, iterate (serially, or par_each with `grain`). */
static void sys_run_with(de_world *w, sim_system *s, de_chunk_fn fn, void *user, u32 grain,
                         bool par) {
    const u32 this_run = ecs_change_tick_advance(w);
    /* last_run: the previous this_run as the query's window holds it (clamped by
     * ecs_check_change_ticks), or "everything" before the first run. */
    const ecs_query *iq = ecs_query_get(w, s->q);
    ASSERT_TRUE(iq != NULL);
    const u32 last_run = s->ran ? iq->this_run_tick : this_run - ECS_MAX_CHANGE_AGE;
    ecs_query_set_change_window(w, s->q, last_run, this_run);
    if (par)
        de_query_par_each(w, s->q, grain, fn, user);
    else
        de_query_each(w, s->q, fn, user);
    s->ran = true;
    s->this_run = this_run;
}

/* A serial run that collects the entities it sees into `out`. */
static u32 sys_run(de_world *w, sim_system *s, seen_set *out) {
    out->n = 0;
    out->overflow = false;
    sys_run_with(w, s, seen_fn, out, 0, false);
    EXPECT_FALSE(out->overflow);
    return out->n;
}

/* The ticks of `comp` in `e`'s row. */
static void row_ticks(de_world *w, de_entity e, de_component comp, u32 *added, u32 *changed) {
    const ecs_record *rec = ecs_entity_record(w, e);
    ASSERT_TRUE(rec != NULL);
    const ecs_archetype *a = ecs_archetype_at(w, rec->archetype);
    const u32 col = ecs_archetype_column(a, comp.id);
    ASSERT_TRUE(col != ECS_INVALID_INDEX);
    const ecs_chunk *ch = ecs_table_chunk(a, rec->chunk);
    *added = ecs_chunk_added(a, ch, col)[rec->row];
    *changed = ecs_chunk_changed(a, ch, col)[rec->row];
}

static de_entity spawn(ecs_fx *f, const de_component *c, u32 n) {
    de_entity e = DE_ENTITY_NULL;
    EXPECT_EQ(de_entity_spawn_with(f->w, c, n, &e), DE_OK);
    return e;
}

static seen_set g_seen;

/* ---- change.added_once ------------------------------------------------------------------------
 */

TEST_F(change, added_once, ecs_fx) {
    const de_query_term t[] = {{f->pos, DE_READ}};
    const de_query_filter added[] = {{f->pos, DE_ADDED}};

    /* Without a window (outside a system) a change filter passes every row. */
    const de_entity e0 = spawn(f, &f->pos, 1);
    {
        const de_query_desc d = {t, 1, added, 1};
        const de_query q = de_query_create(f->w, &d);
        g_seen.n = 0;
        de_query_each(f->w, q, seen_fn, &g_seen);
        EXPECT_EQ(g_seen.n, 1u);
        de_query_each(f->w, q, seen_fn, &g_seen);
        EXPECT_EQ(g_seen.n, 2u);
        de_query_destroy(f->w, q);
    }

    sim_system s = sys_make(f->w, t, 1, added, 1);
    /* First run: everything added so far. Then only what was added since. */
    const de_entity e1 = spawn(f, &f->pos, 1);
    EXPECT_EQ(sys_run(f->w, &s, &g_seen), 2u);
    EXPECT_TRUE(seen_has(&g_seen, e0) && seen_has(&g_seen, e1));
    EXPECT_EQ(sys_run(f->w, &s, &g_seen), 0u);

    /* spawn + add, the same through spawn_with; seen once. */
    const de_entity e2 = de_entity_spawn(f->w);
    ASSERT_TRUE(de_entity_add(f->w, e2, f->pos) != NULL);
    EXPECT_EQ(sys_run(f->w, &s, &g_seen), 1u);
    EXPECT_TRUE(seen_has(&g_seen, e2));
    EXPECT_EQ(sys_run(f->w, &s, &g_seen), 0u);
    EXPECT_EQ(sys_run(f->w, &s, &g_seen), 0u);

    /* Moves keep the ticks: adding another component is not a new pos. Neither is a write, a
     * mark or an add of the component the entity already has. */
    ASSERT_TRUE(de_entity_add(f->w, e1, f->vel) != NULL);
    ASSERT_TRUE(de_entity_add(f->w, e1, f->enemy) != NULL);
    ((TPos *)de_entity_get(f->w, e0, f->pos))->v.x = 3.0f;
    de_entity_mark_changed(f->w, e1, f->pos);
    ASSERT_TRUE(de_entity_add(f->w, e2, f->pos) != NULL);
    EXPECT_EQ(sys_run(f->w, &s, &g_seen), 0u);

    /* Remove and add again: a new add. */
    de_entity_remove(f->w, e2, f->pos);
    ASSERT_TRUE(de_entity_add(f->w, e2, f->pos) != NULL);
    EXPECT_EQ(sys_run(f->w, &s, &g_seen), 1u);
    EXPECT_TRUE(seen_has(&g_seen, e2));

    /* Deferred: spawn + add through commands, seen once after the apply. */
    de_commands *c = de_world_commands(f->w);
    const de_entity pending = de_cmd_spawn(c);
    TPos *payload = (TPos *)de_cmd_add(c, pending, f->pos);
    ASSERT_TRUE(payload != NULL);
    payload->v.x = 7.0f;
    EXPECT_EQ(sys_run(f->w, &s, &g_seen), 0u); /* not applied yet */
    de_world_apply_commands(f->w);
    const de_entity e3 = de_commands_resolve(f->w, pending);
    ASSERT_TRUE(de_entity_alive(f->w, e3));
    EXPECT_EQ(sys_run(f->w, &s, &g_seen), 1u);
    EXPECT_TRUE(seen_has(&g_seen, e3));
    EXPECT_EQ(sys_run(f->w, &s, &g_seen), 0u);

    /* Added ticks are the change tick of the add: added == changed on a new column. */
    u32 ad = 0, chg = 0;
    row_ticks(f->w, e3, f->pos, &ad, &chg);
    EXPECT_EQ(ad, chg);

    /* A system created later sees every existing row on its first run, then nothing. */
    sim_system late = sys_make(f->w, t, 1, added, 1);
    EXPECT_EQ(sys_run(f->w, &late, &g_seen), 4u);
    EXPECT_EQ(sys_run(f->w, &late, &g_seen), 0u);

    /* Each system has its own window: an add between two systems' runs is seen by both. */
    const de_entity e4 = spawn(f, &f->pos, 1);
    EXPECT_EQ(sys_run(f->w, &s, &g_seen), 1u);
    EXPECT_TRUE(seen_has(&g_seen, e4));
    EXPECT_EQ(sys_run(f->w, &late, &g_seen), 1u);
    EXPECT_TRUE(seen_has(&g_seen, e4));
}

/* ---- change.changed_by_mark -------------------------------------------------------------------
 */

TEST_F(change, changed_by_mark, ecs_fx) {
    const de_query_term t[] = {{f->pos, DE_READ}};
    const de_query_filter changed[] = {{f->pos, DE_CHANGED}};
    const de_component pv[] = {f->pos, f->vel};
    const de_entity e1 = spawn(f, &f->pos, 1);
    const de_entity e2 = spawn(f, &f->pos, 1);
    const de_entity e3 = spawn(f, pv, 2);
    const de_entity e4 = spawn(f, &f->vel, 1);
    const de_component pe[] = {f->pos, f->enemy};
    const de_entity e5 = spawn(f, pe, 2);

    sim_system s1 = sys_make(f->w, t, 1, changed, 1);
    sim_system s2 = sys_make(f->w, t, 1, changed, 1);
    /* An add counts as a change. */
    EXPECT_EQ(sys_run(f->w, &s1, &g_seen), 4u);
    EXPECT_FALSE(seen_has(&g_seen, e4));
    EXPECT_EQ(sys_run(f->w, &s2, &g_seen), 4u);
    EXPECT_EQ(sys_run(f->w, &s1, &g_seen), 0u);
    EXPECT_EQ(sys_run(f->w, &s2, &g_seen), 0u);

    de_entity_mark_changed(f->w, e2, f->pos);
    EXPECT_EQ(sys_run(f->w, &s1, &g_seen), 1u);
    EXPECT_TRUE(seen_has(&g_seen, e2));
    EXPECT_EQ(sys_run(f->w, &s1, &g_seen), 0u);

    /* s2 has not run since: it sees e2 and the later e3 once; s1 sees only e3. */
    de_entity_mark_changed(f->w, e3, f->pos);
    EXPECT_EQ(sys_run(f->w, &s2, &g_seen), 2u);
    EXPECT_TRUE(seen_has(&g_seen, e2) && seen_has(&g_seen, e3));
    EXPECT_EQ(sys_run(f->w, &s1, &g_seen), 1u);
    EXPECT_TRUE(seen_has(&g_seen, e3));

    /* Marking a component the entity lacks, a tag, a dead or null entity, or with no world does
     * nothing. */
    de_entity_mark_changed(f->w, e4, f->pos);
    de_entity_mark_changed(f->w, e5, f->enemy);
    de_entity_mark_changed(f->w, DE_ENTITY_NULL, f->pos);
    de_entity_mark_changed(NULL, e1, f->pos);
    const de_entity dead = spawn(f, &f->pos, 1);
    de_entity_despawn(f->w, dead);
    de_entity_mark_changed(f->w, dead, f->pos);
    EXPECT_EQ(sys_run(f->w, &s1, &g_seen), 0u);

    /* The marked tick is the current change tick; added is untouched. */
    u32 ad0 = 0, ch0 = 0, ad1 = 0, ch1 = 0;
    row_ticks(f->w, e1, f->pos, &ad0, &ch0);
    de_entity_mark_changed(f->w, e1, f->pos);
    row_ticks(f->w, e1, f->pos, &ad1, &ch1);
    EXPECT_EQ(ad1, ad0);
    EXPECT_EQ(ch1, ecs_change_tick(f->w));
    EXPECT_EQ(sys_run(f->w, &s1, &g_seen), 1u);

    /* Several change filters must all pass (AND). */
    const de_query_term tv[] = {{f->pos, DE_READ}, {f->vel, DE_READ}};
    const de_query_filter both[] = {{f->pos, DE_CHANGED}, {f->vel, DE_CHANGED}};
    sim_system sb = sys_make(f->w, tv, 2, both, 2);
    EXPECT_EQ(sys_run(f->w, &sb, &g_seen), 1u); /* first run: e3 */
    de_entity_mark_changed(f->w, e3, f->pos);
    EXPECT_EQ(sys_run(f->w, &sb, &g_seen), 0u);
    de_entity_mark_changed(f->w, e3, f->pos);
    de_entity_mark_changed(f->w, e3, f->vel);
    EXPECT_EQ(sys_run(f->w, &sb, &g_seen), 1u);
    const de_query_filter mixed[] = {{f->pos, DE_ADDED}, {f->vel, DE_CHANGED}};
    sim_system sm = sys_make(f->w, tv, 2, mixed, 2);
    EXPECT_EQ(sys_run(f->w, &sm, &g_seen), 1u);
    de_entity_mark_changed(f->w, e3, f->vel); /* changed, but pos was not added since */
    EXPECT_EQ(sys_run(f->w, &sm, &g_seen), 0u);
    de_entity_remove(f->w, e3, f->pos);
    ASSERT_TRUE(de_entity_add(f->w, e3, f->pos) != NULL);
    de_entity_mark_changed(f->w, e3, f->vel);
    EXPECT_EQ(sys_run(f->w, &sm, &g_seen), 1u);

    /* An immediate add of a component the entity already has hands out the value for writing:
     * changed, not added. */
    const de_query_filter added[] = {{f->pos, DE_ADDED}};
    sim_system sa = sys_make(f->w, t, 1, added, 1);
    (void)sys_run(f->w, &sa, &g_seen);
    (void)sys_run(f->w, &s1, &g_seen);
    ASSERT_TRUE(de_entity_add(f->w, e1, f->pos) != NULL);
    EXPECT_EQ(sys_run(f->w, &s1, &g_seen), 1u);
    EXPECT_TRUE(seen_has(&g_seen, e1));
    EXPECT_EQ(sys_run(f->w, &sa, &g_seen), 0u);
}

/* ---- change.changed_by_mut_get ----------------------------------------------------------------
 */

TEST_F(change, changed_by_mut_get, ecs_fx) {
    const de_query_term t[] = {{f->pos, DE_READ}};
    const de_query_filter changed[] = {{f->pos, DE_CHANGED}};
    const de_query_filter added[] = {{f->pos, DE_ADDED}};
    const de_component pve[] = {f->pos, f->vel, f->enemy};
    const de_entity e1 = spawn(f, pve, 3);
    const de_entity e2 = spawn(f, pve, 3);
    sim_system sc = sys_make(f->w, t, 1, changed, 1);
    sim_system sa = sys_make(f->w, t, 1, added, 1);
    EXPECT_EQ(sys_run(f->w, &sc, &g_seen), 2u);
    EXPECT_EQ(sys_run(f->w, &sa, &g_seen), 2u);

    TPos *p = (TPos *)de_entity_get(f->w, e2, f->pos);
    ASSERT_TRUE(p != NULL);
    p->v.y = 5.0f;
    EXPECT_EQ(sys_run(f->w, &sc, &g_seen), 1u);
    EXPECT_TRUE(seen_has(&g_seen, e2));
    EXPECT_EQ(sys_run(f->w, &sa, &g_seen), 0u); /* changed, not added */
    EXPECT_EQ(sys_run(f->w, &sc, &g_seen), 0u);

    /* A mutable get of another component, a tag, an absent component or a dead entity does not
     * mark pos. */
    EXPECT_TRUE(de_entity_get(f->w, e1, f->vel) != NULL);
    EXPECT_TRUE(de_entity_get(f->w, e1, f->enemy) != NULL);
    EXPECT_TRUE(de_entity_get(f->w, e1, f->health) == NULL);
    EXPECT_TRUE(de_entity_get(f->w, DE_ENTITY_NULL, f->pos) == NULL);
    EXPECT_EQ(sys_run(f->w, &sc, &g_seen), 0u);

    /* The vel get did mark vel. */
    const de_query_term tv[] = {{f->vel, DE_READ}};
    const de_query_filter vchanged[] = {{f->vel, DE_CHANGED}};
    sim_system sv = sys_make(f->w, tv, 1, vchanged, 1);
    EXPECT_EQ(sys_run(f->w, &sv, &g_seen), 2u);
    EXPECT_EQ(sys_run(f->w, &sv, &g_seen), 0u);
    (void)de_entity_get(f->w, e1, f->vel);
    EXPECT_EQ(sys_run(f->w, &sv, &g_seen), 1u);
    EXPECT_TRUE(seen_has(&g_seen, e1));
}

/* ---- change.const_get_no_change ---------------------------------------------------------------
 */

static void count_fn(const de_query_chunk *ch, void *user) {
    *(u32 *)user += ch->count;
}

TEST_F(change, const_get_no_change, ecs_fx) {
    const de_query_term t[] = {{f->pos, DE_READ}};
    const de_query_filter changed[] = {{f->pos, DE_CHANGED}};
    const de_component pv[] = {f->pos, f->vel};
    de_entity e[64];
    for (u32 i = 0; i < 64; ++i)
        e[i] = spawn(f, pv, (i % 2u) + 1u);
    sim_system sc = sys_make(f->w, t, 1, changed, 1);
    EXPECT_EQ(sys_run(f->w, &sc, &g_seen), 64u);
    u32 before_ad[64], before_ch[64];
    for (u32 i = 0; i < 64; ++i)
        row_ticks(f->w, e[i], f->pos, &before_ad[i], &before_ch[i]);

    /* Read-only access of every kind. */
    for (u32 i = 0; i < 64; ++i) {
        EXPECT_TRUE(de_entity_get_const(f->w, e[i], f->pos) != NULL);
        EXPECT_TRUE(de_entity_has(f->w, e[i], f->pos));
    }
    const de_query_term reads[] = {{f->pos, DE_READ}, {f->vel, DE_OPTIONAL_READ}};
    sim_system r = sys_make(f->w, reads, 2, NULL, 0);
    u32 n = 0;
    sys_run_with(f->w, &r, count_fn, &n, 0, false);
    sys_run_with(f->w, &r, count_fn, &n, 7, true);
    EXPECT_EQ(n, 128u);
    EXPECT_EQ(de_query_count(f->w, r.q), 64u);
    /* Filtered iterations read ticks only. */
    EXPECT_EQ(sys_run(f->w, &sc, &g_seen), 0u);
    EXPECT_EQ(sys_run(f->w, &sc, &g_seen), 0u);

    for (u32 i = 0; i < 64; ++i) {
        u32 ad = 0, ch = 0;
        row_ticks(f->w, e[i], f->pos, &ad, &ch);
        EXPECT_EQ(ad, before_ad[i]);
        EXPECT_EQ(ch, before_ch[i]);
    }
}

/* ---- change.write_iter_marks ------------------------------------------------------------------
 */

#define PAR_N 20000u

static de_entity g_par_e[PAR_N];

typedef struct par_count {
    de_atomic_u32 rows;
} par_count;

/* Writes TPos.x of its own rows (a par_each callback). */
static void par_write_fn(const de_query_chunk *ch, void *user) {
    par_count *c = (par_count *)user;
    TPos *p = (TPos *)ch->columns[0];
    for (u32 i = 0; i < ch->count; ++i)
        p[i].v.x += 1.0f;
    de_atomic_fetch_add_u32(&c->rows, ch->count, DE_ATOMIC_RELAXED);
}

static void par_count_fn(const de_query_chunk *ch, void *user) {
    de_atomic_fetch_add_u32(&((par_count *)user)->rows, ch->count, DE_ATOMIC_RELAXED);
}

static u64 fnv(u64 h, u64 v) {
    for (u32 i = 0; i < 8; ++i) {
        h ^= (v >> (8u * i)) & 0xFFu;
        h *= 0x100000001B3ull;
    }
    return h;
}

TEST_F(change, write_iter_marks, ecs_fx) {
    /* Serial: DE_WRITE and DE_OPTIONAL_WRITE data columns of the yielded rows are marked with the
     * system's tick; read terms, absent optionals, tags and rows not yielded are not. */
    const de_component p_[] = {f->pos};
    const de_component pv[] = {f->pos, f->vel};
    const de_component v_[] = {f->vel};
    const de_component pvhe[] = {f->pos, f->vel, f->health, f->enemy};
    de_entity e[8];
    e[0] = spawn(f, p_, 1);
    e[1] = spawn(f, pv, 2);
    e[2] = spawn(f, v_, 1);
    e[3] = spawn(f, pvhe, 4);
    e[4] = spawn(f, p_, 1);
    e[5] = spawn(f, pv, 2);
    e[6] = spawn(f, v_, 1);
    e[7] = spawn(f, pvhe, 4);

    const de_query_term tp[] = {{f->pos, DE_READ}};
    const de_query_term tv[] = {{f->vel, DE_READ}};
    const de_query_term th[] = {{f->health, DE_READ}};
    const de_query_filter cp[] = {{f->pos, DE_CHANGED}};
    const de_query_filter cv[] = {{f->vel, DE_CHANGED}};
    const de_query_filter chh[] = {{f->health, DE_CHANGED}};
    sim_system sp = sys_make(f->w, tp, 1, cp, 1);
    sim_system sv = sys_make(f->w, tv, 1, cv, 1);
    sim_system sh = sys_make(f->w, th, 1, chh, 1);
    EXPECT_EQ(sys_run(f->w, &sp, &g_seen), 6u);
    EXPECT_EQ(sys_run(f->w, &sv, &g_seen), 6u);
    EXPECT_EQ(sys_run(f->w, &sh, &g_seen), 2u);

    const de_query_term tw[] = {{f->pos, DE_WRITE},
                                {f->vel, DE_OPTIONAL_WRITE},
                                {f->health, DE_OPTIONAL_READ},
                                {f->enemy, DE_OPTIONAL_WRITE}};
    sim_system w = sys_make(f->w, tw, 4, NULL, 0);
    u32 n = 0;
    sys_run_with(f->w, &w, count_fn, &n, 0, false);
    EXPECT_EQ(n, 6u);
    for (u32 i = 0; i < 8; ++i) {
        u32 ad = 0, ch = 0;
        if (de_entity_has(f->w, e[i], f->pos)) {
            row_ticks(f->w, e[i], f->pos, &ad, &ch);
            EXPECT_EQ(ch, w.this_run);
        }
    }
    EXPECT_EQ(sys_run(f->w, &sp, &g_seen), 6u);
    EXPECT_EQ(sys_run(f->w, &sv, &g_seen), 4u); /* e[2], e[6] (vel only) were not yielded */
    EXPECT_FALSE(seen_has(&g_seen, e[2]) || seen_has(&g_seen, e[6]));
    EXPECT_EQ(sys_run(f->w, &sh, &g_seen), 0u); /* read term */
    /* The writer does not see its own writes as changes on its next run. */
    const de_query_term tw2[] = {{f->pos, DE_WRITE}};
    sim_system w2 = sys_make(f->w, tw2, 1, cp, 1);
    EXPECT_EQ(sys_run(f->w, &w2, &g_seen), 6u);
    EXPECT_EQ(sys_run(f->w, &w2, &g_seen), 0u);
    EXPECT_EQ(sys_run(f->w, &w2, &g_seen), 0u);

    /* Only yielded rows: a filtered writer marks vel of the rows whose pos changed. */
    const de_query_term wv[] = {{f->vel, DE_WRITE}};
    sim_system fw = sys_make(f->w, wv, 1, cp, 1);
    (void)sys_run(f->w, &fw, &g_seen);
    (void)sys_run(f->w, &sp, &g_seen);
    (void)sys_run(f->w, &sv, &g_seen);
    de_entity_mark_changed(f->w, e[5], f->pos);
    EXPECT_EQ(sys_run(f->w, &fw, &g_seen), 1u);
    EXPECT_EQ(sys_run(f->w, &sv, &g_seen), 1u);
    EXPECT_TRUE(seen_has(&g_seen, e[5]));

    /* Without a window (outside a system), writes are stamped with the current change tick. */
    {
        const de_query_desc d = {tw2, 1, NULL, 0};
        const de_query q = de_query_create(f->w, &d);
        n = 0;
        de_query_each(f->w, q, count_fn, &n);
        EXPECT_EQ(n, 6u);
        u32 ad = 0, ch = 0;
        row_ticks(f->w, e[0], f->pos, &ad, &ch);
        EXPECT_EQ(ch, ecs_change_tick(f->w));
        EXPECT_EQ(sys_run(f->w, &sp, &g_seen), 6u);
    }

    /* Parallel: de_query_par_each marks on the iterating threads, for no task system and 1, 2
     * and 4 workers; the results are the same for every configuration. */
    const u32 configs[] = {0, 1, 2, 4};
    u64 ref_hash = 0;
    for (u32 cfg = 0; cfg < 4; ++cfg) {
        de_task_system *ts = NULL;
        if (configs[cfg] > 0) {
            de_fp_env_init_thread();
            de_task_system_desc td = {0};
            td.worker_count = configs[cfg];
            td.background_worker_count = 1;
            ASSERT_EQ(de_task_system_create(&td, &ts), DE_OK);
        }
        ecs_fx_destroy_world(f);
        de_world_desc d;
        memset(&d, 0, sizeof d);
        d.chunk_bytes = 2048;
        d.tasks = ts;
        ecs_fx_create_world(f, &d);
        const de_component sets[4][3] = {
            {f->pos}, {f->pos, f->vel}, {f->vel}, {f->pos, f->vel, f->health}};
        const u32 sizes[4] = {1, 2, 1, 3};
        u32 with_pos = 0, with_pos_vel = 0;
        for (u32 i = 0; i < PAR_N; ++i) {
            const u32 k = (i * 2654435761u >> 7) % 4u;
            g_par_e[i] = spawn(f, sets[k], sizes[k]);
            with_pos += k != 2u ? 1u : 0u;
            with_pos_vel += (k == 1u || k == 3u) ? 1u : 0u;
        }
        sim_system wp = sys_make(f->w, tw2, 1, NULL, 0);
        sim_system cpos = sys_make(f->w, tp, 1, cp, 1);
        sim_system cvel = sys_make(f->w, tv, 1, cv, 1);
        sim_system fwv = sys_make(f->w, wv, 1, cp, 1);
        par_count pc;
        de_atomic_init_u32(&pc.rows, 0);
        (void)sys_run_with(f->w, &cpos, par_count_fn, &pc, 64, true);
        (void)sys_run_with(f->w, &cvel, par_count_fn, &pc, 64, true);
        (void)sys_run_with(f->w, &fwv, par_count_fn, &pc, 64, true);
        (void)sys_run_with(f->w, &cvel, par_count_fn, &pc, 64, true);
        de_atomic_init_u32(&pc.rows, 0);
        sys_run_with(f->w, &cpos, par_count_fn, &pc, 64, true);
        EXPECT_EQ(de_atomic_load_u32(&pc.rows, DE_ATOMIC_RELAXED), 0u);

        /* Write every pos row in parallel: each is marked with the writer's tick. */
        de_atomic_init_u32(&pc.rows, 0);
        sys_run_with(f->w, &wp, par_write_fn, &pc, 64, true);
        EXPECT_EQ(de_atomic_load_u32(&pc.rows, DE_ATOMIC_RELAXED), with_pos);
        u32 bad = 0;
        for (u32 i = 0; i < PAR_N; ++i) {
            if (!de_entity_has(f->w, g_par_e[i], f->pos))
                continue;
            u32 ad = 0, ch = 0;
            row_ticks(f->w, g_par_e[i], f->pos, &ad, &ch);
            bad += ch != wp.this_run ? 1u : 0u;
        }
        EXPECT_EQ(bad, 0u);
        de_atomic_init_u32(&pc.rows, 0);
        sys_run_with(f->w, &cpos, par_count_fn, &pc, 1000, true);
        EXPECT_EQ(de_atomic_load_u32(&pc.rows, DE_ATOMIC_RELAXED), with_pos);

        /* Filtered parallel writer: vel is marked for exactly the rows whose pos changed. The
         * first run consumes the full write above. */
        sys_run_with(f->w, &fwv, par_count_fn, &pc, 50, true);
        for (u32 i = 0; i < PAR_N; i += 3u)
            de_entity_mark_changed(f->w, g_par_e[i], f->pos);
        de_atomic_init_u32(&pc.rows, 0);
        sys_run_with(f->w, &fwv, par_count_fn, &pc, 50, true);
        u32 expect_vel = 0;
        for (u32 i = 0; i < PAR_N; i += 3u)
            expect_vel +=
                de_entity_has(f->w, g_par_e[i], f->pos) && de_entity_has(f->w, g_par_e[i], f->vel)
                    ? 1u
                    : 0u;
        EXPECT_EQ(de_atomic_load_u32(&pc.rows, DE_ATOMIC_RELAXED), expect_vel);
        /* Which rows: serial check, hashed in iteration order and compared across configs. */
        u64 h = 0xCBF29CE484222325ull;
        u32 seen_vel = 0;
        for (u32 i = 0; i < PAR_N; ++i) {
            if (!de_entity_has(f->w, g_par_e[i], f->vel))
                continue;
            u32 ad = 0, ch = 0;
            row_ticks(f->w, g_par_e[i], f->vel, &ad, &ch);
            const bool marked = ch == fwv.this_run;
            EXPECT_EQ(marked, (i % 3u) == 0u && de_entity_has(f->w, g_par_e[i], f->pos));
            seen_vel += marked ? 1u : 0u;
            h = fnv(h, ((u64)i << 1) | (marked ? 1u : 0u));
        }
        EXPECT_EQ(seen_vel, expect_vel);
        if (cfg == 0)
            ref_hash = h;
        EXPECT_EQ(h, ref_hash);
        EXPECT_GT(with_pos_vel, 0u);

        ecs_fx_destroy_world(f);
        ecs_fx_create_world(f, NULL);
        if (ts != NULL)
            de_task_system_destroy(ts);
    }
}

/* ---- change.deferred_add_marks ----------------------------------------------------------------
 */

TEST_F(change, deferred_add_marks, ecs_fx) {
    /* DENG-349: a deferred add on an entity that has the component replaces the value; it is a
     * change, not an add. A deferred add of a new component is an add. */
    const de_query_term t[] = {{f->pos, DE_READ}};
    const de_query_filter changed[] = {{f->pos, DE_CHANGED}};
    const de_query_filter added[] = {{f->pos, DE_ADDED}};
    const de_entity e1 = spawn(f, &f->pos, 1);
    const de_entity e2 = spawn(f, &f->vel, 1);
    sim_system sc = sys_make(f->w, t, 1, changed, 1);
    sim_system sa = sys_make(f->w, t, 1, added, 1);
    EXPECT_EQ(sys_run(f->w, &sc, &g_seen), 1u);
    EXPECT_EQ(sys_run(f->w, &sa, &g_seen), 1u);

    de_commands *c = de_world_commands(f->w);
    ((TPos *)de_cmd_add(c, e1, f->pos))->v.x = 11.0f;
    ((TPos *)de_cmd_add(c, e2, f->pos))->v.x = 12.0f;
    de_world_apply_commands(f->w);
    EXPECT_FLOAT_EQ(((const TPos *)de_entity_get_const(f->w, e1, f->pos))->v.x, 11.0, 0.0);

    EXPECT_EQ(sys_run(f->w, &sc, &g_seen), 2u);
    EXPECT_TRUE(seen_has(&g_seen, e1) && seen_has(&g_seen, e2));
    EXPECT_EQ(sys_run(f->w, &sa, &g_seen), 1u);
    EXPECT_TRUE(seen_has(&g_seen, e2));
    EXPECT_EQ(sys_run(f->w, &sc, &g_seen), 0u);
    EXPECT_EQ(sys_run(f->w, &sa, &g_seen), 0u);

    /* A deferred remove + add is a new add. */
    de_cmd_remove(c, e1, f->pos);
    (void)de_cmd_add(c, e1, f->pos);
    de_world_apply_commands(f->w);
    EXPECT_EQ(sys_run(f->w, &sa, &g_seen), 1u);
    EXPECT_TRUE(seen_has(&g_seen, e1));
}

/* ---- change.wrap_equivalence ------------------------------------------------------------------
 */

#define WRAP_RUNS     1024u
#define WRAP_SYSTEMS  5u
#define WRAP_ENTITIES 256u

typedef struct wrap_trace {
    u64 hash;
    u32 rows[WRAP_SYSTEMS]; /* rows seen per system, summed over the runs */
    u32 end_tick;
} wrap_trace;

static u32 rng_next(u32 *s) {
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

/* One frame of world changes, all from the same PRNG sequence. */
static void wrap_mutate(ecs_fx *f, de_entity *ents, u32 *rng) {
    const de_component comps[3] = {f->pos, f->vel, f->health};
    for (u32 k = 0; k < 6; ++k) {
        const u32 op = rng_next(rng) % 9u;
        const u32 slot = rng_next(rng) % WRAP_ENTITIES;
        const de_component c = comps[rng_next(rng) % 3u];
        de_entity *e = &ents[slot];
        const bool alive = de_entity_alive(f->w, *e);
        switch (op) {
        case 0: /* spawn_with a subset */
        case 1: {
            if (alive)
                break;
            const u32 mask = (rng_next(rng) % 7u) + 1u;
            de_component set[3];
            u32 n = 0;
            for (u32 b = 0; b < 3; ++b)
                if (mask & (1u << b))
                    set[n++] = comps[b];
            *e = spawn(f, set, n);
            break;
        }
        case 2:
            if (alive)
                de_entity_despawn(f->w, *e);
            break;
        case 3:
            if (alive)
                (void)de_entity_add(f->w, *e, c);
            break;
        case 4:
            if (alive)
                de_entity_remove(f->w, *e, c);
            break;
        case 5:
            if (alive)
                de_entity_mark_changed(f->w, *e, c);
            break;
        case 6:
            if (alive) {
                u8 *p = (u8 *)de_entity_get(f->w, *e, c);
                if (p != NULL)
                    p[0] ^= 1u;
            }
            break;
        case 7:
            if (alive)
                (void)de_entity_get_const(f->w, *e, c);
            break;
        default: /* deferred add (new or replacing) */
            if (alive) {
                (void)de_cmd_add(de_world_commands(f->w), *e, c);
                de_world_apply_commands(f->w);
            }
            break;
        }
    }
}

/* 1024 simulated system runs of five systems, starting at change tick `start`. */
static void wrap_simulate(ecs_fx *f, u32 start, wrap_trace *out) {
    ecs_fx_destroy_world(f);
    de_world_desc d;
    memset(&d, 0, sizeof d);
    d.chunk_bytes = 1024;
    ecs_fx_create_world(f, &d);
    ecs_debug_set_tick(f->w, start);
    memset(out, 0, sizeof *out);
    out->hash = 0xCBF29CE484222325ull;

    static de_entity ents[WRAP_ENTITIES];
    memset(ents, 0, sizeof ents);
    const de_query_term t_pos[] = {{f->pos, DE_READ}};
    const de_query_term t_wvel[] = {{f->vel, DE_WRITE}};
    const de_query_term t_vel[] = {{f->vel, DE_READ}};
    const de_query_term t_hp[] = {{f->health, DE_READ}, {f->pos, DE_OPTIONAL_WRITE}};
    const de_query_filter f_apos[] = {{f->pos, DE_ADDED}};
    const de_query_filter f_cpos[] = {{f->pos, DE_CHANGED}};
    const de_query_filter f_cvel[] = {{f->vel, DE_CHANGED}};
    const de_query_filter f_ahp[] = {{f->health, DE_ADDED}};
    sim_system sys[WRAP_SYSTEMS];
    sys[0] = sys_make(f->w, t_pos, 1, f_apos, 1);
    sys[1] = sys_make(f->w, t_pos, 1, f_cpos, 1);
    sys[2] = sys_make(f->w, t_wvel, 1, f_cpos, 1); /* writes vel where pos changed */
    sys[3] = sys_make(f->w, t_vel, 1, f_cvel, 1);
    sys[4] = sys_make(f->w, t_hp, 2, f_ahp, 1); /* optional write of pos */

    u32 rng = 0x2545F491u;
    for (u32 r = 0; r < WRAP_RUNS; ++r) {
        const u32 k = r % WRAP_SYSTEMS;
        if (k == 0)
            wrap_mutate(f, ents, &rng);
        const u32 n = sys_run(f->w, &sys[k], &g_seen);
        out->rows[k] += n;
        out->hash = fnv(out->hash, ((u64)r << 32) | n);
        for (u32 i = 0; i < n; ++i)
            out->hash = fnv(out->hash, g_seen.e[i].bits);
    }
    /* Every stored tick, relative to the start. */
    for (u32 ai = 0; ai < ecs_archetype_count(f->w); ++ai) {
        const ecs_archetype *a = ecs_archetype_at(f->w, ai);
        for (u32 c = 0; c < a->chunks.count; ++c) {
            const ecs_chunk *ch = ecs_table_chunk(a, c);
            for (u32 col = 0; col < a->column_count; ++col)
                for (u32 row = 0; row < ch->count; ++row)
                    out->hash =
                        fnv(out->hash, ((u64)(ecs_chunk_added(a, ch, col)[row] - start) << 32) |
                                           (ecs_chunk_changed(a, ch, col)[row] - start));
        }
    }
    out->end_tick = ecs_change_tick(f->w);
}

TEST_F(change, wrap_equivalence, ecs_fx) {
    static wrap_trace from_zero, near_wrap, mid;
    wrap_simulate(f, 0u, &from_zero);
    wrap_simulate(f, 0xFFFFFF00u, &near_wrap);
    wrap_simulate(f, 0x7FFFFF80u, &mid);
    /* The run near the wrap crossed it. */
    EXPECT_LT(near_wrap.end_tick, 0xFFFFFF00u);
    EXPECT_EQ(near_wrap.end_tick - 0xFFFFFF00u, from_zero.end_tick);
    EXPECT_EQ(near_wrap.hash, from_zero.hash);
    EXPECT_EQ(mid.hash, from_zero.hash);
    for (u32 k = 0; k < WRAP_SYSTEMS; ++k) {
        EXPECT_EQ(near_wrap.rows[k], from_zero.rows[k]);
        EXPECT_GT(from_zero.rows[k], 0u); /* every filter saw something: not vacuous */
    }
    printf("change.wrap_equivalence: trace 0x%016llx, rows %u %u %u %u %u\n",
           (unsigned long long)from_zero.hash, from_zero.rows[0], from_zero.rows[1],
           from_zero.rows[2], from_zero.rows[3], from_zero.rows[4]);
    EXPECT_EQ(from_zero.hash, CHANGE_WRAP_TRACE);
}

/* ---- change.clamp_old_ticks -------------------------------------------------------------------
 */

TEST_F(change, clamp_old_ticks, ecs_fx) {
    const de_query_term t[] = {{f->pos, DE_READ}};
    const de_query_filter changed[] = {{f->pos, DE_CHANGED}};
    const u32 t0 = 1000u;
    ecs_debug_set_tick(f->w, t0);
    const de_entity e_old = spawn(f, &f->pos, 1); /* never touched again */
    sim_system s = sys_make(f->w, t, 1, changed, 1);
    sim_system idle = sys_make(f->w, t, 1, changed, 1);
    EXPECT_EQ(sys_run(f->w, &s, &g_seen), 1u);
    EXPECT_EQ(sys_run(f->w, &idle, &g_seen), 1u);
    const de_entity e_new = spawn(f, &f->pos, 1);

    /* 8 rounds of ECS_CHANGE_CHECK_PERIOD ticks = 2^32 ticks: the counter comes back to where it
     * started. Each round jumps PERIOD - 1 ticks (as many advances without a check) and then
     * advances once, which runs the periodic check. */
    for (u32 round = 0; round < 8u; ++round) {
        f->w->change_tick += ECS_CHANGE_CHECK_PERIOD - 1u;
        de_entity_mark_changed(f->w, e_new, f->pos);
        const u32 check_before = f->w->change_check_tick;
        const u32 last_s = ecs_query_get(f->w, s.q)->this_run_tick;
        EXPECT_EQ(sys_run(f->w, &s, &g_seen), 1u);
        EXPECT_TRUE(seen_has(&g_seen, e_new));
        EXPECT_FALSE(seen_has(&g_seen, e_old));
        EXPECT_NE(f->w->change_check_tick, check_before); /* the advance ran the check */
        EXPECT_EQ(f->w->change_check_tick, ecs_change_tick(f->w));
        /* Every stored tick is at most ECS_MAX_CHANGE_AGE old. */
        u32 ad = 0, ch = 0;
        row_ticks(f->w, e_old, f->pos, &ad, &ch);
        const u32 now = ecs_change_tick(f->w);
        EXPECT_LE(now - ad, ECS_MAX_CHANGE_AGE);
        EXPECT_LE(now - ch, ECS_MAX_CHANGE_AGE);
        if (round >= 2u) { /* older than 2^30: clamped */
            EXPECT_EQ(ad, now - ECS_MAX_CHANGE_AGE);
            EXPECT_EQ(ch, now - ECS_MAX_CHANGE_AGE);
        }
        EXPECT_LE(now - ecs_query_get(f->w, idle.q)->last_run_tick, ECS_MAX_CHANGE_AGE);
        if (round == 7u) {
            /* After 2^32 ticks the raw tick of e_old looks a few ticks old again: without the
             * clamp, s would have reported it as changed. */
            EXPECT_LT(s.this_run - t0, 16u);
            EXPECT_TRUE(ecs_tick_in_window(t0, last_s, s.this_run));
        }
    }
    /* The idle system's window was clamped with the rows: it sees the recent change only. */
    de_entity_mark_changed(f->w, e_new, f->pos);
    EXPECT_EQ(sys_run(f->w, &idle, &g_seen), 1u);
    EXPECT_TRUE(seen_has(&g_seen, e_new));
    EXPECT_EQ(sys_run(f->w, &idle, &g_seen), 0u);

    /* ecs_check_change_ticks directly: only ticks older than 2^30 move, recent ones stay. */
    const u32 base = 0xF0000000u;
    ecs_debug_set_tick(f->w, base);
    const de_entity e_a = spawn(f, &f->pos, 1);
    ecs_debug_set_tick(f->w, base + ECS_MAX_CHANGE_AGE + 5u);
    const de_entity e_b = spawn(f, &f->pos, 1);
    de_entity_mark_changed(f->w, e_a, f->pos);
    ecs_debug_set_tick(f->w, base + ECS_MAX_CHANGE_AGE + 9u);
    ecs_check_change_ticks(f->w);
    u32 ad = 0, ch = 0;
    row_ticks(f->w, e_a, f->pos, &ad, &ch);
    EXPECT_EQ(ad, base + 9u); /* clamped to now - 2^30 */
    EXPECT_EQ(ch, base + ECS_MAX_CHANGE_AGE + 5u);
    row_ticks(f->w, e_b, f->pos, &ad, &ch);
    EXPECT_EQ(ad, base + ECS_MAX_CHANGE_AGE + 5u);
    EXPECT_EQ(ch, base + ECS_MAX_CHANGE_AGE + 5u);
    /* A clamped tick is never in a window, even one that starts at the clamp limit. */
    const u32 now = ecs_change_tick(f->w);
    EXPECT_FALSE(ecs_tick_in_window(now - ECS_MAX_CHANGE_AGE, now - ECS_MAX_CHANGE_AGE, now));
    EXPECT_FALSE(ecs_tick_in_window(now - ECS_MAX_CHANGE_AGE - 7u, now - 1u, now));
    EXPECT_TRUE(ecs_tick_in_window(now, now - 1u, now));
    EXPECT_FALSE(ecs_tick_in_window(now + 1u, now - 1u, now)); /* newer than this_run */
}

/* ---- change.no_alloc_marking ------------------------------------------------------------------
 */

TEST_F(change, no_alloc_marking, ecs_fx) {
    /* Marking (write iteration serial and parallel, mark_changed, mutable get, add of an existing
     * component), the filtered iteration and the clamp allocate nothing. */
    de_fp_env_init_thread();
    de_task_system_desc td = {0};
    td.worker_count = 2;
    td.background_worker_count = 1;
    de_task_system *ts = NULL;
    ASSERT_EQ(de_task_system_create(&td, &ts), DE_OK);
    ecs_fx_destroy_world(f);
    de_world_desc d;
    memset(&d, 0, sizeof d);
    d.chunk_bytes = 1024;
    d.tasks = ts;
    ecs_fx_create_world(f, &d);
    const de_component pv[] = {f->pos, f->vel};
    static de_entity e[2000];
    for (u32 i = 0; i < 2000; ++i)
        e[i] = spawn(f, pv, (i % 2u) + 1u);
    const de_query_term tw[] = {{f->pos, DE_WRITE}, {f->vel, DE_OPTIONAL_WRITE}};
    const de_query_term tp[] = {{f->pos, DE_READ}};
    const de_query_filter cp[] = {{f->pos, DE_CHANGED}};
    sim_system w = sys_make(f->w, tw, 2, NULL, 0);
    sim_system c = sys_make(f->w, tp, 1, cp, 1);
    par_count pc;
    de_atomic_init_u32(&pc.rows, 0);
    const de_tracking_stats before = de_tracking_allocator_stats(&f->track);
    for (u32 round = 0; round < 100; ++round) {
        sys_run_with(f->w, &w, par_count_fn, &pc, 0, false);
        sys_run_with(f->w, &w, par_count_fn, &pc, 100, true);
        de_entity_mark_changed(f->w, e[round], f->pos);
        (void)de_entity_get(f->w, e[round + 1u], f->pos);
        (void)de_entity_add(f->w, e[round + 2u], f->pos);
        sys_run_with(f->w, &c, par_count_fn, &pc, 0, false);
        sys_run_with(f->w, &c, par_count_fn, &pc, 64, true);
        ecs_check_change_ticks(f->w);
    }
    const de_tracking_stats after = de_tracking_allocator_stats(&f->track);
    EXPECT_EQ(after.alloc_count, before.alloc_count);
    EXPECT_EQ(after.realloc_count, before.realloc_count);
    EXPECT_EQ(after.free_count, before.free_count);
    EXPECT_GT(de_atomic_load_u32(&pc.rows, DE_ATOMIC_RELAXED), 0u);
    ecs_fx_destroy_world(f);
    ecs_fx_create_world(f, NULL);
    de_task_system_destroy(ts);
}
