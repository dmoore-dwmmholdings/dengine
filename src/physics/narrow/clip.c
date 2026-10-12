/*
 * clip.c: polygon contacts (DENG-134). Reference / incident face selection, Sutherland-Hodgman
 * clipping that tracks which features made each point, segment clipping for capsules, edge-edge
 * contacts, segment closest points and the deterministic reduction to DE__CONTACT_MAX_POINTS.
 */
#include "physics/narrow/narrowphase.h"

/* Most points of a clipped polygon: a convex k-gon clipped by m half-planes has at most k + m
 * vertices, and a hull face has at most DE_SHAPE_MAX_HULL_VERTICES edges. */
#define CLIP_MAX (2u * DE_SHAPE_MAX_HULL_VERTICES + 8u)

enum { OUT_INCIDENT = 0u, OUT_REFERENCE = 1u };

typedef struct cpt {
    de_vec3 p;
    u32 fr, fi;   /* reference and incident features */
    u32 out_kind; /* the polygon edge to the next point: an incident edge or a reference edge */
    u32 out_idx;  /* incident: canonical edge id; reference: half-edge index */
} cpt;

/* ---- Segments -------------------------------------------------------------------------- */

static f64 clamp01(f64 x) {
    return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x);
}

void de__np_segments_closest(de_vec3 p0, de_vec3 p1, de_vec3 q0, de_vec3 q1, f32 *s_out, f32 *t_out,
                             de_vec3 *cp, de_vec3 *cq) {
    /* Ericson, Real-Time Collision Detection 5.1.9, in f64. */
    const de_dvec3 P0 = de__d3_from(p0), Q0 = de__d3_from(q0);
    const de_dvec3 d1 = de__d3_sub(de__d3_from(p1), P0), d2 = de__d3_sub(de__d3_from(q1), Q0);
    const de_dvec3 r = de__d3_sub(P0, Q0);
    const f64 a = de__d3_dot(d1, d1), e = de__d3_dot(d2, d2), f = de__d3_dot(d2, r);
    const f64 eps = 1e-24;
    f64 s, t;
    if (a <= eps && e <= eps) {
        s = t = 0.0;
    } else if (a <= eps) {
        s = 0.0;
        t = clamp01(f / e);
    } else {
        const f64 c = de__d3_dot(d1, r);
        if (e <= eps) {
            t = 0.0;
            s = clamp01(-c / a);
        } else {
            const f64 b = de__d3_dot(d1, d2);
            const f64 denom = a * e - b * b;
            s = denom > 1e-14 * a * e ? clamp01((b * f - c * e) / denom) : 0.0;
            t = (b * s + f) / e;
            if (t < 0.0) {
                t = 0.0;
                s = clamp01(-c / a);
            } else if (t > 1.0) {
                t = 1.0;
                s = clamp01((b - c) / a);
            }
        }
    }
    const de_dvec3 P = de__d3_add(P0, de__d3_scale(d1, s));
    const de_dvec3 Q = de__d3_add(Q0, de__d3_scale(d2, t));
    *s_out = (f32)s;
    *t_out = (f32)t;
    *cp = de__v3((f32)P.x, (f32)P.y, (f32)P.z);
    *cq = de__v3((f32)Q.x, (f32)Q.y, (f32)Q.z);
}

f32 de__np_segment_closest_t(de_vec3 a, de_vec3 b, de_vec3 p) {
    const de_dvec3 A = de__d3_from(a);
    const de_dvec3 d = de__d3_sub(de__d3_from(b), A);
    const f64 l2 = de__d3_dot(d, d);
    if (!(l2 > 1e-24))
        return 0.0f;
    return (f32)clamp01(de__d3_dot(de__d3_sub(de__d3_from(p), A), d) / l2);
}

/* ---- Reduction ------------------------------------------------------------------------- */

/* Keeps at most DE__CONTACT_MAX_POINTS of keep[0..n): the deepest, then repeatedly the point
 * farthest from the chosen ones (ties: lowest index); the chosen keep their order. */
