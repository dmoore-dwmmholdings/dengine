/*
 * test_nav.c: navmesh generation (DENG-76; src/gameplay/nav/). CTest names nav.<test>.
 *
 *   nav_build_flat_plane        a 100 x 100 m plane gives tiles, after the next de_nav_update
 *   nav_build_box_carves_hole   a 2 m box (geometry, and an entity obstacle) removes its footprint
 *                               inflated by agent_radius; the ground around it stays walkable
 *   nav_rebuild_region_only     a sub-region rebuild runs in the background, replaces only the
 *                               tiles it meets, and keeps every other tile reference
 *   nav_coord_swizzle_roundtrip engine <-> Recast mapping is exact both ways; a ramp along +Y
 *                               under the slope limit is walkable, a steeper one is not
 *   nav_input_from_colliders    static DeColliders in the collision mask are input; dynamic
 *                               bodies, triggers and other layers are not (needs physics)
 *   nav_asset_cook_roundtrip    a `.navmesh` source cooks to the same bytes twice, loads back
 *                               byte-identical as a `navmesh` asset and adds its tiles
 *   nav_allocator_hooks         every Recast / Detour allocation goes through the nav allocator
 */
#include "de_test.h"

#include "core/test_counting_alloc.h"

#include "gameplay/nav/nav_internal.h"

#include <dengine/de_asset.h>
#include <dengine/de_ecs.h>
#include <dengine/de_gameplay.h>
#include <dengine/de_platform.h>
#include <dengine/de_reflect.h>
#include <dengine/de_task.h>

#include <stdio.h>
#include <string.h>

#ifndef DE_HAS_PHYSICS
#define DE_HAS_PHYSICS 0
#endif
#ifndef DE_HAS_COOKER
#define DE_HAS_COOKER 0
#endif
#if DE_HAS_PHYSICS
#include <dengine/de_physics.h>
#endif

#define DEG45_RAD 0.785398163f
#define WAIT_NS   ((u64)60 * 1000000000u)

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

/* A quad (counter-clockwise seen from +Z) as two triangles. */
static void add_quad(de_nav *n, de_dvec3 a, de_dvec3 b, de_dvec3 c, de_dvec3 d) {
    const de_dvec3 t[6] = {a, b, c, a, c, d};
    ASSERT_EQ(de__nav_add_test_tris(n, t, 2), DE_OK);
}

static void add_plane(de_nav *n) {
    add_quad(n, P(0, 0, 0), P(100, 0, 0), P(100, 100, 0), P(0, 100, 0));
}

/* An axis-aligned solid box: its 6 faces, outward. */
static void add_box(de_nav *n, de_dvec3 c, f64 h) {
    const f64 x0 = c.x - h, x1 = c.x + h, y0 = c.y - h, y1 = c.y + h, z0 = c.z - h, z1 = c.z + h;
    add_quad(n, P(x0, y0, z1), P(x1, y0, z1), P(x1, y1, z1), P(x0, y1, z1)); /* top */
    add_quad(n, P(x0, y0, z0), P(x0, y1, z0), P(x1, y1, z0), P(x1, y0, z0)); /* bottom */
    add_quad(n, P(x0, y0, z0), P(x1, y0, z0), P(x1, y0, z1), P(x0, y0, z1)); /* -Y */
    add_quad(n, P(x1, y1, z0), P(x0, y1, z0), P(x0, y1, z1), P(x1, y1, z1)); /* +Y */
    add_quad(n, P(x0, y1, z0), P(x0, y0, z0), P(x0, y0, z1), P(x0, y1, z1)); /* -X */
    add_quad(n, P(x1, y0, z0), P(x1, y1, z0), P(x1, y1, z1), P(x1, y0, z1)); /* +X */
}

static void log_quiet(void) {
    de_log_desc ld;
    memset(&ld, 0, sizeof ld);
    ld.no_console_sink = true;
    ASSERT_EQ(de_log_init(&ld), DE_OK);
    de_fp_env_init_thread();
}

/* The box at (50, 50, 1), half extent 1, agent radius 0.5: the ground is blocked to 1.5 m from
 * the center along X; contour simplification may move the edge by about a cell. */
