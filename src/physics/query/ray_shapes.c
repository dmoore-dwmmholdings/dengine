/*
 * ray_shapes.c: a ray against one shape in the shape's own frame (DENG-137). Everything runs in
 * f64 on the f32 shape data, with + - * / and de_dsqrt64 only.
 *
 * The ray is o + t * d with t in [0, max_t] (inclusive). A ray that starts inside (or on) a solid
 * shape reports `inside` with t = 0. Meshes and heightfields are surfaces: they have no inside,
 * and their triangles are two-sided unless DE_PHYSICS_QUERY_IGNORE_BACK_FACES is set. Normals face
 * the ray.
 *
 * Ties inside one shape go to the lower sub-shape: the hit with the smallest t wins, and for
 * equal t the lower triangle / child index. Pruning never uses a strict bound, so a tie is always
 * seen, and the result for a hit at t <= max_t does not depend on max_t.
 */
#include "physics/query/query_internal.h"

/* Slack (in t) added to traversal bounds, so nodes and cells whose computed entry is a rounding
 * step past an equal hit are still visited. Only affects speed, never the result. */
#define Q_NODE_SLACK 1e-9
/* "No entering face yet" in the hull clipping. */
#define Q_NO_FACE 0xFFFFFFFFu

/* ---- Shared helpers -------------------------------------------------------------------- */

bool de__q_slab(de_dvec3 o, de_dvec3 d, de_dvec3 lo, de_dvec3 hi, f64 max_t, f64 *t_enter) {
    const f64 oo[3] = {o.x, o.y, o.z}, dd[3] = {d.x, d.y, d.z};
    const f64 l[3] = {lo.x, lo.y, lo.z}, h[3] = {hi.x, hi.y, hi.z};
    f64 tmin = 0.0, tmax = max_t;
    if (!(tmax >= 0.0))
        return false;
    for (u32 k = 0; k < 3; ++k) {
        if (dd[k] == 0.0) {
            if (oo[k] < l[k] || oo[k] > h[k])
                return false;
            continue;
        }
        f64 t1 = (l[k] - oo[k]) / dd[k];
        f64 t2 = (h[k] - oo[k]) / dd[k];
        if (t1 > t2) {
            const f64 s = t1;
            t1 = t2;
            t2 = s;
        }
        tmin = de__maxd(tmin, t1);
        tmax = de__mind(tmax, t2);
        if (tmin > tmax)
            return false;
    }
    *t_enter = tmin;
    return true;
}

static bool slab_box(de_dvec3 o, de_dvec3 d, const de_aabb *b, f64 max_t, f64 *t_enter) {
    return de__q_slab(o, d, de__d3_from(b->min), de__d3_from(b->max), max_t, t_enter);
}

/* Better (t, sub) than the current best? */
static bool better(f64 t, u32 sub, bool found, f64 best_t, u32 best_sub) {
    if (!found)
        return true;
    if (t != best_t)
        return t < best_t;
    return sub < best_sub;
}

/* First entry into the sphere (c, r) of a ray that starts outside it; false when it misses. */
static bool sphere_entry(de_dvec3 o, de_dvec3 d, de_dvec3 c, f64 r, f64 max_t, f64 *t_out) {
    const de_dvec3 m = de__d3_sub(o, c);
    const f64 a = de__d3_dot(d, d);
    const f64 b = de__d3_dot(m, d);
    const f64 cc = de__d3_dot(m, m) - r * r;
    if (!(a > 0.0) || b >= 0.0)
        return false; /* standing still, or moving away from outside */
    const f64 disc = b * b - a * cc;
    if (disc < 0.0)
        return false;
    /* Smaller root c / (-b + sqrt(disc)): no cancellation (both terms positive). */
    const f64 t = cc / (-b + de_dsqrt64(disc));
    if (!(t <= max_t))
        return false;
    *t_out = t < 0.0 ? 0.0 : t;
    return true;
}