static u32 reduce(const de_vec3 *pos, const f32 *sep, u32 n, u32 *chosen) {
    if (n <= DE__CONTACT_MAX_POINTS) {
        for (u32 i = 0; i < n; ++i)
            chosen[i] = i;
        return n;
    }
    static const f32 big = 3.0e38f;
    f32 mind[CLIP_MAX];
    u8 used[CLIP_MAX];
    u32 first = 0;
    for (u32 i = 1; i < n; ++i)
        if (sep[i] < sep[first])
            first = i;
    for (u32 i = 0; i < n; ++i) {
        mind[i] = big;
        used[i] = 0;
    }
    u32 k = 0;
    u32 pick = first;
    for (;;) {
        used[pick] = 1;
        chosen[k++] = pick;
        if (k == DE__CONTACT_MAX_POINTS)
            break;
        u32 best = n;
        for (u32 i = 0; i < n; ++i) {
            if (used[i])
                continue;
            const f32 d = de__np_len2(de__v3_sub(pos[i], pos[pick]));
            if (d < mind[i])
                mind[i] = d;
            if (best == n || mind[i] > mind[best])
                best = i;
        }
        pick = best;
    }
    /* Ascending index order. */
    for (u32 i = 1; i < k; ++i) {
        const u32 v = chosen[i];
        u32 j = i;
        while (j > 0 && chosen[j - 1] > v) {
            chosen[j] = chosen[j - 1];
            --j;
        }
        chosen[j] = v;
    }
    return k;
}

/* ---- Face contacts --------------------------------------------------------------------- */

/* The reference vertex shared by half-edges r0 and r1 of the same face, or none. */
static bool shared_ref_vertex(const de__np_poly *ref, u32 r0, u32 r1, u32 *v) {
    if (ref->edges[r0].next == r1) {
        *v = ref->edges[r1].origin;
        return true;
    }
    if (ref->edges[r1].next == r0) {
        *v = ref->edges[r0].origin;
        return true;
    }
    return false;
}