static void expect_box_hole(const de_nav *n, f64 cx, f64 cy) {
    EXPECT_FALSE(de__nav_point_on_mesh(n, P(cx, cy, 0), 0.3f));
    EXPECT_FALSE(de__nav_point_on_mesh(n, P(cx + 1.1, cy, 0), 0.3f));
    EXPECT_FALSE(de__nav_point_on_mesh(n, P(cx, cy - 1.1, 0), 0.3f));
    EXPECT_FALSE(de__nav_point_on_mesh(n, P(cx - 1.1, cy + 1.1, 0), 0.3f));
    EXPECT_TRUE(de__nav_point_on_mesh(n, P(cx + 2.2, cy, 0), 0.3f));
    EXPECT_TRUE(de__nav_point_on_mesh(n, P(cx, cy + 2.2, 0), 0.3f));
    EXPECT_TRUE(de__nav_point_on_mesh(n, P(cx - 2.2, cy - 2.2, 0), 0.3f));
}

TEST(nav, nav_build_flat_plane) {
    log_quiet();
    de_nav *n = NULL;
    ASSERT_EQ(de_nav_create(NULL, NULL, &n), DE_OK);
    add_plane(n);
    const de_navmesh_build_desc d = build_desc();
    ASSERT_EQ(de_nav_rebuild_tiles(n, &d, box(0, 0, -1, 100, 100, 1)), DE_OK);
    /* Asynchronous: nothing until the next update. */
    EXPECT_EQ(de__nav_pending_jobs(n), 1u);
    EXPECT_EQ(de__nav_tile_count(n), 0u);
    de_dvec3 o;
    ASSERT_TRUE(de__nav_origin(n, &o));
    EXPECT_TRUE(o.x == 0.0);
    EXPECT_TRUE(o.y == 0.0);
    EXPECT_TRUE(o.z == 0.0);
    de_nav_update(n);
    EXPECT_EQ(de__nav_pending_jobs(n), 0u);
    /* 100 m / 32 m: x tiles 0..3, y tiles -4..-1 (Recast z = -y). */
    EXPECT_EQ(de__nav_tile_count(n), 16u);
    EXPECT_TRUE(de__nav_point_on_mesh(n, P(50, 50, 0), 0.3f));
    EXPECT_TRUE(de__nav_point_on_mesh(n, P(5, 95, 0), 0.3f));
    EXPECT_TRUE(de__nav_point_on_mesh(n, P(33, 31, 0), 0.3f)); /* across a tile corner */
    EXPECT_FALSE(de__nav_point_on_mesh(n, P(150, 50, 0), 0.3f));
    EXPECT_FALSE(de__nav_point_on_mesh(n, P(50, 50, 5), 0.3f));
    /* The plane's edge is eroded by the agent radius. */
    EXPECT_FALSE(de__nav_point_on_mesh(n, P(0.1, 50, 0), 0.3f));
    i32 tx = 0, tz = 0;
    ASSERT_TRUE(de__nav_tile_of(n, P(50, 50, 0), &tx, &tz));
    EXPECT_EQ(tx, 1);
    EXPECT_EQ(tz, -2);
    EXPECT_NE(de__nav_tile_ref(n, tx, tz), (u64)0);
    /* tile_size must divide 256 m; a later build cannot change it. */
    de_navmesh_build_desc bad = d;
    bad.tile_size = 24.0f;
    EXPECT_NE(de_nav_rebuild_tiles(n, &bad, box(0, 0, -1, 10, 10, 1)), DE_OK);
    bad.tile_size = 16.0f;
    EXPECT_NE(de_nav_rebuild_tiles(n, &bad, box(0, 0, -1, 10, 10, 1)), DE_OK);
    de_nav_destroy(n);
    de_log_shutdown();
}

