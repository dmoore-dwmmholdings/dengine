/* test_query.c: ECS queries: matching, the archetype cache, chunk slices, de_query_each and
 * de_query_count (DENG-346, SPEC §7.2). Parallel iteration is in test_query_par.c. */
#include "ecs_test_util.h"

#include "ecs/query_internal.h"

#include <stdint.h>
#include <stdio.h>

/* ---- World population ------------------------------------------------------------------------ */

/* Entities of the populated world (fixture components: pos 1, vel 2, health 3, enemy 4 (tag),
 * rot 5). POP_MAX bounds every population below. */
#define POP_MAX 4096u

typedef struct pop {
    de_entity e[POP_MAX];
    u32 n;
} pop;

static pop g_pop;

/* Spawns `count` entities with exactly `comps[0..n)` and fills their data from their number. */
static void spawn_set(ecs_fx *f, pop *p, const de_component *comps, u32 n, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        ASSERT_LT(p->n, POP_MAX);
        de_entity e = DE_ENTITY_NULL;
        ASSERT_EQ(de_entity_spawn_with(f->w, comps, n, &e), DE_OK);
        const u32 k = p->n;
        TPos *pos = (TPos *)de_entity_get(f->w, e, f->pos);
        if (pos != NULL)
            pos->v = (de_vec3){(f32)k, (f32)(k * 2u), (f32)(k * 3u)};
        THealth *h = (THealth *)de_entity_get(f->w, e, f->health);
        if (h != NULL)
            *h = (THealth){(f32)k, 100.0f};
        p->e[p->n++] = e;
    }
}

/* Eight archetypes (one left empty) plus entities with no components. */
static void populate8(ecs_fx *f, pop *p) {
    memset(p, 0, sizeof *p);
    const de_component a1[] = {f->pos};
    const de_component a2[] = {f->pos, f->vel};
    const de_component a3[] = {f->pos, f->vel, f->health};
    const de_component a4[] = {f->pos, f->enemy};
    const de_component a5[] = {f->vel, f->health};
    const de_component a6[] = {f->pos, f->vel, f->enemy, f->rot};
    const de_component a7[] = {f->health, f->rot};
    const de_component a8[] = {f->pos, f->health, f->enemy};
    spawn_set(f, p, a1, 1, 37);
    spawn_set(f, p, a2, 2, 120);
    spawn_set(f, p, a3, 3, 5);
    spawn_set(f, p, a4, 2, 64);
    spawn_set(f, p, a5, 2, 1);
    spawn_set(f, p, a6, 4, 200);
    spawn_set(f, p, a7, 2, 13);
    /* a8 exists but is empty: spawn into it, then despawn. */
    pop tmp;
    memset(&tmp, 0, sizeof tmp);
    spawn_set(f, &tmp, a8, 3, 3);
    for (u32 i = 0; i < tmp.n; ++i)
        de_entity_despawn(f->w, tmp.e[i]);
    for (u32 i = 0; i < 10; ++i) {
        p->e[p->n] = de_entity_spawn(f->w);
        ASSERT_TRUE(de_entity_alive(f->w, p->e[p->n]));
        ++p->n;
    }
}

static void recreate_world(ecs_fx *f, u32 chunk_bytes) {
    ecs_fx_destroy_world(f);
    de_world_desc d;
    memset(&d, 0, sizeof d);
    d.chunk_bytes = chunk_bytes;
    ecs_fx_create_world(f, &d);
}

/* ---- A checking callback --------------------------------------------------------------------- */

typedef struct collect {
    de_world *w;
    const de_query_term *terms;
    u32 term_count;
    u32 seen[POP_MAX * 2u]; /* per entity index */
    u32 rows;
    u32 slices;
    u64 last_key;
    u32 next_first_row;
    bool order_ok;
    bool cols_ok;
    bool first_row_ok;
    bool range_ok;
} collect;

static collect g_col;

static void collect_reset(collect *c, de_world *w, const de_query_term *terms, u32 term_count) {
    memset(c, 0, sizeof *c);
    c->w = w;
    c->terms = terms;
    c->term_count = term_count;
    c->order_ok = c->cols_ok = c->first_row_ok = c->range_ok = true;
}

static void collect_fn(const de_query_chunk *ch, void *user) {
    collect *c = (collect *)user;
    ++c->slices;
    if (ch->count == 0)
        c->order_ok = false;
    if (ch->first_row != c->next_first_row)
        c->first_row_ok = false;
    if (ch->range != 0)
        c->range_ok = false;
    c->next_first_row = ch->first_row + ch->count;
    for (u32 i = 0; i < ch->count; ++i) {
        const de_entity e = ch->entities[i];
        const ecs_record *rec = ecs_entity_record(c->w, e);
        if (rec == NULL) {
            c->order_ok = false;
            continue;
        }
        /* Iteration order: archetype creation order, chunk, row; strictly increasing. */
        const u64 key = ((u64)rec->archetype << 42) | ((u64)rec->chunk << 21) | (u64)rec->row;
        if (c->rows > 0 && key <= c->last_key)
            c->order_ok = false;
        c->last_key = key;
        ++c->seen[de_entity_index(e)];
        ++c->rows;
        for (u32 t = 0; t < c->term_count; ++t) {
            const de_component comp = c->terms[t].component;
            const bool has = de_entity_has(c->w, e, comp);
            void *col = ch->columns[t];
            if (!has) {
                if (col != NULL)
                    c->cols_ok = false;
                continue;
            }
            if (col == NULL) {
                c->cols_ok = false;
                continue;
            }
            const u32 size = ecs_component_get(c->w, comp.id)->size;
            const u8 *want = (const u8 *)de_entity_get(c->w, e, comp);
            const u8 *got = size == 0 ? (const u8 *)col : (const u8 *)col + (size_t)i * size;
            if (got != want)
                c->cols_ok = false;
        }
    }
}

