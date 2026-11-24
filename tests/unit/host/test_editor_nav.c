/* DENG-582: world.nav (DIDE protocol 1.25, DIDE-128, DIDE-R174) through the real handlers and the
 * generated protocol readers and writers:
 *   info_and_errors          nav.info defaults and the conformance error cases
 *   wall_move_rebakes_tiles  AC1: moving a wall rebakes only its tiles; the test path goes around
 *   jump_link_used           AC2: a DeNavLink across a gap is used by nav.testPath
 *   cancel_and_overlay       nav.cancelBake and nav.overlay round-trip through nav.info
 *   settings_persist         DENG-833: nav.configure writes nav.toml; a new service reads it back
 *   plane2d_in_info          DENG-863: nav.info.plane2d is side_xz by default, or the system's plane
 *   test_path2d_found        DENG-863: nav.testPath2d finds a path around blocked cells (jps on, off)
 *   test_path2d_errors       DENG-863: each not_found and invalid_params case of nav.testPath2d
 *   overlay_grids            DENG-863: nav.overlay grids draws the DeNavGrid2D cells
 */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "de_test.h"
#if !defined(DE_SHIPPING)
#include "ecs/ecs_internal.h"
#include "gameplay/nav/nav_components.h"
#include "host/editor/editor_internal.h"
#include "host/host_internal.h"
#include <dengine/de_debug.h>
#include <dengine/de_ecs.h>
#include <dengine/de_physics.h>
#include <dengine/de_gameplay.h>
#include <dengine/de_platform.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif

#define STORAGE  (4u << 20)
#define RESPONSE (1u << 20)
#define PARAMS   (64u << 10)

#define EXPECT_NEAR(a, b, t) EXPECT_TRUE(fabs((a) - (b)) < (t))

typedef struct nav_fx {
    de_engine engine;
    de__editor editor;
    de_physics_world *p;
    de_component collider, link;
    unsigned char *storage;
    char *response;
    char *params;
    char *out; /* the editor's notification buffer */
    mpack_tree_t tree;
    bool parsed;
    /* The last nav.bake note. */
    char state[16];
    u32 bake, done, total;
    bool auto_;
} nav_fx;

static void nav_fx_setup(nav_fx *f) {
    memset(f, 0, sizeof *f);
    de_fp_env_init_thread();
    ASSERT_EQ(de_type_registry_create(NULL, &f->engine.types), DE_OK);
    ASSERT_EQ(de_world_create(&(de_world_desc){.types = f->engine.types}, &f->engine.world), DE_OK);
    ASSERT_EQ(de_physics_create(&(de_physics_desc){0}, &f->p), DE_OK);
    ASSERT_EQ(de_physics_attach(f->p, f->engine.world, f->engine.types), DE_OK);
    f->collider = de_component_find(f->engine.world, "DeCollider");
    ASSERT_TRUE(DE_HANDLE_VALID(f->collider));
    f->editor.e = &f->engine;
    f->editor.mode = DE_EP_WORLD_MODE_EDIT;
    f->storage = (unsigned char *)malloc(STORAGE);
    f->response = (char *)malloc(RESPONSE);
    f->params = (char *)malloc(PARAMS);
    f->out = (char *)malloc(RESPONSE);
    ASSERT_TRUE(f->storage != NULL && f->response != NULL && f->params != NULL && f->out != NULL);
    f->editor.note = f->out;
    f->editor.note_cap = RESPONSE;
    f->editor.connected = true; /* no IPC: de__ed_send drops the frame, the buffer keeps it */
    de__ed_nav_register(&f->editor);
    f->link = de_component_find(f->engine.world, "DeNavLink");
    ASSERT_TRUE(DE_HANDLE_VALID(f->link));
    ASSERT_TRUE(DE_HANDLE_VALID(de_component_find(f->engine.world, "DeNavModifier")));
    ASSERT_TRUE(DE_HANDLE_VALID(de_component_find(f->engine.world, "DeNavGrid2D")));
}
static void nav_fx_teardown(nav_fx *f) {
    if (f->parsed)
        mpack_tree_destroy(&f->tree);
    for (u32 i = 0; i < f->editor.reset_count; ++i)
        f->editor.resets[i](&f->editor);
    de__ed_nav_shutdown(&f->editor);
    de_world_destroy(f->engine.world);
    de_physics_destroy(f->p);
    de_type_registry_destroy(f->engine.types);
    free(f->storage);
    free(f->response);
    free(f->params);
    free(f->out);
}

