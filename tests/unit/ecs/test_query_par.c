/* test_query_par.c: de_query_par_each: exact cover, a (range -> rows) mapping that does not depend
 * on the worker count, and a bit-identical fixed-order reduction (DENG-346; SPEC §7.2, §5.7
 * rule 5, §8).
 *
 * Every test builds the same world from scratch for each worker configuration: no task system
 * (ranges run serially, in order), then task systems with 1, 2, 4 and cores - 1 workers besides
 * the main thread. QPAR_EXPECTED_SUM_BITS is the test vector of the reduction: it must hold on
 * every machine and architecture; a change is a determinism bug, never a reason to update it. */
#include "ecs_test_util.h"

#include <dengine/de_platform.h>

#include "ecs/query_internal.h"
#include "platform/de_atomic.h"

#include <stdio.h>

#define QPAR_SPAWNS   60000u
#define QPAR_MAX_ROWS QPAR_SPAWNS
#define QPAR_CONFIGS  5u
/** f32 bits of the fixed-order sum of TPos.x over the query, grain 1000 (see the file comment). */
#define QPAR_EXPECTED_SUM_BITS 0xC962515Du

static u32 hash_u32(u32 x) {
    x += 0x9E3779B9u;
    x = (x ^ (x >> 16)) * 0x85EBCA6Bu;
    x = (x ^ (x >> 13)) * 0xC2B2AE35u;
    return x ^ (x >> 16);
}

/* Values in [-0.5, 0.5) scaled by 2^0..2^15. Every step is exact (24-bit integer to float, power
 * of two scales), so the values are the same on every machine; the sum is very sensitive to the
 * order of the additions. */
static f32 value_of(u32 i) {
    const u32 h = hash_u32(i);
    const f32 unit = (f32)((i32)(h & 0xFFFFFFu) - 0x800000) / 16777216.0f;
    return unit * (f32)(1u << ((h >> 24) & 15u));
}

/* worker_count values of the configurations; 0 in slot 0 means "no task system". */
static void worker_configs(u32 out[QPAR_CONFIGS]) {
    const u32 cores = de_cpu_logical_cores();
    out[0] = 0;
    out[1] = 1;
    out[2] = 2;
    out[3] = 4;
    out[4] = cores > 2u ? cores - 1u : 1u;
}

static de_task_system *make_ts(u32 workers) {
    if (workers == 0)
        return NULL;
    de_fp_env_init_thread();
    de_task_system_desc d = {0};
    d.worker_count = workers;
    d.background_worker_count = 1;
    de_task_system *ts = NULL;
    ASSERT_EQ(de_task_system_create(&d, &ts), DE_OK);
    return ts;
}

/* Builds the test world on `ts`: eight archetypes (six with TPos), entities spawned interleaved,
 * every 7th despawned so swap-removes shuffle the rows, small chunks. Returns the query
 * {READ pos, OPTIONAL_READ vel}. */
static de_query build_world(ecs_fx *f, de_task_system *ts) {
    ecs_fx_destroy_world(f);
    de_world_desc d;
    memset(&d, 0, sizeof d);
    d.chunk_bytes = 2048;
    d.tasks = ts;
    ecs_fx_create_world(f, &d);
    const de_component sets[8][4] = {
        {f->pos},
        {f->pos, f->vel},
        {f->pos, f->vel, f->health},
        {f->pos, f->enemy},
        {f->vel, f->health},
        {f->pos, f->vel, f->enemy, f->rot},
        {f->health, f->rot},
        {f->pos, f->health, f->enemy},
    };
    const u32 sizes[8] = {1, 2, 3, 2, 2, 4, 2, 3};
    de_entity prev = DE_ENTITY_NULL;
    for (u32 i = 0; i < QPAR_SPAWNS; ++i) {
        const u32 k = hash_u32(i * 3u + 1u) % 8u;
        de_entity e = DE_ENTITY_NULL;
        ASSERT_EQ(de_entity_spawn_with(f->w, sets[k], sizes[k], &e), DE_OK);
        TPos *p = (TPos *)de_entity_get(f->w, e, f->pos);
        if (p != NULL)
            p->v = (de_vec3){value_of(i), (f32)i, 0.0f};
        if (i % 7u == 6u)
            de_entity_despawn(f->w, prev);
        prev = e;
    }
    const de_query_term t[] = {{f->pos, DE_READ}, {f->vel, DE_OPTIONAL_READ}};
    const de_query_desc qd = {t, 2, NULL, 0};
    const de_query q = de_query_create(f->w, &qd);
    ASSERT_TRUE(DE_HANDLE_VALID(q));
    return q;
}