/* ---- query.with_without_optional_bruteforce -------------------------------------------------- */

/* Roles of one component in a generated query. */
enum { R_NONE, R_READ, R_WRITE, R_OPT, R_WITH, R_WITHOUT, R_COUNT };

TEST_F(query, with_without_optional_bruteforce, ecs_fx) {
    /* Every assignment of a role to each of the 5 components (6^5 = 7776 queries) over the eight
     * archetypes. The entities each query returns are compared with a brute-force filter over
     * every alive entity, and so is de_query_count. */
    recreate_world(f, 1024); /* small chunks: several chunks per table */
    populate8(f, &g_pop);
    const de_component comps[5] = {f->pos, f->vel, f->health, f->enemy, f->rot};
    u32 total = 1;
    for (u32 i = 0; i < 5; ++i)
        total *= R_COUNT;
    u32 checked = 0;
    for (u32 code = 0; code < total; ++code) {
        u32 role[5];
        u32 x = code;
        for (u32 i = 0; i < 5; ++i) {
            role[i] = x % R_COUNT;
            x /= R_COUNT;
        }
        de_query_term terms[5];
        de_query_filter filters[5];
        u32 nt = 0, nf = 0;
        for (u32 k = 0; k < 5; ++k) {
            const u32 i = (code & 1u) ? 4u - k : k; /* vary the term order */
            switch (role[i]) {
            case R_READ:
                terms[nt++] = (de_query_term){comps[i], DE_READ};
                break;
            case R_WRITE:
                terms[nt++] = (de_query_term){comps[i], DE_WRITE};
                break;
            case R_OPT:
                terms[nt++] =
                    (de_query_term){comps[i], (code & 2u) ? DE_OPTIONAL_WRITE : DE_OPTIONAL_READ};
                break;
            case R_WITH:
                filters[nf++] = (de_query_filter){comps[i], DE_WITH};
                break;
            case R_WITHOUT:
                filters[nf++] = (de_query_filter){comps[i], DE_WITHOUT};
                break;
            default:
                break;
            }
        }
        const de_query_desc d = {terms, nt, filters, nf};
        const de_query q = de_query_create(f->w, &d);
        ASSERT_TRUE(DE_HANDLE_VALID(q));

        collect_reset(&g_col, f->w, terms, nt);
        de_query_each(f->w, q, collect_fn, &g_col);
        u32 expected = 0;
        bool sets_equal = true;
        for (u32 n = 0; n < g_pop.n; ++n) {
            const de_entity e = g_pop.e[n];
            bool match = true;
            for (u32 i = 0; i < 5; ++i) {
                const bool has = de_entity_has(f->w, e, comps[i]);
                if ((role[i] == R_READ || role[i] == R_WRITE || role[i] == R_WITH) && !has)
                    match = false;
                if (role[i] == R_WITHOUT && has)
                    match = false;
            }
            expected += match ? 1u : 0u;
            if (g_col.seen[de_entity_index(e)] != (match ? 1u : 0u))
                sets_equal = false;
        }
        if (!sets_equal || g_col.rows != expected || !g_col.order_ok || !g_col.cols_ok ||
            !g_col.first_row_ok || !g_col.range_ok || de_query_count(f->w, q) != expected) {
            char msg[256];
            snprintf(msg, sizeof msg,
                     "query code %u: expected %u rows, got %u (sets %d order %d cols %d first_row "
                     "%d count %u)",
                     code, expected, g_col.rows, (int)sets_equal, (int)g_col.order_ok,
                     (int)g_col.cols_ok, (int)g_col.first_row_ok, de_query_count(f->w, q));
            FAIL(msg);
        }
        de_query_destroy(f->w, q);
        ++checked;
    }
    EXPECT_EQ(checked, total);
}

/* ---- query.create_validation ----------------------------------------------------------------- */

