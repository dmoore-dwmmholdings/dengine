/*
 * test_nav_query.c: navmesh queries, off-mesh links and dynamic obstacles (DENG-77;
 * src/gameplay/nav/nav_query.c, nav_obstacles.c). CTest names nav_query.<test>.
 *
 *   nav_path_around_wall      a path around a 60 m wall: few corners, none through the wall
 *   nav_path_partial          an unreachable island gives a partial path to the nearest edge
 *   nav_path_invalid_start    a start far from the navmesh is DE_ERR_NOT_FOUND with a message
 *   nav_path_no_alloc         a query allocates only the path points
 *   nav_offmesh_link          a link across a gap makes the path complete; area_mask filters it
 *   nav_obstacle_add_move_remove  an obstacle carves on add, follows a move, and goes on remove;
 *                             a move under two cells rebuilds nothing
 *   nav_raycast_hits_edge     a surface ray stops at the wall (agent radius off), or reaches
 *   nav_nearest_point         nearest navmesh point within a radius, none out of reach
 *   nav_query_on_workers      frame-worker jobs query in parallel, each with its own object,
 *                             in deterministic mode
 */
#include "de_test.h"

#include "core/test_counting_alloc.h"

#include "gameplay/nav/nav_internal.h"

#include <dengine/de_ecs.h>
#include <dengine/de_gameplay.h>
#include <dengine/de_platform.h>
#include <dengine/de_reflect.h>
#include <dengine/de_task.h>

#include <string.h>

#define DEG45_RAD 0.785398163f

static de_navmesh_build_desc build_desc(void) {
    de_navmesh_build_desc d;
    memset(&d, 0, sizeof d);
    d.cell_size = 0.25f;
    d.cell_height = 0.1f;
    d.agent_radius = 0.5f;
    d.agent_height = 2.0f;
    d.agent_max_climb = 0.4f;
    d.agent_max_slope_rad = DEG45_RAD;
    d.tile_size = 32.0f;
    return d;
}

static de_dvec3 P(f64 x, f64 y, f64 z) {
    de_dvec3 p;
    p.x = x;
    p.y = y;
    p.z = z;
    return p;
}

static de_aabb box(f32 x0, f32 y0, f32 z0, f32 x1, f32 y1, f32 z1) {
    de_aabb b;
    b.min.x = x0;
    b.min.y = y0;
    b.min.z = z0;
    b.max.x = x1;
    b.max.y = y1;
    b.max.z = z1;
    return b;
}

static void add_quad(de_nav *n, de_dvec3 a, de_dvec3 b, de_dvec3 c, de_dvec3 d) {
    const de_dvec3 t[6] = {a, b, c, a, c, d};
    ASSERT_EQ(de__nav_add_test_tris(n, t, 2), DE_OK);
}

/* A flat rectangle at z = 0, facing +Z. */
static void add_floor(de_nav *n, f64 x0, f64 y0, f64 x1, f64 y1) {
    add_quad(n, P(x0, y0, 0), P(x1, y0, 0), P(x1, y1, 0), P(x0, y1, 0));
}

/* A solid block [x0, x1] x [y0, y1] x [0, h]: its 5 visible faces, outward. */
static void add_block(de_nav *n, f64 x0, f64 y0, f64 x1, f64 y1, f64 h) {
    add_quad(n, P(x0, y0, h), P(x1, y0, h), P(x1, y1, h), P(x0, y1, h));
    add_quad(n, P(x0, y0, 0), P(x1, y0, 0), P(x1, y0, h), P(x0, y0, h));
    add_quad(n, P(x1, y1, 0), P(x0, y1, 0), P(x0, y1, h), P(x1, y1, h));
    add_quad(n, P(x0, y1, 0), P(x0, y0, 0), P(x0, y0, h), P(x0, y1, h));
    add_quad(n, P(x1, y0, 0), P(x1, y1, 0), P(x1, y1, h), P(x1, y0, h));
}

static void log_quiet(void) {
    de_log_desc ld;
    memset(&ld, 0, sizeof ld);
    ld.no_console_sink = true;
    ASSERT_EQ(de_log_init(&ld), DE_OK);
    de_fp_env_init_thread();
}

static void build_all(de_nav *n) {
    const de_navmesh_build_desc d = build_desc();
    ASSERT_EQ(de_nav_rebuild_tiles(n, &d, box(0, 0, -1, 100, 100, 4)), DE_OK);
    de_nav_update(n);
    ASSERT_EQ(de__nav_pending_jobs(n), 0u);
}

