/*
 * tilemap_runtime.c: the tilemap runtime (DENG-80, SPEC §21.4, §12.12): the `tilemap` asset type,
 * the reflected DeTilemap component, the `tilemap.update` system, live edits and the editor-facing
 * queries of tilemap_internal.h. Chunk building is in tilemap_chunks.c, colliders in
 * tilemap_collision.c, spawns in tilemap_spawn.c.
 *
 * Components the map builds with are found by name when it builds (DeSprite from the renderer,
 * DeCollider2D from 2D physics), so the module works headless and without physics: a world
 * without them gets no sprites or colliders.
 */
#include "gameplay/tilemap/tilemap_internal.h"

#include "asset/slots_internal.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_ecs.h>
#include <dengine/de_gameplay.h>
#include <dengine/de_memory.h>
#include <dengine/de_reflect.h>

#include <math.h>
#include <string.h>

#ifndef DE_HAS_COOKER
#define DE_HAS_COOKER 0
#endif
#if DE_HAS_COOKER
#include "cook/lib/de_cooker.h"
#endif

/* ---- Asset type ------------------------------------------------------------------------------ */

static de_result tm_load(de_asset_load_ctx *ctx) {
    de_asset_system *assets = (de_asset_system *)de_asset_load_ctx_user(ctx);
    const de_allocator *a = de_heap_allocator();
    u64 size = 0;
    const void *data = de_asset_load_ctx_data(ctx, &size);
    const size_t header = (sizeof(de__tm_asset) + 15u) & ~(size_t)15u;
    if (size > (u64)SIZE_MAX - header - 16u)
        return de_asset_load_ctx_fail(ctx, DE_ERR_OUT_OF_MEMORY, "tilemap: too large");
    size_t bytes = header + (size_t)size;
    de__tm_asset *o = (de__tm_asset *)DE_ALLOC(a, bytes, 16, DE_MEM_ASSET);
    if (o == NULL)
        return de_asset_load_ctx_fail(ctx, DE_ERR_OUT_OF_MEMORY, "tilemap: out of memory");
    memset(o, 0, sizeof *o);
    o->alloc = a;
    o->bytes = bytes;
    u8 *copy = (u8 *)o + header;
    memcpy(copy, data, (size_t)size);
    const char *why = NULL;
    if (!de__tm_view_parse(copy, size, &o->view, &why)) {
        DE_FREE(a, o, bytes);
        return de_asset_load_ctx_fail(ctx, DE_ERR_PARSE, "%s", why != NULL ? why : "tilemap");
    }
    /* Dependencies (the importer declares none; see tilemap_import.c): tileset textures when a
     * renderer registered the `texture` type, and mapped prefabs, when the manifest declares
     * them. In PACKED mode (no manifest) put them in the map's bundle. */
    if (assets != NULL) {
        const bool textures = DE_HANDLE_VALID(de__asset_type_find(assets, "texture"));
        for (u32 i = 0; textures && i < o->view.h.tileset_count; ++i) {
            de_asset_id id = {de__tm_tileset_at(&o->view, i).texture};
            if (id.hash != 0 && de_asset_id_path(assets, id) != NULL)
                (void)de_asset_load_ctx_add_dependency(ctx, id);
        }
        for (u32 i = 0; i < o->view.h.spawn_count; ++i) {
            de_asset_id id = {de__tm_spawn_at(&o->view, i).prefab};
            if (id.hash != 0 && de_asset_id_path(assets, id) != NULL)
                (void)de_asset_load_ctx_add_dependency(ctx, id);
        }
    }
    de_asset_load_ctx_set_object(ctx, o, bytes);
    return DE_OK;
}

static void tm_unload(void *object, void *user) {
    (void)user;
    de__tm_asset *o = (de__tm_asset *)object;
    if (o != NULL)
        DE_FREE(o->alloc, o, o->bytes);
}

static const char *const k_tm_ext[] = {".ldtk", ".tmj", ".tmx"};

static de_result register_asset_type(de_asset_system *assets) {
    if (assets == NULL || DE_HANDLE_VALID(de__asset_type_find(assets, "tilemap")))
        return DE_OK;
    de_asset_type_desc td;
    memset(&td, 0, sizeof td);
    td.name = "tilemap";
    td.extensions = k_tm_ext;
    td.extension_count = (u32)(sizeof k_tm_ext / sizeof k_tm_ext[0]);
    td.importer_version = 1;
#if DE_HAS_COOKER
    de_asset_type_desc builtin;
    if (de_cooker_builtin_importer("tilemap", &builtin)) {
        td.import = builtin.import;
        td.importer_version = builtin.importer_version;
    }
#endif
    td.load = tm_load;
    td.unload = tm_unload;
    td.user = assets;
    td.main_thread_load = true; /* reads the manifest for its dependencies */
    td.category = DE_ASSET_CAT_CPU;
    if (!DE_HANDLE_VALID(de_asset_type_register(assets, &td)))
        return DE_FAIL_WRAP(de_error_last() != NULL ? de_error_last()->result : DE_ERR_INTERNAL,
                            "tilemap asset type");
    return DE_OK;
}