static mpack_node_t call(nav_fx *f, de_ep_method m, const void *params) {
    mpack_writer_t w;
    mpack_writer_init(&w, f->params, PARAMS);
    switch (m) {
    case DE_EP_METHOD_NAV_INFO:
        mpack_start_map(&w, 0);
        mpack_finish_map(&w);
        break;
    case DE_EP_METHOD_NAV_CONFIGURE:
        de_ep_write_nav_configure_params(&w, (const de_ep_nav_configure_params *)params);
        break;
    case DE_EP_METHOD_NAV_BAKE:
        de_ep_write_nav_bake_params(&w, (const de_ep_nav_bake_params *)params);
        break;
    case DE_EP_METHOD_NAV_CANCEL_BAKE:
        de_ep_write_nav_cancel_bake_params(&w, (const de_ep_nav_cancel_bake_params *)params);
        break;
    case DE_EP_METHOD_NAV_TILES:
        de_ep_write_nav_tiles_params(&w, (const de_ep_nav_tiles_params *)params);
        break;
    case DE_EP_METHOD_NAV_TILE_MESH:
        de_ep_write_nav_tile_mesh_params(&w, (const de_ep_nav_tile_mesh_params *)params);
        break;
    case DE_EP_METHOD_NAV_TEST_PATH:
        de_ep_write_nav_test_path_params(&w, (const de_ep_nav_test_path_params *)params);
        break;
    default:
        de_ep_write_nav_overlay_params(&w, (const de_ep_nav_overlay_params *)params);
        break;
    }
    const size_t psize = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    const de_ep_request req = {.id = 7, .method = de_ep_cstr("nav"), .params = {f->params, psize}};
    ASSERT_TRUE(f->editor.handlers[m] != NULL);
    de_ep_arena a = {f->storage, STORAGE, 0};
    mpack_writer_init(&w, f->response, RESPONSE);
    f->editor.handlers[m](&f->editor, &req, &a, &w);
    const size_t size = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    if (f->parsed)
        mpack_tree_destroy(&f->tree);
    mpack_tree_init_data(&f->tree, f->response, size);
    mpack_tree_parse(&f->tree);
    f->parsed = true;
    ASSERT_EQ(mpack_tree_error(&f->tree), mpack_ok);
    mpack_node_t root = mpack_tree_root(&f->tree);
    ASSERT_EQ(mpack_node_u64(mpack_node_map_cstr(root, "id")), 7u);
    return root;
}
static mpack_node_t result_of(mpack_node_t root) {
    if (mpack_node_map_contains_cstr(root, "error")) {
        mpack_node_t m = mpack_node_map_cstr(mpack_node_map_cstr(root, "error"), "message");
        fprintf(stderr, "error: %.*s\n", (int)mpack_node_strlen(m), mpack_node_str(m));
    }
    ASSERT_FALSE(mpack_node_map_contains_cstr(root, "error"));
    return mpack_node_map_cstr(root, "result");
}
static void expect_error(mpack_node_t root, de_ep_error code) {
    EXPECT_FALSE(mpack_node_map_contains_cstr(root, "result"));
    mpack_node_t error = mpack_node_map_cstr(root, "error");
    EXPECT_EQ(mpack_node_i32(mpack_node_map_cstr(error, "code")), (i32)code);
}
static bool is_str(mpack_node_t n, const char *s) {
    return mpack_node_type(n) == mpack_type_str && mpack_node_strlen(n) == strlen(s) &&
           memcmp(mpack_node_str(n), s, strlen(s)) == 0;
}

/* Runs one editor frame and records the nav.bake note it sent, if any. */
static void frame(nav_fx *f) {
    const u64 seq = f->editor.seq;
    for (u32 i = 0; i < f->editor.tick_count; ++i)
        f->editor.ticks[i](&f->editor);
    if (f->editor.seq == seq)
        return;
    mpack_tree_t t;
    mpack_tree_init_data(&t, f->out, RESPONSE);
    mpack_tree_parse(&t);
    ASSERT_EQ(mpack_tree_error(&t), mpack_ok);
    mpack_node_t root = mpack_tree_root(&t);
    EXPECT_TRUE(is_str(mpack_node_map_cstr(root, "topic"), "nav.bake"));
    mpack_node_t d = mpack_node_map_cstr(root, "data");
    mpack_node_t st = mpack_node_map_cstr(d, "state");
    const size_t n = mpack_node_strlen(st) < 15 ? mpack_node_strlen(st) : 15;
    memcpy(f->state, mpack_node_str(st), n);
    f->state[n] = '\0';
    f->bake = mpack_node_u32(mpack_node_map_cstr(d, "bake"));
    f->done = mpack_node_u32(mpack_node_map_cstr(d, "done"));
    f->total = mpack_node_u32(mpack_node_map_cstr(d, "total"));
    f->auto_ = mpack_node_bool(mpack_node_map_cstr(d, "auto"));
    mpack_tree_destroy(&t);
}
/* Runs frames until a bake note says done (or failed), at most `max` frames. */
static bool run_until_done(nav_fx *f, u32 max) {
    f->state[0] = '\0';
    for (u32 i = 0; i < max; ++i) {
        frame(f);
        if (strcmp(f->state, "done") == 0)
            return true;
        if (strcmp(f->state, "failed") == 0)
            return false;
    }
    return false;
}

/* A static box collider. */
static de_entity add_box(nav_fx *f, f64 x, f64 y, f64 z, f32 hx, f32 hy, f32 hz) {
    de_world *w = f->engine.world;
    const de_builtin_component_ids b = de_builtin_components(w);
    const de_entity e = de_entity_spawn(w);
    de_transform *t = (de_transform *)de_entity_add(w, e, b.transform);
    ASSERT_TRUE(t != NULL);
    *t = de_transform_identity();
    t->position.x = x, t->position.y = y, t->position.z = z;
    DeCollider *c = (DeCollider *)de_entity_add(w, e, f->collider);
    ASSERT_TRUE(c != NULL);
    c->offset = de_transform_identity();
    c->shape_kind = DE_COLLIDER_SHAPE_BOX;
    c->half_extents.x = hx, c->half_extents.y = hy, c->half_extents.z = hz;
    c->filter.layer = 1u;
    c->filter.mask = 0xFFFFFFFFu;
    return e;
}
static void move(nav_fx *f, de_entity e, f64 x, f64 y, f64 z) {
    const de_builtin_component_ids b = de_builtin_components(f->engine.world);
    de_transform *t = (de_transform *)de_entity_get(f->engine.world, e, b.transform);
    ASSERT_TRUE(t != NULL);
    t->position.x = x, t->position.y = y, t->position.z = z;
}

