/* nav_cook.c: the `.navmesh` TOML source -> DNAV container (DENG-76). The pure core of the
 * cooker's `navmesh` importer (tools/cook/importers/navmesh/), in de_nav_build so the runtime and
 * the tests cook the same way. Deterministic: the same source gives the same bytes.
 *
 * Source format (every length in meters, engine space: Z up):
 *   [build]
 *   cell_size = 0.25            cell_height = 0.1
 *   agent_radius = 0.4          agent_height = 1.8
 *   agent_max_climb = 0.4       agent_max_slope_deg = 45.0
 *   tile_size = 32.0            collision_mask = 0     # optional; stored only
 *   origin = [0.0, 0.0, 0.0]    # optional; else from the region's center
 *   [region]                    # optional; else the bounds of every shape
 *   min = [x, y, z]  max = [x, y, z]
 *   [[box]]  center = [x, y, z]  half_extents = [x, y, z]  rotation = [x, y, z, w] (optional)
 *   [[mesh]] vertices = [[x, y, z], ...]  indices = [i0, i1, i2, ...]   # counter-clockwise up
 */
#include "gameplay/nav/nav_build.h"

#include "core/de_toml_internal.h"

#include <dengine/de_base.h>
#include <dengine/de_memory.h>

#include <stdio.h>
#include <string.h>

typedef struct cook_ctx {
    char *why_buf;
    size_t why_cap;
} cook_ctx;

static de_result fail(cook_ctx *c, de_result r, const char *msg, int line) {
    if (c->why_buf != NULL && c->why_cap > 0) {
        if (line > 0)
            (void)snprintf(c->why_buf, c->why_cap, "navmesh: %s (line %d)", msg, line);
        else
            (void)snprintf(c->why_buf, c->why_cap, "navmesh: %s", msg);
    }
    return r;
}

static bool num(toml_datum_t d, f64 *out) {
    if (d.type == TOML_FP64) {
        *out = d.u.fp64;
        return *out == *out && *out - *out == 0.0;
    }
    if (d.type == TOML_INT64) {
        *out = (f64)d.u.int64;
        return true;
    }
    return false;
}

static bool vec(toml_datum_t d, f64 *out, int n) {
    if (d.type != TOML_ARRAY || d.u.arr.size != n)
        return false;
    for (int i = 0; i < n; ++i)
        if (!num(d.u.arr.elem[i], &out[i]))
            return false;
    return true;
}

static de_dvec3 dv(const f64 v[3]) {
    de_dvec3 r;
    r.x = v[0];
    r.y = v[1];
    r.z = v[2];
    return r;
}

static de_result read_build(cook_ctx *c, toml_datum_t b, de_navmesh_build_desc *d, bool *has_origin,
                            de_dvec3 *origin) {
    if (b.type != TOML_TABLE)
        return fail(c, DE_ERR_PARSE, "missing [build] table", 0);
    static const char *const keys[] = {"cell_size",       "cell_height",        "agent_radius",
                                       "agent_height",    "agent_max_climb",    "agent_max_slope_deg",
                                       "tile_size"};
    f64 v[7];
    for (int i = 0; i < 7; ++i) {
        if (!num(toml_get(b, keys[i]), &v[i])) {
            char msg[96];
            (void)snprintf(msg, sizeof msg, "[build] %s must be a number", keys[i]);
            return fail(c, DE_ERR_PARSE, msg, b.lineno);
        }
    }
    memset(d, 0, sizeof *d);
    d->cell_size = (f32)v[0];
    d->cell_height = (f32)v[1];
    d->agent_radius = (f32)v[2];
    d->agent_height = (f32)v[3];
    d->agent_max_climb = (f32)v[4];
    d->agent_max_slope_rad = (f32)(v[5] * (3.14159265358979323846 / 180.0));
    d->tile_size = (f32)v[6];
    toml_datum_t m = toml_get(b, "collision_mask");
    if (m.type != TOML_UNKNOWN) {
        if (m.type != TOML_INT64 || m.u.int64 < 0 || m.u.int64 > 0xFFFFFFFFll)
            return fail(c, DE_ERR_PARSE, "[build] collision_mask must be a u32", m.lineno);
        d->collision_mask = (u32)m.u.int64;
    }
    toml_datum_t o = toml_get(b, "origin");
    *has_origin = false;
    if (o.type != TOML_UNKNOWN) {
        f64 ov[3];
        if (!vec(o, ov, 3))
            return fail(c, DE_ERR_PARSE, "[build] origin must be [x, y, z]", o.lineno);
        *origin = dv(ov);
        *has_origin = true;
    }
    return DE_OK;
}

