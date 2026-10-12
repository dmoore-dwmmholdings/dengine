/* Unit tests of de_task_parallel_for (src/task/parallel_for.c; DENG-292): every index exactly
 * once, chunk boundaries equal to the formula, nesting inside graph nodes and inside chunks. */
#include "de_test.h"

#include <dengine/de_math.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_task.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "platform/de_atomic.h"
#include "task/task_internal.h"

static de_task_system *make_ts(u32 workers) {
    de_fp_env_init_thread();
    de_task_system_desc d = {0};
    d.worker_count = workers;
    d.background_worker_count = 1;
    de_task_system *ts = NULL;
    ASSERT_EQ(de_task_system_create(&d, &ts), DE_OK);
    ASSERT_TRUE(ts != NULL);
    return ts;
}

#define BIG 1000000u
static const u32 k_grains[] = {1, 7, 64, 4096};

/* ---------- exact cover ---------- */

static de_atomic_u32 *g_hits; /* BIG + 1 entries; the extra one catches overruns */

static void hit_range(u32 begin, u32 end, void *user) {
    (void)user;
    for (u32 i = begin; i < end; ++i)
        de_atomic_fetch_add_u32(&g_hits[i], 1, DE_ATOMIC_RELAXED);
}

/* Runs one loop and checks every index in [0, count) was hit once and nothing past it. Returns
 * the number of bad indices. */
static u32 cover_once(de_task_system *ts, u32 count, u32 grain) {
    memset(g_hits, 0, sizeof(de_atomic_u32) * (BIG + 1));
    de_task_parallel_for(ts, count, grain, hit_range, NULL);
    u32 bad = 0;
    for (u32 i = 0; i <= BIG; ++i)
        bad += de_atomic_load_u32(&g_hits[i], DE_ATOMIC_RELAXED) != (i < count ? 1u : 0u);
    return bad;
}

static void exact_cover_all(de_task_system *ts) {
    g_hits = (de_atomic_u32 *)calloc(BIG + 1, sizeof(de_atomic_u32));
    ASSERT_TRUE(g_hits != NULL);
    for (u32 gi = 0; gi < sizeof k_grains / sizeof k_grains[0]; ++gi) {
        const u32 grain = k_grains[gi];
        const u32 counts[] = {0, 1, grain - 1, grain, grain + 1, BIG};
        for (u32 ci = 0; ci < sizeof counts / sizeof counts[0]; ++ci)
            EXPECT_EQ(cover_once(ts, counts[ci], grain), 0u);
    }
    /* grain 0: automatic grain. */
    EXPECT_EQ(cover_once(ts, BIG, 0), 0u);
    EXPECT_EQ(cover_once(ts, 3, 0), 0u);
    free(g_hits);
    g_hits = NULL;
}

TEST(pfor, exact_cover) {
    de_task_system *ts = make_ts(0);
    exact_cover_all(ts);
    de_task_system_destroy(ts);
}

/* ---------- chunk boundaries ---------- */

typedef struct chunk_rec {
    u32 grain;
    u32 chunks;
    de_atomic_u32 calls;
    de_atomic_u32 bad;   /* begin not on a chunk boundary, or begin >= end */
    de_atomic_u32 *seen; /* per chunk: calls */
    de_atomic_u32 *end;  /* per chunk: recorded end */
} chunk_rec;

static void record_chunk(u32 begin, u32 end, void *user) {
    chunk_rec *r = (chunk_rec *)user;
    de_atomic_fetch_add_u32(&r->calls, 1, DE_ATOMIC_RELAXED);
    if (begin % r->grain != 0 || begin >= end || begin / r->grain >= r->chunks) {
        de_atomic_fetch_add_u32(&r->bad, 1, DE_ATOMIC_RELAXED);
        return;
    }
    const u32 k = begin / r->grain;
    de_atomic_fetch_add_u32(&r->seen[k], 1, DE_ATOMIC_RELAXED);
    de_atomic_store_u32(&r->end[k], end, DE_ATOMIC_RELAXED);
}

/* Checks the chunks of one loop against [k*grain, min((k+1)*grain, count)). `grain` 0 checks
 * against the documented automatic grain. */