de_tilemap de_asset_tilemap(de_asset_system *s, de_asset_id id) {
    return (de_tilemap){de__asset_handle_of(s, id, "tilemap")};
}

const de__tm_asset *de__tilemap_asset(de_asset_system *s, de_tilemap m) {
    if (s == NULL || !DE_HANDLE_VALID(m))
        return NULL;
    const de__slot *sl = de__slot_get(s, m.id);
    if (sl == NULL || sl->state != DE_LOAD_READY || sl->type_hash != de_hash64_str("tilemap"))
        return NULL;
    return (const de__tm_asset *)de__slot_object(sl);
}

u32 de_tilemap_get_tile(de_asset_system *s, de_tilemap m, u32 layer, i32 x, i32 y) {
    const de__tm_asset *a = de__tilemap_asset(s, m);
    if (a == NULL || layer >= a->view.h.layer_count || x < 0 || y < 0)
        return 0;
    de__tm_layer l = de__tm_layer_at(&a->view, layer);
    if ((u32)x >= l.width || (u32)y >= l.height)
        return 0;
    u32 chunk = l.chunk_first + ((u32)y / DE__TM_CHUNK) * l.chunks_x + (u32)x / DE__TM_CHUNK;
    u32 cell = ((u32)y % DE__TM_CHUNK) * DE__TM_CHUNK + (u32)x % DE__TM_CHUNK;
    return de__tm_rd32(de__tm_chunk_cells(&a->view, chunk) + 4u * cell) & ~DE__TM_CELL_SOLID;
}

/* ---- Helpers ----------------------------------------------------------------------------------- */

de_dvec3 de__tm_local(const de__tilemap_sys *s, f64 u, f64 v, f64 depth) {
    if (s->desc.top_down)
        return (de_dvec3){u, v, depth};
    return (de_dvec3){u, depth, v};
}

f32 de__tm_layer_depth(const de__tilemap_sys *s, u32 layer) {
    /* Side view: the camera looks along +Y, so nearer is -Y; top-down: it looks along -Z. */
    f32 d = s->desc.layer_spacing * (f32)layer;
    return s->desc.top_down ? d : -d;
}

bool de__tm_layer_visible(const DeTilemap *c, u32 layer) {
    if (c->layer_mask == 0)
        return true;
    return ((c->layer_mask >> (layer < 31u ? layer : 31u)) & 1u) != 0;
}

de_transform de__tm_entity_xform(de__tilemap_sys *s, de_entity e) {
    const de_transform *g = (const de_transform *)de_entity_get_const(s->w, e, s->global);
    if (g != NULL)
        return *g;
    const de_transform *t = (const de_transform *)de_entity_get_const(s->w, e, s->transform);
    return t != NULL ? *t : de_transform_identity();
}

/* ---- Instances ----------------------------------------------------------------------------------- */

de__tilemap_sys *de__tilemap_sys_of(de_world *w) {
    if (w == NULL)
        return NULL;
    de_type_id t = de_type_find(de_world_types(w), "DeTilemapSystemRef");
    if (!DE_HANDLE_VALID(t))
        return NULL;
    de_resource_id r = de_resource_find(w, t);
    if (!DE_HANDLE_VALID(r))
        return NULL;
    const DeTilemapSystemRef *ref = (const DeTilemapSystemRef *)de_resource_get(w, r);
    return ref != NULL ? ref->ptr : NULL;
}

de__tm_instance *de__tilemap_instance(de__tilemap_sys *s, de_entity e) {
    for (u32 i = 0; s != NULL && i < s->inst_count; ++i)
        if (s->inst[i].entity.bits == e.bits)
            return &s->inst[i];
    return NULL;
}

static void chunk_free(de__tilemap_sys *s, de__tm_chunk *c) {
    const de_allocator *a = s->alloc;
    if (c->cells != NULL)
        DE_FREE(a, c->cells, sizeof(u32) * DE__TM_CHUNK_CELLS);
    if (c->sprites != NULL)
        DE_FREE(a, c->sprites, sizeof(de_entity) * DE__TM_CHUNK_CELLS);
    if (c->animated != NULL)
        DE_FREE(a, c->animated, sizeof(u16) * DE__TM_CHUNK_CELLS);
    if (c->anim_frame != NULL)
        DE_FREE(a, c->anim_frame, sizeof(u32) * DE__TM_CHUNK_CELLS);
    if (c->colliders != NULL)
        DE_FREE(a, c->colliders, sizeof(de_entity) * c->collider_cap);
    if (c->shapes != NULL)
        DE_FREE(a, c->shapes, sizeof(de_shape) * c->shape_cap);
    memset(c, 0, sizeof *c);
}

