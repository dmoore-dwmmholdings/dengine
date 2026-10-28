/*
 * test_tilemap_runtime.c: the tilemap runtime (DENG-80; src/gameplay/tilemap/). The fixtures of
 * tests/data/tilemap/ are cooked in-process (SOURCE mode), so every test that loads a map skips
 * in builds without the cooker. CTest names tilemap.<test>.
 *
 *   tilemap_set_tile_single_chunk  an edit rebuilds the sprites and colliders of its own chunk
 *                                  only; other chunks keep their entities
 *   tilemap_collider_merge         greedy boxes cover exactly the solid cells (random grids and
 *                                  the fixture), tile shapes are polygons and chains of <= 8
 *                                  points, and 2D physics creates a body for every collider
 *   tilemap_entity_spawn           mapped prefabs spawn once at their plane position with the
 *                                  map's field values (Tiled and LDtk)
 *   tilemap_pick_and_palette       world point -> cell, cell corners, layers, tiles, tilesets
 *   tilemap_animation              animated tiles patch their sprite UVs over time
 *   tilemap_flip_rotation          Tiled flip bits -> quarter turns + x-flip
 *   tilemap_top_down_plane         the top-down plane maps (u, v) to (X, Y)
 */
#include "tilemap_test_util.h"

#include "gameplay/tilemap/tilemap_internal.h"

#include <dengine/de_asset.h>
#include <dengine/de_ecs.h>
#include <dengine/de_gameplay.h>
#include <dengine/de_physics.h>
#include <dengine/de_reflect.h>
#include <dengine/de_render.h>
#include <dengine/de_task.h>

#include <math.h>
#include <stdlib.h>

#ifndef DE_HAS_COOKER
#define DE_HAS_COOKER 0
#endif
#ifndef DE_HAS_RENDER
#define DE_HAS_RENDER 0
#endif
#ifndef DE_HAS_PHYSICS
#define DE_HAS_PHYSICS 0
#endif

#define WAIT_NS ((u64)60 * 1000000000u)
#define NO_COOKER "this build does not link the cooker (DE_HAS_COOKER: Debug and Dev with tools)"

typedef struct TmHealth {
    f32 current, max;
    i32 lives;
    char tag[16];
} TmHealth;

static const de_field_info k_health_fields[] = {
    DE_FIELD(TmHealth, current, DE_FK_F32, DE_FIELD_DEFAULT),
    DE_FIELD(TmHealth, max, DE_FK_F32, DE_FIELD_DEFAULT),
    DE_FIELD(TmHealth, lives, DE_FK_I32, DE_FIELD_DEFAULT),
    DE_FIELD(TmHealth, tag, DE_FK_STRING, DE_FIELD_DEFAULT),
};

typedef struct rt_fx {
    de_type_registry *reg;
    de_task_system *tasks;
    de_asset_system *s;
    de_world *w;
    de_component health;
    de_physics2d_world *p2d;
    de_builtin_component_ids b;
} rt_fx;

static de_world *make_world(rt_fx *f, const de_tilemap_desc *desc) {
    de_world *w = NULL;
    ASSERT_EQ(de_world_create(&(de_world_desc){.types = f->reg, .tasks = f->tasks}, &w), DE_OK);
    f->b = de_builtin_components(w);
    ASSERT_TRUE(DE_HANDLE_VALID(f->b.transform));
    f->health = de_component_register(w, de_type_find(f->reg, "TmHealth"), DE_COMP_SERIALIZE);
    ASSERT_TRUE(DE_HANDLE_VALID(f->health));
#if DE_HAS_RENDER
    ASSERT_EQ(de_render_register_components(w), DE_OK);
#endif
    ASSERT_EQ(de_tilemap_attach(w, f->s, desc), DE_OK);
    return w;
}

