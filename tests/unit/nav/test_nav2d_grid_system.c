/*
 * test_nav2d_grid_system.c: the DeNavGrid2D system `nav2d.grid` (DENG-832;
 * src/gameplay/nav/nav2d_grid_system.c). CTest names nav2d_grid.<test>.
 *
 *   test_nav2d_grid_build_and_path   an added grid is built; blocked cells (255) stop a path
 *   test_nav2d_grid_unchanged_no_alloc  runs with no change rebuild nothing and allocate nothing
 *   test_nav2d_grid_change_rebuilds  a write to the component builds the grid again
 *   test_nav2d_grid_origin           the origin follows DeTransform on the system's plane
 *   test_nav2d_grid_remove_despawn   removing the component or despawning frees the grid
 *   test_nav2d_grid_invalid          bad width, height or cell_size build no grid
 *   test_nav2d_grid_many_and_free    many entities, any add order; detach and world destroy
 *                                    free everything; attach twice is a no-op
 */
#include "de_test.h"

#include "core/test_counting_alloc.h"

#include <dengine/de_ecs.h>
#include <dengine/de_gameplay.h>
#include <dengine/de_platform.h>
#include <dengine/de_reflect.h>

#include <string.h>

typedef struct fixture {
    test_counting_alloc ca;
    de_type_registry *reg;
    de_world *w;
    de_component grid;
} fixture;

static void log_quiet(void) {
    de_log_desc ld;
    memset(&ld, 0, sizeof ld);
    ld.no_console_sink = true;
    ASSERT_EQ(de_log_init(&ld), DE_OK);
    de_fp_env_init_thread();
}

static void setup(fixture *f, de_plane_2d plane) {
    log_quiet();
    memset(f, 0, sizeof *f);
    test_counting_init(&f->ca);
    ASSERT_EQ(de_type_registry_create(NULL, &f->reg), DE_OK);
    ASSERT_EQ(de_world_create(&(de_world_desc){.types = f->reg}, &f->w), DE_OK);
    de_nav2d_grid_system_desc d = {0};
    d.plane = plane;
    d.alloc = &f->ca.iface;
    ASSERT_EQ(de_nav2d_grid_attach(f->w, &d), DE_OK);
    f->grid = de_component_find(f->w, "DeNavGrid2D");
    ASSERT_TRUE(DE_HANDLE_VALID(f->grid));
}

static void teardown(fixture *f) {
    de_world_destroy(f->w);
    de_type_registry_destroy(f->reg);
    EXPECT_EQ(f->ca.live_blocks, 0u);
    EXPECT_EQ(f->ca.size_errors, 0u);
    de_log_shutdown();
}

static void run(fixture *f) {
    ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_PRE_UPDATE, 1.0f / 60.0f, 0.0f), DE_OK);
}

/* An entity with a w x h grid of 1 m cells, all walkable (area 0). */
static de_entity spawn_grid(fixture *f, u32 w, u32 h) {
    de_entity e = de_entity_spawn(f->w);
    DeNavGrid2D *g = (DeNavGrid2D *)de_entity_add(f->w, e, f->grid);
    if (g == NULL)
        return e;
    g->width = w;
    g->height = h;
    g->cell_size = 1.0f;
    memset(g->cells, 0, sizeof g->cells);
    return e;
}

static de_vec2 v2(f32 x, f32 y) {
    de_vec2 v;
    v.x = x;
    v.y = y;
    return v;
}

TEST(nav2d_grid, test_nav2d_grid_build_and_path) {
    fixture f;
    setup(&f, DE_PLANE_SIDE_XZ);
    de_entity e = spawn_grid(&f, 8, 4);
    DeNavGrid2D *g = (DeNavGrid2D *)de_entity_get(f.w, e, f.grid);
    ASSERT_TRUE(g != NULL);
    /* A wall at x = 4 with a gap at y = 3. */
    for (u32 y = 0; y < 3; ++y)
        g->cells[y * 8u + 4u] = DE_NAV_GRID2D_BLOCKED;
    g->cells[3u * 8u + 1u] = 7; /* another area: still walkable */
    run(&f);

    de_nav2d_grid ng;
    u32 rev = 0;
    ASSERT_TRUE(de_nav2d_grid_get(f.w, e, &ng, &rev));
    EXPECT_EQ(ng.width, 8u);
    EXPECT_EQ(ng.height, 4u);
    EXPECT_TRUE(ng.cell_size == 1.0f);
    EXPECT_GT(rev, 0u);
    EXPECT_EQ(ng.walkable[0 * 8 + 4], 0);
    EXPECT_EQ(ng.walkable[3 * 8 + 4], 1);
    EXPECT_EQ(ng.walkable[3 * 8 + 1], 1);

    de_vec2 *pts = NULL;
    u32 n = 0;
    ASSERT_EQ(de_nav2d_find_path(&ng, v2(0.5f, 0.5f), v2(7.5f, 0.5f), true, NULL, &pts, &n),
              DE_OK);
    ASSERT_GT(n, 1u);
    bool through_gap = false;
    for (u32 i = 0; i < n; ++i)
        if (pts[i].y > 3.0f)
            through_gap = true;
    EXPECT_TRUE(through_gap);
    de_nav2d_path_free(NULL, pts, n);
    teardown(&f);
}

