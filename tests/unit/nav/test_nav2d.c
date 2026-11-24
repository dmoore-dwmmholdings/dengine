/*
 * test_nav2d.c: 2D navigation (DENG-79; src/gameplay/nav/nav2d_grid.c, nav2d_mesh.c).
 * CTest names nav2d.<test>.
 *
 *   test_nav2d_astar_vs_jps_cost   100 seeded 256x256 grids: A* and JPS paths have equal cost,
 *                                  both are valid grid walks, JPS expands fewer nodes on average
 *   test_nav2d_no_corner_cut       a diagonal past a blocked corner is refused in both modes
 *   test_nav2d_blocked_endpoints   blocked or outside start/goal, and no path: DE_ERR_NOT_FOUND,
 *                                  with no memory left behind
 *   test_nav2d_grid_alloc          a query allocates scratch and the points only, from `a`
 *   test_nav2d_mesh_holes          a mesh with two holes: every path segment stays walkable
 *   test_nav2d_mesh_vs_visibility_graph  path lengths equal a brute-force visibility graph
 *   test_nav2d_mesh_nearest_point  inside points stay, outside points snap to the boundary
 *   test_nav2d_mesh_alloc          build and queries use only the given allocator
 *   test_nav2d_order_and_thread    the same queries give the same bytes in any order or thread
 *   test_nav2d_plane_mapping       plane <-> world for DE_PLANE_SIDE_XZ and DE_PLANE_TOP_XY
 */
#include "de_test.h"

#include "core/test_counting_alloc.h"

#include "gameplay/nav/nav2d_internal.h"

#include <dengine/de_gameplay.h>
#include <dengine/de_math.h>
#include <dengine/de_platform.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

static de_vec2 v2(f32 x, f32 y) {
    de_vec2 v;
    v.x = x;
    v.y = y;
    return v;
}

/* ---------- grid helpers ---------- */

#define GW 256u
#define GH 256u

static void random_grid(de_rng *r, u8 *cells, u32 w, u32 h, f32 blocked) {
    for (u32 i = 0; i < w * h; ++i)
        cells[i] = de_rng_f32(r) < blocked ? 0u : 1u;
}

static de_vec2 random_open_cell(de_rng *r, const de_nav2d_grid *g) {
    for (;;) {
        const u32 x = de_rng_range(r, 0, g->width), y = de_rng_range(r, 0, g->height);
        if (g->walkable[y * g->width + x])
            return v2(g->origin.x + ((f32)x + 0.5f) * g->cell_size,
                      g->origin.y + ((f32)y + 0.5f) * g->cell_size);
    }
}

static bool open_cell(const de_nav2d_grid *g, i32 x, i32 y) {
    return x >= 0 && y >= 0 && x < (i32)g->width && y < (i32)g->height &&
           g->walkable[(u32)y * g->width + (u32)x] != 0;
}

/* Checks that the points are a legal walk (pure straight or diagonal segments over open
 * cells, no corner cut) and returns its cost in DE__NAV2D_COST_* units; UINT64_MAX if not. */
static u64 walk_cost(const de_nav2d_grid *g, const de_vec2 *p, u32 n) {
    u64 cost = 0;
    for (u32 i = 0; i + 1u < n; ++i) {
        i32 x = (i32)((p[i].x - g->origin.x) / g->cell_size);
        i32 y = (i32)((p[i].y - g->origin.y) / g->cell_size);
        const i32 x1 = (i32)((p[i + 1u].x - g->origin.x) / g->cell_size);
        const i32 y1 = (i32)((p[i + 1u].y - g->origin.y) / g->cell_size);
        const i32 ax = x1 > x ? x1 - x : x - x1, ay = y1 > y ? y1 - y : y - y1;
        if (ax != 0 && ay != 0 && ax != ay)
            return UINT64_MAX;
        const i32 dx = (x1 > x) - (x1 < x), dy = (y1 > y) - (y1 < y);
        while (x != x1 || y != y1) {
            if (dx != 0 && dy != 0 && !(open_cell(g, x + dx, y) && open_cell(g, x, y + dy)))
                return UINT64_MAX;
            x += dx;
            y += dy;
            if (!open_cell(g, x, y))
                return UINT64_MAX;
            cost += (dx != 0 && dy != 0) ? DE__NAV2D_COST_DIAGONAL : DE__NAV2D_COST_STRAIGHT;
        }
    }
    return cost;
}

