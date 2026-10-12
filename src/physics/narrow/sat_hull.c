/*
 * sat_hull.c: polytope-polytope SAT (DENG-134): hull-hull, hull-box and triangle against box or
 * hull. Face queries over both polytopes, edge-pair queries pruned by the Gauss map (Gregorius,
 * "The Separating Axis Test between Convex Polyhedra", GDC 2013), then a face contact (clip.c)
 * or one edge-edge point. Triangles are flat polytopes with two faces: their edge pairs are
 * checked numerically instead of through the Gauss map, inactive edges give no edge axes, and a
 * contact on an inactive feature falls back to the triangle's front face.
 */
#include "physics/narrow/narrowphase.h"

/* ---- Polytopes ------------------------------------------------------------------------- */

void de__np_hull_poly(const de__hull *h, de__np_poly *out) {
    out->verts = h->vertices;
    out->planes = h->planes;
    out->faces = h->faces;
    out->edges = h->edges;
    out->vcount = h->vertex_count;
    out->fcount = h->face_count;
    out->ecount = h->edge_count;
    out->centroid = h->centroid;
    out->flat = false;
}

/* Front face 0 = corners 0 1 2, back face 1 = 1 0 2 order; half-edge i < 3 is triangle edge i
 * (corner i -> i + 1), half-edges 3..5 its twins on the back face. */
static const de__hull_face tri_faces[2] = {{0, 3}, {3, 3}};
static const de__hull_edge tri_edges[6] = {
    {1, 3, 0, 0}, {2, 5, 1, 0}, {0, 4, 2, 0}, /* front: 0->1, 1->2, 2->0 */
    {4, 0, 1, 1}, {5, 2, 0, 1}, {3, 1, 2, 1}, /* back: 1->0, 0->2, 2->1 */
};

bool de__np_tri_init(de__np_tri *t, de_vec3 a, de_vec3 b, de_vec3 c, u8 active_edges) {
    t->v[0] = a;
    t->v[1] = b;
    t->v[2] = c;
    const de_dvec3 A = de__d3_from(a);
    const de_dvec3 n = de__d3_cross(de__d3_sub(de__d3_from(b), A), de__d3_sub(de__d3_from(c), A));
    const f64 l2 = de__d3_dot(n, n);
    if (!(l2 > 1e-30))
        return false;
    const f64 k = 1.0 / de_dsqrt64(l2);
    t->n = de__v3((f32)(n.x * k), (f32)(n.y * k), (f32)(n.z * k));
    const f32 d = -de__v3_dot(t->n, a);
    t->p[0].n = t->n;
    t->p[0].d = d;
    t->p[1].n = de__v3(-t->n.x, -t->n.y, -t->n.z);
    t->p[1].d = -d;
    t->poly.verts = t->v;
    t->poly.planes = t->p;
    t->poly.faces = tri_faces;
    t->poly.edges = tri_edges;
    t->poly.vcount = 3;
    t->poly.fcount = 2;
    t->poly.ecount = 6;
    t->poly.centroid =
        de__v3((f32)(((f64)a.x + b.x + c.x) / 3.0), (f32)(((f64)a.y + b.y + c.y) / 3.0),
               (f32)(((f64)a.z + b.z + c.z) / 3.0));
    t->poly.flat = true;
    t->active = (u8)(active_edges & 7u);
    return true;
}

/* ---- Features -------------------------------------------------------------------------- */

static u32 best_face(const de__np_poly *p, de_vec3 n_out) {
    u32 best = 0;
    f32 bd = de__v3_dot(p->planes[0].n, n_out);
    for (u32 f = 1; f < p->fcount; ++f) {
        const f32 d = de__v3_dot(p->planes[f].n, n_out);
        if (d > bd) {
            bd = d;
            best = f;
        }
    }
    return best;
}

u32 de__np_poly_support_feature(const de__np_poly *p, de_vec3 n_out) {
    f32 mx = de__v3_dot(p->verts[0], n_out);
    for (u32 i = 1; i < p->vcount; ++i) {
        const f32 d = de__v3_dot(p->verts[i], n_out);
        if (d > mx)
            mx = d;
    }
    u32 idx[2] = {0, 0};
    u32 cnt = 0;
    for (u32 i = 0; i < p->vcount; ++i) {
        if (de__v3_dot(p->verts[i], n_out) >= mx - DE__NP_FEATURE_TOL) {
            if (cnt < 2)
                idx[cnt] = i;
            ++cnt;
        }
    }
    if (cnt == 1)
        return de__feat(DE__FEAT_VERTEX, idx[0]);
    if (cnt == 2) {
        for (u32 e = 0; e < p->ecount; ++e) {
            const u32 o = p->edges[e].origin, d = p->edges[p->edges[e].next].origin;
            if (o == idx[0] && d == idx[1])
                return de__feat(DE__FEAT_EDGE, de__np_edge_id(p, e));
            if (o == idx[1] && d == idx[0])
                return de__feat(DE__FEAT_EDGE, de__np_edge_id(p, e));
        }
    }
    return de__feat(DE__FEAT_FACE, best_face(p, n_out));
}