TEST_F(query, create_validation, ecs_fx) {
    const de_query_term ok_terms[] = {{f->pos, DE_READ}};
    const de_query_desc ok = {ok_terms, 1, NULL, 0};
    de_query q = de_query_create(NULL, &ok);
    EXPECT_FALSE(DE_HANDLE_VALID(q));
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    q = de_query_create(f->w, NULL);
    EXPECT_FALSE(DE_HANDLE_VALID(q));

    const de_query_desc null_terms = {NULL, 1, NULL, 0};
    EXPECT_FALSE(DE_HANDLE_VALID(de_query_create(f->w, &null_terms)));
    const de_query_desc null_filters = {ok_terms, 1, NULL, 2};
    EXPECT_FALSE(DE_HANDLE_VALID(de_query_create(f->w, &null_filters)));

    const de_query_term unreg[] = {{{99}, DE_READ}};
    const de_query_desc d_unreg = {unreg, 1, NULL, 0};
    EXPECT_FALSE(DE_HANDLE_VALID(de_query_create(f->w, &d_unreg)));
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);

    const de_query_term dup[] = {{f->pos, DE_READ}, {f->vel, DE_READ}, {f->pos, DE_WRITE}};
    const de_query_desc d_dup = {dup, 3, NULL, 0};
    EXPECT_FALSE(DE_HANDLE_VALID(de_query_create(f->w, &d_dup)));

    const de_query_term bad_access[] = {{f->pos, (de_access)7}};
    const de_query_desc d_bad_access = {bad_access, 1, NULL, 0};
    EXPECT_FALSE(DE_HANDLE_VALID(de_query_create(f->w, &d_bad_access)));

    const de_query_filter bad_filter[] = {{f->vel, (de_filter)9}};
    const de_query_desc d_bad_filter = {ok_terms, 1, bad_filter, 1};
    EXPECT_FALSE(DE_HANDLE_VALID(de_query_create(f->w, &d_bad_filter)));

    const de_query_filter unreg_filter[] = {{{77}, DE_WITH}};
    const de_query_desc d_unreg_filter = {ok_terms, 1, unreg_filter, 1};
    EXPECT_FALSE(DE_HANDLE_VALID(de_query_create(f->w, &d_unreg_filter)));

    /* WITHOUT a term, or WITHOUT and WITH the same component: never matches. */
    const de_query_filter without_term[] = {{f->pos, DE_WITHOUT}};
    const de_query_desc d_without_term = {ok_terms, 1, without_term, 1};
    EXPECT_FALSE(DE_HANDLE_VALID(de_query_create(f->w, &d_without_term)));
    const de_query_filter with_without[] = {{f->vel, DE_WITH}, {f->vel, DE_WITHOUT}};
    const de_query_desc d_with_without = {ok_terms, 1, with_without, 2};
    EXPECT_FALSE(DE_HANDLE_VALID(de_query_create(f->w, &d_with_without)));
    const de_query_term opt_vel[] = {{f->vel, DE_OPTIONAL_READ}};
    const de_query_filter without_vel[] = {{f->vel, DE_WITHOUT}};
    const de_query_desc d_opt_without = {opt_vel, 1, without_vel, 1};
    EXPECT_FALSE(DE_HANDLE_VALID(de_query_create(f->w, &d_opt_without)));

    /* ADDED / CHANGED on a tag: tags have no ticks. */
    const de_query_filter changed_tag[] = {{f->enemy, DE_CHANGED}};
    const de_query_desc d_changed_tag = {ok_terms, 1, changed_tag, 1};
    EXPECT_FALSE(DE_HANDLE_VALID(de_query_create(f->w, &d_changed_tag)));
    EXPECT_EQ(de_error_last()->result, DE_ERR_UNSUPPORTED);

    /* Limits. */
    de_query_term many[DE_QUERY_MAX_TERMS + 1u];
    for (u32 i = 0; i < DE_QUERY_MAX_TERMS + 1u; ++i)
        many[i] = (de_query_term){f->pos, DE_READ};
    const de_query_desc d_many = {many, DE_QUERY_MAX_TERMS + 1u, NULL, 0};
    EXPECT_FALSE(DE_HANDLE_VALID(de_query_create(f->w, &d_many)));
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);

    /* Accepted: duplicate filters, WITH on a term, ADDED + CHANGED on a term, an empty query. */
    const de_query_filter fine[] = {
        {f->vel, DE_WITH},    {f->vel, DE_WITH},      {f->pos, DE_WITH},     {f->pos, DE_ADDED},
        {f->pos, DE_CHANGED}, {f->enemy, DE_WITHOUT}, {f->enemy, DE_WITHOUT}};
    const de_query_desc d_fine = {ok_terms, 1, fine, 7};
    q = de_query_create(f->w, &d_fine);
    EXPECT_TRUE(DE_HANDLE_VALID(q));
    const ecs_query *iq = ecs_query_get(f->w, q);
    ASSERT_TRUE(iq != NULL);
    EXPECT_EQ(iq->required_count, 2u); /* pos, vel */
    EXPECT_EQ(iq->excluded_count, 1u); /* enemy */
    EXPECT_EQ(iq->change_count, 2u);
    const de_query_desc empty = {NULL, 0, NULL, 0};
    const de_query qe = de_query_create(f->w, &empty);
    EXPECT_TRUE(DE_HANDLE_VALID(qe));
    /* The empty query matches every archetype, the empty one included. */
    de_entity_spawn(f->w);
    EXPECT_EQ(de_query_count(f->w, qe), 1u);
    EXPECT_EQ(ecs_query_entry_count(ecs_query_get(f->w, qe)), ecs_archetype_count(f->w));

    /* Invalid handles do nothing. */
    const de_query bogus = {0x12345u};
    de_query_each(f->w, bogus, collect_fn, &g_col);
    de_query_par_each(f->w, bogus, 0, collect_fn, &g_col);
    EXPECT_EQ(de_query_count(f->w, bogus), 0u);
    de_query_destroy(f->w, bogus);
    de_query_destroy(NULL, q);
    EXPECT_EQ(de_query_count(NULL, q), 0u);
}

/* ---- query.destroy_handles ------------------------------------------------------------------- */

TEST_F(query, destroy_handles, ecs_fx) {
    const de_query_term t[] = {{f->pos, DE_READ}};
    const de_query_desc d = {t, 1, NULL, 0};
    const de_query a = de_query_create(f->w, &d);
    const de_query b = de_query_create(f->w, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(a) && DE_HANDLE_VALID(b));
    EXPECT_NE(a.id, b.id);
    de_query_destroy(f->w, a);
    EXPECT_TRUE(ecs_query_get(f->w, a) == NULL);
    EXPECT_TRUE(ecs_query_get(f->w, b) != NULL);
    de_query_destroy(f->w, a); /* stale: ignored */
    /* The freed slot is reused (LIFO) with a new generation; the old handle stays stale. */
    const de_query c = de_query_create(f->w, &d);
    EXPECT_EQ(c.id & ECS_QUERY_INDEX_MASK, a.id & ECS_QUERY_INDEX_MASK);
    EXPECT_NE(c.id, a.id);
    EXPECT_TRUE(ecs_query_get(f->w, a) == NULL);
    EXPECT_EQ(de_query_count(f->w, a), 0u);

    /* A slot whose generation would wrap is retired. */
    ecs_query_registry *r = f->w->queries;
    const u32 slot = (c.id & ECS_QUERY_INDEX_MASK) - 1u;
    DE_ARRAY_AT(&r->slots, ecs_query_slot, slot).generation = ECS_QUERY_GEN_MAX;
    const de_query c_max = {(ECS_QUERY_GEN_MAX << ECS_QUERY_INDEX_BITS) | (slot + 1u)};
    ASSERT_TRUE(ecs_query_get(f->w, c_max) != NULL);
    de_query_destroy(f->w, c_max);
    const de_query d2 = de_query_create(f->w, &d);
    EXPECT_NE(d2.id & ECS_QUERY_INDEX_MASK, slot + 1u);

    /* Many queries; the rest are destroyed with the world (teardown checks for leaks). */
    for (u32 i = 0; i < 200; ++i)
        ASSERT_TRUE(DE_HANDLE_VALID(de_query_create(f->w, &d)));
}