TEST(nav, nav_build_box_carves_hole) {
    log_quiet();
    de_type_registry *reg = NULL;
    ASSERT_EQ(de_type_registry_create(NULL, &reg), DE_OK);
    de_world *w = NULL;
    ASSERT_EQ(de_world_create(&(de_world_desc){.types = reg}, &w), DE_OK);
    const de_builtin_component_ids b = de_builtin_components(w);
    ASSERT_TRUE(DE_HANDLE_VALID(b.transform));

    de_nav *n = NULL;
    ASSERT_EQ(de_nav_create(NULL, NULL, &n), DE_OK);
    de_nav_attach(n, w);
    add_plane(n);
    add_box(n, P(50, 50, 1), 1.0);
    /* An entity obstacle: a 2 m box footprint at (20, 70). */
    const de_entity e = de_entity_spawn(w);
    de_transform *t = (de_transform *)de_entity_add(w, e, b.transform);
    ASSERT_TRUE(t != NULL);
    *t = de_transform_identity();
    t->position = P(20, 70, 1);
    de_vec3 he;
    he.x = he.y = he.z = 1.0f;
    de_nav_add_obstacle(n, e, he);

    const de_navmesh_build_desc d = build_desc();
    ASSERT_EQ(de_nav_rebuild_tiles(n, &d, box(0, 0, -1, 100, 100, 3)), DE_OK);
    de_nav_update(n);
    EXPECT_EQ(de__nav_tile_count(n), 16u);
    expect_box_hole(n, 50, 50);
    expect_box_hole(n, 20, 70);

    /* The world attachment: the nav.update system and the resource; detaching removes both. */
    de_nav_attach(n, NULL);
    de_nav_destroy(n);
    de_world_destroy(w);
    de_type_registry_destroy(reg);
    de_log_shutdown();
}

TEST(nav, nav_rebuild_region_only) {
    log_quiet();
    de_task_system *ts = NULL;
    ASSERT_EQ(de_task_system_create(&(de_task_system_desc){.worker_count = 2}, &ts), DE_OK);
    de_nav *n = NULL;
    ASSERT_EQ(de_nav_create(NULL, ts, &n), DE_OK);
    add_plane(n);
    const de_navmesh_build_desc d = build_desc();
    ASSERT_EQ(de_nav_rebuild_tiles(n, &d, box(0, 0, -1, 100, 100, 3)), DE_OK);
    u64 waited = 0;
    while (de__nav_pending_jobs(n) > 0 && waited < WAIT_NS) {
        de_nav_update(n);
        if (de__nav_pending_jobs(n) > 0) {
            de_sleep_ns(1000000u);
            waited += 1000000u;
        }
    }
    ASSERT_EQ(de__nav_pending_jobs(n), 0u);
    ASSERT_EQ(de__nav_tile_count(n), 16u);
    u64 before[4][5];
    for (i32 tx = 0; tx < 4; ++tx)
        for (i32 tz = -4; tz <= 0; ++tz)
            before[tx][tz + 4] = de__nav_tile_ref(n, tx, tz);
    EXPECT_TRUE(de__nav_point_on_mesh(n, P(7, 7, 0), 0.3f));

    /* A box appears in tile (0, -1); rebuild only the region around it. */
    add_box(n, P(7, 7, 1), 1.0);
    ASSERT_EQ(de_nav_rebuild_tiles(n, &d, box(5, 5, -1, 10, 10, 3)), DE_OK);
    /* Nothing changes before the build is applied. */
    for (i32 tx = 0; tx < 4; ++tx)
        for (i32 tz = -4; tz <= 0; ++tz)
            EXPECT_EQ(de__nav_tile_ref(n, tx, tz), before[tx][tz + 4]);
    EXPECT_TRUE(de__nav_point_on_mesh(n, P(7, 7, 0), 0.3f));
    waited = 0;
    while (de__nav_pending_jobs(n) > 0 && waited < WAIT_NS) {
        de_nav_update(n);
        if (de__nav_pending_jobs(n) > 0) {
            de_sleep_ns(1000000u);
            waited += 1000000u;
        }
    }
    ASSERT_EQ(de__nav_pending_jobs(n), 0u);
    EXPECT_EQ(de__nav_tile_count(n), 16u);
    for (i32 tx = 0; tx < 4; ++tx) {
        for (i32 tz = -4; tz <= 0; ++tz) {
            if (tx == 0 && tz == -1)
                EXPECT_NE(de__nav_tile_ref(n, tx, tz), before[tx][tz + 4]);
            else
                EXPECT_EQ(de__nav_tile_ref(n, tx, tz), before[tx][tz + 4]);
        }
    }
    expect_box_hole(n, 7, 7);
    de_nav_destroy(n);
    de_task_system_destroy(ts);
    de_log_shutdown();
}

