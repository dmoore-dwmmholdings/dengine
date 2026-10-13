/*
 * closest_point.c: de_physics_closest_point (DENG-137).
 *
 * Convex shapes: GJK distance (narrow/gjk.c) between the shape's core and the query point in the
 * shape's frame, minus the shape's radius; a point inside the solid has distance 0. Meshes and
 * heightfields: the closest point of each triangle in the box around the point (Ericson's
 * region test, f64). Compounds: each child in the box. Inside one body the smallest distance
 * wins, ties to the lower sub-shape; over bodies the smallest fraction, ties to the lower body
 * index.
 */
#include "physics/query/query_internal.h"

/* A closest-point result for one side (world). `inside`: the point is inside a solid. */
typedef struct cp_hit {
    f64 dist;
    de_dvec3 point;
    de_dvec3 normal;
    u32 sub;
    bool inside;
} cp_hit;

static bool cp_better(const cp_hit *h, bool found, const cp_hit *best) {
    if (!found)
        return true;
    if (h->dist != best->dist)
        return h->dist < best->dist;
    return h->sub < best->sub;
}

/* Closest point of triangle (a, b, c) to p (Ericson, Real-Time Collision Detection 5.1.5). */
static de_dvec3 closest_on_tri(de_dvec3 p, de_dvec3 a, de_dvec3 b, de_dvec3 c) {
    const de_dvec3 ab = de__d3_sub(b, a), ac = de__d3_sub(c, a), ap = de__d3_sub(p, a);
    const f64 d1 = de__d3_dot(ab, ap), d2 = de__d3_dot(ac, ap);
    if (d1 <= 0.0 && d2 <= 0.0)
        return a;
    const de_dvec3 bp = de__d3_sub(p, b);
    const f64 d3 = de__d3_dot(ab, bp), d4 = de__d3_dot(ac, bp);
    if (d3 >= 0.0 && d4 <= d3)
        return b;
    const f64 vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0)
        return de__d3_add(a, de__d3_scale(ab, d1 / (d1 - d3)));
    const de_dvec3 cp = de__d3_sub(p, c);
    const f64 d5 = de__d3_dot(ab, cp), d6 = de__d3_dot(ac, cp);
    if (d6 >= 0.0 && d5 <= d6)
        return c;
    const f64 vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0)
        return de__d3_add(a, de__d3_scale(ac, d2 / (d2 - d6)));
    const f64 va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0)
        return de__d3_add(b, de__d3_scale(de__d3_sub(c, b), (d4 - d3) / ((d4 - d3) + (d5 - d6))));
    const f64 denom = 1.0 / (va + vb + vc);
    const f64 v = vb * denom, w = vc * denom;
    return de__d3_add(a, de__d3_add(de__d3_scale(ab, v), de__d3_scale(ac, w)));
}

typedef struct tri_job {
    de_dvec3 p; /* query point, shape frame */
    f64 max_dist;
    bool found;
    f64 best_d;
    u32 best_sub;
    de_dvec3 best_c, best_n;
} tri_job;

static bool tri_cb(void *user, u32 sub, de_vec3 a, de_vec3 b, de_vec3 c) {
    tri_job *j = (tri_job *)user;
    const de_dvec3 da = de__d3_from(a), db = de__d3_from(b), dc = de__d3_from(c);
    const de_dvec3 q = closest_on_tri(j->p, da, db, dc);
    const de_dvec3 v = de__d3_sub(j->p, q);
    f64 d = de_dsqrt64(de__d3_dot(v, v));
    /* A point on the triangle (up to the rounding of the region test) is at distance 0. */
    const f64 scale =
        1.0 + de__maxd(de__absd(j->p.x), de__maxd(de__absd(j->p.y), de__absd(j->p.z)));
    if (d <= 1e-9 * scale)
        d = 0.0;
    if (!(d <= j->max_dist))
        return true;
    if (j->found && (d > j->best_d || (d == j->best_d && sub > j->best_sub)))
        return true;
    j->found = true;
    j->best_d = d;
    j->best_sub = sub;
    j->best_c = q;
    /* On the triangle: its front normal; else the direction toward the point. */
    j->best_n = d > 0.0 ? de__d3_scale(v, 1.0 / d)
                        : de__q_dnorm(de__d3_cross(de__d3_sub(db, da), de__d3_sub(dc, da)));
    return true;
}