/* ---- query.cache_new_archetype --------------------------------------------------------------- */

TEST_F(query, cache_new_archetype, ecs_fx) {
    /* Queries made before the archetypes exist pick each new one up on the next iteration, and
     * each new archetype costs exactly one match test per live query. */
    const de_query_term t_pos[] = {{f->pos, DE_READ}};
    const de_query_term t_vel[] = {{f->vel, DE_WRITE}, {f->rot, DE_OPTIONAL_READ}};
    const de_query_filter no_enemy[] = {{f->enemy, DE_WITHOUT}};
    const de_query_desc descs[] = {
        {t_pos, 1, NULL, 0}, {t_vel, 2, NULL, 0}, {t_pos, 1, no_enemy, 1}};
    de_query qs[3];
    u64 tests0[3];
    for (u32 i = 0; i < 3; ++i) {
        qs[i] = de_query_create(f->w, &descs[i]);
        ASSERT_TRUE(DE_HANDLE_VALID(qs[i]));
        const ecs_query *q = ecs_query_get(f->w, qs[i]);
        EXPECT_EQ(q->scanned, ecs_archetype_count(f->w));
        EXPECT_EQ(q->match_tests, (u64)ecs_archetype_count(f->w)); /* initial match */
        tests0[i] = q->match_tests;
        EXPECT_EQ(de_query_count(f->w, qs[i]), 0u);
    }

    /* Make every one of the 31 non-empty subsets of the 5 components, one at a time. */
    const de_component comps[5] = {f->pos, f->vel, f->health, f->enemy, f->rot};
    u32 created = 0;
    u32 expect[3] = {0, 0, 0};
    for (u32 mask = 1; mask < 32u; ++mask) {
        de_component set[5];
        u32 n = 0;
        for (u32 i = 0; i < 5; ++i)
            if (mask & (1u << i))
                set[n++] = comps[i];
        const u32 before = ecs_archetype_count(f->w);
        de_entity e = DE_ENTITY_NULL;
        ASSERT_EQ(de_entity_spawn_with(f->w, set, n, &e), DE_OK);
        ASSERT_EQ(ecs_archetype_count(f->w), before + 1u);
        ++created;
        const bool has_pos = (mask & 1u) != 0, has_vel = (mask & 2u) != 0;
        const bool has_enemy = (mask & 8u) != 0;
        expect[0] += has_pos ? 1u : 0u;
        expect[1] += has_vel ? 1u : 0u;
        expect[2] += (has_pos && !has_enemy) ? 1u : 0u;
        for (u32 i = 0; i < 3; ++i) {
            const ecs_query *q = ecs_query_get(f->w, qs[i]);
            EXPECT_EQ(q->match_tests, tests0[i] + created); /* O(new archetypes) */
            EXPECT_EQ(q->scanned, ecs_archetype_count(f->w));
            EXPECT_EQ(de_query_count(f->w, qs[i]), expect[i]);
            EXPECT_EQ(ecs_query_entry_count(q), expect[i]);
        }
        /* The new entity is in the next iteration. */
        collect_reset(&g_col, f->w, t_pos, 1);
        de_query_each(f->w, qs[0], collect_fn, &g_col);
        EXPECT_EQ(g_col.seen[de_entity_index(e)], has_pos ? 1u : 0u);
        EXPECT_TRUE(g_col.order_ok && g_col.cols_ok);
    }
    /* Entries are in archetype creation order. */
    for (u32 i = 0; i < 3; ++i) {
        const ecs_query *q = ecs_query_get(f->w, qs[i]);
        for (u32 k = 1; k < ecs_query_entry_count(q); ++k)
            EXPECT_LT(ecs_query_entry(q, k - 1u)[0], ecs_query_entry(q, k)[0]);
    }
    /* Structural changes that create no archetype run no match test. */
    const ecs_query *q0 = ecs_query_get(f->w, qs[0]);
    const u64 t = q0->match_tests;
    for (u32 i = 0; i < 100; ++i) {
        const de_entity e = de_entity_spawn(f->w);
        de_entity_add(f->w, e, f->pos);
        de_entity_add(f->w, e, f->vel);
        de_entity_remove(f->w, e, f->pos);
        de_entity_despawn(f->w, e);
    }
    EXPECT_EQ(q0->match_tests, t);
}

/* ---- query.cache_oom_fallback ---------------------------------------------------------------- */

typedef struct par_rows {
    u32 rows;
    u32 seen[64];
} par_rows;

static void count_rows_fn(const de_query_chunk *ch, void *user) {
    par_rows *p = (par_rows *)user;
    p->rows += ch->count;
    for (u32 i = 0; i < ch->count; ++i)
        if (de_entity_index(ch->entities[i]) < 64u)
            ++p->seen[de_entity_index(ch->entities[i])];
}