TEST(nav, nav_coord_swizzle_roundtrip) {
    log_quiet();
    static const f32 vals[] = {0.0f, -0.0f, 1.0f, -1.0f, 0.1f, -255.75f, 1e-30f, 3.4e38f, -7.125f};
    const u32 nv = (u32)(sizeof vals / sizeof vals[0]);
    for (u32 i = 0; i < nv; ++i) {
        for (u32 j = 0; j < nv; ++j) {
            for (u32 k = 0; k < nv; ++k) {
                de_vec3 e;
                e.x = vals[i];
                e.y = vals[j];
                e.z = vals[k];
                f32 r[3];
                de__nav_to_recast(e, r);
                /* Recast is Y up: engine up (Z) is Recast Y; engine Y is Recast -Z. */
                EXPECT_TRUE(memcmp(&r[0], &e.x, sizeof(f32)) == 0);
                EXPECT_TRUE(memcmp(&r[1], &e.z, sizeof(f32)) == 0);
                const de_vec3 back = de__nav_from_recast(r);
                EXPECT_TRUE(memcmp(&back, &e, sizeof e) == 0);
            }
        }
    }
    /* World points relative to a 256 m-grid origin, at f32-exact offsets. */
    const de_dvec3 origin = de__nav_origin_for(P(1000.5, -300.25, 42.0));
    EXPECT_TRUE(origin.x == 768.0);
    EXPECT_TRUE(origin.y == -512.0);
    EXPECT_TRUE(origin.z == 0.0);
    const de_dvec3 p = P(900.125, -400.5, 12.75);
    f32 r[3];
    de__nav_world_to_recast(&origin, p, r);
    EXPECT_TRUE(r[0] == 132.125f);
    EXPECT_TRUE(r[1] == 12.75f);
    EXPECT_TRUE(r[2] == -111.5f);
    const de_dvec3 q = de__nav_recast_to_world(&origin, r);
    EXPECT_TRUE(q.x == p.x);
    EXPECT_TRUE(q.y == p.y);
    EXPECT_TRUE(q.z == p.z);

    /* Ramps rising along +Y: 20 degrees (walkable under the 45 degree limit), 60 (not). */
    de_nav *n = NULL;
    ASSERT_EQ(de_nav_create(NULL, NULL, &n), DE_OK);
    const f64 tan20 = 0.36397023426620234, tan60 = 1.7320508075688772;
    add_quad(n, P(10, 10, 0), P(20, 10, 0), P(20, 30, 20 * tan20), P(10, 30, 20 * tan20));
    add_quad(n, P(40, 10, 0), P(50, 10, 0), P(50, 15, 5 * tan60), P(40, 15, 5 * tan60));
    const de_navmesh_build_desc d = build_desc();
    ASSERT_EQ(de_nav_rebuild_tiles(n, &d, box(0, 0, -1, 60, 40, 10)), DE_OK);
    de_nav_update(n);
    EXPECT_TRUE(de__nav_point_on_mesh(n, P(15, 20, 10 * tan20), 0.3f));
    EXPECT_TRUE(de__nav_point_on_mesh(n, P(15, 25, 15 * tan20), 0.3f));
    EXPECT_FALSE(de__nav_point_on_mesh(n, P(45, 12.5, 2.5 * tan60), 0.5f));
    /* The same ramp is walkable when the limit is above 60 degrees. */
    de_nav *n2 = NULL;
    ASSERT_EQ(de_nav_create(NULL, NULL, &n2), DE_OK);
    add_quad(n2, P(40, 10, 0), P(50, 10, 0), P(50, 15, 5 * tan60), P(40, 15, 5 * tan60));
    de_navmesh_build_desc steep = d;
    steep.agent_max_slope_rad = 1.1344640f; /* 65 degrees */
    steep.agent_max_climb = 1.0f;
    ASSERT_EQ(de_nav_rebuild_tiles(n2, &steep, box(0, 0, -1, 60, 40, 10)), DE_OK);
    de_nav_update(n2);
    EXPECT_TRUE(de__nav_point_on_mesh(n2, P(45, 12.5, 2.5 * tan60), 0.5f));
    de_nav_destroy(n2);
    de_nav_destroy(n);
    de_log_shutdown();
}