static f64 absd(f64 v) { return v < 0 ? -v : v; }

static f64 dist2d(de_dvec3 a, de_dvec3 b) {
    const f64 dx = a.x - b.x, dy = a.y - b.y;
    return de_dsqrt64(dx * dx + dy * dy);
}

/* true when segment a-b passes through the rectangle [x0, x1] x [y0, y1] (sampled each 5 cm). */
static bool crosses(de_dvec3 a, de_dvec3 b, f64 x0, f64 y0, f64 x1, f64 y1) {
    const f64 len = dist2d(a, b);
    const u32 steps = (u32)(len / 0.05) + 1u;
    for (u32 i = 0; i <= steps; ++i) {
        const f64 t = (f64)i / (f64)steps;
        const f64 x = a.x + (b.x - a.x) * t, y = a.y + (b.y - a.y) * t;
        if (x > x0 && x < x1 && y > y0 && y < y1)
            return true;
    }
    return false;
}

static f64 path_length(const de_nav_path *p) {
    f64 len = 0.0;
    for (u32 i = 1; i < p->count; ++i)
        len += dist2d(p->points[i - 1], p->points[i]);
    return len;
}

TEST(nav_query, nav_path_around_wall) {
    log_quiet();
    de_nav *n = NULL;
    ASSERT_EQ(de_nav_create(NULL, NULL, &n), DE_OK);
    add_floor(n, 0, 0, 100, 100);
    add_block(n, 49.5, 20, 50.5, 80, 3); /* a wall across the direct line */
    build_all(n);

    de_nav_path p;
    ASSERT_EQ(de_nav_find_path(n, P(40, 50, 0), P(60, 50, 0), 0, NULL, &p), DE_OK);
    EXPECT_FALSE(p.partial);
    ASSERT_GE(p.count, 3u);
    EXPECT_LE(p.count, 5u); /* string pulling: start, the wall's corners, end */
    EXPECT_TRUE(dist2d(p.points[0], P(40, 50, 0)) < 0.01);
    EXPECT_TRUE(dist2d(p.points[p.count - 1], P(60, 50, 0)) < 0.01);
    for (u32 i = 0; i + 1 < p.count; ++i)
        EXPECT_FALSE(crosses(p.points[i], p.points[i + 1], 49.5, 20, 50.5, 80));
    /* Around one end: about 2 x 32 m, well under going around twice. */
    EXPECT_TRUE(path_length(&p) < 70.0);
    EXPECT_TRUE(path_length(&p) > 60.0);
    for (u32 i = 0; i < p.count; ++i)
        EXPECT_TRUE(absd(p.points[i].z) < 0.3);
    de_nav_path_free(NULL, &p);
    EXPECT_TRUE(p.points == NULL);
    EXPECT_EQ(p.count, 0u);
    de_nav_destroy(n);
    de_log_shutdown();
}

TEST(nav_query, nav_path_partial) {
    log_quiet();
    de_nav *n = NULL;
    ASSERT_EQ(de_nav_create(NULL, NULL, &n), DE_OK);
    add_floor(n, 0, 0, 40, 100);
    add_floor(n, 60, 0, 100, 100); /* an island: no way across */
    build_all(n);

    de_nav_path p;
    ASSERT_EQ(de_nav_find_path(n, P(10, 50, 0), P(80, 50, 0), 0, NULL, &p), DE_OK);
    EXPECT_TRUE(p.partial);
    ASSERT_GE(p.count, 2u);
    const de_dvec3 last = p.points[p.count - 1];
    EXPECT_TRUE(last.x > 38.5); /* the near edge, eroded by the agent radius */
    EXPECT_TRUE(last.x < 40.0);
    EXPECT_TRUE(absd(last.y - 50) < 1.0);
    de_nav_path_free(NULL, &p);
    de_nav_destroy(n);
    de_log_shutdown();
}

TEST(nav_query, nav_path_invalid_start) {
    log_quiet();
    de_nav *n = NULL;
    ASSERT_EQ(de_nav_create(NULL, NULL, &n), DE_OK);
    de_nav_path p;
    /* No navmesh yet. */
    EXPECT_EQ(de_nav_find_path(n, P(10, 10, 0), P(20, 20, 0), 0, NULL, &p), DE_ERR_NOT_FOUND);
    add_floor(n, 0, 0, 100, 100);
    build_all(n);
    de_error_clear();
    EXPECT_EQ(de_nav_find_path(n, P(500, 500, 0), P(50, 50, 0), 0, NULL, &p), DE_ERR_NOT_FOUND);
    EXPECT_EQ(p.count, 0u);
    EXPECT_TRUE(p.points == NULL);
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_TRUE(strstr(de_error_last()->message, "start") != NULL);
    EXPECT_EQ(de_nav_find_path(n, P(50, 50, 0), P(50, 50, 30), 0, NULL, &p), DE_ERR_NOT_FOUND);
    EXPECT_TRUE(strstr(de_error_last()->message, "end") != NULL);
    EXPECT_EQ(de_nav_find_path(NULL, P(0, 0, 0), P(0, 0, 0), 0, NULL, &p),
              DE_ERR_INVALID_ARGUMENT);
    de_nav_destroy(n);
    de_log_shutdown();
}

