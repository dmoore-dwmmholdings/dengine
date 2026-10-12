/*
 * epa.c: EPA penetration depth (DENG-134) and the GJK/EPA contact used for cylinder pairs, for
 * triangle-cylinder and as a fallback.
 *
 * EPA grows a polytope inside A - B from GJK's simplex: each iteration takes the face closest to
 * the origin (ties: lowest index), adds the support point along its normal, removes the faces
 * that see it and closes the hole with the horizon. At most DE__EPA_MAX_ITER vertices are added,
 * so the face list is bounded by 2V - 4 (V = 4 + DE__EPA_MAX_ITER) and lives on the stack.
 */
#include "physics/narrow/narrowphase.h"

#define EPA_MAX_VERTS (4u + DE__EPA_MAX_ITER)
#define EPA_MAX_FACES (2u * EPA_MAX_VERTS)
#define EPA_MAX_EDGES (3u * EPA_MAX_FACES)

typedef struct epa_face {
    u32 v[3];
    de_dvec3 n;
    f64 dist;
    bool valid;
} epa_face;

typedef struct epa {
    de_dvec3 w[EPA_MAX_VERTS], a[EPA_MAX_VERTS], b[EPA_MAX_VERTS];
    u32 nv;
    epa_face f[EPA_MAX_FACES];
    u32 nf;
} epa;

static u32 add_vert(epa *e, const de__gjk_proxy *pa, const de__gjk_proxy *pb, de_dvec3 d) {
    const u32 k = e->nv++;
    e->a[k] = de__gjk_support(pa, d, NULL);
    e->b[k] = de__gjk_support(pb, de__d3(-d.x, -d.y, -d.z), NULL);
    e->w[k] = de__d3_sub(e->a[k], e->b[k]);
    return k;
}

static void make_face(epa *e, epa_face *f, u32 i0, u32 i1, u32 i2) {
    f->v[0] = i0;
    f->v[1] = i1;
    f->v[2] = i2;
    const de_dvec3 n = de__d3_cross(de__d3_sub(e->w[i1], e->w[i0]), de__d3_sub(e->w[i2], e->w[i0]));
    const f64 l2 = de__d3_dot(n, n);
    if (!(l2 > 1e-30)) {
        f->valid = false;
        f->n = de__d3(0.0, 0.0, 0.0);
        f->dist = 1e300;
        return;
    }
    f->n = de__d3_scale(n, 1.0 / de_dsqrt64(l2));
    f->dist = de__d3_dot(f->n, e->w[i0]);
    f->valid = true;
}

static f64 dist_to_line(de_dvec3 p, de_dvec3 a, de_dvec3 b) {
    const de_dvec3 d = de__d3_sub(b, a);
    const de_dvec3 c = de__d3_cross(d, de__d3_sub(p, a));
    const f64 l2 = de__d3_dot(d, d);
    return l2 > 0.0 ? de__d3_dot(c, c) / l2 : de__d3_dot(de__d3_sub(p, a), de__d3_sub(p, a));
}

static void fail(const de__gjk_result *g, de_vec3 fallback, u32 iter, de__epa_result *out) {
    out->ok = false;
    out->depth = 0.0;
    out->normal = de__d3_from(fallback);
    out->point_a = g->point_a;
    out->point_b = g->point_b;
    out->iterations = iter;
}