static void rt_fx_setup(rt_fx *f) {
    de_log_desc ld;
    memset(&ld, 0, sizeof ld);
    ld.no_console_sink = true;
    ASSERT_EQ(de_log_init(&ld), DE_OK);
    tm_mount();
    de_fp_env_init_thread();
    ASSERT_EQ(de_type_registry_create(NULL, &f->reg), DE_OK);
    ASSERT_TRUE(DE_HANDLE_VALID(
        de_type_register(f->reg, &DE_TYPE_DESC(TmHealth, 1, k_health_fields))));
    ASSERT_EQ(de_task_system_create(&(de_task_system_desc){.worker_count = 2}, &f->tasks), DE_OK);
    ASSERT_EQ(de_asset_system_create(&(de_asset_system_desc){.tasks = f->tasks,
                                                             .io_threads = 1,
                                                             .cache_dir = "nocache:/decache",
                                                             .types = f->reg},
                                     &f->s),
              DE_OK);
    f->w = make_world(f, NULL);
#if DE_HAS_PHYSICS
    ASSERT_EQ(de_physics2d_create(NULL, &f->p2d), DE_OK);
    de_physics2d_attach(f->p2d, f->w);
#endif
    ASSERT_EQ(de_asset_manifest_load(f->s, "tmd:/assets.toml"), DE_OK);
}

static void rt_fx_teardown(rt_fx *f) {
    if (f->w != NULL)
        de_world_destroy(f->w);
#if DE_HAS_PHYSICS
    if (f->p2d != NULL)
        de_physics2d_destroy(f->p2d);
#endif
    de_asset_system_destroy(f->s);
    de_task_system_destroy(f->tasks);
    de_type_registry_destroy(f->reg);
    de_log_shutdown();
}

#if DE_HAS_COOKER
static de_tilemap load_map(rt_fx *f, const char *id) {
    de_load_handle h = de_asset_load(f->s, de_asset_id_from(id), 0);
    de_load_state st = de_asset_wait(f->s, h, WAIT_NS);
    if (st != DE_LOAD_READY) {
        de_load_info info;
        (void)de_asset_status(f->s, h, &info);
        printf("  %s: %s\n", id, info.first_error_msg);
    }
    ASSERT_EQ((int)st, (int)DE_LOAD_READY);
    de_tilemap m = de_asset_tilemap(f->s, de_asset_id_from(id));
    ASSERT_TRUE(DE_HANDLE_VALID(m));
    return m;
}

static de_entity spawn_map(rt_fx *f, de_world *w, de_tilemap m, de_dvec3 pos, bool colliders,
                           bool spawns) {
    de_entity e = de_entity_spawn(w);
    de_transform *t = (de_transform *)de_entity_add(w, e, f->b.transform);
    ASSERT_TRUE(t != NULL);
    *t = de_transform_identity();
    t->position = pos;
    de__tilemap_sys *s = de__tilemap_sys_of(w);
    ASSERT_TRUE(s != NULL);
    DeTilemap *c = (DeTilemap *)de_entity_add(w, e, s->comp);
    ASSERT_TRUE(c != NULL);
    memset(c, 0, sizeof *c);
    c->map = m;
    c->generate_colliders = colliders;
    c->spawn_entities = spawns;
    c->lit = true;
    return e;
}

static u32 chunk_index(const de__tm_instance *in, u32 layer, u32 cx, u32 cy) {
    de__tm_layer l = de__tm_layer_at(&in->asset->view, layer);
    return l.chunk_first + cy * l.chunks_x + cx;
}
#endif

/* ---- tilemap_set_tile_single_chunk ------------------------------------------------------------ */

