/*
 * foliage_extract.c: DeFoliage instances into the render world (DENG-822).
 *
 * Every placed instance of a DeFoliage entity becomes one instance slot, so foliage takes the
 * mesh path and the GPU scene's instance table like any DeMeshRenderer (one draw per mesh batch,
 * GPU-driven culling). The slots are detached: the entity map knows only the layer's entity, which
 * is every slot's owner (picking a blade of grass picks the layer).
 *
 * Distance LOD: a cell's instances of a type are dropped when the cell's nearest point (x and z)
 * is beyond the type's cullDistance from the main camera. Impostors are not drawn yet: an
 * instance past impostorDistance keeps its mesh until cullDistance.
 *
 * The foliageType assets are read here (only `mesh` and `cullDistance`), not through the
 * world/foliage model: render sits below the gameplay band (tools/check_layers/layers.toml).
 */
#include "render/world/render_world.h"
#include "render/loaders/render_assets.h"
#include "render/renderer_internal.h"

#include "core/de_toml_internal.h"

#include <math.h>
#include <string.h>

/* ---------- foliageType ---------- */

bool de__rw_foliage_parse(const char *text, size_t n, char *mesh, size_t cap, f32 *cull_distance) {
    if (text == NULL || n > 0x7fffffffu || cap == 0)
        return false;
    toml_result_t tr = de__toml_parse(de_heap_allocator(), text, (int)n);
    bool ok = tr.ok;
    *cull_distance = 0.0f;
    mesh[0] = '\0';
    bool has_mesh = false;
    for (int32_t i = 0; ok && i < tr.toptab.u.tab.size; ++i) {
        const char *key = tr.toptab.u.tab.key[i];
        const toml_datum_t *d = &tr.toptab.u.tab.value[i];
        if (strcmp(key, "mesh") == 0) {
            if (d->type != TOML_STRING || (size_t)d->u.str.len >= cap || d->u.str.len == 0) {
                ok = false;
                break;
            }
            memcpy(mesh, d->u.str.ptr, (size_t)d->u.str.len);
            mesh[d->u.str.len] = '\0';
            has_mesh = true;
        } else if (strcmp(key, "cullDistance") == 0) {
            if (d->type == TOML_FP64)
                *cull_distance = (f32)d->u.fp64;
            else if (d->type == TOML_INT64)
                *cull_distance = (f32)d->u.int64;
            else
                ok = false;
        }
    }
    de__toml_free(tr);
    return ok && has_mesh;
}

de_quat de__rw_foliage_rotation(f32 yaw, i8 nx, i8 nz) {
    const f32 h = yaw * (DE_PI / 180.0f) * 0.5f;
    const de_quat q_yaw = {0.0f, sinf(h), 0.0f, cosf(h)};
    if (nx == 0 && nz == 0)
        return q_yaw;
    const f32 x = (f32)nx / 127.0f, z = (f32)nz / 127.0f;
    const f32 y2 = 1.0f - x * x - z * z;
    const f32 y = y2 > 0.0f ? sqrtf(y2) : 0.0f;
    /* The shortest arc from +y to n: axis +y x n = (z, 0, -x), half-way quaternion. */
    const de_quat q_tilt = de_quat_normalize((de_quat){z, 0.0f, -x, 1.0f + y});
    return de_quat_mul(q_tilt, q_yaw);
}

static de__rw_foliage_type *type_find(de__rw_foliage *fo, u32 blob) {
    for (u32 i = 0; i < DE__RW_FOLIAGE_TYPE_CACHE; ++i)
        if (fo->types[i].blob == blob)
            return &fo->types[i];
    return NULL;
}

static de__rw_foliage_type *type_entry(de__rw_foliage *fo, u32 blob) {
    de__rw_foliage_type *t = type_find(fo, blob);
    if (t == NULL && (t = type_find(fo, 0)) != NULL) {
        memset(t, 0, sizeof *t);
        t->blob = blob;
    }
    return t;
}

bool de__rw_foliage_set_type(de__render_world *rw, de_blob blob, de_mesh mesh, f32 cull_distance) {
    de__rw_foliage_type *t = type_entry(&rw->foliage, blob.id);
    if (t == NULL)
        return false;
    t->mesh = mesh;
    t->cull_distance = cull_distance;
    t->ok = DE_HANDLE_VALID(mesh);
    t->pinned = true;
    rw->foliage.type_gen += 1u;
    return true;
}

