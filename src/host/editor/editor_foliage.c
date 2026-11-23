/* editor_foliage.c: foliage.info, foliage.instances, foliage.type and the foliage.* edit ops
 * (DENG-581; DIDE protocol 1.22, capability world.foliage).
 *
 * A foliage layer is an entity with DeFoliage (a render component): a palette of foliageType
 * assets (.foliage.toml) and scatter rules. Its instances are kept per 16 m cell on DeFoliagePage
 * entities (DENG-823): a cell of n instances has pages 0 .. ceil(n / 512) - 1, each naming the layer.
 * The pages join the layer's scene (so a save keeps them) but the IDE never lists them. The IDE
 * reads a layer with
 * the three methods and changes it with four edit ops inside an edit transaction, so undo and redo
 * work as for every other edit:
 *
 *   - foliage.paint, foliage.erase: `value` a FoliageStroke;
 *   - foliage.scatter: `value` a FoliageScatter (optional: every enabled rule);
 *   - foliage.restore: `value` {cells: [{cell: [x, z], instances: [x, y, z, yaw, scale, type,
 *     rule, ...]}]}, the cells another op changed, as they were.
 *
 * The inverse of each is a foliage.restore that holds only the cells the op changed. A changed cell
 * takes the entity revision after the op. The math is the deterministic CPU model in
 * src/world/foliage; this file reads the protocol, the type assets and the undo data.
 *
 * A paint surface is a terrain (DeTerrain), or any other entity as a horizontal plane at its world
 * height. A cell holds at most DE_FOLIAGE_CELL_INSTANCES (max_per_cell), a layer at most
 * DE_FOLIAGE_LAYER_INSTANCES. Each op loads the layer's pages into a de_foliage_store, runs the
 * model on a copy, and writes back only the cells that changed.
 */
#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS /* snprintf */
#endif
#include "host/editor/editor_internal.h"

#include "world/foliage/foliage.h"
#include "world/terrain/terrain.h"
#include <dengine/de_memory.h>
#include <dengine/de_render.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SURFACES 16u

static de_world *world_of(de__editor *ed) {
    return ed->e != NULL ? ed->e->world : NULL;
}

/* A render component of the world, registered on first use (the world may be replaced). */
static de_component comp_id(de_world *w, const char *name) {
    if (w == NULL)
        return (de_component){0};
    de_component c = de_component_find(w, name);
    if (!DE_HANDLE_VALID(c) && de_render_register_components(w) == DE_OK)
        c = de_component_find(w, name);
    return c;
}

static const DeFoliage *foliage_get(de__editor *ed, u64 bits) {
    de_world *w = world_of(ed);
    const de_component c = comp_id(w, DE_FOLIAGE_COMPONENT);
    const de_entity e = {bits};
    if (!DE_HANDLE_VALID(c) || !de_entity_alive(w, e))
        return NULL;
    return (const DeFoliage *)de_entity_get_const(w, e, c);
}

/* ---------- Pages ---------- */

/* One DeFoliagePage of a layer. */
typedef struct page_ref {
    u64 bits;
    i32 x, z;
    u32 page;
} page_ref;

/* The pages of one layer, sorted by z, x, then page. */
typedef struct page_list {
    page_ref *v;
    u32 n, cap;
    u64 layer;
    bool oom;
} page_list;

static void pages_free(page_list *l) {
    DE_FREE(de_heap_allocator(), l->v, sizeof(page_ref) * l->cap);
    memset(l, 0, sizeof *l);
}

static void page_chunk(const de_query_chunk *ch, void *user) {
    page_list *l = (page_list *)user;
    const DeFoliagePage *p = (const DeFoliagePage *)ch->columns[0];
    for (u32 r = 0; r < ch->count && !l->oom; ++r) {
        if (p[r].layer.bits != l->layer)
            continue;
        if (l->n == l->cap) {
            const u32 cap = l->cap ? l->cap * 2u : 64u;
            page_ref *v = (page_ref *)DE_REALLOC(de_heap_allocator(), l->v,
                                                 sizeof *v * l->cap, sizeof *v * cap,
                                                 DE_ALIGNOF(page_ref), DE_MEM_GENERAL);
            if (v == NULL) {
                l->oom = true;
                return;
            }
            l->v = v;
            l->cap = cap;
        }
        l->v[l->n++] = (page_ref){ch->entities[r].bits, p[r].x, p[r].z, p[r].page};
    }
}

static int cmp_xz(i32 ax, i32 az, i32 bx, i32 bz) {
    if (az != bz)
        return az < bz ? -1 : 1;
    return ax < bx ? -1 : ax > bx ? 1 : 0;
}

static int cmp_page(const void *pa, const void *pb) {
    const page_ref *a = (const page_ref *)pa, *b = (const page_ref *)pb;
    const int c = cmp_xz(a->x, a->z, b->x, b->z);
    if (c != 0)
        return c;
    if (a->page != b->page)
        return a->page < b->page ? -1 : 1;
    return a->bits < b->bits ? -1 : a->bits > b->bits ? 1 : 0;
}