TEST_F(tilemap, tilemap_set_tile_single_chunk, rt_fx) {
#if !DE_HAS_COOKER
    SKIP(NO_COOKER);
#else
    de_tilemap m = load_map(f, "maps/fixture_tmj");
    de_entity e = spawn_map(f, f->w, m, (de_dvec3){0, 0, 0}, true, false);
    de_tilemap_flush(f->w);
    de__tilemap_sys *s = de__tilemap_sys_of(f->w);
    de__tm_instance *in = de__tilemap_instance(s, e);
    ASSERT_TRUE(in != NULL);
    ASSERT_TRUE(in->built);
    const u32 n = in->asset->view.h.chunk_count;
    ASSERT_EQ(n, 12u);
    for (u32 i = 0; i < n; ++i) {
        EXPECT_EQ(in->chunks[i].mesh_builds, 1u);
        EXPECT_EQ(in->chunks[i].collider_builds, 1u);
    }
    /* Snapshot every chunk's sprite and collider entities. */
    static de_entity sprites[12][DE__TM_CHUNK_CELLS];
    static de_entity colliders[12][256];
    u32 collider_counts[12];
    for (u32 i = 0; i < n; ++i) {
        const de__tm_chunk *c = &in->chunks[i];
        for (u32 k = 0; k < DE__TM_CHUNK_CELLS; ++k)
            sprites[i][k] = c->sprites != NULL ? c->sprites[k] : DE_ENTITY_NULL;
        collider_counts[i] = c->collider_count;
        ASSERT_LE(c->collider_count, 256u);
        for (u32 k = 0; k < c->collider_count; ++k)
            colliders[i][k] = c->colliders[k];
    }
    const u32 target = chunk_index(in, 1, 0, 1); /* ground, cells 0..31 x 32..39 */
#if DE_HAS_RENDER
    EXPECT_TRUE(DE_HANDLE_VALID(s->sprite));
    EXPECT_TRUE(sprites[target][(35 - 32) * DE__TM_CHUNK + 3].bits != 0);
#endif
    /* Dirt (tile 4) at (3, 35) becomes a brick (tile 6). */
    EXPECT_EQ(de_tilemap_get_cell(f->w, e, 1, 3, 35), 5u);
    de_tilemap_set_tile(f->w, e, 1, 3, 35, 7u);
    EXPECT_EQ(de_tilemap_get_cell(f->w, e, 1, 3, 35), 7u);
    EXPECT_EQ(de_tilemap_get_tile(f->s, m, 1, 3, 35), 5u); /* the asset is untouched */
    EXPECT_EQ(in->chunks[target].mesh_builds, 1u);         /* rebuilt on the next update */
    /* The system path: PostUpdate runs `tilemap.update`. */
    ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_POST_UPDATE, 0.0f, 0.0f), DE_OK);
    in = de__tilemap_instance(s, e);
    for (u32 i = 0; i < n; ++i) {
        const de__tm_chunk *c = &in->chunks[i];
        const u32 want = i == target ? 2u : 1u;
        EXPECT_EQ(c->mesh_builds, want);
        EXPECT_EQ(c->collider_builds, want);
        if (i == target)
            continue;
        for (u32 k = 0; k < DE__TM_CHUNK_CELLS; ++k) {
            de_entity now = c->sprites != NULL ? c->sprites[k] : DE_ENTITY_NULL;
            if (now.bits != sprites[i][k].bits) {
                EXPECT_EQ(now.bits, sprites[i][k].bits);
                break;
            }
        }
        ASSERT_EQ(c->collider_count, collider_counts[i]);
        for (u32 k = 0; k < c->collider_count; ++k) {
            EXPECT_EQ(c->colliders[k].bits, colliders[i][k].bits);
            EXPECT_TRUE(de_entity_alive(f->w, c->colliders[k]));
        }
    }
#if DE_HAS_RENDER
    /* In the edited chunk every cell keeps its entity; the edited one shows the brick. */
    const de__tm_chunk *tc = &in->chunks[target];
    for (u32 k = 0; k < DE__TM_CHUNK_CELLS; ++k)
        EXPECT_EQ(tc->sprites[k].bits, sprites[target][k].bits);
    const DeSprite *sp = (const DeSprite *)de_entity_get_const(
        f->w, tc->sprites[(35 - 32) * DE__TM_CHUNK + 3], s->sprite);
    ASSERT_TRUE(sp != NULL);
    EXPECT_FLOAT_EQ(sp->uv_rect.x, 0.5, 0.0); /* tile 6: column 2, row 1 */
    EXPECT_FLOAT_EQ(sp->uv_rect.y, 0.5, 0.0);
    /* Clearing the cell despawns its sprite (flush: the editor path). */
    de_entity old = tc->sprites[(35 - 32) * DE__TM_CHUNK + 3];
    de_tilemap_set_tile(f->w, e, 1, 3, 35, 0);
    de_tilemap_flush(f->w);
    EXPECT_FALSE(de_entity_alive(f->w, old));
    EXPECT_EQ(in->chunks[target].mesh_builds, 3u);
    EXPECT_EQ(in->chunks[chunk_index(in, 1, 1, 1)].mesh_builds, 1u);
#endif
    /* Out of range and unknown tiles change nothing. */
    de_tilemap_set_tile(f->w, e, 1, 99, 0, 1u);
    de_tilemap_set_tile(f->w, e, 9, 0, 0, 1u);
    de_tilemap_set_tile(f->w, e, 1, 0, 0, 999u);
    de_tilemap_flush(f->w);
    for (u32 i = 0; i < n; ++i)
        if (i != target)
            EXPECT_EQ(in->chunks[i].mesh_builds, 1u);
