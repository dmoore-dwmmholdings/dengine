/*
 * sphere_capsule.c: the specialized paths of spheres and capsules (DENG-134).
 *
 * A sphere is a point core and a capsule a segment core, each swept by its radius. Sphere and
 * capsule pairs use segment closest points (two points when two capsules are near-parallel);
 * sphere-box clamps the center into the box. Against a polytope (box, hull, triangle) the core's
 * distance comes from GJK; a capsule lying on a face is clipped to it for two points; when the
 * core touches or enters the polytope, SAT over the polytope's faces and the edge-segment cross
 * axes gives the contact.
 */
#include "physics/narrow/narrowphase.h"

/* ---- Segment against segment ----------------------------------------------------------- */

typedef struct seg {
    de_vec3 p0, p1;
    f32 r;
    u32 f0, f1, fe; /* end point and interior features */
} seg;

static u32 seg_feature(const seg *s, f32 t) {
    return t <= 0.0f ? s->f0 : (t >= 1.0f ? s->f1 : s->fe);
}

static bool seg_seg(const seg *a, const seg *b, const de__np_rel *rel, de__np_manifold *out) {
    out->count = 0;
    f32 s, t;
    de_vec3 ca, cb;
    de__np_segments_closest(a->p0, a->p1, b->p0, b->p1, &s, &t, &ca, &cb);
    const de_vec3 d = de__v3_sub(cb, ca);
    const f32 dist = de__np_len(d);
    if (dist > a->r + b->r + rel->spec)
        return false;
    /* Coincident cores: world +Z. */
    const de_vec3 n = dist > 1e-6f ? de__np_normalize(d, rel->up) : rel->up;
    out->normal = n;

    const de_vec3 da = de__v3_sub(a->p1, a->p0), db = de__v3_sub(b->p1, b->p0);
    const f32 la2 = de__np_len2(da), lb2 = de__np_len2(db);
    if (la2 > 1e-12f && lb2 > 1e-12f &&
        de__np_len2(de__v3_cross(da, db)) <= DE__NP_PARALLEL_SIN2 * la2 * lb2) {
        /* Near-parallel: the overlap of B's projection onto A gives two points. */
        const f32 sb0 = de__v3_dot(de__v3_sub(b->p0, a->p0), da) / la2;
        const f32 sb1 = de__v3_dot(de__v3_sub(b->p1, a->p0), da) / la2;
        const bool b0_low = sb0 <= sb1;
        const f32 smin = b0_low ? sb0 : sb1, smax = b0_low ? sb1 : sb0;
        const f32 lo = smin > 0.0f ? smin : 0.0f, hi = smax < 1.0f ? smax : 1.0f;
        if ((hi - lo) * (hi - lo) * la2 > 1e-6f) {
            for (u32 k = 0; k < 2; ++k) {
                const f32 sa = k == 0 ? lo : hi;
                const bool from_a = k == 0 ? !(smin > 0.0f) : !(smax < 1.0f);
                const de_vec3 pa_core = de__v3_add(a->p0, de__v3_scale(da, sa));
                const f32 tb = de__np_segment_closest_t(b->p0, b->p1, pa_core);
                const de_vec3 pb_core = de__v3_add(b->p0, de__v3_scale(db, tb));
                const f32 sep = de__v3_dot(de__v3_sub(pb_core, pa_core), n) - a->r - b->r;
                u32 fa, fb;
                if (from_a) {
                    fa = k == 0 ? a->f0 : a->f1;
                    fb = b->fe;
                } else {
                    fa = a->fe;
                    fb = (k == 0) == b0_low ? b->f0 : b->f1;
                }
                de__np_add(out, de__v3_add(pa_core, de__v3_scale(n, a->r)),
                           de__v3_sub(pb_core, de__v3_scale(n, b->r)), sep, de__feature_id(fa, fb),
                           rel->spec);
            }
            if (out->count > 0)
                return true;
        }
    }
    de__np_add(out, de__v3_add(ca, de__v3_scale(n, a->r)), de__v3_sub(cb, de__v3_scale(n, b->r)),
               dist - a->r - b->r, de__feature_id(seg_feature(a, s), seg_feature(b, t)), rel->spec);
    return out->count > 0;
}

