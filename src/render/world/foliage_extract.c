/*
 * foliage_extract.c: DeFoliage instances into the render world (DENG-822).
 *
 * Every placed instance of a DeFoliage layer becomes one instance slot, so foliage takes the
 * mesh path and the GPU scene's instance table like any DeMeshRenderer (one draw per mesh batch,
 * GPU-driven culling). The instances are on DeFoliagePage entities (DENG-823), up to 512 per page
 * and any number of pages per 16 m cell; each page is tracked and rebuilt on its own, so a stroke
 * rebuilds only the pages it changed. The slots are detached: the entity map knows only the
 * layer's entity, which is every slot's owner (picking a blade of grass picks the layer). A page
 * whose layer is not alive draws nothing.
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
#include <stdlib.h>
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

/* ---------- Palettes ---------- */

/* The palettes of this frame's DeFoliage layers, sorted by entity, so a page finds its layer's. */
static void palette_chunk(const de_query_chunk *c, void *user) {
    extract_ctx *x = (extract_ctx *)user;
    de__rw_foliage *fo = &x->rw->foliage;
    const DeFoliage *layers = (const DeFoliage *)c->columns[0];
    for (u32 i = 0; i < c->count; ++i) {
        if (fo->palette_count == fo->palette_cap) {
            const u32 cap = fo->palette_cap ? fo->palette_cap * 2u : 8u;
            de__rw_foliage_palette *n = (de__rw_foliage_palette *)DE_REALLOC(
                x->rw->alloc, fo->palettes, sizeof *n * fo->palette_cap, sizeof *n * cap,
                DE_ALIGNOF(de__rw_foliage_palette), DE_MEM_RENDER);
            if (n == NULL)
                return; /* its pages draw nothing this frame */
            fo->palettes = n;
            fo->palette_cap = cap;
        }
        de__rw_foliage_palette *p = &fo->palettes[fo->palette_count++];
        p->entity = c->entities[i].bits;
        for (u32 k = 0; k < DE_FOLIAGE_TYPES; ++k)
            p->types[k] = DE_HANDLE_VALID(layers[i].types[k]) ? layers[i].types[k].id : 0u;
    }
}

static int cmp_palette(const void *a, const void *b) {
    const u64 x = ((const de__rw_foliage_palette *)a)->entity;
    const u64 y = ((const de__rw_foliage_palette *)b)->entity;
    return x < y ? -1 : x > y ? 1 : 0;
}

static const de__rw_foliage_palette *palette_find(const de__rw_foliage *fo, u64 entity) {
    u32 lo = 0, hi = fo->palette_count;
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2u;
        if (fo->palettes[mid].entity < entity)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo < fo->palette_count && fo->palettes[lo].entity == entity ? &fo->palettes[lo] : NULL;
}

/* ---------- Pages ---------- */

/* The entry of page `entity` (sorted by entity), added when `add`. */
static de__rw_foliage_page *page_of(de__render_world *rw, u64 entity, bool add) {
    de__rw_foliage *fo = &rw->foliage;
    u32 lo = 0, hi = fo->page_count;
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2u;
        if (fo->pages[mid].entity < entity)
            lo = mid + 1u;
        else
            hi = mid;
    }
    if (lo < fo->page_count && fo->pages[lo].entity == entity)
        return &fo->pages[lo];
    if (!add)
        return NULL;
    if (fo->page_count == fo->page_cap) {
        const u32 cap = fo->page_cap ? fo->page_cap * 2u : 16u;
        de__rw_foliage_page *n = (de__rw_foliage_page *)DE_REALLOC(
            rw->alloc, fo->pages, sizeof *n * fo->page_cap, sizeof *n * cap,
            DE_ALIGNOF(de__rw_foliage_page), DE_MEM_RENDER);
        if (n == NULL)
            return NULL;
        fo->pages = n;
        fo->page_cap = cap;
    }
    memmove(&fo->pages[lo + 1], &fo->pages[lo], (fo->page_count - lo) * sizeof fo->pages[0]);
    fo->page_count++;
    de__rw_foliage_page *l = &fo->pages[lo];
    memset(l, 0, sizeof *l);
    l->entity = entity;
    l->dirty = true;
    return l;
}

static bool page_reserve(de__render_world *rw, de__rw_foliage_page *l, u32 n) {
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

static void page_build(extract_ctx *x, de__rw_foliage_page *l, const DeFoliagePage *pg,
                       const de__rw_foliage_type *const types[], u32 mask, u64 sig) {
    de__render_world *rw = x->rw;
    u32 want = 0;
    for (u32 i = 0; i < pg->count; ++i)
        if (pg->instances[i].type < DE_FOLIAGE_TYPES && (mask >> pg->instances[i].type) & 1u)
            ++want;
    if (!page_reserve(rw, l, want)) {
        x->failed += want;
        want = 0;
    }
    const u32 old = l->count;
    u32 n = 0;
    bool need_bounds = false;
    for (u32 i = 0; i < pg->count && n < want; ++i) {
        const de_foliage_instance *in = &pg->instances[i];
        if (in->type >= DE_FOLIAGE_TYPES || !((mask >> in->type) & 1u))
            continue;
        u32 s = n < old ? l->slots[n] : DE__RW_NONE;
        if (s == DE__RW_NONE) {
            s = de__rw_pool_take_detached(rw, DE__RW_KIND_INSTANCE, (de_entity){l->owner});
            if (s == DE__RW_NONE) {
                ++x->failed;
                continue;
            }
        }
        rw->inst_pool.seen[s] = rw->frame;
        slot_write(rw, s, in, types[in->type]->mesh, &need_bounds);
        l->slots[n++] = s;
    }
    /* Slots n..old are not seen this frame: the sweep frees them. */
    l->count = n;
    l->sig = sig;
    l->dirty = false;
    l->need_bounds = need_bounds;
}

static void page_keep(de__render_world *rw, de__rw_foliage_page *l) {
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
        de__rw_foliage_page *l = page_of(x->rw, c->entities[i].bits, false);
        if (l != NULL)
            l->dirty = true;
    }
}