/* The first page of cell (x, z) in `l`, or where it would go. */
static u32 pages_find(const page_list *l, i32 x, i32 z) {
    u32 lo = 0, hi = l->n;
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2u;
        if (cmp_xz(l->v[mid].x, l->v[mid].z, x, z) < 0)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo;
}

typedef enum load_rc { LOAD_OK, LOAD_BAD, LOAD_OOM } load_rc;

/* Loads the pages of `layer` into `l` and `st` (both empty). LOAD_BAD: the pages are not the
 * pages of a valid cell table (a missing or doubled page, a page not full before the last, too
 * many instances). */
static load_rc layer_load(de__editor *ed, u64 layer, page_list *l, de_foliage_store *st) {
    memset(l, 0, sizeof *l);
    l->layer = layer;
    de_world *w = world_of(ed);
    const de_component pc = comp_id(w, DE_FOLIAGE_PAGE_COMPONENT);
    if (w == NULL || !DE_HANDLE_VALID(pc))
        return LOAD_OK;
    const de_query_term terms[1] = {{pc, DE_READ}};
    const de_query_desc qd = {terms, 1, NULL, 0};
    const de_query q = de_query_create(w, &qd);
    de_query_each(w, q, page_chunk, l);
    de_query_destroy(w, q);
    if (l->oom)
        return LOAD_OOM;
    if (l->n > 1)
        qsort(l->v, l->n, sizeof l->v[0], cmp_page);
    for (u32 i = 0; i < l->n;) {
        u32 j = i;
        while (j < l->n && l->v[j].x == l->v[i].x && l->v[j].z == l->v[i].z)
            ++j;
        for (u32 k = i; k < j; ++k) {
            const DeFoliagePage *p =
                (const DeFoliagePage *)de_entity_get_const(w, (de_entity){l->v[k].bits}, pc);
            if (p == NULL || p->page != k - i || p->count == 0 ||
                p->count > DE_FOLIAGE_PAGE_INSTANCES ||
                (k + 1 < j && p->count != DE_FOLIAGE_PAGE_INSTANCES))
                return LOAD_BAD;
            if (!de__foliage_store_append(st, p->x, p->z, p->revision, p->instances, p->count))
                return LOAD_BAD;
        }
        i = j;
    }
    return de__foliage_valid(st) ? LOAD_OK : LOAD_BAD;
}

/* Writes cell (x, z) of `st` to the layer's pages: the cell's pages in `l` are reused in order,
 * missing ones spawned into the layer's scene, surplus ones despawned. False when out of memory. */
static bool cell_write(de__editor *ed, de_entity layer, const page_list *l,
                       const de_foliage_store *st, i32 x, i32 z, u64 rev) {
    de_world *w = world_of(ed);
    const de_component pc = comp_id(w, DE_FOLIAGE_PAGE_COMPONENT);
    u32 n = 0;
    const de_foliage_instance *in = de__foliage_cell(st, x, z, &n);
    const u32 first = pages_find(l, x, z);
    u32 have = 0;
    while (first + have < l->n && l->v[first + have].x == x && l->v[first + have].z == z)
        ++have;
    const u32 need = (n + DE_FOLIAGE_PAGE_INSTANCES - 1u) / DE_FOLIAGE_PAGE_INSTANCES;
    for (u32 p = 0; p < need; ++p) {
        DeFoliagePage *pg;
        if (p < have) {
            pg = (DeFoliagePage *)de_entity_get(w, (de_entity){l->v[first + p].bits}, pc);
        } else {
            const de_entity e = de_entity_spawn(w);
            pg = e.bits != 0 ? (DeFoliagePage *)de_entity_add(w, e, pc) : NULL;
            if (pg == NULL) {
                if (e.bits != 0)
                    de_entity_despawn(w, e);
                return false;
            }
            de__ed_scene_adopt(ed, e, 0, layer);
        }
        if (pg == NULL)
            return false;
        const u32 at = p * DE_FOLIAGE_PAGE_INSTANCES;
        const u32 count = n - at < DE_FOLIAGE_PAGE_INSTANCES ? n - at : DE_FOLIAGE_PAGE_INSTANCES;
        memset(pg, 0, sizeof *pg);
        pg->layer = layer;
        pg->x = x;
        pg->z = z;
        pg->page = p;
        pg->count = count;
        pg->revision = rev;
        memcpy(pg->instances, &in[at], sizeof in[0] * count);
    }
    for (u32 p = need; p < have; ++p) {
        de__ed_scene_member_drop(l->v[first + p].bits);
        de_entity_despawn(w, (de_entity){l->v[first + p].bits});
    }
    return true;
}

/* The layer of `bits` with its instances in `st`, or answers the request with an error and
 * returns NULL (`st` then empty). */
