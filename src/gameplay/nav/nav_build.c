/* nav_build.c: settings, coordinate helpers, the triangle soup and its primitive surfaces, and
 * the per-tile build (DENG-76). Shared by the runtime and the cooker (target de_nav_build).
 * Deterministic module code: de_dmath only, no libm (SPEC §5.7). */
#include "gameplay/nav/nav_build.h"

#include <dengine/de_base.h>
#include <dengine/de_math.h>
#include <dengine/de_memory.h>

#include <string.h>

/* ---- Settings -------------------------------------------------------------------------------- */

static bool finite_f32(f32 x) {
    return x == x && x - x == 0.0f;
}

de_result de__nav_settings_make(const de_navmesh_build_desc *d, de__nav_settings *out,
                                const char **why) {
    const char *dummy = NULL;
    if (why == NULL)
        why = &dummy;
    *why = NULL;
    if (d == NULL || out == NULL) {
        *why = "NULL argument";
        return DE_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof *out);
    const f32 vals[] = {d->cell_size,    d->cell_height,     d->agent_radius,       d->agent_height,
                        d->agent_max_climb, d->agent_max_slope_rad, d->tile_size};
    for (size_t i = 0; i < sizeof vals / sizeof vals[0]; ++i) {
        if (!finite_f32(vals[i])) {
            *why = "build settings must be finite";
            return DE_ERR_INVALID_ARGUMENT;
        }
    }
    if (!(d->cell_size > 0.0f) || !(d->cell_height > 0.0f) || !(d->agent_height > 0.0f) ||
        !(d->tile_size > 0.0f) || d->agent_radius < 0.0f || d->agent_max_climb < 0.0f) {
        *why = "cell_size, cell_height, agent_height and tile_size must be > 0; agent_radius and "
               "agent_max_climb >= 0";
        return DE_ERR_INVALID_ARGUMENT;
    }
    if (!(d->agent_max_slope_rad > 0.0f) || !(d->agent_max_slope_rad < DE_PI * 0.5f)) {
        *why = "agent_max_slope_rad must be in (0, pi/2)";
        return DE_ERR_INVALID_ARGUMENT;
    }
    const f32 cells = de_dround(d->tile_size / d->cell_size);
    const f32 err = cells * d->cell_size - d->tile_size;
    if (cells < 8.0f || cells > 1024.0f || err > 1e-4f * d->tile_size ||
        err < -1e-4f * d->tile_size) {
        *why = "tile_size must be a whole number (8..1024) of cells (tile_size / cell_size)";
        return DE_ERR_INVALID_ARGUMENT;
    }
    const f32 tiles = de_dround((f32)DE__NAV_CELL_M / d->tile_size);
    const f32 terr = tiles * d->tile_size - (f32)DE__NAV_CELL_M;
    if (tiles < 1.0f || terr > 1e-3f || terr < -1e-3f) {
        *why = "tile_size must divide the 256 m world cell into whole tiles";
        return DE_ERR_INVALID_ARGUMENT;
    }
    out->desc = *d;
    out->tile_cells = (i32)cells;
    out->walkable_height = (i32)de_dceil(d->agent_height / d->cell_height);
    out->walkable_climb = (i32)de_dfloor(d->agent_max_climb / d->cell_height);
    out->walkable_radius = (i32)de_dceil(d->agent_radius / d->cell_size);
    out->border = out->walkable_radius + 3;
    out->slope_deg = d->agent_max_slope_rad * (180.0f / DE_PI);
    return DE_OK;
}

static f64 floor_cell(f64 v) {
    /* floor(v / 256) * 256 without libm: exact for |v| < 2^53. */
    const f64 q = v / DE__NAV_CELL_M;
    i64 i = (i64)q;
    if ((f64)i > q)
        --i;
    return (f64)i * DE__NAV_CELL_M;
}

de_dvec3 de__nav_origin_for(de_dvec3 c) {
    de_dvec3 o;
    o.x = floor_cell(c.x);
    o.y = floor_cell(c.y);
    o.z = 0.0;
    return o;
}

void de__nav_tile_range(f32 t, const f32 rmin[3], const f32 rmax[3], i32 *tx0, i32 *tz0,
                        i32 *tx1, i32 *tz1) {
    *tx0 = (i32)de_dfloor(rmin[0] / t);
    *tz0 = (i32)de_dfloor(rmin[2] / t);
    *tx1 = (i32)de_dceil(rmax[0] / t) - 1;
    *tz1 = (i32)de_dceil(rmax[2] / t) - 1;
    if (*tx1 < *tx0)
        *tx1 = *tx0;
    if (*tz1 < *tz0)
        *tz1 = *tz0;
}