TEST(nav2d, test_nav2d_astar_vs_jps_cost) {
    static u8 cells[GW * GH];
    de_nav2d_grid g;
    memset(&g, 0, sizeof g);
    g.width = GW;
    g.height = GH;
    g.cell_size = 0.5f;
    g.origin = v2(-10.0f, 4.0f);
    g.walkable = cells;
    de_rng r = de_rng_seed(0x79u, 1);
    u64 exp_astar = 0, exp_jps = 0;
    u32 found = 0;
    for (u32 k = 0; k < 100u; ++k) {
        random_grid(&r, cells, GW, GH, 0.25f + 0.1f * (f32)(k % 3u));
        const de_vec2 a = random_open_cell(&r, &g), b = random_open_cell(&r, &g);
        de_vec2 *pa = NULL, *pj = NULL;
        u32 na = 0, nj = 0;
        de__nav2d_grid_stats sa, sj;
        const de_result ra = de__nav2d_find_path_stats(&g, a, b, false, NULL, &pa, &na, &sa);
        const de_result rj = de__nav2d_find_path_stats(&g, a, b, true, NULL, &pj, &nj, &sj);
        ASSERT_EQ(ra, rj);
        exp_astar += sa.expanded;
        exp_jps += sj.expanded;
        if (ra == DE_OK) {
            found++;
            EXPECT_EQ(sa.cost, sj.cost);
            EXPECT_EQ(walk_cost(&g, pa, na), sa.cost);
            EXPECT_EQ(walk_cost(&g, pj, nj), sj.cost);
            EXPECT_TRUE(pa[0].x == a.x && pa[0].y == a.y);
            EXPECT_TRUE(pj[nj - 1u].x == b.x && pj[nj - 1u].y == b.y);
        }
        de_nav2d_path_free(NULL, pa, na);
        de_nav2d_path_free(NULL, pj, nj);
    }
    EXPECT_GT(found, 50u);
    EXPECT_LT(exp_jps, exp_astar);
}

TEST(nav2d, test_nav2d_no_corner_cut) {
    /* 3x3, (1,0) blocked: (0,0) -> (1,1) may not step diagonally past the blocked corner. */
    u8 cells[9] = {1, 0, 1, 1, 1, 1, 1, 1, 1};
    de_nav2d_grid g;
    memset(&g, 0, sizeof g);
    g.width = 3;
    g.height = 3;
    g.cell_size = 1.0f;
    g.walkable = cells;
    for (u32 mode = 0; mode < 2u; ++mode) {
        de_vec2 *p = NULL;
        u32 n = 0;
        de__nav2d_grid_stats st;
        ASSERT_EQ(de__nav2d_find_path_stats(&g, v2(0.5f, 0.5f), v2(1.5f, 1.5f), mode == 1u, NULL,
                                            &p, &n, &st),
                  DE_OK);
        EXPECT_EQ(st.cost, 2u * DE__NAV2D_COST_STRAIGHT);
        EXPECT_EQ(n, 3u);
        EXPECT_EQ(walk_cost(&g, p, n), st.cost);
        de_nav2d_path_free(NULL, p, n);
    }
    /* Two blocked corners seal (0,0) off from (1,1) entirely. */
    cells[3] = 0;
    for (u32 mode = 0; mode < 2u; ++mode) {
        de_vec2 *p = NULL;
        u32 n = 0;
        EXPECT_EQ(de_nav2d_find_path(&g, v2(0.5f, 0.5f), v2(2.5f, 2.5f), mode == 1u, NULL, &p, &n),
                  DE_ERR_NOT_FOUND);
        EXPECT_TRUE(p == NULL);
    }
}