static const DeFoliage *foliage_of(de__editor *ed, mpack_writer_t *w, u64 id, u64 bits,
                                   const char *method, de_foliage_store *st) {
    de__foliage_store_init(st, NULL);
    const DeFoliage *fo = foliage_get(ed, bits);
    if (fo == NULL) {
        de__ed_error(w, id, DE_EP_ERR_NOT_FOUND, "%s: entity %llu has no " DE_FOLIAGE_COMPONENT,
                     method, (unsigned long long)bits);
        return NULL;
    }
    page_list l;
    const load_rc rc = layer_load(ed, bits, &l, st);
    pages_free(&l);
    if (rc != LOAD_OK) {
        de__foliage_store_free(st);
        if (rc == LOAD_OOM)
            de__ed_error(w, id, DE_EP_ERR_BUSY, "%s: out of memory", method);
        else
            de__ed_error(w, id, DE_EP_ERR_INVALID_PARAMS,
                         "%s: the foliage cell table is inconsistent", method);
        return NULL;
    }
    return fo;
}

/* ---------- Types and surfaces ---------- */

static de_foliage_error fail(de_foliage_error e, char *msg, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, cap, fmt, ap);
    va_end(ap);
    return e;
}

/* Reads and parses foliage type asset `id`: DE_FOLIAGE_NOT_FOUND when it has no source. */
static de_foliage_error load_type(de__editor *ed, const char *id, de_foliage_type *out, char *msg,
                                  size_t cap) {
    void *src = NULL;
    u64 size = 0;
    if (!de__ed_asset_source(ed, id, &src, &size))
        return fail(DE_FOLIAGE_NOT_FOUND, msg, cap, "unknown foliage type %s", id);
    const bool ok = de__foliage_parse_type(id, (const char *)src, (size_t)size, out, msg, cap);
    DE_FREE(de_heap_allocator(), src, size + 1);
    return ok ? DE_FOLIAGE_OK : DE_FOLIAGE_INVALID;
}

typedef struct env_ctx {
    de__editor *ed;
    const DeFoliage *fo;
} env_ctx;

static de_foliage_error env_type(void *user, u32 slot, de_foliage_type *out, char *msg,
                                 size_t cap) {
    const env_ctx *c = (const env_ctx *)user;
    if (slot >= DE_FOLIAGE_TYPES || !DE_HANDLE_VALID(c->fo->types[slot]))
        return fail(DE_FOLIAGE_INVALID, msg, cap, "palette entry %u is empty", slot);
    const char *id = de__ed_asset_path(c->ed, c->fo->types[slot].id);
    if (id == NULL)
        return fail(DE_FOLIAGE_INVALID, msg, cap, "unknown foliage type in palette entry %u",
                    slot);
    /* An unknown type in the palette is bad layer data, not a missing request target. */
    const de_foliage_error e = load_type(c->ed, id, out, msg, cap);
    return e == DE_FOLIAGE_OK ? e : DE_FOLIAGE_INVALID;
}

static de_foliage_error env_surface(void *user, de_entity e, de_foliage_surface *out, char *msg,
                                    size_t cap) {
    const env_ctx *c = (const env_ctx *)user;
    de_world *w = world_of(c->ed);
    if (w == NULL || !de_entity_alive(w, e))
        return fail(DE_FOLIAGE_NOT_FOUND, msg, cap, "surface %llu", (unsigned long long)e.bits);
    const de_transform x = de__ed_world_xf(w, e);
    out->ox = (f64)x.position.x;
    out->oy = (f64)x.position.y;
    out->oz = (f64)x.position.z;
    out->terrain = NULL;
    const de_component tc = comp_id(w, DE_TERRAIN_COMPONENT);
    const DeTerrain *t =
        DE_HANDLE_VALID(tc) ? (const DeTerrain *)de_entity_get_const(w, e, tc) : NULL;
    if (t != NULL) {
        if (!de__terrain_valid(t))
            return fail(DE_FOLIAGE_INVALID, msg, cap, "surface %llu: the terrain grid is not %ux%u",
                        (unsigned long long)e.bits, DE_TERRAIN_RES, DE_TERRAIN_RES);
        out->terrain = t;
    }
    return DE_FOLIAGE_OK;
}

static de_ep_error code_of(de_foliage_error e) {
    return e == DE_FOLIAGE_NOT_FOUND ? DE_EP_ERR_NOT_FOUND : DE_EP_ERR_INVALID_PARAMS;
}

/* ---------- Methods ---------- */