void de__np_face_contact(const de__np_poly *a, const de__np_poly *b, const de_vec3 *bverts,
                         const de__np_rel *rel, bool ref_is_a, u32 face, de__np_manifold *out) {
    const de__np_poly *ref = ref_is_a ? a : b;
    const de__np_poly *inc = ref_is_a ? b : a;
    const de_vec3 *rv = ref_is_a ? a->verts : bverts;
    const de_vec3 *iv = ref_is_a ? bverts : a->verts;
    const de__hull_face rf = ref->faces[face];
    de_vec3 rn;
    f32 rd;
    de_vec3 rn_inc; /* the reference normal in the incident polytope's own frame */
    if (ref_is_a) {
        rn = a->planes[face].n;
        rd = a->planes[face].d;
        rn_inc = de__np_dir_to_b(rel, rn);
    } else {
        rn = de__np_dir_to_a(rel, b->planes[face].n);
        rd = -de__v3_dot(rn, rv[b->edges[rf.first_edge].origin]);
        rn_inc = rn;
    }

    /* Incident face: most anti-parallel to the reference normal (ties: lowest index). */
    u32 fi = 0;
    f32 best = de__v3_dot(inc->planes[0].n, rn_inc);
    for (u32 f = 1; f < inc->fcount; ++f) {
        const f32 d = de__v3_dot(inc->planes[f].n, rn_inc);
        if (d < best) {
            best = d;
            fi = f;
        }
    }

    cpt buf[2][CLIP_MAX];
    u32 n = 0;
    const de__hull_face ifc = inc->faces[fi];
    for (u32 k = 0; k < (u32)ifc.edge_count && n < CLIP_MAX; ++k) {
        const u32 e = (u32)ifc.first_edge + k;
        cpt *c = &buf[0][n++];
        c->p = iv[inc->edges[e].origin];
        c->fr = de__feat(DE__FEAT_FACE, face);
        c->fi = de__feat(DE__FEAT_VERTEX, inc->edges[e].origin);
        c->out_kind = OUT_INCIDENT;
        c->out_idx = de__np_edge_id(inc, e);
    }

    /* Clip against the side plane of every reference edge. */
    u32 cur = 0;
    for (u32 k = 0; k < (u32)rf.edge_count && n > 0; ++k) {
        const u32 re = (u32)rf.first_edge + k;
        const de_vec3 o = rv[ref->edges[re].origin];
        const de_vec3 e1 = rv[ref->edges[ref->edges[re].next].origin];
        const de_vec3 sn = de__v3_cross(de__v3_sub(e1, o), rn); /* outward of the face */
        const f32 so = de__v3_dot(sn, o);
        const cpt *src = buf[cur];
        cpt *dst = buf[cur ^ 1u];
        u32 m = 0;
        for (u32 i = 0; i < n; ++i) {
            const cpt *c = &src[i];
            const cpt *x = &src[(i + 1) % n];
            const f32 dc = de__v3_dot(sn, c->p) - so;
            const f32 dn = de__v3_dot(sn, x->p) - so;
            if (dc <= 0.0f && m < CLIP_MAX) {
                dst[m] = *c;
                if (dc == 0.0f && dn > 0.0f) {
                    dst[m].out_kind = OUT_REFERENCE;
                    dst[m].out_idx = re;
                }
                ++m;
            }
            if (((dc < 0.0f && dn > 0.0f) || (dc > 0.0f && dn < 0.0f)) && m < CLIP_MAX) {
                const f32 t = dc / (dc - dn);
                cpt *q = &dst[m++];
                q->p = de__v3_add(c->p, de__v3_scale(de__v3_sub(x->p, c->p), t));
                if (c->out_kind == OUT_INCIDENT) {
                    q->fr = de__feat(DE__FEAT_EDGE, de__np_edge_id(ref, re));
                    q->fi = de__feat(DE__FEAT_EDGE, c->out_idx);
                } else {
                    u32 v;
                    if (shared_ref_vertex(ref, c->out_idx, re, &v))
                        q->fr = de__feat(DE__FEAT_VERTEX, v);
                    else
                        q->fr = de__feat(DE__FEAT_EDGE, de__np_edge_id(ref, re));
                    q->fi = de__feat(DE__FEAT_FACE, fi);
                }
                if (dc > 0.0f) { /* entering: continues along c's edge */
                    q->out_kind = c->out_kind;
                    q->out_idx = c->out_idx;
                } else { /* leaving: continues along this side plane */
                    q->out_kind = OUT_REFERENCE;
                    q->out_idx = re;
                }
            }
        }
        n = m;
        cur ^= 1u;
    }

    /* Keep the points below the reference face within the speculative distance. */
    de_vec3 pos[CLIP_MAX];
    f32 sep[CLIP_MAX];
    u32 idx[CLIP_MAX];
    u32 kept = 0;
    for (u32 i = 0; i < n; ++i) {
        const f32 s = de__v3_dot(rn, buf[cur][i].p) + rd;
        if (s <= rel->spec) {
            idx[kept] = i;
            pos[kept] = buf[cur][i].p;
            sep[kept] = s;
            ++kept;
        }
    }
    u32 chosen[DE__CONTACT_MAX_POINTS];
    const u32 nc = reduce(pos, sep, kept, chosen);
    out->count = 0;
    out->normal = ref_is_a ? rn : de__v3(-rn.x, -rn.y, -rn.z);
    for (u32 k = 0; k < nc; ++k) {
        const cpt *c = &buf[cur][idx[chosen[k]]];
        const f32 s = sep[chosen[k]];
        const de_vec3 on_ref = de__v3_sub(c->p, de__v3_scale(rn, s));
        if (ref_is_a)
            de__np_add(out, on_ref, c->p, s, de__feature_id(c->fr, c->fi), rel->spec);
        else
            de__np_add(out, c->p, on_ref, s, de__feature_id(c->fi, c->fr), rel->spec);
    }
}