/* Frees an instance. With `world_alive` its chunk entities and colliders are despawned too
 * (not when the world is being destroyed). Spawned map entities are the game's and stay. */
static void instance_free(de__tilemap_sys *s, de__tm_instance *in, bool world_alive) {
    const de_allocator *a = s->alloc;
    if (in->chunks != NULL) {
        for (u32 i = 0; i < in->chunk_count; ++i) {
            de__tm_chunk *c = &in->chunks[i];
            if (world_alive) {
                de__tm_chunk_clear_colliders(s, c, true);
                if (c->root.bits != 0 && de_entity_alive(s->w, c->root))
                    de_entity_despawn_recursive(s->w, c->root);
            }
            chunk_free(s, c);
        }
        DE_FREE(a, in->chunks, sizeof(de__tm_chunk) * in->chunk_count);
    }
    if (in->textures != NULL)
        DE_FREE(a, in->textures, sizeof(de_texture) * (in->tileset_count + 1u));
    if (in->spawned != NULL)
        DE_FREE(a, in->spawned, sizeof(de_entity) * (in->spawn_count + 1u));
    memset(in, 0, sizeof *in);
}

static de__tm_instance *instance_create(de__tilemap_sys *s, de_entity e, const DeTilemap *comp,
                                        const de__tm_asset *asset) {
    if (s->inst_count == s->inst_cap) {
        u32 cap = s->inst_cap != 0 ? s->inst_cap * 2u : 8u;
        de__tm_instance *n =
            (de__tm_instance *)DE_ALLOC(s->alloc, sizeof *n * cap, DE_ALIGNOF(de__tm_instance),
                                        DE_MEM_GENERAL);
        if (n == NULL)
            return NULL;
        if (s->inst_count != 0)
            memcpy(n, s->inst, sizeof *n * s->inst_count);
        if (s->inst != NULL)
            DE_FREE(s->alloc, s->inst, sizeof *n * s->inst_cap);
        s->inst = n;
        s->inst_cap = cap;
    }
    de__tm_instance *in = &s->inst[s->inst_count];
    memset(in, 0, sizeof *in);
    in->entity = e;
    in->comp = *comp;
    in->asset = asset;
    const de__tm_view *v = &asset->view;
    const u32 n = v->h.chunk_count;
    in->chunk_count = n;
    in->tileset_count = v->h.tileset_count;
    in->spawn_count = v->h.spawn_count;
    in->chunks = n != 0 ? (de__tm_chunk *)DE_ALLOC(s->alloc, sizeof(de__tm_chunk) * n,
                                                   DE_ALIGNOF(de__tm_chunk), DE_MEM_GENERAL)
                        : NULL;
    in->textures = (de_texture *)DE_ALLOC(s->alloc, sizeof(de_texture) * (v->h.tileset_count + 1u),
                                          DE_ALIGNOF(de_texture), DE_MEM_GENERAL);
    in->spawned = (de_entity *)DE_ALLOC(s->alloc, sizeof(de_entity) * (v->h.spawn_count + 1u),
                                        DE_ALIGNOF(de_entity), DE_MEM_GENERAL);
    if (in->chunks != NULL)
        memset(in->chunks, 0, sizeof(de__tm_chunk) * n);
    if ((n != 0 && in->chunks == NULL) || in->textures == NULL || in->spawned == NULL) {
        instance_free(s, in, false);
        return NULL;
    }
    memset(in->textures, 0, sizeof(de_texture) * (v->h.tileset_count + 1u));
    memset(in->spawned, 0, sizeof(de_entity) * (v->h.spawn_count + 1u));
    s->inst_count++;
    for (u32 li = 0; li < v->h.layer_count; ++li) {
        de__tm_layer l = de__tm_layer_at(v, li);
        for (u32 cy = 0; cy < l.chunks_y; ++cy)
            for (u32 cx = 0; cx < l.chunks_x; ++cx) {
                u32 idx = l.chunk_first + cy * l.chunks_x + cx;
                de__tm_chunk *c = &in->chunks[idx];
                c->layer = li;
                c->cx = cx;
                c->cy = cy;
                c->cells = (u32 *)DE_ALLOC(s->alloc, sizeof(u32) * DE__TM_CHUNK_CELLS, 4,
                                           DE_MEM_GENERAL);
                if (c->cells == NULL) {
                    s->inst_count--;
                    instance_free(s, in, false);
                    return NULL;
                }
                const u8 *src = de__tm_chunk_cells(v, idx);
                for (u32 k = 0; k < DE__TM_CHUNK_CELLS; ++k)
                    c->cells[k] = de__tm_rd32(src + 4u * k);
                c->dirty_mesh = true;
                c->dirty_colliders = true;
            }
    }
    return in;
}