static void sphere_seg(const de__shape *s, de_vec3 c, seg *out) {
    out->p0 = out->p1 = c;
    out->r = s->u.sphere.radius;
    out->f0 = out->f1 = out->fe = de__feat(DE__FEAT_SHAPE, 0);
}

static void capsule_seg(const de__shape *s, const de__np_rel *rel, seg *out) {
    const f32 h = s->u.capsule.half_height;
    out->p0 = de__v3(0.0f, 0.0f, -h);
    out->p1 = de__v3(0.0f, 0.0f, h);
    if (rel != NULL) {
        out->p0 = de__np_to_a(rel, out->p0);
        out->p1 = de__np_to_a(rel, out->p1);
    }
    out->r = s->u.capsule.radius;
    out->f0 = de__feat(DE__FEAT_VERTEX, 0);
    out->f1 = de__feat(DE__FEAT_VERTEX, 1);
    out->fe = de__feat(DE__FEAT_EDGE, 0);
}

bool de__np_sphere_sphere(const de__shape *a, const de__shape *b, const de__np_rel *rel,
                          de__np_manifold *out) {
    seg sa, sb;
    sphere_seg(a, de__v3(0.0f, 0.0f, 0.0f), &sa);
    sphere_seg(b, rel->t, &sb);
    return seg_seg(&sa, &sb, rel, out);
}

bool de__np_capsule_sphere(const de__shape *a, const de__shape *b, const de__np_rel *rel,
                           de__np_manifold *out) {
    seg sa, sb;
    capsule_seg(a, NULL, &sa);
    sphere_seg(b, rel->t, &sb);
    return seg_seg(&sa, &sb, rel, out);
}

bool de__np_capsule_capsule(const de__shape *a, const de__shape *b, const de__np_rel *rel,
                            de__np_manifold *out) {
    seg sa, sb;
    capsule_seg(a, NULL, &sa);
    capsule_seg(b, rel, &sb);
    return seg_seg(&sa, &sb, rel, out);
}

/* ---- Sphere against box ---------------------------------------------------------------- */

static f32 comp(de_vec3 v, u32 i) {
    return i == 0 ? v.x : (i == 1 ? v.y : v.z);
}

bool de__np_box_sphere(const de__shape *a, const de__shape *b, const de__np_rel *rel,
                       de__np_manifold *out) {
    out->count = 0;
    const de_vec3 e = a->u.box.half_extents;
    const f32 r = b->u.sphere.radius;
    const de_vec3 c = rel->t;
    f32 q[3];
    u32 clamped = 0, mask = 0;
    for (u32 i = 0; i < 3; ++i) {
        const f32 ci = comp(c, i), ei = comp(e, i);
        q[i] = ci > ei ? ei : (ci < -ei ? -ei : ci);
        if (q[i] != ci) {
            ++clamped;
            mask |= 1u << i;
        }
    }
    const u32 fb = de__feat(DE__FEAT_SHAPE, 0);
    if (clamped > 0) {
        const de_vec3 qa = de__v3(q[0], q[1], q[2]);
        const de_vec3 d = de__v3_sub(c, qa);
        const f32 dist = de__np_len(d);
        if (dist > r + rel->spec)
            return false;
        const de_vec3 n = de__np_normalize(d, rel->up);
        u32 fa;
        if (clamped == 1) {
            const u32 i = mask == 1u ? 0u : (mask == 2u ? 1u : 2u);
            fa = de__feat(DE__FEAT_FACE, 2u * i + (comp(c, i) < 0.0f ? 1u : 0u));
        } else {
            u32 v = 0;
            for (u32 i = 0; i < 3; ++i)
                if (comp(c, i) >= 0.0f)
                    v |= 1u << i;
            if (clamped == 3) {
                fa = de__feat(DE__FEAT_VERTEX, v);
            } else {
                const u32 free_axis = (~mask) & 7u;
                const u32 v0 = v & ~free_axis;
                fa = de__feat(DE__FEAT_EDGE, de__np_box_edge(v0, v0 | free_axis));
            }
        }
        out->normal = n;
        de__np_add(out, qa, de__v3_sub(c, de__v3_scale(n, r)), dist - r, de__feature_id(fa, fb),
                   rel->spec);
        return out->count > 0;
    }
    /* Center inside: push out through the nearest face (ties: the lowest axis). */
    u32 axis = 0;
    f32 pen = comp(e, 0) - de__absf(c.x);
    for (u32 i = 1; i < 3; ++i) {
        const f32 p = comp(e, i) - de__absf(comp(c, i));
        if (p < pen) {
            pen = p;
            axis = i;
        }
    }
    const f32 sgn = comp(c, axis) >= 0.0f ? 1.0f : -1.0f;
    de_vec3 n = de__v3(0.0f, 0.0f, 0.0f);
    de_vec3 pa = c;
    if (axis == 0) {
        n.x = sgn;
        pa.x = sgn * e.x;
    } else if (axis == 1) {
        n.y = sgn;
        pa.y = sgn * e.y;
    } else {
        n.z = sgn;
        pa.z = sgn * e.z;
    }
    out->normal = n;
    de__np_add(out, pa, de__v3_sub(c, de__v3_scale(n, r)), -pen - r,
               de__feature_id(de__feat(DE__FEAT_FACE, 2u * axis + (sgn < 0.0f ? 1u : 0u)), fb),
               rel->spec);
    return out->count > 0;
}

