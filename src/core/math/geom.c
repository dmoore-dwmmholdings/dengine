/* geom.c: boxes, planes, rays and frustums, scalar reference (SPEC §5.4, DENG-241). */
#include "core/math/math_internal.h"
#include "core/math/math_simd.h"

/* ---- de_aabb ---- */

de_aabb de_aabb_empty(void) {
    de_aabb b = {{FLT_MAX, FLT_MAX, FLT_MAX}, {-FLT_MAX, -FLT_MAX, -FLT_MAX}};
    return b;
}

bool de_aabb_is_empty(de_aabb b) {
    return b.min.x > b.max.x || b.min.y > b.max.y || b.min.z > b.max.z;
}

de_aabb de_aabb_add_point(de_aabb b, de_vec3 p) {
    b.min = de_v3_min(b.min, p);
    b.max = de_v3_max(b.max, p);
    return b;
}

de_aabb de_aabb_from_points(const de_vec3 *points, size_t count) {
    de_aabb b = de_aabb_empty();
    if (points == NULL)
        return b;
    for (size_t i = 0; i < count; ++i)
        b = de_aabb_add_point(b, points[i]);
    return b;
}

de_aabb de_aabb_merge(de_aabb a, de_aabb b) {
    de_aabb r = {de_v3_min(a.min, b.min), de_v3_max(a.max, b.max)};
    return r;
}

de_vec3 de_aabb_center(de_aabb b) {
    return de_v3_scale(de_v3_add(b.min, b.max), 0.5f);
}

de_vec3 de_aabb_extents(de_aabb b) {
    return de_v3_scale(de_v3_sub(b.max, b.min), 0.5f);
}

de_aabb de_aabb_transform(de_aabb b, const de_affine *a) {
    /* Arvo, in center/extents form: the new center is the transformed center, and each new half
     * extent is the sum of |L_ij| * e_j (the box's corners projected on each axis). */
    if (de_aabb_is_empty(b))
        return b;
    de_vec3 c = de_affine_transform_point(a, de_aabb_center(b));
    de_vec3 e = de_aabb_extents(b);
    const f32 *m = a->m;
    de_vec3 ne = de__v3(fabsf(m[0]) * e.x + fabsf(m[3]) * e.y + fabsf(m[6]) * e.z,
                        fabsf(m[1]) * e.x + fabsf(m[4]) * e.y + fabsf(m[7]) * e.z,
                        fabsf(m[2]) * e.x + fabsf(m[5]) * e.y + fabsf(m[8]) * e.z);
    de_aabb r = {de_v3_sub(c, ne), de_v3_add(c, ne)};
    return r;
}

bool de_aabb_contains(de_aabb b, de_vec3 p) {
    return p.x >= b.min.x && p.x <= b.max.x && p.y >= b.min.y && p.y <= b.max.y && p.z >= b.min.z &&
           p.z <= b.max.z;
}

bool de_aabb_intersects(de_aabb a, de_aabb b) {
    if (de_aabb_is_empty(a) || de_aabb_is_empty(b))
        return false;
    return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y && a.max.y >= b.min.y &&
           a.min.z <= b.max.z && a.max.z >= b.min.z;
}

/* ---- de_plane ---- */

de_plane de_plane_from_point_normal(de_vec3 point, de_vec3 normal) {
    de_plane p;
    p.n = de_v3_normalize(normal);
    p.d = -de_v3_dot(p.n, point);
    return p;
}

de_plane de_plane_normalize(de_plane p) {
    f32 len = de_v3_length(p.n);
    if (!(len > 0.0f))
        return p;
    f32 inv = 1.0f / len;
    p.n = de_v3_scale(p.n, inv);
    p.d *= inv;
    return p;
}

f32 de_plane_distance(de_plane p, de_vec3 point) {
    return de_v3_dot(p.n, point) + p.d;
}

/* ---- de_ray ---- */

bool de_ray_aabb(de_ray r, de_aabb box, f32 *t_hit) {
    if (de_aabb_is_empty(box))
        return false;
    const f32 o[3] = {r.origin.x, r.origin.y, r.origin.z};
    const f32 d[3] = {r.dir.x, r.dir.y, r.dir.z};
    const f32 lo[3] = {box.min.x, box.min.y, box.min.z};
    const f32 hi[3] = {box.max.x, box.max.y, box.max.z};
    f32 tmin = -FLT_MAX, tmax = FLT_MAX;
    for (int i = 0; i < 3; ++i) {
        if (d[i] == 0.0f) {
            /* Parallel to this slab: 0 * inf would give NaN, so decide directly. */
            if (o[i] < lo[i] || o[i] > hi[i])
                return false;
            continue;
        }
        f32 inv = 1.0f / d[i];
        f32 t1 = (lo[i] - o[i]) * inv;
        f32 t2 = (hi[i] - o[i]) * inv;
        if (t1 > t2) {
            f32 tmp = t1;
            t1 = t2;
            t2 = tmp;
        }
        tmin = t1 > tmin ? t1 : tmin;
        tmax = t2 < tmax ? t2 : tmax;
        if (tmin > tmax)
            return false;
    }
    if (tmax < 0.0f)
        return false; /* the box is behind the origin */
    if (t_hit)
        *t_hit = tmin > 0.0f ? tmin : 0.0f;
    return true;
}

bool de_ray_plane(de_ray r, de_plane p, f32 *t_hit) {
    f32 denom = de_v3_dot(p.n, r.dir);
    if (denom == 0.0f)
        return false;
    f32 t = -de_plane_distance(p, r.origin) / denom;
    if (!(t >= 0.0f))
        return false;
    if (t_hit)
        *t_hit = t;
    return true;
}