bool de__np_tri_feature_ok(const de__np_tri *t, u32 f, de_vec3 n) {
    if (!(de__v3_dot(n, t->n) > 0.0f))
        return false;
    const u32 i = de__feat_index(f);
    switch (de__feat_type(f)) {
    case DE__FEAT_FACE:
        return i == 0;
    case DE__FEAT_EDGE:
        return i < 3 && (t->active & (1u << i)) != 0;
    case DE__FEAT_VERTEX:
        return i < 3 && (t->active & ((1u << i) | (1u << ((i + 2u) % 3u)))) != 0;
    default:
        return false;
    }
}

/* ---- Polytope SAT ---------------------------------------------------------------------- */

/* Gauss-map test: do arcs (a, b) of A and (c, d) of -B cross? Then the edge pair builds a face
 * of the Minkowski difference and its cross product is a candidate axis. */
static bool minkowski_face(de_vec3 a, de_vec3 b, de_vec3 c, de_vec3 d) {
    const de_vec3 bxa = de__v3_cross(b, a), dxc = de__v3_cross(d, c);
    const f32 cba = de__v3_dot(c, bxa), dba = de__v3_dot(d, bxa);
    const f32 adc = de__v3_dot(a, dxc), bdc = de__v3_dot(b, dxc);
    return cba * dba < 0.0f && adc * bdc < 0.0f && cba * bdc > 0.0f;
}

static f32 near_tol(f32 x) {
    return 1e-5f + 1e-6f * de__absf(x);
}

