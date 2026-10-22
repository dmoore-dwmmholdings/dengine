/* test_archetype.c: archetype sets, creation order, chunk layout and row moves (DENG-344). */
#include "ecs_test_util.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Every row of every table points back at itself through its entity's record, and the rows add
 * up to `alive` entities. */
static void check_records(de_world *w, u32 alive) {
    u32 rows = 0;
    for (u32 ai = 0; ai < ecs_archetype_count(w); ++ai) {
        const ecs_archetype *a = ecs_archetype_at(w, ai);
        u32 in_table = 0;
        for (u32 c = 0; c < a->chunks.count; ++c) {
            const ecs_chunk *ch = ecs_table_chunk(a, c);
            if (c + 1u < a->chunks.count)
                ASSERT_EQ(ch->count, a->rows_per_chunk); /* dense */
            ASSERT_GT(ch->count, 0u);
            for (u32 r = 0; r < ch->count; ++r) {
                const ecs_record *rec = ecs_entity_record(w, ecs_chunk_entities(ch)[r]);
                ASSERT_TRUE(rec != NULL);
                ASSERT_EQ(rec->archetype, ai);
                ASSERT_EQ(rec->chunk, c);
                ASSERT_EQ(rec->row, r);
            }
            in_table += ch->count;
        }
        ASSERT_EQ(in_table, a->count);
        rows += in_table;
    }
    ASSERT_EQ(rows, alive);
}

/* ---- archetype.set_canonical ----------------------------------------------------------------- */

TEST_F(archetype, optional_components_collide_move_and_restore_without_false_matches, ecs_fx) {
    static const de_field_info fields[] = {DE_FIELD(TPos, v, DE_FK_VEC3, DE_FIELD_DEFAULT)};
    de_component components[132];
    bool present[132] = {false}, saved[132] = {false};
    for (u32 i = 0; i < 132; ++i) {
        char name[32];
        snprintf(name, sizeof name, "OptionalComponent%u", i);
        de_type_desc desc = DE_TYPE_DESC(TPos, 1, fields);
        desc.name = name;
        de_type_id type = de_type_register(f->types, &desc);
        ASSERT_TRUE(type.id != 0);
        components[i] =
            de_component_register(f->w, type, DE_COMP_SNAPSHOT | (i % 2 ? DE_COMP_TAG : 0));
        ASSERT_TRUE(components[i].id != 0);
    }
    de_entity entity = de_entity_spawn(f->w);
    de_world_snapshot snap = {0};
    u64 checksum = 0;
    for (u32 phase = 0; phase < 6; ++phase) {
        if (phase == 1)
            for (u32 i = 0; i < 64; ++i)
                if ((components[i].id & 63u) == 63u || (components[i].id & 63u) == 0) {
                    ASSERT_TRUE(de_entity_add(f->w, entity, components[i]) != NULL);
                    present[i] = true;
                }
        if (phase == 1 || phase == 2 || phase == 4) {
            u32 first = phase == 1 ? 0 : phase == 2 ? 64 : 128;
            for (u32 i = first; i < first + 2; ++i) {
                ASSERT_TRUE(de_entity_add(f->w, entity, components[i]) != NULL);
                present[i] = true;
            }
        }
        if (phase == 3 || phase == 4) {
            u32 first = phase == 3 ? 0 : 64;
            for (u32 i = first; i < first + 2; ++i) {
                de_entity_remove(f->w, entity, components[i]);
                present[i] = false;
            }
        }
        if (phase == 2) {
            memcpy(saved, present, sizeof saved);
            checksum = de_world_checksum(f->w, DE_COMP_SNAPSHOT);
            ASSERT_EQ(de_world_snapshot_take(f->w, NULL, &snap), DE_OK);
        }
        if (phase == 5) {
            ASSERT_EQ(de_world_snapshot_restore(f->w, &snap), DE_OK);
            memcpy(present, saved, sizeof present);
            EXPECT_EQ(de_world_checksum(f->w, DE_COMP_SNAPSHOT), checksum);
        }
        de_tracking_stats before = de_tracking_allocator_stats(&f->track);
        for (u32 i = 0; i < 132; ++i) {
            EXPECT_EQ(de_entity_has(f->w, entity, components[i]), present[i]);
            EXPECT_EQ(de_entity_get_const(f->w, entity, components[i]) != NULL, present[i]);
        }
        EXPECT_EQ(de_tracking_allocator_stats(&f->track).alloc_count, before.alloc_count);
    }
    de_world_snapshot_free(NULL, &snap);
    de_entity_despawn(f->w, entity);
    de_entity replacement = de_entity_spawn(f->w);
    ASSERT_TRUE(entity.bits != replacement.bits);
    for (u32 i = 0; i < 132; ++i) {
        EXPECT_FALSE(de_entity_has(f->w, entity, components[i]));
        EXPECT_TRUE(de_entity_get_const(f->w, entity, components[i]) == NULL);
        EXPECT_FALSE(de_entity_has(f->w, replacement, components[i]));
    }
}