de__tm_instance *de__tilemap_instance_ensure(de__tilemap_sys *s, de_entity e) {
    de__tm_instance *in = de__tilemap_instance(s, e);
    if (in != NULL)
        return in;
    const DeTilemap *c = (const DeTilemap *)de_entity_get_const(s->w, e, s->comp);
    if (c == NULL)
        return NULL;
    const de__tm_asset *a = de__tilemap_asset(s->assets, c->map);
    if (a == NULL)
        return NULL;
    return instance_create(s, e, c, a);
}

static void resolve_textures(de__tilemap_sys *s, de__tm_instance *in) {
    const de__tm_view *v = &in->asset->view;
    const bool textures =
        s->assets != NULL && DE_HANDLE_VALID(de__asset_type_find(s->assets, "texture"));
    for (u32 i = 0; i < v->h.tileset_count; ++i) {
        de_asset_id id = {de__tm_tileset_at(v, i).texture};
        in->textures[i] = (de_texture){0};
        if (textures && id.hash != 0) {
            in->textures[i] = de_asset_texture(s->assets, id);
            if (!DE_HANDLE_VALID(in->textures[i]))
                de_error_clear();
        }
    }
}

static void lookup_components(de__tilemap_sys *s) {
    if (!DE_HANDLE_VALID(s->sprite))
        s->sprite = de_component_find(s->w, "DeSprite");
    if (!DE_HANDLE_VALID(s->collider))
        s->collider = de_component_find(s->w, "DeCollider2D");
    s->p2d = NULL;
    de_type_id t = de_type_find(de_world_types(s->w), "DePhysics2DWorld");
    if (DE_HANDLE_VALID(t)) {
        de_resource_id r = de_resource_find(s->w, t);
        const void *p = DE_HANDLE_VALID(r) ? de_resource_get(s->w, r) : NULL;
        if (p != NULL)
            memcpy(&s->p2d, p, sizeof s->p2d); /* DePhysics2DWorld { de_physics2d_world * } */
    }
}

/* Collects the entities of the query into s->scratch. */
typedef struct collect_ctx {
    de__tilemap_sys *s;
    u32 count;
    bool overflow;
} collect_ctx;
static void collect(const de_query_chunk *chunk, void *user) {
    collect_ctx *c = (collect_ctx *)user;
    for (u32 i = 0; i < chunk->count; ++i) {
        if (c->count >= c->s->scratch_cap) {
            c->overflow = true;
            return;
        }
        c->s->scratch[c->count++] = chunk->entities[i];
    }
}

static bool grow_scratch(de__tilemap_sys *s, u32 need) {
    if (need <= s->scratch_cap)
        return true;
    u32 cap = s->scratch_cap != 0 ? s->scratch_cap : 16u;
    while (cap < need)
        cap *= 2u;
    de_entity *n = (de_entity *)DE_ALLOC(s->alloc, sizeof(de_entity) * cap, DE_ALIGNOF(de_entity),
                                         DE_MEM_GENERAL);
    if (n == NULL)
        return false;
    if (s->scratch != NULL)
        DE_FREE(s->alloc, s->scratch, sizeof(de_entity) * s->scratch_cap);
    s->scratch = n;
    s->scratch_cap = cap;
    return true;
}

static void build_instance(de__tilemap_sys *s, de__tm_instance *in) {
    if (!in->built) {
        resolve_textures(s, in);
        in->built = true;
    }
    const de__tm_view *v = &in->asset->view;
    for (u32 i = 0; i < v->h.chunk_count; ++i) {
        de__tm_chunk *c = &in->chunks[i];
        if (c->dirty_mesh) {
            de__tm_chunk_build_mesh(s, in, c);
            c->dirty_mesh = false;
        }
        if (c->dirty_colliders) {
            if (in->comp.generate_colliders)
                de__tm_chunk_build_colliders(s, in, c);
            c->dirty_colliders = false;
        }
    }
    if (in->comp.spawn_entities && !in->spawned_done) {
        de__tm_spawn_entities(s, in);
        in->spawned_done = true;
    }
}