bool de__np_poly_poly(const de__np_poly *a, const de__np_tri *tri, const de__np_poly *b,
                      const de__np_rel *rel, de__np_manifold *out) {
    out->count = 0;
    const f32 spec = rel->spec;
    de_vec3 bverts[DE_SHAPE_MAX_HULL_VERTICES];
    de_vec3 bn[2u * DE_SHAPE_MAX_HULL_VERTICES];
    for (u32 v = 0; v < b->vcount; ++v)
        bverts[v] = de__np_to_a(rel, b->verts[v]);
    for (u32 f = 0; f < b->fcount; ++f)
        bn[f] = de__np_dir_to_a(rel, b->planes[f].n);

    /* Face queries of A. */
    f32 sep_a = -3.0e38f, sep_front = -3.0e38f;
    u32 face_a = 0;
    for (u32 f = 0; f < a->fcount; ++f) {
        const de_vec3 n = a->planes[f].n;
        f32 mn = de__v3_dot(n, bverts[0]);
        for (u32 v = 1; v < b->vcount; ++v) {
            const f32 d = de__v3_dot(n, bverts[v]);
            if (d < mn)
                mn = d;
        }
        const f32 s = mn + a->planes[f].d;
        if (f == 0)
            sep_front = s;
        if (s > sep_a) {
            sep_a = s;
            face_a = f;
        }
        if (s > spec)
            return false;
    }
    /* Face queries of B. */
    f32 sep_b = -3.0e38f;
    u32 face_b = 0;
    for (u32 f = 0; f < b->fcount; ++f) {
        const de_vec3 n = bn[f];
        const f32 d = -de__v3_dot(n, bverts[b->edges[b->faces[f].first_edge].origin]);
        f32 mn = de__v3_dot(n, a->verts[0]);
        for (u32 v = 1; v < a->vcount; ++v) {
            const f32 x = de__v3_dot(n, a->verts[v]);
            if (x < mn)
                mn = x;
        }
        const f32 s = mn + d;
        if (s > sep_b) {
            sep_b = s;
            face_b = f;
        }
        if (s > spec)
            return false;
    }
    /* Edge pairs. */
    f32 sep_e = -3.0e38f;
    u32 edge_a = 0, edge_b = 0;
    de_vec3 axis = de__v3(0.0f, 0.0f, 0.0f);
    bool have_edge = false;
    for (u32 ea = 0; ea < a->ecount; ++ea) {
        if (a->edges[ea].twin < ea)
            continue;
        if (tri != NULL && (ea > 2u || (tri->active & (1u << ea)) == 0))
            continue; /* inactive triangle edge: no edge contacts */
        const de_vec3 a0 = a->verts[a->edges[ea].origin];
        const de_vec3 a1 = a->verts[a->edges[a->edges[ea].next].origin];
        const de_vec3 da = de__v3_sub(a1, a0);
        const f32 la2 = de__np_len2(da);
        for (u32 eb = 0; eb < b->ecount; ++eb) {
            if (b->edges[eb].twin < eb)
                continue;
            const de_vec3 b0 = bverts[b->edges[eb].origin];
            const de_vec3 b1 = bverts[b->edges[b->edges[eb].next].origin];
            const de_vec3 db = de__v3_sub(b1, b0);
            de_vec3 l = de__v3_cross(da, db);
            if (de__np_len2(l) < 1e-6f * la2 * de__np_len2(db))
                continue; /* parallel: the face axes cover it */
            f32 s;
            if (!a->flat) {
                const de_vec3 na1 = a->planes[a->edges[ea].face].n;
                const de_vec3 na2 = a->planes[a->edges[a->edges[ea].twin].face].n;
                const de_vec3 nb1 = bn[b->edges[eb].face];
                const de_vec3 nb2 = bn[b->edges[b->edges[eb].twin].face];
                if (!minkowski_face(na1, na2, de__v3(-nb1.x, -nb1.y, -nb1.z),
                                    de__v3(-nb2.x, -nb2.y, -nb2.z)))
                    continue;
                l = de__np_normalize(l, l);
                if (de__v3_dot(l, de__v3_sub(a0, a->centroid)) < 0.0f)
                    l = de__v3(-l.x, -l.y, -l.z);
                s = de__v3_dot(l, de__v3_sub(b0, a0));
            } else {
                l = de__np_normalize(l, l);
                f32 max_a = de__v3_dot(l, a->verts[0]), min_a = max_a;
                for (u32 v = 1; v < a->vcount; ++v) {
                    const f32 x = de__v3_dot(l, a->verts[v]);
                    max_a = x > max_a ? x : max_a;
                    min_a = x < min_a ? x : min_a;
                }
                f32 min_b = de__v3_dot(l, bverts[0]), max_b = min_b;
                for (u32 v = 1; v < b->vcount; ++v) {
                    const f32 x = de__v3_dot(l, bverts[v]);
                    max_b = x > max_b ? x : max_b;
                    min_b = x < min_b ? x : min_b;
                }
                const f32 s1 = min_b - max_a, s2 = min_a - max_b;
                f32 sa, sb;
                if (s2 > s1) {
                    l = de__v3(-l.x, -l.y, -l.z);
                    s = s2;
                    sa = -min_a;
                    sb = -max_b;
                } else {
                    s = s1;
                    sa = max_a;
                    sb = min_b;
                }
                /* Both edges must support the axis, or the pair is not the touching feature. */
                if (de__absf(de__v3_dot(l, a0) - sa) > near_tol(sa) ||
                    de__absf(de__v3_dot(l, a1) - sa) > near_tol(sa) ||
                    de__absf(de__v3_dot(l, b0) - sb) > near_tol(sb) ||
                    de__absf(de__v3_dot(l, b1) - sb) > near_tol(sb))
                    continue;
            }
            if (s > sep_e) {
                sep_e = s;
                edge_a = ea;
                edge_b = eb;
                axis = l;
                have_edge = true;
                if (s > spec)
                    return false;
            }
        }
    }

    bool ref_b = sep_b > sep_a + DE__NP_FACE_TOL;
    bool edge = have_edge && sep_e > (sep_a > sep_b ? sep_a : sep_b) + DE__NP_EDGE_TOL;
    if (tri != NULL) {
        /* Internal-edge correction and one-sidedness: only the front face, active edges and
         * vertices on an active edge may give the normal. */
        bool front = false;
        if (edge) {
            front = !(de__v3_dot(axis, tri->n) > 0.0f);
        } else if (ref_b) {
            const de_vec3 nb = bn[face_b];
            const de_vec3 n = de__v3(-nb.x, -nb.y, -nb.z);
            front = !de__np_tri_feature_ok(tri, de__np_poly_support_feature(a, n), n);
        } else {
            front = face_a != 0;
        }
        if (front) {
            if (sep_front > spec)
                return false;
            edge = false;
            ref_b = false;
            face_a = 0;
        }
    }
    if (edge) {
        de__np_edge_contact(a, edge_a, b, bverts, edge_b, axis, rel, out);
        return out->count > 0;
    }
    de__np_face_contact(a, b, bverts, rel, !ref_b, ref_b ? face_b : face_a, out);
    return out->count > 0;
}

/* ---- Pair functions -------------------------------------------------------------------- */

bool de__np_hull_box(const de__shape *a, const de__shape *b, const de__np_rel *rel,
                     de__np_manifold *out) {
    de__np_poly pa;
    de__np_hull_poly(&a->u.hull, &pa);
    de__np_box_poly pb;
    de__np_box_poly_init(&pb, b->u.box.half_extents);
    return de__np_poly_poly(&pa, NULL, &pb.poly, rel, out);
}

bool de__np_hull_hull(const de__shape *a, const de__shape *b, const de__np_rel *rel,
                      de__np_manifold *out) {
    de__np_poly pa, pb;
    de__np_hull_poly(&a->u.hull, &pa);
    de__np_hull_poly(&b->u.hull, &pb);
    return de__np_poly_poly(&pa, NULL, &pb, rel, out);
}