#endif
}

/* ---- tilemap_collider_merge ------------------------------------------------------------------- */

static u32 lcg(u32 *s) {
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

TEST_F(tilemap, tilemap_collider_merge, rt_fx) {
    /* Random grids: the rectangles tile the solid cells exactly. */
    u32 seed = 12345u;
    static u8 solid[DE__TM_CHUNK_CELLS];
    static de__tm_rect rects[DE__TM_CHUNK_CELLS];
    for (u32 iter = 0; iter < 200; ++iter) {
        const u32 density = iter % 10u;
        u32 count = 0;
        for (u32 k = 0; k < DE__TM_CHUNK_CELLS; ++k) {
            solid[k] = (lcg(&seed) % 10u) < density ? 1u : 0u;
            count += solid[k];
        }
        u32 n = de__tm_merge_rects(solid, rects);
        u8 cover[DE__TM_CHUNK_CELLS];
        memset(cover, 0, sizeof cover);
        u32 area = 0;
        bool ok = true;
        for (u32 i = 0; i < n; ++i) {
            area += rects[i].w * rects[i].h;
            for (u32 y = rects[i].y; y < rects[i].y + rects[i].h; ++y)
                for (u32 x = rects[i].x; x < rects[i].x + rects[i].w; ++x) {
                    ok &= x < DE__TM_CHUNK && y < DE__TM_CHUNK;
                    if (x < DE__TM_CHUNK && y < DE__TM_CHUNK) {
                        ok &= solid[y * DE__TM_CHUNK + x] == 1u;
                        cover[y * DE__TM_CHUNK + x]++;
                    }
                }
        }
        for (u32 k = 0; k < DE__TM_CHUNK_CELLS; ++k)
            ok &= cover[k] == solid[k];
        EXPECT_TRUE(ok);
        EXPECT_EQ(area, count);
    }
    /* A full chunk is one box. */
    memset(solid, 1, sizeof solid);
    ASSERT_EQ(de__tm_merge_rects(solid, rects), 1u);
    EXPECT_EQ(rects[0].w * rects[0].h, DE__TM_CHUNK_CELLS);
#if !DE_HAS_COOKER
    SKIP(NO_COOKER);
#else
    /* The fixture: per chunk, the box colliders cover exactly its solid cells. */
    de_tilemap m = load_map(f, "maps/fixture_tmj");
    const de_dvec3 origin = {5, 0, 3};
    de_entity e = spawn_map(f, f->w, m, origin, true, false);
    de_tilemap_flush(f->w);
    de__tilemap_sys *s = de__tilemap_sys_of(f->w);
    de__tm_instance *in = de__tilemap_instance(s, e);
    ASSERT_TRUE(in != NULL);
    ASSERT_TRUE(DE_HANDLE_VALID(s->collider));
    const de__tm_view *v = &in->asset->view;
    u32 boxes = 0, polygons = 0, chains = 0, total = 0;
    for (u32 i = 0; i < v->h.chunk_count; ++i) {
        const de__tm_chunk *c = &in->chunks[i];
        const de__tm_layer l = de__tm_layer_at(v, c->layer);
        u8 cover[DE__TM_CHUNK_CELLS];
        memset(cover, 0, sizeof cover);
        for (u32 k = 0; k < c->collider_count; ++k) {
            const DeCollider2D *col =
                (const DeCollider2D *)de_entity_get_const(f->w, c->colliders[k], s->collider);
            const de_transform *t =
                (const de_transform *)de_entity_get_const(f->w, c->colliders[k], f->b.transform);
            ASSERT_TRUE(col != NULL && t != NULL);
            ++total;
            if (col->kind != DE_SHAPE2D_BOX) {
                EXPECT_LE(col->point_count, 8u);
                EXPECT_GE(col->point_count, 2u);
                if (col->kind == DE_SHAPE2D_POLYGON)
                    ++polygons;
                else if (col->kind == DE_SHAPE2D_CHAIN)
                    ++chains;
#if DE_HAS_PHYSICS
                if (col->kind == DE_SHAPE2D_CHAIN)
                    EXPECT_TRUE(de_shape2d_valid(f->p2d, col->shape));
#endif
                continue;
            }
            ++boxes;
            /* Side view: X = u, Z = v. Back to cells of this chunk. */
            const f64 u0 = t->position.x - origin.x - col->half_extents.x;
            const f64 u1 = t->position.x - origin.x + col->half_extents.x;
            const f64 v1 = t->position.z - origin.z + col->half_extents.y;
            const f64 v0 = t->position.z - origin.z - col->half_extents.y;
            const i32 x0 = (i32)floor((u0 - l.origin_u) / l.cell_w + 0.5);
            const i32 x1 = (i32)floor((u1 - l.origin_u) / l.cell_w + 0.5);
            const i32 y0 = (i32)floor((l.origin_v - v1) / l.cell_h + 0.5);
            const i32 y1 = (i32)floor((l.origin_v - v0) / l.cell_h + 0.5);
            for (i32 y = y0; y < y1; ++y)
                for (i32 x = x0; x < x1; ++x) {
                    const i32 lx = x - (i32)(c->cx * DE__TM_CHUNK);
                    const i32 ly = y - (i32)(c->cy * DE__TM_CHUNK);
                    ASSERT_TRUE(lx >= 0 && ly >= 0 && lx < 32 && ly < 32);
                    cover[ly * 32 + lx]++;
                }
        }
        for (u32 k = 0; k < DE__TM_CHUNK_CELLS; ++k) {
            const u32 x = c->cx * DE__TM_CHUNK + k % 32, y = c->cy * DE__TM_CHUNK + k / 32;
            const u32 want = x < l.width && y < l.height && de__tm_cell_solid(v, &l, c->cells[k]);
            if (cover[k] != want) {
                printf("  layer %u cell (%u, %u): covered %u times, solid %u\n", c->layer, x, y,
                       cover[k], want);
                EXPECT_EQ((u32)cover[k], want);
                break;
            }
        }
    }
    EXPECT_GT(boxes, 0u);
    EXPECT_EQ(polygons, 3u); /* the three slopes */
    EXPECT_EQ(chains, 10u);  /* five platforms, two chains each */
#if DE_HAS_PHYSICS
    /* The 2D binding accepts every collider: one static body each. */
    ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_FIXED_UPDATE, 1.0f / 60.0f, 0.0f), DE_OK);
    EXPECT_EQ(de_body2d_count(f->p2d), total);