/* Re-reads entry `t` when its blob's bytes changed. */
static void type_refresh(de__render_world *rw, de__rw_foliage_type *t) {
    de_asset_system *assets = rw->r->assets;
    if (t->pinned || assets == NULL)
        return;
    u64 size = 0;
    const void *data = de_blob_data(assets, (de_blob){t->blob}, &size);
    if (data == t->data)
        return;
    t->data = data;
    const bool was_ok = t->ok;
    t->ok = false;
    char path[256];
    f32 cull = 0.0f;
    if (data != NULL && de__rw_foliage_parse((const char *)data, (size_t)size, path, sizeof path,
                                             &cull)) {
        const de_asset_id id = de_asset_id_from(path);
        const de_mesh mesh = id.hash != 0 ? de_asset_mesh(assets, id) : (de_mesh){0};
        if (DE_HANDLE_VALID(mesh)) {
            if (mesh.id != t->mesh.id || !DE_HANDLE_VALID(t->load)) {
                if (DE_HANDLE_VALID(t->load))
                    de_asset_release(assets, t->load);
                t->load = de_asset_load(assets, id, 0);
            }
            t->mesh = mesh;
            t->cull_distance = cull;
            t->ok = true;
        } else {
            DE_WARN(rw->log, "foliage: the mesh `%s` of a foliage type is not an asset", path);
        }
    }
    if (t->ok || was_ok)
        rw->foliage.type_gen += 1u;
}

typedef struct resolve_ctx {
    de__render_world *rw;
} resolve_ctx;

static void resolve_chunk(const de_query_chunk *c, void *user) {
    de__render_world *rw = ((resolve_ctx *)user)->rw;
    const DeFoliage *layers = (const DeFoliage *)c->columns[0];
    for (u32 i = 0; i < c->count; ++i)
        for (u32 k = 0; k < DE_FOLIAGE_TYPES; ++k) {
            const de_blob b = layers[i].types[k];
            if (!DE_HANDLE_VALID(b))
                continue;
            de__rw_foliage_type *t = type_entry(&rw->foliage, b.id);
            if (t != NULL)
                type_refresh(rw, t);
        }
}

void de__rw_foliage_resolve(de__render_world *rw, de_world *w, de_query q, bool has_eye,
                            de_dvec3 eye) {
    rw->foliage.has_eye = has_eye;
    rw->foliage.eye = eye;
    resolve_ctx x = {rw};
    de_query_each(w, q, resolve_chunk, &x);
}

/* ---------- Layers ---------- */

static de__rw_foliage_layer *layer_of(de__render_world *rw, u64 entity, bool add) {
    de__rw_foliage *fo = &rw->foliage;
    for (u32 i = 0; i < fo->layer_count; ++i)
        if (fo->layers[i].entity == entity)
            return &fo->layers[i];
    if (!add)
        return NULL;
    if (fo->layer_count == fo->layer_cap) {
        const u32 cap = fo->layer_cap ? fo->layer_cap * 2u : 8u;
        de__rw_foliage_layer *n = (de__rw_foliage_layer *)DE_REALLOC(
            rw->alloc, fo->layers, sizeof *n * fo->layer_cap, sizeof *n * cap,
            DE_ALIGNOF(de__rw_foliage_layer), DE_MEM_RENDER);
        if (n == NULL)
            return NULL;
        fo->layers = n;
        fo->layer_cap = cap;
    }
    de__rw_foliage_layer *l = &fo->layers[fo->layer_count++];
    memset(l, 0, sizeof *l);
    l->entity = entity;
    l->dirty = true;
    return l;
}

static bool layer_reserve(de__render_world *rw, de__rw_foliage_layer *l, u32 n) {
    if (n <= l->cap)
        return true;
    u32 cap = l->cap ? l->cap : 64u;
    while (cap < n)
        cap *= 2u;
    u32 *s = (u32 *)DE_REALLOC(rw->alloc, l->slots, sizeof(u32) * l->cap, sizeof(u32) * cap,
                               DE_ALIGNOF(u32), DE_MEM_RENDER);
    if (s == NULL)
        return false;
    l->slots = s;
    l->cap = cap;
    return true;
}

static u64 mix(u64 h, u64 v) {
    for (u32 i = 0; i < 8; ++i)
        h = (h ^ ((v >> (8u * i)) & 0xffu)) * 0x100000001b3ull;
    return h;
}

/* Distance from the eye to cell (x, z) in the xz plane, metres. */
static f64 cell_distance(const de__rw_foliage *fo, i32 x, i32 z) {
    const f64 s = (f64)DE_FOLIAGE_CELL_SIZE;
    const f64 x0 = (f64)x * s, z0 = (f64)z * s;
    const f64 dx = fo->eye.x < x0 ? x0 - fo->eye.x : (fo->eye.x > x0 + s ? fo->eye.x - x0 - s : 0);
    const f64 dz = fo->eye.z < z0 ? z0 - fo->eye.z : (fo->eye.z > z0 + s ? fo->eye.z - z0 - s : 0);
    return sqrt(dx * dx + dz * dz);
}