void de__nav_box_to_recast(const de_dvec3 *origin, de_dvec3 wmin, de_dvec3 wmax, f32 rmin[3],
                           f32 rmax[3]) {
    /* (x, y, z) -> (x, z, -y): the y extent flips into Recast z. */
    rmin[0] = (f32)(wmin.x - origin->x);
    rmin[1] = (f32)(wmin.z - origin->z);
    rmin[2] = (f32)(-(wmax.y - origin->y));
    rmax[0] = (f32)(wmax.x - origin->x);
    rmax[1] = (f32)(wmax.z - origin->z);
    rmax[2] = (f32)(-(wmin.y - origin->y));
}

/* ---- Poses ----------------------------------------------------------------------------------- */

static void qrot(const f64 q[4], const f64 v[3], f64 out[3]) {
    /* v' = v + 2w (u x v) + 2 u x (u x v), u = (x, y, z). */
    const f64 ux = q[0], uy = q[1], uz = q[2], w = q[3];
    const f64 cx = uy * v[2] - uz * v[1];
    const f64 cy = uz * v[0] - ux * v[2];
    const f64 cz = ux * v[1] - uy * v[0];
    const f64 ccx = uy * cz - uz * cy;
    const f64 ccy = uz * cx - ux * cz;
    const f64 ccz = ux * cy - uy * cx;
    out[0] = v[0] + 2.0 * (w * cx + ccx);
    out[1] = v[1] + 2.0 * (w * cy + ccy);
    out[2] = v[2] + 2.0 * (w * cz + ccz);
}

de__nav_pose de__nav_pose_from(const de_transform *t) {
    de__nav_pose p;
    p.pos = t->position;
    f64 q[4] = {t->rotation.x, t->rotation.y, t->rotation.z, t->rotation.w};
    f64 n = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    if (!(n > 1e-12)) {
        q[0] = q[1] = q[2] = 0.0;
        q[3] = 1.0;
    } else {
        n = de_dsqrt64(n);
        for (int i = 0; i < 4; ++i)
            q[i] /= n;
    }
    memcpy(p.q, q, sizeof q);
    return p;
}

de__nav_pose de__nav_pose_mul(const de__nav_pose *a, const de__nav_pose *b) {
    de__nav_pose r;
    const f64 v[3] = {b->pos.x, b->pos.y, b->pos.z};
    f64 o[3];
    qrot(a->q, v, o);
    r.pos.x = a->pos.x + o[0];
    r.pos.y = a->pos.y + o[1];
    r.pos.z = a->pos.z + o[2];
    const f64 *p = a->q, *q = b->q;
    r.q[0] = p[3] * q[0] + p[0] * q[3] + p[1] * q[2] - p[2] * q[1];
    r.q[1] = p[3] * q[1] - p[0] * q[2] + p[1] * q[3] + p[2] * q[0];
    r.q[2] = p[3] * q[2] + p[0] * q[1] - p[1] * q[0] + p[2] * q[3];
    r.q[3] = p[3] * q[3] - p[0] * q[0] - p[1] * q[1] - p[2] * q[2];
    return r;
}

de_dvec3 de__nav_pose_apply(const de__nav_pose *p, de_vec3 v) {
    const f64 l[3] = {v.x, v.y, v.z};
    f64 o[3];
    qrot(p->q, l, o);
    de_dvec3 r;
    r.x = p->pos.x + o[0];
    r.y = p->pos.y + o[1];
    r.z = p->pos.z + o[2];
    return r;
}

/* ---- Soup ------------------------------------------------------------------------------------ */

void de__nav_soup_init(de__nav_soup *s, const de_allocator *a, de_dvec3 origin) {
    memset(s, 0, sizeof *s);
    s->alloc = a;
    s->origin = origin;
}