#endif
#endif
}

/* ---- tilemap_entity_spawn ---------------------------------------------------------------------- */

#if DE_HAS_COOKER
typedef struct found {
    de_entity e[8];
    u32 n;
} found;
static void collect_health(const de_query_chunk *c, void *user) {
    found *fd = (found *)user;
    for (u32 i = 0; i < c->count && fd->n < 8; ++i)
        fd->e[fd->n++] = c->entities[i];
}
static found find_health(rt_fx *f) {
    found fd;
    memset(&fd, 0, sizeof fd);
    de_query_term term = {f->health, DE_READ};
    de_query q = de_query_create(f->w, &(de_query_desc){.terms = &term, .term_count = 1});
    de_query_each(f->w, q, collect_health, &fd);
    de_query_destroy(f->w, q);
    return fd;
}
#endif

TEST_F(tilemap, tilemap_entity_spawn, rt_fx) {
#if !DE_HAS_COOKER
    SKIP(NO_COOKER);
#else
    de_tilemap m = load_map(f, "maps/fixture_tmj");
    de_entity e = spawn_map(f, f->w, m, (de_dvec3){10, 0, 2}, false, true);
    de_tilemap_flush(f->w);
    found fd = find_health(f);
    ASSERT_EQ(fd.n, 1u); /* Player only: Coin is not mapped */
    const TmHealth *h = (const TmHealth *)de_entity_get_const(f->w, fd.e[0], f->health);
    ASSERT_TRUE(h != NULL);
    EXPECT_FLOAT_EQ(h->current, 50.0, 0.0);
    EXPECT_FLOAT_EQ(h->max, 120.0, 0.0);
    EXPECT_EQ(h->lives, 3);
    EXPECT_STREQ(h->tag, "hero");
    const de_transform *t =
        (const de_transform *)de_entity_get_const(f->w, fd.e[0], f->b.transform);
    ASSERT_TRUE(t != NULL);
    /* Map (3, -33.25), side view: X = u, Z = v, plus the map entity's position. */
    EXPECT_FLOAT_EQ(t->position.x, 13.0, 1e-9);
    EXPECT_FLOAT_EQ(t->position.y, 0.0, 1e-9);
    EXPECT_FLOAT_EQ(t->position.z, 2.0 - 33.25, 1e-9);
    EXPECT_EQ(de_entity_parent(f->w, fd.e[0]).bits, 0u);
    /* Spawns happen once per map instance. */
    de_tilemap_flush(f->w);
    ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_POST_UPDATE, 0.016f, 0.0f), DE_OK);
    EXPECT_EQ(find_health(f).n, 1u);
    (void)e;

    /* LDtk: the field set by the map wins, the others keep the prefab's values. */
    de_tilemap lm = load_map(f, "maps/fixture_ldtk");
    (void)spawn_map(f, f->w, lm, (de_dvec3){0, 0, 0}, false, true);
    de_tilemap_flush(f->w);
    fd = find_health(f);
    ASSERT_EQ(fd.n, 2u);
    const TmHealth *lh = NULL;
    de_entity le = DE_ENTITY_NULL;
    for (u32 i = 0; i < fd.n; ++i) {
        const TmHealth *x = (const TmHealth *)de_entity_get_const(f->w, fd.e[i], f->health);
        if (x != NULL && strcmp(x->tag, "ldtk") == 0)
            lh = x, le = fd.e[i];
    }
    ASSERT_TRUE(lh != NULL);
    EXPECT_FLOAT_EQ(lh->current, 75.0, 0.0);
    EXPECT_FLOAT_EQ(lh->max, 100.0, 0.0);
    EXPECT_EQ(lh->lives, 2);
    t = (const de_transform *)de_entity_get_const(f->w, le, f->b.transform);
    ASSERT_TRUE(t != NULL);
    EXPECT_FLOAT_EQ(t->position.x, 2.5, 1e-9);
    EXPECT_FLOAT_EQ(t->position.z, -14.0, 1e-9);