static void foliage_info(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                         mpack_writer_t *w) {
    DE__ED_PARAMS(foliage_info, de_ep_foliage_params, q);
    de_foliage_store st;
    if (foliage_of(ed, w, req->id, q.entity, "foliage.info", &st) == NULL)
        return;
    const u32 nc = st.cell_count;
    de_ep_foliage_cell *cells =
        nc ? (de_ep_foliage_cell *)de__ed_alloc(a, sizeof(de_ep_foliage_cell) * nc) : NULL;
    i32 *xz = nc ? (i32 *)de__ed_alloc(a, sizeof(i32) * 2u * nc) : NULL;
    if (nc > 0 && (cells == NULL || xz == NULL)) {
        de__foliage_store_free(&st);
        de__ed_error(w, req->id, DE_EP_ERR_BUSY, "foliage.info: out of scratch memory");
        return;
    }
    for (u32 i = 0; i < nc; ++i) {
        xz[2 * i] = st.cells[i].x;
        xz[2 * i + 1] = st.cells[i].z;
        cells[i].cell = (de_ep_list_i32){&xz[2 * i], 2};
        cells[i].count = st.cells[i].count;
        cells[i].revision = st.cells[i].revision;
    }
    de_ep_foliage_info r;
    memset(&r, 0, sizeof r);
    r.entity = q.entity;
    r.component = de_ep_cstr(DE_FOLIAGE_COMPONENT);
    r.cell_size = DE_FOLIAGE_CELL_SIZE;
    r.max_types = DE_FOLIAGE_TYPES;
    r.max_rules = DE_FOLIAGE_RULES;
    r.max_per_cell = DE_FOLIAGE_CELL_INSTANCES;
    r.type_suffix = de_ep_cstr(DE_FOLIAGE_SUFFIX);
    r.count = st.instance_count;
    r.cells = (de_ep_list_foliage_cell){cells, nc};
    r.revision = de__ed_entity_rev(ed, (de_entity){q.entity});
    de_ep_write_response_foliage_info(w, req->id, &r);
    de__foliage_store_free(&st);
}

static void foliage_instances(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                              mpack_writer_t *w) {
    DE__ED_PARAMS(foliage_instances, de_ep_foliage_cell_params, q);
    if (q.cell.count != 2) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "foliage.instances: cell needs [x, z]");
        return;
    }
    de_foliage_store st;
    if (foliage_of(ed, w, req->id, q.entity, "foliage.instances", &st) == NULL)
        return;
    static i32 xz[2];
    xz[0] = q.cell.items[0];
    xz[1] = q.cell.items[1];
    const de_foliage_cell *cell = de__foliage_store_cell(&st, xz[0], xz[1]);
    const u32 n = cell != NULL ? cell->count : 0;
    u32 *types = n ? (u32 *)de__ed_alloc(a, sizeof(u32) * n) : NULL;
    u32 *rules = n ? (u32 *)de__ed_alloc(a, sizeof(u32) * n) : NULL;
    f32 *xf = n ? (f32 *)de__ed_alloc(a, sizeof(f32) * 10u * n) : NULL;
    if (n > 0 && (types == NULL || rules == NULL || xf == NULL)) {
        de__foliage_store_free(&st);
        de__ed_error(w, req->id, DE_EP_ERR_BUSY, "foliage.instances: out of scratch memory");
        return;
    }
    u32 k = 0;
    for (u32 i = 0; i < n; ++i) {
        const de_foliage_instance *in = &cell->inst[i];
        if (q.has_type && in->type != q.type)
            continue;
        types[k] = in->type;
        rules[k] = in->rule;
        de__foliage_transform(in, &xf[k * 10]);
        ++k;
    }
    de_ep_foliage_instances r;
    memset(&r, 0, sizeof r);
    r.cell = (de_ep_list_i32){xz, 2};
    r.revision = cell != NULL ? cell->revision : 0;
    r.types = (de_ep_list_u32){types, k};
    r.rules = (de_ep_list_u32){rules, k};
    r.transforms = (de_ep_list_f32){xf, k * 10};
    de_ep_write_response_foliage_instances(w, req->id, &r);
    de__foliage_store_free(&st);
}

static void foliage_type(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                         mpack_writer_t *w) {
    DE__ED_PARAMS(foliage_type, de_ep_foliage_type_params, q);
    char id[DE_FOLIAGE_ID_MAX];
    if (q.id.len == 0 || q.id.len >= sizeof id) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "foliage.type: bad id");
        return;
    }
    memcpy(id, q.id.ptr, q.id.len);
    id[q.id.len] = '\0';
    static de_foliage_type t;
    char msg[256];
    const de_foliage_error e = load_type(ed, id, &t, msg, sizeof msg);
    if (e != DE_FOLIAGE_OK) {
        de__ed_error(w, req->id, code_of(e), "foliage.type: %s", msg);
        return;
    }
    de_ep_foliage_type r;
    memset(&r, 0, sizeof r);
    r.id = de_ep_cstr(t.id);
    r.mesh = de_ep_cstr(t.mesh);
    r.has_impostor = t.has_impostor;
    r.impostor = de_ep_cstr(t.has_impostor ? t.impostor : "");
    r.has_impostor_distance = t.has_impostor_distance;
    r.impostor_distance = t.impostor_distance;
    r.density = t.density;
    r.scale = (de_ep_list_f32){t.scale, 2};
    r.rotation = (de_ep_list_f32){t.rotation, 2};
    r.align_to_normal = t.align_to_normal;
    r.slope = (de_ep_list_f32){t.slope, 2};
    r.height = (de_ep_list_f32){t.height, 2};
    r.min_spacing = t.min_spacing;
    r.collision = t.collision;
    r.cull_distance = t.cull_distance;
    de_ep_write_response_foliage_type(w, req->id, &r);
}