TEST(nav2d_grid, test_nav2d_grid_unchanged_no_alloc) {
    fixture f;
    setup(&f, DE_PLANE_SIDE_XZ);
    de_entity e = spawn_grid(&f, 16, 16);
    run(&f);
    run(&f); /* warm the schedule's graph and command buffers */
    u32 rev0 = 0;
    de_nav2d_grid ng;
    ASSERT_TRUE(de_nav2d_grid_get(f.w, e, &ng, &rev0));
    const u64 calls = test_counting_calls(&f.ca);
    for (int i = 0; i < 10; ++i)
        run(&f);
    u32 rev1 = 0;
    ASSERT_TRUE(de_nav2d_grid_get(f.w, e, &ng, &rev1));
    EXPECT_EQ(rev1, rev0);
    EXPECT_EQ(test_counting_calls(&f.ca), calls);
    teardown(&f);
}

TEST(nav2d_grid, test_nav2d_grid_change_rebuilds) {
    fixture f;
    setup(&f, DE_PLANE_SIDE_XZ);
    de_entity e = spawn_grid(&f, 4, 4);
    run(&f);
    u32 rev0 = 0;
    de_nav2d_grid ng;
    ASSERT_TRUE(de_nav2d_grid_get(f.w, e, &ng, &rev0));
    EXPECT_EQ(ng.walkable[5], 1);
    const u64 calls = test_counting_calls(&f.ca);

    DeNavGrid2D *g = (DeNavGrid2D *)de_entity_get(f.w, e, f.grid); /* marks changed */
    g->cells[5] = DE_NAV_GRID2D_BLOCKED;
    g->width = 5;
    run(&f);
    u32 rev1 = 0;
    ASSERT_TRUE(de_nav2d_grid_get(f.w, e, &ng, &rev1));
    EXPECT_EQ(rev1, rev0 + 1u);
    EXPECT_EQ(ng.width, 5u);
    EXPECT_EQ(ng.walkable[5], 0);
    EXPECT_EQ(test_counting_calls(&f.ca), calls); /* a rebuild allocates nothing */

    run(&f);
    u32 rev2 = 0;
    ASSERT_TRUE(de_nav2d_grid_get(f.w, e, &ng, &rev2));
    EXPECT_EQ(rev2, rev1);

    /* flush builds every grid again (outside a system everything counts as changed). */
    de_nav2d_grid_flush(f.w);
    ASSERT_TRUE(de_nav2d_grid_get(f.w, e, &ng, &rev2));
    EXPECT_EQ(rev2, rev1 + 1u);
    teardown(&f);
}

TEST(nav2d_grid, test_nav2d_grid_origin) {
    fixture f;
    setup(&f, DE_PLANE_SIDE_XZ);
    const de_builtin_component_ids b = de_builtin_components(f.w);
    de_entity e = spawn_grid(&f, 4, 4);
    de_entity bare = spawn_grid(&f, 4, 4);
    de_transform *t = (de_transform *)de_entity_add(f.w, e, b.transform);
    ASSERT_TRUE(t != NULL);
    *t = de_transform_identity();
    t->position.x = 3.0;
    t->position.y = 9.0;
    t->position.z = 5.0;
    run(&f);
    de_nav2d_grid ng;
    ASSERT_TRUE(de_nav2d_grid_get(f.w, e, &ng, NULL));
    EXPECT_TRUE(ng.origin.x == 3.0f && ng.origin.y == 5.0f);
    ASSERT_TRUE(de_nav2d_grid_get(f.w, bare, &ng, NULL));
    EXPECT_TRUE(ng.origin.x == 0.0f && ng.origin.y == 0.0f);

    /* Moving the entity moves the origin without a rebuild. */
    u32 rev0 = 0, rev1 = 0;
    ASSERT_TRUE(de_nav2d_grid_get(f.w, e, &ng, &rev0));
    t = (de_transform *)de_entity_get(f.w, e, b.transform);
    t->position.z = -2.0;
    run(&f);
    ASSERT_TRUE(de_nav2d_grid_get(f.w, e, &ng, &rev1));
    EXPECT_TRUE(ng.origin.x == 3.0f && ng.origin.y == -2.0f);
    EXPECT_EQ(rev1, rev0);
    teardown(&f);

    setup(&f, DE_PLANE_TOP_XY);
    const de_builtin_component_ids b2 = de_builtin_components(f.w);
    e = spawn_grid(&f, 4, 4);
    t = (de_transform *)de_entity_add(f.w, e, b2.transform);
    *t = de_transform_identity();
    t->position.x = 3.0;
    t->position.y = 9.0;
    t->position.z = 5.0;
    run(&f);
    ASSERT_TRUE(de_nav2d_grid_get(f.w, e, &ng, NULL));
    EXPECT_TRUE(ng.origin.x == 3.0f && ng.origin.y == 9.0f);
    teardown(&f);
}