#endif
}

/* ---- tilemap_pick_and_palette ----------------------------------------------------------------- */

TEST_F(tilemap, tilemap_pick_and_palette, rt_fx) {
#if !DE_HAS_COOKER
    SKIP(NO_COOKER);
#else
    de_tilemap m = load_map(f, "maps/fixture_tmj");
    de_entity e = spawn_map(f, f->w, m, (de_dvec3){10, 0, 2}, false, false);
    /* The editor queries create the live map on demand (no flush needed). */
    ASSERT_EQ(de__tilemap_layer_count(f->w, e), 3u);
    de__tilemap_layer_info li;
    ASSERT_TRUE(de__tilemap_layer(f->w, e, 1, &li));
    EXPECT_STREQ(li.name, "ground");
    EXPECT_EQ(li.width, 48u);
    EXPECT_EQ(li.height, 40u);
    EXPECT_FLOAT_EQ(li.cell_w, 1.0, 0.0);
    EXPECT_FLOAT_EQ(li.origin.x, 0.0, 0.0);
    EXPECT_FLOAT_EQ(li.depth, -0.01, 1e-7);
    EXPECT_TRUE(li.visible);
    EXPECT_FALSE(de__tilemap_layer(f->w, e, 3, &li));
    /* Picking: the center of cell (3, 35) is map (3.5, -35.5); the depth axis is ignored. */
    i32 x = -1, y = -1;
    const de_dvec3 p = {10 + 3.5, 7.0, 2 - 35.5};
    EXPECT_TRUE(de__tilemap_pick(f->w, e, p, 1, &x, &y));
    EXPECT_EQ(x, 3);
    EXPECT_EQ(y, 35);
    EXPECT_FALSE(de__tilemap_pick(f->w, e, (de_dvec3){10 - 0.5, 0, 2}, 1, &x, &y));
    EXPECT_EQ(x, -1);
    u32 layer = 99;
    EXPECT_TRUE(de__tilemap_pick_top(f->w, e, p, &layer, &x, &y));
    EXPECT_EQ(layer, 1u); /* deco is empty there */
    EXPECT_TRUE(de__tilemap_pick_top(f->w, e, (de_dvec3){10 + 2.5, 0, 2 - 33.5}, &layer, &x, &y));
    EXPECT_EQ(layer, 2u); /* the flipped flower of `deco` at (2, 33) */
    EXPECT_EQ(x, 2);
    EXPECT_EQ(y, 33);
    de_dvec3 corners[4];
    ASSERT_TRUE(de__tilemap_cell_corners(f->w, e, 1, 3, 35, corners));
    EXPECT_FLOAT_EQ(corners[0].x, 13.0, 1e-6);
    EXPECT_FLOAT_EQ(corners[0].z, 2.0 - 36.0, 1e-6);
    EXPECT_FLOAT_EQ(corners[2].x, 14.0, 1e-6);
    EXPECT_FLOAT_EQ(corners[2].z, 2.0 - 35.0, 1e-6);
    EXPECT_FLOAT_EQ(corners[0].y, -0.01, 1e-6);
    /* The palette. */
    ASSERT_EQ(de__tilemap_tile_count(f->w, e), 8u);
    ASSERT_EQ(de__tilemap_tileset_count(f->w, e), 1u);
    de__tilemap_tileset_info tsi;
    ASSERT_TRUE(de__tilemap_tileset(f->w, e, 0, &tsi));
    EXPECT_STREQ(tsi.name, "terrain");
    EXPECT_EQ(tsi.texture.hash, de_asset_id_from("tiles/terrain").hash);
    EXPECT_EQ(tsi.columns, 4u);
    EXPECT_EQ(tsi.tile_count, 8u);
    de__tilemap_tile_info ti;
    ASSERT_TRUE(de__tilemap_tile(f->w, e, 5, &ti));
    EXPECT_FLOAT_EQ(ti.uv_rect.x, 0.25, 0.0);
    EXPECT_FLOAT_EQ(ti.uv_rect.y, 0.5, 0.0);
    EXPECT_FLOAT_EQ(ti.uv_rect.z, 0.25, 0.0);
    EXPECT_FLOAT_EQ(ti.size.x, 1.0, 0.0);
    EXPECT_EQ(ti.texture.hash, de_asset_id_from("tiles/terrain").hash);
    ASSERT_TRUE(de__tilemap_tile(f->w, e, 1, &ti));
    EXPECT_TRUE(ti.animated);
    ASSERT_TRUE(de__tilemap_tile(f->w, e, 6, &ti));
    EXPECT_TRUE(ti.solid);
    ASSERT_TRUE(de__tilemap_tile(f->w, e, 7, &ti));
    EXPECT_EQ(ti.shape_count, 2u);
    EXPECT_FALSE(de__tilemap_tile(f->w, e, 8, &ti));
    /* Painting through the live map. */
    de_tilemap_set_tile(f->w, e, 2, 4, 4, 6u | DE_TILEMAP_FLIP_H);
    EXPECT_EQ(de_tilemap_get_cell(f->w, e, 2, 4, 4), 6u | DE_TILEMAP_FLIP_H);
    de_tilemap_flush(f->w);
    EXPECT_EQ(de_tilemap_get_cell(f->w, e, 2, 4, 4), 6u | DE_TILEMAP_FLIP_H);
#endif
}