/* ---------- Edit ops ---------- */

typedef struct op_fail {
    de_ep_error *code;
    char *msg;
    size_t cap;
} op_fail;

static bool op_error(op_fail *f, de_ep_error code, const char *fmt, ...) {
    *f->code = code;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(f->msg, f->cap, fmt, ap);
    va_end(ap);
    return false;
}

static bool num_at(mpack_node_t list, size_t i, f64 *out) {
    const mpack_node_t n = mpack_node_array_at(list, i);
    switch (mpack_node_type(n)) {
    case mpack_type_double: *out = mpack_node_double(n); break;
    case mpack_type_float: *out = (f64)mpack_node_float(n); break;
    case mpack_type_int: *out = (f64)mpack_node_i64(n); break;
    case mpack_type_uint: *out = (f64)mpack_node_u64(n); break;
    default: return false;
    }
    return *out == *out && *out - *out == 0.0;
}

static bool int_at(mpack_node_t list, size_t i, i32 *out) {
    f64 v;
    if (!num_at(list, i, &v) || v != (f64)(i64)v || v < -2147483648.0 || v > 2147483647.0)
        return false;
    *out = (i32)v;
    return true;
}

/* Applies a foliage.restore value to `st`. 0: done; 1: bad data; 2: no room. */
static int restore(de_foliage_store *st, de_ep_raw value) {
    mpack_tree_t tree;
    mpack_tree_init_data(&tree, value.ptr, value.len);
    mpack_tree_parse(&tree);
    int rc = mpack_tree_error(&tree) == mpack_ok ? 0 : 1;
    const mpack_node_t root = rc == 0 ? mpack_tree_root(&tree) : (mpack_node_t){0};
    mpack_node_t cells = {0};
    if (rc == 0 && mpack_node_type(root) == mpack_type_map)
        cells = mpack_node_map_cstr_optional(root, "cells");
    if (rc == 0 && mpack_node_type(cells) != mpack_type_array)
        rc = 1;
    const size_t cell_count = rc == 0 ? mpack_node_array_length(cells) : 0;
    de_foliage_instance *inst = NULL;
    size_t inst_cap = 0;
    for (size_t k = 0; rc == 0 && k < cell_count; ++k) {
        const mpack_node_t c = mpack_node_array_at(cells, k);
        if (mpack_node_type(c) != mpack_type_map) {
            rc = 1;
            break;
        }
        const mpack_node_t xz = mpack_node_map_cstr_optional(c, "cell");
        const mpack_node_t list = mpack_node_map_cstr_optional(c, "instances");
        i32 x = 0, z = 0;
        if (mpack_node_type(xz) != mpack_type_array || mpack_node_array_length(xz) < 2 ||
            !int_at(xz, 0, &x) || !int_at(xz, 1, &z) ||
            mpack_node_type(list) != mpack_type_array ||
            mpack_node_array_length(list) % DE_FOLIAGE_STRIDE != 0) {
            rc = 1;
            break;
        }
        const size_t n = mpack_node_array_length(list) / DE_FOLIAGE_STRIDE;
        if (n > DE_FOLIAGE_CELL_INSTANCES) {
            rc = 2;
            break;
        }
        if (n > inst_cap) {
            de_foliage_instance *p = (de_foliage_instance *)DE_REALLOC(
                de_heap_allocator(), inst, sizeof *inst * inst_cap, sizeof *inst * n,
                DE_ALIGNOF(de_foliage_instance), DE_MEM_GENERAL);
            if (p == NULL) {
                rc = 2;
                break;
            }
            inst = p;
            inst_cap = n;
        }
        for (size_t i = 0; rc == 0 && i < n; ++i) {
            f64 v[DE_FOLIAGE_STRIDE];
            for (u32 j = 0; rc == 0 && j < DE_FOLIAGE_STRIDE; ++j)
                if (!num_at(list, i * DE_FOLIAGE_STRIDE + j, &v[j]))
                    rc = 1;
            if (rc != 0 || v[5] < 0.0 || v[5] >= (f64)DE_FOLIAGE_TYPES || v[6] < 0.0 ||
                v[6] > 4294967295.0) {
                rc = 1;
                break;
            }
            memset(&inst[i], 0, sizeof inst[i]);
            inst[i].x = (f32)v[0];
            inst[i].y = (f32)v[1];
            inst[i].z = (f32)v[2];
            inst[i].yaw = (f32)v[3];
            inst[i].scale = (f32)v[4];
            inst[i].type = (u8)v[5];
            inst[i].rule = (u32)v[6];
        }
        if (rc == 0 && !de__foliage_set_cell(st, x, z, inst, (u32)n))
            rc = 2;
    }
    DE_FREE(de_heap_allocator(), inst, sizeof *inst * inst_cap);
    if (mpack_tree_destroy(&tree) != mpack_ok && rc == 0)
        rc = 1;
    return rc;
}