void de__nav_soup_free(de__nav_soup *s) {
    if (s->verts != NULL)
        DE_FREE(s->alloc, s->verts, sizeof(f32) * 3u * s->vert_cap);
    if (s->tris != NULL)
        DE_FREE(s->alloc, s->tris, sizeof(i32) * 3u * s->tri_cap);
    if (s->obstacles != NULL)
        DE_FREE(s->alloc, s->obstacles, sizeof(f32) * DE__NAV_RC_OBSTACLE_FLOATS * s->obstacle_cap);
    s->obstacles = NULL;
    s->obstacle_count = s->obstacle_cap = 0;
    s->verts = NULL;
    s->tris = NULL;
    s->vert_count = s->vert_cap = s->tri_count = s->tri_cap = 0;
}

static bool grow(const de_allocator *a, void **p, u32 *cap, u32 need, size_t elem) {
    if (need <= *cap)
        return true;
    u32 nc = *cap != 0 ? *cap : 64u;
    while (nc < need) {
        if (nc > 0x3FFFFFFFu)
            return false;
        nc *= 2u;
    }
    void *np = *p == NULL ? DE_ALLOC(a, elem * nc, 16, DE_MEM_GENERAL)
                          : DE_REALLOC(a, *p, elem * *cap, elem * nc, 16, DE_MEM_GENERAL);
    if (np == NULL)
        return false;
    *p = np;
    *cap = nc;
    return true;
}

static void add_recast_tri(de__nav_soup *s, const f32 v[9]) {
    if (s->failed)
        return;
    if (s->clip) {
        for (int k = 0; k < 3; k += 2) {
            const f32 lo = v[k] < v[3 + k] ? (v[k] < v[6 + k] ? v[k] : v[6 + k])
                                           : (v[3 + k] < v[6 + k] ? v[3 + k] : v[6 + k]);
            const f32 hi = v[k] > v[3 + k] ? (v[k] > v[6 + k] ? v[k] : v[6 + k])
                                           : (v[3 + k] > v[6 + k] ? v[3 + k] : v[6 + k]);
            if (hi < s->clip_min[k] || lo > s->clip_max[k])
                return;
        }
    }
    if (s->vert_count > 0x7FFFFFF0u ||
        !grow(s->alloc, (void **)&s->verts, &s->vert_cap, s->vert_count + 3u, sizeof(f32) * 3u) ||
        !grow(s->alloc, (void **)&s->tris, &s->tri_cap, s->tri_count + 1u, sizeof(i32) * 3u)) {
        s->failed = true;
        return;
    }
    memcpy(&s->verts[s->vert_count * 3u], v, sizeof(f32) * 9u);
    for (u32 i = 0; i < 3; ++i)
        s->tris[s->tri_count * 3u + i] = (i32)(s->vert_count + i);
    s->vert_count += 3u;
    s->tri_count += 1u;
}

void de__nav_soup_add_tri(de__nav_soup *s, de_dvec3 a, de_dvec3 b, de_dvec3 c) {
    f32 v[9];
    de__nav_world_to_recast(&s->origin, a, &v[0]);
    de__nav_world_to_recast(&s->origin, b, &v[3]);
    de__nav_world_to_recast(&s->origin, c, &v[6]);
    add_recast_tri(s, v);
}

void de__nav_soup_append(de__nav_soup *s, const de__nav_soup *src) {
    for (u32 t = 0; t < src->tri_count; ++t) {
        f32 v[9];
        for (u32 i = 0; i < 3; ++i)
            memcpy(&v[i * 3u], &src->verts[(u32)src->tris[t * 3u + i] * 3u], sizeof(f32) * 3u);
        add_recast_tri(s, v);
    }
}

static de_vec3 v3(f32 x, f32 y, f32 z) {
    de_vec3 r;
    r.x = x;
    r.y = y;
    r.z = z;
    return r;
}

static void add_local_tri(de__nav_soup *s, const de__nav_pose *p, de_vec3 a, de_vec3 b,
                          de_vec3 c) {
    de__nav_soup_add_tri(s, de__nav_pose_apply(p, a), de__nav_pose_apply(p, b),
                         de__nav_pose_apply(p, c));
}

void de__nav_soup_add_box(de__nav_soup *s, const de__nav_pose *p, de_vec3 h) {
    /* Corner i: x sign bit 0, y sign bit 1, z sign bit 2. Faces counter-clockwise from outside. */
    static const u8 k_faces[6][4] = {
        {4, 5, 7, 6}, /* +Z */
        {0, 2, 3, 1}, /* -Z */
        {1, 3, 7, 5}, /* +X */
        {0, 4, 6, 2}, /* -X */
        {2, 6, 7, 3}, /* +Y */
        {0, 1, 5, 4}, /* -Y */
    };
    de_vec3 c[8];
    for (u32 i = 0; i < 8; ++i)
        c[i] = v3((i & 1u) ? h.x : -h.x, (i & 2u) ? h.y : -h.y, (i & 4u) ? h.z : -h.z);
    for (u32 f = 0; f < 6; ++f) {
        const u8 *q = k_faces[f];
        add_local_tri(s, p, c[q[0]], c[q[1]], c[q[2]]);
        add_local_tri(s, p, c[q[0]], c[q[2]], c[q[3]]);
    }
}