/* ---- Polytope against a segment core --------------------------------------------------- */

/* One point: core point q of B, its feature fb, at distance `dist` from A's point pa along n. */
static void one_point(de__np_manifold *out, de_vec3 n, de_vec3 pa, de_vec3 q, f32 rb, f32 sep,
                      u32 fa, u32 fb, f32 spec) {
    out->count = 0;
    out->normal = n;
    de__np_add(out, pa, de__v3_sub(q, de__v3_scale(n, rb)), sep, de__feature_id(fa, fb), spec);
}

/* Contact against the triangle's front face (internal-edge correction). q: B's closest core
 * point, used when the clipped segment misses the triangle. */
static bool tri_front(const de__np_tri *tri, de_vec3 p0, de_vec3 p1, de_vec3 q, f32 rb, u32 f0,
                      u32 f1, u32 fe, u32 fq, f32 spec, de__np_manifold *out) {
    out->count = 0;
    if (de__np_clip_segment(&tri->poly, 0, p0, p1, rb, f0, f1, fe, spec, out) > 0)
        return true;
    const de_vec3 n = tri->n;
    const f32 dist = de__v3_dot(n, q) + tri->p[0].d;
    one_point(out, n, de__v3_sub(q, de__v3_scale(n, dist)), q, rb, dist - rb,
              de__feat(DE__FEAT_FACE, 0), fq, spec);
    return out->count > 0;
}