bool de_ray_triangle(de_ray r, de_vec3 a, de_vec3 b, de_vec3 c, f32 *t_hit, de_vec2 *uv) {
    de_vec3 e1 = de_v3_sub(b, a);
    de_vec3 e2 = de_v3_sub(c, a);
    de_vec3 p = de_v3_cross(r.dir, e2);
    f32 det = de_v3_dot(e1, p);
    /* det = dir . (e1 x e2) (up to sign): ~0 for a degenerate triangle or a ray in its plane.
     * Scale the threshold by the inputs so it means "within ~1e-7 rad", whatever the units. */
    f32 scale = de_v3_length(e1) * de_v3_length(e2) * de_v3_length(r.dir);
    if (!(fabsf(det) > 1e-7f * scale))
        return false;
    f32 inv = 1.0f / det;
    de_vec3 s = de_v3_sub(r.origin, a);
    f32 u = de_v3_dot(s, p) * inv;
    if (u < 0.0f || u > 1.0f)
        return false;
    de_vec3 q = de_v3_cross(s, e1);
    f32 v = de_v3_dot(r.dir, q) * inv;
    if (v < 0.0f || u + v > 1.0f)
        return false;
    f32 t = de_v3_dot(e2, q) * inv;
    if (t < 0.0f)
        return false;
    if (t_hit)
        *t_hit = t;
    if (uv) {
        uv->x = u;
        uv->y = v;
    }
    return true;
}

/* ---- de_frustum ---- */

/* Row i of a column-major de_mat4 as a plane (a, b, c, d). */
static void de__m4_row(const de_mat4 *m, int i, f32 out[4]) {
    out[0] = m->m[i];
    out[1] = m->m[4 + i];
    out[2] = m->m[8 + i];
    out[3] = m->m[12 + i];
}

/* (a + sign * b) as a normalized plane; a zero normal becomes the always-pass plane. */
static de_plane de__frustum_plane(const f32 a[4], const f32 b[4], f32 sign) {
    de_plane p;
    p.n = de__v3(a[0] + sign * b[0], a[1] + sign * b[1], a[2] + sign * b[2]);
    p.d = a[3] + sign * b[3];
    f32 len = de_v3_length(p.n);
    if (!(len > 0.0f)) {
        /* Reverse-Z infinite far: the row is (0, 0, 0, near). Keep it out of every test. */
        p.n = de__v3(0.0f, 0.0f, 0.0f);
        p.d = FLT_MAX;
        return p;
    }
    f32 inv = 1.0f / len;
    p.n = de_v3_scale(p.n, inv);
    p.d *= inv;
    return p;
}

de_frustum de_frustum_from_m4(const de_mat4 *view_proj) {
    /* Gribb-Hartmann: clip point c = M p is inside when -w <= x <= w, -w <= y <= w and
     * 0 <= z <= w. Each inequality is a plane built from rows of M: e.g. x >= -w is
     * (row3 + row0) . (p, 1) >= 0. Reverse-Z: z <= w is the near plane (depth 1), z >= 0 the far
     * plane (depth 0). */
    f32 r0[4], r1[4], r2[4], r3[4];
    const f32 zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    de__m4_row(view_proj, 0, r0);
    de__m4_row(view_proj, 1, r1);
    de__m4_row(view_proj, 2, r2);
    de__m4_row(view_proj, 3, r3);
    de_frustum f;
    f.planes[DE_FRUSTUM_LEFT] = de__frustum_plane(r3, r0, 1.0f);
    f.planes[DE_FRUSTUM_RIGHT] = de__frustum_plane(r3, r0, -1.0f);
    f.planes[DE_FRUSTUM_BOTTOM] = de__frustum_plane(r3, r1, 1.0f);
    f.planes[DE_FRUSTUM_TOP] = de__frustum_plane(r3, r1, -1.0f);
    f.planes[DE_FRUSTUM_NEAR] = de__frustum_plane(r3, r2, -1.0f);
    f.planes[DE_FRUSTUM_FAR] = de__frustum_plane(r2, zero, 1.0f);
    return f;
}

bool de__frustum_test_aabb_scalar(const de_frustum *f, de_aabb box) {
    if (de_aabb_is_empty(box))
        return false;
    for (int i = 0; i < 6; ++i) {
        const de_plane *p = &f->planes[i];
        /* The corner farthest along the normal: if even it is behind the plane, all are. */
        de_vec3 pv =
            de__v3(p->n.x >= 0.0f ? box.max.x : box.min.x, p->n.y >= 0.0f ? box.max.y : box.min.y,
                   p->n.z >= 0.0f ? box.max.z : box.min.z);
        /* One operation per statement, so no compiler fuses a multiply-add (C11 contraction
         * applies within one expression only): the SIMD culls (DENG-242) evaluate exactly these
         * roundings and must return the same result for every box. */
        f32 dist = p->n.x * pv.x;
        f32 dy = p->n.y * pv.y;
        dist = dist + dy;
        f32 dz = p->n.z * pv.z;
        dist = dist + dz;
        dist = dist + p->d;
        if (dist < 0.0f)
            return false;
    }
    return true;
}

bool de_frustum_test_sphere(const de_frustum *f, de_vec3 center, f32 radius) {
    for (int i = 0; i < 6; ++i)
        if (de_plane_distance(f->planes[i], center) < -radius)
            return false;
    return true;
}