TEST(nav_query, nav_path_no_alloc) {
    log_quiet();
    test_counting_alloc na, pa;
    test_counting_init(&na);
    test_counting_init(&pa);
    de_nav *n = NULL;
    ASSERT_EQ(de_nav_create(&na.iface, NULL, &n), DE_OK);
    add_floor(n, 0, 0, 100, 100);
    add_block(n, 49.5, 20, 50.5, 80, 3);
    build_all(n);
    const u64 nav_allocs = na.allocs + na.reallocs;
    for (u32 i = 0; i < 10; ++i) {
        de_nav_path p;
        ASSERT_EQ(de_nav_find_path(n, P(40, 50, 0), P(60, 50 + i, 0), 0, &pa.iface, &p), DE_OK);
        de_dvec3 h;
        (void)de_nav_raycast(n, P(40, 50, 0), P(60, 50, 0), &h);
        (void)de_nav_nearest_point(n, P(50, 10, 1), 2.0f, &h);
        de_nav_path_free(&pa.iface, &p);
    }
    EXPECT_EQ(na.allocs + na.reallocs, nav_allocs);
    EXPECT_EQ(pa.allocs, (u64)10); /* one block of points per path */
    EXPECT_EQ(pa.live_blocks, (u64)0);
    EXPECT_EQ(pa.size_errors, (u64)0);
    de_nav_destroy(n);
    EXPECT_EQ(na.live_blocks, (u64)0);
    EXPECT_EQ(na.size_errors, (u64)0);
    de_log_shutdown();
}

TEST(nav_query, nav_offmesh_link) {
    log_quiet();
    de_nav *n = NULL;
    ASSERT_EQ(de_nav_create(NULL, NULL, &n), DE_OK);
    /* Two platforms with an 8 m gap that crosses the tile edge at x = 32. */
    add_floor(n, 0, 0, 28, 20);
    add_floor(n, 36, 0, 64, 20);
    build_all(n);
    de_nav_path p;
    ASSERT_EQ(de_nav_find_path(n, P(5, 10, 0), P(60, 10, 0), 0, NULL, &p), DE_OK);
    EXPECT_TRUE(p.partial);
    de_nav_path_free(NULL, &p);

    /* A one-way jump link, area 1; links take effect with the next rebuild. */
    EXPECT_EQ(de_nav_add_offmesh_link(n, P(27, 10, 0), P(37, 10, 0), 0.5f, false, 1),
              DE_OK);
    EXPECT_EQ(de_nav_add_offmesh_link(n, P(27, 10, 0), P(37, 10, 0), 0.5f, false, 16),
              DE_ERR_INVALID_ARGUMENT);
    build_all(n);
    ASSERT_EQ(de_nav_find_path(n, P(5, 10, 0), P(60, 10, 0), 0, NULL, &p), DE_OK);
    EXPECT_FALSE(p.partial);
    bool at_start = false, at_end = false;
    for (u32 i = 0; i < p.count; ++i) {
        at_start = at_start || dist2d(p.points[i], P(27, 10, 0)) < 0.6;
        at_end = at_end || dist2d(p.points[i], P(37, 10, 0)) < 0.6;
    }
    EXPECT_TRUE(at_start);
    EXPECT_TRUE(at_end);
    EXPECT_TRUE(dist2d(p.points[p.count - 1], P(60, 10, 0)) < 0.01);
    de_nav_path_free(NULL, &p);

    /* area_mask: ground only (bit 0) leaves the link out. */
    ASSERT_EQ(de_nav_find_path(n, P(5, 10, 0), P(60, 10, 0), 1u, NULL, &p), DE_OK);
    EXPECT_TRUE(p.partial);
    de_nav_path_free(NULL, &p);
    ASSERT_EQ(de_nav_find_path(n, P(5, 10, 0), P(60, 10, 0), 3u, NULL, &p), DE_OK);
    EXPECT_FALSE(p.partial);
    de_nav_path_free(NULL, &p);
    /* One way: no path back. */
    ASSERT_EQ(de_nav_find_path(n, P(60, 10, 0), P(5, 10, 0), 0, NULL, &p), DE_OK);
    EXPECT_TRUE(p.partial);
    de_nav_path_free(NULL, &p);
    de_nav_destroy(n);
    de_log_shutdown();
}