/* 32 x 32 m bounds in 8 m tiles: 4 x 4 tiles. */
static void configure_small(nav_fx *f, bool auto_rebake) {
    double mn[3] = {-16, -16, -2}, mx[3] = {16, 16, 6};
    de_ep_nav_configure_params q;
    memset(&q, 0, sizeof q);
    q.has_bounds = true;
    q.bounds.min = (de_ep_list_f64){mn, 3};
    q.bounds.max = (de_ep_list_f64){mx, 3};
    q.has_tile_size = true;
    q.tile_size = 8.0f;
    q.has_auto_rebake = true;
    q.auto_rebake = auto_rebake;
    mpack_node_t r = result_of(call(f, DE_EP_METHOD_NAV_CONFIGURE, &q));
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(r, "tilesX")), 4u);
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(r, "tilesY")), 4u);
}
static u32 bake_all(nav_fx *f) {
    de_ep_nav_bake_params q;
    memset(&q, 0, sizeof q);
    mpack_node_t r = result_of(call(f, DE_EP_METHOD_NAV_BAKE, &q));
    return mpack_node_u32(mpack_node_map_cstr(r, "bake"));
}

typedef struct tile_rec {
    char state[16];
    u64 revision;
    u32 polys;
} tile_rec;
/* Reads nav.tiles for the humanoid into recs[y * 4 + x]. */
static void read_tiles(nav_fx *f, tile_rec *recs) {
    const de_ep_nav_tiles_params q = {.agent = de_ep_cstr("humanoid")};
    mpack_node_t r = result_of(call(f, DE_EP_METHOD_NAV_TILES, &q));
    mpack_node_t tiles = mpack_node_map_cstr(r, "tiles");
    ASSERT_EQ(mpack_node_array_length(tiles), 16u);
    for (u32 i = 0; i < 16; ++i) {
        mpack_node_t t = mpack_node_array_at(tiles, i);
        const i32 x = mpack_node_i32(mpack_node_map_cstr(t, "x"));
        const i32 y = mpack_node_i32(mpack_node_map_cstr(t, "y"));
        tile_rec *d = &recs[y * 4 + x];
        mpack_node_t st = mpack_node_map_cstr(t, "state");
        const size_t n = mpack_node_strlen(st) < 15 ? mpack_node_strlen(st) : 15;
        memcpy(d->state, mpack_node_str(st), n);
        d->state[n] = '\0';
        d->revision = mpack_node_u64(mpack_node_map_cstr(t, "revision"));
        d->polys = mpack_node_u32(mpack_node_map_cstr(t, "polys"));
    }
}

typedef struct path_rec {
    bool ok, partial;
    u32 points;
    f64 max_abs_y, length;
    u32 links;
    u64 link0;
} path_rec;
static path_rec test_path(nav_fx *f, f64 x0, f64 y0, f64 x1, f64 y1) {
    double from[3] = {x0, y0, 0}, to[3] = {x1, y1, 0};
    de_ep_nav_test_path_params q;
    memset(&q, 0, sizeof q);
    q.agent = de_ep_cstr("humanoid");
    q.from = (de_ep_list_f64){from, 3};
    q.to = (de_ep_list_f64){to, 3};
    mpack_node_t root = call(f, DE_EP_METHOD_NAV_TEST_PATH, &q);
    path_rec p;
    memset(&p, 0, sizeof p);
    if (mpack_node_map_contains_cstr(root, "error"))
        return p;
    mpack_node_t r = mpack_node_map_cstr(root, "result");
    p.ok = true;
    p.partial = mpack_node_bool(mpack_node_map_cstr(r, "partial"));
    p.length = mpack_node_double(mpack_node_map_cstr(r, "length"));
    mpack_node_t pts = mpack_node_map_cstr(r, "points");
    p.points = (u32)mpack_node_array_length(pts) / 3;
    for (u32 i = 0; i < p.points; ++i) {
        const f64 y = fabs(mpack_node_double(mpack_node_array_at(pts, i * 3 + 1)));
        p.max_abs_y = y > p.max_abs_y ? y : p.max_abs_y;
    }
    mpack_node_t links = mpack_node_map_cstr(r, "links");
    p.links = (u32)mpack_node_array_length(links);
    if (p.links > 0)
        p.link0 = mpack_node_u64(mpack_node_array_at(links, 0));
    return p;
}