void de__nav_soup_add_obstacle(de__nav_soup *s, const de__nav_pose *p, de_vec3 h) {
    if (s->failed)
        return;
    f32 lo[3] = {0}, hi[3] = {0};
    for (u32 i = 0; i < 8; ++i) {
        const de_vec3 c = v3((i & 1u) ? h.x : -h.x, (i & 2u) ? h.y : -h.y, (i & 4u) ? h.z : -h.z);
        f32 r[3];
        de__nav_world_to_recast(&s->origin, de__nav_pose_apply(p, c), r);
        for (int k = 0; k < 3; ++k) {
            if (i == 0 || r[k] < lo[k])
                lo[k] = r[k];
            if (i == 0 || r[k] > hi[k])
                hi[k] = r[k];
        }
    }
    if (!grow(s->alloc, (void **)&s->obstacles, &s->obstacle_cap, s->obstacle_count + 1u,
              sizeof(f32) * DE__NAV_RC_OBSTACLE_FLOATS)) {
        s->failed = true;
        return;
    }
    f32 *o = &s->obstacles[s->obstacle_count * DE__NAV_RC_OBSTACLE_FLOATS];
    const f32 q[10] = {lo[0], lo[2], hi[0], lo[2], hi[0], hi[2], lo[0], hi[2], lo[1], hi[1]};
    memcpy(o, q, sizeof q);
    ++s->obstacle_count;
}

#define LATHE_SEGMENTS 8u
/* 1 / cos(22.5 deg): an 8-gon with this circumradius contains the unit circle. */
#define LATHE_SCALE 1.0823922f
static const f32 k_cos8[LATHE_SEGMENTS] = {1.0f,  0.70710678f,  0.0f, -0.70710678f,
                                           -1.0f, -0.70710678f, 0.0f, 0.70710678f};
static const f32 k_sin8[LATHE_SEGMENTS] = {0.0f,  0.70710678f,  1.0f,  0.70710678f,
                                           0.0f, -0.70710678f, -1.0f, -0.70710678f};

/* rings[i] = {z, radius}, bottom to top. */
static void add_lathe(de__nav_soup *s, const de__nav_pose *p, const f32 (*rings)[2], u32 n) {
    for (u32 r = 0; r + 1u < n; ++r) {
        const f32 z0 = rings[r][0], r0 = rings[r][1], z1 = rings[r + 1u][0], r1 = rings[r + 1u][1];
        for (u32 k = 0; k < LATHE_SEGMENTS; ++k) {
            const u32 k1 = (k + 1u) % LATHE_SEGMENTS;
            const de_vec3 a0 = v3(r0 * k_cos8[k], r0 * k_sin8[k], z0);
            const de_vec3 a1 = v3(r0 * k_cos8[k1], r0 * k_sin8[k1], z0);
            const de_vec3 b0 = v3(r1 * k_cos8[k], r1 * k_sin8[k], z1);
            const de_vec3 b1 = v3(r1 * k_cos8[k1], r1 * k_sin8[k1], z1);
            if (r0 > 0.0f)
                add_local_tri(s, p, a0, a1, b1);
            if (r1 > 0.0f)
                add_local_tri(s, p, a0, b1, b0);
        }
    }
}

/* sin and cos of -90, -45, 0, 45, 90 degrees. */
static const f32 k_lat_sin[5] = {-1.0f, -0.70710678f, 0.0f, 0.70710678f, 1.0f};
static const f32 k_lat_cos[5] = {0.0f, 0.70710678f, 1.0f, 0.70710678f, 0.0f};

void de__nav_soup_add_sphere(de__nav_soup *s, const de__nav_pose *p, f32 radius) {
    de__nav_soup_add_capsule(s, p, radius, 0.0f);
}