bool de__np_poly_segment(const de__np_poly *a, const de__np_tri *tri, de_vec3 p0, de_vec3 p1,
                         f32 rb, u32 f0, u32 f1, u32 fe, const de__np_rel *rel,
                         de__np_manifold *out) {
    out->count = 0;
    const f32 spec = rel->spec;
    const bool point = p0.x == p1.x && p0.y == p1.y && p0.z == p1.z;
    de_vec3 segpts[2] = {p0, p1};
    de__gjk_proxy pa, pb;
    de__gjk_proxy_points(&pa, a->verts, a->vcount, NULL);
    de__gjk_proxy_points(&pb, segpts, point ? 1u : 2u, NULL);
    de__gjk_result g;
    de__gjk_distance(&pa, &pb, &g);
    if (!g.overlap && g.distance > (f64)(rb + spec))
        return false;

    if (!g.overlap && g.distance > (f64)DE__NP_CORE_EPS) {
        /* Separated cores: the GJK closest points. */
        const de_vec3 ca = de__v3((f32)g.point_a.x, (f32)g.point_a.y, (f32)g.point_a.z);
        const de_vec3 cb = de__v3((f32)g.point_b.x, (f32)g.point_b.y, (f32)g.point_b.z);
        const de_vec3 n = de__np_normalize(de__v3_sub(cb, ca), rel->up);
        const f32 tq = point ? 0.0f : de__np_segment_closest_t(p0, p1, cb);
        const u32 fq = point ? f0 : (tq <= 0.0f ? f0 : (tq >= 1.0f ? f1 : fe));
        const u32 fa = de__np_poly_support_feature(a, n);
        if (tri != NULL && !de__np_tri_feature_ok(tri, fa, n))
            return tri_front(tri, p0, p1, cb, rb, f0, f1, fe, fq, spec, out);
        if (!point && de__feat_type(fa) == DE__FEAT_FACE) {
            /* A capsule lying on a face: clip the segment to it for up to two points. */
            const u32 f = de__feat_index(fa);
            if (de__v3_dot(a->planes[f].n, n) >= DE__NP_FACE_COS &&
                de__np_clip_segment(a, f, p0, p1, rb, f0, f1, fe, spec, out) >= 2)
                return true;
            out->count = 0;
        }
        one_point(out, n, ca, cb, rb, (f32)g.distance - rb, fa, fq, spec);
        return out->count > 0;
    }

    /* Cores touch or overlap: SAT over A's faces and the edge x segment axes. */
    f32 sep_f = -3.0e38f;
    u32 face = 0;
    for (u32 f = 0; f < a->fcount; ++f) {
        const de_vec3 n = a->planes[f].n;
        const f32 d0 = de__v3_dot(n, p0), d1 = de__v3_dot(n, p1);
        const f32 s = (d0 < d1 ? d0 : d1) + a->planes[f].d;
        if (s > sep_f) {
            sep_f = s;
            face = f;
        }
    }
    f32 sep_e = -3.0e38f;
    u32 edge = 0;
    de_vec3 axis = de__v3(0.0f, 0.0f, 0.0f);
    bool have_edge = false;
    const de_vec3 sd = de__v3_sub(p1, p0);
    if (!point) {
        for (u32 e = 0; e < a->ecount; ++e) {
            if (a->edges[e].twin < e)
                continue;
            if (tri != NULL && (e > 2u || (tri->active & (1u << e)) == 0))
                continue;
            const de_vec3 a0 = a->verts[a->edges[e].origin];
            const de_vec3 a1 = a->verts[a->edges[a->edges[e].next].origin];
            const de_vec3 da = de__v3_sub(a1, a0);
            de_vec3 l = de__v3_cross(da, sd);
            if (de__np_len2(l) < 1e-6f * de__np_len2(da) * de__np_len2(sd))
                continue;
            l = de__np_normalize(l, l);
            f32 max_a = de__v3_dot(l, a->verts[0]), min_a = max_a;
            for (u32 v = 1; v < a->vcount; ++v) {
                const f32 x = de__v3_dot(l, a->verts[v]);
                max_a = x > max_a ? x : max_a;
                min_a = x < min_a ? x : min_a;
            }
            const f32 q0 = de__v3_dot(l, p0); /* the segment is orthogonal to l: one value */
            f32 s, sa;
            if (min_a - q0 > q0 - max_a) {
                l = de__v3(-l.x, -l.y, -l.z);
                s = min_a - q0;
                sa = -min_a;
            } else {
                s = q0 - max_a;
                sa = max_a;
            }
            const f32 tol = 1e-5f + 1e-6f * de__absf(sa);
            if (de__absf(de__v3_dot(l, a0) - sa) > tol || de__absf(de__v3_dot(l, a1) - sa) > tol)
                continue; /* the edge does not support the axis */
            if (s > sep_e) {
                sep_e = s;
                edge = e;
                axis = l;
                have_edge = true;
            }
        }
    }
    bool use_edge = have_edge && sep_e > sep_f + DE__NP_EDGE_TOL;
    if (tri != NULL &&
        ((use_edge && !(de__v3_dot(axis, tri->n) > 0.0f)) || (!use_edge && face != 0))) {
        use_edge = false;
        face = 0;
    }
    if (use_edge) {
        const de_vec3 a0 = a->verts[a->edges[edge].origin];
        const de_vec3 a1 = a->verts[a->edges[a->edges[edge].next].origin];
        f32 s, t;
        de_vec3 ca, cb;
        de__np_segments_closest(a0, a1, p0, p1, &s, &t, &ca, &cb);
        const u32 fq = t <= 0.0f ? f0 : (t >= 1.0f ? f1 : fe);
        one_point(out, axis, ca, cb, rb, de__v3_dot(de__v3_sub(cb, ca), axis) - rb,
                  de__feat(DE__FEAT_EDGE, de__np_edge_id(a, edge)), fq, spec);
        return out->count > 0;
    }
    if (de__np_clip_segment(a, face, p0, p1, rb, f0, f1, fe, spec, out) > 0)
        return true;
    /* The segment misses the face: its deepest end point. */
    const de_vec3 n = a->planes[face].n;
    const bool first = de__v3_dot(n, p0) <= de__v3_dot(n, p1);
    const de_vec3 q = first ? p0 : p1;
    const f32 dist = de__v3_dot(n, q) + a->planes[face].d;
    one_point(out, n, de__v3_sub(q, de__v3_scale(n, dist)), q, rb, dist - rb,
              de__feat(DE__FEAT_FACE, face), first ? f0 : f1, spec);
    return out->count > 0;
}