TEST_F(editor_nav, info_and_errors, nav_fx) {
    mpack_node_t r = result_of(call(f, DE_EP_METHOD_NAV_INFO, NULL));
    mpack_node_t up = mpack_node_map_cstr(r, "up");
    EXPECT_TRUE(mpack_node_double(mpack_node_array_at(up, 2)) == 1.0);
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(r, "tilesX")), 8u);
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(r, "maxAgents")), 4u);
    EXPECT_TRUE(is_str(mpack_node_map_cstr(r, "linkComponent"), "DeNavLink"));
    mpack_node_t agents = mpack_node_map_cstr(r, "agents");
    ASSERT_EQ(mpack_node_array_length(agents), 1u);
    EXPECT_TRUE(is_str(mpack_node_map_cstr(mpack_node_array_at(agents, 0), "name"), "humanoid"));
    EXPECT_EQ(mpack_node_array_length(mpack_node_map_cstr(r, "areas")), 4u);
    const u64 rev = mpack_node_u64(mpack_node_map_cstr(r, "revision"));

    const de_ep_nav_tiles_params tq = {.agent = de_ep_cstr("nobody")};
    expect_error(call(f, DE_EP_METHOD_NAV_TILES, &tq), DE_EP_ERR_INVALID_PARAMS);
    double p0[3] = {0, 0, 0};
    de_ep_nav_test_path_params pq;
    memset(&pq, 0, sizeof pq);
    pq.agent = de_ep_cstr("nobody");
    pq.from = (de_ep_list_f64){p0, 3};
    pq.to = (de_ep_list_f64){p0, 3};
    expect_error(call(f, DE_EP_METHOD_NAV_TEST_PATH, &pq), DE_EP_ERR_INVALID_PARAMS);
    de_ep_nav_bake_params bq;
    memset(&bq, 0, sizeof bq);
    bq.has_agent = true;
    bq.agent = de_ep_cstr("nobody");
    expect_error(call(f, DE_EP_METHOD_NAV_BAKE, &bq), DE_EP_ERR_INVALID_PARAMS);
    const de_ep_nav_cancel_bake_params cq = {0xFFFFFFFFu};
    expect_error(call(f, DE_EP_METHOD_NAV_CANCEL_BAKE, &cq), DE_EP_ERR_NOT_FOUND);
    de_ep_nav_area area = {.id = 1, .name = de_ep_cstr("road"), .cost = 1.0f};
    de_ep_nav_configure_params kq;
    memset(&kq, 0, sizeof kq);
    kq.has_areas = true;
    kq.areas = (de_ep_list_nav_area){&area, 1};
    expect_error(call(f, DE_EP_METHOD_NAV_CONFIGURE, &kq), DE_EP_ERR_INVALID_PARAMS);
    const de_ep_nav_tile_mesh_params mq = {.agent = de_ep_cstr("humanoid"), .x = -1, .y = -1};
    expect_error(call(f, DE_EP_METHOD_NAV_TILE_MESH, &mq), DE_EP_ERR_NOT_FOUND);

    /* Out-of-range values, a tile size that does not divide 256 m, unaligned bounds. */
    de_ep_nav_agent bad = {.name = de_ep_cstr("tiny"), .radius = 0.4f, .height = 1.8f,
                           .max_slope_deg = 95.0f, .step_height = 0.3f, .cell_size = 0.5f,
                           .cell_height = 0.2f};
    memset(&kq, 0, sizeof kq);
    kq.has_agents = true;
    kq.agents = (de_ep_list_nav_agent){&bad, 1};
    expect_error(call(f, DE_EP_METHOD_NAV_CONFIGURE, &kq), DE_EP_ERR_INVALID_PARAMS);
    memset(&kq, 0, sizeof kq);
    kq.has_tile_size = true;
    kq.tile_size = 7.0f;
    expect_error(call(f, DE_EP_METHOD_NAV_CONFIGURE, &kq), DE_EP_ERR_INVALID_PARAMS);
    double mn[3] = {-13, -16, -2}, mx[3] = {16, 16, 6};
    memset(&kq, 0, sizeof kq);
    kq.has_bounds = true;
    kq.bounds.min = (de_ep_list_f64){mn, 3};
    kq.bounds.max = (de_ep_list_f64){mx, 3};
    expect_error(call(f, DE_EP_METHOD_NAV_CONFIGURE, &kq), DE_EP_ERR_INVALID_PARAMS);

    /* A failed configure changes nothing; a good one bumps the revision. */
    r = result_of(call(f, DE_EP_METHOD_NAV_INFO, NULL));
    EXPECT_EQ(mpack_node_u64(mpack_node_map_cstr(r, "revision")), rev);
    configure_small(f, false);
    r = result_of(call(f, DE_EP_METHOD_NAV_INFO, NULL));
    EXPECT_EQ(mpack_node_u64(mpack_node_map_cstr(r, "revision")), rev + 1);

    /* An empty world bakes empty tiles, and nav.testPath finds no navmesh. */
    bake_all(f);
    ASSERT_TRUE(run_until_done(f, 200));
    EXPECT_EQ(f->total, 16u);
    EXPECT_EQ(f->done, 16u);
    EXPECT_FALSE(f->auto_);
    expect_error(call(f, DE_EP_METHOD_NAV_TEST_PATH,
                      &(de_ep_nav_test_path_params){.agent = de_ep_cstr("humanoid"),
                                                    .from = {p0, 3},
                                                    .to = {p0, 3}}),
                 DE_EP_ERR_NOT_FOUND);
}

/* AC1: a wall across the middle; the path goes around its end. Moving the wall marks only the
 * tiles it left and entered dirty, the automatic rebake rebakes just those, and the new path
 * still goes around. */
TEST_F(editor_nav, wall_move_rebakes_tiles, nav_fx) {
    add_box(f, 0, 0, -0.5, 16, 16, 0.5);                     /* the ground */
    const de_entity wall = add_box(f, -4, 0, 1.5, 0.5, 12, 1.5); /* x -4.5..-3.5, y -12..12 */
    configure_small(f, true);
    bake_all(f);
    ASSERT_TRUE(run_until_done(f, 400));
    EXPECT_EQ(f->done, 16u);
    tile_rec before[16];
    read_tiles(f, before);
    for (u32 i = 0; i < 16; ++i) {
        EXPECT_TRUE(strcmp(before[i].state, "baked") == 0);
        EXPECT_GT(before[i].polys, 0u);
    }

    path_rec p = test_path(f, -10, 0, 10, 0);
    ASSERT_TRUE(p.ok);
    EXPECT_FALSE(p.partial);
    EXPECT_TRUE(p.max_abs_y > 11.5); /* around an end of the wall */
    EXPECT_TRUE(p.length > 24.0);

    /* tileMesh of a baked tile has triangles in world space, inside the tile. */
    const de_ep_nav_tile_mesh_params mq = {.agent = de_ep_cstr("humanoid"), .x = 3, .y = 3};
    mpack_node_t m = result_of(call(f, DE_EP_METHOD_NAV_TILE_MESH, &mq));
    mpack_node_t pos = mpack_node_map_cstr(m, "positions");
    ASSERT_GT(mpack_node_array_length(pos), 0u);
    const f64 x = mpack_node_float(mpack_node_array_at(pos, 0));
    const f64 y = mpack_node_float(mpack_node_array_at(pos, 1));
    EXPECT_TRUE(x >= 7.9 && x <= 16.1 && y >= 7.9 && y <= 16.1);
    EXPECT_EQ(mpack_node_array_length(mpack_node_map_cstr(m, "triangles")) * 3,
              mpack_node_array_length(pos));

    /* Move the wall from x = -4 (tile column 1) to x = 4 (column 2). */
    move(f, wall, 4, 0, 1.5);
    tile_rec dirty[16];
    read_tiles(f, dirty);
    for (u32 y = 0; y < 4; ++y)
        for (u32 x = 0; x < 4; ++x) {
            const bool moved = x == 1 || x == 2;
            EXPECT_TRUE(strcmp(dirty[y * 4 + x].state, moved ? "dirty" : "baked") == 0);
        }
    ASSERT_TRUE(run_until_done(f, 400));
    EXPECT_TRUE(f->auto_);
    EXPECT_EQ(f->total, 8u);
    tile_rec after[16];
    read_tiles(f, after);
    for (u32 y = 0; y < 4; ++y)
        for (u32 x = 0; x < 4; ++x) {
            const u32 i = y * 4 + x;
            EXPECT_TRUE(strcmp(after[i].state, "baked") == 0);
            if (x == 1 || x == 2)
                EXPECT_GT(after[i].revision, before[i].revision);
            else
                EXPECT_EQ(after[i].revision, before[i].revision);
        }
    p = test_path(f, -10, 0, 10, 0);
    ASSERT_TRUE(p.ok);
    EXPECT_FALSE(p.partial);
    EXPECT_TRUE(p.max_abs_y > 11.5);
    /* Where the wall was, the way is now straight. */
    p = test_path(f, -8, 0, 0, 0);
    ASSERT_TRUE(p.ok);
    EXPECT_TRUE(p.max_abs_y < 1.0);
}