static void check_boundaries(de_task_system *ts, u32 count, u32 grain) {
    static chunk_rec r;
    const u32 eff = grain != 0 ? grain : de__task_parallel_for_auto_grain(ts, count);
    if (grain == 0 && count != 0) {
        const u32 want = count / (de_task_worker_count(ts) * 4u);
        ASSERT_EQ(eff, want > 0 ? want : 1u);
    }
    r.grain = eff;
    r.chunks = count == 0 ? 0 : (count - 1) / eff + 1;
    de_atomic_store_u32(&r.calls, 0, DE_ATOMIC_RELAXED);
    de_atomic_store_u32(&r.bad, 0, DE_ATOMIC_RELAXED);
    r.seen = (de_atomic_u32 *)calloc(r.chunks + 1, sizeof(de_atomic_u32));
    r.end = (de_atomic_u32 *)calloc(r.chunks + 1, sizeof(de_atomic_u32));
    ASSERT_TRUE(r.seen != NULL && r.end != NULL);
    de_task_parallel_for(ts, count, grain, record_chunk, &r);
    EXPECT_EQ(de_atomic_load_u32(&r.calls, DE_ATOMIC_RELAXED), r.chunks);
    EXPECT_EQ(de_atomic_load_u32(&r.bad, DE_ATOMIC_RELAXED), 0u);
    u32 wrong = 0;
    for (u32 k = 0; k < r.chunks; ++k) {
        const u64 want_end = (u64)(k + 1) * eff < count ? (u64)(k + 1) * eff : count;
        wrong += de_atomic_load_u32(&r.seen[k], DE_ATOMIC_RELAXED) != 1u;
        wrong += de_atomic_load_u32(&r.end[k], DE_ATOMIC_RELAXED) != (u32)want_end;
    }
    EXPECT_EQ(wrong, 0u);
    free(r.seen);
    free(r.end);
}

static void boundaries_all(de_task_system *ts) {
    for (u32 gi = 0; gi < sizeof k_grains / sizeof k_grains[0]; ++gi) {
        const u32 grain = k_grains[gi];
        const u32 counts[] = {0, 1, grain - 1, grain, grain + 1, BIG};
        for (u32 ci = 0; ci < sizeof counts / sizeof counts[0]; ++ci)
            check_boundaries(ts, counts[ci], grain);
    }
    check_boundaries(ts, BIG, 0);
    check_boundaries(ts, 5, 0);
    /* The last chunk's begin + grain overflows u32. */
    check_boundaries(ts, 0xFFFFFFFFu, 0x40000000u);
}

TEST(pfor, chunk_boundaries) {
    de_task_system *ts = make_ts(0);
    boundaries_all(ts);
    de_task_system_destroy(ts);
}

/* ---------- nested ---------- */

/* Three levels: each graph node runs a loop of L1 indices; each index runs a loop of L2; each
 * of those a loop of L3, which counts leaves. Leaf (node, i, j, k) has its own counter. */
enum { NODES = 8, L1 = 32, L2 = 16, L3 = 16, LEAVES = NODES * L1 * L2 * L3 };

typedef struct nest_ctx {
    de_task_system *ts;
    de_atomic_u32 *leaves;
    u32 base; /* leaf index of this level's index 0 */
} nest_ctx;

static void level3(u32 begin, u32 end, void *user) {
    const nest_ctx *c = (const nest_ctx *)user;
    for (u32 k = begin; k < end; ++k)
        de_atomic_fetch_add_u32(&c->leaves[c->base + k], 1, DE_ATOMIC_RELAXED);
}

static void level2(u32 begin, u32 end, void *user) {
    const nest_ctx *c = (const nest_ctx *)user;
    for (u32 j = begin; j < end; ++j) {
        nest_ctx inner = {c->ts, c->leaves, c->base + j * L3};
        de_task_parallel_for(c->ts, L3, 3, level3, &inner);
    }
}

static void level1(u32 begin, u32 end, void *user) {
    const nest_ctx *c = (const nest_ctx *)user;
    for (u32 i = begin; i < end; ++i) {
        nest_ctx inner = {c->ts, c->leaves, c->base + i * L2 * L3};
        de_task_parallel_for(c->ts, L2, 2, level2, &inner);
    }
}

static void nest_node(void *user) {
    nest_ctx *c = (nest_ctx *)user;
    de_task_parallel_for(c->ts, L1, 4, level1, c);
}

static void run_nested(u32 workers) {
    de_task_system *ts = make_ts(workers);
    de_atomic_u32 *leaves = (de_atomic_u32 *)calloc(LEAVES, sizeof(de_atomic_u32));
    ASSERT_TRUE(leaves != NULL);
    static nest_ctx ctx[NODES];
    de_task_graph *g = NULL;
    ASSERT_EQ(de_task_graph_create(ts, &g), DE_OK);
    de_task_node h[NODES];
    for (u32 n = 0; n < NODES; ++n) {
        ctx[n] = (nest_ctx){ts, leaves, n * L1 * L2 * L3};
        de_task_node_desc d = {0};
        d.name = "nest";
        d.fn = nest_node;
        d.user = &ctx[n];
        h[n] = de_task_graph_add(g, &d);
    }
    /* Two diamonds in a row: 0 -> {1,2,3} -> 4 -> {5,6} -> 7. */
    for (u32 n = 1; n <= 3; ++n) {
        de_task_graph_depend(g, h[0], h[n]);
        de_task_graph_depend(g, h[n], h[4]);
    }
    for (u32 n = 5; n <= 6; ++n) {
        de_task_graph_depend(g, h[4], h[n]);
        de_task_graph_depend(g, h[n], h[7]);
    }
    ASSERT_EQ(de_task_graph_compile(g), DE_OK);
    enum { RUNS = 4 };
    for (u32 r = 0; r < RUNS; ++r) {
        de_task_counter done = {0};
        de_task_graph_run(g, &done);
        de_task_wait(ts, &done);
    }
    u32 bad = 0;
    for (u32 i = 0; i < LEAVES; ++i)
        bad += de_atomic_load_u32(&leaves[i], DE_ATOMIC_RELAXED) != (u32)RUNS;
    EXPECT_EQ(bad, 0u);
    de_task_graph_destroy(g);
    de_task_system_destroy(ts);
    free(leaves);
}