/* ---- Pair functions -------------------------------------------------------------------- */

bool de__np_box_capsule(const de__shape *a, const de__shape *b, const de__np_rel *rel,
                        de__np_manifold *out) {
    de__np_box_poly bp;
    de__np_box_poly_init(&bp, a->u.box.half_extents);
    seg s;
    capsule_seg(b, rel, &s);
    return de__np_poly_segment(&bp.poly, NULL, s.p0, s.p1, s.r, s.f0, s.f1, s.fe, rel, out);
}

bool de__np_hull_sphere(const de__shape *a, const de__shape *b, const de__np_rel *rel,
                        de__np_manifold *out) {
    de__np_poly p;
    de__np_hull_poly(&a->u.hull, &p);
    seg s;
    sphere_seg(b, rel->t, &s);
    return de__np_poly_segment(&p, NULL, s.p0, s.p1, s.r, s.f0, s.f1, s.fe, rel, out);
}

bool de__np_hull_capsule(const de__shape *a, const de__shape *b, const de__np_rel *rel,
                         de__np_manifold *out) {
    de__np_poly p;
    de__np_hull_poly(&a->u.hull, &p);
    seg s;
    capsule_seg(b, rel, &s);
    return de__np_poly_segment(&p, NULL, s.p0, s.p1, s.r, s.f0, s.f1, s.fe, rel, out);
}

/* ---- Triangles ------------------------------------------------------------------------- */

bool de__np_triangle_convex(const de__np_tri *tri, const de__shape *b, const de__np_rel *rel,
                            de__np_manifold *out) {
    out->count = 0;
    switch (b->kind) {
    case DE_SHAPE_SPHERE: {
        seg s;
        sphere_seg(b, rel->t, &s);
        return de__np_poly_segment(&tri->poly, tri, s.p0, s.p1, s.r, s.f0, s.f1, s.fe, rel, out);
    }
    case DE_SHAPE_CAPSULE: {
        seg s;
        capsule_seg(b, rel, &s);
        return de__np_poly_segment(&tri->poly, tri, s.p0, s.p1, s.r, s.f0, s.f1, s.fe, rel, out);
    }
    case DE_SHAPE_BOX: {
        de__np_box_poly bp;
        de__np_box_poly_init(&bp, b->u.box.half_extents);
        return de__np_poly_poly(&tri->poly, tri, &bp.poly, rel, out);
    }
    case DE_SHAPE_CONVEX: {
        de__np_poly p;
        de__np_hull_poly(&b->u.hull, &p);
        return de__np_poly_poly(&tri->poly, tri, &p, rel, out);
    }
    case DE_SHAPE_CYLINDER: {
        de__gjk_proxy pa, pb;
        de__gjk_proxy_points(&pa, tri->v, 3, NULL);
        de__gjk_proxy_shape(&pb, b, true, rel);
        return de__np_gjk_epa(&pa, &tri->poly, 0.0f, 0, tri, &pb, NULL, 0.0f,
                              de__feat(DE__FEAT_SHAPE, 0), rel, out);
    }
    default:
        return false;
    }
}
