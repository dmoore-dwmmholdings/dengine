/* Reduction determinism of de_task_parallel_for and task graphs (DENG-293; SPEC §5.7 rule 5).
 *
 * The pattern every deterministic parallel computation follows: split the work into fixed
 * chunks (boundaries depend only on count and grain), let each chunk write its partial result
 * into a slot indexed by the chunk number, and reduce the slots serially in index order. Which
 * thread runs a chunk, and when, then cannot change the result: it is bit-identical for every
 * worker count and every run. DET_EXPECTED_BITS is the test vector: the f32 sum of the DET_COUNT
 * values below, chunked by DET_GRAIN. It must hold on every machine and architecture; a change is
 * a determinism bug, never a reason to update the constant.
 *
 * The negative example (an f32 accumulated with an atomic compare-and-swap loop, in whatever
 * order the chunks finish) is printed, never asserted: it is documentation of what not to do.
 * Float addition is not associative, so its result depends on the finishing order. */
#include "de_test.h"

#include <dengine/de_math.h>
#include <dengine/de_platform.h>
#include <dengine/de_task.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "platform/de_atomic.h"

#define DET_COUNT  1000000u
#define DET_GRAIN  4096u
#define DET_CHUNKS ((DET_COUNT + DET_GRAIN - 1u) / DET_GRAIN) /* 245 */
#define DET_RUNS   20u

/** The bits of the reference result (see the file comment). */
#define DET_EXPECTED_BITS 0xC8424754u

static float g_values[DET_COUNT];
static float g_partials[DET_CHUNKS];

static u32 f32_bits(float f) {
    u32 u;
    memcpy(&u, &f, sizeof u);
    return u;
}