TEST(pfor, nested) {
    run_nested(1); /* one worker thread plus the main thread */
    run_nested(0); /* the default: one per core minus one */
}

/* ---------- deep recursion (wait depth beyond DE__TASK_MAX_WAIT_DEPTH) ---------- */

typedef struct deep_ctx {
    de_task_system *ts;
    u32 depth;
    de_atomic_u32 *leaves;
} deep_ctx;

static void deep_chunk(u32 begin, u32 end, void *user) {
    const deep_ctx *c = (const deep_ctx *)user;
    for (u32 i = begin; i < end; ++i) {
        if (i == 0 && c->depth > 0) {
            deep_ctx inner = {c->ts, c->depth - 1, c->leaves};
            de_task_parallel_for(c->ts, 4, 1, deep_chunk, &inner);
        } else {
            de_atomic_fetch_add_u32(c->leaves, 1, DE_ATOMIC_RELAXED);
        }
    }
}

TEST(pfor, deep_recursion) {
    for (u32 w = 0; w < 2; ++w) {
        de_task_system *ts = make_ts(w == 0 ? 1 : 0);
        de_atomic_u32 leaves;
        de_atomic_init_u32(&leaves, 0);
        enum { DEPTH = 100 };
        deep_ctx c = {ts, DEPTH, &leaves};
        for (u32 r = 0; r < 20; ++r)
            de_task_parallel_for(ts, 4, 1, deep_chunk, &c);
        /* Each level has 3 leaves; the deepest has 4. */
        EXPECT_EQ(de_atomic_load_u32(&leaves, DE_ATOMIC_RELAXED), 20u * (3u * DEPTH + 4u));
        de_task_system_destroy(ts);
    }
}

/* ---------- single worker ---------- */

TEST(pfor, single_worker) {
    de_task_system *ts = make_ts(1);
    ASSERT_EQ(de_task_worker_count(ts), 2u);
    exact_cover_all(ts);
    boundaries_all(ts);
    de_task_system_destroy(ts);
}

/* ---------- from other threads ---------- */

typedef struct ext_ctx {
    de_task_system *ts;
    de_atomic_u32 *hits;
} ext_ctx;

static void ext_hit(u32 begin, u32 end, void *user) {
    de_atomic_u32 *hits = (de_atomic_u32 *)user;
    for (u32 i = begin; i < end; ++i)
        de_atomic_fetch_add_u32(&hits[i], 1, DE_ATOMIC_RELAXED);
}

static i32 ext_thread(void *p) {
    ext_ctx *c = (ext_ctx *)p;
    de_task_parallel_for(c->ts, 10000, 16, ext_hit, c->hits);
    return 0;
}

static void bg_task(void *p) {
    ext_ctx *c = (ext_ctx *)p;
    de_task_parallel_for(c->ts, 10000, 16, ext_hit, c->hits + 10000);
}

TEST(pfor, other_threads) {
    de_task_system *ts = make_ts(0);
    de_atomic_u32 *hits = (de_atomic_u32 *)calloc(20000, sizeof(de_atomic_u32));
    ASSERT_TRUE(hits != NULL);
    ext_ctx c = {ts, hits};
    /* From a thread that is not a worker, and from a background task at the same time. */
    de_task_counter bg = {0};
    de_task_submit(ts, DE_TASK_POOL_BACKGROUND, bg_task, &c, &bg);
    de_thread *t = NULL;
    ASSERT_EQ(de_thread_create(ext_thread, &c, "ext", DE_THREAD_NORMAL, 0, &t), DE_OK);
    EXPECT_EQ(de_thread_join(t), 0);
    de_task_wait(ts, &bg);
    u32 bad = 0;
    for (u32 i = 0; i < 20000; ++i)
        bad += de_atomic_load_u32(&hits[i], DE_ATOMIC_RELAXED) != 1u;
    EXPECT_EQ(bad, 0u);
    free(hits);
    de_task_system_destroy(ts);
}