TEST(nav2d_grid, test_nav2d_grid_remove_despawn) {
    fixture f;
    setup(&f, DE_PLANE_SIDE_XZ);
    de_entity a = spawn_grid(&f, 4, 4);
    de_entity b = spawn_grid(&f, 4, 4);
    de_entity c = spawn_grid(&f, 4, 4);
    run(&f);
    de_nav2d_grid ng;
    EXPECT_TRUE(de_nav2d_grid_get(f.w, a, &ng, NULL));
    EXPECT_TRUE(de_nav2d_grid_get(f.w, b, &ng, NULL));
    de_entity_remove(f.w, a, f.grid);
    de_entity_despawn(f.w, b);
    run(&f);
    EXPECT_FALSE(de_nav2d_grid_get(f.w, a, &ng, NULL));
    EXPECT_TRUE(ng.walkable == NULL);
    EXPECT_FALSE(de_nav2d_grid_get(f.w, b, &ng, NULL));
    EXPECT_TRUE(de_nav2d_grid_get(f.w, c, &ng, NULL));

    /* Added again: a new grid. */
    DeNavGrid2D *g = (DeNavGrid2D *)de_entity_add(f.w, a, f.grid);
    ASSERT_TRUE(g != NULL);
    run(&f);
    EXPECT_TRUE(de_nav2d_grid_get(f.w, a, &ng, NULL));
    EXPECT_EQ(ng.width, 16u); /* the construct hook's default */
    teardown(&f);
}

TEST(nav2d_grid, test_nav2d_grid_invalid) {
    fixture f;
    setup(&f, DE_PLANE_SIDE_XZ);
    de_entity e0 = spawn_grid(&f, 0, 4);
    de_entity e1 = spawn_grid(&f, 4, 65);
    de_entity e2 = spawn_grid(&f, 4, 4);
    ((DeNavGrid2D *)de_entity_get(f.w, e2, f.grid))->cell_size = 0.0f;
    run(&f);
    de_nav2d_grid ng;
    EXPECT_FALSE(de_nav2d_grid_get(f.w, e0, &ng, NULL));
    EXPECT_FALSE(de_nav2d_grid_get(f.w, e1, &ng, NULL));
    EXPECT_FALSE(de_nav2d_grid_get(f.w, e2, &ng, NULL));
    /* Fixed: built. */
    ((DeNavGrid2D *)de_entity_get(f.w, e2, f.grid))->cell_size = 0.5f;
    run(&f);
    EXPECT_TRUE(de_nav2d_grid_get(f.w, e2, &ng, NULL));
    EXPECT_TRUE(ng.cell_size == 0.5f);
    /* NULL arguments. */
    EXPECT_FALSE(de_nav2d_grid_get(NULL, e2, &ng, NULL));
    EXPECT_FALSE(de_nav2d_grid_get(f.w, e2, NULL, NULL));
    EXPECT_NE(de_nav2d_grid_attach(NULL, NULL), DE_OK);
    teardown(&f);
}

TEST(nav2d_grid, test_nav2d_grid_many_and_free) {
    fixture f;
    setup(&f, DE_PLANE_SIDE_XZ);
    /* Attaching again does nothing. */
    ASSERT_EQ(de_nav2d_grid_attach(f.w, NULL), DE_OK);
    enum { N = 40 };
    de_entity es[N];
    for (u32 i = 0; i < N; ++i)
        es[i] = spawn_grid(&f, 1u + i % 64u, 2u);
    /* Despawn some and spawn more, so indices are reused out of order. */
    for (u32 i = 0; i < N; i += 3)
        de_entity_despawn(f.w, es[i]);
    for (u32 i = 0; i < N; i += 3)
        es[i] = spawn_grid(&f, 1u + i % 64u, 2u);
    run(&f);
    for (u32 i = 0; i < N; ++i) {
        de_nav2d_grid ng;
        ASSERT_TRUE(de_nav2d_grid_get(f.w, es[i], &ng, NULL));
        EXPECT_EQ(ng.width, 1u + i % 64u);
    }
    de_nav2d_grid_detach(f.w);
    EXPECT_EQ(f.ca.live_blocks, 0u);
    de_nav2d_grid ng;
    EXPECT_FALSE(de_nav2d_grid_get(f.w, es[1], &ng, NULL));
    run(&f); /* no system left */
    /* Attach again; the world frees the state on destroy. */
    de_nav2d_grid_system_desc d = {0};
    d.alloc = &f.ca.iface;
    ASSERT_EQ(de_nav2d_grid_attach(f.w, &d), DE_OK);
    run(&f);
    EXPECT_TRUE(de_nav2d_grid_get(f.w, es[1], &ng, NULL));
    EXPECT_GT(f.ca.live_blocks, 0u);
    teardown(&f);
}