/* AC2: two floors with a 4 m gap. With no link the path is partial; a DeNavLink jump across the
 * gap rebakes its tile and the test path uses it, naming the link's entity. */
TEST_F(editor_nav, jump_link_used, nav_fx) {
    add_box(f, -9, 0, -0.5, 7, 16, 0.5); /* x -16..-2 */
    add_box(f, 9, 0, -0.5, 7, 16, 0.5);  /* x 2..16 */
    configure_small(f, true);
    bake_all(f);
    ASSERT_TRUE(run_until_done(f, 400));
    path_rec p = test_path(f, -8, 0, 8, 0);
    ASSERT_TRUE(p.ok);
    EXPECT_TRUE(p.partial);
    EXPECT_EQ(p.links, 0u);

    de_world *w = f->engine.world;
    const de_builtin_component_ids b = de_builtin_components(w);
    const de_entity e = de_entity_spawn(w);
    de_transform *t = (de_transform *)de_entity_add(w, e, b.transform);
    ASSERT_TRUE(t != NULL);
    *t = de_transform_identity();
    t->position.x = -3, t->position.y = 4, t->position.z = 0;
    DeNavLink *l = (DeNavLink *)de_entity_add(w, e, f->link);
    ASSERT_TRUE(l != NULL);
    l->end.x = 6, l->end.y = 0, l->end.z = 0;
    l->radius = 0.5f;
    l->bidirectional = false;
    l->area = 3;
    ASSERT_TRUE(run_until_done(f, 400));
    EXPECT_TRUE(f->auto_);
    EXPECT_GE(f->total, 1u);
    EXPECT_LE(f->total, 8u); /* only the tiles near the link's start */

    p = test_path(f, -8, 4, 8, 4);
    ASSERT_TRUE(p.ok);
    EXPECT_FALSE(p.partial);
    ASSERT_EQ(p.links, 1u);
    EXPECT_EQ(p.link0, e.bits);
    /* One way: the way back has no link. */
    p = test_path(f, 8, 0, -8, 0);
    ASSERT_TRUE(p.ok);
    EXPECT_TRUE(p.partial);
}

TEST_F(editor_nav, cancel_and_overlay, nav_fx) {
    add_box(f, 0, 0, -0.5, 16, 16, 0.5);
    configure_small(f, false);
    const u32 bake = bake_all(f);
    mpack_node_t r = result_of(call(f, DE_EP_METHOD_NAV_INFO, NULL));
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(r, "bake")), bake);
    /* configure and a second bake are refused while it runs. */
    de_ep_nav_configure_params kq;
    memset(&kq, 0, sizeof kq);
    expect_error(call(f, DE_EP_METHOD_NAV_CONFIGURE, &kq), DE_EP_ERR_CONFLICT);
    de_ep_nav_bake_params bq;
    memset(&bq, 0, sizeof bq);
    expect_error(call(f, DE_EP_METHOD_NAV_BAKE, &bq), DE_EP_ERR_CONFLICT);
    const de_ep_nav_cancel_bake_params cq = {bake};
    result_of(call(f, DE_EP_METHOD_NAV_CANCEL_BAKE, &cq));
    frame(f); /* the cancel note went out with the reply */
    expect_error(call(f, DE_EP_METHOD_NAV_CANCEL_BAKE, &cq), DE_EP_ERR_NOT_FOUND);
    for (u32 i = 0; i < 20; ++i)
        frame(f);
    r = result_of(call(f, DE_EP_METHOD_NAV_INFO, NULL));
    EXPECT_FALSE(mpack_node_map_contains_cstr(r, "bake") &&
                 mpack_node_type(mpack_node_map_cstr(r, "bake")) != mpack_type_nil);

    /* nav.overlay round-trips through nav.info.overlay. */
    double path[6] = {0, 0, 0, 4, 4, 0};
    de_ep_nav_overlay_params oq;
    memset(&oq, 0, sizeof oq);
    oq.has_agent = true;
    oq.agent = de_ep_cstr("humanoid");
    oq.navmesh = oq.tiles = oq.links = true;
    oq.has_path = true;
    oq.path = (de_ep_list_f64){path, 6};
    result_of(call(f, DE_EP_METHOD_NAV_OVERLAY, &oq));
    frame(f); /* draws */
    r = result_of(call(f, DE_EP_METHOD_NAV_INFO, NULL));
    mpack_node_t ov = mpack_node_map_cstr(r, "overlay");
    EXPECT_TRUE(is_str(mpack_node_map_cstr(ov, "agent"), "humanoid"));
    EXPECT_TRUE(mpack_node_bool(mpack_node_map_cstr(ov, "navmesh")));
    EXPECT_EQ(mpack_node_array_length(mpack_node_map_cstr(ov, "path")), 6u);
    oq.path.count = 5;
    expect_error(call(f, DE_EP_METHOD_NAV_OVERLAY, &oq), DE_EP_ERR_INVALID_PARAMS);
}