static de_result add_boxes(cook_ctx *c, toml_datum_t arr, de__nav_soup *s) {
    if (arr.type == TOML_UNKNOWN)
        return DE_OK;
    if (arr.type != TOML_ARRAY)
        return fail(c, DE_ERR_PARSE, "box must be an array of tables ([[box]])", arr.lineno);
    for (int i = 0; i < arr.u.arr.size; ++i) {
        toml_datum_t t = arr.u.arr.elem[i];
        f64 ctr[3], he[3], q[4] = {0.0, 0.0, 0.0, 1.0};
        if (t.type != TOML_TABLE || !vec(toml_get(t, "center"), ctr, 3) ||
            !vec(toml_get(t, "half_extents"), he, 3) || !(he[0] > 0.0) || !(he[1] > 0.0) ||
            !(he[2] > 0.0))
            return fail(c, DE_ERR_PARSE, "[[box]] needs center and half_extents > 0", t.lineno);
        toml_datum_t r = toml_get(t, "rotation");
        if (r.type != TOML_UNKNOWN && !vec(r, q, 4))
            return fail(c, DE_ERR_PARSE, "[[box]] rotation must be [x, y, z, w]", r.lineno);
        de_transform tr;
        memset(&tr, 0, sizeof tr);
        tr.position = dv(ctr);
        tr.rotation.x = (f32)q[0];
        tr.rotation.y = (f32)q[1];
        tr.rotation.z = (f32)q[2];
        tr.rotation.w = (f32)q[3];
        de__nav_pose p = de__nav_pose_from(&tr);
        de_vec3 h;
        h.x = (f32)he[0];
        h.y = (f32)he[1];
        h.z = (f32)he[2];
        de__nav_soup_add_box(s, &p, h);
    }
    return DE_OK;
}

static de_result add_meshes(cook_ctx *c, toml_datum_t arr, de__nav_soup *s) {
    if (arr.type == TOML_UNKNOWN)
        return DE_OK;
    if (arr.type != TOML_ARRAY)
        return fail(c, DE_ERR_PARSE, "mesh must be an array of tables ([[mesh]])", arr.lineno);
    for (int i = 0; i < arr.u.arr.size; ++i) {
        toml_datum_t t = arr.u.arr.elem[i];
        toml_datum_t vs = toml_get(t, "vertices"), is = toml_get(t, "indices");
        if (t.type != TOML_TABLE || vs.type != TOML_ARRAY || is.type != TOML_ARRAY ||
            is.u.arr.size % 3 != 0)
            return fail(c, DE_ERR_PARSE,
                        "[[mesh]] needs vertices = [[x, y, z], ...] and indices (3 per triangle)",
                        t.lineno);
        for (int k = 0; k < is.u.arr.size; k += 3) {
            de_dvec3 tri[3];
            for (int j = 0; j < 3; ++j) {
                toml_datum_t ix = is.u.arr.elem[k + j];
                f64 v[3];
                if (ix.type != TOML_INT64 || ix.u.int64 < 0 || ix.u.int64 >= vs.u.arr.size ||
                    !vec(vs.u.arr.elem[ix.u.int64], v, 3))
                    return fail(c, DE_ERR_PARSE, "[[mesh]] bad index or vertex", ix.lineno);
                tri[j] = dv(v);
            }
            de__nav_soup_add_tri(s, tri[0], tri[1], tri[2]);
        }
    }
    return DE_OK;
}