TEST(nav2d, test_nav2d_blocked_endpoints) {
    u8 cells[16];
    memset(cells, 1, sizeof cells);
    cells[5] = 0;             /* (1,1) */
    cells[2] = cells[6] = 0;  /* (2,0), (2,1) */
    cells[10] = cells[14] = 0; /* (2,2), (2,3): column 2 is a wall */
    de_nav2d_grid g;
    memset(&g, 0, sizeof g);
    g.width = 4;
    g.height = 4;
    g.cell_size = 1.0f;
    g.walkable = cells;
    test_counting_alloc ca;
    test_counting_init(&ca);
    for (u32 mode = 0; mode < 2u; ++mode) {
        const bool jps = mode == 1u;
        de_vec2 *p = NULL;
        u32 n = 7;
        EXPECT_EQ(de_nav2d_find_path(&g, v2(1.5f, 1.5f), v2(0.5f, 0.5f), jps, &ca.iface, &p, &n),
                  DE_ERR_NOT_FOUND);
        EXPECT_EQ(n, 0u);
        EXPECT_EQ(de_nav2d_find_path(&g, v2(0.5f, 0.5f), v2(1.5f, 1.5f), jps, &ca.iface, &p, &n),
                  DE_ERR_NOT_FOUND);
        EXPECT_EQ(de_nav2d_find_path(&g, v2(-0.5f, 0.5f), v2(0.5f, 0.5f), jps, &ca.iface, &p, &n),
                  DE_ERR_NOT_FOUND);
        EXPECT_EQ(de_nav2d_find_path(&g, v2(0.5f, 0.5f), v2(0.5f, 4.5f), jps, &ca.iface, &p, &n),
                  DE_ERR_NOT_FOUND);
        /* Unreachable: the far side of the wall. */
        EXPECT_EQ(de_nav2d_find_path(&g, v2(0.5f, 0.5f), v2(3.5f, 0.5f), jps, &ca.iface, &p, &n),
                  DE_ERR_NOT_FOUND);
        EXPECT_TRUE(p == NULL);
        /* Same cell: one point. */
        ASSERT_EQ(de_nav2d_find_path(&g, v2(0.2f, 0.2f), v2(0.9f, 0.7f), jps, &ca.iface, &p, &n),
                  DE_OK);
        EXPECT_EQ(n, 1u);
        de_nav2d_path_free(&ca.iface, p, n);
    }
    EXPECT_EQ(ca.live_blocks, 0u);
    EXPECT_EQ(ca.size_errors, 0u);
    de_nav2d_grid bad = g;
    bad.cell_size = 0.0f;
    de_vec2 *p = NULL;
    u32 n = 0;
    EXPECT_EQ(de_nav2d_find_path(&bad, v2(0.5f, 0.5f), v2(0.5f, 0.5f), false, NULL, &p, &n),
              DE_ERR_INVALID_ARGUMENT);
}

TEST(nav2d, test_nav2d_grid_alloc) {
    static u8 cells[64 * 64];
    de_rng r = de_rng_seed(0x2du, 0);
    random_grid(&r, cells, 64, 64, 0.2f);
    cells[0] = cells[64 * 64 - 1] = 1;
    de_nav2d_grid g;
    memset(&g, 0, sizeof g);
    g.width = 64;
    g.height = 64;
    g.cell_size = 1.0f;
    g.walkable = cells;
    for (u32 mode = 0; mode < 2u; ++mode) {
        test_counting_alloc ca;
        test_counting_init(&ca);
        de_vec2 *p = NULL;
        u32 n = 0;
        const de_result res =
            de_nav2d_find_path(&g, v2(0.5f, 0.5f), v2(63.5f, 63.5f), mode == 1u, &ca.iface, &p, &n);
        if (res == DE_OK) {
            EXPECT_EQ(ca.allocs, 2u); /* scratch, then the points */
            EXPECT_EQ(ca.live_blocks, 1u);
            de_nav2d_path_free(&ca.iface, p, n);
        }
        EXPECT_EQ(ca.live_blocks, 0u);
        EXPECT_EQ(ca.size_errors, 0u);
        /* An allocator that fails: out of memory, nothing leaked. */
        test_counting_init(&ca);
        ca.fail_after = 0;
        EXPECT_EQ(de_nav2d_find_path(&g, v2(0.5f, 0.5f), v2(63.5f, 63.5f), mode == 1u, &ca.iface,
                                     &p, &n),
                  DE_ERR_OUT_OF_MEMORY);
        EXPECT_EQ(ca.live_blocks, 0u);
    }
}

/* ---------- mesh helpers ---------- */

static const de_vec2 OUTER[] = {{0, 0}, {20, 0}, {20, 10}, {0, 10}};
static const de_vec2 HOLE_A[] = {{4, 2}, {8, 2}, {8, 7}, {4, 7}};           /* CCW given */
static const de_vec2 HOLE_B[] = {{12, 2}, {13, 8}, {16, 5}};                 /* CW given */

