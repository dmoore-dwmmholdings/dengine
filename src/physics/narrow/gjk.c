/*
 * gjk.c: GJK distance between two convex support mappings (DENG-134), in f64.
 *
 * The simplex lives in the difference A - B. Each iteration adds the support point along -v
 * and replaces the simplex by the smallest sub-simplex holding its closest point to the origin.
 * That sub-simplex is found by brute force over the (at most 15) subsets: the valid subset
 * (all barycentric weights > 0) with the smallest distance wins, ties to the lower subset mask.
 * This is slower than Johnson's distance sub-algorithm but has no case analysis to get wrong
 * and degrades gracefully on near-degenerate simplices. At most DE__GJK_MAX_ITER iterations.
 */
#include "physics/narrow/narrowphase.h"

/* ---- Proxies --------------------------------------------------------------------------- */

static void set_frame(de__gjk_proxy *p, const de__np_rel *rel) {
    if (rel == NULL) {
        p->identity = true;
        memset(&p->rot, 0, sizeof p->rot);
        p->rot.m[0] = p->rot.m[4] = p->rot.m[8] = 1.0f;
        p->pos = de__v3(0.0f, 0.0f, 0.0f);
    } else {
        p->identity = false;
        p->rot = rel->r;
        p->pos = rel->t;
    }
}

void de__gjk_proxy_shape(de__gjk_proxy *p, const de__shape *s, bool core, const de__np_rel *rel) {
    p->shape = s;
    p->points = NULL;
    p->count = 0;
    p->core = core;
    set_frame(p, rel);
}

void de__gjk_proxy_points(de__gjk_proxy *p, const de_vec3 *points, u32 count,
                          const de__np_rel *rel) {
    p->shape = NULL;
    p->points = points;
    p->count = count;
    p->core = true;
    set_frame(p, rel);
}

static u32 points_support(const de_vec3 *pts, u32 n, de_dvec3 d) {
    u32 best = 0;
    f64 bd = (f64)pts[0].x * d.x + (f64)pts[0].y * d.y + (f64)pts[0].z * d.z;
    for (u32 i = 1; i < n; ++i) {
        const f64 x = (f64)pts[i].x * d.x + (f64)pts[i].y * d.y + (f64)pts[i].z * d.z;
        if (x > bd) { /* strict: ties keep the lowest index */
            bd = x;
            best = i;
        }
    }
    return best;
}

de_dvec3 de__gjk_support(const de__gjk_proxy *p, de_dvec3 dir, u32 *index) {
    /* Direction into the proxy's frame. */
    de_dvec3 d = dir;
    if (!p->identity) {
        const f32 *m = p->rot.m;
        d = de__d3((f64)m[0] * dir.x + (f64)m[1] * dir.y + (f64)m[2] * dir.z,
                   (f64)m[3] * dir.x + (f64)m[4] * dir.y + (f64)m[5] * dir.z,
                   (f64)m[6] * dir.x + (f64)m[7] * dir.y + (f64)m[8] * dir.z);
    }
    de_vec3 v;
    u32 idx = 0;
    if (p->shape == NULL) {
        idx = points_support(p->points, p->count, d);
        v = p->points[idx];
    } else {
        const de__shape *s = p->shape;
        if (s->kind == DE_SHAPE_CONVEX) {
            const de__hull *h = &s->u.hull;
            idx = points_support(h->vertices, h->vertex_count, d);
            v = h->vertices[idx];
        } else {
            const de_vec3 df = de__v3((f32)d.x, (f32)d.y, (f32)d.z);
            v = p->core ? de__shape_support_core(s, df) : de__shape_support(s, df);
            if (s->kind == DE_SHAPE_BOX)
                idx =
                    (df.x >= 0.0f ? 1u : 0u) | (df.y >= 0.0f ? 2u : 0u) | (df.z >= 0.0f ? 4u : 0u);
            else if (s->kind == DE_SHAPE_CAPSULE)
                idx = df.z >= 0.0f ? 1u : 0u;
        }
    }
    if (index != NULL)
        *index = idx;
    if (p->identity)
        return de__d3_from(v);
    const f32 *m = p->rot.m;
    return de__d3((f64)m[0] * v.x + (f64)m[3] * v.y + (f64)m[6] * v.z + p->pos.x,
                  (f64)m[1] * v.x + (f64)m[4] * v.y + (f64)m[7] * v.z + p->pos.y,
                  (f64)m[2] * v.x + (f64)m[5] * v.y + (f64)m[8] * v.z + p->pos.z);
}