/* ---- Recording ------------------------------------------------------------------------------- */

typedef struct par_rec {
    u32 grain;
    u32 rows;                          /* de_query_count */
    de_atomic_u32 hits[QPAR_MAX_ROWS]; /* per row-space row */
    u64 entity[QPAR_MAX_ROWS];         /* per row: entity bits, written by its range */
    u32 range[QPAR_MAX_ROWS];          /* per row: chunk->range */
    u8 slice_start[QPAR_MAX_ROWS];     /* 1 where a slice starts */
    f32 partial[QPAR_MAX_ROWS];        /* per range */
    de_atomic_u32 bad;                 /* slices that broke a rule */
} par_rec;

static par_rec g_rec;
static u64 g_ref_entity[QPAR_MAX_ROWS];
static u32 g_ref_range[QPAR_MAX_ROWS];
static u8 g_ref_slice_start[QPAR_MAX_ROWS];

static void rec_reset(par_rec *r, u32 grain, u32 rows) {
    r->grain = grain;
    r->rows = rows;
    for (u32 i = 0; i < QPAR_MAX_ROWS; ++i) {
        de_atomic_init_u32(&r->hits[i], 0);
        r->entity[i] = 0;
        r->range[i] = 0xFFFFFFFFu;
        r->slice_start[i] = 0;
        r->partial[i] = 0.0f;
    }
    de_atomic_init_u32(&r->bad, 0);
}

static void rec_fn(const de_query_chunk *ch, void *user) {
    par_rec *r = (par_rec *)user;
    const u32 grain = r->grain;
    /* A slice lies inside its range, and its range is first_row / grain. */
    if (ch->count == 0 || ch->first_row / grain != ch->range ||
        (u64)ch->first_row + ch->count > ((u64)ch->range + 1u) * grain ||
        (u64)ch->first_row + ch->count > r->rows) {
        de_atomic_fetch_add_u32(&r->bad, 1, DE_ATOMIC_RELAXED);
        return;
    }
    const TPos *p = (const TPos *)ch->columns[0];
    r->slice_start[ch->first_row] = 1;
    f32 s = r->partial[ch->range]; /* a range's slices run in order on one thread */
    for (u32 i = 0; i < ch->count; ++i) {
        const u32 row = ch->first_row + i;
        de_atomic_fetch_add_u32(&r->hits[row], 1, DE_ATOMIC_RELAXED);
        r->entity[row] = ch->entities[i].bits;
        r->range[row] = ch->range;
        s += p[i].v.x;
    }
    r->partial[ch->range] = s;
}

static f32 reduce_partials(const par_rec *r) {
    const u32 ranges = (r->rows + r->grain - 1u) / r->grain;
    f32 sum = 0.0f;
    for (u32 k = 0; k < ranges; ++k)
        sum += r->partial[k];
    return sum;
}

static u32 f32_bits(f32 v) {
    u32 u;
    memcpy(&u, &v, sizeof u);
    return u;
}

/* The row order of de_query_each (serial): entity bits per row. */
static void ref_each_fn(const de_query_chunk *ch, void *user) {
    u32 *next = (u32 *)user;
    if (ch->first_row != *next)
        FAIL("de_query_each: first_row is not consecutive");
    for (u32 i = 0; i < ch->count; ++i)
        g_ref_entity[ch->first_row + i] = ch->entities[i].bits;
    *next += ch->count;
}

/* ---- qpar.exact_cover ------------------------------------------------------------------------ */

TEST_F(qpar, exact_cover, ecs_fx) {
    /* Every row of the row space is delivered exactly once, as the entity de_query_each has at
     * that row, for many grains, with no task system and with every worker count. */
    u32 configs[QPAR_CONFIGS];
    worker_configs(configs);
    const u32 grains[] = {1, 7, 100, 1000, 0, 4999, 1000000};
    for (u32 c = 0; c < QPAR_CONFIGS; ++c) {
        de_task_system *ts = make_ts(configs[c]);
        const de_query q = build_world(f, ts);
        const u32 rows = de_query_count(f->w, q);
        ASSERT_GT(rows, 30000u);
        ASSERT_LE(rows, QPAR_MAX_ROWS);
        u32 next = 0;
        de_query_each(f->w, q, ref_each_fn, &next);
        ASSERT_EQ(next, rows);
        for (u32 g = 0; g < sizeof grains / sizeof grains[0]; ++g) {
            const u32 grain = grains[g] != 0 ? grains[g] : DE_QUERY_DEFAULT_GRAIN_ROWS;
            rec_reset(&g_rec, grain, rows);
            de_query_par_each(f->w, q, grains[g], rec_fn, &g_rec);
            EXPECT_EQ(de_atomic_load_u32(&g_rec.bad, DE_ATOMIC_RELAXED), 0u);
            u32 wrong = 0;
            for (u32 i = 0; i < rows; ++i)
                if (de_atomic_load_u32(&g_rec.hits[i], DE_ATOMIC_RELAXED) != 1u ||
                    g_rec.entity[i] != g_ref_entity[i] || g_rec.range[i] != i / grain)
                    ++wrong;
            if (wrong != 0) {
                char msg[160];
                snprintf(msg, sizeof msg, "workers %u grain %u: %u rows wrong", configs[c],
                         grains[g], wrong);
                FAIL(msg);
            }
        }
        ecs_fx_destroy_world(f);
        de_task_system_destroy(ts);
    }
}

