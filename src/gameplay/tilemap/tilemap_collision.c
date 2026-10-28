/*
 * tilemap_collision.c: 2D colliders of the tilemap runtime (DENG-80, SPEC §13.3, §21.4). Per
 * chunk, the cells that collide as a whole (IntGrid solid cells, tiles of a collision layer, tiles
 * whose collision rectangle covers them) are merged greedily into rectangles, each a static
 * DeCollider2D box on its own root entity in world space (the 2D ECS binding reads DeTransform as
 * the body pose). A tile's own collision shapes (polygons, chains of at most 8 points) get one
 * collider each at the tile. A chunk's colliders are rebuilt only when that chunk changed.
 */
#include "gameplay/tilemap/tilemap_internal.h"

#include <dengine/de_ecs.h>
#include <dengine/de_math.h>
#include <dengine/de_physics.h>

#include <string.h>

#ifndef DE_HAS_PHYSICS
#define DE_HAS_PHYSICS 0
#endif

bool de__tm_cell_solid(const de__tm_view *v, const de__tm_layer *l, u32 cell) {
    if (cell & DE__TM_CELL_SOLID)
        return true;
    u32 tile = cell & DE__TM_CELL_TILE_MASK;
    if (tile == 0 || tile > v->h.tile_count)
        return false;
    const de__tm_tile t = de__tm_tile_at(v, tile - 1u);
    if (t.flags & DE__TM_TILE_SOLID)
        return true;
    /* In a collision layer every tile collides as a whole cell, except tiles with their own
     * shapes (slopes, platforms): their shapes are their collision. */
    return (l->flags & DE__TM_LAYER_COLLIDE) != 0 && t.shape_count == 0;
}

u32 de__tm_merge_rects(const u8 *solid, de__tm_rect *out) {
    u8 used[DE__TM_CHUNK_CELLS];
    memset(used, 0, sizeof used);
    u32 n = 0;
    const u32 N = DE__TM_CHUNK;
    for (u32 y = 0; y < N; ++y)
        for (u32 x = 0; x < N; ++x) {
            if (!solid[y * N + x] || used[y * N + x])
                continue;
            u32 w = 1;
            while (x + w < N && solid[y * N + x + w] && !used[y * N + x + w])
                ++w;
            u32 h = 1;
            for (; y + h < N; ++h) {
                bool row = true;
                for (u32 k = 0; k < w && row; ++k)
                    row = solid[(y + h) * N + x + k] && !used[(y + h) * N + x + k];
                if (!row)
                    break;
            }
            for (u32 j = 0; j < h; ++j)
                memset(&used[(y + j) * N + x], 1, w);
            out[n++] = (de__tm_rect){x, y, w, h};
        }
    return n;
}

void de__tm_chunk_clear_colliders(de__tilemap_sys *s, de__tm_chunk *c, bool world_alive) {
    for (u32 i = 0; world_alive && i < c->collider_count; ++i)
        if (de_entity_alive(s->w, c->colliders[i]))
            de_entity_despawn(s->w, c->colliders[i]);
    c->collider_count = 0;
#if DE_HAS_PHYSICS
    /* The bodies go when the 2D ECS binding sees their entities gone (its next step); a shape
     * still used by a body is refused (DE_ERR_BUSY), so destroy what can go and keep the rest. */
    u32 kept = 0;
    for (u32 i = 0; i < c->shape_count; ++i) {
        if (world_alive && s->p2d != NULL && de_shape2d_valid(s->p2d, c->shapes[i]) &&
            de_shape2d_destroy(s->p2d, c->shapes[i]) == DE_ERR_BUSY) {
            de_error_clear();
            c->shapes[kept++] = c->shapes[i];
        }
    }
    c->shape_count = kept;
#else
    c->shape_count = 0;
#endif
}