/* DENG-833: nav.configure writes the project file; a restarted service (register again) loads it,
 * and the runtime loads the same file. */
TEST_F(editor_nav, settings_persist, nav_fx) {
    char dir[] = "/tmp/de_ed_nav_XXXXXX";
    ASSERT_TRUE(mkdtemp(dir) != NULL);
    ASSERT_EQ(de_fs_mount("ednav", dir, DE_MOUNT_WRITABLE, 0), DE_OK);
    /* The fixture registered before the path was known: the next tick loads it (no file yet). */
    de__ed_nav_set_settings_path("ednav:/nav.toml");
    frame(f);

    double mn[3] = {-32, -16, -3}, mx[3] = {32, 48, 9};
    de_ep_nav_agent ag[2];
    memset(ag, 0, sizeof ag);
    ag[0] = (de_ep_nav_agent){.name = de_ep_cstr("tank"), .radius = 1.0f, .height = 2.0f,
                              .max_slope_deg = 30.0f, .step_height = 0.4f, .cell_size = 0.5f,
                              .cell_height = 0.25f, .collision_mask = 6};
    ag[1] = (de_ep_nav_agent){.name = de_ep_cstr("humanoid"), .radius = 0.4f, .height = 1.8f,
                              .max_slope_deg = 45.0f, .step_height = 0.3f, .cell_size = 0.5f,
                              .cell_height = 0.2f};
    de_ep_nav_area ar[2] = {{.id = 0, .name = de_ep_cstr("ground"), .cost = 1.0f},
                            {.id = 3, .name = de_ep_cstr("mud"), .cost = 4.0f}};
    de_ep_nav_configure_params q;
    memset(&q, 0, sizeof q);
    q.has_agents = true;
    q.agents = (de_ep_list_nav_agent){ag, 2};
    q.has_areas = true;
    q.areas = (de_ep_list_nav_area){ar, 2};
    q.has_bounds = true;
    q.bounds.min = (de_ep_list_f64){mn, 3};
    q.bounds.max = (de_ep_list_f64){mx, 3};
    q.has_tile_size = true;
    q.tile_size = 16.0f;
    q.has_auto_rebake = true;
    q.auto_rebake = false;
    result_of(call(f, DE_EP_METHOD_NAV_CONFIGURE, &q));

    /* The runtime reads the file the editor wrote. */
    de_nav_settings rt;
    ASSERT_EQ(de_nav_settings_load("ednav:/nav.toml", &rt), DE_OK);
    ASSERT_EQ(rt.agent_count, 2u);
    EXPECT_EQ(strcmp(rt.agents[0].name, "tank"), 0);
    EXPECT_EQ(rt.agents[0].collision_mask, 6u);
    EXPECT_EQ(rt.area_count, 2u);
    EXPECT_EQ(rt.areas[1].id, 3u);
    EXPECT_TRUE(rt.tile_size == 16.0f);
    EXPECT_FALSE(rt.auto_rebake);
    EXPECT_TRUE(rt.bounds_max[1] == 48.0);

    /* A service restart: register again and read nav.info. */
    for (u32 i = 0; i < f->editor.reset_count; ++i)
        f->editor.resets[i](&f->editor);
    de__ed_nav_shutdown(&f->editor);
    de__ed_nav_register(&f->editor);
    mpack_node_t r = result_of(call(f, DE_EP_METHOD_NAV_INFO, NULL));
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(r, "tilesX")), 4u);
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(r, "tilesY")), 4u);
    EXPECT_FALSE(mpack_node_bool(mpack_node_map_cstr(r, "autoRebake")));
    EXPECT_EQ(mpack_node_array_length(mpack_node_map_cstr(r, "agents")), 2u);
    EXPECT_EQ(mpack_node_array_length(mpack_node_map_cstr(r, "areas")), 2u);
    de__ed_nav_set_settings_path(NULL);
    ASSERT_EQ(de_file_delete("ednav:/nav.toml"), DE_OK);
}

/* ---------- DENG-863: nav.testPath2d, NavInfo.plane2d, NavOverlayParams.grids ---------- */

/* A DeNavGrid2D entity at (x, y, z): `w` x `h` cells of `cs` m, area 0 everywhere except the
 * `blocked` cells (given as x, y pairs). */
static de_entity add_grid(nav_fx *f, f64 x, f64 y, f64 z, u32 w, u32 h, f32 cs, const u32 *blocked,
                          u32 nblocked) {
    de_world *world = f->engine.world;
    const de_builtin_component_ids b = de_builtin_components(world);
    const de_entity e = de_entity_spawn(world);
    de_transform *t = (de_transform *)de_entity_add(world, e, b.transform);
    ASSERT_TRUE(t != NULL);
    *t = de_transform_identity();
    t->position.x = x, t->position.y = y, t->position.z = z;
    const de_component gc = de_component_find(world, "DeNavGrid2D");
    DeNavGrid2D *g = (DeNavGrid2D *)de_entity_add(world, e, gc);
    ASSERT_TRUE(g != NULL);
    memset(g, 0, sizeof *g);
    g->width = w, g->height = h, g->cell_size = cs;
    for (u32 i = 0; i < nblocked; ++i)
        g->cells[blocked[i * 2 + 1] * w + blocked[i * 2]] = DE_NAV_GRID2D_BLOCKED;
    return e;
}