static void process(de__tilemap_sys *s, de_query q, f32 dt) {
    lookup_components(s);
    for (u32 i = 0; i < s->inst_count; ++i)
        s->inst[i].seen = false;
    u32 count = 0;
    if (DE_HANDLE_VALID(q)) {
        for (;;) {
            collect_ctx cc = {s, 0, false};
            de_query_each(s->w, q, collect, &cc);
            count = cc.count;
            if (!cc.overflow)
                break;
            if (!grow_scratch(s, s->scratch_cap * 2u + 16u)) {
                count = 0;
                break;
            }
        }
    }
    for (u32 k = 0; k < count; ++k) {
        de_entity e = s->scratch[k];
        const DeTilemap *comp = (const DeTilemap *)de_entity_get_const(s->w, e, s->comp);
        if (comp == NULL)
            continue;
        const de__tm_asset *asset = de__tilemap_asset(s->assets, comp->map);
        de__tm_instance *in = de__tilemap_instance(s, e);
        if (in != NULL &&
            (in->asset != asset || memcmp(&in->comp, comp, sizeof *comp) != 0)) {
            /* Another map, a reload, or other settings: build again from the asset. */
            bool spawned = in->spawned_done && in->comp.map.id == comp->map.id;
            instance_free(s, in, true);
            *in = s->inst[--s->inst_count];
            in = NULL;
            if (asset != NULL) {
                in = instance_create(s, e, comp, asset);
                if (in != NULL)
                    in->spawned_done = spawned;
            }
        } else if (in == NULL && asset != NULL) {
            in = instance_create(s, e, comp, asset);
        }
        if (in == NULL)
            continue;
        in->seen = true;
        if (!de_entity_has(s->w, e, s->transform)) {
            de_transform *t = (de_transform *)de_entity_add(s->w, e, s->transform);
            if (t != NULL)
                *t = de_transform_identity();
        }
        build_instance(s, in);
        if (dt > 0.0f) {
            in->anim_ms += (f64)dt * 1000.0;
            de__tm_animate(s, in);
        }
    }
    /* Instances whose entity lost DeTilemap (or died). */
    for (u32 i = 0; i < s->inst_count;) {
        if (!s->inst[i].seen && !de_entity_has(s->w, s->inst[i].entity, s->comp)) {
            instance_free(s, &s->inst[i], true);
            s->inst[i] = s->inst[--s->inst_count];
        } else {
            ++i;
        }
    }
}

static void update_fn(de_system_ctx *ctx) {
    de__tilemap_sys *s = (de__tilemap_sys *)ctx->user;
    process(s, ctx->queries != NULL ? ctx->queries[0] : (de_query){0}, ctx->dt);
}

void de__tilemap_update(de__tilemap_sys *s, f32 dt) {
    if (s == NULL)
        return;
    de_query_term term = {s->comp, DE_READ};
    de_query q = de_query_create(s->w, &(de_query_desc){.terms = &term, .term_count = 1});
    process(s, q, dt);
    if (DE_HANDLE_VALID(q))
        de_query_destroy(s->w, q);
}

void de_tilemap_flush(de_world *w) {
    de__tilemap_update(de__tilemap_sys_of(w), 0.0f);
}

/* ---- Live edits ---------------------------------------------------------------------------------- */

static de__tm_chunk *cell_of(de__tm_instance *in, u32 layer, i32 x, i32 y, u32 *cell) {
    const de__tm_view *v = &in->asset->view;
    if (layer >= v->h.layer_count || x < 0 || y < 0)
        return NULL;
    de__tm_layer l = de__tm_layer_at(v, layer);
    if ((u32)x >= l.width || (u32)y >= l.height)
        return NULL;
    u32 idx = l.chunk_first + ((u32)y / DE__TM_CHUNK) * l.chunks_x + (u32)x / DE__TM_CHUNK;
    *cell = ((u32)y % DE__TM_CHUNK) * DE__TM_CHUNK + (u32)x % DE__TM_CHUNK;
    return &in->chunks[idx];
}

void de_tilemap_set_tile(de_world *w, de_entity e, u32 layer, i32 x, i32 y, u32 tile) {
    de__tilemap_sys *s = de__tilemap_sys_of(w);
    de__tm_instance *in = s != NULL ? de__tilemap_instance_ensure(s, e) : NULL;
    if (in == NULL) {
        if (s != NULL)
            DE_WARN(s->log, "de_tilemap_set_tile: entity %llx has no loaded tilemap",
                    (unsigned long long)e.bits);
        return;
    }
    u32 cell;
    de__tm_chunk *c = cell_of(in, layer, x, y, &cell);
    if (c == NULL)
        return;
    u32 t = tile & DE_TILEMAP_TILE_MASK;
    if (t > in->asset->view.h.tile_count) {
        DE_WARN(s->log, "de_tilemap_set_tile: tile %u is not in the map (%u tiles)", t - 1u,
                in->asset->view.h.tile_count);
        return;
    }
    u32 value = (tile & (DE_TILEMAP_TILE_MASK | DE__TM_CELL_FLIPS)) |
                (c->cells[cell] & DE__TM_CELL_SOLID);
    if (t == 0)
        value = c->cells[cell] & DE__TM_CELL_SOLID; /* empty: no flips */
    if (value == c->cells[cell])
        return;
    c->cells[cell] = value;
    c->dirty_mesh = true;
    c->dirty_colliders = true;
}