TEST_F(query, cache_oom_fallback, ecs_fx) {
    /* When the listener cannot store a new match (out of memory), the query still sees the new
     * archetype: the iterator matches the unscanned archetypes on the fly, and the next
     * archetype creation catches the cache up. */
    const de_query_term t[] = {{f->pos, DE_READ}};
    const de_query_desc d = {t, 1, NULL, 0};
    const de_query q = de_query_create(f->w, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(q));
    const ecs_query *iq = ecs_query_get(f->w, q);
    const de_component s1[] = {f->pos};
    de_entity e1 = DE_ENTITY_NULL;
    ASSERT_EQ(de_entity_spawn_with(f->w, s1, 1, &e1), DE_OK);
    /* Fill the entry array to its capacity with more {pos, ...} archetypes, so that the next
     * match has to grow it. */
    const de_component more[] = {f->pos, f->vel, f->health, f->enemy, f->rot};
    for (u32 mask = 1; iq->entries.count < iq->entries.capacity && mask < 16u; ++mask) {
        de_component set[5] = {f->pos};
        u32 n = 1;
        for (u32 i = 0; i < 4; ++i)
            if (mask & (1u << i))
                set[n++] = more[1u + i];
        if (mask == 0xBu)
            continue; /* {pos, vel, health, rot}: made below */
        de_entity x = DE_ENTITY_NULL;
        ASSERT_EQ(de_entity_spawn_with(f->w, set, n, &x), DE_OK);
        de_entity_despawn(f->w, x);
    }
    ASSERT_EQ(iq->entries.count, iq->entries.capacity);
    const u32 entries = ecs_query_entry_count(iq);

    /* Find an allocation budget where the archetype is created but the query entry is not. */
    bool degraded = false;
    de_entity e2 = DE_ENTITY_NULL;
    for (i64 n = 0; n < 32 && !degraded; ++n) {
        const u32 arch_before = ecs_archetype_count(f->w);
        de_tracking_allocator_fail_after(&f->track, n);
        const de_component s2[] = {f->pos, f->vel, f->health, f->rot};
        de_entity e = DE_ENTITY_NULL;
        const de_result r = de_entity_spawn_with(f->w, s2, 4, &e);
        de_tracking_allocator_fail_after(&f->track, -1);
        if (ecs_archetype_count(f->w) > arch_before && iq->scanned < ecs_archetype_count(f->w))
            degraded = true;
        if (r == DE_OK) {
            e2 = e;
            if (!degraded)
                break; /* everything succeeded: no budget gives the degraded state */
        }
    }
    ASSERT_TRUE(degraded);
    if (!de_entity_alive(f->w, e2)) {
        const de_component s2[] = {f->pos, f->vel, f->health, f->rot};
        ASSERT_EQ(de_entity_spawn_with(f->w, s2, 4, &e2), DE_OK);
    }
    EXPECT_EQ(ecs_query_entry_count(iq), entries); /* the match was not stored */

    /* each, count and par_each still cover the new archetype. */
    par_rows p;
    memset(&p, 0, sizeof p);
    de_query_each(f->w, q, count_rows_fn, &p);
    EXPECT_EQ(p.rows, 2u);
    EXPECT_EQ(p.seen[de_entity_index(e2)], 1u);
    EXPECT_EQ(de_query_count(f->w, q), 2u);
    for (u32 grain = 1; grain <= 3; ++grain) {
        memset(&p, 0, sizeof p);
        de_query_par_each(f->w, q, grain, count_rows_fn, &p);
        EXPECT_EQ(p.rows, 2u);
        EXPECT_EQ(p.seen[de_entity_index(e1)], 1u);
        EXPECT_EQ(p.seen[de_entity_index(e2)], 1u);
    }

    /* The next archetype creation catches the cache up. */
    const de_component s3[] = {f->pos, f->enemy};
    de_entity e3 = DE_ENTITY_NULL;
    ASSERT_EQ(de_entity_spawn_with(f->w, s3, 2, &e3), DE_OK);
    EXPECT_EQ(iq->scanned, ecs_archetype_count(f->w));
    EXPECT_EQ(de_query_count(f->w, q), 3u);
    EXPECT_GE(ecs_query_entry_count(iq), 3u);
}

/* ---- query.create_oom ------------------------------------------------------------------------ */

TEST_F(query, create_oom, ecs_fx) {
    /* Each allocation of create fails in turn: create returns an invalid handle and nothing
     * leaks (teardown); then it succeeds. */
    populate8(f, &g_pop);
    const de_query_term t[] = {{f->pos, DE_READ}, {f->vel, DE_OPTIONAL_WRITE}};
    const de_query_desc d = {t, 2, NULL, 0};
    bool created = false;
    for (i64 n = 0; n < 64 && !created; ++n) {
        ecs_fx_destroy_world(f);
        ecs_fx_create_world(f, NULL);
        populate8(f, &g_pop);
        de_tracking_allocator_fail_after(&f->track, n);
        const de_query q = de_query_create(f->w, &d);
        de_tracking_allocator_fail_after(&f->track, -1);
        if (DE_HANDLE_VALID(q)) {
            created = true;
            EXPECT_EQ(de_query_count(f->w, q), 37u + 120u + 5u + 64u + 200u);
        } else {
            EXPECT_EQ(de_error_last()->result, DE_ERR_OUT_OF_MEMORY);
        }
    }
    EXPECT_TRUE(created);
}

/* ---- query.chunk_contiguous ------------------------------------------------------------------ */

typedef struct contig {
    de_world *w;
    de_component pos, vel, rot;
    u32 rows;
    u32 slices;
    u32 max_count;
    bool ok;
} contig;

static void contig_fn(const de_query_chunk *ch, void *user) {
    contig *c = (contig *)user;
    ++c->slices;
    c->rows += ch->count;
    if (ch->count > c->max_count)
        c->max_count = ch->count;
    const ecs_record *r0 = ecs_entity_record(c->w, ch->entities[0]);
    if (r0 == NULL) {
        c->ok = false;
        return;
    }
    /* Columns are aligned to their component (TRot: 16) and are SoA arrays of the slice. */
    if (((uintptr_t)ch->columns[0] % DE_ALIGNOF(TPos)) != 0 ||
        ((uintptr_t)ch->columns[1] % DE_ALIGNOF(TVel)) != 0 ||
        ((uintptr_t)ch->columns[2] % 16u) != 0)
        c->ok = false;
    for (u32 i = 0; i < ch->count; ++i) {
        const de_entity e = ch->entities[i];
        const ecs_record *r = ecs_entity_record(c->w, e);
        /* One chunk, consecutive rows. */
        if (r == NULL || r->archetype != r0->archetype || r->chunk != r0->chunk ||
            r->row != r0->row + i) {
            c->ok = false;
            continue;
        }
        if ((void *)((TPos *)ch->columns[0] + i) != de_entity_get(c->w, e, c->pos) ||
            (void *)((TVel *)ch->columns[1] + i) != de_entity_get(c->w, e, c->vel) ||
            (void *)((TRot *)ch->columns[2] + i) != de_entity_get(c->w, e, c->rot))
            c->ok = false;
    }
}