typedef struct path2d_rec {
    bool ok;
    de_ep_error code;
    u32 points;
    f64 first[3], last[3], length;
    u64 revision;
} path2d_rec;
static path2d_rec test_path2d(nav_fx *f, u64 grid, const double *from, u32 nfrom, const double *to,
                              bool has_jps, bool jps) {
    de_ep_nav_test_path2d_params q;
    memset(&q, 0, sizeof q);
    q.grid = grid;
    q.from = (de_ep_list_f64){(double *)from, nfrom};
    q.to = (de_ep_list_f64){(double *)to, 3};
    q.has_jps = has_jps;
    q.jps = jps;
    mpack_writer_t w;
    mpack_writer_init(&w, f->params, PARAMS);
    de_ep_write_nav_test_path2d_params(&w, &q);
    const size_t psize = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    const de_ep_request req = {.id = 7, .method = de_ep_cstr("nav"), .params = {f->params, psize}};
    ASSERT_TRUE(f->editor.handlers[DE_EP_METHOD_NAV_TEST_PATH2D] != NULL);
    de_ep_arena a = {f->storage, STORAGE, 0};
    mpack_writer_init(&w, f->response, RESPONSE);
    f->editor.handlers[DE_EP_METHOD_NAV_TEST_PATH2D](&f->editor, &req, &a, &w);
    const size_t size = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    if (f->parsed)
        mpack_tree_destroy(&f->tree);
    mpack_tree_init_data(&f->tree, f->response, size);
    mpack_tree_parse(&f->tree);
    f->parsed = true;
    ASSERT_EQ(mpack_tree_error(&f->tree), mpack_ok);
    mpack_node_t root = mpack_tree_root(&f->tree);
    path2d_rec p;
    memset(&p, 0, sizeof p);
    if (mpack_node_map_contains_cstr(root, "error")) {
        p.code = (de_ep_error)mpack_node_i32(
            mpack_node_map_cstr(mpack_node_map_cstr(root, "error"), "code"));
        return p;
    }
    mpack_node_t r = mpack_node_map_cstr(root, "result");
    p.ok = true;
    mpack_node_t pts = mpack_node_map_cstr(r, "points");
    p.points = (u32)mpack_node_array_length(pts) / 3;
    for (u32 k = 0; p.points > 0 && k < 3; ++k) {
        p.first[k] = mpack_node_double(mpack_node_array_at(pts, k));
        p.last[k] = mpack_node_double(mpack_node_array_at(pts, (p.points - 1) * 3 + k));
    }
    p.length = mpack_node_double(mpack_node_map_cstr(r, "length"));
    p.revision = mpack_node_u64(mpack_node_map_cstr(r, "revision"));
    return p;
}

TEST_F(editor_nav, plane2d_in_info, nav_fx) {
    mpack_node_t r = result_of(call(f, DE_EP_METHOD_NAV_INFO, NULL));
    EXPECT_TRUE(is_str(mpack_node_map_cstr(r, "plane2d"), "side_xz"));
    mpack_node_t ov = mpack_node_map_cstr(r, "overlay");
    EXPECT_FALSE(mpack_node_bool(mpack_node_map_cstr(ov, "grids")));
    /* A game that attached the system on the top-down plane first: the service reports it. */
    de_nav2d_grid_detach(f->engine.world);
    ASSERT_EQ(de_nav2d_grid_attach(f->engine.world, &(de_nav2d_grid_system_desc){.plane = DE_PLANE_TOP_XY}),
              DE_OK);
    r = result_of(call(f, DE_EP_METHOD_NAV_INFO, NULL));
    EXPECT_TRUE(is_str(mpack_node_map_cstr(r, "plane2d"), "top_xy"));
}

TEST_F(editor_nav, test_path2d_found, nav_fx) {
    /* 5 x 3 cells of 1 m at world (10, 7, 20); the plane is side_xz, so cell (cx, cy) has its
     * centre at (10.5 + cx, 7, 20.5 + cy). Column 2 is blocked at rows 0 and 1. */
    const u32 blocked[] = {2, 0, 2, 1};
    const de_entity e = add_grid(f, 10, 7, 20, 5, 3, 1.0f, blocked, 2);
    const double from[3] = {10.5, 99, 20.5}, to[3] = {14.5, -3, 20.5};
    path2d_rec p = test_path2d(f, e.bits, from, 3, to, false, false); /* jps absent: true */
    ASSERT_TRUE(p.ok);
    EXPECT_GE(p.points, 3u);
    EXPECT_NEAR(p.first[0], 10.5, 1e-6);
    EXPECT_NEAR(p.first[1], 7.0, 1e-6); /* the grid entity's off-plane coordinate */
    EXPECT_NEAR(p.first[2], 20.5, 1e-6);
    EXPECT_NEAR(p.last[0], 14.5, 1e-6);
    EXPECT_NEAR(p.last[1], 7.0, 1e-6);
    EXPECT_NEAR(p.last[2], 20.5, 1e-6);
    EXPECT_TRUE(p.length > 4.0); /* it goes around the wall */
    EXPECT_GT(p.revision, 0u);
    const f64 jps_length = p.length;
    p = test_path2d(f, e.bits, from, 3, to, true, false);
    ASSERT_TRUE(p.ok);
    EXPECT_NEAR(p.length, jps_length, 1e-4); /* A* and JPS agree on the cost */
    p = test_path2d(f, e.bits, from, 3, to, true, true);
    ASSERT_TRUE(p.ok);
    EXPECT_NEAR(p.length, jps_length, 1e-4);
    /* The same cell: one point, no length. */
    p = test_path2d(f, e.bits, from, 3, from, false, false);
    ASSERT_TRUE(p.ok);
    EXPECT_EQ(p.points, 1u);
    EXPECT_NEAR(p.length, 0.0, 1e-9);
}