typedef struct ring {
    const de_vec2 *p;
    u32 n;
} ring;

static const ring RINGS[3] = {{OUTER, 4}, {HOLE_A, 4}, {HOLE_B, 3}};

static de_nav2d_mesh *two_hole_mesh(const de_allocator *a) {
    de_nav2d_polygon outline = {OUTER, 4};
    de_nav2d_polygon holes[2] = {{HOLE_A, 4}, {HOLE_B, 3}};
    de_nav2d_mesh_desc d;
    memset(&d, 0, sizeof d);
    d.outlines = &outline;
    d.outline_count = 1;
    d.holes = holes;
    d.hole_count = 2;
    de_nav2d_mesh *m = NULL;
    if (de_nav2d_mesh_build(&d, a, &m) != DE_OK)
        return NULL;
    return m;
}

static f64 cr(f64 ax, f64 ay, f64 bx, f64 by, f64 cx, f64 cy) {
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

static bool in_poly(const ring *r, f64 x, f64 y) {
    bool in = false;
    for (u32 i = 0, j = r->n - 1u; i < r->n; j = i++) {
        const f64 xi = r->p[i].x, yi = r->p[i].y, xj = r->p[j].x, yj = r->p[j].y;
        if ((yi > y) != (yj > y) && x < xi + (y - yi) / (yj - yi) * (xj - xi))
            in = !in;
    }
    return in;
}

static f64 seg_dist(f64 px, f64 py, f64 ax, f64 ay, f64 bx, f64 by) {
    const f64 dx = bx - ax, dy = by - ay;
    f64 t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy);
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    const f64 qx = ax + t * dx - px, qy = ay + t * dy - py;
    return sqrt(qx * qx + qy * qy);
}

static f64 boundary_dist(f64 x, f64 y) {
    f64 best = 1e300;
    for (u32 k = 0; k < 3u; ++k)
        for (u32 i = 0; i < RINGS[k].n; ++i) {
            const de_vec2 a = RINGS[k].p[i], b = RINGS[k].p[(i + 1u) % RINGS[k].n];
            const f64 d = seg_dist(x, y, a.x, a.y, b.x, b.y);
            best = d < best ? d : best;
        }
    return best;
}

/* Walkable: inside or on the outer ring and not strictly inside a hole. */
static bool walkable_pt(f64 x, f64 y) {
    if (boundary_dist(x, y) < 1e-6)
        return true;
    return in_poly(&RINGS[0], x, y) && !in_poly(&RINGS[1], x, y) && !in_poly(&RINGS[2], x, y);
}

/* A segment is walkable when it crosses no boundary edge and sampled points are walkable. */
static bool seg_walkable(f64 ax, f64 ay, f64 bx, f64 by) {
    for (u32 k = 0; k < 3u; ++k)
        for (u32 i = 0; i < RINGS[k].n; ++i) {
            const de_vec2 p = RINGS[k].p[i], q = RINGS[k].p[(i + 1u) % RINGS[k].n];
            const f64 d1 = cr(ax, ay, bx, by, p.x, p.y), d2 = cr(ax, ay, bx, by, q.x, q.y);
            const f64 d3 = cr(p.x, p.y, q.x, q.y, ax, ay), d4 = cr(p.x, p.y, q.x, q.y, bx, by);
            if (d1 * d2 < -1e-12 && d3 * d4 < -1e-12)
                return false;
        }
    for (u32 s = 1; s < 16u; ++s) {
        const f64 t = (f64)s / 16.0;
        if (!walkable_pt(ax + (bx - ax) * t, ay + (by - ay) * t))
            return false;
    }
    return true;
}

static de_vec2 random_walkable(de_rng *r) {
    for (;;) {
        const de_vec2 p = v2(0.05f + de_rng_f32(r) * 19.9f, 0.05f + de_rng_f32(r) * 9.9f);
        if (walkable_pt(p.x, p.y) && boundary_dist(p.x, p.y) > 1e-3)
            return p;
    }
}

static f64 path_len(const de_vec2 *p, u32 n) {
    f64 s = 0;
    for (u32 i = 0; i + 1u < n; ++i) {
        const f64 dx = (f64)p[i + 1u].x - p[i].x, dy = (f64)p[i + 1u].y - p[i].y;
        s += sqrt(dx * dx + dy * dy);
    }
    return s;
}