/* ---- qpar.ranges_independent_of_workers ------------------------------------------------------ */

TEST_F(qpar, ranges_independent_of_workers, ecs_fx) {
    /* The (range -> rows) mapping and the slice boundaries are identical for every worker count:
     * they depend only on grain_rows and the data. */
    u32 configs[QPAR_CONFIGS];
    worker_configs(configs);
    const u32 grains[] = {333, 0};
    for (u32 g = 0; g < 2; ++g) {
        const u32 grain = grains[g] != 0 ? grains[g] : DE_QUERY_DEFAULT_GRAIN_ROWS;
        u32 ref_rows = 0;
        for (u32 c = 0; c < QPAR_CONFIGS; ++c) {
            de_task_system *ts = make_ts(configs[c]);
            const de_query q = build_world(f, ts);
            const u32 rows = de_query_count(f->w, q);
            for (u32 run = 0; run < 3; ++run) {
                rec_reset(&g_rec, grain, rows);
                de_query_par_each(f->w, q, grains[g], rec_fn, &g_rec);
                EXPECT_EQ(de_atomic_load_u32(&g_rec.bad, DE_ATOMIC_RELAXED), 0u);
                if (c == 0 && run == 0) {
                    ref_rows = rows;
                    memcpy(g_ref_entity, g_rec.entity, sizeof g_ref_entity);
                    memcpy(g_ref_range, g_rec.range, sizeof g_ref_range);
                    memcpy(g_ref_slice_start, g_rec.slice_start, sizeof g_ref_slice_start);
                    continue;
                }
                ASSERT_EQ(rows, ref_rows);
                EXPECT_MEMEQ(g_rec.entity, g_ref_entity, sizeof g_ref_entity);
                EXPECT_MEMEQ(g_rec.range, g_ref_range, sizeof g_ref_range);
                EXPECT_MEMEQ(g_rec.slice_start, g_ref_slice_start, sizeof g_ref_slice_start);
            }
            ecs_fx_destroy_world(f);
            de_task_system_destroy(ts);
        }
        /* Slices start at every range start, and ranges are [k * grain, (k + 1) * grain). */
        for (u32 i = 0; i < ref_rows; i += grain)
            EXPECT_EQ(g_ref_slice_start[i], 1u);
        for (u32 i = 0; i < ref_rows; ++i)
            ASSERT_EQ(g_ref_range[i], i / grain);
    }
}

/* ---- qpar.reduction_bitwise ------------------------------------------------------------------ */

TEST_F(qpar, reduction_bitwise, ecs_fx) {
    /* Per-range partial sums of TPos.x, reduced in range order: bit-identical for every worker
     * count and run, and equal to the test vector. */
    u32 configs[QPAR_CONFIGS];
    worker_configs(configs);
    const u32 grain = 1000;
    u32 ref_bits = 0;
    for (u32 c = 0; c < QPAR_CONFIGS; ++c) {
        de_task_system *ts = make_ts(configs[c]);
        const de_query q = build_world(f, ts);
        const u32 rows = de_query_count(f->w, q);
        for (u32 run = 0; run < 5; ++run) {
            rec_reset(&g_rec, grain, rows);
            de_query_par_each(f->w, q, grain, rec_fn, &g_rec);
            const u32 bits = f32_bits(reduce_partials(&g_rec));
            if (c == 0 && run == 0) {
                ref_bits = bits;
                printf("qpar.reduction_bitwise: rows %u, sum bits 0x%08X\n", rows, bits);
            }
            EXPECT_EQ(bits, ref_bits);
        }
        ecs_fx_destroy_world(f);
        de_task_system_destroy(ts);
    }
    EXPECT_EQ(ref_bits, QPAR_EXPECTED_SUM_BITS);
}