TEST_F(archetype, set_canonical, ecs_fx) {
    EXPECT_EQ(ecs_archetype_count(f->w), 1u);
    ecs_archetype *empty = NULL;
    ASSERT_EQ(ecs_archetype_get_or_create(f->w, NULL, 0, &empty), DE_OK);
    EXPECT_EQ(empty->index, ECS_EMPTY_ARCHETYPE);
    EXPECT_EQ(empty->component_count, 0u);

    const u32 a_ids[] = {f->vel.id, f->pos.id};
    const u32 b_ids[] = {f->pos.id, f->vel.id, f->pos.id, f->vel.id};
    const u32 c_ids[] = {f->pos.id, f->vel.id};
    ecs_archetype *a = NULL, *b = NULL, *c = NULL;
    ASSERT_EQ(ecs_archetype_get_or_create(f->w, a_ids, 2, &a), DE_OK);
    ASSERT_EQ(ecs_archetype_get_or_create(f->w, b_ids, 4, &b), DE_OK);
    ASSERT_EQ(ecs_archetype_get_or_create(f->w, c_ids, 2, &c), DE_OK);
    EXPECT_TRUE(a == b && b == c);
    EXPECT_EQ(ecs_archetype_count(f->w), 2u);
    ASSERT_EQ(a->component_count, 2u);
    EXPECT_EQ(a->components[0], f->pos.id); /* sorted ascending */
    EXPECT_EQ(a->components[1], f->vel.id);
    EXPECT_TRUE(ecs_archetype_find(f->w, c_ids, 2) == a);
    EXPECT_TRUE(ecs_archetype_find(f->w, c_ids, 1) == NULL);
    EXPECT_TRUE(ecs_archetype_find(f->w, NULL, 0) == empty);

    /* A subset and a superset are different archetypes. */
    const u32 d_ids[] = {f->rot.id, f->vel.id, f->pos.id};
    ecs_archetype *d = NULL, *e = NULL;
    ASSERT_EQ(ecs_archetype_get_or_create(f->w, d_ids, 3, &d), DE_OK);
    ASSERT_EQ(ecs_archetype_get_or_create(f->w, d_ids + 2, 1, &e), DE_OK);
    EXPECT_TRUE(d != a && e != a && d != e);
    EXPECT_EQ(ecs_archetype_count(f->w), 4u);
    EXPECT_TRUE(ecs_archetype_has(d, f->rot.id));
    EXPECT_FALSE(ecs_archetype_has(a, f->rot.id));
    EXPECT_EQ(ecs_archetype_find_component(d, f->rot.id), 2u);
    EXPECT_EQ(ecs_archetype_find_component(d, f->health.id), ECS_INVALID_INDEX);

    /* Unregistered ids are refused and create nothing. */
    const u32 bad[] = {f->pos.id, 77};
    ecs_archetype *x = NULL;
    EXPECT_EQ(ecs_archetype_get_or_create(f->w, bad, 2, &x), DE_ERR_INVALID_ARGUMENT);
    const u32 zero[] = {0};
    EXPECT_EQ(ecs_archetype_get_or_create(f->w, zero, 1, &x), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecs_archetype_get_or_create(f->w, NULL, 1, &x), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecs_archetype_count(f->w), 4u);

    /* Moving an entity to the same set in another spelling keeps it where it is. */
    const de_entity en = de_entity_spawn(f->w);
    ecs_test_set(f->w, en, a_ids, 2);
    const ecs_record before = *ecs_entity_record(f->w, en);
    ecs_test_set(f->w, en, b_ids, 4);
    const ecs_record after = *ecs_entity_record(f->w, en);
    EXPECT_EQ(before.archetype, after.archetype);
    EXPECT_EQ(before.row, after.row);
    EXPECT_EQ(before.chunk, after.chunk);
}