/* Entry into the infinite Z cylinder of radius r for a ray that starts outside it. */
static bool zcyl_entry(de_dvec3 o, de_dvec3 d, f64 r, f64 max_t, f64 *t_out) {
    const f64 a = d.x * d.x + d.y * d.y;
    const f64 b = o.x * d.x + o.y * d.y;
    const f64 c = o.x * o.x + o.y * o.y - r * r;
    if (!(a > 0.0) || !(c > 0.0) || b >= 0.0)
        return false;
    const f64 disc = b * b - a * c;
    if (disc < 0.0)
        return false;
    const f64 t = c / (-b + de_dsqrt64(disc));
    if (!(t <= max_t))
        return false;
    *t_out = t < 0.0 ? 0.0 : t;
    return true;
}

static de_dvec3 at(de_dvec3 o, de_dvec3 d, f64 t) {
    return de__d3_add(o, de__d3_scale(d, t));
}

static void set_inside(de__qlocal_hit *out) {
    out->t = 0.0;
    out->normal = de__d3(0.0, 0.0, 0.0);
    out->sub = DE_SUB_SHAPE_NONE;
    out->inside = true;
}

static void set_hit(de__qlocal_hit *out, f64 t, de_dvec3 n, u32 sub) {
    out->t = t;
    out->normal = de__q_dnorm(n);
    out->sub = sub;
    out->inside = false;
}

/* ---- Primitives ------------------------------------------------------------------------ */

static bool ray_sphere(f64 r, de_dvec3 o, de_dvec3 d, f64 max_t, de__qlocal_hit *out) {
    if (de__d3_dot(o, o) <= r * r) {
        set_inside(out);
        return true;
    }
    f64 t = 0.0;
    if (!sphere_entry(o, d, de__d3(0.0, 0.0, 0.0), r, max_t, &t))
        return false;
    set_hit(out, t, at(o, d, t), DE_SUB_SHAPE_NONE);
    return true;
}

static bool ray_capsule(f64 r, f64 h, de_dvec3 o, de_dvec3 d, f64 max_t, de__qlocal_hit *out) {
    const f64 zc = o.z < -h ? -h : (o.z > h ? h : o.z);
    if (o.x * o.x + o.y * o.y + (o.z - zc) * (o.z - zc) <= r * r) {
        set_inside(out);
        return true;
    }
    /* Outside the capsule: the entry is the first entry into the clipped side or an end sphere
     * (each lies inside the capsule, and the capsule's boundary is made of them). */
    bool found = false;
    f64 best = 0.0;
    de_dvec3 n = de__d3(0.0, 0.0, 0.0);
    f64 t = 0.0;
    if (zcyl_entry(o, d, r, max_t, &t)) {
        const de_dvec3 p = at(o, d, t);
        if (p.z >= -h && p.z <= h) {
            found = true;
            best = t;
            n = de__d3(p.x, p.y, 0.0);
        }
    }
    for (u32 e = 0; e < 2; ++e) {
        const de_dvec3 c = de__d3(0.0, 0.0, e == 0 ? -h : h);
        if (sphere_entry(o, d, c, r, max_t, &t) && (!found || t < best)) {
            found = true;
            best = t;
            n = de__d3_sub(at(o, d, t), c);
        }
    }
    if (!found)
        return false;
    set_hit(out, best, n, DE_SUB_SHAPE_NONE);
    return true;
}

static bool ray_box(de_vec3 he, de_dvec3 o, de_dvec3 d, f64 max_t, de__qlocal_hit *out) {
    const f64 h[3] = {he.x, he.y, he.z};
    const f64 oo[3] = {o.x, o.y, o.z}, dd[3] = {d.x, d.y, d.z};
    if (de__absd(oo[0]) <= h[0] && de__absd(oo[1]) <= h[1] && de__absd(oo[2]) <= h[2]) {
        set_inside(out);
        return true;
    }
    f64 tmin = 0.0, tmax = max_t;
    u32 axis = 3u;
    for (u32 k = 0; k < 3; ++k) {
        if (dd[k] == 0.0) {
            if (oo[k] < -h[k] || oo[k] > h[k])
                return false;
            continue;
        }
        f64 t1 = (-h[k] - oo[k]) / dd[k];
        f64 t2 = (h[k] - oo[k]) / dd[k];
        if (t1 > t2) {
            const f64 s = t1;
            t1 = t2;
            t2 = s;
        }
        /* An axis where o is outside the slab enters at t1 > 0; strict: ties to the lower axis. */
        if (t1 > tmin) {
            tmin = t1;
            axis = k;
        }
        tmax = de__mind(tmax, t2);
        if (tmin > tmax)
            return false;
    }
    if (axis == 3u)
        return false;
    f64 n[3] = {0.0, 0.0, 0.0};
    n[axis] = dd[axis] > 0.0 ? -1.0 : 1.0;
    set_hit(out, tmin, de__d3(n[0], n[1], n[2]), DE_SUB_SHAPE_NONE);
    return true;
}