static void write_cell(mpack_writer_t *mw, const de_foliage_store *st, i32 x, i32 z) {
    u32 n = 0;
    const de_foliage_instance *in = de__foliage_cell(st, x, z, &n);
    mpack_start_map(mw, 2);
    mpack_write_cstr(mw, "cell");
    mpack_start_array(mw, 2);
    mpack_write_i32(mw, x);
    mpack_write_i32(mw, z);
    mpack_finish_array(mw);
    mpack_write_cstr(mw, "instances");
    mpack_start_array(mw, n * DE_FOLIAGE_STRIDE);
    for (u32 i = 0; i < n; ++i) {
        mpack_write_double(mw, (f64)in[i].x);
        mpack_write_double(mw, (f64)in[i].y);
        mpack_write_double(mw, (f64)in[i].z);
        mpack_write_double(mw, (f64)in[i].yaw);
        mpack_write_double(mw, (f64)in[i].scale);
        mpack_write_double(mw, (f64)in[i].type);
        mpack_write_double(mw, (f64)in[i].rule);
    }
    mpack_finish_array(mw);
    mpack_finish_map(mw);
}

/* The cells of `a` and `b` that differ, sorted by z then x, in `out` (heap; `*n` of them). False
 * when out of memory. */
static bool changed_cells(const de_foliage_store *a, const de_foliage_store *b, i32 (**out)[2],
                          u32 *n) {
    const u32 cap = a->cell_count + b->cell_count;
    *n = 0;
    *out = NULL;
    if (cap == 0)
        return true;
    i32(*keys)[2] = (i32(*)[2])DE_ALLOC(de_heap_allocator(), sizeof(i32[2]) * cap,
                                        DE_ALIGNOF(i32), DE_MEM_GENERAL);
    if (keys == NULL)
        return false;
    u32 i = 0, j = 0;
    while (i < a->cell_count || j < b->cell_count) {
        const de_foliage_cell *ca = i < a->cell_count ? &a->cells[i] : NULL;
        const de_foliage_cell *cb = j < b->cell_count ? &b->cells[j] : NULL;
        const de_foliage_cell *pick =
            cb == NULL || (ca != NULL && cmp_xz(ca->x, ca->z, cb->x, cb->z) <= 0) ? ca : cb;
        const i32 x = pick->x, z = pick->z;
        if (ca != NULL && ca->x == x && ca->z == z)
            ++i;
        if (cb != NULL && cb->x == x && cb->z == z)
            ++j;
        if (!de__foliage_cell_equal(a, b, x, z)) {
            keys[*n][0] = x;
            keys[*n][1] = z;
            ++*n;
        }
    }
    *out = keys;
    return true;
}

/* The foliage.restore value that turns `after` back into `before`: the `n` changed cells `keys`,
 * as `before` has them. Empty when it does not fit the arena. */
static de_ep_raw restore_from(de_ep_arena *a, const de_foliage_store *before, i32 (*keys)[2],
                              u32 n) {
    mpack_writer_t mw;
    de__ed_raw_begin(a, &mw);
    mpack_start_map(&mw, 1);
    mpack_write_cstr(&mw, "cells");
    mpack_start_array(&mw, n);
    for (u32 k = 0; k < n; ++k)
        write_cell(&mw, before, keys[k][0], keys[k][1]);
    mpack_finish_array(&mw);
    mpack_finish_map(&mw);
    return de__ed_raw_end(a, &mw);
}

static bool read_stroke(de_ep_arena *a, const de_ep_edit_op *o, de_ep_foliage_stroke *p,
                        op_fail *f) {
    if (!o->has_value)
        return op_error(f, DE_EP_ERR_INVALID_PARAMS, "%.*s needs value", (int)o->op.len,
                        o->op.ptr);
    memset(p, 0, sizeof *p);
    mpack_reader_t r;
    mpack_reader_init_data(&r, o->value.ptr, o->value.len);
    de_ep_read_foliage_stroke(&r, a, p);
    if (mpack_reader_destroy(&r) != mpack_ok)
        return op_error(f, DE_EP_ERR_INVALID_PARAMS, "%.*s: bad value", (int)o->op.len,
                        o->op.ptr);
    return true;
}

static de_foliage_stroke stroke_of(const de_ep_foliage_stroke *p) {
    de_foliage_stroke s;
    memset(&s, 0, sizeof s);
    s.has_type = p->has_type;
    s.type = p->type;
    s.points = p->points.items;
    s.point_count = p->points.count;
    s.radius = p->radius;
    s.strength = p->strength;
    s.falloff = p->falloff.items;
    s.falloff_count = p->falloff.count;
    s.has_density = p->has_density;
    s.density = p->density;
    s.seed = p->has_seed ? p->seed : 0u;
    return s;
}

static bool model_ok(de_foliage_error e, op_fail *f) {
    if (e == DE_FOLIAGE_OK)
        return true;
    *f->code = code_of(e);
    return false;
}