/* Brute-force reference: Dijkstra over the visibility graph of {from, to, ring vertices}. */
static f64 visibility_len(de_vec2 from, de_vec2 to) {
    enum { MAXN = 16 };
    f64 px[MAXN], py[MAXN];
    u32 n = 0;
    px[n] = from.x;
    py[n++] = from.y;
    px[n] = to.x;
    py[n++] = to.y;
    for (u32 k = 0; k < 3u; ++k)
        for (u32 i = 0; i < RINGS[k].n; ++i) {
            px[n] = RINGS[k].p[i].x;
            py[n++] = RINGS[k].p[i].y;
        }
    f64 d[MAXN];
    bool done[MAXN];
    for (u32 i = 0; i < n; ++i) {
        d[i] = 1e300;
        done[i] = false;
    }
    d[0] = 0;
    for (;;) {
        u32 u = MAXN;
        for (u32 i = 0; i < n; ++i)
            if (!done[i] && (u == MAXN || d[i] < d[u]))
                u = i;
        if (u == MAXN || d[u] >= 1e300)
            break;
        done[u] = true;
        if (u == 1u)
            break;
        for (u32 v = 0; v < n; ++v) {
            if (done[v] || !seg_walkable(px[u], py[u], px[v], py[v]))
                continue;
            const f64 dx = px[v] - px[u], dy = py[v] - py[u];
            const f64 nd = d[u] + sqrt(dx * dx + dy * dy);
            if (nd < d[v])
                d[v] = nd;
        }
    }
    return d[1];
}

TEST(nav2d, test_nav2d_mesh_holes) {
    de_nav2d_mesh *m = two_hole_mesh(NULL);
    ASSERT_TRUE(m != NULL);
    /* Every triangle lies in the walkable area, and the areas add up: 200 - 20 - 10.5. */
    f64 area = 0;
    for (u32 t = 0; t < m->tri_count; ++t) {
        const de_vec2 a = m->verts[m->tris[t].v[0]], b = m->verts[m->tris[t].v[1]],
                      c = m->verts[m->tris[t].v[2]];
        const f64 ar = cr(a.x, a.y, b.x, b.y, c.x, c.y);
        EXPECT_TRUE(ar > 0.0);
        area += ar * 0.5;
        EXPECT_TRUE(walkable_pt((a.x + b.x + c.x) / 3.0, (a.y + b.y + c.y) / 3.0));
    }
    EXPECT_FLOAT_EQ(area, 200.0 - 20.0 - 10.5, 1e-9);
    de_rng r = de_rng_seed(0x2du, 7);
    for (u32 q = 0; q < 300u; ++q) {
        const de_vec2 a = random_walkable(&r), b = random_walkable(&r);
        de_vec2 *p = NULL;
        u32 n = 0;
        ASSERT_EQ(de_nav2d_mesh_find_path(m, a, b, NULL, &p, &n), DE_OK);
        ASSERT_GE(n, 2u);
        EXPECT_TRUE(p[0].x == a.x && p[0].y == a.y);
        EXPECT_TRUE(p[n - 1u].x == b.x && p[n - 1u].y == b.y);
        for (u32 i = 0; i + 1u < n; ++i)
            EXPECT_TRUE(seg_walkable(p[i].x, p[i].y, p[i + 1u].x, p[i + 1u].y));
        de_nav2d_path_free(NULL, p, n);
    }
    /* Outside the mesh (in a hole, or beyond the outline). */
    de_vec2 *p = NULL;
    u32 n = 0;
    EXPECT_EQ(de_nav2d_mesh_find_path(m, v2(6, 4), v2(1, 1), NULL, &p, &n), DE_ERR_NOT_FOUND);
    EXPECT_EQ(de_nav2d_mesh_find_path(m, v2(1, 1), v2(25, 1), NULL, &p, &n), DE_ERR_NOT_FOUND);
    de_nav2d_mesh_destroy(m);
}