/* The palette slots drawn in a cell at distance `d`: a bit per slot. */
static u32 cell_mask(const de__rw_foliage *fo, const de__rw_foliage_type *const types[], f64 d) {
    u32 mask = 0;
    for (u32 k = 0; k < DE_FOLIAGE_TYPES; ++k)
        if (types[k] != NULL && types[k]->ok &&
            (!fo->has_eye || types[k]->cull_distance <= 0.0f || d <= (f64)types[k]->cull_distance))
            mask |= 1u << k;
    return mask;
}

typedef struct extract_ctx {
    de__render_world *rw;
    u32 failed;
} extract_ctx;

static bool slot_bounds(de__render_world *rw, u32 s) {
    de_asset_system *assets = rw->r->assets;
    if (assets == NULL)
        return false;
    const de_render_mesh *m = de_render_mesh_get(assets, rw->inst.mesh[s]);
    if (m == NULL)
        return false;
    rw->inst_pool.local_bounds[s] =
        (de_aabb){{m->bounds_min[0], m->bounds_min[1], m->bounds_min[2]},
                  {m->bounds_max[0], m->bounds_max[1], m->bounds_max[2]}};
    rw->inst.flags[s] |= DE__RW_INST_HAS_BOUNDS;
    de__rw_inst_update_bounds(rw, s);
    return true;
}

static void slot_write(de__render_world *rw, u32 s, const de_foliage_instance *in, de_mesh mesh,
                       bool *need_bounds) {
    rw->inst.mesh[s] = mesh;
    memset(&rw->inst.materials[(size_t)s * DE__RW_MATERIALS], 0,
           sizeof(de_material) * DE__RW_MATERIALS);
    memset(&rw->inst.sprite[s], 0, sizeof rw->inst.sprite[s]);
    rw->inst.layer_mask[s] = 0xffffffffu;
    rw->inst.flags[s] = DE__RW_INST_CAST_SHADOWS | DE__RW_INST_VISIBLE_IN_RT;
    de_transform t;
    t.position = (de_dvec3){(f64)in->x, (f64)in->y, (f64)in->z};
    t.rotation = de__rw_foliage_rotation(in->yaw, in->nx, in->nz);
    t.scale = (de_vec3){in->scale, in->scale, in->scale};
    /* fresh: a rebuilt slot gets no motion vector from what it held before. */
    de__rw_pool_set_xform(rw, DE__RW_KIND_INSTANCE, s, &t, true);
    if (!slot_bounds(rw, s))
        *need_bounds = true;
}

static void layer_build(extract_ctx *x, de__rw_foliage_layer *l, const DeFoliage *fo,
                        const u32 *masks, u64 sig) {
    de__render_world *rw = x->rw;
    const de__rw_foliage_type *types[DE_FOLIAGE_TYPES];
    for (u32 k = 0; k < DE_FOLIAGE_TYPES; ++k)
        types[k] = DE_HANDLE_VALID(fo->types[k]) ? type_find(&rw->foliage, fo->types[k].id) : NULL;
    u32 want = 0, at = 0;
    for (u32 c = 0; c < fo->cell_count; ++c) {
        for (u32 i = 0; i < fo->cells[c].count; ++i) {
            const de_foliage_instance *in = &fo->instances[at + i];
            if (in->type < DE_FOLIAGE_TYPES && (masks[c] >> in->type) & 1u)
                ++want;
        }
        at += fo->cells[c].count;
    }
    if (!layer_reserve(rw, l, want)) {
        x->failed += want;
        want = 0;
    }
    const u32 old = l->count;
    u32 n = 0;
    bool need_bounds = false;
    at = 0;
    for (u32 c = 0; c < fo->cell_count && n < want; ++c) {
        for (u32 i = 0; i < fo->cells[c].count && n < want; ++i) {
            const de_foliage_instance *in = &fo->instances[at + i];
            if (in->type >= DE_FOLIAGE_TYPES || !((masks[c] >> in->type) & 1u))
                continue;
            u32 s = n < old ? l->slots[n] : DE__RW_NONE;
            if (s == DE__RW_NONE) {
                s = de__rw_pool_take_detached(rw, DE__RW_KIND_INSTANCE, (de_entity){l->entity});
                if (s == DE__RW_NONE) {
                    ++x->failed;
                    continue;
                }
            }
            rw->inst_pool.seen[s] = rw->frame;
            slot_write(rw, s, in, types[in->type]->mesh, &need_bounds);
            l->slots[n++] = s;
        }
        at += fo->cells[c].count;
    }
    /* Slots n..old are not seen this frame: the sweep frees them. */
    l->count = n;
    l->sig = sig;
    l->dirty = false;
    l->need_bounds = need_bounds;
}