static void all_chunk(const de_query_chunk *c, void *user) {
    extract_ctx *x = (extract_ctx *)user;
    de__render_world *rw = x->rw;
    const de__rw_foliage *ff = &rw->foliage;
    const DeFoliagePage *pages = (const DeFoliagePage *)c->columns[0];
    for (u32 i = 0; i < c->count; ++i) {
        const DeFoliagePage *pg = &pages[i];
        de__rw_foliage_page *l = page_of(rw, c->entities[i].bits, true);
        if (l == NULL) {
            x->failed += pg->count <= DE_FOLIAGE_PAGE_INSTANCES ? pg->count : 0u;
            continue;
        }
        l->seen = rw->frame;
        u64 sig = mix(0xcbf29ce484222325ull, ff->type_gen);
        sig = mix(sig, pg->layer.bits);
        const de__rw_foliage_palette *pal = palette_find(ff, pg->layer.bits);
        if (pal == NULL || pg->count > DE_FOLIAGE_PAGE_INSTANCES) {
            /* No live layer, or a bad page: nothing drawn rather than reading past it. */
            sig = mix(sig, 0xbadull);
            if (l->sig != sig || l->dirty) {
                l->count = 0;
                l->sig = sig;
                l->dirty = false;
            }
            continue;
        }
        if (l->owner != pg->layer.bits) {
            l->owner = pg->layer.bits; /* slots of another owner are rebuilt */
            l->dirty = true;
        }
        const de__rw_foliage_type *types[DE_FOLIAGE_TYPES];
        for (u32 k = 0; k < DE_FOLIAGE_TYPES; ++k) {
            types[k] = pal->types[k] != 0 ? type_find(&rw->foliage, pal->types[k]) : NULL;
            sig = mix(sig, pal->types[k]);
        }
        const u32 mask = cell_mask(ff, types, cell_distance(ff, pg->x, pg->z));
        sig = mix(sig, ((u64)(u32)pg->x << 32) | (u32)pg->z);
        sig = mix(sig, ((u64)pg->count << 32) | mask);
        sig = mix(sig, pg->revision);
        if (l->dirty || l->sig != sig)
            page_build(x, l, pg, types, mask, sig);
        else
            page_keep(rw, l);
    }
}

u32 de__rw_foliage_extract(de__render_world *rw, de_world *w, de_query layers, de_query all,
                           de_query changed) {
    extract_ctx x = {rw, 0};
    de__rw_foliage *fo = &rw->foliage;
    fo->palette_count = 0;
    de_query_each(w, layers, palette_chunk, &x);
    if (fo->palette_count > 1)
        qsort(fo->palettes, fo->palette_count, sizeof fo->palettes[0], cmp_palette);
    de_query_each(w, changed, changed_chunk, &x);
    de_query_each(w, all, all_chunk, &x);
    /* Pages whose entity is gone, or no longer has DeFoliagePage: their slots are swept. */
    u32 kept = 0;
    for (u32 i = 0; i < fo->page_count; ++i) {
        if (fo->pages[i].seen == rw->frame) {
            fo->pages[kept++] = fo->pages[i];
            continue;
        }
        DE_FREE(rw->alloc, fo->pages[i].slots, sizeof(u32) * fo->pages[i].cap);
    }
    fo->page_count = kept;
    return x.failed;
}

void de__rw_foliage_reset(de__render_world *rw) {
    de__rw_foliage *fo = &rw->foliage;
    for (u32 i = 0; i < fo->page_count; ++i)
        DE_FREE(rw->alloc, fo->pages[i].slots, sizeof(u32) * fo->pages[i].cap);
    fo->page_count = 0;
}

void de__rw_foliage_free(de__render_world *rw) {
    de__rw_foliage *fo = &rw->foliage;
    de__rw_foliage_reset(rw);
    DE_FREE(rw->alloc, fo->pages, sizeof(de__rw_foliage_page) * fo->page_cap);
    fo->pages = NULL;
    fo->page_cap = 0;
    DE_FREE(rw->alloc, fo->palettes, sizeof(de__rw_foliage_palette) * fo->palette_cap);
    fo->palettes = NULL;
    fo->palette_cap = 0;
    fo->palette_count = 0;
    de_asset_system *assets = rw->r != NULL ? rw->r->assets : NULL;
    for (u32 i = 0; i < DE__RW_FOLIAGE_TYPE_CACHE; ++i)
        if (assets != NULL && DE_HANDLE_VALID(fo->types[i].load))
            de_asset_release(assets, fo->types[i].load);
    memset(fo->types, 0, sizeof fo->types);
}