/* ---- archetype.creation_order ---------------------------------------------------------------- */

/* Archetype creation listener: records the indices it sees. */
typedef struct listener_log {
    u32 seen[64];
    u32 count;
} listener_log;

static void on_created(de_world *w, ecs_archetype *a, void *user) {
    listener_log *log = (listener_log *)user;
    /* The archetype is already in the array and the lookup map. */
    EXPECT_TRUE(ecs_archetype_at(w, a->index) == a);
    EXPECT_TRUE(ecs_archetype_find(w, a->components, a->component_count) == a);
    if (log->count < 64)
        log->seen[log->count++] = a->index;
}

#define ORDER_STEPS   4000u
#define ORDER_OUT_MAX 4096u

typedef struct order_result {
    u32 words[ORDER_OUT_MAX]; /* per archetype: count, then the ids */
    u32 word_count;
    u32 archetypes;
    u32 first_seen[32]; /* component masks in first-use order */
    u32 first_seen_count;
    u64 entity_hash; /* final entity ids and their archetype indices */
    listener_log log;
} order_result;

/* A fixed operation sequence: spawn, move to a pseudo-random component set, despawn. Integer
 * LCG only, so it is the same on every platform. */
static void run_order_sequence(ecs_fx *f, de_task_system *ts, order_result *out) {
    memset(out, 0, sizeof *out);
    de_world_desc d;
    memset(&d, 0, sizeof d);
    d.types = f->types;
    d.tasks = ts;
    d.chunk_bytes = 2048; /* small chunks: many chunk boundaries */
    de_world *w = NULL;
    ASSERT_EQ(de_world_create(&d, &w), DE_OK);
    const u32 comps[5] = {de_component_register(w, f->t_pos, 0).id,
                          de_component_register(w, f->t_vel, 0).id,
                          de_component_register(w, f->t_health, 0).id,
                          de_component_register(w, f->t_enemy, DE_COMP_TAG).id,
                          de_component_register(w, f->t_rot, 0).id};
    ASSERT_EQ(ecs_on_archetype_created(w, on_created, &out->log), DE_OK);
    bool seen_mask[32] = {false};
    seen_mask[0] = true;
    out->first_seen[out->first_seen_count++] = 0;

    de_entity live[512];
    u32 live_count = 0;
    u32 state = 12345u;
    for (u32 step = 0; step < ORDER_STEPS; ++step) {
        state = state * 1664525u + 1013904223u;
        const u32 op = (state >> 24) % 8u;
        const u32 pick = (state >> 8) & 0xFFFFu;
        if (live_count == 0 || (op < 3 && live_count < 512)) {
            live[live_count++] = de_entity_spawn(w);
        } else if (op == 7) {
            const u32 i = pick % live_count;
            ecs_test_despawn(w, live[i]);
            live[i] = live[--live_count];
        } else {
            const u32 mask = (state >> 3) & 31u;
            u32 ids[5];
            u32 n = 0;
            for (u32 b = 0; b < 5; ++b)
                if (mask & (1u << b))
                    ids[n++] = comps[4u - b]; /* deliberately unsorted */
            if (!seen_mask[mask]) {
                seen_mask[mask] = true;
                out->first_seen[out->first_seen_count++] = mask;
            }
            ecs_test_set(w, live[pick % live_count], ids, n);
        }
    }
    check_records(w, live_count);

    out->archetypes = ecs_archetype_count(w);
    for (u32 ai = 0; ai < out->archetypes; ++ai) {
        const ecs_archetype *a = ecs_archetype_at(w, ai);
        ASSERT_LT(out->word_count + 1u + a->component_count, ORDER_OUT_MAX);
        out->words[out->word_count++] = a->component_count;
        for (u32 i = 0; i < a->component_count; ++i)
            out->words[out->word_count++] = a->components[i];
    }
    u64 h = 0;
    for (u32 i = 0; i < live_count; ++i) {
        const u64 pair[2] = {live[i].bits, ecs_entity_record(w, live[i])->archetype};
        h = de_hash64(pair, sizeof pair, h);
    }
    out->entity_hash = h;
    ecs_off_archetype_created(w, on_created, &out->log);
    de_world_destroy(w);
}