u32 de_tilemap_get_cell(de_world *w, de_entity e, u32 layer, i32 x, i32 y) {
    de__tilemap_sys *s = de__tilemap_sys_of(w);
    de__tm_instance *in = s != NULL ? de__tilemap_instance_ensure(s, e) : NULL;
    if (in == NULL)
        return 0;
    u32 cell;
    de__tm_chunk *c = cell_of(in, layer, x, y, &cell);
    return c != NULL ? c->cells[cell] & ~DE__TM_CELL_SOLID : 0u;
}

/* ---- Editor queries ------------------------------------------------------------------------------ */

static de__tm_instance *editor_instance(de_world *w, de_entity e) {
    de__tilemap_sys *s = de__tilemap_sys_of(w);
    return s != NULL ? de__tilemap_instance_ensure(s, e) : NULL;
}

u32 de__tilemap_layer_count(de_world *w, de_entity e) {
    de__tm_instance *in = editor_instance(w, e);
    return in != NULL ? in->asset->view.h.layer_count : 0u;
}

bool de__tilemap_layer(de_world *w, de_entity e, u32 layer, de__tilemap_layer_info *out) {
    de__tm_instance *in = editor_instance(w, e);
    if (in == NULL || out == NULL || layer >= in->asset->view.h.layer_count)
        return false;
    de__tm_layer l = de__tm_layer_at(&in->asset->view, layer);
    out->name = de__tm_str(&in->asset->view, l.name);
    out->width = l.width;
    out->height = l.height;
    out->cell_w = l.cell_w;
    out->cell_h = l.cell_h;
    out->origin = (de_vec2){l.origin_u, l.origin_v};
    out->depth = de__tm_layer_depth(de__tilemap_sys_of(w), layer);
    out->flags = l.flags;
    out->visible = de__tm_layer_visible(&in->comp, layer);
    return true;
}

u32 de__tilemap_tile_count(de_world *w, de_entity e) {
    de__tm_instance *in = editor_instance(w, e);
    return in != NULL ? in->asset->view.h.tile_count : 0u;
}

bool de__tilemap_tile(de_world *w, de_entity e, u32 tile_index, de__tilemap_tile_info *out) {
    de__tm_instance *in = editor_instance(w, e);
    if (in == NULL || out == NULL || tile_index >= in->asset->view.h.tile_count)
        return false;
    const de__tm_view *v = &in->asset->view;
    de__tm_tile t = de__tm_tile_at(v, tile_index);
    memset(out, 0, sizeof *out);
    out->uv_rect = (de_vec4){t.u, t.v, t.du, t.dv};
    out->texture = (de_asset_id){de__tm_tileset_at(v, t.tileset).texture};
    if (!in->built)
        resolve_textures(de__tilemap_sys_of(w), in);
    out->texture_handle = in->textures[t.tileset];
    out->size = (de_vec2){t.w, t.h};
    out->tileset = t.tileset;
    out->animated = t.anim != 0;
    out->solid = (t.flags & DE__TM_TILE_SOLID) != 0;
    out->shape_count = t.shape_count;
    return true;
}

u32 de__tilemap_tileset_count(de_world *w, de_entity e) {
    de__tm_instance *in = editor_instance(w, e);
    return in != NULL ? in->asset->view.h.tileset_count : 0u;
}

bool de__tilemap_tileset(de_world *w, de_entity e, u32 tileset,
                              de__tilemap_tileset_info *out) {
    de__tm_instance *in = editor_instance(w, e);
    if (in == NULL || out == NULL || tileset >= in->asset->view.h.tileset_count)
        return false;
    de__tm_tileset t = de__tm_tileset_at(&in->asset->view, tileset);
    out->name = de__tm_str(&in->asset->view, t.name);
    out->texture = (de_asset_id){t.texture};
    out->first_tile = t.first_tile;
    out->tile_count = t.tile_count;
    out->columns = t.columns;
    out->image_w = t.image_w;
    out->image_h = t.image_h;
    return true;
}