static bool push_entity(de__tilemap_sys *s, de__tm_chunk *c, de_entity e) {
    if (c->collider_count == c->collider_cap) {
        u32 cap = c->collider_cap != 0 ? c->collider_cap * 2u : 16u;
        de_entity *n = (de_entity *)DE_ALLOC(s->alloc, sizeof(de_entity) * cap,
                                             DE_ALIGNOF(de_entity), DE_MEM_GENERAL);
        if (n == NULL)
            return false;
        if (c->collider_count != 0)
            memcpy(n, c->colliders, sizeof(de_entity) * c->collider_count);
        if (c->colliders != NULL)
            DE_FREE(s->alloc, c->colliders, sizeof(de_entity) * c->collider_cap);
        c->colliders = n;
        c->collider_cap = cap;
    }
    c->colliders[c->collider_count++] = e;
    return true;
}

#if DE_HAS_PHYSICS
static void push_shape(de__tilemap_sys *s, de__tm_chunk *c, de_shape sh) {
    if (c->shape_count == c->shape_cap) {
        u32 cap = c->shape_cap != 0 ? c->shape_cap * 2u : 8u;
        de_shape *n = (de_shape *)DE_ALLOC(s->alloc, sizeof(de_shape) * cap, DE_ALIGNOF(de_shape),
                                           DE_MEM_GENERAL);
        if (n == NULL)
            return;
        if (c->shape_count != 0)
            memcpy(n, c->shapes, sizeof(de_shape) * c->shape_count);
        if (c->shapes != NULL)
            DE_FREE(s->alloc, c->shapes, sizeof(de_shape) * c->shape_cap);
        c->shapes = n;
        c->shape_cap = cap;
    }
    c->shapes[c->shape_count++] = sh;
}
#endif

/* A collider root entity at map position (u, v), in world space. */
static DeCollider2D *spawn_collider(de__tilemap_sys *s, de__tm_chunk *c, const de_transform *map,
                                    f64 u, f64 v) {
    const de_component comps[2] = {s->transform, s->collider};
    de_entity e = DE_ENTITY_NULL;
    if (de_entity_spawn_with(s->w, comps, 2, &e) != DE_OK)
        return NULL;
    if (!push_entity(s, c, e)) {
        de_entity_despawn(s->w, e);
        return NULL;
    }
    de_transform local = de_transform_identity();
    local.position = de__tm_local(s, u, v, 0.0);
    de_transform *t = (de_transform *)de_entity_get(s->w, e, s->transform);
    if (t != NULL)
        *t = de_transform_mul(map, &local);
    DeCollider2D *col = (DeCollider2D *)de_entity_get(s->w, e, s->collider);
    if (col != NULL) {
        memset(col, 0, sizeof *col);
        col->material.friction = 0.6f;
    }
    return col;
}