static void expect_same_order(const order_result *a, const order_result *b) {
    EXPECT_EQ(a->archetypes, b->archetypes);
    EXPECT_EQ(a->word_count, b->word_count);
    EXPECT_MEMEQ(a->words, b->words, sizeof a->words);
    EXPECT_EQ(a->entity_hash, b->entity_hash);
}

TEST_F(archetype, creation_order, ecs_fx) {
    static order_result base, again, one_worker, many_workers;
    run_order_sequence(f, NULL, &base);
    run_order_sequence(f, NULL, &again);
    expect_same_order(&base, &again);

    /* Archetype order is the first-use order of the component sets. */
    ASSERT_EQ(base.archetypes, base.first_seen_count);
    u32 w = 0;
    for (u32 i = 0; i < base.archetypes; ++i) {
        const u32 mask = base.first_seen[i];
        u32 n = 0;
        for (u32 b = 0; b < 5; ++b)
            n += (mask >> b) & 1u;
        ASSERT_EQ(base.words[w], n);
        /* comps are ids 1..5; bit b selects id 5 - b; sorted ascending = high bit first. */
        u32 k = 0;
        for (u32 b = 5; b-- > 0;)
            if (mask & (1u << b))
                EXPECT_EQ(base.words[w + 1u + k++], 5u - b);
        w += 1u + n;
    }
    /* The listener saw every archetype created after it subscribed, in creation order. */
    ASSERT_EQ(base.log.count, base.archetypes - 1u);
    for (u32 i = 0; i < base.log.count; ++i)
        EXPECT_EQ(base.log.seen[i], i + 1u);

    /* The world's task system does not change the order. */
    de_fp_env_init_thread();
    de_task_system *ts = NULL;
    de_task_system_desc td;
    memset(&td, 0, sizeof td);
    td.worker_count = 1;
    td.background_worker_count = 1;
    ASSERT_EQ(de_task_system_create(&td, &ts), DE_OK);
    run_order_sequence(f, ts, &one_worker);
    de_task_system_destroy(ts);
    td.worker_count = 7;
    ASSERT_EQ(de_task_system_create(&td, &ts), DE_OK);
    run_order_sequence(f, ts, &many_workers);
    de_task_system_destroy(ts);
    expect_same_order(&base, &one_worker);
    expect_same_order(&base, &many_workers);
}

/* ---- archetype.chunk_layout_alignment -------------------------------------------------------- */

/* The test's own computation of the documented layout (ecs_internal.h). */
static u64 expected_layout(const de_world *w, const ecs_archetype *a, u64 rows) {
    u64 off = rows * sizeof(de_entity);
    for (u32 c = 0; c < a->column_count; ++c) {
        const u64 al = ecs_component_get(w, a->columns[c].component)->align;
        off = (off + al - 1u) / al * al;
        off += rows * a->columns[c].size;
    }
    off = (off + 3u) / 4u * 4u;
    return off + rows * 8u * a->column_count;
}

typedef struct span {
    u64 begin, end;
} span;