static bool ray_cylinder(f64 r, f64 h, de_dvec3 o, de_dvec3 d, f64 max_t, de__qlocal_hit *out) {
    if (o.x * o.x + o.y * o.y <= r * r && de__absd(o.z) <= h) {
        set_inside(out);
        return true;
    }
    bool found = false;
    f64 best = 0.0, t = 0.0;
    de_dvec3 n = de__d3(0.0, 0.0, 0.0);
    if (zcyl_entry(o, d, r, max_t, &t)) {
        const de_dvec3 p = at(o, d, t);
        if (p.z >= -h && p.z <= h) {
            found = true;
            best = t;
            n = de__d3(p.x, p.y, 0.0);
        }
    }
    for (u32 e = 0; e < 2; ++e) {
        const f64 zc = e == 0 ? h : -h;
        const bool from_out = e == 0 ? (o.z > h && d.z < 0.0) : (o.z < -h && d.z > 0.0);
        if (!from_out)
            continue;
        t = (zc - o.z) / d.z;
        if (!(t <= max_t))
            continue;
        const de_dvec3 p = at(o, d, t);
        if (p.x * p.x + p.y * p.y <= r * r && (!found || t < best)) {
            found = true;
            best = t;
            n = de__d3(0.0, 0.0, e == 0 ? 1.0 : -1.0);
        }
    }
    if (!found)
        return false;
    set_hit(out, best, n, DE_SUB_SHAPE_NONE);
    return true;
}

/* Cyrus-Beck clipping against the hull's face planes. */
static bool ray_hull(const de__hull *hull, de_dvec3 o, de_dvec3 d, f64 max_t, de__qlocal_hit *out) {
    bool inside = true;
    for (u32 f = 0; f < hull->face_count; ++f) {
        const de_plane *pl = &hull->planes[f];
        if (de__d3_dot(de__d3_from(pl->n), o) + (f64)pl->d > 0.0) {
            inside = false;
            break;
        }
    }
    if (inside) {
        set_inside(out);
        return true;
    }
    f64 t_in = 0.0, t_out = max_t;
    u32 face = Q_NO_FACE;
    for (u32 f = 0; f < hull->face_count; ++f) {
        const de_plane *pl = &hull->planes[f];
        const de_dvec3 n = de__d3_from(pl->n);
        const f64 num = -(de__d3_dot(n, o) + (f64)pl->d); /* < 0: o is outside this plane */
        const f64 den = de__d3_dot(n, d);
        if (den == 0.0) {
            if (num < 0.0)
                return false; /* parallel and outside */
            continue;
        }
        const f64 t = num / den;
        if (den < 0.0) { /* entering; strict: ties to the lower face */
            if (t > t_in) {
                t_in = t;
                face = f;
            }
        } else if (t < t_out) {
            t_out = t;
        }
        if (t_in > t_out)
            return false;
    }
    if (face == Q_NO_FACE)
        return false;
    set_hit(out, t_in, de__d3_from(hull->planes[face].n), DE_SUB_SHAPE_NONE);
    return true;
}

/* ---- Triangles (watertight, Woop, Benthin and Wald 2013) -------------------------------- */

typedef struct tri_ray {
    de_dvec3 o, d;
    u32 kx, ky, kz;
    f64 sx, sy, sz;
    u32 flags;
} tri_ray;