void de__tm_chunk_build_colliders(de__tilemap_sys *s, de__tm_instance *in, de__tm_chunk *c) {
    c->collider_builds++;
    de__tm_chunk_clear_colliders(s, c, true);
    if (!DE_HANDLE_VALID(s->collider))
        return;
    const de__tm_view *v = &in->asset->view;
    const de__tm_layer l = de__tm_layer_at(v, c->layer);
    const de_transform map = de__tm_entity_xform(s, in->entity);
    /* Plane scale of the map entity (its in-plane axes). */
    const f32 su = map.scale.x, sv = s->desc.top_down ? map.scale.y : map.scale.z;
    u8 solid[DE__TM_CHUNK_CELLS];
    for (u32 k = 0; k < DE__TM_CHUNK_CELLS; ++k) {
        const u32 x = c->cx * DE__TM_CHUNK + k % DE__TM_CHUNK;
        const u32 y = c->cy * DE__TM_CHUNK + k / DE__TM_CHUNK;
        solid[k] = x < l.width && y < l.height && de__tm_cell_solid(v, &l, c->cells[k]) ? 1u : 0u;
    }
    de__tm_rect rects[DE__TM_CHUNK_CELLS];
    const u32 n = de__tm_merge_rects(solid, rects);
    for (u32 i = 0; i < n; ++i) {
        const de__tm_rect *r = &rects[i];
        const f64 gx = (f64)(c->cx * DE__TM_CHUNK + r->x), gy = (f64)(c->cy * DE__TM_CHUNK + r->y);
        const f64 u = l.origin_u + (gx + 0.5 * r->w) * l.cell_w;
        const f64 vv = l.origin_v - (gy + 0.5 * r->h) * l.cell_h;
        DeCollider2D *col = spawn_collider(s, c, &map, u, vv);
        if (col == NULL)
            return;
        col->kind = DE_SHAPE2D_BOX;
        col->half_extents = (de_vec2){0.5f * (f32)r->w * l.cell_w * su,
                                      0.5f * (f32)r->h * l.cell_h * sv};
    }
    /* Tile shapes. */
    for (u32 k = 0; k < DE__TM_CHUNK_CELLS; ++k) {
        const u32 cell = c->cells[k];
        const u32 tile = cell & DE__TM_CELL_TILE_MASK;
        const u32 x = c->cx * DE__TM_CHUNK + k % DE__TM_CHUNK;
        const u32 y = c->cy * DE__TM_CHUNK + k / DE__TM_CHUNK;
        if (tile == 0 || tile > v->h.tile_count || x >= l.width || y >= l.height)
            continue;
        const de__tm_tile t = de__tm_tile_at(v, tile - 1u);
        if (t.shape_count == 0)
            continue;
        u32 turns = 0;
        bool flip_x = false;
        de__tm_flip_to_rotation(cell, &turns, &flip_x);
        static const int k_rot[4][4] = {{1, 0, 0, 1}, {0, -1, 1, 0}, {-1, 0, 0, -1}, {0, 1, -1, 0}};
        const int *rm = k_rot[turns & 3u];
        const f64 u0 = l.origin_u + (f64)x * l.cell_w;
        const f64 v0 = l.origin_v - (f64)(y + 1u) * l.cell_h;
        const f64 cxu = 0.5 * t.w, cyv = 0.5 * t.h;
        for (u32 si = 0; si < t.shape_count; ++si) {
            const de__tm_shape sh = de__tm_shape_at(v, t.shape_first + si);
            if (sh.point_count > 8u)
                continue; /* the importer splits chains; never more than DeCollider2D holds */
            de_vec2 pts[8];
            for (u32 p = 0; p < sh.point_count; ++p) {
                de__tm_point pt = de__tm_point_at(v, sh.point_first + p);
                f64 px = pt.x - cxu, py = pt.y - cyv;
                if (flip_x)
                    px = -px;
                const f64 rx = rm[0] * px + rm[1] * py, ry = rm[2] * px + rm[3] * py;
                pts[p] = (de_vec2){(f32)((rx + cxu) * su), (f32)((ry + cyv) * sv)};
            }
            if (flip_x && sh.kind != DE__TM_SHAPE_CHAIN) {
                /* A mirror reverses the winding; keep loops counter-clockwise (solid inside). */
                for (u32 a = 0, b = sh.point_count - 1u; a < b; ++a, --b) {
                    de_vec2 tmp = pts[a];
                    pts[a] = pts[b];
                    pts[b] = tmp;
                }
            }
            DeCollider2D *col = spawn_collider(s, c, &map, u0, v0);
            if (col == NULL)
                return;
            memcpy(col->points, pts, sizeof(de_vec2) * sh.point_count);
            col->point_count = sh.point_count;
            if (sh.kind == DE__TM_SHAPE_POLYGON) {
                col->kind = DE_SHAPE2D_POLYGON;
            } else {
                col->kind = DE_SHAPE2D_CHAIN;
#if DE_HAS_PHYSICS
                if (s->p2d != NULL) {
                    col->shape = de_shape2d_chain_create(s->p2d, pts, sh.point_count,
                                                         sh.kind == DE__TM_SHAPE_CHAIN_LOOP);
                    if (DE_HANDLE_VALID(col->shape))
                        push_shape(s, c, col->shape);
                    else
                        de_error_clear();
                }
#endif
            }
        }
    }
}