static bool op_paint(de__editor *ed, de_ep_arena *a, const de_ep_edit_op *o, de_foliage_store *st,
                     const env_ctx *ctx, op_fail *f) {
    de_ep_foliage_stroke p;
    if (!read_stroke(a, o, &p, f))
        return false;
    const u32 ns = p.has_surfaces ? p.surfaces.count : 0;
    if (ns > MAX_SURFACES)
        return op_error(f, DE_EP_ERR_INVALID_PARAMS, "foliage.paint: at most %u surfaces",
                        MAX_SURFACES);
    de_foliage_surface surfaces[MAX_SURFACES];
    for (u32 i = 0; i < ns; ++i) {
        const de_foliage_error e = env_surface((void *)ctx, (de_entity){p.surfaces.items[i]},
                                               &surfaces[i], f->msg, f->cap);
        if (e != DE_FOLIAGE_OK)
            return model_ok(e, f);
    }
    (void)ed;
    const de_foliage_env env = {(void *)ctx, env_type, env_surface};
    const de_foliage_stroke s = stroke_of(&p);
    return model_ok(de__foliage_paint(st, &env, &s, surfaces, ns, f->msg, f->cap), f);
}

static bool op_erase(de_ep_arena *a, const de_ep_edit_op *o, de_foliage_store *st, op_fail *f) {
    de_ep_foliage_stroke p;
    if (!read_stroke(a, o, &p, f))
        return false;
    const de_foliage_stroke s = stroke_of(&p);
    return model_ok(de__foliage_erase(st, &s, f->msg, f->cap), f);
}

static bool op_scatter(de_ep_arena *a, const de_ep_edit_op *o, de_foliage_store *st,
                       const env_ctx *ctx, op_fail *f) {
    de_ep_foliage_scatter p;
    memset(&p, 0, sizeof p);
    if (o->has_value) {
        mpack_reader_t r;
        mpack_reader_init_data(&r, o->value.ptr, o->value.len);
        de_ep_read_foliage_scatter(&r, a, &p);
        if (mpack_reader_destroy(&r) != mpack_ok)
            return op_error(f, DE_EP_ERR_INVALID_PARAMS, "foliage.scatter: bad value");
    }
    static const u32 none = 0;
    const u32 *ids = p.has_rules ? (p.rules.count > 0 ? p.rules.items : &none) : NULL;
    const de_foliage_env env = {(void *)ctx, env_type, env_surface};
    return model_ok(de__foliage_scatter(st, ctx->fo->rules, &env, ids,
                                        p.has_rules ? p.rules.count : 0, p.has_clear && p.clear,
                                        f->msg, f->cap),
                    f);
}

/* The terrains of the world, as surfaces (at most MAX_SURFACES). */
typedef struct terrain_scan {
    de_entity e[MAX_SURFACES];
    u32 n;
} terrain_scan;

static void terrain_chunk(const de_query_chunk *ch, void *user) {
    terrain_scan *s = (terrain_scan *)user;
    for (u32 r = 0; r < ch->count && s->n < MAX_SURFACES; ++r)
        s->e[s->n++] = ch->entities[r];
}

/* DENG-822: the undo data has no normals, so a restore gives the restored alignToNormal instances
 * the normal of the terrain they stand on again. */
static void realign(de__editor *ed, de_foliage_store *st, const env_ctx *ctx) {
    de_world *w = world_of(ed);
    const de_component tc = comp_id(w, DE_TERRAIN_COMPONENT);
    if (w == NULL || !DE_HANDLE_VALID(tc))
        return;
    terrain_scan scan = {.n = 0};
    const de_query_term terms[1] = {{tc, DE_READ}};
    const de_query_desc qd = {terms, 1, NULL, 0};
    const de_query q = de_query_create(w, &qd);
    de_query_each(w, q, terrain_chunk, &scan);
    de_query_destroy(w, q);
    de_foliage_surface surfaces[MAX_SURFACES];
    u32 n = 0;
    char msg[8];
    for (u32 i = 0; i < scan.n; ++i)
        if (env_surface((void *)ctx, scan.e[i], &surfaces[n], msg, sizeof msg) == DE_FOLIAGE_OK &&
            surfaces[n].terrain != NULL)
            n++;
    const de_foliage_env env = {(void *)ctx, env_type, env_surface};
    de__foliage_realign(st, &env, surfaces, n);
}

