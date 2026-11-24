/* DENG-79: 2D navigation gives the same bytes on every platform (SPEC sections 5.7 and 21.2).
 * A seeded 128x128 grid and a seeded polygon mesh (one outline, 15 holes) take 1 000 queries:
 * grid A*, grid JPS, mesh find_path and mesh nearest_point in turn. Step 0 is the mesh hash;
 * steps 1..10 are an FNV-1a hash of every result so far, after each block of 100 queries.
 * They must match the golden file nav2d_1000_checksums.txt on every platform. Regenerate it with
 * DE_DET_UPDATE_NAV2D_GOLDEN=1, only for an intended change of the algorithms or the queries. */
#include "de_det_harness.h"
#include "de_test.h"
#include "gameplay/nav/nav2d_internal.h"

#include <dengine/de_gameplay.h>
#include <dengine/de_math.h>
#include <dengine/de_platform.h>

#include <stdlib.h>
#include <string.h>

#define SEED   0x79u
#define BLOCKS 10u
#define PER    100u
#define STEPS  (BLOCKS + 1u)
#define GW     128u
#define HX     5u
#define HY     3u

static u64 fnv(u64 h, const void *data, size_t size) {
    const u8 *p = (const u8 *)data;
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static de_vec2 v2(f32 x, f32 y) {
    de_vec2 v;
    v.x = x;
    v.y = y;
    return v;
}

static void run(u64 *out) {
    de_log_desc ld;
    memset(&ld, 0, sizeof ld);
    ld.no_console_sink = true;
    ASSERT_EQ(de_log_init(&ld), DE_OK);
    de_fp_env_init_thread();
    de_rng r = de_rng_seed(SEED, 0);

    static u8 cells[GW * GW];
    for (u32 i = 0; i < GW * GW; ++i)
        cells[i] = de_rng_f32(&r) < 0.3f ? 0u : 1u;
    de_nav2d_grid g;
    memset(&g, 0, sizeof g);
    g.width = GW;
    g.height = GW;
    g.cell_size = 0.25f;
    g.origin = v2(-3.0f, 1.5f);
    g.walkable = cells;

    /* Outline 50 x 30; one quad hole per 10 x 10 cell, its corners jittered inside the cell. */
    static const de_vec2 outline_pts[4] = {{0, 0}, {50, 0}, {50, 30}, {0, 30}};
    static de_vec2 hole_pts[HX * HY][4];
    de_nav2d_polygon holes[HX * HY];
    for (u32 j = 0; j < HY; ++j)
        for (u32 i = 0; i < HX; ++i) {
            const f32 x0 = 10.0f * (f32)i, y0 = 10.0f * (f32)j;
            de_vec2 *q = hole_pts[j * HX + i];
            q[0] = v2(x0 + 2.0f + de_rng_f32(&r) * 2.0f, y0 + 2.0f + de_rng_f32(&r) * 2.0f);
            q[1] = v2(x0 + 6.0f + de_rng_f32(&r) * 2.0f, y0 + 2.0f + de_rng_f32(&r) * 2.0f);
            q[2] = v2(x0 + 6.0f + de_rng_f32(&r) * 2.0f, y0 + 6.0f + de_rng_f32(&r) * 2.0f);
            q[3] = v2(x0 + 2.0f + de_rng_f32(&r) * 2.0f, y0 + 6.0f + de_rng_f32(&r) * 2.0f);
            holes[j * HX + i].points = q;
            holes[j * HX + i].count = 4;
        }
    de_nav2d_polygon outline = {outline_pts, 4};
    de_nav2d_mesh_desc d;
    memset(&d, 0, sizeof d);
    d.outlines = &outline;
    d.outline_count = 1;
    d.holes = holes;
    d.hole_count = HX * HY;
    de_nav2d_mesh *m = NULL;
    ASSERT_EQ(de_nav2d_mesh_build(&d, NULL, &m), DE_OK);

    u64 h = 0xcbf29ce484222325ull;
    h = fnv(h, m->verts, sizeof(de_vec2) * m->vert_count);
    h = fnv(h, m->tris, sizeof(de__nav2d_tri) * m->tri_count);
    h = fnv(h, m->corners, sizeof(u32) * m->corner_count);
    h = fnv(h, m->vis_nbr, sizeof(u32) * m->vis_count);
    out[0] = h;

    for (u32 b = 0; b < BLOCKS; ++b) {
        for (u32 q = 0; q < PER; ++q) {
            const u32 kind = q % 4u;
            de_vec2 *p = NULL;
            u32 n = 0;
            de_result res = DE_OK;
            if (kind < 2u) {
                const de_vec2 a = v2(-3.5f + de_rng_f32(&r) * 33.0f, 1.0f + de_rng_f32(&r) * 33.0f);
                const de_vec2 c = v2(-3.5f + de_rng_f32(&r) * 33.0f, 1.0f + de_rng_f32(&r) * 33.0f);
                res = de_nav2d_find_path(&g, a, c, kind == 1u, NULL, &p, &n);
            } else if (kind == 2u) {
                const de_vec2 a = v2(de_rng_f32(&r) * 50.0f, de_rng_f32(&r) * 30.0f);
                const de_vec2 c = v2(de_rng_f32(&r) * 50.0f, de_rng_f32(&r) * 30.0f);
                res = de_nav2d_mesh_find_path(m, a, c, NULL, &p, &n);
            } else {
                const de_vec2 a = v2(-5.0f + de_rng_f32(&r) * 60.0f, -5.0f + de_rng_f32(&r) * 40.0f);
                de_vec2 o = v2(0, 0);
                const bool ok = de_nav2d_mesh_nearest_point(m, a, &o);
                h = fnv(h, &ok, sizeof ok);
                h = fnv(h, &o, sizeof o);
            }
            const i32 code = (i32)res;
            h = fnv(h, &code, sizeof code);
            h = fnv(h, &n, sizeof n);
            if (n)
                h = fnv(h, p, sizeof(de_vec2) * n);
            de_nav2d_path_free(NULL, p, n);
        }
        out[1 + b] = h;
    }
    de_nav2d_mesh_destroy(m);
    de_log_shutdown();
}

TEST(nav2d_determinism, repeat_runs_match) {
    u64 a[STEPS], b[STEPS];
    run(a);
    run(b);
    EXPECT_EQ(de_det_report("nav2d run 1 vs 2", a, b, STEPS), DE_DET_NO_DIVERGENCE);
}

TEST(nav2d_determinism, nav2d_1000_matches_golden) {
    u64 actual[STEPS];
    run(actual);
    const char *update = getenv("DE_DET_UPDATE_NAV2D_GOLDEN");
    if (update != NULL && update[0] == '1')
        ASSERT_EQ(de_det_write_checksums(DE_NAV2D_DET_GOLDEN, "nav2d_1000", SEED, actual, STEPS),
                  DE_OK);
    ASSERT_EQ(de_det_write_checksums(DE_DET_OUT_DIR "/nav2d_1000.checksums", "nav2d_1000", SEED,
                                     actual, STEPS),
              DE_OK);
    u64 golden[STEPS];
    u32 count = 0;
    ASSERT_EQ(de_det_read_checksums(DE_NAV2D_DET_GOLDEN, golden, STEPS, &count), DE_OK);
    ASSERT_EQ(count, STEPS);
    EXPECT_EQ(de_det_report("nav2d vs golden", golden, actual, STEPS), DE_DET_NO_DIVERGENCE);
}