/* World-space bounds of a soup (Recast space relative to its origin, mapped back). */
static bool soup_bounds(const de__nav_soup *s, f32 rmin[3], f32 rmax[3]) {
    if (s->vert_count == 0)
        return false;
    for (u32 i = 0; i < s->vert_count; ++i) {
        for (int k = 0; k < 3; ++k) {
            const f32 v = s->verts[i * 3u + (u32)k];
            if (i == 0 || v < rmin[k])
                rmin[k] = v;
            if (i == 0 || v > rmax[k])
                rmax[k] = v;
        }
    }
    return true;
}

typedef struct tile_list {
    const de_allocator *a;
    de__nav_tile_blob *t;
    u32 count, cap;
} tile_list;

static void tile_list_free(tile_list *l) {
    for (u32 i = 0; i < l->count; ++i)
        de__nav_rc_free((void *)l->t[i].data);
    if (l->t != NULL)
        DE_FREE(l->a, l->t, sizeof(de__nav_tile_blob) * l->cap);
}

de_result de__nav_cook_source(const de_allocator *a, const char *src, u64 len, u8 **out,
                              u64 *out_size, char *why_buf, size_t why_cap) {
    cook_ctx c = {why_buf, why_cap};
    if (why_buf != NULL && why_cap > 0)
        why_buf[0] = '\0';
    *out = NULL;
    *out_size = 0;
    if (a == NULL || src == NULL || len > 0x7FFFFFFFu)
        return fail(&c, DE_ERR_INVALID_ARGUMENT, "bad arguments", 0);
    toml_result_t res = de__toml_parse(a, src, (int)len);
    if (!res.ok) {
        de_result r = fail(&c, DE_ERR_PARSE, res.errmsg, 0);
        de__toml_free(res);
        return r;
    }
    de_navmesh_build_desc d;
    bool has_origin = false;
    de_dvec3 origin = {0.0, 0.0, 0.0};
    de_result r = read_build(&c, toml_get(res.toptab, "build"), &d, &has_origin, &origin);
    de__nav_settings st;
    if (r == DE_OK) {
        const char *why = NULL;
        r = de__nav_settings_make(&d, &st, &why);
        if (r != DE_OK)
            (void)fail(&c, r, why, 0);
    }
    /* Region: [region] or the bounds of the shapes. The shapes go into a soup at a provisional
     * origin of 0 first (for the bounds), then into the final soup at the navmesh origin. */
    f64 rg[2][3];
    bool has_region = false;
    if (r == DE_OK) {
        toml_datum_t rt = toml_get(res.toptab, "region");
        if (rt.type != TOML_UNKNOWN) {
            if (rt.type != TOML_TABLE || !vec(toml_get(rt, "min"), rg[0], 3) ||
                !vec(toml_get(rt, "max"), rg[1], 3) || !(rg[0][0] < rg[1][0]) ||
                !(rg[0][1] < rg[1][1]) || !(rg[0][2] <= rg[1][2]))
                r = fail(&c, DE_ERR_PARSE, "[region] needs min < max", rt.lineno);
            else
                has_region = true;
        }
    }
    const de_dvec3 zero = {0.0, 0.0, 0.0};
    de__nav_soup probe;
    de__nav_soup_init(&probe, a, zero);
    if (r == DE_OK && !has_region) {
        r = add_boxes(&c, toml_get(res.toptab, "box"), &probe);
        if (r == DE_OK)
            r = add_meshes(&c, toml_get(res.toptab, "mesh"), &probe);
        f32 bmin[3], bmax[3];
        if (r == DE_OK && probe.failed)
            r = fail(&c, DE_ERR_OUT_OF_MEMORY, "out of memory", 0);
        if (r == DE_OK && !soup_bounds(&probe, bmin, bmax))
            r = fail(&c, DE_ERR_PARSE, "no shapes and no [region]", 0);
        if (r == DE_OK) {
            const f32 lo[3] = {bmin[0], bmin[1], bmin[2]}, hi[3] = {bmax[0], bmax[1], bmax[2]};
            /* Recast (x, y, z) -> engine (x, -z, y). */
            rg[0][0] = lo[0];
            rg[1][0] = hi[0];
            rg[0][1] = -(f64)hi[2];
            rg[1][1] = -(f64)lo[2];
            rg[0][2] = lo[1];
            rg[1][2] = hi[1];
        }
    }
    de__nav_soup_free(&probe);
    if (r == DE_OK && !has_origin) {
        const f64 cc[3] = {(rg[0][0] + rg[1][0]) * 0.5, (rg[0][1] + rg[1][1]) * 0.5, 0.0};
        origin = de__nav_origin_for(dv(cc));
    }
    de__nav_soup soup;
    de__nav_soup_init(&soup, a, origin);
    if (r == DE_OK) {
        r = add_boxes(&c, toml_get(res.toptab, "box"), &soup);
        if (r == DE_OK)
            r = add_meshes(&c, toml_get(res.toptab, "mesh"), &soup);
        if (r == DE_OK && soup.failed)
            r = fail(&c, DE_ERR_OUT_OF_MEMORY, "out of memory", 0);
    }
    de__toml_free(res);
    tile_list tl = {a, NULL, 0, 0};
    if (r == DE_OK) {
        f32 rmin[3], rmax[3];
        de__nav_box_to_recast(&origin, dv(rg[0]), dv(rg[1]), rmin, rmax);
        i32 tx0, tz0, tx1, tz1;
        de__nav_tile_range(d.tile_size, rmin, rmax, &tx0, &tz0, &tx1, &tz1);
        const i64 n = ((i64)tx1 - tx0 + 1) * ((i64)tz1 - tz0 + 1);
        if (n > DE__NAV_MAX_TILES)
            r = fail(&c, DE_ERR_INVALID_ARGUMENT, "region covers more than 4096 tiles", 0);
        for (i32 tz = tz0; r == DE_OK && tz <= tz1; ++tz) {
            for (i32 tx = tx0; r == DE_OK && tx <= tx1; ++tx) {
                u8 *data = NULL;
                i32 size = 0;
                r = de__nav_build_tile(a, &st, &soup, tx, tz, &data, &size);
                if (r != DE_OK) {
                    (void)fail(&c, r, "tile build failed", 0);
                    break;
                }
                if (data == NULL)
                    continue;
                if (tl.count == tl.cap) {
                    const u32 nc = tl.cap != 0 ? tl.cap * 2u : 16u;
                    void *np =
                        tl.t == NULL
                            ? DE_ALLOC(a, sizeof(de__nav_tile_blob) * nc, 16, DE_MEM_GENERAL)
                            : DE_REALLOC(a, tl.t, sizeof(de__nav_tile_blob) * tl.cap,
                                         sizeof(de__nav_tile_blob) * nc, 16, DE_MEM_GENERAL);
                    if (np == NULL) {
                        de__nav_rc_free(data);
                        r = fail(&c, DE_ERR_OUT_OF_MEMORY, "out of memory", 0);
                        break;
                    }
                    tl.t = (de__nav_tile_blob *)np;
                    tl.cap = nc;
                }
                tl.t[tl.count].tx = tx;
                tl.t[tl.count].tz = tz;
                tl.t[tl.count].data = data;
                tl.t[tl.count].size = (u32)size;
                ++tl.count;
            }
        }
    }
    de__nav_soup_free(&soup);
    if (r == DE_OK) {
        de__nav_container_info info;
        memset(&info, 0, sizeof info);
        info.origin = origin;
        info.desc = d;
        info.tile_count = tl.count;
        r = de__nav_container_write(a, &info, tl.t, tl.count, out, out_size);
        if (r != DE_OK)
            (void)fail(&c, r, "out of memory", 0);
    }
    tile_list_free(&tl);
    return r;
}
