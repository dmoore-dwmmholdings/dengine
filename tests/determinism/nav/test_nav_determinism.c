/* DENG-77: a fixed scene (floor, walls, blocks, an off-mesh link) is built into a navmesh, then
 * 10 000 seeded queries (find_path, raycast, nearest_point) run in deterministic mode. Step 0 is
 * the navmesh hash; steps 1..10 are an FNV-1a hash of every result so far, after each block of
 * 1 000 queries. They must match the golden file on every platform. Regenerate the golden with
 * DE_DET_UPDATE_NAV_GOLDEN=1, only for an intended change of the build or the queries.
 *
 * Known gap: Recast's build itself still calls libm (sqrtf, cosf), so step 0 can differ across
 * platforms until that is replaced; the queries use de_dmath (de_detour_math.h). */
#include "de_det_harness.h"
#include "de_test.h"
#include "gameplay/nav/nav_internal.h"

#include <dengine/de_gameplay.h>
#include <dengine/de_math.h>
#include <dengine/de_platform.h>

#include <stdlib.h>
#include <string.h>

#define SEED    0x77u
#define BLOCKS  10u
#define PER     1000u
#define STEPS   (BLOCKS + 1u)

static de_dvec3 P(f64 x, f64 y, f64 z) {
    de_dvec3 p;
    p.x = x;
    p.y = y;
    p.z = z;
    return p;
}

static void quad(de_nav *n, de_dvec3 a, de_dvec3 b, de_dvec3 c, de_dvec3 d) {
    const de_dvec3 t[6] = {a, b, c, a, c, d};
    ASSERT_EQ(de__nav_add_test_tris(n, t, 2), DE_OK);
}

static void block(de_nav *n, f64 x0, f64 y0, f64 x1, f64 y1, f64 h) {
    quad(n, P(x0, y0, h), P(x1, y0, h), P(x1, y1, h), P(x0, y1, h));
    quad(n, P(x0, y0, 0), P(x1, y0, 0), P(x1, y0, h), P(x0, y0, h));
    quad(n, P(x1, y1, 0), P(x0, y1, 0), P(x0, y1, h), P(x1, y1, h));
    quad(n, P(x0, y1, 0), P(x0, y0, 0), P(x0, y0, h), P(x0, y1, h));
    quad(n, P(x1, y0, 0), P(x1, y1, 0), P(x1, y1, h), P(x1, y0, h));
}

static u64 fnv(u64 h, const void *data, size_t size) {
    const u8 *p = (const u8 *)data;
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static u64 fnv_point(u64 h, de_dvec3 p) {
    const f64 v[3] = {p.x, p.y, p.z};
    return fnv(h, v, sizeof v);
}

static de_dvec3 rand_point(de_rng *r) {
    return P(de_rng_f64(r) * 100.0, de_rng_f64(r) * 100.0, de_rng_f64(r) * 0.5);
}

static void run(u64 *out) {
    de_log_desc ld;
    memset(&ld, 0, sizeof ld);
    ld.no_console_sink = true;
    ASSERT_EQ(de_log_init(&ld), DE_OK);
    de_fp_env_init_thread();

    de_nav *n = NULL;
    ASSERT_EQ(de_nav_create(NULL, NULL, &n), DE_OK);
    de_nav_set_deterministic(n, true);
    quad(n, P(0, 0, 0), P(60, 0, 0), P(60, 100, 0), P(0, 100, 0));
    quad(n, P(64, 0, 0), P(100, 0, 0), P(100, 100, 0), P(64, 100, 0));
    block(n, 20, 10, 21, 70, 3);
    block(n, 35, 30, 36, 100, 3);
    block(n, 45, 45, 52, 52, 2);
    block(n, 75, 20, 90, 24, 3);
    ASSERT_EQ(de_nav_add_offmesh_link(n, P(59, 50, 0), P(65, 50, 0), 0.5f, true, 1), DE_OK);
    ASSERT_EQ(de_nav_add_offmesh_link(n, P(59, 10, 0), P(65, 10, 0), 0.5f, false, 2), DE_OK);

    de_navmesh_build_desc d;
    memset(&d, 0, sizeof d);
    d.cell_size = 0.25f;
    d.cell_height = 0.1f;
    d.agent_radius = 0.5f;
    d.agent_height = 2.0f;
    d.agent_max_climb = 0.4f;
    d.agent_max_slope_rad = 0.785398163f;
    d.tile_size = 32.0f;
    de_aabb all;
    all.min.x = 0;
    all.min.y = 0;
    all.min.z = -1;
    all.max.x = 100;
    all.max.y = 100;
    all.max.z = 4;
    ASSERT_EQ(de_nav_rebuild_tiles(n, &d, all), DE_OK);
    de_nav_update(n);
    ASSERT_EQ(de__nav_pending_jobs(n), 0u);
    out[0] = de__nav_mesh_checksum(n);

    de_rng r = de_rng_seed(SEED, 0);
    u64 h = 0xcbf29ce484222325ull;
    for (u32 b = 0; b < BLOCKS; ++b) {
        for (u32 i = 0; i < PER; ++i) {
            const de_dvec3 a = rand_point(&r), e = rand_point(&r);
            const u32 kind = de_rng_range(&r, 0, 3);
            if (kind == 0) {
                const u32 mask = de_rng_range(&r, 0, 8);
                de_nav_path p;
                const de_result res = de_nav_find_path(n, a, e, mask, NULL, &p);
                const u32 head[3] = {(u32)res, p.count, p.partial ? 1u : 0u};
                h = fnv(h, head, sizeof head);
                for (u32 k = 0; k < p.count; ++k)
                    h = fnv_point(h, p.points[k]);
                de_nav_path_free(NULL, &p);
            } else if (kind == 1) {
                de_dvec3 hit;
                const u8 got = de_nav_raycast(n, a, e, &hit) ? 1u : 0u;
                h = fnv(h, &got, 1);
                h = fnv_point(h, hit);
            } else {
                de_dvec3 o = P(0, 0, 0);
                const u8 got = de_nav_nearest_point(n, a, 1.0f + de_rng_f32(&r) * 3.0f, &o) ? 1u
                                                                                             : 0u;
                h = fnv(h, &got, 1);
                if (got)
                    h = fnv_point(h, o);
            }
        }
        out[1 + b] = h;
    }
    de_nav_destroy(n);
    de_log_shutdown();
}

TEST(nav_determinism, repeat_runs_match) {
    u64 a[STEPS], b[STEPS];
    run(a);
    run(b);
    EXPECT_EQ(de_det_report("nav run 1 vs 2", a, b, STEPS), DE_DET_NO_DIVERGENCE);
}

TEST(nav_determinism, queries_match_golden) {
    u64 actual[STEPS];
    run(actual);
    const char *update = getenv("DE_DET_UPDATE_NAV_GOLDEN");
    if (update != NULL && update[0] == '1')
        ASSERT_EQ(de_det_write_checksums(DE_NAV_DET_GOLDEN, "nav_queries", SEED, actual, STEPS),
                  DE_OK);
    ASSERT_EQ(de_det_write_checksums(DE_DET_OUT_DIR "/nav_queries.checksums", "nav_queries", SEED,
                                     actual, STEPS),
              DE_OK);
    u64 golden[STEPS];
    u32 count = 0;
    ASSERT_EQ(de_det_read_checksums(DE_NAV_DET_GOLDEN, golden, STEPS, &count), DE_OK);
    ASSERT_EQ(count, STEPS);
    EXPECT_EQ(de_det_report("nav vs golden", golden, actual, STEPS), DE_DET_NO_DIVERGENCE);
}
