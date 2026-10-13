/*
 * polygon2d.c: the deterministic 2D convex hull builder for DE_SHAPE2D_POLYGON and the polygon
 * finishing step shared with boxes (DENG-252). SPEC §13.3: at most 8 vertices.
 *
 * Steps, all in f64 on the f32 input (no allocation; at most DE_SHAPE2D_MAX_POLYGON_INPUT points):
 *   1. Weld: in input order, a point within `tol` of an already kept point is dropped.
 *   2. Sort the kept points by (x, y) (insertion sort: stable, a total order after welding).
 *   3. Andrew's monotone chain: lower then upper hull, popping on a cross product <= 0, so exactly
 *      collinear points never enter. The result is counter-clockwise from the lowest (x, y).
 *   4. Collinear removal: repeatedly drop the first vertex (in hull order) within `tol` of the
 *      line through its neighbours, until none is.
 *   5. Fewer than 3 vertices is degenerate; more than 8 is refused.
 * Every comparison is on values computed the same way on every platform (IEEE f64, no FMA), so
 * the same input gives the same bytes everywhere.
 */
#include "physics/2d/p2d_internal.h"

f32 de__poly2d_auto_tolerance(const de_vec2 *points, u32 n) {
    if (n == 0)
        return 1e-5f;
    f64 lox = points[0].x, hix = points[0].x, loy = points[0].y, hiy = points[0].y, big = 0.0;
    for (u32 i = 0; i < n; ++i) {
        const f64 x = points[i].x, y = points[i].y;
        lox = de__mind(lox, x);
        hix = de__maxd(hix, x);
        loy = de__mind(loy, y);
        hiy = de__maxd(hiy, y);
        big = de__maxd(big, de__maxd(de__absd(x), de__absd(y)));
    }
    const f64 side = de__maxd(hix - lox, hiy - loy);
    const f64 t = de__maxd(1e-5, de__maxd(1e-4 * side, 4e-7 * big));
    return de__f32_round_up(t);
}

bool de__poly2d_finish(de__polygon2d *poly) {
    const u32 n = poly->count;
    if (n < 3 || n > DE_SHAPE2D_MAX_POLYGON_VERTICES)
        return false;
    /* Edges, normals and convexity. */
    for (u32 i = 0; i < n; ++i) {
        const de__d2 a = de__d2_from(poly->vertices[i]);
        const de__d2 b = de__d2_from(poly->vertices[(i + 1u) % n]);
        const de__d2 c = de__d2_from(poly->vertices[(i + 2u) % n]);
        const de__d2 e = de__d2_sub(b, a);
        if (e.x == 0.0 && e.y == 0.0)
            return false;
        if (!(de__d2_cross(e, de__d2_sub(c, b)) > 0.0))
            return false;
        poly->normals[i] = de__v2_normalize_d(de__d2_make(e.y, -e.x));
    }
    /* Area centroid by a triangle fan from vertex 0. */
    const de__d2 o = de__d2_from(poly->vertices[0]);
    f64 area2 = 0.0;
    de__d2 acc = de__d2_make(0.0, 0.0);
    for (u32 i = 1; i + 1 < n; ++i) {
        const de__d2 e1 = de__d2_sub(de__d2_from(poly->vertices[i]), o);
        const de__d2 e2 = de__d2_sub(de__d2_from(poly->vertices[i + 1u]), o);
        const f64 d = de__d2_cross(e1, e2);
        area2 += d;
        acc = de__d2_add(acc, de__d2_scale(de__d2_add(e1, e2), d));
    }
    if (!(area2 > 0.0))
        return false;
    /* Each triangle's centroid is (o + e1 + e2) / 3, weighted by its doubled area d. */
    const de__d2 c = de__d2_add(o, de__d2_scale(acc, 1.0 / (3.0 * area2)));
    poly->centroid = de__v2((f32)c.x, (f32)c.y);
    return true;
}

/* true when a sorts before b: by x, then y, then input index. */
static bool point_less(const de__d2 *pts, const u32 *idx, u32 a, u32 b) {
    const de__d2 pa = pts[a], pb = pts[b];
    if (pa.x != pb.x)
        return pa.x < pb.x;
    if (pa.y != pb.y)
        return pa.y < pb.y;
    return idx[a] < idx[b];
}