TEST(nav, nav_input_from_colliders) {
#if !DE_HAS_PHYSICS
    SKIP("this build has no physics module (DE_WITH_PHYSICS off)");
#else
    log_quiet();
    de_type_registry *reg = NULL;
    ASSERT_EQ(de_type_registry_create(NULL, &reg), DE_OK);
    de_world *w = NULL;
    ASSERT_EQ(de_world_create(&(de_world_desc){.types = reg}, &w), DE_OK);
    const de_builtin_component_ids b = de_builtin_components(w);
    de_physics_world *p = NULL;
    ASSERT_EQ(de_physics_create(NULL, &p), DE_OK);
    ASSERT_EQ(de_physics_attach(p, w, reg), DE_OK);
    const de_component cc = de_component_find(w, "DeCollider");
    const de_component rc = de_component_find(w, "DeRigidBody");
    ASSERT_TRUE(DE_HANDLE_VALID(cc));
    ASSERT_TRUE(DE_HANDLE_VALID(rc));

    struct spec {
        f64 x, y, z;
        f32 hx, hy, hz;
        u32 layer;
        bool trigger;
        int body; /* 0 none, 1 static, 2 dynamic */
    };
    const struct spec specs[] = {
        {50, 50, -0.5, 50, 50, 0.5, 1u, false, 0}, /* the ground */
        {50, 50, 1, 1, 1, 1, 1u, false, 1},        /* static body: carves */
        {20, 20, 1, 1, 1, 1, 1u, false, 2},        /* dynamic: ignored */
        {80, 20, 1, 1, 1, 1, 1u, true, 0},         /* trigger: ignored */
        {20, 80, 1, 1, 1, 1, 2u, false, 0},        /* layer 2, not in the mask: ignored */
    };
    for (u32 i = 0; i < sizeof specs / sizeof specs[0]; ++i) {
        const de_entity e = de_entity_spawn(w);
        de_transform *t = (de_transform *)de_entity_add(w, e, b.transform);
        ASSERT_TRUE(t != NULL);
        *t = de_transform_identity();
        t->position = P(specs[i].x, specs[i].y, specs[i].z);
        DeCollider *c = (DeCollider *)de_entity_add(w, e, cc);
        ASSERT_TRUE(c != NULL);
        c->offset = de_transform_identity();
        c->shape_kind = DE_COLLIDER_SHAPE_BOX;
        c->half_extents.x = specs[i].hx;
        c->half_extents.y = specs[i].hy;
        c->half_extents.z = specs[i].hz;
        c->filter.layer = specs[i].layer;
        c->filter.mask = 0xFFFFFFFFu;
        c->is_trigger = specs[i].trigger;
        if (specs[i].body != 0) {
            DeRigidBody *rb = (DeRigidBody *)de_entity_add(w, e, rc);
            ASSERT_TRUE(rb != NULL);
            rb->type = specs[i].body == 1 ? DE_BODY_STATIC : DE_BODY_DYNAMIC;
        }
    }

    de_nav *n = NULL;
    ASSERT_EQ(de_nav_create(NULL, NULL, &n), DE_OK);
    de_nav_attach(n, w);
    de_navmesh_build_desc d = build_desc();
    d.collision_mask = 1u;
    ASSERT_EQ(de_nav_rebuild_tiles(n, &d, box(0, 0, -1, 100, 100, 3)), DE_OK);
    de_nav_update(n);
    EXPECT_EQ(de__nav_tile_count(n), 16u);
    expect_box_hole(n, 50, 50);
    EXPECT_TRUE(de__nav_point_on_mesh(n, P(20, 20, 0), 0.3f));
    EXPECT_TRUE(de__nav_point_on_mesh(n, P(80, 20, 0), 0.3f));
    EXPECT_TRUE(de__nav_point_on_mesh(n, P(20, 80, 0), 0.3f));

    /* With every layer (mask 0) the layer-2 box carves too. */
    d.collision_mask = 0u;
    ASSERT_EQ(de_nav_rebuild_tiles(n, &d, box(0, 0, -1, 100, 100, 3)), DE_OK);
    de_nav_update(n);
    expect_box_hole(n, 20, 80);
    EXPECT_TRUE(de__nav_point_on_mesh(n, P(20, 20, 0), 0.3f));

    de_nav_destroy(n);
    de_world_destroy(w);
    de_physics_destroy(p);
    de_type_registry_destroy(reg);
    de_log_shutdown();
#endif
}