/* ---- Closest point of a simplex -------------------------------------------------------- */

/* Barycentric weights of the point of the affine hull of y[idx[0..k)] closest to the origin.
 * false when the subset is degenerate. */
static bool affine_closest(const de_dvec3 *y, const u32 *idx, u32 k, f64 *lam) {
    if (k == 1) {
        lam[0] = 1.0;
        return true;
    }
    const de_dvec3 y0 = y[idx[0]];
    de_dvec3 e[3];
    for (u32 i = 1; i < k; ++i)
        e[i - 1] = de__d3_sub(y[idx[i]], y0);
    f64 g[3][3], rhs[3];
    for (u32 i = 0; i + 1 < k; ++i) {
        for (u32 j = 0; j + 1 < k; ++j)
            g[i][j] = de__d3_dot(e[i], e[j]);
        rhs[i] = -de__d3_dot(e[i], y0);
    }
    f64 mu[3];
    if (k == 2) {
        if (!(g[0][0] > 1e-24))
            return false;
        mu[0] = rhs[0] / g[0][0];
    } else if (k == 3) {
        const f64 det = g[0][0] * g[1][1] - g[0][1] * g[1][0];
        if (!(det > 1e-12 * g[0][0] * g[1][1]) || !(det > 0.0))
            return false;
        mu[0] = (rhs[0] * g[1][1] - g[0][1] * rhs[1]) / det;
        mu[1] = (g[0][0] * rhs[1] - rhs[0] * g[1][0]) / det;
    } else {
        const f64 c00 = g[1][1] * g[2][2] - g[1][2] * g[2][1];
        const f64 c01 = g[1][2] * g[2][0] - g[1][0] * g[2][2];
        const f64 c02 = g[1][0] * g[2][1] - g[1][1] * g[2][0];
        const f64 det = g[0][0] * c00 + g[0][1] * c01 + g[0][2] * c02;
        if (!(det > 1e-12 * g[0][0] * g[1][1] * g[2][2]) || !(det > 0.0))
            return false;
        /* Cramer's rule. */
        f64 m[3][3];
        for (u32 c = 0; c < 3; ++c) {
            for (u32 r = 0; r < 3; ++r)
                for (u32 q = 0; q < 3; ++q)
                    m[r][q] = q == c ? rhs[r] : g[r][q];
            mu[c] = (m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                     m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                     m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0])) /
                    det;
        }
    }
    f64 sum = 0.0;
    for (u32 i = 0; i + 1 < k; ++i) {
        lam[i + 1] = mu[i];
        sum += mu[i];
    }
    lam[0] = 1.0 - sum;
    return true;
}

typedef struct simplex {
    u32 n;
    de_dvec3 w[4], a[4], b[4];
    u32 ia[4], ib[4];
    f64 lam[4];
} simplex;

/* Reduces s to the sub-simplex of its closest point to the origin; returns |v|^2 and v. */
static f64 solve(simplex *s, de_dvec3 *v) {
    u32 best_mask = 0;
    f64 best_d2 = 0.0, best_lam[4] = {0, 0, 0, 0};
    de_dvec3 best_v = de__d3(0.0, 0.0, 0.0);
    const u32 full = (1u << s->n) - 1u;
    for (u32 mask = 1; mask <= full; ++mask) {
        u32 idx[4], k = 0;
        for (u32 i = 0; i < s->n; ++i)
            if (mask & (1u << i))
                idx[k++] = i;
        f64 lam[4];
        if (!affine_closest(s->w, idx, k, lam))
            continue;
        bool valid = true;
        for (u32 i = 0; i < k; ++i)
            if (!(lam[i] > 0.0))
                valid = false;
        if (!valid)
            continue;
        de_dvec3 p = de__d3(0.0, 0.0, 0.0);
        for (u32 i = 0; i < k; ++i)
            p = de__d3_add(p, de__d3_scale(s->w[idx[i]], lam[i]));
        const f64 d2 = de__d3_dot(p, p);
        if (best_mask == 0 || d2 < best_d2) {
            best_mask = mask;
            best_d2 = d2;
            best_v = p;
            for (u32 i = 0; i < 4; ++i)
                best_lam[i] = i < k ? lam[i] : 0.0;
        }
    }
    /* Singletons always qualify, so best_mask != 0. */
    simplex r;
    r.n = 0;
    for (u32 i = 0; i < s->n; ++i) {
        if (best_mask & (1u << i)) {
            r.w[r.n] = s->w[i];
            r.a[r.n] = s->a[i];
            r.b[r.n] = s->b[i];
            r.ia[r.n] = s->ia[i];
            r.ib[r.n] = s->ib[i];
            r.lam[r.n] = best_lam[r.n];
            ++r.n;
        }
    }
    *s = r;
    *v = best_v;
    return best_d2;
}