/* ---- tilemap_animation ------------------------------------------------------------------------ */

TEST_F(tilemap, tilemap_animation, rt_fx) {
#if !DE_HAS_COOKER
    SKIP(NO_COOKER);
#else
    de_tilemap m = load_map(f, "maps/fixture_tmj");
    de_entity e = spawn_map(f, f->w, m, (de_dvec3){0, 0, 0}, false, false);
    de_tilemap_flush(f->w);
    de__tilemap_sys *s = de__tilemap_sys_of(f->w);
    de__tm_instance *in = de__tilemap_instance(s, e);
    ASSERT_TRUE(in != NULL);
    const de__tm_view *v = &in->asset->view;
    EXPECT_EQ(de__tm_anim_tile(v, 1, 0.0), 1u);
    EXPECT_EQ(de__tm_anim_tile(v, 1, 199.0), 1u);
    EXPECT_EQ(de__tm_anim_tile(v, 1, 200.0), 2u);
    EXPECT_EQ(de__tm_anim_tile(v, 1, 499.0), 2u);
    EXPECT_EQ(de__tm_anim_tile(v, 1, 500.0), 1u);
    EXPECT_EQ(de__tm_anim_tile(v, 4, 300.0), 4u); /* not animated */
#if DE_HAS_RENDER
    const de__tm_chunk *c = &in->chunks[chunk_index(in, 1, 0, 1)];
    EXPECT_EQ(c->animated_count, 12u); /* the pool: 6 x 2 water cells */
    const de_entity water = c->sprites[(34 - 32) * DE__TM_CHUNK + 12];
    const DeSprite *sp = (const DeSprite *)de_entity_get_const(f->w, water, s->sprite);
    ASSERT_TRUE(sp != NULL);
    EXPECT_FLOAT_EQ(sp->uv_rect.x, 0.25, 0.0); /* tile 1 */
    ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_POST_UPDATE, 0.25f, 0.0f), DE_OK);
    sp = (const DeSprite *)de_entity_get_const(f->w, water, s->sprite);
    EXPECT_FLOAT_EQ(sp->uv_rect.x, 0.5, 0.0); /* tile 2 at 250 ms */
    ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_POST_UPDATE, 0.3f, 0.0f), DE_OK);
    sp = (const DeSprite *)de_entity_get_const(f->w, water, s->sprite);
    EXPECT_FLOAT_EQ(sp->uv_rect.x, 0.25, 0.0); /* 550 ms: tile 1 again */
    /* The animated cell in chunk (0, 0) of `deco` animates too. */
    EXPECT_EQ(in->chunks[chunk_index(in, 2, 0, 0)].animated_count, 1u);
