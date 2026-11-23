/* editor_foliage.c: foliage.info, foliage.instances, foliage.type and the foliage.* edit ops
 * (DENG-581; DIDE protocol 1.22, capability world.foliage).
 *
 * A foliage layer is an entity with DeFoliage (a render component): a palette of foliageType
 * assets (.foliage.toml), scatter rules, and instances kept per 16 m cell. The IDE reads it with
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
 * height. Storage is bounded (DE_FOLIAGE_INSTANCES per layer, which is also max_per_cell).
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

/* The layer of `bits`, or answers the request with an error and returns NULL. */
static const DeFoliage *foliage_of(de__editor *ed, mpack_writer_t *w, u64 id, u64 bits,
                                   const char *method) {
    const DeFoliage *fo = foliage_get(ed, bits);
    if (fo == NULL) {
        de__ed_error(w, id, DE_EP_ERR_NOT_FOUND, "%s: entity %llu has no " DE_FOLIAGE_COMPONENT,
                     method, (unsigned long long)bits);
        return NULL;
    }
    if (!de__foliage_valid(fo)) {
        de__ed_error(w, id, DE_EP_ERR_INVALID_PARAMS, "%s: the foliage cell table is inconsistent",
                     method);
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
    const DeFoliage *fo = foliage_of(ed, w, req->id, q.entity, "foliage.info");
    if (fo == NULL)
        return;
    static de_ep_foliage_cell cells[DE_FOLIAGE_CELLS];
    static i32 xz[DE_FOLIAGE_CELLS][2];
    for (u32 i = 0; i < fo->cell_count; ++i) {
        xz[i][0] = fo->cells[i].x;
        xz[i][1] = fo->cells[i].z;
        cells[i].cell = (de_ep_list_i32){xz[i], 2};
        cells[i].count = fo->cells[i].count;
        cells[i].revision = fo->cells[i].revision;
    }
    de_ep_foliage_info r;
    memset(&r, 0, sizeof r);
    r.entity = q.entity;
    r.component = de_ep_cstr(DE_FOLIAGE_COMPONENT);
    r.cell_size = DE_FOLIAGE_CELL_SIZE;
    r.max_types = DE_FOLIAGE_TYPES;
    r.max_rules = DE_FOLIAGE_RULES;
    r.max_per_cell = DE_FOLIAGE_INSTANCES;
    r.type_suffix = de_ep_cstr(DE_FOLIAGE_SUFFIX);
    r.count = fo->instance_count;
    r.cells = (de_ep_list_foliage_cell){cells, fo->cell_count};
    r.revision = de__ed_entity_rev(ed, (de_entity){q.entity});
    de_ep_write_response_foliage_info(w, req->id, &r);
}

static void foliage_instances(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                              mpack_writer_t *w) {
    DE__ED_PARAMS(foliage_instances, de_ep_foliage_cell_params, q);
    if (q.cell.count != 2) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "foliage.instances: cell needs [x, z]");
        return;
    }
    const DeFoliage *fo = foliage_of(ed, w, req->id, q.entity, "foliage.instances");
    if (fo == NULL)
        return;
    static i32 xz[2];
    static u32 types[DE_FOLIAGE_INSTANCES], rules[DE_FOLIAGE_INSTANCES];
    static f32 xf[DE_FOLIAGE_INSTANCES * 10];
    xz[0] = q.cell.items[0];
    xz[1] = q.cell.items[1];
    u32 n = 0, k = 0;
    const de_foliage_instance *in = de__foliage_cell(fo, xz[0], xz[1], &n);
    u64 rev = 0;
    for (u32 i = 0; i < fo->cell_count; ++i)
        if (fo->cells[i].x == xz[0] && fo->cells[i].z == xz[1])
            rev = fo->cells[i].revision;
    for (u32 i = 0; i < n; ++i) {
        if (q.has_type && in[i].type != q.type)
            continue;
        types[k] = in[i].type;
        rules[k] = in[i].rule;
        de__foliage_transform(&in[i], &xf[k * 10]);
        ++k;
    }
    de_ep_foliage_instances r;
    memset(&r, 0, sizeof r);
    r.cell = (de_ep_list_i32){xz, 2};
    r.revision = rev;
    r.types = (de_ep_list_u32){types, k};
    r.rules = (de_ep_list_u32){rules, k};
    r.transforms = (de_ep_list_f32){xf, k * 10};
    de_ep_write_response_foliage_instances(w, req->id, &r);
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