/* ---- GJK ------------------------------------------------------------------------------- */

static void add_vertex(simplex *s, const de__gjk_proxy *a, const de__gjk_proxy *b, de_dvec3 d) {
    const u32 k = s->n;
    s->a[k] = de__gjk_support(a, d, &s->ia[k]);
    s->b[k] = de__gjk_support(b, de__d3(-d.x, -d.y, -d.z), &s->ib[k]);
    s->w[k] = de__d3_sub(s->a[k], s->b[k]);
    s->lam[k] = 0.0;
    s->n = k + 1u;
}

static void finish(const simplex *s, f64 d2, bool overlap, u32 iter, de__gjk_result *out) {
    memset(out, 0, sizeof *out);
    out->overlap = overlap;
    out->iterations = iter;
    out->count = s->n;
    de_dvec3 pa = de__d3(0.0, 0.0, 0.0), pb = de__d3(0.0, 0.0, 0.0);
    for (u32 i = 0; i < s->n; ++i) {
        out->a[i] = s->a[i];
        out->b[i] = s->b[i];
        out->ia[i] = s->ia[i];
        out->ib[i] = s->ib[i];
        out->lambda[i] = s->lam[i];
        pa = de__d3_add(pa, de__d3_scale(s->a[i], s->lam[i]));
        pb = de__d3_add(pb, de__d3_scale(s->b[i], s->lam[i]));
    }
    out->point_a = pa;
    out->point_b = pb;
    out->distance = overlap ? 0.0 : de_dsqrt64(d2);
}

void de__gjk_distance(const de__gjk_proxy *a, const de__gjk_proxy *b, de__gjk_result *out) {
    simplex s;
    s.n = 0;
    /* First direction: from B's frame origin toward A's. */
    de_dvec3 d =
        de__d3(-(f64)b->pos.x + a->pos.x, -(f64)b->pos.y + a->pos.y, -(f64)b->pos.z + a->pos.z);
    if (!(de__d3_dot(d, d) > 0.0))
        d = de__d3(1.0, 0.0, 0.0);
    add_vertex(&s, a, b, d);
    s.lam[0] = 1.0;
    de_dvec3 v = s.w[0];
    f64 d2 = de__d3_dot(v, v);
    u32 iter = 0;
    for (; iter < DE__GJK_MAX_ITER; ++iter) {
        if (!(d2 > 1e-24)) {
            finish(&s, 0.0, true, iter, out);
            return;
        }
        const de_dvec3 dir = de__d3(-v.x, -v.y, -v.z);
        simplex next = s;
        add_vertex(&next, a, b, dir);
        const de_dvec3 w = next.w[next.n - 1];
        /* Converged: the new point does not get closer than v (relative tolerance). */
        if (d2 - de__d3_dot(v, w) <= 1e-12 * d2)
            break;
        bool dup = false;
        for (u32 i = 0; i < s.n; ++i)
            if (s.w[i].x == w.x && s.w[i].y == w.y && s.w[i].z == w.z)
                dup = true;
        if (dup)
            break;
        de_dvec3 nv;
        const f64 nd2 = solve(&next, &nv);
        if (next.n == 4 || !(nd2 > 1e-24)) {
            finish(&next, 0.0, true, iter + 1u, out);
            return;
        }
        if (!(nd2 < d2))
            break; /* no progress: keep the previous simplex */
        s = next;
        v = nv;
        d2 = nd2;
    }
    finish(&s, d2, false, iter, out);
}
