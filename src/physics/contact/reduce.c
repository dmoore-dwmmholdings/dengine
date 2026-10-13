/*
 * reduce.c: contact reduction to at most four points (DENG-135). See manifold.h.
 *
 * The deepest point always stays (it carries the largest correction); the other three are the
 * ones that make the largest contact patch with it, measured as the area of the convex hull of
 * the four points projected on the contact plane. With at most 8 points there are at most 35
 * candidate triples, so every one is tried: the result is the exact maximum, not a greedy guess.
 * Ties go to the larger spread (so collinear points keep their end points), then to the lower
 * feature ids, which are stable from tick to tick.
 *
 * Determinism: f32 + - * / only, fixed loop order, explicit tie rules.
 */
#include "physics/contact/manifold.h"

/* Twice the signed area of triangle (p, q, r) in the plane with unit normal n. */
static f32 tri2(de_vec3 p, de_vec3 q, de_vec3 r, de_vec3 n) {
    return de__v3_dot(de__v3_cross(de__v3_sub(q, p), de__v3_sub(r, p)), n);
}

/* Twice the area of the quadrilateral with diagonals d1, d2, in the plane with normal n. */
static f32 quad2(de_vec3 d1, de_vec3 d2, de_vec3 n) {
    return de__absf(de__v3_dot(de__v3_cross(d1, d2), n));
}

/* Twice the hull area of four points: the largest of the four triangles and of the three ways
 * to order the points as a quadrilateral. For points in convex position one ordering is the hull
 * (the others cross themselves and give less); when one point lies inside the triangle of the
 * other three, that triangle is the hull and every quadrilateral is smaller. */
static f32 hull4_2(de_vec3 p0, de_vec3 p1, de_vec3 p2, de_vec3 p3, de_vec3 n) {
    f32 best = de__absf(tri2(p0, p1, p2, n));
    best = de__maxf(best, de__absf(tri2(p0, p1, p3, n)));
    best = de__maxf(best, de__absf(tri2(p0, p2, p3, n)));
    best = de__maxf(best, de__absf(tri2(p1, p2, p3, n)));
    best = de__maxf(best, quad2(de__v3_sub(p2, p0), de__v3_sub(p3, p1), n)); /* 0 1 2 3 */
    best = de__maxf(best, quad2(de__v3_sub(p3, p0), de__v3_sub(p2, p1), n)); /* 0 1 3 2 */
    best = de__maxf(best, quad2(de__v3_sub(p1, p0), de__v3_sub(p3, p2), n)); /* 0 2 1 3 */
    return best;
}

f32 de__contact_quad_area(de_vec3 p0, de_vec3 p1, de_vec3 p2, de_vec3 p3, de_vec3 n) {
    return 0.5f * hull4_2(p0, p1, p2, p3, n);
}

/* Squared distance of p and q along the plane with unit normal n. */
static f32 plane_dist2(de_vec3 p, de_vec3 q, de_vec3 n) {
    de_vec3 d = de__v3_sub(q, p);
    d = de__v3_sub(d, de__v3_scale(n, de__v3_dot(d, n)));
    return de__v3_dot(d, d);
}

/* Sorts three feature ids ascending. */
static void sort3(u32 f[3]) {
    for (u32 i = 1; i < 3; ++i)
        for (u32 j = i; j > 0 && f[j - 1] > f[j]; --j) {
            const u32 t = f[j];
            f[j] = f[j - 1];
            f[j - 1] = t;
        }
}

u32 de__contact_reduce(const de_vec3 *pos, const f32 *sep, const u32 *feature, u32 n,
                       de_vec3 normal, u32 out[DE__MANIFOLD_MAX_POINTS]) {
    DE_ASSERT(n <= DE__CONTACT_MAX_POINTS);
    if (n > DE__CONTACT_MAX_POINTS)
        n = DE__CONTACT_MAX_POINTS;
    if (n <= DE__MANIFOLD_MAX_POINTS) {
        for (u32 i = 0; i < n; ++i)
            out[i] = i;
        return n;
    }

    /* 1. The deepest point. */
    u32 d = 0;
    for (u32 i = 1; i < n; ++i)
        if (sep[i] < sep[d] || (sep[i] == sep[d] && feature[i] < feature[d]))
            d = i;

    /* 2. Every triple of the others: largest hull area, then spread, then feature ids. */
    u32 best[3] = {0, 0, 0};
    u32 best_f[3] = {0, 0, 0};
    f32 best_area = -1.0f, best_spread = -1.0f;
    for (u32 i = 0; i < n; ++i) {
        if (i == d)
            continue;
        for (u32 j = i + 1; j < n; ++j) {
            if (j == d)
                continue;
            for (u32 k = j + 1; k < n; ++k) {
                if (k == d)
                    continue;
                const de_vec3 q[4] = {pos[d], pos[i], pos[j], pos[k]};
                const f32 area = hull4_2(q[0], q[1], q[2], q[3], normal);
                f32 spread = 0.0f;
                for (u32 s = 0; s < 4; ++s)
                    for (u32 t = s + 1; t < 4; ++t)
                        spread = de__maxf(spread, plane_dist2(q[s], q[t], normal));
                u32 f[3] = {feature[i], feature[j], feature[k]};
                sort3(f);
                bool better = area > best_area;
                if (area == best_area) {
                    if (spread > best_spread)
                        better = true;
                    else if (spread == best_spread)
                        better = f[0] < best_f[0] || (f[0] == best_f[0] && f[1] < best_f[1]) ||
                                 (f[0] == best_f[0] && f[1] == best_f[1] && f[2] < best_f[2]);
                }
                if (better) {
                    best_area = area;
                    best_spread = spread;
                    best[0] = i;
                    best[1] = j;
                    best[2] = k;
                    memcpy(best_f, f, sizeof best_f);
                }
            }
        }
    }

    /* 3. Output in ascending index order. */
    u32 sel[4] = {d, best[0], best[1], best[2]};
    for (u32 i = 1; i < 4; ++i)
        for (u32 j = i; j > 0 && sel[j - 1] > sel[j]; --j) {
            const u32 t = sel[j];
            sel[j] = sel[j - 1];
            sel[j - 1] = t;
        }
    for (u32 i = 0; i < 4; ++i)
        out[i] = sel[i];
    return 4;
}