/* World point -> map (u, v). */
static bool to_map(de_world *w, de_entity e, de_dvec3 p, f64 *u, f64 *v) {
    de__tilemap_sys *s = de__tilemap_sys_of(w);
    if (s == NULL)
        return false;
    de_transform t = de__tm_entity_xform(s, e);
    de_transform inv = de_transform_inverse(&t);
    de_dvec3 l = de_transform_point(&inv, p);
    *u = l.x;
    *v = s->desc.top_down ? l.y : l.z;
    return true;
}

bool de__tilemap_pick(de_world *w, de_entity e, de_dvec3 world_point, u32 layer, i32 *x, i32 *y) {
    de__tm_instance *in = editor_instance(w, e);
    f64 u, v;
    if (in == NULL || layer >= in->asset->view.h.layer_count || !to_map(w, e, world_point, &u, &v))
        return false;
    de__tm_layer l = de__tm_layer_at(&in->asset->view, layer);
    f64 fx = floor((u - l.origin_u) / l.cell_w);
    f64 fy = floor((l.origin_v - v) / l.cell_h);
    if (fx < -2147483648.0 || fx > 2147483647.0 || fy < -2147483648.0 || fy > 2147483647.0)
        return false;
    i32 cx = (i32)fx, cy = (i32)fy;
    if (x != NULL)
        *x = cx;
    if (y != NULL)
        *y = cy;
    return cx >= 0 && cy >= 0 && (u32)cx < l.width && (u32)cy < l.height;
}

bool de__tilemap_pick_top(de_world *w, de_entity e, de_dvec3 world_point, u32 *layer, i32 *x,
                          i32 *y) {
    de__tm_instance *in = editor_instance(w, e);
    if (in == NULL)
        return false;
    bool have = false;
    u32 best = 0;
    i32 bx = 0, by = 0;
    for (u32 i = in->asset->view.h.layer_count; i-- > 0;) {
        if (!de__tm_layer_visible(&in->comp, i))
            continue;
        i32 cx, cy;
        if (!de__tilemap_pick(w, e, world_point, i, &cx, &cy))
            continue;
        if (de_tilemap_get_cell(w, e, i, cx, cy) != 0) {
            best = i, bx = cx, by = cy, have = true;
            break;
        }
        if (!have) {
            best = i, bx = cx, by = cy;
            have = true;
        }
    }
    if (!have)
        return false;
    if (layer != NULL)
        *layer = best;
    if (x != NULL)
        *x = bx;
    if (y != NULL)
        *y = by;
    return true;
}

bool de__tilemap_cell_corners(de_world *w, de_entity e, u32 layer, i32 x, i32 y,
                              de_dvec3 corners[4]) {
    de__tilemap_sys *s = de__tilemap_sys_of(w);
    de__tm_instance *in = editor_instance(w, e);
    if (s == NULL || in == NULL || layer >= in->asset->view.h.layer_count || corners == NULL)
        return false;
    de__tm_layer l = de__tm_layer_at(&in->asset->view, layer);
    de_transform t = de__tm_entity_xform(s, e);
    const f64 u0 = l.origin_u + (f64)x * l.cell_w, u1 = u0 + l.cell_w;
    const f64 v1 = l.origin_v - (f64)y * l.cell_h, v0 = v1 - l.cell_h;
    const f64 d = de__tm_layer_depth(s, layer);
    const f64 uv[4][2] = {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}};
    for (u32 k = 0; k < 4; ++k)
        corners[k] = de_transform_point(&t, de__tm_local(s, uv[k][0], uv[k][1], d));
    return true;
}

/* ---- Attach -------------------------------------------------------------------------------------- */

static void ref_destruct(void *obj) {
    DeTilemapSystemRef *ref = (DeTilemapSystemRef *)obj;
    de__tilemap_sys *s = ref->ptr;
    if (s == NULL)
        return;
    /* The world is going away (or detach removed the resource): free the memory only. */
    for (u32 i = 0; i < s->inst_count; ++i)
        instance_free(s, &s->inst[i], false);
    if (s->inst != NULL)
        DE_FREE(s->alloc, s->inst, sizeof(de__tm_instance) * s->inst_cap);
    if (s->scratch != NULL)
        DE_FREE(s->alloc, s->scratch, sizeof(de_entity) * s->scratch_cap);
    const de_allocator *a = s->alloc;
    DE_FREE(a, s, sizeof *s);
    ref->ptr = NULL;
}

static void ref_copy(void *dst, const void *src) {
    memcpy(dst, src, sizeof(DeTilemapSystemRef));
}

static de_type_id find_or_register(de_type_registry *r, const de_type_desc *d) {
    de_type_id id = de_type_find(r, d->name);
    if (!DE_HANDLE_VALID(id))
        id = de_type_register(r, d);
    return id;
}