TEST_F(query, chunk_contiguous, ecs_fx) {
    recreate_world(f, 1024);
    const de_component set[] = {f->pos, f->vel, f->rot};
    for (u32 i = 0; i < 1000; ++i) {
        de_entity e = DE_ENTITY_NULL;
        ASSERT_EQ(de_entity_spawn_with(f->w, set, 3, &e), DE_OK);
    }
    const ecs_archetype *a = ecs_archetype_at(f->w, ecs_archetype_count(f->w) - 1u);
    ASSERT_GT(a->chunks.count, 10u);
    const de_query_term t[] = {{f->pos, DE_READ}, {f->vel, DE_WRITE}, {f->rot, DE_READ}};
    const de_query_desc d = {t, 3, NULL, 0};
    const de_query q = de_query_create(f->w, &d);
    contig c;
    memset(&c, 0, sizeof c);
    c.w = f->w;
    c.pos = f->pos;
    c.vel = f->vel;
    c.rot = f->rot;
    c.ok = true;
    de_query_each(f->w, q, contig_fn, &c);
    EXPECT_TRUE(c.ok);
    EXPECT_EQ(c.rows, 1000u);
    EXPECT_EQ(c.slices, a->chunks.count); /* one slice per chunk */
    EXPECT_EQ(c.max_count, a->rows_per_chunk);

    /* par_each slices are contiguous too, also when a range ends inside a chunk. */
    const u32 grains[] = {1, 7, a->rows_per_chunk, a->rows_per_chunk + 3u, 0};
    for (u32 g = 0; g < 5; ++g) {
        memset(&c, 0, sizeof c);
        c.w = f->w;
        c.pos = f->pos;
        c.vel = f->vel;
        c.rot = f->rot;
        c.ok = true;
        de_query_par_each(f->w, q, grains[g], contig_fn, &c);
        EXPECT_TRUE(c.ok);
        EXPECT_EQ(c.rows, 1000u);
    }
}

/* ---- query.write_through --------------------------------------------------------------------- */

static void write_fn(const de_query_chunk *ch, void *user) {
    (void)user;
    const TPos *p = (const TPos *)ch->columns[0];
    TVel *v = (TVel *)ch->columns[1];
    THealth *h = (THealth *)ch->columns[2]; /* optional */
    for (u32 i = 0; i < ch->count; ++i) {
        v[i].v = (de_vec3){p[i].v.x + 1.0f, p[i].v.y * 2.0f, -p[i].v.z};
        if (h != NULL)
            h[i].max = p[i].v.x + 0.5f;
    }
}

TEST_F(query, write_through, ecs_fx) {
    recreate_world(f, 1024);
    populate8(f, &g_pop);
    const de_query_term t[] = {
        {f->pos, DE_READ}, {f->vel, DE_WRITE}, {f->health, DE_OPTIONAL_WRITE}};
    const de_query_desc d = {t, 3, NULL, 0};
    const de_query q = de_query_create(f->w, &d);
    de_query_each(f->w, q, write_fn, NULL);
    u32 checked = 0;
    for (u32 n = 0; n < g_pop.n; ++n) {
        const de_entity e = g_pop.e[n];
        const TPos *p = (const TPos *)de_entity_get_const(f->w, e, f->pos);
        const TVel *v = (const TVel *)de_entity_get_const(f->w, e, f->vel);
        if (p == NULL || v == NULL)
            continue;
        ++checked;
        EXPECT_FLOAT_EQ(v->v.x, p->v.x + 1.0f, 0.0);
        EXPECT_FLOAT_EQ(v->v.y, p->v.y * 2.0f, 0.0);
        EXPECT_FLOAT_EQ(v->v.z, -p->v.z, 0.0);
        const THealth *h = (const THealth *)de_entity_get_const(f->w, e, f->health);
        if (h != NULL)
            EXPECT_FLOAT_EQ(h->max, p->v.x + 0.5f, 0.0);
    }
    EXPECT_EQ(checked, 120u + 5u + 200u);
}

/* ---- query.empty_archetypes_skipped ---------------------------------------------------------- */

static void count_slices_fn(const de_query_chunk *ch, void *user) {
    u32 *n = (u32 *)user;
    EXPECT_GT(ch->count, 0u);
    n[0] += 1u;
    n[1] += ch->count;
}

