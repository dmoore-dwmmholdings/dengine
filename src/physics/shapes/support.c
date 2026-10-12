/* support.c: support functions and world AABBs of shapes (DENG-132). */
#include "physics/shapes/shape_internal.h"

/* ---- Support --------------------------------------------------------------------------- */

static de_vec3 hull_support(const de__hull *h, de_vec3 dir) {
    u32 best = 0;
    f32 best_dot = de__v3_dot(h->vertices[0], dir);
    for (u32 i = 1; i < h->vertex_count; ++i) {
        const f32 d = de__v3_dot(h->vertices[i], dir);
        if (d > best_dot) { /* strict: ties keep the lowest index */
            best_dot = d;
            best = i;
        }
    }
    return h->vertices[best];
}

de_vec3 de__shape_support_core(const de__shape *s, de_vec3 dir) {
    switch (s->kind) {
    case DE_SHAPE_SPHERE:
        return de__v3(0.0f, 0.0f, 0.0f);
    case DE_SHAPE_CAPSULE: {
        const f32 h = s->u.capsule.half_height;
        return de__v3(0.0f, 0.0f, dir.z >= 0.0f ? h : -h);
    }
    case DE_SHAPE_BOX: {
        const de_vec3 e = s->u.box.half_extents;
        return de__v3(dir.x >= 0.0f ? e.x : -e.x, dir.y >= 0.0f ? e.y : -e.y,
                      dir.z >= 0.0f ? e.z : -e.z);
    }
    case DE_SHAPE_CYLINDER: {
        const f32 r = s->u.cylinder.radius, h = s->u.cylinder.half_height;
        const f32 z = dir.z >= 0.0f ? h : -h;
        const f64 len2 = (f64)dir.x * dir.x + (f64)dir.y * dir.y;
        if (!(len2 > 0.0))
            return de__v3(0.0f, 0.0f, z);
        const f64 k = (f64)r / de_dsqrt64(len2);
        return de__v3((f32)(dir.x * k), (f32)(dir.y * k), z);
    }
    case DE_SHAPE_CONVEX:
        return hull_support(&s->u.hull, dir);
    default:
        return de__v3(0.0f, 0.0f, 0.0f);
    }
}

de_vec3 de__shape_support(const de__shape *s, de_vec3 dir) {
    const de_vec3 core = de__shape_support_core(s, dir);
    const f32 m = s->margin;
    if (m == 0.0f)
        return core;
    const f64 len2 = (f64)dir.x * dir.x + (f64)dir.y * dir.y + (f64)dir.z * dir.z;
    if (!(len2 > 0.0))
        return de__v3(core.x, core.y, core.z + m);
    const f64 k = (f64)m / de_dsqrt64(len2);
    return de__v3((f32)((f64)core.x + dir.x * k), (f32)((f64)core.y + dir.y * k),
                  (f32)((f64)core.z + dir.z * k));
}

/* ---- World AABBs ----------------------------------------------------------------------- */

/* Pads x away from the box by a few f64 ulps of |x| before rounding to f32, so f64 rounding in
 * the transform can never leave the f32 box inside the exact one. */
static f32 lower(f64 x) {
    return de__f32_round_down(x - de__absd(x) * 0x1p-50);
}
static f32 upper(f64 x) {
    return de__f32_round_up(x + de__absd(x) * 0x1p-50);
}

static de_aabb aabb_from_d(de_dvec3 lo, de_dvec3 hi) {
    de_aabb b;
    b.min = de__v3(lower(lo.x), lower(lo.y), lower(lo.z));
    b.max = de__v3(upper(hi.x), upper(hi.y), upper(hi.z));
    return b;
}

static de_aabb aabb_around(de_dvec3 c, de_dvec3 e) {
    return aabb_from_d(de__d3_sub(c, e), de__d3_add(c, e));
}

de_aabb de__aabb_transform(de_aabb local, de_dvec3 pos, const de__dmat3 *r) {
    if (local.min.x > local.max.x || local.min.y > local.max.y || local.min.z > local.max.z)
        return local;
    const de_dvec3 c =
        de__d3(((f64)local.min.x + local.max.x) * 0.5, ((f64)local.min.y + local.max.y) * 0.5,
               ((f64)local.min.z + local.max.z) * 0.5);
    const de_dvec3 e =
        de__d3(((f64)local.max.x - local.min.x) * 0.5, ((f64)local.max.y - local.min.y) * 0.5,
               ((f64)local.max.z - local.min.z) * 0.5);
    const de_dvec3 wc = de__d3_add(pos, de__dmat3_mul(r, c));
    /* Arvo: each world half extent is the sum of |R_ij| * e_j over j. */
    const f64 *m = r->m;
    const de_dvec3 we = de__d3(de__absd(m[0]) * e.x + de__absd(m[3]) * e.y + de__absd(m[6]) * e.z,
                               de__absd(m[1]) * e.x + de__absd(m[4]) * e.y + de__absd(m[7]) * e.z,
                               de__absd(m[2]) * e.x + de__absd(m[5]) * e.y + de__absd(m[8]) * e.z);
    return aabb_around(wc, we);
}

de_aabb de__shape_world_aabb(const de__shape *s, de_dvec3 pos, de_quat rot) {
    const de__dmat3 r = de__dmat3_from_quat(rot);
    switch (s->kind) {
    case DE_SHAPE_SPHERE: {
        const f64 rad = s->u.sphere.radius;
        return aabb_around(pos, de__d3(rad, rad, rad));
    }
    case DE_SHAPE_CAPSULE: {
        const f64 rad = s->u.capsule.radius, h = s->u.capsule.half_height;
        /* Axis = third column of R. */
        const de_dvec3 e = de__d3(de__absd(r.m[6]) * h + rad, de__absd(r.m[7]) * h + rad,
                                  de__absd(r.m[8]) * h + rad);
        return aabb_around(pos, e);
    }
    case DE_SHAPE_CYLINDER: {
        const f64 rad = s->u.cylinder.radius, h = s->u.cylinder.half_height;
        f64 e[3];
        for (u32 i = 0; i < 3; ++i) {
            const f64 a = r.m[6 + i];
            /* The disc of radius rad orthogonal to the axis reaches rad * sqrt(1 - a_i^2). */
            e[i] = de__absd(a) * h + rad * de_dsqrt64(de__maxd(0.0, 1.0 - a * a));
        }
        return aabb_around(pos, de__d3(e[0], e[1], e[2]));
    }
    default:
        return de__aabb_transform(s->local_aabb, pos, &r);
    }
}
