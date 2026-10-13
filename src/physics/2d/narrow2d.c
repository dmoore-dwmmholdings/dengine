/*
 * narrow2d.c: the 2D narrowphase (DENG-253): canonical pair order, the pair frame, the manifold
 * functions of circles, capsules, segments and polygons, and the shared helpers of
 * collide_chain2d.c. See narrow2d.h. The functions follow Box2D v3 (manifold.c) in structure; the
 * code, ids and tie rules are this engine's.
 */
#include "physics/2d/narrow2d.h"

/* ---- Small vector helpers ----------------------------------------------------------------- */

static de_vec2 v2_neg(de_vec2 a) {
    return de__v2(-a.x, -a.y);
}
/* a + s * b */
static de_vec2 v2_mad(de_vec2 a, f32 s, de_vec2 b) {
    return de__v2(a.x + s * b.x, a.y + s * b.y);
}
/* a + t * (b - a) */
static de_vec2 v2_lerp(de_vec2 a, de_vec2 b, f32 t) {
    return de__v2(a.x + t * (b.x - a.x), a.y + t * (b.y - a.y));
}
static f32 clamp01(f32 x) {
    return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

f32 de__n2_normalize(de_vec2 v, de_vec2 *out) {
    const f32 l = de_dsqrt(v.x * v.x + v.y * v.y);
    if (!(l >= FLT_EPSILON)) {
        *out = de__v2(0.0f, 0.0f);
        return 0.0f;
    }
    const f32 inv = 1.0f / l;
    *out = de__v2(v.x * inv, v.y * inv);
    return l;
}

de__n2_segdist de__n2_segment_distance(de_vec2 p1, de_vec2 q1, de_vec2 p2, de_vec2 q2) {
    de__n2_segdist r;
    const de_vec2 d1 = de__v2_sub(q1, p1);
    const de_vec2 d2 = de__v2_sub(q2, p2);
    const de_vec2 w = de__v2_sub(p1, p2);
    const f32 dd1 = de__v2_dot(d1, d1);
    const f32 dd2 = de__v2_dot(d2, d2);
    const f32 rd1 = de__v2_dot(w, d1);
    const f32 rd2 = de__v2_dot(w, d2);
    const f32 eps2 = FLT_EPSILON * FLT_EPSILON;
    f32 f1 = 0.0f, f2 = 0.0f;
    if (dd1 < eps2 || dd2 < eps2) {
        if (dd1 >= eps2)
            f1 = clamp01(-rd1 / dd1); /* segment 2 is a point */
        else if (dd2 >= eps2)
            f2 = clamp01(rd2 / dd2); /* segment 1 is a point */
    } else {
        const f32 d12 = de__v2_dot(d1, d2);
        const f32 denom = dd1 * dd2 - d12 * d12;
        if (denom != 0.0f) /* not parallel */
            f1 = clamp01((d12 * rd2 - rd1 * dd2) / denom);
        f2 = (d12 * f1 + rd2) / dd2;
        /* Clamping segment 2 means segment 1 is solved again. */
        if (f2 < 0.0f) {
            f2 = 0.0f;
            f1 = clamp01(-rd1 / dd1);
        } else if (f2 > 1.0f) {
            f2 = 1.0f;
            f1 = clamp01((d12 - rd1) / dd1);
        }
    }
    r.f1 = f1;
    r.f2 = f2;
    /* End points exactly at the end fractions (tests compare fractions with 0 and 1). */
    r.c1 = f1 == 0.0f ? p1 : (f1 == 1.0f ? q1 : v2_mad(p1, f1, d1));
    r.c2 = f2 == 0.0f ? p2 : (f2 == 1.0f ? q2 : v2_mad(p2, f2, d2));
    const de_vec2 dc = de__v2_sub(r.c2, r.c1);
    r.d2 = de__v2_dot(dc, dc);
    return r;
}

u32 de__n2_clip_segments(de_vec2 a1, de_vec2 a2, de_vec2 b1, de_vec2 b2, de_vec2 normal, f32 ra,
                         f32 rb, de__n2_clip *out) {
    const de_vec2 tangent = de__v2_left_perp(normal);
    const f32 lower1 = 0.0f;
    const f32 upper1 = de__v2_dot(de__v2_sub(a2, a1), tangent);
    /* The incident segment runs against the tangent (both CCW). */
    const f32 upper2 = de__v2_dot(de__v2_sub(b1, a1), tangent);
    const f32 lower2 = de__v2_dot(de__v2_sub(b2, a1), tangent);
    if (upper2 < lower1 || upper1 < lower2)
        return 0;
    de_vec2 v_lower = b2, v_upper = b1;
    if (lower2 < lower1 && upper2 - lower2 > FLT_EPSILON)
        v_lower = v2_lerp(b2, b1, (lower1 - lower2) / (upper2 - lower2));
    if (upper2 > upper1 && upper2 - lower2 > FLT_EPSILON)
        v_upper = v2_lerp(b2, b1, (upper1 - lower2) / (upper2 - lower2));
    const de_vec2 v[2] = {v_lower, v_upper};
    for (u32 i = 0; i < 2; ++i) {
        const f32 s = de__v2_dot(de__v2_sub(v[i], a1), normal);
        out->inc[i] = v2_mad(v[i], -rb, normal);
        out->ref[i] = v2_mad(v[i], ra - s, normal);
        out->separation[i] = s - ra - rb;
    }
    return 2;
}

void de__n2_add(de__n2_manifold *m, de_vec2 pa, de_vec2 pb, f32 separation, u32 id,
                f32 speculative) {
    if (!(separation <= speculative) || m->count >= DE__P2D_MAX_POINTS)
        return;
    de__n2_point *p = &m->pt[m->count++];
    p->pa = pa;
    p->pb = pb;
    p->separation = separation;
    p->id = id;
}

/* (SHAPE 0, SHAPE 0): the id of every point that involves a circle. */
#define K_CURVED_ID ((2u << 14) | ((2u << 14) << 16))

/* ---- Circles and capsules ------------------------------------------------------------------ */

/* Circle (or a point of a core) at ca with radius ra against one at cb with rb. `fallback` is the
 * normal when the centers coincide. */
static void collide_points(de_vec2 ca, f32 ra, de_vec2 cb, f32 rb, de_vec2 fallback, f32 spec,
                           de__n2_manifold *m) {
    de_vec2 n;
    const f32 dist = de__n2_normalize(de__v2_sub(cb, ca), &n);
    if (dist == 0.0f)
        n = fallback;
    const f32 sep = dist - ra - rb;
    if (!(sep <= spec))
        return;
    m->normal = n;
    de__n2_add(m, v2_mad(ca, ra, n), v2_mad(cb, -rb, n), sep, K_CURVED_ID, spec);
}

static void collide_circles(de_vec2 ca, f32 ra, de_vec2 cb, f32 rb, f32 spec, de__n2_manifold *m) {
    collide_points(ca, ra, cb, rb, de__v2(0.0f, 1.0f), spec, m);
}

/* Segment p1-p2 swept by ra (A) against a circle at c with radius rb (B). */
static void collide_capsule_circle(de_vec2 p1, de_vec2 p2, f32 ra, de_vec2 c, f32 rb, f32 spec,
                                   de__n2_manifold *m) {
    const de_vec2 e = de__v2_sub(p2, p1);
    const f32 ee = de__v2_dot(e, e);
    de_vec2 q = p1;
    de_vec2 fallback = de__v2(0.0f, 1.0f);
    if (ee > 0.0f) {
        const f32 t = clamp01(de__v2_dot(de__v2_sub(c, p1), e) / ee);
        q = t == 0.0f ? p1 : (t == 1.0f ? p2 : v2_mad(p1, t, e));
        de_vec2 rp;
        if (de__n2_normalize(de__v2_right_perp(e), &rp) > 0.0f)
            fallback = rp;
    }
    collide_points(q, ra, c, rb, fallback, spec, m);
}

/* Capsule p1-q1 (ra, A) against capsule p2-q2 (rb, B): two clipped points when B's core lies on
 * one side of A's line and the projections overlap, else the closest points (Box2D v3
 * b2CollideCapsules, with a side rule instead of its crossing heuristics). */
static void collide_capsules(de_vec2 p1, de_vec2 q1, f32 ra, de_vec2 p2, de_vec2 q2, f32 rb,
                             f32 spec, de__n2_manifold *m) {
    const de__n2_segdist r = de__n2_segment_distance(p1, q1, p2, q2);
    const f32 max_d = ra + rb + spec;
    if (r.d2 > max_d * max_d)
        return;
    de_vec2 u1, u2;
    const f32 len1 = de__n2_normalize(de__v2_sub(q1, p1), &u1);
    const f32 len2 = de__n2_normalize(de__v2_sub(q2, p2), &u2);
    if (len1 > 0.0f && len2 > 0.0f) {
        const f32 fp2 = de__v2_dot(de__v2_sub(p2, p1), u1);
        const f32 fq2 = de__v2_dot(de__v2_sub(q2, p1), u1);
        const bool outside_a = (fp2 <= 0.0f && fq2 <= 0.0f) || (fp2 >= len1 && fq2 >= len1);
        const f32 fp1 = de__v2_dot(de__v2_sub(p1, p2), u2);
        const f32 fq1 = de__v2_dot(de__v2_sub(q1, p2), u2);
        const bool outside_b = (fp1 <= 0.0f && fq1 <= 0.0f) || (fp1 >= len2 && fq1 >= len2);
        if (!outside_a && !outside_b) {
            /* B's core projected on A's segment, clipped to [0, len1]. */
            de_vec2 v_lower, v_upper;
            if (fp2 < 0.0f && fq2 - fp2 > FLT_EPSILON)
                v_lower = v2_lerp(p2, q2, (0.0f - fp2) / (fq2 - fp2));
            else if (fq2 < 0.0f && fp2 - fq2 > FLT_EPSILON)
                v_lower = v2_lerp(q2, p2, (0.0f - fq2) / (fp2 - fq2));
            else
                v_lower = fp2 < fq2 ? p2 : q2;
            if (fp2 > len1 && fp2 - fq2 > FLT_EPSILON)
                v_upper = v2_lerp(p2, q2, (fp2 - len1) / (fp2 - fq2));
            else if (fq2 > len1 && fq2 - fp2 > FLT_EPSILON)
                v_upper = v2_lerp(q2, p2, (fq2 - len1) / (fq2 - fp2));
            else
                v_upper = fp2 < fq2 ? q2 : p2;
            const u32 lower_vertex = fp2 < fq2 ? 0u : 1u;
            const de_vec2 rp = de__v2_right_perp(u1);
            f32 s_lower = de__v2_dot(de__v2_sub(v_lower, p1), rp);
            f32 s_upper = de__v2_dot(de__v2_sub(v_upper, p1), rp);
            de_vec2 n = rp;
            u32 side = 0;
            bool one_side = true;
            if (s_lower >= 0.0f && s_upper >= 0.0f) {
                /* right of A */
            } else if (s_lower <= 0.0f && s_upper <= 0.0f) {
                n = v2_neg(rp);
                side = 1;
                s_lower = -s_lower;
                s_upper = -s_upper;
            } else {
                one_side = false; /* B crosses A's line: one point */
            }
            if (one_side) {
                m->normal = n;
                const u32 half_a = de__f2(DE__F2_EDGE, side);
                de__n2_add(m, v2_mad(v_lower, ra - s_lower, n), v2_mad(v_lower, -rb, n),
                           s_lower - ra - rb,
                           de__f2_id(half_a, de__f2(DE__F2_VERTEX, lower_vertex)), spec);
                de__n2_add(m, v2_mad(v_upper, ra - s_upper, n), v2_mad(v_upper, -rb, n),
                           s_upper - ra - rb,
                           de__f2_id(half_a, de__f2(DE__F2_VERTEX, 1u - lower_vertex)), spec);
                if (m->count > 0)
                    return;
                m->normal = de__v2(0.0f, 0.0f);
            }
        }
    }
    /* Closest points. Crossing cores: the side of B's midpoint on A's line. */
    de_vec2 fallback = de__v2(0.0f, 1.0f);
    if (len1 > 0.0f) {
        const de_vec2 rp = de__v2_right_perp(u1);
        const de_vec2 mid = de__v2_scale(de__v2_add(p2, q2), 0.5f);
        fallback = de__v2_dot(de__v2_sub(mid, p1), rp) >= 0.0f ? rp : v2_neg(rp);
    }
    collide_points(r.c1, ra, r.c2, rb, fallback, spec, m);
}

/* ---- Polygons ------------------------------------------------------------------------------ */

static u32 next_index(u32 i, u32 count) {
    return i + 1u < count ? i + 1u : 0u;
}

/* Polygon (A) against a circle at c with radius rb (B): Box2D v3 b2CollidePolygonAndCircle. */
static void collide_polygon_circle(const de__n2_poly *a, de_vec2 c, f32 rb, f32 spec,
                                   de__n2_manifold *m) {
    const f32 radius = a->radius + rb;
    u32 normal_index = 0;
    f32 separation = -FLT_MAX;
    for (u32 i = 0; i < a->count; ++i) {
        const f32 s = de__v2_dot(a->n[i], de__v2_sub(c, a->v[i]));
        if (s > separation) {
            separation = s;
            normal_index = i;
        }
    }
    if (separation - radius > spec)
        return;
    const de_vec2 v1 = a->v[normal_index];
    const de_vec2 v2 = a->v[next_index(normal_index, a->count)];
    const f32 u1 = de__v2_dot(de__v2_sub(c, v1), de__v2_sub(v2, v1));
    const f32 u2 = de__v2_dot(de__v2_sub(c, v2), de__v2_sub(v1, v2));
    if ((u1 < 0.0f || u2 < 0.0f) && separation > FLT_EPSILON) {
        /* The center is outside, closest to a vertex. */
        const de_vec2 v = u1 < 0.0f ? v1 : v2;
        collide_points(v, a->radius, c, rb, a->n[normal_index], spec, m);
        return;
    }
    /* Between v1 and v2 (or inside): the face normal. */
    const de_vec2 n = a->n[normal_index];
    const f32 s = de__v2_dot(de__v2_sub(c, v1), n);
    m->normal = n;
    de__n2_add(m, v2_mad(c, a->radius - s, n), v2_mad(c, -rb, n), s - radius, K_CURVED_ID, spec);
}

/* The edge of p1 with the largest separation from p2's vertices (min over p2 of the distance in
 * front of the edge); ties to the lower edge. */
static f32 find_max_separation(u32 *edge, const de__n2_poly *p1, const de__n2_poly *p2) {
    u32 best = 0;
    f32 max_sep = -FLT_MAX;
    for (u32 i = 0; i < p1->count; ++i) {
        const de_vec2 n = p1->n[i];
        const de_vec2 v1 = p1->v[i];
        f32 si = FLT_MAX;
        for (u32 j = 0; j < p2->count; ++j) {
            const f32 sij = de__v2_dot(n, de__v2_sub(p2->v[j], v1));
            if (sij < si)
                si = sij;
        }
        if (si > max_sep) {
            max_sep = si;
            best = i;
        }
    }
    *edge = best;
    return max_sep;
}

/* The edge of `p` whose normal is most anti-parallel to `dir`; ties to the lower edge. */
static u32 incident_edge(const de__n2_poly *p, de_vec2 dir) {
    u32 best = 0;
    f32 min_dot = FLT_MAX;
    for (u32 i = 0; i < p->count; ++i) {
        const f32 d = de__v2_dot(dir, p->n[i]);
        if (d < min_dot) {
            min_dot = d;
            best = i;
        }
    }
    return best;
}

/* Reference-edge clipping (Box2D v3 b2ClipPolygons). Not flipped: A holds the reference edge. */
static void clip_polygons(const de__n2_poly *a, const de__n2_poly *b, u32 edge_a, u32 edge_b,
                          bool flip, f32 spec, de__n2_manifold *m) {
    const de__n2_poly *p1 = flip ? b : a; /* reference */
    const de__n2_poly *p2 = flip ? a : b; /* incident */
    const u32 i11 = flip ? edge_b : edge_a;
    const u32 i12 = next_index(i11, p1->count);
    const u32 i21 = flip ? edge_a : edge_b;
    const u32 i22 = next_index(i21, p2->count);
    const de_vec2 normal = p1->n[i11];
    const de_vec2 v11 = p1->v[i11], v12 = p1->v[i12];
    const de_vec2 v21 = p2->v[i21], v22 = p2->v[i22];
    const de_vec2 tangent = de__v2_left_perp(normal);
    const f32 lower1 = 0.0f;
    const f32 upper1 = de__v2_dot(de__v2_sub(v12, v11), tangent);
    const f32 upper2 = de__v2_dot(de__v2_sub(v21, v11), tangent);
    const f32 lower2 = de__v2_dot(de__v2_sub(v22, v11), tangent);
    de_vec2 v_lower = v22, v_upper = v21;
    if (lower2 < lower1 && upper2 - lower2 > FLT_EPSILON)
        v_lower = v2_lerp(v22, v21, (lower1 - lower2) / (upper2 - lower2));
    if (upper2 > upper1 && upper2 - lower2 > FLT_EPSILON)
        v_upper = v2_lerp(v22, v21, (upper1 - lower2) / (upper2 - lower2));
    const f32 s_lower = de__v2_dot(de__v2_sub(v_lower, v11), normal);
    const f32 s_upper = de__v2_dot(de__v2_sub(v_upper, v11), normal);
    const f32 r1 = p1->radius, r2 = p2->radius;
    const de_vec2 ref_lower = v2_mad(v_lower, r1 - s_lower, normal);
    const de_vec2 ref_upper = v2_mad(v_upper, r1 - s_upper, normal);
    const de_vec2 inc_lower = v2_mad(v_lower, -r2, normal);
    const de_vec2 inc_upper = v2_mad(v_upper, -r2, normal);
    const f32 radius = r1 + r2;
    const u32 vtx = DE__F2_VERTEX;
    if (!flip) {
        m->normal = normal;
        de__n2_add(m, ref_lower, inc_lower, s_lower - radius,
                   de__f2_id(de__f2(vtx, i11), de__f2(vtx, i22)), spec);
        de__n2_add(m, ref_upper, inc_upper, s_upper - radius,
                   de__f2_id(de__f2(vtx, i12), de__f2(vtx, i21)), spec);
    } else {
        m->normal = v2_neg(normal);
        de__n2_add(m, inc_upper, ref_upper, s_upper - radius,
                   de__f2_id(de__f2(vtx, i21), de__f2(vtx, i12)), spec);
        de__n2_add(m, inc_lower, ref_lower, s_lower - radius,
                   de__f2_id(de__f2(vtx, i22), de__f2(vtx, i11)), spec);
    }
    if (m->count == 0)
        m->normal = de__v2(0.0f, 0.0f);
}

/* Rounded polygon (A) against rounded polygon (B), both 2..8 vertices (Box2D v3
 * b2CollidePolygons): SAT on both normal sets, the reference face with a 0.1-slop preference
 * for A, a vertex-vertex point with the true normal when the cores are apart at two vertices,
 * else reference-edge clipping. */
static void collide_polygons(const de__n2_poly *a, const de__n2_poly *b, f32 spec,
                             de__n2_manifold *m) {
    u32 edge_a = 0, edge_b = 0;
    const f32 sep_a = find_max_separation(&edge_a, a, b);
    const f32 sep_b = find_max_separation(&edge_b, b, a);
    const f32 radius = a->radius + b->radius;
    if (sep_a > spec + radius || sep_b > spec + radius)
        return;
    bool flip;
    if (sep_b > 0.1f * DE__P2D_LINEAR_SLOP + sep_a) {
        flip = true;
        edge_a = incident_edge(a, b->n[edge_b]);
    } else {
        flip = false;
        edge_b = incident_edge(b, a->n[edge_a]);
    }
    const f32 separation = de__maxf(sep_a, sep_b);
    if (separation > 0.1f * DE__P2D_LINEAR_SLOP) {
        /* The cores are apart: if the closest features of the two edges are vertices, one point
         * along the true normal (it matters for rounded corners and speculative contacts). */
        const u32 i11 = edge_a, i12 = next_index(edge_a, a->count);
        const u32 i21 = edge_b, i22 = next_index(edge_b, b->count);
        const de__n2_segdist r =
            de__n2_segment_distance(a->v[i11], a->v[i12], b->v[i21], b->v[i22]);
        const bool end1 = r.f1 == 0.0f || r.f1 == 1.0f;
        const bool end2 = r.f2 == 0.0f || r.f2 == 1.0f;
        if (end1 && end2) {
            const u32 ia = r.f1 == 0.0f ? i11 : i12;
            const u32 ib = r.f2 == 0.0f ? i21 : i22;
            de_vec2 n;
            const f32 dist = de__n2_normalize(de__v2_sub(r.c2, r.c1), &n);
            if (dist == 0.0f || dist > spec + radius)
                return;
            m->normal = n;
            de__n2_add(m, v2_mad(r.c1, a->radius, n), v2_mad(r.c2, -b->radius, n), dist - radius,
                       de__f2_id(de__f2(DE__F2_VERTEX, ia), de__f2(DE__F2_VERTEX, ib)), spec);
            return;
        }
    }
    clip_polygons(a, b, edge_a, edge_b, flip, spec, m);
}

/* ---- Pair frame and dispatch ---------------------------------------------------------------- */

/* B's frame -> the pair frame (A's frame shifted by `shift`), in f64. */
typedef struct rel2d {
    f64 c, s;
    de__d2 d;
} rel2d;

static de_vec2 rel_point(const rel2d *r, de_vec2 v) {
    const f64 x = (f64)v.x, y = (f64)v.y;
    return de__v2((f32)(r->c * x - r->s * y + r->d.x), (f32)(r->s * x + r->c * y + r->d.y));
}
static de_vec2 rel_dir(const rel2d *r, de_vec2 v) {
    const f64 x = (f64)v.x, y = (f64)v.y;
    return de__v2((f32)(r->c * x - r->s * y), (f32)(r->s * x + r->c * y));
}

/* A polygon shape as a core polygon, through `xf` (NULL: shifted by -shift only). */
static void poly_from_polygon(const de__polygon2d *src, const rel2d *xf, de_vec2 shift,
                              de__n2_poly *out) {
    out->count = src->count;
    out->radius = src->radius;
    for (u32 i = 0; i < src->count; ++i) {
        if (xf != NULL) {
            out->v[i] = rel_point(xf, src->vertices[i]);
            out->n[i] = rel_dir(xf, src->normals[i]);
        } else {
            out->v[i] = de__v2_sub(src->vertices[i], shift);
            out->n[i] = src->normals[i];
        }
    }
    out->centroid = xf != NULL ? rel_point(xf, src->centroid) : de__v2_sub(src->centroid, shift);
}

/* A segment p1-p2 (already in the pair frame) with radius r as a two-vertex polygon. */
static void poly_from_segment(de_vec2 p1, de_vec2 p2, f32 r, de__n2_poly *out) {
    de_vec2 n;
    (void)de__n2_normalize(de__v2_right_perp(de__v2_sub(p2, p1)), &n);
    out->count = 2;
    out->radius = r;
    out->v[0] = p1;
    out->v[1] = p2;
    out->n[0] = n;
    out->n[1] = v2_neg(n);
    out->centroid = de__v2_scale(de__v2_add(p1, p2), 0.5f);
}

/* A shape's convex core in the pair frame, as a tagged union of the cases the dispatch needs. */
typedef enum core_kind { CORE_CIRCLE, CORE_SEGMENT, CORE_POLY, CORE_CHAIN, CORE_NONE } core_kind;

typedef struct core2d {
    core_kind kind;
    de_vec2 c;      /* circle center */
    de_vec2 p1, p2; /* segment / capsule core */
    f32 radius;     /* circle / capsule radius */
    de__n2_poly poly;
    const de__chain_segment *seg;
} core2d;

/* The point of A the pair frame starts at (precision: chain segments far from their body). */
static de_vec2 shape_anchor(const de__shape2d *s, u32 child) {
    switch (s->kind) {
    case DE_SHAPE2D_CIRCLE:
        return s->u.circle.center;
    case DE_SHAPE2D_CAPSULE:
        return s->u.capsule.p1;
    case DE_SHAPE2D_SEGMENT:
        return s->u.segment.p1;
    case DE_SHAPE2D_BOX:
    case DE_SHAPE2D_POLYGON:
        return s->u.polygon.vertices[0];
    case DE_SHAPE2D_CHAIN:
        return child < s->u.chain.segment_count ? s->u.chain.segments[child].p1 : de__v2(0, 0);
    default:
        return de__v2(0.0f, 0.0f);
    }
}

static bool capsule_is_circle(const de__shape2d *s) {
    return s->u.capsule.p1.x == s->u.capsule.p2.x && s->u.capsule.p1.y == s->u.capsule.p2.y;
}

/* B's core in the pair frame. */
static void core_b(const de__shape2d *s, const rel2d *xf, core2d *out) {
    switch (s->kind) {
    case DE_SHAPE2D_CIRCLE:
        out->kind = CORE_CIRCLE;
        out->c = rel_point(xf, s->u.circle.center);
        out->radius = s->u.circle.radius;
        return;
    case DE_SHAPE2D_CAPSULE:
        out->radius = s->u.capsule.radius;
        if (capsule_is_circle(s)) {
            out->kind = CORE_CIRCLE;
            out->c = rel_point(xf, s->u.capsule.p1);
            return;
        }
        out->kind = CORE_SEGMENT;
        out->p1 = rel_point(xf, s->u.capsule.p1);
        out->p2 = rel_point(xf, s->u.capsule.p2);
        return;
    case DE_SHAPE2D_SEGMENT:
        out->kind = CORE_SEGMENT;
        out->radius = 0.0f;
        out->p1 = rel_point(xf, s->u.segment.p1);
        out->p2 = rel_point(xf, s->u.segment.p2);
        return;
    case DE_SHAPE2D_BOX:
    case DE_SHAPE2D_POLYGON:
        out->kind = CORE_POLY;
        poly_from_polygon(&s->u.polygon, xf, de__v2(0.0f, 0.0f), &out->poly);
        return;
    default:
        out->kind = CORE_NONE;
        return;
    }
}

/* A's core in the pair frame (A's frame shifted by `shift`). */
static void core_a(const de__shape2d *s, u32 child, de_vec2 shift, core2d *out) {
    switch (s->kind) {
    case DE_SHAPE2D_CIRCLE:
        out->kind = CORE_CIRCLE;
        out->c = de__v2_sub(s->u.circle.center, shift);
        out->radius = s->u.circle.radius;
        return;
    case DE_SHAPE2D_CAPSULE:
        out->radius = s->u.capsule.radius;
        if (capsule_is_circle(s)) {
            out->kind = CORE_CIRCLE;
            out->c = de__v2_sub(s->u.capsule.p1, shift);
            return;
        }
        out->kind = CORE_SEGMENT;
        out->p1 = de__v2_sub(s->u.capsule.p1, shift);
        out->p2 = de__v2_sub(s->u.capsule.p2, shift);
        return;
    case DE_SHAPE2D_SEGMENT:
        out->kind = CORE_SEGMENT;
        out->radius = 0.0f;
        out->p1 = de__v2_sub(s->u.segment.p1, shift);
        out->p2 = de__v2_sub(s->u.segment.p2, shift);
        return;
    case DE_SHAPE2D_BOX:
    case DE_SHAPE2D_POLYGON:
        out->kind = CORE_POLY;
        poly_from_polygon(&s->u.polygon, NULL, shift, &out->poly);
        return;
    case DE_SHAPE2D_CHAIN:
        if (child >= s->u.chain.segment_count) {
            out->kind = CORE_NONE;
            return;
        }
        out->kind = CORE_CHAIN;
        out->seg = &s->u.chain.segments[child];
        return;
    default:
        out->kind = CORE_NONE;
        return;
    }
}

/* B's core as a polygon (segment cores become two-vertex polygons). */
static void as_poly(const core2d *b, de__n2_poly *out) {
    if (b->kind == CORE_POLY)
        *out = b->poly;
    else
        poly_from_segment(b->p1, b->p2, b->radius, out);
}

/* The pair in canonical order: A's rank >= B's (narrow2d.h). */
static void collide_local(const core2d *a, const core2d *b, de_vec2 shift, f32 spec,
                          de__n2_manifold *m) {
    if (a->kind == CORE_NONE || b->kind == CORE_NONE)
        return;
    switch (a->kind) {
    case CORE_CIRCLE:
        if (b->kind == CORE_CIRCLE)
            collide_circles(a->c, a->radius, b->c, b->radius, spec, m);
        return;
    case CORE_SEGMENT:
        if (b->kind == CORE_CIRCLE) {
            collide_capsule_circle(a->p1, a->p2, a->radius, b->c, b->radius, spec, m);
        } else if (b->kind == CORE_SEGMENT) {
            collide_capsules(a->p1, a->p2, a->radius, b->p1, b->p2, b->radius, spec, m);
        } else if (b->kind == CORE_POLY) {
            /* A segment against a polygon: A as a two-vertex polygon. */
            de__n2_poly pa;
            poly_from_segment(a->p1, a->p2, a->radius, &pa);
            collide_polygons(&pa, &b->poly, spec, m);
        }
        return;
    case CORE_POLY:
        if (b->kind == CORE_CIRCLE) {
            collide_polygon_circle(&a->poly, b->c, b->radius, spec, m);
        } else {
            de__n2_poly pb;
            as_poly(b, &pb);
            collide_polygons(&a->poly, &pb, spec, m);
        }
        return;
    case CORE_CHAIN:
        if (b->kind == CORE_CIRCLE) {
            de__n2_chain_circle(a->seg, shift, b->c, b->radius, spec, m);
        } else if (b->kind == CORE_SEGMENT || b->kind == CORE_POLY) {
            de__n2_poly pb;
            as_poly(b, &pb);
            de__n2_chain_polygon(a->seg, shift, &pb, spec, m);
        }
        return;
    default:
        return;
    }
}

/* -1, 0, 1 for the lexicographic order of two poses. */
static int pose_cmp(const de__xf2d *x, const de__xf2d *y) {
    const f64 a[4] = {x->p.x, x->p.y, (f64)x->q.c, (f64)x->q.s};
    const f64 b[4] = {y->p.x, y->p.y, (f64)y->q.c, (f64)y->q.s};
    for (u32 i = 0; i < 4; ++i)
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    return 0;
}

/* true when (sb, xb) must be A: it ranks higher, or ties and comes first. */
static bool must_swap(const de__shape2d *sa, const de__xf2d *xa, const de__shape2d *sb,
                      const de__xf2d *xb) {
    const u32 ra = de__shape2d_rank(sa->kind), rb = de__shape2d_rank(sb->kind);
    if (ra != rb)
        return rb > ra;
    if (sa->handle.id != sb->handle.id)
        return sb->handle.id < sa->handle.id;
    return pose_cmp(xb, xa) < 0;
}

static u32 sym_key(u32 id) {
    const u32 w = de__f2_swap(id);
    return w < id ? w : id;
}
static bool point_before(const de__n2_point *x, const de__n2_point *y) {
    const u32 kx = sym_key(x->id), ky = sym_key(y->id);
    return kx != ky ? kx < ky : x->id < y->id;
}

/* Collides in canonical order (sa is A) and fills `out` in A/B terms. */
static void collide_canonical(const de__shape2d *sa, de__xf2d xa, const de__shape2d *sb,
                              de__xf2d xb, u32 child, f32 spec, de__contact_set2d *out) {
    memset(out, 0, sizeof *out);
    const de_vec2 shift = shape_anchor(sa, child);
    /* B's frame in the pair frame, in f64: R(qa)^T (R(qb) v + pb - pa) - shift. */
    const f64 ca = (f64)xa.q.c, sna = (f64)xa.q.s;
    const f64 cb = (f64)xb.q.c, snb = (f64)xb.q.s;
    const de__d2 dp = de__d2_sub(xb.p, xa.p);
    rel2d xf;
    xf.c = ca * cb + sna * snb;
    xf.s = ca * snb - sna * cb;
    xf.d =
        de__d2_make(ca * dp.x + sna * dp.y - (f64)shift.x, -sna * dp.x + ca * dp.y - (f64)shift.y);

    core2d a, b;
    core_a(sa, child, shift, &a);
    core_b(sb, &xf, &b);
    de__n2_manifold m;
    memset(&m, 0, sizeof m);
    collide_local(&a, &b, shift, spec, &m);
    if (m.count == 0)
        return;
    /* Points in the order of min(id, swapped id): it does not depend on which shape is A or
     * which gives the reference face, so two equal boxes resting on each other keep both their
     * ids and their order (ties: the canonical id). */
    if (m.count == 2u && point_before(&m.pt[1], &m.pt[0])) {
        const de__n2_point t = m.pt[0];
        m.pt[0] = m.pt[1];
        m.pt[1] = t;
    }

    /* Back to body frames and plane axes, in f64. */
    const f64 nx = (f64)m.normal.x, ny = (f64)m.normal.y;
    out->normal = de__v2((f32)(ca * nx - sna * ny), (f32)(sna * nx + ca * ny));
    out->point_count = m.count;
    for (u32 i = 0; i < m.count; ++i) {
        const de__n2_point *src = &m.pt[i];
        de__contact_point2d *dst = &out->points[i];
        dst->local_a =
            de__v2((f32)((f64)src->pa.x + (f64)shift.x), (f32)((f64)src->pa.y + (f64)shift.y));
        /* B's frame: R(rel)^T (pb - d). */
        const f64 bx = (f64)src->pb.x - xf.d.x, by = (f64)src->pb.y - xf.d.y;
        dst->local_b = de__v2((f32)(xf.c * bx + xf.s * by), (f32)(-xf.s * bx + xf.c * by));
        dst->separation = src->separation;
        dst->feature_id = src->id;
    }
}

u32 de__collide2d(const de__shape2d *sa, de__xf2d xa, const de__shape2d *sb, de__xf2d xb, u32 child,
                  f32 speculative, de__contact_set2d *out) {
    if (!(speculative >= 0.0f))
        speculative = 0.0f;
    if (!must_swap(sa, &xa, sb, &xb)) {
        collide_canonical(sa, xa, sb, xb, child, speculative, out);
        return out->point_count;
    }
    de__contact_set2d t;
    collide_canonical(sb, xb, sa, xa, child, speculative, &t);
    memset(out, 0, sizeof *out);
    if (t.point_count == 0)
        return 0;
    out->normal = v2_neg(t.normal);
    out->point_count = t.point_count;
    for (u32 i = 0; i < t.point_count; ++i) {
        out->points[i].local_a = t.points[i].local_b;
        out->points[i].local_b = t.points[i].local_a;
        out->points[i].separation = t.points[i].separation;
        out->points[i].feature_id = de__f2_swap(t.points[i].feature_id);
    }
    return out->point_count;
}
