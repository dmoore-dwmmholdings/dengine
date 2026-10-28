/*
 * tilemap_chunks.c: chunk rendering of the tilemap runtime (DENG-80, SPEC §12.12). A chunk is an
 * entity under the map entity; each non-empty cell is a child with DeSprite, so tiles go through
 * the Sprite material model like any sprite (visibility buffer, lighting, RT shadows). A rebuild
 * touches only its own chunk: cells that stayed the same keep their entity and data, cleared
 * cells despawn theirs, new ones spawn. Tile animation patches the uv_rect of the animated cells'
 * sprites; it allocates nothing.
 */
#include "gameplay/tilemap/tilemap_internal.h"

#include <dengine/de_ecs.h>
#include <dengine/de_math.h>
#include <dengine/de_render.h>

#include <math.h>
#include <string.h>

/* ---- Flips ------------------------------------------------------------------------------------ */

void de__tm_flip_to_rotation(u32 cell, u32 *quarter_turns, bool *flip_x) {
    /* Tiled: the diagonal flip (transpose) first, then horizontal, then vertical, in image
     * space (y down). In map space (y up) that is S * M * S with S = diag(1, -1). Find the
     * rotation R(k * 90 deg) and the local x-flip F^f with R * F^f equal to it. */
    int m[4] = {1, 0, 0, 1}; /* row-major 2x2 */
    if (cell & DE__TM_CELL_FLIP_D) {
        m[0] = 0, m[1] = 1, m[2] = 1, m[3] = 0;
    }
    if (cell & DE__TM_CELL_FLIP_H) {
        m[0] = -m[0], m[1] = -m[1];
    }
    if (cell & DE__TM_CELL_FLIP_V) {
        m[2] = -m[2], m[3] = -m[3];
    }
    /* S * M * S negates the off-diagonal entries. */
    const int t[4] = {m[0], -m[1], -m[2], m[3]};
    static const int k_rot[4][4] = {{1, 0, 0, 1}, {0, -1, 1, 0}, {-1, 0, 0, -1}, {0, 1, -1, 0}};
    for (u32 k = 0; k < 4; ++k)
        for (u32 f = 0; f < 2; ++f) {
            const int *r = k_rot[k];
            /* R * F, F = diag(-1, 1): negate the first column. */
            int rf[4] = {f ? -r[0] : r[0], r[1], f ? -r[2] : r[2], r[3]};
            if (memcmp(rf, t, sizeof rf) == 0) {
                *quarter_turns = k;
                *flip_x = f != 0;
                return;
            }
        }
    *quarter_turns = 0;
    *flip_x = false;
}

u32 de__tm_anim_tile(const de__tm_view *v, u32 tile, f64 ms) {
    if (tile >= v->h.tile_count)
        return tile;
    de__tm_tile t = de__tm_tile_at(v, tile);
    if (t.anim == 0)
        return tile;
    de__tm_anim a = de__tm_anim_at(v, t.anim - 1u);
    f64 m = fmod(ms, (f64)a.total_ms);
    if (!(m >= 0.0))
        m = 0.0;
    for (u32 k = 0; k < a.frame_count; ++k) {
        de__tm_frame f = de__tm_frame_at(v, a.frame_first + k);
        if (m < (f64)f.duration_ms)
            return f.tile;
        m -= (f64)f.duration_ms;
    }
    return de__tm_frame_at(v, a.frame_first + a.frame_count - 1u).tile;
}

/* ---- Sprites ---------------------------------------------------------------------------------- */

static void despawn_sprite(de__tilemap_sys *s, de__tm_chunk *c, u32 k) {
    if (c->sprites != NULL && c->sprites[k].bits != 0) {
        if (de_entity_alive(s->w, c->sprites[k]))
            de_entity_despawn(s->w, c->sprites[k]);
        c->sprites[k] = DE_ENTITY_NULL;
    }
}

static void clear_sprites(de__tilemap_sys *s, de__tm_chunk *c) {
    for (u32 k = 0; c->sprites != NULL && k < DE__TM_CHUNK_CELLS; ++k)
        despawn_sprite(s, c, k);
    c->animated_count = 0;
}

static bool ensure_arrays(de__tilemap_sys *s, de__tm_chunk *c) {
    if (c->sprites == NULL) {
        c->sprites = (de_entity *)DE_ALLOC(s->alloc, sizeof(de_entity) * DE__TM_CHUNK_CELLS,
                                           DE_ALIGNOF(de_entity), DE_MEM_GENERAL);
        if (c->sprites == NULL)
            return false;
        memset(c->sprites, 0, sizeof(de_entity) * DE__TM_CHUNK_CELLS);
    }
    if (c->animated == NULL) {
        c->animated = (u16 *)DE_ALLOC(s->alloc, sizeof(u16) * DE__TM_CHUNK_CELLS, 2,
                                      DE_MEM_GENERAL);
        c->anim_frame = (u32 *)DE_ALLOC(s->alloc, sizeof(u32) * DE__TM_CHUNK_CELLS, 4,
                                        DE_MEM_GENERAL);
        if (c->animated == NULL || c->anim_frame == NULL)
            return false;
    }
    return true;
}

static void set_uv(DeSprite *sp, const de__tm_tile *t) {
    sp->uv_rect = (de_vec4){t->u, t->v, t->du, t->dv};
}