static void tri_ray_init(tri_ray *r, de_dvec3 o, de_dvec3 d, u32 flags) {
    r->o = o;
    r->d = d;
    r->flags = flags;
    const f64 dd[3] = {d.x, d.y, d.z};
    const f64 ax = de__absd(d.x), ay = de__absd(d.y), az = de__absd(d.z);
    u32 kz = 0;
    if (ay > ax)
        kz = 1;
    if (az > (kz == 0 ? ax : ay))
        kz = 2;
    u32 kx = (kz + 1u) % 3u, ky = (kx + 1u) % 3u;
    if (dd[kz] < 0.0) { /* keep the winding */
        const u32 s = kx;
        kx = ky;
        ky = s;
    }
    r->kx = kx;
    r->ky = ky;
    r->kz = kz;
    r->sx = dd[kx] / dd[kz];
    r->sy = dd[ky] / dd[kz];
    r->sz = 1.0 / dd[kz];
}

static f64 comp(de_dvec3 v, u32 k) {
    return k == 0 ? v.x : (k == 1 ? v.y : v.z);
}

/* Hit parameter of the triangle (a, b, c); the edge tests use the same per-vertex values for
 * every triangle sharing an edge, so a ray never slips between two neighbours. */
static bool ray_tri(const tri_ray *r, de_vec3 a, de_vec3 b, de_vec3 c, f64 max_t, f64 *t_out,
                    de_dvec3 *n_out) {
    const de_dvec3 A = de__d3_sub(de__d3_from(a), r->o);
    const de_dvec3 B = de__d3_sub(de__d3_from(b), r->o);
    const de_dvec3 C = de__d3_sub(de__d3_from(c), r->o);
    const f64 az = comp(A, r->kz), bz = comp(B, r->kz), cz = comp(C, r->kz);
    const f64 ax = comp(A, r->kx) - r->sx * az, ay = comp(A, r->ky) - r->sy * az;
    const f64 bx = comp(B, r->kx) - r->sx * bz, by = comp(B, r->ky) - r->sy * bz;
    const f64 cx = comp(C, r->kx) - r->sx * cz, cy = comp(C, r->ky) - r->sy * cz;
    const f64 U = cx * by - cy * bx;
    const f64 V = ax * cy - ay * cx;
    const f64 W = bx * ay - by * ax;
    if ((U < 0.0 || V < 0.0 || W < 0.0) && (U > 0.0 || V > 0.0 || W > 0.0))
        return false;
    const f64 det = U + V + W;
    if (det == 0.0)
        return false;
    const f64 T = U * (r->sz * az) + V * (r->sz * bz) + W * (r->sz * cz);
    const f64 t = T / det;
    if (!(t >= 0.0) || !(t <= max_t))
        return false;
    /* Geometric normal; front = counter-clockwise side. */
    const de_dvec3 n = de__d3_cross(de__d3_sub(de__d3_from(b), de__d3_from(a)),
                                    de__d3_sub(de__d3_from(c), de__d3_from(a)));
    const f64 dn = de__d3_dot(n, r->d);
    if ((r->flags & (u32)DE_PHYSICS_QUERY_IGNORE_BACK_FACES) != 0u && !(dn < 0.0))
        return false;
    *t_out = t;
    *n_out = dn > 0.0 ? de__d3_scale(n, -1.0) : n;
    return true;
}

/* ---- Mesh ------------------------------------------------------------------------------- */

static bool ray_mesh(const de__mesh *m, de_dvec3 o, de_dvec3 d, f64 max_t, u32 flags,
                     de__qlocal_hit *out) {
    const de__bvh *bvh = &m->bvh;
    if (bvh->node_count == 0)
        return false;
    tri_ray r;
    tri_ray_init(&r, o, d, flags);
    bool found = false;
    f64 best_t = max_t;
    u32 best_sub = 0;
    de_dvec3 best_n = de__d3(0.0, 0.0, 0.0);
    u32 stack[2u * DE__BVH_MAX_DEPTH + 2u];
    u32 sp = 0;
    stack[sp++] = 0;
    while (sp > 0) {
        const de__bvh_node *node = &bvh->nodes[stack[--sp]];
        f64 te = 0.0;
        if (!slab_box(o, d, &node->bounds, best_t + Q_NODE_SLACK, &te))
            continue;
        if (node->count > 0) {
            for (u32 k = 0; k < node->count; ++k) {
                const u32 tri = bvh->prims[node->first + k];
                if (m->edge_flags[tri] & DE__MESH_TRI_DEGENERATE)
                    continue;
                const u32 *ix = &m->indices[3u * tri];
                f64 t = 0.0;
                de_dvec3 n = {0.0, 0.0, 0.0};
                if (!ray_tri(&r, m->vertices[ix[0]], m->vertices[ix[1]], m->vertices[ix[2]], max_t,
                             &t, &n))
                    continue;
                if (better(t, tri, found, best_t, best_sub)) {
                    found = true;
                    best_t = t;
                    best_sub = tri;
                    best_n = n;
                }
            }
        } else if (sp + 2u <= 2u * DE__BVH_MAX_DEPTH + 2u) {
            stack[sp++] = node->first + 1u; /* right, popped after the left */
            stack[sp++] = node->first;
        }
    }
    if (!found)
        return false;
    set_hit(out, best_t, best_n, best_sub);
    return true;
}