static int span_cmp(const void *x, const void *y) {
    const span *a = (const span *)x, *b = (const span *)y;
    return a->begin < b->begin ? -1 : a->begin > b->begin ? 1 : 0;
}

TEST_F(archetype, chunk_layout_alignment, ecs_fx) {
    const u32 chunk_sizes[] = {0, 1000, 1024, 4096, 16384, 65536};
    for (u32 cs = 0; cs < sizeof chunk_sizes / sizeof chunk_sizes[0]; ++cs) {
        ecs_fx_destroy_world(f);
        de_world_desc d;
        memset(&d, 0, sizeof d);
        d.chunk_bytes = chunk_sizes[cs];
        ecs_fx_create_world(f, &d);
        const u32 chunk_bytes = f->w->chunk_bytes;
        EXPECT_EQ(chunk_bytes % DE_ECS_MAX_COMPONENT_ALIGN, 0u);

        const u32 p = f->pos.id, v = f->vel.id, h = f->health.id, t = f->enemy.id, r = f->rot.id;
        const u32 sets[][4] = {{r, 0, 0, 0}, {p, r, 0, 0}, {h, t, r, v},
                               {p, v, h, r}, {p, 0, 0, 0}, {h, t, 0, 0}};
        const u32 sizes[] = {1, 2, 4, 4, 1, 2};
        for (u32 si = 0; si < 6; ++si) {
            ecs_archetype *a = NULL;
            ASSERT_EQ(ecs_archetype_get_or_create(f->w, sets[si], sizes[si], &a), DE_OK);

            /* The layout fits, and one more row would not. */
            const u64 used = expected_layout(f->w, a, a->rows_per_chunk);
            EXPECT_EQ(a->chunk_used, used);
            EXPECT_LE(used, chunk_bytes);
            EXPECT_GT(expected_layout(f->w, a, a->rows_per_chunk + 1u), chunk_bytes);

            /* Offsets are aligned and the arrays do not overlap. */
            span spans[1 + 3 * 4];
            u32 ns = 0;
            spans[ns++] = (span){0, (u64)a->rows_per_chunk * sizeof(de_entity)};
            for (u32 c = 0; c < a->column_count; ++c) {
                const ecs_column *col = &a->columns[c];
                const u32 al = ecs_component_get(f->w, col->component)->align;
                EXPECT_EQ(col->data_offset % al, 0u);
                EXPECT_EQ(col->added_offset % 4u, 0u);
                EXPECT_EQ(col->changed_offset % 4u, 0u);
                spans[ns++] =
                    (span){col->data_offset, col->data_offset + (u64)a->rows_per_chunk * col->size};
                spans[ns++] =
                    (span){col->added_offset, col->added_offset + (u64)a->rows_per_chunk * 4u};
                spans[ns++] =
                    (span){col->changed_offset, col->changed_offset + (u64)a->rows_per_chunk * 4u};
            }
            qsort(spans, ns, sizeof spans[0], span_cmp);
            for (u32 i = 0; i < ns; ++i) {
                EXPECT_LE(spans[i].end, used);
                if (i + 1u < ns)
                    EXPECT_LE(spans[i].end, spans[i + 1u].begin);
            }

            /* Fill two chunks and a bit: every pointer stays aligned and inside its chunk. */
            const u32 n = 2u * a->rows_per_chunk + 3u;
            for (u32 i = 0; i < n; ++i)
                ecs_test_set(f->w, de_entity_spawn(f->w), sets[si], sizes[si]);
            ASSERT_EQ(a->chunks.count, 3u);
            for (u32 c = 0; c < a->chunks.count; ++c) {
                const ecs_chunk *ch = ecs_table_chunk(a, c);
                EXPECT_EQ((uintptr_t)ch->data % DE_ECS_MAX_COMPONENT_ALIGN, 0u);
                for (u32 row = 0; row < ch->count; ++row) {
                    const de_entity e = ecs_chunk_entities(ch)[row];
                    const u8 *rot = (const u8 *)de_entity_get_const(f->w, e, f->rot);
                    if (ecs_archetype_has(a, f->rot.id)) {
                        ASSERT_TRUE(rot != NULL);
                        EXPECT_EQ((uintptr_t)rot % 16u, 0u);
                        EXPECT_TRUE(rot >= ch->data && rot + 16 <= ch->data + chunk_bytes);
                    }
                }
            }
        }
    }
}