static bool foliage_op(de__editor *ed, de_ep_arena *a, const de_ep_edit_op *o, de_ep_edit_op *inv,
                       de_ep_error *code, char *msg, size_t cap) {
    op_fail f = {code, msg, cap};
    if (!o->has_entity)
        return op_error(&f, DE_EP_ERR_INVALID_PARAMS, "%.*s needs entity", (int)o->op.len,
                        o->op.ptr);
    de_world *w = world_of(ed);
    const de_component c = comp_id(w, DE_FOLIAGE_COMPONENT);
    const de_entity e = {o->entity};
    const DeFoliage *live = DE_HANDLE_VALID(c) && de_entity_alive(w, e)
                                ? (const DeFoliage *)de_entity_get_const(w, e, c)
                                : NULL;
    if (live == NULL)
        return op_error(&f, DE_EP_ERR_NOT_FOUND, "%llu has no " DE_FOLIAGE_COMPONENT,
                        (unsigned long long)o->entity);
    page_list pages;
    de_foliage_store before, after;
    de__foliage_store_init(&before, NULL);
    de__foliage_store_init(&after, NULL);
    i32(*keys)[2] = NULL;
    u32 nk = 0;
    bool ok = false;
    const load_rc lr = layer_load(ed, o->entity, &pages, &before);
    if (lr != LOAD_OK) {
        if (lr == LOAD_OOM)
            op_error(&f, DE_EP_ERR_BUSY, "%.*s: out of memory", (int)o->op.len, o->op.ptr);
        else
            op_error(&f, DE_EP_ERR_INVALID_PARAMS, "%.*s: the foliage cell table is inconsistent",
                     (int)o->op.len, o->op.ptr);
        goto done;
    }
    if (!de__foliage_store_copy(&after, &before)) {
        op_error(&f, DE_EP_ERR_BUSY, "%.*s: out of memory", (int)o->op.len, o->op.ptr);
        goto done;
    }
    const env_ctx ctx = {ed, live};
    if (de_ep_str_eq(o->op, "foliage.paint"))
        ok = op_paint(ed, a, o, &after, &ctx, &f);
    else if (de_ep_str_eq(o->op, "foliage.erase"))
        ok = op_erase(a, o, &after, &f);
    else if (de_ep_str_eq(o->op, "foliage.scatter"))
        ok = op_scatter(a, o, &after, &ctx, &f);
    else if (de_ep_str_eq(o->op, "foliage.restore")) {
        if (!o->has_value) {
            op_error(&f, DE_EP_ERR_INVALID_PARAMS, "foliage.restore needs value");
            goto done;
        }
        const int rc = restore(&after, o->value);
        if (rc == 0)
            realign(ed, &after, &ctx);
        ok = rc == 0 ||
             op_error(&f, DE_EP_ERR_INVALID_PARAMS, rc == 1 ? "foliage.restore: bad cell data"
                                                             : "foliage.restore: the layer is full");
    } else {
        op_error(&f, DE_EP_ERR_INVALID_PARAMS, "unknown op `%.*s`", (int)o->op.len, o->op.ptr);
    }
    if (!ok)
        goto done;
    ok = false;
    if (!changed_cells(&before, &after, &keys, &nk)) {
        op_error(&f, DE_EP_ERR_BUSY, "%.*s: out of memory", (int)o->op.len, o->op.ptr);
        goto done;
    }
    const de_ep_raw undo = restore_from(a, &before, keys, nk);
    if (undo.len == 0) {
        op_error(&f, DE_EP_ERR_BUSY, "%.*s: out of scratch memory", (int)o->op.len, o->op.ptr);
        goto done;
    }
    /* The layer is marked changed, so its revision moves; the changed cells take it. */
    de_entity_mark_changed(w, e, c);
    const u64 rev = de__ed_entity_rev(ed, e);
    for (u32 k = 0; k < nk; ++k) {
        if (!cell_write(ed, e, &pages, &after, keys[k][0], keys[k][1], rev)) {
            op_error(&f, DE_EP_ERR_BUSY, "%.*s: out of memory writing foliage pages",
                     (int)o->op.len, o->op.ptr);
            goto done;
        }
    }
    memset(inv, 0, sizeof *inv);
    inv->op = de_ep_cstr("foliage.restore");
    inv->has_entity = true;
    inv->entity = o->entity;
    inv->has_value = true;
    inv->value = undo;
    ok = true;
done:
    DE_FREE(de_heap_allocator(), keys, sizeof(i32[2]) * (before.cell_count + after.cell_count));
    pages_free(&pages);
    de__foliage_store_free(&before);
    de__foliage_store_free(&after);
    return ok;
}

bool de__ed_foliage_load(de__editor *ed, de_entity layer, de_foliage_store *st) {
    page_list l;
    const load_rc rc = layer_load(ed, layer.bits, &l, st);
    pages_free(&l);
    return rc == LOAD_OK;
}

void de__ed_foliage_register(de__editor *ed) {
    (void)comp_id(world_of(ed), DE_FOLIAGE_COMPONENT); /* listed by reflection from the start */
    (void)comp_id(world_of(ed), DE_FOLIAGE_PAGE_COMPONENT);
    ed->foliage_op = foliage_op;
    de__ed_on(ed, DE_EP_METHOD_FOLIAGE_INFO, foliage_info);
    de__ed_on(ed, DE_EP_METHOD_FOLIAGE_INSTANCES, foliage_instances);
    de__ed_on(ed, DE_EP_METHOD_FOLIAGE_TYPE, foliage_type);
    de__ed_add_capability(ed, DE_FOLIAGE_CAPABILITY);
}