TEST(nav_query, nav_obstacle_add_move_remove) {
    log_quiet();
    de_type_registry *reg = NULL;
    ASSERT_EQ(de_type_registry_create(NULL, &reg), DE_OK);
    de_world *w = NULL;
    ASSERT_EQ(de_world_create(&(de_world_desc){.types = reg}, &w), DE_OK);
    const de_builtin_component_ids b = de_builtin_components(w);
    de_nav *n = NULL;
    ASSERT_EQ(de_nav_create(NULL, NULL, &n), DE_OK);
    de_nav_attach(n, w);
    add_floor(n, 0, 0, 100, 100);
    build_all(n);

    de_nav_path p;
    ASSERT_EQ(de_nav_find_path(n, P(10, 50, 0), P(90, 50, 0), 0, NULL, &p), DE_OK);
    EXPECT_EQ(p.count, 2u);
    de_nav_path_free(NULL, &p);

    /* A 2 x 20 m crate across the line. */
    const de_entity e = de_entity_spawn(w);
    de_transform *t = (de_transform *)de_entity_add(w, e, b.transform);
    ASSERT_TRUE(t != NULL);
    *t = de_transform_identity();
    t->position = P(50, 50, 1);
    de_vec3 he;
    he.x = 1.0f;
    he.y = 10.0f;
    he.z = 1.0f;
    de_nav_add_obstacle(n, e, he);
    de_nav_update(n); /* no task system: queued, built and applied in this update */
    EXPECT_EQ(de__nav_pending_jobs(n), 0u);
    EXPECT_FALSE(de__nav_point_on_mesh(n, P(50, 50, 0), 0.3f));
    ASSERT_EQ(de_nav_find_path(n, P(10, 50, 0), P(90, 50, 0), 0, NULL, &p), DE_OK);
    EXPECT_FALSE(p.partial);
    EXPECT_GE(p.count, 3u);
    for (u32 i = 0; i + 1 < p.count; ++i)
        EXPECT_FALSE(crosses(p.points[i], p.points[i + 1], 49, 40, 51, 60));
    de_nav_path_free(NULL, &p);

    /* A small move (under two cells) rebuilds nothing. */
    i32 tx = 0, tz = 0;
    ASSERT_TRUE(de__nav_tile_of(n, P(50, 50, 0), &tx, &tz));
    const u64 ref = de__nav_tile_ref(n, tx, tz);
    t = (de_transform *)de_entity_get(w, e, b.transform);
    ASSERT_TRUE(t != NULL);
    t->position = P(50.2, 50, 1);
    de_nav_update(n);
    EXPECT_EQ(de__nav_tile_ref(n, tx, tz), ref);

    /* Moved off the line: the old footprint opens, the new one is carved. */
    t = (de_transform *)de_entity_get(w, e, b.transform);
    t->position = P(50, 20, 1);
    de_nav_update(n);
    EXPECT_TRUE(de__nav_point_on_mesh(n, P(50, 50, 0), 0.3f));
    EXPECT_FALSE(de__nav_point_on_mesh(n, P(50, 20, 0), 0.3f));
    ASSERT_EQ(de_nav_find_path(n, P(10, 50, 0), P(90, 50, 0), 0, NULL, &p), DE_OK);
    EXPECT_EQ(p.count, 2u);
    de_nav_path_free(NULL, &p);

    /* Removed: its tiles are rebuilt and it leaves the list. */
    de_nav_remove_obstacle(n, e);
    de_nav_update(n);
    EXPECT_EQ(de__nav_obstacle_count(n), 0u);
    EXPECT_TRUE(de__nav_point_on_mesh(n, P(50, 20, 0), 0.3f));

    de_nav_attach(n, NULL);
    de_nav_destroy(n);
    de_world_destroy(w);
    de_type_registry_destroy(reg);
    de_log_shutdown();
}