TEST(nav2d, test_nav2d_mesh_vs_visibility_graph) {
    de_nav2d_mesh *m = two_hole_mesh(NULL);
    ASSERT_TRUE(m != NULL);
    de_rng r = de_rng_seed(0x79u, 3);
    u32 worse = 0;
    for (u32 q = 0; q < 300u; ++q) {
        const de_vec2 a = random_walkable(&r), b = random_walkable(&r);
        de_vec2 *p = NULL;
        u32 n = 0;
        ASSERT_EQ(de_nav2d_mesh_find_path(m, a, b, NULL, &p, &n), DE_OK);
        const f64 got = path_len(p, n), ref = visibility_len(a, b);
        EXPECT_TRUE(got >= ref - 1e-4);
        if (got > ref + 1e-4)
            worse++;
        de_nav2d_path_free(NULL, p, n);
    }
    printf("nav2d: %u of 300 mesh paths longer than the visibility-graph reference\n", worse);
    EXPECT_EQ(worse, 0u);
    de_nav2d_mesh_destroy(m);
}

TEST(nav2d, test_nav2d_mesh_nearest_point) {
    de_nav2d_mesh *m = two_hole_mesh(NULL);
    ASSERT_TRUE(m != NULL);
    de_vec2 o;
    ASSERT_TRUE(de_nav2d_mesh_nearest_point(m, v2(1, 1), &o));
    EXPECT_TRUE(o.x == 1.0f && o.y == 1.0f);
    ASSERT_TRUE(de_nav2d_mesh_nearest_point(m, v2(25, 5), &o));
    EXPECT_FLOAT_EQ(o.x, 20.0, 1e-5);
    EXPECT_FLOAT_EQ(o.y, 5.0, 1e-5);
    ASSERT_TRUE(de_nav2d_mesh_nearest_point(m, v2(4.5f, 4.0f), &o)); /* in hole A, near x = 4 */
    EXPECT_FLOAT_EQ(o.x, 4.0, 1e-5);
    EXPECT_FLOAT_EQ(o.y, 4.0, 1e-5);
    EXPECT_FALSE(de_nav2d_mesh_nearest_point(NULL, v2(0, 0), &o));
    de_nav2d_mesh_destroy(m);
}