static void layer_keep(de__render_world *rw, de__rw_foliage_layer *l) {
    bool need_bounds = false;
    for (u32 i = 0; i < l->count; ++i) {
        const u32 s = l->slots[i];
        rw->inst_pool.seen[s] = rw->frame;
        if (l->need_bounds && !(rw->inst.flags[s] & DE__RW_INST_HAS_BOUNDS)) {
            if (slot_bounds(rw, s))
                de__rw_pool_touch(rw, DE__RW_KIND_INSTANCE, s);
            else
                need_bounds = true;
        }
    }
    l->need_bounds = need_bounds;
}

static void changed_chunk(const de_query_chunk *c, void *user) {
    extract_ctx *x = (extract_ctx *)user;
    for (u32 i = 0; i < c->count; ++i) {
        de__rw_foliage_layer *l = layer_of(x->rw, c->entities[i].bits, false);
        if (l != NULL)
            l->dirty = true;
    }
}

static void all_chunk(const de_query_chunk *c, void *user) {
    extract_ctx *x = (extract_ctx *)user;
    de__render_world *rw = x->rw;
    const de__rw_foliage *ff = &rw->foliage;
    const DeFoliage *layers = (const DeFoliage *)c->columns[0];
    for (u32 i = 0; i < c->count; ++i) {
        const DeFoliage *fo = &layers[i];
        de__rw_foliage_layer *l = layer_of(rw, c->entities[i].bits, true);
        if (l == NULL) {
            x->failed += fo->instance_count;
            continue;
        }
        l->seen = rw->frame;
        const de__rw_foliage_type *types[DE_FOLIAGE_TYPES];
        for (u32 k = 0; k < DE_FOLIAGE_TYPES; ++k)
            types[k] = DE_HANDLE_VALID(fo->types[k]) ? type_find(&rw->foliage, fo->types[k].id)
                                                     : NULL;
        const u32 cells = fo->cell_count <= DE_FOLIAGE_CELLS ? fo->cell_count : 0;
        u32 masks[DE_FOLIAGE_CELLS];
        u64 sig = mix(0xcbf29ce484222325ull, ff->type_gen);
        sig = mix(sig, cells);
        u32 total = 0;
        for (u32 k = 0; k < cells; ++k) {
            masks[k] = cell_mask(ff, types, cell_distance(ff, fo->cells[k].x, fo->cells[k].z));
            sig = mix(sig, ((u64)(u32)fo->cells[k].x << 32) | (u32)fo->cells[k].z);
            sig = mix(sig, ((u64)fo->cells[k].count << 32) | masks[k]);
            sig = mix(sig, fo->cells[k].revision);
            total += fo->cells[k].count;
        }
        if (total != fo->instance_count || fo->instance_count > DE_FOLIAGE_INSTANCES) {
            /* An inconsistent table draws nothing rather than reading past it. */
            sig = mix(sig, 0xbadull);
            if (l->sig != sig || l->dirty) {
                l->count = 0;
                l->sig = sig;
                l->dirty = false;
            }
            continue;
        }
        if (l->dirty || l->sig != sig)
            layer_build(x, l, fo, masks, sig);
        else
            layer_keep(rw, l);
    }
}

u32 de__rw_foliage_extract(de__render_world *rw, de_world *w, de_query all, de_query changed) {
    extract_ctx x = {rw, 0};
    de_query_each(w, changed, changed_chunk, &x);
    de_query_each(w, all, all_chunk, &x);
    /* Layers whose entity is gone, or no longer has DeFoliage: their slots are swept. */
    de__rw_foliage *fo = &rw->foliage;
    u32 kept = 0;
    for (u32 i = 0; i < fo->layer_count; ++i) {
        if (fo->layers[i].seen == rw->frame) {
            fo->layers[kept++] = fo->layers[i];
            continue;
        }
        DE_FREE(rw->alloc, fo->layers[i].slots, sizeof(u32) * fo->layers[i].cap);
    }
    fo->layer_count = kept;
    return x.failed;
}

void de__rw_foliage_reset(de__render_world *rw) {
    de__rw_foliage *fo = &rw->foliage;
    for (u32 i = 0; i < fo->layer_count; ++i)
        DE_FREE(rw->alloc, fo->layers[i].slots, sizeof(u32) * fo->layers[i].cap);
    fo->layer_count = 0;
}

void de__rw_foliage_free(de__render_world *rw) {
    de__rw_foliage *fo = &rw->foliage;
    de__rw_foliage_reset(rw);
    DE_FREE(rw->alloc, fo->layers, sizeof(de__rw_foliage_layer) * fo->layer_cap);
    fo->layers = NULL;
    fo->layer_cap = 0;
    de_asset_system *assets = rw->r != NULL ? rw->r->assets : NULL;
    for (u32 i = 0; i < DE__RW_FOLIAGE_TYPE_CACHE; ++i)
        if (assets != NULL && DE_HANDLE_VALID(fo->types[i].load))
            de_asset_release(assets, fo->types[i].load);
    memset(fo->types, 0, sizeof fo->types);
}