/* Applies a foliage.restore value to `fo`. 0: done; 1: bad data; 2: no room. */
static int restore(DeFoliage *fo, de_ep_raw value) {
    static de_foliage_instance inst[DE_FOLIAGE_INSTANCES];
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
        if (n > DE_FOLIAGE_INSTANCES) {
            rc = 2;
            break;
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
        if (rc == 0 && !de__foliage_set_cell(fo, x, z, inst, (u32)n))
            rc = 2;
    }
    if (mpack_tree_destroy(&tree) != mpack_ok && rc == 0)
        rc = 1;
    return rc;
}

static void write_cell(mpack_writer_t *mw, const DeFoliage *fo, i32 x, i32 z) {
    u32 n = 0;
    const de_foliage_instance *in = de__foliage_cell(fo, x, z, &n);
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

/* The cells of `a` and `b` together, sorted by z then x: `out` gets at most 2 * DE_FOLIAGE_CELLS. */
static u32 cell_union(const DeFoliage *a, const DeFoliage *b, i32 out[][2]) {
    u32 i = 0, j = 0, n = 0;
    while (i < a->cell_count || j < b->cell_count) {
        const de_foliage_cell *ca = i < a->cell_count ? &a->cells[i] : NULL;
        const de_foliage_cell *cb = j < b->cell_count ? &b->cells[j] : NULL;
        const de_foliage_cell *pick;
        if (cb == NULL || (ca != NULL && (ca->z < cb->z || (ca->z == cb->z && ca->x <= cb->x))))
            pick = ca;
        else
            pick = cb;
        out[n][0] = pick->x;
        out[n][1] = pick->z;
        ++n;
        if (ca != NULL && ca->x == pick->x && ca->z == pick->z)
            ++i;
        if (cb != NULL && cb->x == pick->x && cb->z == pick->z)
            ++j;
    }
    return n;
}

/* The foliage.restore value that turns `after` back into `before`: the cells that differ, as
 * `before` has them. Empty when it does not fit the arena. */
static de_ep_raw restore_from(de_ep_arena *a, const DeFoliage *before, const DeFoliage *after) {
    i32 keys[2 * DE_FOLIAGE_CELLS][2];
    const u32 nk = cell_union(before, after, keys);
    u32 changed = 0;
    for (u32 k = 0; k < nk; ++k)
        if (!de__foliage_cell_equal(before, after, keys[k][0], keys[k][1]))
            ++changed;
    mpack_writer_t mw;
    de__ed_raw_begin(a, &mw);
    mpack_start_map(&mw, 1);
    mpack_write_cstr(&mw, "cells");
    mpack_start_array(&mw, changed);
    for (u32 k = 0; k < nk; ++k)
        if (!de__foliage_cell_equal(before, after, keys[k][0], keys[k][1]))
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

static bool op_paint(de__editor *ed, de_ep_arena *a, const de_ep_edit_op *o, DeFoliage *fo,
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
    return model_ok(de__foliage_paint(fo, &env, &s, surfaces, ns, f->msg, f->cap), f);
}

static bool op_erase(de_ep_arena *a, const de_ep_edit_op *o, DeFoliage *fo, op_fail *f) {
    de_ep_foliage_stroke p;
    if (!read_stroke(a, o, &p, f))
        return false;
    const de_foliage_stroke s = stroke_of(&p);
    return model_ok(de__foliage_erase(fo, &s, f->msg, f->cap), f);
}

static bool op_scatter(de_ep_arena *a, const de_ep_edit_op *o, DeFoliage *fo, const env_ctx *ctx,
                       op_fail *f) {
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
    return model_ok(de__foliage_scatter(fo, &env, ids, p.has_rules ? p.rules.count : 0,
                                        p.has_clear && p.clear, f->msg, f->cap),
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
static void realign(de__editor *ed, DeFoliage *fo, const env_ctx *ctx) {
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
    de__foliage_realign(fo, &env, surfaces, n);
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
    DeFoliage *live = DE_HANDLE_VALID(c) && de_entity_alive(w, e)
                          ? (DeFoliage *)de_entity_get(w, e, c)
                          : NULL;
    if (live == NULL)
        return op_error(&f, DE_EP_ERR_NOT_FOUND, "%llu has no " DE_FOLIAGE_COMPONENT,
                        (unsigned long long)o->entity);
    if (!de__foliage_valid(live))
        return op_error(&f, DE_EP_ERR_INVALID_PARAMS, "%.*s: the foliage cell table is inconsistent",
                        (int)o->op.len, o->op.ptr);
    static DeFoliage after; /* main thread only; about 12 KB */
    after = *live;
    const env_ctx ctx = {ed, live};
    bool ok;
    if (de_ep_str_eq(o->op, "foliage.paint"))
        ok = op_paint(ed, a, o, &after, &ctx, &f);
    else if (de_ep_str_eq(o->op, "foliage.erase"))
        ok = op_erase(a, o, &after, &f);
    else if (de_ep_str_eq(o->op, "foliage.scatter"))
        ok = op_scatter(a, o, &after, &ctx, &f);
    else if (de_ep_str_eq(o->op, "foliage.restore")) {
        if (!o->has_value)
            return op_error(&f, DE_EP_ERR_INVALID_PARAMS, "foliage.restore needs value");
        const int rc = restore(&after, o->value);
        if (rc == 0)
            realign(ed, &after, &ctx);
        ok = rc == 0 ||
             op_error(&f, DE_EP_ERR_INVALID_PARAMS, rc == 1 ? "foliage.restore: bad cell data"
                                                             : "foliage.restore: the layer is full");
    } else {
        return op_error(&f, DE_EP_ERR_INVALID_PARAMS, "unknown op `%.*s`", (int)o->op.len,
                        o->op.ptr);
    }
    if (!ok)
        return false;
    const de_ep_raw undo = restore_from(a, live, &after);
    if (undo.len == 0)
        return op_error(&f, DE_EP_ERR_BUSY, "%.*s: out of scratch memory", (int)o->op.len,
                        o->op.ptr);
    bool changed[DE_FOLIAGE_CELLS];
    for (u32 i = 0; i < after.cell_count; ++i)
        changed[i] = !de__foliage_cell_equal(live, &after, after.cells[i].x, after.cells[i].z);
    *live = after;
    de_entity_mark_changed(w, e, c);
    const u64 rev = de__ed_entity_rev(ed, e);
    for (u32 i = 0; i < live->cell_count; ++i)
        if (changed[i])
            live->cells[i].revision = rev;
    memset(inv, 0, sizeof *inv);
    inv->op = de_ep_cstr("foliage.restore");
    inv->has_entity = true;
    inv->entity = o->entity;
    inv->has_value = true;
    inv->value = undo;
    return true;
}

void de__ed_foliage_register(de__editor *ed) {
    (void)comp_id(world_of(ed), DE_FOLIAGE_COMPONENT); /* listed by reflection from the start */
    ed->foliage_op = foliage_op;
    de__ed_on(ed, DE_EP_METHOD_FOLIAGE_INFO, foliage_info);
    de__ed_on(ed, DE_EP_METHOD_FOLIAGE_INSTANCES, foliage_instances);
    de__ed_on(ed, DE_EP_METHOD_FOLIAGE_TYPE, foliage_type);
    de__ed_add_capability(ed, DE_FOLIAGE_CAPABILITY);
}