void de__tm_chunk_build_mesh(de__tilemap_sys *s, de__tm_instance *in, de__tm_chunk *c) {
    c->mesh_builds++;
    const de__tm_view *v = &in->asset->view;
    const de__tm_layer l = de__tm_layer_at(v, c->layer);
    if (!DE_HANDLE_VALID(s->sprite) || !de__tm_layer_visible(&in->comp, c->layer)) {
        clear_sprites(s, c);
        return;
    }
    bool any = false;
    for (u32 k = 0; k < DE__TM_CHUNK_CELLS && !any; ++k)
        any = (c->cells[k] & DE__TM_CELL_TILE_MASK) != 0;
    if (!any && c->sprites == NULL)
        return;
    if (!ensure_arrays(s, c)) {
        DE_WARN(s->log, "tilemap: out of memory building a chunk");
        return;
    }
    if (c->root.bits == 0 || !de_entity_alive(s->w, c->root)) {
        const de_component comps[1] = {s->transform};
        if (de_entity_spawn_with(s->w, comps, 1, &c->root) != DE_OK)
            return;
        de_transform *t = (de_transform *)de_entity_get(s->w, c->root, s->transform);
        if (t != NULL)
            *t = de_transform_identity();
        (void)de_entity_set_parent(s->w, c->root, in->entity);
    }
    const de_vec3 axis = s->desc.top_down ? (de_vec3){0, 0, 1} : (de_vec3){0, -1, 0};
    const f32 depth = de__tm_layer_depth(s, c->layer);
    c->animated_count = 0;
    for (u32 k = 0; k < DE__TM_CHUNK_CELLS; ++k) {
        const u32 cell = c->cells[k];
        const u32 x = c->cx * DE__TM_CHUNK + k % DE__TM_CHUNK;
        const u32 y = c->cy * DE__TM_CHUNK + k / DE__TM_CHUNK;
        const u32 tile = cell & DE__TM_CELL_TILE_MASK;
        if (tile == 0 || tile > v->h.tile_count || x >= l.width || y >= l.height) {
            despawn_sprite(s, c, k);
            continue;
        }
        const u32 index = tile - 1u;
        de__tm_tile t = de__tm_tile_at(v, index);
        u32 shown = index;
        if (t.anim != 0) {
            shown = de__tm_anim_tile(v, index, in->anim_ms);
            c->animated[c->animated_count] = (u16)k;
            c->anim_frame[c->animated_count] = shown;
            c->animated_count++;
        }
        de__tm_tile ts = shown == index ? t : de__tm_tile_at(v, shown);
        if (c->sprites[k].bits == 0 || !de_entity_alive(s->w, c->sprites[k])) {
            const de_component comps[2] = {s->transform, s->sprite};
            if (de_entity_spawn_with(s->w, comps, 2, &c->sprites[k]) != DE_OK) {
                c->sprites[k] = DE_ENTITY_NULL;
                continue;
            }
            (void)de_entity_set_parent(s->w, c->sprites[k], c->root);
        }
        u32 turns = 0;
        bool flip_x = false;
        de__tm_flip_to_rotation(cell, &turns, &flip_x);
        /* The tile's sprite is anchored at the bottom-left of its cell; its center is the pivot. */
        const f64 u0 = (f64)l.origin_u + (f64)x * l.cell_w;
        const f64 v0 = (f64)l.origin_v - (f64)(y + 1u) * l.cell_h;
        de_transform *tr = (de_transform *)de_entity_get(s->w, c->sprites[k], s->transform);
        if (tr != NULL) {
            tr->position = de__tm_local(s, u0 + 0.5 * t.w, v0 + 0.5 * t.h, depth);
            tr->rotation = turns == 0 ? de_quat_identity()
                                      : de_quat_axis_angle(axis, (f32)turns * (DE_PI * 0.5f));
            tr->scale = (de_vec3){1, 1, 1};
        }
        DeSprite *sp = (DeSprite *)de_entity_get(s->w, c->sprites[k], s->sprite);
        if (sp != NULL) {
            memset(sp, 0, sizeof *sp);
            sp->texture = t.tileset < v->h.tileset_count ? in->textures[t.tileset] : (de_texture){0};
            set_uv(sp, &ts);
            sp->size = (de_vec2){t.w, t.h};
            sp->pivot = (de_vec2){0.5f, 0.5f};
            sp->color = (de_vec4){1, 1, 1, l.opacity};
            sp->layer = (i16)(c->layer < 32767u ? c->layer : 32767u);
            sp->order = 0;
            sp->flip_x = flip_x;
            sp->cast_shadows = in->comp.lit;
        }
    }
}

void de__tm_animate(de__tilemap_sys *s, de__tm_instance *in) {
    if (!DE_HANDLE_VALID(s->sprite) || in->chunks == NULL)
        return;
    const de__tm_view *v = &in->asset->view;
    for (u32 i = 0; i < v->h.chunk_count; ++i) {
        de__tm_chunk *c = &in->chunks[i];
        for (u32 a = 0; a < c->animated_count; ++a) {
            const u32 k = c->animated[a];
            const u32 index = (c->cells[k] & DE__TM_CELL_TILE_MASK) - 1u;
            const u32 shown = de__tm_anim_tile(v, index, in->anim_ms);
            if (shown == c->anim_frame[a])
                continue;
            c->anim_frame[a] = shown;
            DeSprite *sp = (DeSprite *)de_entity_get(s->w, c->sprites[k], s->sprite);
            if (sp != NULL) {
                de__tm_tile t = de__tm_tile_at(v, shown);
                set_uv(sp, &t);
            }
        }
    }
}