void de__nav_soup_add_capsule(de__nav_soup *s, const de__nav_pose *p, f32 radius,
                              f32 half_height) {
    if (!(radius > 0.0f))
        return;
    const f32 r = radius * LATHE_SCALE;
    f32 rings[6][2];
    u32 n = 0;
    for (u32 i = 0; i < 3; ++i) { /* bottom hemisphere, to the equator */
        rings[n][0] = -half_height + r * k_lat_sin[i];
        rings[n][1] = r * k_lat_cos[i];
        ++n;
    }
    for (u32 i = 2; i < 5; ++i) { /* top hemisphere, from the equator */
        if (i == 2 && !(half_height > 0.0f))
            continue; /* a sphere has one equator ring */
        rings[n][0] = half_height + r * k_lat_sin[i];
        rings[n][1] = r * k_lat_cos[i];
        ++n;
    }
    add_lathe(s, p, (const f32(*)[2])rings, n);
}

void de__nav_soup_add_hull(de__nav_soup *s, const de__nav_pose *p, const de_vec3 *pts,
                           u32 count) {
    if (pts == NULL || count < 4u || count > 64u)
        return;
    f64 scale = 0.0;
    for (u32 i = 0; i < count; ++i) {
        const f64 m[3] = {pts[i].x, pts[i].y, pts[i].z};
        for (int k = 0; k < 3; ++k) {
            const f64 a = m[k] < 0.0 ? -m[k] : m[k];
            if (a > scale)
                scale = a;
        }
    }
    const f64 eps = 1e-6 * (scale > 1.0 ? scale : 1.0);
    for (u32 i = 0; i < count; ++i) {
        for (u32 j = i + 1u; j < count; ++j) {
            for (u32 k = j + 1u; k < count; ++k) {
                const f64 e1[3] = {(f64)pts[j].x - pts[i].x, (f64)pts[j].y - pts[i].y,
                                   (f64)pts[j].z - pts[i].z};
                const f64 e2[3] = {(f64)pts[k].x - pts[i].x, (f64)pts[k].y - pts[i].y,
                                   (f64)pts[k].z - pts[i].z};
                const f64 nx = e1[1] * e2[2] - e1[2] * e2[1];
                const f64 ny = e1[2] * e2[0] - e1[0] * e2[2];
                const f64 nz = e1[0] * e2[1] - e1[1] * e2[0];
                const f64 len2 = nx * nx + ny * ny + nz * nz;
                if (!(len2 > 1e-18 * scale * scale * scale * scale))
                    continue;
                const f64 tol = eps * de_dsqrt64(len2);
                bool below = true, above = true;
                for (u32 m = 0; m < count && (below || above); ++m) {
                    if (m == i || m == j || m == k)
                        continue;
                    const f64 d = nx * ((f64)pts[m].x - pts[i].x) +
                                  ny * ((f64)pts[m].y - pts[i].y) +
                                  nz * ((f64)pts[m].z - pts[i].z);
                    if (d > tol)
                        below = false;
                    if (d < -tol)
                        above = false;
                }
                if (below)
                    add_local_tri(s, p, pts[i], pts[j], pts[k]);
                else if (above)
                    add_local_tri(s, p, pts[i], pts[k], pts[j]);
            }
        }
    }
}

/* ---- Tile build ------------------------------------------------------------------------------ */