void de__epa_penetration(const de__gjk_proxy *pa, const de__gjk_proxy *pb, const de__gjk_result *g,
                         const de_vec3 fallback, de__epa_result *out) {
    epa e;
    e.nv = 0;
    e.nf = 0;
    for (u32 i = 0; i < g->count && i < 4u; ++i) {
        e.a[i] = g->a[i];
        e.b[i] = g->b[i];
        e.w[i] = de__d3_sub(g->a[i], g->b[i]);
        e.nv = i + 1u;
    }
    if (e.nv == 0) {
        fail(g, fallback, 0, out);
        return;
    }
    /* Blow a lower-dimensional simplex up to a tetrahedron. */
    static const f64 axes[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                   {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
    if (e.nv == 1) {
        for (u32 k = 0; k < 6 && e.nv == 1; ++k) {
            const u32 i = add_vert(&e, pa, pb, de__d3(axes[k][0], axes[k][1], axes[k][2]));
            const de_dvec3 d = de__d3_sub(e.w[i], e.w[0]);
            if (!(de__d3_dot(d, d) > 1e-18))
                e.nv = 1;
        }
    }
    if (e.nv == 2) {
        const de_dvec3 d = de__d3_sub(e.w[1], e.w[0]);
        const f64 ax = de__absd(d.x), ay = de__absd(d.y), az = de__absd(d.z);
        const de_dvec3 axis =
            ax <= ay && ax <= az ? de__d3(1, 0, 0) : (ay <= az ? de__d3(0, 1, 0) : de__d3(0, 0, 1));
        const de_dvec3 u = de__d3_cross(d, axis);
        const de_dvec3 w = de__d3_cross(d, u);
        const de_dvec3 dirs[4] = {u, de__d3_scale(u, -1.0), w, de__d3_scale(w, -1.0)};
        for (u32 k = 0; k < 4 && e.nv == 2; ++k) {
            const u32 i = add_vert(&e, pa, pb, dirs[k]);
            if (!(dist_to_line(e.w[i], e.w[0], e.w[1]) > 1e-18))
                e.nv = 2;
        }
    }
    if (e.nv == 3) {
        const de_dvec3 n = de__d3_cross(de__d3_sub(e.w[1], e.w[0]), de__d3_sub(e.w[2], e.w[0]));
        const f64 nl = de_dsqrt64(de__d3_dot(n, n));
        for (u32 k = 0; k < 2 && e.nv == 3 && nl > 0.0; ++k) {
            const u32 i = add_vert(&e, pa, pb, k == 0 ? n : de__d3_scale(n, -1.0));
            if (!(de__absd(de__d3_dot(de__d3_sub(e.w[i], e.w[0]), n)) > 1e-9 * nl))
                e.nv = 3;
        }
    }
    if (e.nv < 4) {
        fail(g, fallback, 0, out);
        return;
    }
    /* Tetrahedron faces, oriented away from its centroid. */
    const de_dvec3 c =
        de__d3_scale(de__d3_add(de__d3_add(e.w[0], e.w[1]), de__d3_add(e.w[2], e.w[3])), 0.25);
    static const u32 tet[4][3] = {{0, 1, 2}, {0, 3, 1}, {0, 2, 3}, {1, 3, 2}};
    for (u32 k = 0; k < 4; ++k) {
        u32 i0 = tet[k][0], i1 = tet[k][1], i2 = tet[k][2];
        const de_dvec3 n = de__d3_cross(de__d3_sub(e.w[i1], e.w[i0]), de__d3_sub(e.w[i2], e.w[i0]));
        if (de__d3_dot(n, de__d3_sub(e.w[i0], c)) < 0.0) {
            const u32 t = i1;
            i1 = i2;
            i2 = t;
        }
        make_face(&e, &e.f[e.nf++], i0, i1, i2);
        if (!e.f[e.nf - 1].valid) {
            fail(g, fallback, 0, out);
            return;
        }
    }

    u32 best = 0, iter = 0;
    for (;;) {
        best = EPA_MAX_FACES;
        for (u32 k = 0; k < e.nf; ++k)
            if (e.f[k].valid && (best == EPA_MAX_FACES || e.f[k].dist < e.f[best].dist))
                best = k;
        if (best == EPA_MAX_FACES) {
            fail(g, fallback, iter, out);
            return;
        }
        if (iter >= DE__EPA_MAX_ITER || e.nv >= EPA_MAX_VERTS)
            break;
        const epa_face fb = e.f[best];
        const u32 wi = add_vert(&e, pa, pb, fb.n);
        const f64 gain = de__d3_dot(e.w[wi], fb.n) - fb.dist;
        if (gain <= 1e-9 + 1e-7 * de__absd(fb.dist)) {
            --e.nv; /* converged: drop the new vertex */
            break;
        }
        ++iter;
        /* Faces seen from the new point, and their horizon. */
        u32 edges[EPA_MAX_EDGES][2];
        u32 ne = 0;
        bool seen[EPA_MAX_FACES];
        u32 nseen = 0;
        for (u32 k = 0; k < e.nf; ++k) {
            seen[k] = e.f[k].valid
                          ? de__d3_dot(e.f[k].n, de__d3_sub(e.w[wi], e.w[e.f[k].v[0]])) > 1e-12
                          : true;
            if (seen[k]) {
                ++nseen;
                for (u32 j = 0; j < 3 && ne < EPA_MAX_EDGES; ++j) {
                    edges[ne][0] = e.f[k].v[j];
                    edges[ne][1] = e.f[k].v[(j + 1) % 3];
                    ++ne;
                }
            }
        }
        u32 horizon[EPA_MAX_EDGES][2];
        u32 nh = 0;
        for (u32 i = 0; i < ne; ++i) {
            bool shared = false;
            for (u32 j = 0; j < ne && !shared; ++j)
                shared = edges[j][0] == edges[i][1] && edges[j][1] == edges[i][0];
            if (!shared) {
                horizon[nh][0] = edges[i][0];
                horizon[nh][1] = edges[i][1];
                ++nh;
            }
        }
        if (e.nf - nseen + nh > EPA_MAX_FACES || nh == 0) {
            --e.nv;
            break;
        }
        u32 m = 0;
        for (u32 k = 0; k < e.nf; ++k)
            if (!seen[k])
                e.f[m++] = e.f[k];
        e.nf = m;
        for (u32 i = 0; i < nh; ++i)
            make_face(&e, &e.f[e.nf++], horizon[i][0], horizon[i][1], wi);
    }

    /* The closest face: barycentric weights of the origin's projection onto it. */
    const epa_face *f = &e.f[best];
    const f64 dist = f->dist > 0.0 ? f->dist : 0.0;
    const de_dvec3 p = de__d3_scale(f->n, dist);
    const de_dvec3 w0 = e.w[f->v[0]], w1 = e.w[f->v[1]], w2 = e.w[f->v[2]];
    const de_dvec3 v0 = de__d3_sub(w1, w0), v1 = de__d3_sub(w2, w0), v2 = de__d3_sub(p, w0);
    const f64 d00 = de__d3_dot(v0, v0), d01 = de__d3_dot(v0, v1), d11 = de__d3_dot(v1, v1);
    const f64 d20 = de__d3_dot(v2, v0), d21 = de__d3_dot(v2, v1);
    const f64 den = d00 * d11 - d01 * d01;
    f64 l1 = 0.0, l2 = 0.0;
    if (den > 0.0) {
        l1 = (d11 * d20 - d01 * d21) / den;
        l2 = (d00 * d21 - d01 * d20) / den;
    }
    const f64 l0 = 1.0 - l1 - l2;
    out->ok = true;
    out->depth = dist;
    out->normal = f->n;
    out->point_a =
        de__d3_add(de__d3_add(de__d3_scale(e.a[f->v[0]], l0), de__d3_scale(e.a[f->v[1]], l1)),
                   de__d3_scale(e.a[f->v[2]], l2));
    out->point_b = de__d3_sub(out->point_a, de__d3_scale(f->n, dist));
    out->iterations = iter;
}

/* ---- GJK/EPA contact ------------------------------------------------------------------- */

static de_vec3 to_f(de_dvec3 v) {
    return de__v3((f32)v.x, (f32)v.y, (f32)v.z);
}

bool de__np_gjk_epa(const de__gjk_proxy *a, const de__np_poly *poly_a, f32 ra, u32 fa,
                    const de__np_tri *tri, const de__gjk_proxy *b, const de__np_poly *poly_b,
                    f32 rb, u32 fb, const de__np_rel *rel, de__np_manifold *out) {
    out->count = 0;
    de__gjk_result g;
    de__gjk_distance(a, b, &g);
    if (!g.overlap && g.distance > (f64)(ra + rb + rel->spec))
        return false;
    de_vec3 n, ca, cb;
    f32 core_sep;
    if (!g.overlap && g.distance > (f64)DE__NP_CORE_EPS) {
        ca = to_f(g.point_a);
        cb = to_f(g.point_b);
        n = de__np_normalize(de__v3_sub(cb, ca), rel->up);
        core_sep = (f32)g.distance;
    } else {
        de__epa_result r;
        de__epa_penetration(a, b, &g, rel->up, &r);
        n = de__np_normalize(to_f(r.normal), rel->up);
        ca = to_f(r.point_a);
        cb = to_f(r.point_b);
        core_sep = -(f32)r.depth;
    }
    u32 ffa = poly_a != NULL ? de__np_poly_support_feature(poly_a, n) : fa;
    if (tri != NULL && !de__np_tri_feature_ok(tri, ffa, n)) {
        /* Internal-edge correction: B against the triangle's plane. */
        n = tri->n;
        cb = to_f(de__gjk_support(b, de__d3(-(f64)n.x, -(f64)n.y, -(f64)n.z), NULL));
        const f32 dist = de__v3_dot(n, cb) + tri->p[0].d;
        ca = de__v3_sub(cb, de__v3_scale(n, dist));
        core_sep = dist;
        ffa = de__feat(DE__FEAT_FACE, 0);
    }
    const u32 ffb =
        poly_b != NULL
            ? de__np_poly_support_feature(poly_b, de__np_dir_to_b(rel, de__v3(-n.x, -n.y, -n.z)))
            : fb;
    out->normal = n;
    de__np_add(out, de__v3_add(ca, de__v3_scale(n, ra)), de__v3_sub(cb, de__v3_scale(n, rb)),
               core_sep - ra - rb, de__feature_id(ffa, ffb), rel->spec);
    return out->count > 0;
}

static u32 curved(const de__shape *s) {
    (void)s;
    return de__feat(DE__FEAT_SHAPE, 0);
}

bool de__np_convex_gjk(const de__shape *a, const de__shape *b, const de__np_rel *rel,
                       de__np_manifold *out) {
    de__gjk_proxy pa, pb;
    de__gjk_proxy_shape(&pa, a, true, NULL);
    de__gjk_proxy_shape(&pb, b, true, rel);
    de__np_box_poly ba, bb;
    de__np_poly ha, hb;
    const de__np_poly *polya = NULL, *polyb = NULL;
    if (a->kind == DE_SHAPE_BOX) {
        de__np_box_poly_init(&ba, a->u.box.half_extents);
        polya = &ba.poly;
    } else if (a->kind == DE_SHAPE_CONVEX) {
        de__np_hull_poly(&a->u.hull, &ha);
        polya = &ha;
    }
    if (b->kind == DE_SHAPE_BOX) {
        de__np_box_poly_init(&bb, b->u.box.half_extents);
        polyb = &bb.poly;
    } else if (b->kind == DE_SHAPE_CONVEX) {
        de__np_hull_poly(&b->u.hull, &hb);
        polyb = &hb;
    }
    return de__np_gjk_epa(&pa, polya, de__shape_margin(a), curved(a), NULL, &pb, polyb,
                          de__shape_margin(b), curved(b), rel, out);
}