static bool closest_side(const de__qside *s, de_dvec3 point, f64 max_dist, cp_hit *out) {
    const de__dmat3 w = de__dmat3_from_quat(s->pose.rotation);
    const de_dvec3 lp = de__dmat3_tmul(&w, de__d3_sub(point, s->pose.position));
    const de_shape_kind k = s->shape->kind;
    if (k == DE_SHAPE_COMPOUND || k == DE_SHAPE_MESH || k == DE_SHAPE_HEIGHTFIELD) {
        /* The box of everything within max_dist of the point (shape frame), rounded outward. */
        de_aabb box;
        box.min = de__v3(de__f32_round_down(lp.x - max_dist), de__f32_round_down(lp.y - max_dist),
                         de__f32_round_down(lp.z - max_dist));
        box.max = de__v3(de__f32_round_up(lp.x + max_dist), de__f32_round_up(lp.y + max_dist),
                         de__f32_round_up(lp.z + max_dist));
        if (k == DE_SHAPE_COMPOUND) {
            const de__compound *cp = &s->shape->u.compound;
            bool found = false;
            cp_hit best;
            memset(&best, 0, sizeof best);
            for (u32 i = 0; i < cp->child_count; ++i) {
                if (!de__aabb_overlap(&cp->children[i].bounds, &box))
                    continue;
                const de__qside child = de__q_child_side(s, i);
                cp_hit h;
                if (!closest_side(&child, point, max_dist, &h))
                    continue;
                h.sub = i;
                if (cp_better(&h, found, &best)) {
                    found = true;
                    best = h;
                }
            }
            if (found)
                *out = best;
            return found;
        }
        tri_job j;
        memset(&j, 0, sizeof j);
        j.p = lp;
        j.max_dist = max_dist;
        de__q_triangles(s->shape, box, tri_cb, &j);
        if (!j.found)
            return false;
        out->dist = j.best_d;
        out->point = de__d3_add(s->pose.position, de__dmat3_mul(&w, j.best_c));
        out->normal = de__q_dnorm(de__dmat3_mul(&w, j.best_n));
        out->sub = j.best_sub;
        out->inside = false;
        return true;
    }
    /* Convex: GJK between the core and the point (shape frame). */
    const de_vec3 pt = de__q_to_f32(lp);
    de__gjk_proxy pa, pb;
    de__gjk_proxy_shape(&pa, s->shape, true, NULL);
    de__gjk_proxy_points(&pb, &pt, 1, NULL);
    de__gjk_result g;
    de__gjk_distance(&pa, &pb, &g);
    const f64 r = (f64)de__shape_margin(s->shape);
    if (g.overlap || g.distance <= r) {
        out->dist = 0.0;
        out->point = point;
        out->normal = de__d3(0.0, 0.0, 0.0);
        out->sub = DE_SUB_SHAPE_NONE;
        out->inside = true;
        return true;
    }
    const f64 d = g.distance - r;
    if (!(d <= max_dist))
        return false;
    const de_dvec3 n = de__d3_scale(de__d3_sub(g.point_b, g.point_a), 1.0 / g.distance);
    const de_dvec3 c = de__d3_add(g.point_a, de__d3_scale(n, r));
    out->dist = d;
    out->point = de__d3_add(s->pose.position, de__dmat3_mul(&w, c));
    out->normal = de__q_dnorm(de__dmat3_mul(&w, n));
    out->sub = DE_SUB_SHAPE_NONE;
    out->inside = false;
    return true;
}

bool de__query_closest_body(const de_physics_world *p, u32 body, de_dvec3 point, f64 max_dist,
                            de_ray_hit *out) {
    de__qside s;
    s.shape = de__shape_get(p, p->bodies.shape[body]);
    if (s.shape == NULL)
        return false;
    s.pose.position = p->bodies.position[body];
    s.pose.rotation = p->bodies.rotation[body];
    s.sub = DE_SUB_SHAPE_NONE;
    cp_hit h;
    if (!closest_side(&s, point, max_dist, &h))
        return false;
    out->entity = p->bodies.owner[body];
    out->body = de__body_handle(p, body);
    out->point = h.inside ? point : h.point;
    out->normal = de__q_to_f32(h.normal);
    out->fraction = max_dist > 0.0 ? (f32)(h.dist / max_dist) : 0.0f;
    out->sub_shape = h.sub;
    return true;
}

typedef struct cp_ctx {
    const de_physics_world *p;
    de_dvec3 point;
    f64 max_dist;
    bool found;
    de_ray_hit best;
} cp_ctx;

static bool cp_cb(void *user, u32 body) {
    cp_ctx *c = (cp_ctx *)user;
    de_ray_hit h;
    if (!de__query_closest_body(c->p, body, c->point, c->max_dist, &h))
        return true;
    if (!c->found || de__q_hit_less(&h, &c->best)) {
        c->found = true;
        c->best = h;
    }
    return true;
}

static bool closest_point(const de_physics_world *p, de_dvec3 point, f32 max_dist,
                          const de__qfilter *f, de_ray_hit *hit) {
    if (p == NULL || hit == NULL || !de__d3_finite(point) || !de__f32_finite(max_dist) ||
        !(max_dist >= 0.0f))
        return false;
    const f64 r = (f64)max_dist;
    de_aabb box;
    box.min = de__v3(de__f32_round_down(point.x - r), de__f32_round_down(point.y - r),
                     de__f32_round_down(point.z - r));
    box.max = de__v3(de__f32_round_up(point.x + r), de__f32_round_up(point.y + r),
                     de__f32_round_up(point.z + r));
    cp_ctx c;
    memset(&c, 0, sizeof c);
    c.p = p;
    c.point = point;
    c.max_dist = r;
    de__query_each_aabb(p, box, f, cp_cb, &c);
    if (c.found)
        *hit = c.best;
    return c.found;
}

bool de_physics_closest_point(const de_physics_world *p, de_dvec3 point, f32 max_dist,
                              de_collision_filter filter, de_ray_hit *hit) {
    de__qfilter f;
    de__qfilter_plain(filter, &f);
    return closest_point(p, point, max_dist, &f, hit);
}

bool de_physics_closest_point_ex(const de_physics_world *p, de_dvec3 point, f32 max_dist,
                                 const de_physics_query_filter *filter, de_ray_hit *hit) {
    if (p == NULL)
        return false;
    de__qfilter f;
    de__qfilter_init(p, filter, &f);
    return closest_point(p, point, max_dist, &f, hit);
}