/* ---- Heightfield (2D DDA over the cells) ------------------------------------------------ */

/* floor(x) for |x| well inside the i64 range, clamped to [lo, hi]. */
static i64 floor_clamp(f64 x, i64 lo, i64 hi) {
    if (!(x > (f64)lo))
        return lo;
    if (!(x < (f64)hi))
        return hi;
    i64 i = (i64)x; /* truncates toward zero */
    if ((f64)i > x)
        --i;
    return i < lo ? lo : (i > hi ? hi : i);
}

static bool ray_heightfield(const de__shape *s, de_dvec3 o, de_dvec3 d, f64 max_t, u32 flags,
                            de__qlocal_hit *out) {
    const de__heightfield *hf = &s->u.hf;
    const u32 cells_x = hf->cols - 1u, cells_y = hf->rows - 1u;
    const f64 sx = hf->scale.x, sy = hf->scale.y;
    /* Clip to the grid's box (x, y extent; z from the local box). */
    const de_dvec3 lo = de__d3(0.0, 0.0, s->local_aabb.min.z);
    const de_dvec3 hi = de__d3((f64)cells_x * sx, (f64)cells_y * sy, s->local_aabb.max.z);
    f64 t0;
    if (!de__q_slab(o, d, lo, hi, max_t, &t0))
        return false;
    tri_ray r;
    tri_ray_init(&r, o, d, flags);
    const de_dvec3 p0 = at(o, d, t0);
    i64 cx = floor_clamp(p0.x / sx, 0, (i64)cells_x - 1);
    i64 cy = floor_clamp(p0.y / sy, 0, (i64)cells_y - 1);
    const i64 step_x = d.x > 0.0 ? 1 : (d.x < 0.0 ? -1 : 0);
    const i64 step_y = d.y > 0.0 ? 1 : (d.y < 0.0 ? -1 : 0);
    bool found = false;
    f64 best_t = max_t;
    u32 best_sub = 0;
    de_dvec3 best_n = de__d3(0.0, 0.0, 0.0);
    const u32 max_steps = cells_x + cells_y + 2u;
    for (u32 it = 0; it < max_steps; ++it) {
        /* Parameter where the ray leaves the current cell (its x and y boundaries). */
        f64 tx = 1e300, ty = 1e300;
        if (step_x != 0)
            tx = ((f64)(cx + (step_x > 0 ? 1 : 0)) * sx - o.x) / d.x;
        if (step_y != 0)
            ty = ((f64)(cy + (step_y > 0 ? 1 : 0)) * sy - o.y) / d.y;
        const f64 t_exit = de__mind(de__mind(tx, ty), max_t);
        const u32 c = (u32)cx, rw = (u32)cy;
        if (!de__hf_cell_is_hole(hf, c, rw)) {
            de_vec3 tri[2][3];
            de__hf_cell_triangles(hf, c, rw, tri);
            for (u32 k = 0; k < 2; ++k) {
                f64 t = 0.0;
                de_dvec3 n = {0.0, 0.0, 0.0};
                const u32 sub = 2u * (rw * cells_x + c) + k;
                if (ray_tri(&r, tri[k][0], tri[k][1], tri[k][2], max_t, &t, &n) &&
                    better(t, sub, found, best_t, best_sub)) {
                    found = true;
                    best_t = t;
                    best_sub = sub;
                    best_n = n;
                }
            }
        }
        /* Later cells start at t_exit (up to rounding): a hit clearly before it is final. */
        if (found && best_t + Q_NODE_SLACK < t_exit)
            break;
        if (t_exit >= max_t)
            break;
        if (tx <= ty) { /* a corner: x first, then y on the next step */
            cx += step_x;
            if (cx < 0 || cx >= (i64)cells_x)
                break;
        } else {
            cy += step_y;
            if (cy < 0 || cy >= (i64)cells_y)
                break;
        }
    }
    if (!found)
        return false;
    set_hit(out, best_t, best_n, best_sub);
    return true;
}