TEST(nav2d, test_nav2d_mesh_alloc) {
    test_counting_alloc ca;
    test_counting_init(&ca);
    de_nav2d_mesh *m = two_hole_mesh(&ca.iface);
    ASSERT_TRUE(m != NULL);
    EXPECT_GE(ca.live_blocks, 3u); /* mesh, vertices, triangles, edges, visibility graph */
    const u64 before = ca.allocs;
    de_vec2 *p = NULL;
    u32 n = 0;
    test_counting_alloc qa;
    test_counting_init(&qa);
    ASSERT_EQ(de_nav2d_mesh_find_path(m, v2(1, 1), v2(19, 9), &qa.iface, &p, &n), DE_OK);
    EXPECT_EQ(qa.allocs, 2u); /* scratch, then the points */
    EXPECT_EQ(qa.live_blocks, 1u);
    de_nav2d_path_free(&qa.iface, p, n);
    EXPECT_EQ(qa.live_blocks, 0u);
    EXPECT_EQ(ca.allocs, before);
    de_nav2d_mesh_destroy(m);
    EXPECT_EQ(ca.live_blocks, 0u);
    EXPECT_EQ(ca.size_errors, 0u);
    /* Out of memory at every step of the build: an error, nothing leaked. */
    for (i64 k = 0; k < 64; ++k) {
        test_counting_init(&ca);
        ca.fail_after = k;
        de_nav2d_mesh *f = two_hole_mesh(&ca.iface);
        if (f != NULL) {
            de_nav2d_mesh_destroy(f);
            EXPECT_EQ(ca.live_blocks, 0u);
            break;
        }
        EXPECT_EQ(ca.live_blocks, 0u);
    }
    /* Bad input. */
    de_nav2d_mesh_desc d;
    memset(&d, 0, sizeof d);
    de_nav2d_mesh *f = NULL;
    EXPECT_EQ(de_nav2d_mesh_build(&d, NULL, &f), DE_ERR_INVALID_ARGUMENT);
    const de_vec2 line[3] = {{0, 0}, {1, 1}, {2, 2}};
    de_nav2d_polygon flat = {line, 3};
    d.outlines = &flat;
    d.outline_count = 1;
    EXPECT_EQ(de_nav2d_mesh_build(&d, NULL, &f), DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(f == NULL);
}

/* ---------- order and thread independence ---------- */

#define OQ 64u

typedef struct order_ctx {
    const de_nav2d_grid *grid;
    const de_nav2d_mesh *mesh;
    de_vec2 from[OQ], to[OQ];
    u64 hash[OQ];
    bool reverse;
} order_ctx;

static u64 fnv(u64 h, const void *data, size_t size) {
    const u8 *p = (const u8 *)data;
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static void run_one(order_ctx *c, u32 q) {
    u64 h = 0xcbf29ce484222325ull;
    for (u32 kind = 0; kind < 3u; ++kind) {
        de_vec2 *p = NULL;
        u32 n = 0;
        de_result res;
        if (kind < 2u)
            res = de_nav2d_find_path(c->grid, c->from[q], c->to[q], kind == 1u, NULL, &p, &n);
        else
            res = de_nav2d_mesh_find_path(c->mesh, c->from[q], c->to[q], NULL, &p, &n);
        h = fnv(h, &res, sizeof res);
        if (n)
            h = fnv(h, p, sizeof(de_vec2) * n);
        de_nav2d_path_free(NULL, p, n);
    }
    c->hash[q] = h;
}

static i32 run_all(void *user) {
    order_ctx *c = (order_ctx *)user;
    for (u32 i = 0; i < OQ; ++i)
        run_one(c, c->reverse ? OQ - 1u - i : i);
    return 0;
}

TEST(nav2d, test_nav2d_order_and_thread) {
    static u8 cells[40 * 20];
    de_rng r = de_rng_seed(0x5eu, 0);
    random_grid(&r, cells, 40, 20, 0.2f);
    de_nav2d_grid g;
    memset(&g, 0, sizeof g);
    g.width = 40;
    g.height = 20;
    g.cell_size = 0.5f;
    g.walkable = cells;
    de_nav2d_mesh *m = two_hole_mesh(NULL);
    ASSERT_TRUE(m != NULL);
    static order_ctx c[3];
    for (u32 i = 0; i < OQ; ++i) {
        c[0].from[i] = random_walkable(&r);
        c[0].to[i] = random_walkable(&r);
    }
    c[0].grid = &g;
    c[0].mesh = m;
    c[0].reverse = false;
    c[1] = c[0];
    c[1].reverse = true;
    c[2] = c[0];
    run_all(&c[0]);
    run_all(&c[1]);
    de_thread *t = NULL;
    ASSERT_EQ(de_thread_create(run_all, &c[2], "nav2d-test", DE_THREAD_NORMAL, 0, &t), DE_OK);
    de_thread_join(t);
    EXPECT_EQ(memcmp(c[0].hash, c[1].hash, sizeof c[0].hash), 0);
    EXPECT_EQ(memcmp(c[0].hash, c[2].hash, sizeof c[0].hash), 0);
    de_nav2d_mesh_destroy(m);
}

TEST(nav2d, test_nav2d_plane_mapping) {
    /* Same axes as 2D physics (src/physics/2d/ecs2d/plane_map.c): SIDE_XZ is plane (x, y) =
     * world (x, z), TOP_XY is plane (x, y) = world (x, y). */
    const de_vec2 p = v2(3.25f, -7.5f);
    de_dvec3 w = de_nav2d_to_world(DE_PLANE_SIDE_XZ, p, 2.0);
    EXPECT_TRUE(w.x == 3.25 && w.y == 2.0 && w.z == -7.5);
    de_vec2 b = de_nav2d_from_world(DE_PLANE_SIDE_XZ, w);
    EXPECT_TRUE(b.x == p.x && b.y == p.y);
    w = de_nav2d_to_world(DE_PLANE_TOP_XY, p, -1.0);
    EXPECT_TRUE(w.x == 3.25 && w.y == -7.5 && w.z == -1.0);
    b = de_nav2d_from_world(DE_PLANE_TOP_XY, w);
    EXPECT_TRUE(b.x == p.x && b.y == p.y);
    de_dvec3 q;
    q.x = 1.0;
    q.y = 2.0;
    q.z = 3.0;
    b = de_nav2d_from_world(DE_PLANE_SIDE_XZ, q);
    EXPECT_TRUE(b.x == 1.0f && b.y == 3.0f);
    b = de_nav2d_from_world(DE_PLANE_TOP_XY, q);
    EXPECT_TRUE(b.x == 1.0f && b.y == 2.0f);
}