static const char k_source[] =
    "[build]\n"
    "cell_size = 0.25\ncell_height = 0.1\nagent_radius = 0.5\nagent_height = 2.0\n"
    "agent_max_climb = 0.4\nagent_max_slope_deg = 45.0\ntile_size = 32.0\n"
    "[region]\nmin = [0.0, 0.0, -1.0]\nmax = [100.0, 100.0, 3.0]\n"
    "[[mesh]]\n"
    "vertices = [[0.0, 0.0, 0.0], [100.0, 0.0, 0.0], [100.0, 100.0, 0.0], [0.0, 100.0, 0.0]]\n"
    "indices = [0, 1, 2, 0, 2, 3]\n"
    "[[box]]\ncenter = [50.0, 50.0, 1.0]\nhalf_extents = [1.0, 1.0, 1.0]\n";

TEST(nav, nav_asset_cook_roundtrip) {
    log_quiet();
    const de_allocator *a = de_heap_allocator();
    char why[256];
    u8 *c1 = NULL, *c2 = NULL;
    u64 s1 = 0, s2 = 0;
    ASSERT_EQ(de__nav_cook_source(a, k_source, sizeof k_source - 1u, &c1, &s1, why, sizeof why),
              DE_OK);
    ASSERT_EQ(de__nav_cook_source(a, k_source, sizeof k_source - 1u, &c2, &s2, why, sizeof why),
              DE_OK);
    ASSERT_EQ(s1, s2);
    EXPECT_TRUE(memcmp(c1, c2, (size_t)s1) == 0);
    de__nav_container_info info;
    const char *msg = NULL;
    ASSERT_TRUE(de__nav_container_read(c1, s1, &info, &msg));
    EXPECT_EQ(info.tile_count, 16u);
    EXPECT_TRUE(info.desc.tile_size == 32.0f);
    EXPECT_TRUE(info.origin.x == 0.0);
    /* Truncated and corrupted containers are refused. */
    EXPECT_FALSE(de__nav_container_read(c1, s1 - 1u, &info, &msg));
    EXPECT_FALSE(de__nav_container_read(c1, DE__NAV_HEADER_SIZE - 1u, &info, &msg));
    c2[0] ^= 0xFFu;
    EXPECT_FALSE(de__nav_container_read(c2, s2, &info, &msg));
    /* Bad sources name the problem. */
    u8 *bad = NULL;
    u64 bad_size = 0;
    EXPECT_NE(de__nav_cook_source(a, "[build]\ncell_size = 0.25\n", 24, &bad, &bad_size, why,
                                  sizeof why),
              DE_OK);
    EXPECT_TRUE(why[0] != '\0');

    /* The tiles load into a nav. */
    de_nav *n = NULL;
    ASSERT_EQ(de_nav_create(NULL, NULL, &n), DE_OK);
    ASSERT_EQ(de__nav_add_container(n, c1, s1), DE_OK);
    EXPECT_EQ(de__nav_tile_count(n), 16u);
    expect_box_hole(n, 50, 50);
    de_nav_destroy(n);

#if DE_HAS_COOKER
    /* As an asset: the fixture cooks in-process and loads back byte-identical. */
    static bool mounted;
    if (!mounted) {
        ASSERT_EQ(de_fs_mount("navd", DE_NAV_TEST_DATA_DIR, 0, 0), DE_OK);
        mounted = true;
    }
    void *src = NULL;
    u64 src_size = 0;
    ASSERT_EQ(de_file_read_all("navd:/plane_box.navmesh", a, &src, &src_size), DE_OK);
    u8 *c3 = NULL;
    u64 s3 = 0;
    ASSERT_EQ(de__nav_cook_source(a, (const char *)src, src_size, &c3, &s3, why, sizeof why),
              DE_OK);
    de_type_registry *reg = NULL;
    ASSERT_EQ(de_type_registry_create(NULL, &reg), DE_OK);
    de_task_system *ts = NULL;
    ASSERT_EQ(de_task_system_create(&(de_task_system_desc){.worker_count = 1}, &ts), DE_OK);
    de_asset_system *s = NULL;
    ASSERT_EQ(de_asset_system_create(&(de_asset_system_desc){.tasks = ts,
                                                             .io_threads = 1,
                                                             .cache_dir = "nocache:/decache",
                                                             .types = reg},
                                     &s),
              DE_OK);
    ASSERT_EQ(de_nav_create(NULL, NULL, &n), DE_OK);
    ASSERT_EQ(de_nav_use_assets(n, s), DE_OK);
    ASSERT_EQ(de_asset_manifest_load(s, "navd:/assets.toml"), DE_OK);
    const de_asset_id id = de_asset_id_from("nav/plane_box");
    const de_load_handle h = de_asset_load(s, id, 0);
    const de_load_state st = de_asset_wait(s, h, WAIT_NS);
    if (st != DE_LOAD_READY) {
        de_load_info li;
        (void)de_asset_status(s, h, &li);
        printf("  nav/plane_box: %s\n", li.first_error_msg);
    }
    ASSERT_EQ((int)st, (int)DE_LOAD_READY);
    const de_navmesh m = de_asset_navmesh(s, id);
    ASSERT_TRUE(DE_HANDLE_VALID(m));
    u64 loaded_size = 0;
    const u8 *loaded = de__nav_asset_bytes(s, m, &loaded_size);
    ASSERT_TRUE(loaded != NULL);
    ASSERT_EQ(loaded_size, s3);
    EXPECT_TRUE(memcmp(loaded, c3, (size_t)s3) == 0);
    de_nav_add_navmesh(n, m);
    EXPECT_EQ(de__nav_tile_count(n), 16u);
    expect_box_hole(n, 50, 50);
    de_nav_destroy(n);
    de_asset_release(s, h);
    de_asset_system_destroy(s);
    de_task_system_destroy(ts);
    de_type_registry_destroy(reg);
    DE_FREE(a, c3, (size_t)s3);
    DE_FREE(a, src, (size_t)src_size);
#endif
    DE_FREE(a, c1, (size_t)s1);
    DE_FREE(a, c2, (size_t)s2);
    de_log_shutdown();
}