TEST_F(editor_nav, test_path2d_errors, nav_fx) {
    const u32 blocked[] = {2, 0, 2, 1};
    const de_entity e = add_grid(f, 10, 7, 20, 5, 3, 1.0f, blocked, 2);
    const u32 wall[] = {2, 0, 2, 1, 2, 2};
    const de_entity split = add_grid(f, 0, 0, 0, 5, 3, 1.0f, wall, 3);
    const double in_a[3] = {10.5, 0, 20.5}, in_b[3] = {14.5, 0, 20.5};
    /* not_found: an end outside the grid. */
    const double outside[3] = {0.0, 0, 0.0};
    path2d_rec p = test_path2d(f, e.bits, outside, 3, in_b, false, false);
    EXPECT_FALSE(p.ok);
    EXPECT_EQ(p.code, DE_EP_ERR_NOT_FOUND);
    p = test_path2d(f, e.bits, in_a, 3, outside, false, false);
    EXPECT_EQ(p.code, DE_EP_ERR_NOT_FOUND);
    /* not_found: an end on a blocked cell (cell (2, 0)). */
    const double on_wall[3] = {12.5, 0, 20.5};
    p = test_path2d(f, e.bits, on_wall, 3, in_b, false, false);
    EXPECT_EQ(p.code, DE_EP_ERR_NOT_FOUND);
    p = test_path2d(f, e.bits, in_a, 3, on_wall, false, false);
    EXPECT_EQ(p.code, DE_EP_ERR_NOT_FOUND);
    /* not_found: the ends are not connected (a full wall in column 2). */
    const double s_a[3] = {0.5, 0, 0.5}, s_b[3] = {4.5, 0, 0.5};
    p = test_path2d(f, split.bits, s_a, 3, s_b, true, true);
    EXPECT_FALSE(p.ok);
    EXPECT_EQ(p.code, DE_EP_ERR_NOT_FOUND);
    p = test_path2d(f, split.bits, s_a, 3, s_b, true, false);
    EXPECT_EQ(p.code, DE_EP_ERR_NOT_FOUND);
    /* invalid_params: an entity with no grid, a dead entity, and a grid that is not valid. */
    const de_entity plain = de_entity_spawn(f->engine.world);
    p = test_path2d(f, plain.bits, in_a, 3, in_b, false, false);
    EXPECT_EQ(p.code, DE_EP_ERR_INVALID_PARAMS);
    p = test_path2d(f, 0xdeadbeefull, in_a, 3, in_b, false, false);
    EXPECT_EQ(p.code, DE_EP_ERR_INVALID_PARAMS);
    const de_entity bad = add_grid(f, 0, 0, 0, 0, 3, 1.0f, NULL, 0); /* width 0 */
    p = test_path2d(f, bad.bits, in_a, 3, in_b, false, false);
    EXPECT_EQ(p.code, DE_EP_ERR_INVALID_PARAMS);
    /* invalid_params: a bad point (2 values, or not finite). */
    p = test_path2d(f, e.bits, in_a, 2, in_b, false, false);
    EXPECT_EQ(p.code, DE_EP_ERR_INVALID_PARAMS);
    const double nan3[3] = {NAN, 0, 20.5};
    p = test_path2d(f, e.bits, in_a, 3, nan3, false, false);
    EXPECT_EQ(p.code, DE_EP_ERR_INVALID_PARAMS);
}

TEST_F(editor_nav, overlay_grids, nav_fx) {
    const u32 blocked[] = {1, 1};
    add_grid(f, 0, 0, 0, 3, 2, 2.0f, blocked, 1);
    de_draw_line_item items[1024];
    /* Off by default: nothing is drawn. */
    frame(f);
    EXPECT_EQ(de_draw_update(0.0f, items, 1024), 0u);
    /* grids on, with no agent type: the cells are drawn in the navigation category. */
    de_ep_nav_overlay_params oq;
    memset(&oq, 0, sizeof oq);
    oq.has_grids = true;
    oq.grids = true;
    result_of(call(f, DE_EP_METHOD_NAV_OVERLAY, &oq));
    frame(f);
    const u32 on = de_draw_update(0.0f, items, 1024);
    EXPECT_GE(on, 6u); /* at least one line per cell */
    mpack_node_t r = result_of(call(f, DE_EP_METHOD_NAV_INFO, NULL));
    EXPECT_TRUE(mpack_node_bool(mpack_node_map_cstr(mpack_node_map_cstr(r, "overlay"), "grids")));
    /* Blocked cells are drawn dark, walkable ones are not. */
    u32 dark = 0, bright = 0;
    for (u32 i = 0; i < on; ++i) {
        const bool d = items[i].color.x < 0.2f && items[i].color.y < 0.2f;
        dark += d, bright += !d;
    }
    EXPECT_GT(dark, 0u);
    EXPECT_GT(bright, 0u);
    /* Switched off again. */
    oq.grids = false;
    result_of(call(f, DE_EP_METHOD_NAV_OVERLAY, &oq));
    frame(f);
    EXPECT_EQ(de_draw_update(0.0f, items, 1024), 0u);
}

#endif /* !DE_SHIPPING */