de_result de__nav_build_tile(const de_allocator *a, const de__nav_settings *st,
                             const de__nav_soup *soup, i32 tx, i32 tz, u8 **data, i32 *size) {
    *data = NULL;
    *size = 0;
    const f32 t = st->desc.tile_size;
    const f32 pad = (f32)st->border * st->desc.cell_size;
    const f32 x0 = (f32)tx * t - pad, x1 = (f32)(tx + 1) * t + pad;
    const f32 z0 = (f32)tz * t - pad, z1 = (f32)(tz + 1) * t + pad;
    const u32 n = soup->tri_count;
    if (n == 0)
        return DE_OK; /* no input: obstacles alone carve nothing */
    i32 *sel = (i32 *)DE_ALLOC(a, sizeof(i32) * 3u * n, 16, DE_MEM_GENERAL);
    if (sel == NULL)
        return DE_ERR_OUT_OF_MEMORY;
    u32 m = 0;
    f32 ymin = 0.0f, ymax = 0.0f;
    for (u32 i = 0; i < soup->tri_count; ++i) {
        const i32 *tri = &soup->tris[i * 3u];
        f32 lo[3], hi[3];
        for (u32 c = 0; c < 3; ++c) {
            const f32 *v = &soup->verts[(u32)tri[c] * 3u];
            for (u32 k = 0; k < 3; ++k) {
                if (c == 0 || v[k] < lo[k])
                    lo[k] = v[k];
                if (c == 0 || v[k] > hi[k])
                    hi[k] = v[k];
            }
        }
        if (hi[0] < x0 || lo[0] > x1 || hi[2] < z0 || lo[2] > z1)
            continue;
        if (m == 0 || lo[1] < ymin)
            ymin = lo[1];
        if (m == 0 || hi[1] > ymax)
            ymax = hi[1];
        memcpy(&sel[m * 3u], tri, sizeof(i32) * 3u);
        ++m;
    }
    de_result r = DE_OK;
    if (m > 0) {
        /* The vertical voxel grid is the same for every tile: multiples of cell_height. */
        const f32 ch = st->desc.cell_height;
        de__nav_rc_tile_config c;
        memset(&c, 0, sizeof c);
        c.cs = st->desc.cell_size;
        c.ch = ch;
        c.walkable_slope_deg = st->slope_deg;
        c.walkable_height = st->walkable_height;
        c.walkable_climb = st->walkable_climb;
        c.walkable_radius = st->walkable_radius;
        c.tile_cells = st->tile_cells;
        c.border = st->border;
        c.tx = tx;
        c.tz = tz;
        c.bmin[0] = (f32)tx * t;
        c.bmin[1] = (de_dfloor(ymin / ch) - 1.0f) * ch;
        c.bmin[2] = (f32)tz * t;
        c.bmax[0] = (f32)(tx + 1) * t;
        c.bmax[1] = (de_dceil(ymax / ch) + 1.0f) * ch;
        c.bmax[2] = (f32)(tz + 1) * t;
        c.agent_radius = st->desc.agent_radius;
        c.agent_height = st->desc.agent_height;
        c.agent_max_climb = st->desc.agent_max_climb;
        c.obstacles = soup->obstacles;
        c.obstacle_count = (i32)soup->obstacle_count;
        r = de__nav_rc_build_tile(a, &c, soup->verts, (i32)soup->vert_count, sel, (i32)m, data,
                                  size);
    }
    DE_FREE(a, sel, sizeof(i32) * 3u * n);
    return r;
}

/* ---- Container ------------------------------------------------------------------------------- */

static void put_u32(u8 *p, u32 v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}
static u32 get_u32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static void put_f32(u8 *p, f32 v) {
    u32 u;
    memcpy(&u, &v, 4);
    put_u32(p, u);
}
static f32 get_f32(const u8 *p) {
    u32 u = get_u32(p);
    f32 v;
    memcpy(&v, &u, 4);
    return v;
}
static void put_f64(u8 *p, f64 v) {
    u64 u;
    memcpy(&u, &v, 8);
    put_u32(p, (u32)u);
    put_u32(p + 4, (u32)(u >> 32));
}
static f64 get_f64(const u8 *p) {
    u64 u = (u64)get_u32(p) | ((u64)get_u32(p + 4) << 32);
    f64 v;
    memcpy(&v, &u, 8);
    return v;
}

static int blob_cmp(const de__nav_tile_blob *a, const de__nav_tile_blob *b) {
    if (a->tz != b->tz)
        return a->tz < b->tz ? -1 : 1;
    if (a->tx != b->tx)
        return a->tx < b->tx ? -1 : 1;
    return 0;
}