/* ---- Compound --------------------------------------------------------------------------- */

static bool ray_compound(const de__compound *cp, de_dvec3 o, de_dvec3 d, f64 max_t, u32 flags,
                         de__qlocal_hit *out) {
    const de__bvh *bvh = &cp->bvh;
    if (bvh->node_count == 0)
        return false;
    bool found = false;
    f64 best_t = max_t;
    u32 best_sub = 0;
    de__qlocal_hit best;
    memset(&best, 0, sizeof best);
    u32 stack[2u * DE__BVH_MAX_DEPTH + 2u];
    u32 sp = 0;
    stack[sp++] = 0;
    while (sp > 0) {
        const de__bvh_node *node = &bvh->nodes[stack[--sp]];
        f64 te = 0.0;
        if (!slab_box(o, d, &node->bounds, best_t + Q_NODE_SLACK, &te))
            continue;
        if (node->count > 0) {
            for (u32 k = 0; k < node->count; ++k) {
                const u32 ci = bvh->prims[node->first + k];
                const de__compound_child *ch = &cp->children[ci];
                if (!slab_box(o, d, &ch->bounds, best_t + Q_NODE_SLACK, &te))
                    continue;
                const de__dmat3 rc = de__dmat3_from_quat(ch->rotation);
                const de_dvec3 oc = de__dmat3_tmul(&rc, de__d3_sub(o, de__d3_from(ch->position)));
                const de_dvec3 dc = de__dmat3_tmul(&rc, d);
                de__qlocal_hit h;
                if (!de__ray_shape(ch->shape, oc, dc, max_t, flags, &h))
                    continue;
                if (better(h.t, ci, found, best_t, best_sub)) {
                    found = true;
                    best_t = h.t;
                    best_sub = ci;
                    best = h;
                    best.normal = de__dmat3_mul(&rc, h.normal);
                }
            }
        } else if (sp + 2u <= 2u * DE__BVH_MAX_DEPTH + 2u) {
            stack[sp++] = node->first + 1u;
            stack[sp++] = node->first;
        }
    }
    if (!found)
        return false;
    *out = best;
    out->sub = best_sub;
    return true;
}

/* ---- Dispatch --------------------------------------------------------------------------- */

bool de__ray_shape(const de__shape *s, de_dvec3 o, de_dvec3 d, f64 max_t, u32 flags,
                   de__qlocal_hit *out) {
    if (s == NULL || !(max_t >= 0.0))
        return false;
    switch (s->kind) {
    case DE_SHAPE_SPHERE:
        return ray_sphere(s->u.sphere.radius, o, d, max_t, out);
    case DE_SHAPE_CAPSULE:
        return ray_capsule(s->u.capsule.radius, s->u.capsule.half_height, o, d, max_t, out);
    case DE_SHAPE_BOX:
        return ray_box(s->u.box.half_extents, o, d, max_t, out);
    case DE_SHAPE_CYLINDER:
        return ray_cylinder(s->u.cylinder.radius, s->u.cylinder.half_height, o, d, max_t, out);
    case DE_SHAPE_CONVEX:
        return ray_hull(&s->u.hull, o, d, max_t, out);
    case DE_SHAPE_MESH:
        return ray_mesh(&s->u.mesh, o, d, max_t, flags, out);
    case DE_SHAPE_HEIGHTFIELD:
        return ray_heightfield(s, o, d, max_t, flags, out);
    case DE_SHAPE_COMPOUND:
        return ray_compound(&s->u.compound, o, d, max_t, flags, out);
    }
    return false;
}