#endif
#endif
}

/* ---- tilemap_flip_rotation ------------------------------------------------------------------- */

TEST_F(tilemap, tilemap_flip_rotation, rt_fx) {
    (void)f;
    static const struct {
        u32 bits, turns;
        bool flip;
    } k[] = {
        {0, 0, false},
        {DE_TILEMAP_FLIP_H, 0, true},
        {DE_TILEMAP_FLIP_V, 2, true},
        {DE_TILEMAP_FLIP_H | DE_TILEMAP_FLIP_V, 2, false},
        {DE_TILEMAP_FLIP_D, 1, true},
        {DE_TILEMAP_FLIP_D | DE_TILEMAP_FLIP_H, 3, false}, /* Tiled: 90 degrees clockwise */
        {DE_TILEMAP_FLIP_D | DE_TILEMAP_FLIP_V, 1, false}, /* 90 degrees counter-clockwise */
        {DE_TILEMAP_FLIP_D | DE_TILEMAP_FLIP_H | DE_TILEMAP_FLIP_V, 3, true},
    };
    for (u32 i = 0; i < sizeof k / sizeof k[0]; ++i) {
        u32 turns = 99;
        bool flip = false;
        de__tm_flip_to_rotation(k[i].bits | 5u, &turns, &flip);
        EXPECT_EQ(turns, k[i].turns);
        EXPECT_EQ(flip, k[i].flip);
    }
}

/* ---- tilemap_top_down_plane --------------------------------------------------------------------- */

TEST_F(tilemap, tilemap_top_down_plane, rt_fx) {
#if !DE_HAS_COOKER
    SKIP(NO_COOKER);
#else
    de_tilemap m = load_map(f, "maps/fixture_tmj");
    de_world *main_world = f->w;
    de_tilemap_desc d = {.top_down = true, .layer_spacing = 0.5f};
    f->w = make_world(f, &d);
    de_entity e = spawn_map(f, f->w, m, (de_dvec3){0, 0, 0}, false, true);
    de_tilemap_flush(f->w);
    found fd = find_health(f);
    ASSERT_EQ(fd.n, 1u);
    const de_transform *t =
        (const de_transform *)de_entity_get_const(f->w, fd.e[0], f->b.transform);
    ASSERT_TRUE(t != NULL);
    EXPECT_FLOAT_EQ(t->position.x, 3.0, 1e-9);
    EXPECT_FLOAT_EQ(t->position.y, -33.25, 1e-9);
    EXPECT_FLOAT_EQ(t->position.z, 0.0, 1e-9);
    i32 x, y;
    EXPECT_TRUE(de__tilemap_pick(f->w, e, (de_dvec3){3.5, -35.5, 9.0}, 1, &x, &y));
    EXPECT_EQ(x, 3);
    EXPECT_EQ(y, 35);
    de__tilemap_layer_info li;
    ASSERT_TRUE(de__tilemap_layer(f->w, e, 2, &li));
    EXPECT_FLOAT_EQ(li.depth, 1.0, 1e-7); /* toward a camera looking down -Z */
    de_world_destroy(f->w);
    f->w = main_world;
#endif
}