static float bits_f32(u32 u) {
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

/* splitmix32-style integer hash: the values depend only on the index, on every platform. */
static u32 hash_u32(u32 x) {
    x += 0x9E3779B9u;
    x = (x ^ (x >> 16)) * 0x85EBCA6Bu;
    x = (x ^ (x >> 13)) * 0xC2B2AE35u;
    return x ^ (x >> 16);
}

/* Values in [-0.5, 0.5) scaled by 2^0..2^15, so the sum is very sensitive to the order of the
 * additions. Every step is exact (24-bit integer to float, power-of-two scales), so the values
 * are the same on every machine even with FMA contraction. */
static void fill_values(void) {
    for (u32 i = 0; i < DET_COUNT; ++i) {
        const u32 h = hash_u32(i);
        const float unit = (float)(h & 0xFFFFFFu) * (1.0f / 16777216.0f) - 0.5f;
        g_values[i] = unit * (float)(1u << ((h >> 24) & 15u));
    }
}

static void chunk_sum(u32 begin, u32 end, void *user) {
    (void)user;
    float s = 0.0f;
    for (u32 i = begin; i < end; ++i)
        s += g_values[i];
    g_partials[begin / DET_GRAIN] = s;
}

static float reduce_partials(void) {
    float s = 0.0f;
    for (u32 k = 0; k < DET_CHUNKS; ++k)
        s += g_partials[k];
    return s;
}

/* The same computation on one thread, as the reference. */
static float serial_reference(void) {
    for (u32 k = 0; k < DET_CHUNKS; ++k) {
        const u32 begin = k * DET_GRAIN;
        const u32 end = begin + DET_GRAIN < DET_COUNT ? begin + DET_GRAIN : DET_COUNT;
        chunk_sum(begin, end, NULL);
    }
    return reduce_partials();
}

/* The worker counts every test runs with: {1, 2, 4, cores - 1} (de_task_system_desc
 * worker_count, i.e. threads besides the main thread). */
static u32 worker_counts(u32 out[4]) {
    const u32 cores = de_cpu_logical_cores();
    out[0] = 1;
    out[1] = 2;
    out[2] = 4;
    out[3] = cores > 2 ? cores - 1 : 1;
    return 4;
}

static de_task_system *make_ts(u32 workers) {
    de_fp_env_init_thread();
    de_task_system_desc d = {0};
    d.worker_count = workers;
    d.background_worker_count = 1;
    de_task_system *ts = NULL;
    ASSERT_EQ(de_task_system_create(&d, &ts), DE_OK);
    return ts;
}

/* ---------- Negative example: atomic float accumulation ---------- */

static de_atomic_u32 g_atomic_sum; /* f32 bits */

static void chunk_atomic_accumulate(u32 begin, u32 end, void *user) {
    (void)user;
    float s = 0.0f;
    for (u32 i = begin; i < end; ++i)
        s += g_values[i];
    u32 old = de_atomic_load_u32(&g_atomic_sum, DE_ATOMIC_RELAXED);
    for (;;) {
        const u32 desired = f32_bits(bits_f32(old) + s);
        if (de_atomic_cas_weak_u32(&g_atomic_sum, &old, desired, DE_ATOMIC_RELAXED,
                                   DE_ATOMIC_RELAXED))
            break;
    }
}

/* ---------- det.pfor_reduction_bitwise ---------- */

TEST(det, pfor_reduction_bitwise) {
    fill_values();
    const u32 expected = f32_bits(serial_reference());
    printf("det: serial chunked reduction of %u f32 = %a (bits 0x%08X)\n", DET_COUNT,
           (double)bits_f32(expected), expected);
    EXPECT_EQ(expected, DET_EXPECTED_BITS);

    u32 counts[4];
    const u32 n = worker_counts(counts);
    for (u32 c = 0; c < n; ++c) {
        de_task_system *ts = make_ts(counts[c]);
        u32 atomic_distinct[DET_RUNS];
        u32 atomic_n = 0;
        for (u32 r = 0; r < DET_RUNS; ++r) {
            memset(g_partials, 0xFF, sizeof g_partials); /* NaNs: a missed chunk shows */
            de_task_parallel_for(ts, DET_COUNT, DET_GRAIN, chunk_sum, NULL);
            const u32 got = f32_bits(reduce_partials());
            if (got != expected) {
                printf("det: workers %u run %u: 0x%08X != 0x%08X\n", counts[c] + 1, r, got,
                       expected);
            }
            EXPECT_EQ(got, expected);

            /* Negative example (not asserted). */
            de_atomic_store_u32(&g_atomic_sum, 0, DE_ATOMIC_RELAXED);
            de_task_parallel_for(ts, DET_COUNT, DET_GRAIN, chunk_atomic_accumulate, NULL);
            const u32 a = de_atomic_load_u32(&g_atomic_sum, DE_ATOMIC_RELAXED);
            bool seen = false;
            for (u32 k = 0; k < atomic_n; ++k)
                seen = seen || atomic_distinct[k] == a;
            if (!seen)
                atomic_distinct[atomic_n++] = a;
        }
        printf("det: %u frame workers: %u runs bit-identical 0x%08X; atomic-float accumulate "
               "(NOT deterministic, not asserted) gave %u distinct result(s), first 0x%08X\n",
               counts[c] + 1, DET_RUNS, expected, atomic_n, atomic_distinct[0]);
        de_task_system_destroy(ts);
    }
}

/* ---------- det.graph_fixed_order_reduce ---------- */

/* DET_SLICES graph nodes each cover a fixed, contiguous range of chunks (summed with a nested
 * de_task_parallel_for, so chunks spread over the workers again); a final node depends on all of
 * them and reduces the partials in chunk order. The result must equal the parallel_for one. */
#define DET_SLICES 16u

typedef struct det_slice {
    u32 first_chunk;
    u32 chunk_count;
    de_task_system *ts;
} det_slice;

static det_slice g_slices[DET_SLICES];
static u32 g_graph_result;

static void slice_sum(u32 begin, u32 end, void *user) {
    const det_slice *s = (const det_slice *)user;
    /* Chunk indices are relative to the slice; map them back to global value indices. */
    for (u32 k = begin; k < end; ++k) {
        const u32 chunk = s->first_chunk + k;
        const u32 b = chunk * DET_GRAIN;
        const u32 e = b + DET_GRAIN < DET_COUNT ? b + DET_GRAIN : DET_COUNT;
        chunk_sum(b, e, NULL);
    }
}

static void slice_node(void *user) {
    det_slice *s = (det_slice *)user;
    /* Grain 1 over chunk indices: chunk boundaries stay exactly those of the reference. */
    de_task_parallel_for(s->ts, s->chunk_count, 1, slice_sum, s);
}

static void reduce_node(void *user) {
    (void)user;
    g_graph_result = f32_bits(reduce_partials());
}

TEST(det, graph_fixed_order_reduce) {
    fill_values();
    const u32 expected = f32_bits(serial_reference());
    EXPECT_EQ(expected, DET_EXPECTED_BITS);

    u32 counts[4];
    const u32 n = worker_counts(counts);
    for (u32 c = 0; c < n; ++c) {
        de_task_system *ts = make_ts(counts[c]);
        de_task_graph *g = NULL;
        ASSERT_EQ(de_task_graph_create(ts, &g), DE_OK);
        de_task_node_desc rd = {0};
        rd.name = "det.reduce";
        rd.fn = reduce_node;
        const de_task_node reduce = de_task_graph_add(g, &rd);
        for (u32 s = 0; s < DET_SLICES; ++s) {
            g_slices[s].first_chunk = s * DET_CHUNKS / DET_SLICES;
            g_slices[s].chunk_count = (s + 1) * DET_CHUNKS / DET_SLICES - g_slices[s].first_chunk;
            g_slices[s].ts = ts;
            de_task_node_desc d = {0};
            d.name = "det.slice";
            d.fn = slice_node;
            d.user = &g_slices[s];
            const de_task_node h = de_task_graph_add(g, &d);
            de_task_graph_depend(g, h, reduce);
        }
        ASSERT_EQ(de_task_graph_compile(g), DE_OK);
        de_task_counter done = {0};
        for (u32 r = 0; r < DET_RUNS; ++r) {
            memset(g_partials, 0xFF, sizeof g_partials);
            g_graph_result = 0;
            de_task_graph_run(g, &done);
            de_task_wait(ts, &done);
            EXPECT_EQ(g_graph_result, expected);
        }
        printf("det: graph, %u frame workers: %u runs bit-identical 0x%08X\n", counts[c] + 1,
               DET_RUNS, g_graph_result);
        de_task_graph_destroy(g);
        de_task_system_destroy(ts);
    }
}