de_result de_tilemap_attach(de_world *w, de_asset_system *assets, const de_tilemap_desc *desc) {
    if (w == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_tilemap_attach: NULL world");
    if (de__tilemap_sys_of(w) != NULL)
        return DE_OK;
    DE_TRY(register_asset_type(assets));
    de_type_registry *types = de_world_types(w);
    static const de_field_info fields[] = {
        DE_FIELD_EX(DeTilemap, map, DE_FK_ASSET, DE_FIELD_DEFAULT | DE_FIELD_ASSET_REF,
                    {0, 0, 0, "The tilemap asset", "tilemap", 0}),
        DE_FIELD_EX(DeTilemap, layer_mask, DE_FK_U32, DE_FIELD_DEFAULT,
                    {0, 0, 0, "Layers to show: bit i = layer i; 0 = all", 0, 0}),
        DE_FIELD(DeTilemap, generate_colliders, DE_FK_BOOL, DE_FIELD_DEFAULT),
        DE_FIELD(DeTilemap, spawn_entities, DE_FK_BOOL, DE_FIELD_DEFAULT),
        DE_FIELD(DeTilemap, lit, DE_FK_BOOL, DE_FIELD_DEFAULT),
    };
    de_type_id ct = find_or_register(types, &DE_TYPE_DESC(DeTilemap, 1, fields));
    if (!DE_HANDLE_VALID(ct))
        return DE_FAIL_WRAP(DE_ERR_INTERNAL, "DeTilemap type");
    de_component comp = de_component_register(w, ct, DE_COMP_SERIALIZE);
    if (!DE_HANDLE_VALID(comp))
        return DE_FAIL_WRAP(DE_ERR_INTERNAL, "DeTilemap component");
    de_type_desc rd;
    memset(&rd, 0, sizeof rd);
    rd.name = "DeTilemapSystemRef";
    rd.size = sizeof(DeTilemapSystemRef);
    rd.align = DE_ALIGNOF(DeTilemapSystemRef);
    rd.version = 1;
    rd.hooks.destruct = ref_destruct;
    rd.hooks.copy = ref_copy;
    de_type_id rt = find_or_register(types, &rd);
    if (!DE_HANDLE_VALID(rt))
        return DE_FAIL_WRAP(DE_ERR_INTERNAL, "DeTilemapSystemRef type");
    de_builtin_component_ids b = de_builtin_components(w);
    if (!DE_HANDLE_VALID(b.transform))
        return DE_FAIL_WRAP(DE_ERR_INTERNAL, "tilemap: ECS built-ins");

    const de_allocator *a = de_heap_allocator();
    de__tilemap_sys *s = DE_NEW(a, de__tilemap_sys, DE_MEM_GENERAL);
    if (s == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "tilemap system");
    memset(s, 0, sizeof *s);
    s->alloc = a;
    s->w = w;
    s->assets = assets;
    if (desc != NULL)
        s->desc = *desc;
    if (!(s->desc.layer_spacing > 0.0f))
        s->desc.layer_spacing = 0.01f;
    s->comp = comp;
    s->transform = b.transform;
    s->global = b.global_transform;
    s->ref_type = rt;
    s->log = de_log_register_category("tilemap", DE_LOG_INFO);
    DeTilemapSystemRef ref = {s};
    s->resource = de_resource_insert(w, rt, &ref);
    if (!DE_HANDLE_VALID(s->resource)) {
        DE_FREE(a, s, sizeof *s);
        return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "tilemap resource");
    }
    de_query_term term = {comp, DE_READ};
    de_query_desc qd = {.terms = &term, .term_count = 1};
    const char *before[] = {DE_TRANSFORM_PROPAGATE_SYSTEM};
    de_system_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.name = "tilemap.update";
    sd.schedule = DE_SCHEDULE_POST_UPDATE;
    sd.fn = update_fn;
    sd.user = s;
    sd.flags = DE_SYSTEM_MAIN_THREAD | DE_SYSTEM_EXCLUSIVE;
    sd.queries = &qd;
    sd.query_count = 1;
    sd.before = before;
    sd.before_count = 1;
    s->update = de_system_add(w, &sd);
    if (!DE_HANDLE_VALID(s->update)) {
        de_resource_remove(w, s->resource); /* frees `s` through ref_destruct */
        return DE_FAIL_WRAP(DE_ERR_INTERNAL, "tilemap.update system");
    }
    return DE_OK;
}

void de_tilemap_detach(de_world *w) {
    de__tilemap_sys *s = de__tilemap_sys_of(w);
    if (s == NULL)
        return;
    for (u32 i = 0; i < s->inst_count; ++i)
        instance_free(s, &s->inst[i], true);
    s->inst_count = 0;
    de_system_remove(w, s->update);
    de_resource_remove(w, s->resource); /* ref_destruct frees `s` */
}