TEST(nav_query, nav_raycast_hits_edge) {
    log_quiet();
    de_nav *n = NULL;
    de_dvec3 h = P(0, 0, 0);
    ASSERT_EQ(de_nav_create(NULL, NULL, &n), DE_OK);
    EXPECT_TRUE(de_nav_raycast(n, P(1, 2, 3), P(4, 5, 6), &h)); /* no navmesh */
    EXPECT_TRUE(h.x == 1.0 && h.y == 2.0 && h.z == 3.0);
    add_floor(n, 0, 0, 100, 100);
    add_block(n, 49.5, 0, 50.5, 100, 3); /* a full-width wall */
    build_all(n);
    ASSERT_TRUE(de_nav_raycast(n, P(40, 50, 0), P(60, 50, 0), &h));
    EXPECT_TRUE(h.x > 48.6); /* the wall minus the agent radius */
    EXPECT_TRUE(h.x < 49.3);
    EXPECT_TRUE(absd(h.y - 50) < 0.01);
    EXPECT_TRUE(absd(h.z) < 0.3);
    EXPECT_FALSE(de_nav_raycast(n, P(40, 50, 0), P(45, 60, 0), &h));
    /* Off the navmesh: a hit at the start. */
    EXPECT_TRUE(de_nav_raycast(n, P(300, 50, 0), P(310, 50, 0), &h));
    EXPECT_TRUE(h.x == 300.0);
    de_nav_destroy(n);
    de_log_shutdown();
}

TEST(nav_query, nav_nearest_point) {
    log_quiet();
    de_nav *n = NULL;
    ASSERT_EQ(de_nav_create(NULL, NULL, &n), DE_OK);
    add_floor(n, 0, 0, 100, 100);
    add_block(n, 49.5, 0, 50.5, 100, 3);
    build_all(n);
    de_dvec3 o;
    ASSERT_TRUE(de_nav_nearest_point(n, P(20, 30, 1.5), 3.0f, &o));
    EXPECT_TRUE(dist2d(o, P(20, 30, 0)) < 0.01);
    EXPECT_TRUE(absd(o.z) < 0.3);
    /* Inside the wall: the nearest walkable edge, the agent radius off the wall. */
    ASSERT_TRUE(de_nav_nearest_point(n, P(49.8, 30, 0), 3.0f, &o));
    EXPECT_TRUE((o.x > 48.6 && o.x < 49.3) || (o.x > 50.7 && o.x < 51.4));
    EXPECT_FALSE(de_nav_nearest_point(n, P(200, 30, 0), 3.0f, &o));
    EXPECT_FALSE(de_nav_nearest_point(n, P(20, 30, 0), 0.0f, &o));
    de_nav_destroy(n);
    de_log_shutdown();
}

typedef struct worker_ctx {
    de_nav *n;
    u32 ok[64];
    u32 worker[64];
} worker_ctx;

static void worker_paths(u32 begin, u32 end, void *user) {
    worker_ctx *c = (worker_ctx *)user;
    for (u32 i = begin; i < end; ++i) {
        de_nav_path p;
        c->worker[i] = de_task_worker_index();
        if (de_nav_find_path(c->n, P(40, 50, 0), P(60, 30 + (f64)(i % 40), 0), 0, NULL, &p) ==
                DE_OK &&
            !p.partial && p.count >= 3u)
            c->ok[i] = 1;
        de_nav_path_free(NULL, &p);
    }
}

TEST(nav_query, nav_query_on_workers) {
    log_quiet();
    de_task_system *ts = NULL;
    ASSERT_EQ(de_task_system_create(&(de_task_system_desc){.worker_count = 2}, &ts), DE_OK);
    de_nav *n = NULL;
    ASSERT_EQ(de_nav_create(NULL, ts, &n), DE_OK);
    de_nav_set_deterministic(n, true);
    add_floor(n, 0, 0, 100, 100);
    add_block(n, 49.5, 20, 50.5, 80, 3);
    const de_navmesh_build_desc d = build_desc();
    ASSERT_EQ(de_nav_rebuild_tiles(n, &d, box(0, 0, -1, 100, 100, 4)), DE_OK);
    de_nav_update(n); /* deterministic: waits for the background build */
    EXPECT_EQ(de__nav_pending_jobs(n), 0u);
    EXPECT_EQ(n->query_count, de_task_worker_count(ts));
    worker_ctx c;
    memset(&c, 0, sizeof c);
    c.n = n;
    de_task_parallel_for(ts, 64, 1, worker_paths, &c);
    for (u32 i = 0; i < 64; ++i) {
        EXPECT_EQ(c.ok[i], 1u);
        EXPECT_LT(c.worker[i], de_task_worker_count(ts));
    }
    de_nav_destroy(n);
    de_task_system_destroy(ts);
    de_log_shutdown();
}