TEST_F(query, empty_archetypes_skipped, ecs_fx) {
    /* Archetypes that had rows and lost them, and archetypes that never had any, are matched
     * but produce no slice. */
    const de_query_term t[] = {{f->pos, DE_READ}};
    const de_query_desc d = {t, 1, NULL, 0};
    const de_query q = de_query_create(f->w, &d);
    const de_component s1[] = {f->pos, f->vel};
    const de_component s2[] = {f->pos, f->health};
    const de_component s3[] = {f->pos, f->rot};
    de_entity e1 = DE_ENTITY_NULL, e2 = DE_ENTITY_NULL, e3 = DE_ENTITY_NULL;
    ASSERT_EQ(de_entity_spawn_with(f->w, s1, 2, &e1), DE_OK);
    ASSERT_EQ(de_entity_spawn_with(f->w, s2, 2, &e2), DE_OK);
    ASSERT_EQ(de_entity_spawn_with(f->w, s3, 2, &e3), DE_OK);
    de_entity_despawn(f->w, e1);
    de_entity_despawn(f->w, e3);
    EXPECT_EQ(ecs_query_entry_count(ecs_query_get(f->w, q)), 3u);
    u32 n[2] = {0, 0};
    de_query_each(f->w, q, count_slices_fn, n);
    EXPECT_EQ(n[0], 1u);
    EXPECT_EQ(n[1], 1u);
    EXPECT_EQ(de_query_count(f->w, q), 1u);
    for (u32 grain = 1; grain < 4; ++grain) {
        n[0] = n[1] = 0;
        de_query_par_each(f->w, q, grain, count_slices_fn, n);
        EXPECT_EQ(n[0], 1u);
        EXPECT_EQ(n[1], 1u);
    }
    /* All empty: no call at all. */
    de_entity_despawn(f->w, e2);
    n[0] = n[1] = 0;
    de_query_each(f->w, q, count_slices_fn, n);
    de_query_par_each(f->w, q, 0, count_slices_fn, n);
    EXPECT_EQ(n[0], 0u);
    EXPECT_EQ(de_query_count(f->w, q), 0u);
}

/* ---- query.tags_and_optional ----------------------------------------------------------------- */

typedef struct tag_check {
    de_world *w;
    u32 rows, with_rot, ok;
} tag_check;

static void tag_fn(const de_query_chunk *ch, void *user) {
    tag_check *c = (tag_check *)user;
    c->rows += ch->count;
    /* columns[0]: tag -> the non-NULL placeholder; columns[1]: optional rot. */
    if (ch->columns[0] == NULL ||
        ch->columns[0] != de_entity_get(c->w, ch->entities[0], (de_component){4}))
        c->ok = 0;
    if (ch->columns[1] != NULL)
        c->with_rot += ch->count;
}

TEST_F(query, tags_and_optional, ecs_fx) {
    populate8(f, &g_pop);
    const de_query_term t[] = {{f->enemy, DE_READ}, {f->rot, DE_OPTIONAL_READ}};
    const de_query_desc d = {t, 2, NULL, 0};
    const de_query q = de_query_create(f->w, &d);
    tag_check c = {f->w, 0, 0, 1};
    de_query_each(f->w, q, tag_fn, &c);
    EXPECT_EQ(c.ok, 1u);
    EXPECT_EQ(c.rows, 64u + 200u);
    EXPECT_EQ(c.with_rot, 200u);
}

/* ---- query.change_filter_runs ---------------------------------------------------------------- */

/* The DENG-347 predicate is not built yet; this checks the run splitting with a stand-in that
 * passes rows whose TPos.x is even. */
static bool even_x_filter(const ecs_query *q, const ecs_archetype *a, const ecs_chunk *ch,
                          const u32 *filter_cols, u32 row) {
    (void)q;
    const TPos *p = (const TPos *)ecs_chunk_column(a, ch, filter_cols[0]);
    return ((u32)p[row].v.x % 2u) == 0u;
}

typedef struct runs {
    de_world *w;
    u32 rows, slices;
    bool ok;
    u32 seen[POP_MAX * 2u];
} runs;

static runs g_runs;

static void runs_fn(const de_query_chunk *ch, void *user) {
    runs *r = (runs *)user;
    ++r->slices;
    const TPos *p = (const TPos *)ch->columns[0];
    const ecs_record *r0 = ecs_entity_record(r->w, ch->entities[0]);
    for (u32 i = 0; i < ch->count; ++i) {
        ++r->seen[de_entity_index(ch->entities[i])];
        ++r->rows;
        const ecs_record *ri = ecs_entity_record(r->w, ch->entities[i]);
        if (((u32)p[i].v.x % 2u) != 0u || ri->chunk != r0->chunk || ri->row != r0->row + i)
            r->ok = false;
    }
}

TEST_F(query, change_filter_runs, ecs_fx) {
    recreate_world(f, 1024);
    populate8(f, &g_pop);
    /* Give runs of different lengths: x even for 3 rows, odd for 2, ... */
    for (u32 n = 0; n < g_pop.n; ++n) {
        TPos *p = (TPos *)de_entity_get(f->w, g_pop.e[n], f->pos);
        if (p != NULL)
            p->v.x = (f32)((n % 5u) < 3u ? 2u * n : 2u * n + 1u);
    }
    const de_query_term t[] = {{f->pos, DE_READ}};
    const de_query_filter fl[] = {{f->pos, DE_CHANGED}};
    const de_query_desc d = {t, 1, fl, 1};
    const de_query q = de_query_create(f->w, &d);
    ecs_query *iq = ecs_query_get(f->w, q);
    ASSERT_TRUE(iq != NULL);
    EXPECT_TRUE(iq->change_filter == ecs_change_filter_default);

    /* The default (until DENG-347) passes every row. */
    memset(&g_runs, 0, sizeof g_runs);
    g_runs.w = f->w;
    g_runs.ok = true;
    collect_reset(&g_col, f->w, t, 1);
    de_query_each(f->w, q, collect_fn, &g_col);
    EXPECT_EQ(g_col.rows, 37u + 120u + 5u + 64u + 200u);

    ecs_query_set_change_window(f->w, q, 3u, 9u);
    EXPECT_EQ(iq->last_run_tick, 3u);
    EXPECT_EQ(iq->this_run_tick, 9u);

    iq->change_filter = even_x_filter;
    de_query_each(f->w, q, runs_fn, &g_runs);
    u32 expected = 0;
    for (u32 n = 0; n < g_pop.n; ++n) {
        const TPos *p = (const TPos *)de_entity_get_const(f->w, g_pop.e[n], f->pos);
        const bool pass = p != NULL && ((u32)p->v.x % 2u) == 0u;
        expected += pass ? 1u : 0u;
        EXPECT_EQ(g_runs.seen[de_entity_index(g_pop.e[n])], pass ? 1u : 0u);
    }
    EXPECT_TRUE(g_runs.ok);
    EXPECT_EQ(g_runs.rows, expected);
    EXPECT_GT(g_runs.slices, 50u); /* runs, not whole chunks */
    /* Counting honors the same row predicate as serial and parallel iteration. */
    EXPECT_EQ(de_query_count(f->w, q), expected);

    /* par_each delivers the same rows. */
    for (u32 grain = 1; grain < 40; grain += 13) {
        memset(&g_runs, 0, sizeof g_runs);
        g_runs.w = f->w;
        g_runs.ok = true;
        de_query_par_each(f->w, q, grain, runs_fn, &g_runs);
        EXPECT_TRUE(g_runs.ok);
        EXPECT_EQ(g_runs.rows, expected);
    }
}