/* ---- Segment against a face ------------------------------------------------------------ */

u32 de__np_clip_segment(const de__np_poly *poly, u32 face, de_vec3 p0, de_vec3 p1, f32 r, u32 f0,
                        u32 f1, u32 fe, f32 spec, de__np_manifold *out) {
    const de__hull_face fc = poly->faces[face];
    const de_vec3 n = poly->planes[face].n;
    const f32 d = poly->planes[face].d;
    f32 t0 = 0.0f, t1 = 1.0f;
    u32 c0 = DE__SUB_NONE, c1 = DE__SUB_NONE; /* side half-edge that clipped each end */
    const de_vec3 dir = de__v3_sub(p1, p0);
    for (u32 k = 0; k < (u32)fc.edge_count; ++k) {
        const u32 e = (u32)fc.first_edge + k;
        const de_vec3 o = poly->verts[poly->edges[e].origin];
        const de_vec3 e1 = poly->verts[poly->edges[poly->edges[e].next].origin];
        const de_vec3 sn = de__v3_cross(de__v3_sub(e1, o), n);
        const f32 d0 = de__v3_dot(sn, de__v3_sub(p0, o));
        const f32 d1 = de__v3_dot(sn, de__v3_sub(p1, o));
        if (d0 > 0.0f && d1 > 0.0f)
            return 0;
        if (d0 > 0.0f) {
            const f32 t = d0 / (d0 - d1);
            if (t > t0) {
                t0 = t;
                c0 = e;
            }
        } else if (d1 > 0.0f) {
            const f32 t = d0 / (d0 - d1);
            if (t < t1) {
                t1 = t;
                c1 = e;
            }
        }
    }
    if (t0 > t1)
        return 0;
    const bool point = p0.x == p1.x && p0.y == p1.y && p0.z == p1.z;
    const u32 before = out->count;
    for (u32 k = 0; k < (point ? 1u : 2u); ++k) {
        const f32 t = k == 0 ? t0 : t1;
        const u32 clip = k == 0 ? c0 : c1;
        const de_vec3 q = de__v3_add(p0, de__v3_scale(dir, t));
        const f32 dist = de__v3_dot(n, q) + d;
        const de_vec3 pa = de__v3_sub(q, de__v3_scale(n, dist));
        const de_vec3 pb = de__v3_sub(q, de__v3_scale(n, r));
        u32 fa, fb;
        if (clip == DE__SUB_NONE) {
            fa = de__feat(DE__FEAT_FACE, face);
            fb = k == 0 ? f0 : f1;
        } else {
            fa = de__feat(DE__FEAT_EDGE, de__np_edge_id(poly, clip));
            fb = fe;
        }
        de__np_add(out, pa, pb, dist - r, de__feature_id(fa, fb), spec);
    }
    if (out->count > before)
        out->normal = n;
    return out->count - before;
}

/* ---- Edge-edge ------------------------------------------------------------------------- */

void de__np_edge_contact(const de__np_poly *a, u32 ea, const de__np_poly *b, const de_vec3 *bverts,
                         u32 eb, de_vec3 axis, const de__np_rel *rel, de__np_manifold *out) {
    const de_vec3 a0 = a->verts[a->edges[ea].origin];
    const de_vec3 a1 = a->verts[a->edges[a->edges[ea].next].origin];
    const de_vec3 b0 = bverts[b->edges[eb].origin];
    const de_vec3 b1 = bverts[b->edges[b->edges[eb].next].origin];
    f32 s, t;
    de_vec3 pa, pb;
    de__np_segments_closest(a0, a1, b0, b1, &s, &t, &pa, &pb);
    out->count = 0;
    out->normal = axis;
    const f32 sep = de__v3_dot(de__v3_sub(pb, pa), axis);
    de__np_add(out, pa, pb, sep,
               de__feature_id(de__feat(DE__FEAT_EDGE, de__np_edge_id(a, ea)),
                              de__feat(DE__FEAT_EDGE, de__np_edge_id(b, eb))),
               rel->spec);
}