TEST(nav, nav_allocator_hooks) {
    log_quiet();
    test_counting_alloc ca;
    test_counting_init(&ca);
    const u64 unscoped = de__nav_rc_unscoped_allocs();
    de_nav *n = NULL;
    ASSERT_EQ(de_nav_create(&ca.iface, NULL, &n), DE_OK);
    add_plane(n);
    add_box(n, P(50, 50, 1), 1.0);
    const de_navmesh_build_desc d = build_desc();
    ASSERT_EQ(de_nav_rebuild_tiles(n, &d, box(0, 0, -1, 100, 100, 3)), DE_OK);
    de_nav_update(n);
    EXPECT_EQ(de__nav_tile_count(n), 16u);
    /* Rebuild part of it: old tiles are freed, new ones allocated, through the same allocator. */
    ASSERT_EQ(de_nav_rebuild_tiles(n, &d, box(40, 40, -1, 60, 60, 3)), DE_OK);
    de_nav_update(n);
    const u64 allocs = ca.allocs;
    EXPECT_GT(allocs, (u64)100);
    de_nav_destroy(n);
    EXPECT_EQ(ca.live_blocks, (u64)0);
    EXPECT_EQ(ca.live_bytes, (u64)0);
    EXPECT_EQ(ca.size_errors, (u64)0);
    EXPECT_EQ(ca.align_errors, (u64)0);

    /* The cooker path too. */
    u8 *c = NULL;
    u64 cs = 0;
    char why[256];
    ASSERT_EQ(de__nav_cook_source(&ca.iface, k_source, sizeof k_source - 1u, &c, &cs, why,
                                  sizeof why),
              DE_OK);
    DE_FREE(&ca.iface, c, (size_t)cs);
    EXPECT_EQ(ca.live_blocks, (u64)0);
    EXPECT_EQ(ca.size_errors, (u64)0);
    EXPECT_EQ(de__nav_rc_unscoped_allocs(), unscoped);

    /* Out of memory in the nav itself fails cleanly. (Recast's rcVector does not survive an
     * allocation failure inside a build, upstream, so no failure is injected there.) */
    test_counting_alloc fa;
    test_counting_init(&fa);
    fa.fail_after = 0;
    de_nav *f = NULL;
    EXPECT_NE(de_nav_create(&fa.iface, NULL, &f), DE_OK);
    EXPECT_TRUE(f == NULL);
    EXPECT_EQ(fa.live_blocks, (u64)0);
    de_log_shutdown();
}