de_result de__nav_container_write(const de_allocator *a, const de__nav_container_info *info,
                                  de__nav_tile_blob *tiles, u32 count, u8 **out, u64 *out_size) {
    *out = NULL;
    *out_size = 0;
    /* Insertion sort: tile counts are small, and the order must not depend on qsort. */
    for (u32 i = 1; i < count; ++i) {
        de__nav_tile_blob b = tiles[i];
        u32 j = i;
        while (j > 0 && blob_cmp(&tiles[j - 1u], &b) > 0) {
            tiles[j] = tiles[j - 1u];
            --j;
        }
        tiles[j] = b;
    }
    u64 total = DE__NAV_HEADER_SIZE;
    for (u32 i = 0; i < count; ++i)
        total += 12u + (((u64)tiles[i].size + 3u) & ~(u64)3u);
    if (total > (u64)SIZE_MAX)
        return DE_ERR_OUT_OF_MEMORY;
    u8 *buf = (u8 *)DE_ALLOC(a, (size_t)total, 16, DE_MEM_ASSET);
    if (buf == NULL)
        return DE_ERR_OUT_OF_MEMORY;
    memset(buf, 0, (size_t)total);
    const de_navmesh_build_desc *d = &info->desc;
    put_u32(buf + 0, DE__NAV_MAGIC);
    put_u32(buf + 4, DE__NAV_VERSION);
    put_u32(buf + 8, count);
    put_u32(buf + 12, 0);
    put_f64(buf + 16, info->origin.x);
    put_f64(buf + 24, info->origin.y);
    put_f64(buf + 32, info->origin.z);
    put_f32(buf + 40, d->tile_size);
    put_f32(buf + 44, d->cell_size);
    put_f32(buf + 48, d->cell_height);
    put_f32(buf + 52, d->agent_radius);
    put_f32(buf + 56, d->agent_height);
    put_f32(buf + 60, d->agent_max_climb);
    put_f32(buf + 64, d->agent_max_slope_rad);
    put_u32(buf + 68, d->collision_mask);
    u64 off = DE__NAV_HEADER_SIZE;
    for (u32 i = 0; i < count; ++i) {
        put_u32(buf + off, (u32)tiles[i].tx);
        put_u32(buf + off + 4, (u32)tiles[i].tz);
        put_u32(buf + off + 8, tiles[i].size);
        if (tiles[i].size > 0)
            memcpy(buf + off + 12, tiles[i].data, tiles[i].size);
        off += 12u + (((u64)tiles[i].size + 3u) & ~(u64)3u);
    }
    *out = buf;
    *out_size = total;
    return DE_OK;
}

bool de__nav_container_read(const u8 *data, u64 size, de__nav_container_info *info,
                            const char **why) {
    const char *dummy = NULL;
    if (why == NULL)
        why = &dummy;
    if (data == NULL || size < DE__NAV_HEADER_SIZE) {
        *why = "navmesh: truncated header";
        return false;
    }
    if (get_u32(data) != DE__NAV_MAGIC) {
        *why = "navmesh: not a DNAV container";
        return false;
    }
    if (get_u32(data + 4) != DE__NAV_VERSION) {
        *why = "navmesh: unsupported version";
        return false;
    }
    memset(info, 0, sizeof *info);
    info->tile_count = get_u32(data + 8);
    info->origin.x = get_f64(data + 16);
    info->origin.y = get_f64(data + 24);
    info->origin.z = get_f64(data + 32);
    info->desc.tile_size = get_f32(data + 40);
    info->desc.cell_size = get_f32(data + 44);
    info->desc.cell_height = get_f32(data + 48);
    info->desc.agent_radius = get_f32(data + 52);
    info->desc.agent_height = get_f32(data + 56);
    info->desc.agent_max_climb = get_f32(data + 60);
    info->desc.agent_max_slope_rad = get_f32(data + 64);
    info->desc.collision_mask = get_u32(data + 68);
    de__nav_settings st;
    if (de__nav_settings_make(&info->desc, &st, why) != DE_OK)
        return false;
    u64 off = DE__NAV_HEADER_SIZE;
    for (u32 i = 0; i < info->tile_count; ++i) {
        if (size - off < 12u) {
            *why = "navmesh: truncated tile record";
            return false;
        }
        const u64 n = get_u32(data + off + 8);
        const u64 padded = (n + 3u) & ~(u64)3u;
        if (n == 0 || n > 0x7FFFFFFFu || size - off - 12u < padded) {
            *why = "navmesh: bad tile size";
            return false;
        }
        off += 12u + padded;
    }
    if (off != size) {
        *why = "navmesh: trailing bytes";
        return false;
    }
    return true;
}

bool de__nav_container_tile(const u8 *data, u64 size, u32 index, de__nav_tile_blob *out) {
    if (data == NULL || size < DE__NAV_HEADER_SIZE || index >= get_u32(data + 8))
        return false;
    u64 off = DE__NAV_HEADER_SIZE;
    for (u32 i = 0;; ++i) {
        const u32 n = get_u32(data + off + 8);
        if (i == index) {
            out->tx = (i32)get_u32(data + off);
            out->tz = (i32)get_u32(data + off + 4);
            out->size = n;
            out->data = data + off + 12;
            return true;
        }
        off += 12u + (((u64)n + 3u) & ~(u64)3u);
    }
}