/* ---- query.no_alloc_iter --------------------------------------------------------------------- */

static void sum_fn(const de_query_chunk *ch, void *user) {
    f32 *s = (f32 *)user;
    const TPos *p = (const TPos *)ch->columns[0];
    for (u32 i = 0; i < ch->count; ++i)
        *s += p[i].v.x;
}

TEST_F(query, no_alloc_iter, ecs_fx) {
    /* Iteration allocates nothing: de_query_each, de_query_count and de_query_par_each (with
     * and without a task system), counted by the world's tracking allocator. */
    de_fp_env_init_thread();
    de_task_system_desc td = {0};
    td.worker_count = 2;
    td.background_worker_count = 1;
    de_task_system *ts = NULL;
    ASSERT_EQ(de_task_system_create(&td, &ts), DE_OK);
    for (u32 pass = 0; pass < 2; ++pass) {
        ecs_fx_destroy_world(f);
        de_world_desc d;
        memset(&d, 0, sizeof d);
        d.chunk_bytes = 1024;
        d.tasks = pass == 0 ? NULL : ts;
        ecs_fx_create_world(f, &d);
        populate8(f, &g_pop);
        const de_query_term t[] = {{f->pos, DE_READ}, {f->vel, DE_OPTIONAL_READ}};
        const de_query_filter fl[] = {{f->rot, DE_WITHOUT}};
        const de_query_desc qd = {t, 2, fl, 1};
        const de_query q = de_query_create(f->w, &qd);
        const de_query_term t2[] = {{f->pos, DE_READ}};
        const de_query_filter fl2[] = {{f->pos, DE_CHANGED}};
        const de_query_desc qd2 = {t2, 1, fl2, 1};
        const de_query q2 = de_query_create(f->w, &qd2);
        ASSERT_TRUE(DE_HANDLE_VALID(q) && DE_HANDLE_VALID(q2));
        const de_tracking_stats before = de_tracking_allocator_stats(&f->track);
        f32 sum = 0.0f;
        for (u32 i = 0; i < 100; ++i) {
            de_query_each(f->w, q, sum_fn, &sum);
            de_query_each(f->w, q2, sum_fn, &sum);
            EXPECT_EQ(de_query_count(f->w, q), 37u + 120u + 5u + 64u);
            if (pass == 0) {
                de_query_par_each(f->w, q, 16, sum_fn, &sum); /* serial: no task system */
                de_query_par_each(f->w, q2, 0, sum_fn, &sum);
            }
        }
        const de_tracking_stats after = de_tracking_allocator_stats(&f->track);
        EXPECT_EQ(after.alloc_count, before.alloc_count);
        EXPECT_EQ(after.realloc_count, before.realloc_count);
        EXPECT_EQ(after.free_count, before.free_count);
        EXPECT_TRUE(sum > 0.0f);
    }
    ecs_fx_destroy_world(f);
    de_task_system_destroy(ts);
}

/* ---- query.count ----------------------------------------------------------------------------- */

TEST_F(query, count, ecs_fx) {
    populate8(f, &g_pop);
    const de_query_term t_pos[] = {{f->pos, DE_READ}};
    const de_query_term t_health[] = {{f->health, DE_READ}};
    const de_query_filter no_vel[] = {{f->vel, DE_WITHOUT}};
    const de_query_filter with_enemy[] = {{f->enemy, DE_WITH}};
    const de_query_desc d1 = {t_pos, 1, NULL, 0};
    const de_query_desc d2 = {t_pos, 1, no_vel, 1};
    const de_query_desc d3 = {t_health, 1, with_enemy, 1};
    const de_query_desc d4 = {NULL, 0, NULL, 0};
    const de_query q1 = de_query_create(f->w, &d1);
    const de_query q2 = de_query_create(f->w, &d2);
    const de_query q3 = de_query_create(f->w, &d3);
    const de_query q4 = de_query_create(f->w, &d4);
    EXPECT_EQ(de_query_count(f->w, q1), 37u + 120u + 5u + 64u + 200u);
    EXPECT_EQ(de_query_count(f->w, q2), 37u + 64u);
    EXPECT_EQ(de_query_count(f->w, q3), 0u); /* {pos, health, enemy} is empty */
    EXPECT_EQ(de_query_count(f->w, q4), g_pop.n);
    /* Counts follow spawns and despawns without new archetypes. */
    const de_component s[] = {f->pos, f->health, f->enemy};
    de_entity e = DE_ENTITY_NULL;
    ASSERT_EQ(de_entity_spawn_with(f->w, s, 3, &e), DE_OK);
    EXPECT_EQ(de_query_count(f->w, q3), 1u);
    EXPECT_EQ(de_query_count(f->w, q1), 37u + 120u + 5u + 64u + 200u + 1u);
    de_entity_despawn(f->w, g_pop.e[0]);                      /* in {pos} */
    EXPECT_EQ(de_query_count(f->w, q2), 37u + 64u + 1u - 1u); /* + e, - e[0] */
    EXPECT_EQ(de_query_count(f->w, q4), g_pop.n);
}