/* ---- archetype.row_move_preserves_data ------------------------------------------------------- */

typedef struct TCounted {
    u32 value;
} TCounted;

static int g_counted_construct;
static int g_counted_destruct;
static void counted_construct(void *o) {
    ((TCounted *)o)->value = 0xABCDu;
    ++g_counted_construct;
}
static void counted_destruct(void *o) {
    (void)o;
    ++g_counted_destruct;
}
static void counted_copy(void *dst, const void *src) {
    memcpy(dst, src, sizeof(TCounted));
}

#define MOVE_N 2000u

static f32 fval(u32 i, u32 k) {
    return (f32)i * 4.0f + (f32)k * 0.25f;
}

TEST_F(archetype, row_move_preserves_data, ecs_fx) {
    static const de_field_info counted_fields[] = {
        DE_FIELD(TCounted, value, DE_FK_U32, DE_FIELD_DEFAULT)};
    de_type_desc cd = DE_TYPE_DESC(TCounted, 1, counted_fields);
    cd.is_pod = false;
    cd.hooks.construct = counted_construct;
    cd.hooks.destruct = counted_destruct;
    cd.hooks.copy = counted_copy;
    const de_component counted = de_component_register(f->w, de_type_register(f->types, &cd), 0);
    ASSERT_TRUE(DE_HANDLE_VALID(counted));
    g_counted_construct = 0;
    g_counted_destruct = 0;

    const u32 p = f->pos.id, v = f->vel.id, h = f->health.id, t = f->enemy.id, r = f->rot.id;
    static de_entity e[MOVE_N];
    f->w->change_tick = 1;
    for (u32 i = 0; i < MOVE_N; ++i) {
        e[i] = de_entity_spawn(f->w);
        ecs_test_set(f->w, e[i], &p, 1);
        TPos *pos = (TPos *)de_entity_get(f->w, e[i], f->pos);
        pos->v = (de_vec3){fval(i, 0), fval(i, 1), fval(i, 2)};
    }
    check_records(f->w, MOVE_N);

    /* Step 1: {pos} -> {pos, vel}, in a scattered order, so swap-removes hit the middle. */
    f->w->change_tick = 2;
    const u32 s1[] = {v, p};
    for (u32 k = 0; k < MOVE_N; ++k) {
        const u32 i = (k * 7919u) % MOVE_N;
        ecs_test_set(f->w, e[i], s1, 2);
        TVel *vel = (TVel *)de_entity_get(f->w, e[i], f->vel);
        EXPECT_FLOAT_EQ(vel->v.x, 0.0, 0.0); /* new columns are zeroed */
        vel->v = (de_vec3){-fval(i, 0), -fval(i, 1), -fval(i, 2)};
        if (k % 499u == 0)
            check_records(f->w, MOVE_N);
    }
    check_records(f->w, MOVE_N);

    /* Step 2: -> {pos, vel, rot, health, counted, enemy}. */
    f->w->change_tick = 3;
    const u32 s2[] = {counted.id, h, t, r, v, p};
    for (u32 k = 0; k < MOVE_N; ++k) {
        const u32 i = MOVE_N - 1u - k;
        ecs_test_set(f->w, e[i], s2, 6);
        EXPECT_EQ(((TCounted *)de_entity_get(f->w, e[i], counted))->value, 0xABCDu);
        TRot *rot = (TRot *)de_entity_get(f->w, e[i], f->rot);
        rot->q = (de_quat){fval(i, 3), fval(i, 4), fval(i, 5), fval(i, 6)};
        THealth *hp = (THealth *)de_entity_get(f->w, e[i], f->health);
        *hp = (THealth){fval(i, 7), fval(i, 8)};
    }
    EXPECT_EQ(g_counted_construct, (int)MOVE_N);
    EXPECT_EQ(g_counted_destruct, 0);
    check_records(f->w, MOVE_N);

    /* Step 3: -> {vel, rot, enemy}: pos, health and counted are dropped. */
    f->w->change_tick = 4;
    const u32 s3[] = {t, r, v};
    for (u32 k = 0; k < MOVE_N; ++k) {
        const u32 i = (k * 104729u) % MOVE_N;
        /* Data intact before the move. */
        const TPos *pos = (const TPos *)de_entity_get_const(f->w, e[i], f->pos);
        ASSERT_TRUE(pos != NULL);
        EXPECT_FLOAT_EQ(pos->v.x, fval(i, 0), 0.0);
        EXPECT_FLOAT_EQ(pos->v.z, fval(i, 2), 0.0);
        ecs_test_set(f->w, e[i], s3, 3);
    }
    EXPECT_EQ(g_counted_destruct, (int)MOVE_N);
    check_records(f->w, MOVE_N);

    /* Every value survived three moves; ticks moved with their columns. */
    for (u32 i = 0; i < MOVE_N; ++i) {
        EXPECT_FALSE(de_entity_has(f->w, e[i], f->pos));
        EXPECT_FALSE(de_entity_has(f->w, e[i], f->health));
        EXPECT_TRUE(de_entity_has(f->w, e[i], f->enemy));
        const TVel *vel = (const TVel *)de_entity_get_const(f->w, e[i], f->vel);
        const TRot *rot = (const TRot *)de_entity_get_const(f->w, e[i], f->rot);
        ASSERT_TRUE(vel != NULL && rot != NULL);
        EXPECT_FLOAT_EQ(vel->v.x, -fval(i, 0), 0.0);
        EXPECT_FLOAT_EQ(vel->v.y, -fval(i, 1), 0.0);
        EXPECT_FLOAT_EQ(vel->v.z, -fval(i, 2), 0.0);
        EXPECT_FLOAT_EQ(rot->q.x, fval(i, 3), 0.0);
        EXPECT_FLOAT_EQ(rot->q.w, fval(i, 6), 0.0);

        const ecs_record *rec = ecs_entity_record(f->w, e[i]);
        const ecs_archetype *a = ecs_archetype_at(f->w, rec->archetype);
        const ecs_chunk *ch = ecs_table_chunk(a, rec->chunk);
        const u32 vc = ecs_archetype_column(a, f->vel.id);
        const u32 rc = ecs_archetype_column(a, f->rot.id);
        EXPECT_EQ(ecs_chunk_added(a, ch, vc)[rec->row], 2u);
        EXPECT_EQ(ecs_chunk_changed(a, ch, vc)[rec->row], 2u);
        EXPECT_EQ(ecs_chunk_added(a, ch, rc)[rec->row], 3u);
    }

    /* Swap-remove updates the moved entity's record: despawn from the front of the table. */
    ecs_archetype *a3 = NULL;
    ASSERT_EQ(ecs_archetype_get_or_create(f->w, s3, 3, &a3), DE_OK);
    ASSERT_EQ(a3->count, MOVE_N);
    for (u32 k = 0; k < MOVE_N / 2u; ++k) {
        const ecs_archetype *a = a3;
        const de_entity front = ecs_chunk_entities(ecs_table_chunk(a, 0))[0];
        ecs_test_despawn(f->w, front);
        EXPECT_FALSE(de_entity_alive(f->w, front));
    }
    check_records(f->w, MOVE_N - MOVE_N / 2u);

    /* Moving a dead entity fails; moving to its own archetype does nothing. */
    ecs_archetype *empty = ecs_archetype_at(f->w, ECS_EMPTY_ARCHETYPE);
    de_entity dead = DE_ENTITY_NULL;
    for (u32 i = 0; i < MOVE_N && dead.bits == 0; ++i)
        if (!de_entity_alive(f->w, e[i]))
            dead = e[i];
    EXPECT_EQ(ecs_entity_move(f->w, dead, empty), DE_ERR_INVALID_ARGUMENT);
}