/* cross(b - a, c - a) > 0: c is left of a -> b (counter-clockwise turn). */
static f64 turn(de__d2 a, de__d2 b, de__d2 c) {
    return de__d2_cross(de__d2_sub(b, a), de__d2_sub(c, a));
}

de_result de__poly2d_build(const de_vec2 *points, u32 n, f32 tol, f32 radius, de__polygon2d *out) {
    memset(out, 0, sizeof *out);
    if (points == NULL || n < 3 || n > DE_SHAPE2D_MAX_POLYGON_INPUT || !(tol > 0.0f))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "polygon: bad input (%u points)", n);

    /* 1. Weld in input order. */
    de__d2 pts[DE_SHAPE2D_MAX_POLYGON_INPUT];
    u32 src[DE_SHAPE2D_MAX_POLYGON_INPUT];
    u32 m = 0;
    const f64 tol2 = (f64)tol * (f64)tol;
    for (u32 i = 0; i < n; ++i) {
        const de__d2 q = de__d2_from(points[i]);
        bool dup = false;
        for (u32 j = 0; j < m && !dup; ++j) {
            const de__d2 d = de__d2_sub(q, pts[j]);
            dup = de__d2_dot(d, d) <= tol2;
        }
        if (!dup) {
            pts[m] = q;
            src[m] = i;
            ++m;
        }
    }
    if (m < 3)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "polygon: degenerate input (%u distinct points after welding)", m);

    /* 2. Sort (insertion sort on an index permutation: stable and deterministic). */
    u32 order[DE_SHAPE2D_MAX_POLYGON_INPUT];
    for (u32 i = 0; i < m; ++i)
        order[i] = i;
    for (u32 i = 1; i < m; ++i) {
        const u32 x = order[i];
        u32 j = i;
        while (j > 0 && point_less(pts, src, x, order[j - 1u])) {
            order[j] = order[j - 1u];
            --j;
        }
        order[j] = x;
    }

    /* 3. Monotone chain: lower hull left to right, upper hull right to left. */
    u32 hull[2u * DE_SHAPE2D_MAX_POLYGON_INPUT];
    u32 h = 0;
    for (u32 k = 0; k < m; ++k) {
        const u32 i = order[k];
        while (h >= 2 && turn(pts[hull[h - 2u]], pts[hull[h - 1u]], pts[i]) <= 0.0)
            --h;
        hull[h++] = i;
    }
    const u32 lower_size = h + 1u;
    for (u32 k = m - 1u; k-- > 0;) {
        const u32 i = order[k];
        while (h >= lower_size && turn(pts[hull[h - 2u]], pts[hull[h - 1u]], pts[i]) <= 0.0)
            --h;
        hull[h++] = i;
    }
    --h; /* the last point repeats the first */

    /* 4. Collinear removal within tol: drop the first offending vertex, rescan. */
    bool removed = true;
    while (removed && h >= 3) {
        removed = false;
        for (u32 k = 0; k < h; ++k) {
            const de__d2 a = pts[hull[(k + h - 1u) % h]];
            const de__d2 b = pts[hull[k]];
            const de__d2 c = pts[hull[(k + 1u) % h]];
            const de__d2 ac = de__d2_sub(c, a);
            const f64 cr = turn(a, b, c);
            /* distance(b, line a c) = |cr| / |ac| <= tol, without a square root. */
            if (cr * cr <= tol2 * de__d2_dot(ac, ac)) {
                for (u32 j = k; j + 1 < h; ++j)
                    hull[j] = hull[j + 1u];
                --h;
                removed = true;
                break;
            }
        }
    }

    /* 5. Limits. */
    if (h < 3)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "polygon: degenerate input (all points on one line within %g m)",
                       (double)tol);
    if (h > DE_SHAPE2D_MAX_POLYGON_VERTICES)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "polygon: the hull has %u vertices (at most %u)", h,
                       DE_SHAPE2D_MAX_POLYGON_VERTICES);

    out->count = h;
    out->radius = radius;
    for (u32 k = 0; k < h; ++k)
        out->vertices[k] = points[src[hull[k]]];
    if (!de__poly2d_finish(out)) {
        memset(out, 0, sizeof *out);
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "polygon: degenerate hull");
    }
    return DE_OK;
}
