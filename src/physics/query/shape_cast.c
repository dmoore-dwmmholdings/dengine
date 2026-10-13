/*
 * shape_cast.c: de_physics_shape_cast (DENG-137): a convex shape (or a compound of convex
 * children) swept along a straight line without rotating.
 *
 * Convex against convex: conservative advancement on GJK distance (narrow/gjk.c), with the pair
 * computed in the target's frame. At each step the closest points give a separating plane of
 * width `sep` with normal n (target toward cast shape). Translating the cast shape by s along
 * the cast closes that slab at speed -dot(n, delta), so the shapes cannot touch before
 * t + sep / speed: the step never passes the contact. When the speed is not positive the plane
 * separates them for the rest of the cast (a miss). The loop stops when the shapes are closer
 * than DE__Q_CAST_TOL (plus two f32 steps of their offset), after at most DE__Q_CAST_MAX_ITER
 * steps. Sphere and capsule radii are kept out of GJK (cores + radius), like the narrowphase.
 *
 * Meshes and heightfields: every triangle in the cast's swept box (target frame) is cast against
 * as a three-point proxy (two-sided unless DE_PHYSICS_QUERY_IGNORE_BACK_FACES). Compounds: the
 * target's children in the swept box, and each child of a compound cast shape, recursively. Inside
 * a body the hit with the smallest t wins, ties to the lower sub-shape (target) and then to the
 * lower child of the cast shape, so the result does not depend on the visiting order.
 *
 * Large worlds: the start offset of the cast shape from the target is formed in f64 and moved
 * along the cast in f64; only the current relative offset is rounded to f32 for GJK.
 */
#include "physics/query/query_internal.h"

/* Box slack for the triangle and child culling (m, plus a relative part): culling only. */
#define Q_CULL_SLACK 1e-3f
/* When the iteration cap is reached, a separation below this still counts as a hit (m). */
#define Q_CAST_CAP_SEP 1e-3

/* ---- Shared helpers (also used by overlap.c and closest_point.c) ------------------------- */

de__qside de__q_child_side(const de__qside *c, u32 i) {
    const de__compound_child *ch = &c->shape->u.compound.children[i];
    const de__dmat3 w = de__dmat3_from_quat(c->pose.rotation);
    de__qside s;
    s.shape = ch->shape;
    s.pose.position = de__d3_add(c->pose.position, de__dmat3_mul(&w, de__d3_from(ch->position)));
    s.pose.rotation = de__q_quat_mul(c->pose.rotation, ch->rotation);
    s.sub = i;
    return s;
}

static de_aabb tri_box(de_vec3 a, de_vec3 b, de_vec3 c) {
    de_aabb r;
    r.min = de__v3_min(a, de__v3_min(b, c));
    r.max = de__v3_max(a, de__v3_max(b, c));
    return r;
}

/* Cell range [*c0, *c1] of [lo, hi] / step over `cells` cells (like the midphase). */
static bool cell_range(f32 lo, f32 hi, f32 step, u32 cells, u32 *c0, u32 *c1) {
    const f32 a = lo / step, b = hi / step;
    if (!(b >= 0.0f) || !(a <= (f32)cells))
        return false;
    const f32 last = (f32)(cells - 1u);
    *c0 = a <= 0.0f ? 0u : (a >= last ? cells - 1u : (u32)a);
    *c1 = b >= last ? cells - 1u : (u32)b;
    return *c0 <= *c1;
}

void de__q_triangles(const de__shape *s, de_aabb box, de__q_tri_fn fn, void *user) {
    if (s->kind == DE_SHAPE_MESH) {
        const de__mesh *m = &s->u.mesh;
        const de__bvh *bvh = &m->bvh;
        if (bvh->node_count == 0)
            return;
        u32 stack[2u * DE__BVH_MAX_DEPTH + 2u];
        u32 sp = 0;
        stack[sp++] = 0;
        while (sp > 0) {
            const de__bvh_node *node = &bvh->nodes[stack[--sp]];
            if (!de__aabb_overlap(&node->bounds, &box))
                continue;
            if (node->count > 0) {
                for (u32 k = 0; k < node->count; ++k) {
                    const u32 t = bvh->prims[node->first + k];
                    if (m->edge_flags[t] & DE__MESH_TRI_DEGENERATE)
                        continue;
                    const u32 *ix = &m->indices[3u * t];
                    const de_vec3 a = m->vertices[ix[0]], b = m->vertices[ix[1]],
                                  c = m->vertices[ix[2]];
                    const de_aabb tb = tri_box(a, b, c);
                    if (de__aabb_overlap(&tb, &box) && !fn(user, t, a, b, c))
                        return;
                }
            } else if (sp + 2u <= 2u * DE__BVH_MAX_DEPTH + 2u) {
                stack[sp++] = node->first + 1u;
                stack[sp++] = node->first;
            }
        }
        return;
    }
    if (s->kind != DE_SHAPE_HEIGHTFIELD)
        return;
    const de__heightfield *hf = &s->u.hf;
    const u32 cells_x = hf->cols - 1u, cells_y = hf->rows - 1u;
    u32 cx0, cx1, cy0, cy1;
    if (!cell_range(box.min.x, box.max.x, hf->scale.x, cells_x, &cx0, &cx1) ||
        !cell_range(box.min.y, box.max.y, hf->scale.y, cells_y, &cy0, &cy1))
        return;
    const u32 B = DE_HEIGHTFIELD_BLOCK;
    for (u32 r = cy0; r <= cy1; ++r) {
        for (u32 c = cx0; c <= cx1; ++c) {
            const u32 blk = (r / B) * hf->blocks_x + c / B;
            if (hf->block_min[blk] > box.max.z || hf->block_max[blk] < box.min.z)
                continue;
            if (de__hf_cell_is_hole(hf, c, r))
                continue;
            de_vec3 tri[2][3];
            de__hf_cell_triangles(hf, c, r, tri);
            for (u32 k = 0; k < 2; ++k) {
                const de_aabb tb = tri_box(tri[k][0], tri[k][1], tri[k][2]);
                if (de__aabb_overlap(&tb, &box) &&
                    !fn(user, 2u * (r * cells_x + c) + k, tri[k][0], tri[k][1], tri[k][2]))
                    return;
            }
        }
    }
}

/* Is every shape of `s` a convex kind (a compound: every child)? */
static bool castable(const de__shape *s) {
    if (de__shape_kind_is_convex(s->kind))
        return true;
    if (s->kind != DE_SHAPE_COMPOUND)
        return false;
    for (u32 i = 0; i < s->u.compound.child_count; ++i)
        if (!de__shape_kind_is_convex(s->u.compound.children[i].shape->kind))
            return false;
    return s->u.compound.child_count > 0;
}

/* ---- Conservative advancement ----------------------------------------------------------- */

/* A cast result (`inside`: overlapping at t = 0; then normal and point are unused). */
typedef struct ca_hit {
    f64 t;
    de_dvec3 normal; /* world, unit, target -> cast shape */
    de_dvec3 point;  /* on the target, world */
    u32 sub;
    bool inside;
} ca_hit;

/* One convex (sub-)shape of the cast, in a target's frame. */
typedef struct ca_cast {
    const de__shape *shape; /* convex */
    de__np_rel rel;         /* at t = 0 (rotation; the offset is replaced every step) */
    de__pose target;        /* the target's world pose */
    de_dvec3 t0;            /* cast shape origin at t = 0, target frame (f64) */
    de_dvec3 dl;            /* cast delta, target frame (f64) */
    f64 max_t;
    u32 flags;
} ca_cast;

static void ca_cast_init(ca_cast *c, const de__shape *shape, de__pose target, de__pose start,
                         de_dvec3 delta, f64 max_t, u32 flags) {
    c->shape = shape;
    c->target = target;
    de__np_rel_init(&c->rel, target, start, 0.0f);
    c->t0 = de__dmat3_tmul(&c->rel.wa, de__d3_sub(start.position, target.position));
    c->dl = de__dmat3_tmul(&c->rel.wa, delta);
    c->max_t = max_t;
    c->flags = flags;
}

/* The cast's box in the target frame over [0, max_t], grown for culling. */
static de_aabb ca_swept_box(const ca_cast *c) {
    de__np_rel r = c->rel;
    r.t = de__q_to_f32(c->t0);
    const de_aabb b0 = de__np_box_in_a(&r, c->shape->local_aabb, 0.0f);
    r.t = de__q_to_f32(de__d3_add(c->t0, de__d3_scale(c->dl, c->max_t)));
    const de_aabb b1 = de__np_box_in_a(&r, c->shape->local_aabb, 0.0f);
    de_aabb u;
    u.min = de__v3_min(b0.min, b1.min);
    u.max = de__v3_max(b0.max, b1.max);
    const f32 big = de__maxf(de__maxf(de__absf(u.min.x), de__absf(u.max.x)),
                             de__maxf(de__maxf(de__absf(u.min.y), de__absf(u.max.y)),
                                      de__maxf(de__absf(u.min.z), de__absf(u.max.z))));
    const f32 g = Q_CULL_SLACK + big * 1e-6f;
    u.min = de__v3_sub(u.min, de__v3(g, g, g));
    u.max = de__v3_add(u.max, de__v3(g, g, g));
    return u;
}

/* Casts c against the target proxy `pa` (radius ra, target frame). */
static bool ca_run(const ca_cast *c, const de__gjk_proxy *pa, f32 ra, u32 sub, ca_hit *out) {
    de__gjk_proxy pb;
    de__gjk_proxy_shape(&pb, c->shape, true, &c->rel);
    const f64 r = (f64)ra + (f64)de__shape_margin(c->shape);
    f64 t = 0.0;
    de_dvec3 n = de__d3(0.0, 0.0, 0.0), pt = de__d3(0.0, 0.0, 0.0);
    for (u32 it = 0;; ++it) {
        const de_dvec3 rt = de__d3_add(c->t0, de__d3_scale(c->dl, t));
        pb.pos = de__q_to_f32(rt);
        de__gjk_result g;
        de__gjk_distance(pa, &pb, &g);
        const f64 sep = g.distance - r;
        if (g.overlap || !(sep > 0.0)) {
            if (it == 0) {
                memset(out, 0, sizeof *out);
                out->sub = sub;
                out->inside = true;
                return true;
            }
            break; /* rounding pushed the last step into contact: keep the previous plane */
        }
        n = de__d3_scale(de__d3_sub(g.point_b, g.point_a), 1.0 / g.distance);
        pt = de__d3_add(g.point_a, de__d3_scale(n, (f64)ra));
        const f64 speed = -de__d3_dot(n, c->dl);
        if (!(speed > 0.0))
            return false; /* the plane separates them for the rest of the cast */
        const f64 tol = DE__Q_CAST_TOL + 2.4e-7 * de_dsqrt64(de__d3_dot(rt, rt));
        if (sep <= tol)
            break;
        if (it + 1u >= DE__Q_CAST_MAX_ITER) {
            if (sep <= Q_CAST_CAP_SEP)
                break;
            return false;
        }
        /* Land half a tolerance short of the plane, so the last GJK is well conditioned. */
        t += (sep - 0.5 * tol) / speed;
        if (!(t <= c->max_t))
            return false;
    }
    const de__dmat3 *wa = &c->rel.wa;
    out->t = t;
    out->normal = de__q_dnorm(de__dmat3_mul(wa, n));
    out->point = de__d3_add(c->target.position, de__dmat3_mul(wa, pt));
    out->sub = sub;
    out->inside = false;
    return true;
}

static bool ca_better(const ca_hit *h, bool found, const ca_hit *best) {
    if (!found)
        return true;
    if (h->t != best->t)
        return h->t < best->t;
    return h->sub < best->sub;
}

/* ---- Cast against one target side -------------------------------------------------------- */

typedef struct tri_job {
    const ca_cast *c;
    bool found;
    ca_hit best;
} tri_job;

static bool tri_cb(void *user, u32 sub, de_vec3 a, de_vec3 b, de_vec3 c) {
    tri_job *j = (tri_job *)user;
    if ((j->c->flags & (u32)DE_PHYSICS_QUERY_IGNORE_BACK_FACES) != 0u) {
        /* Front = counter-clockwise side: moving along the front normal comes from behind. */
        const de_dvec3 n = de__d3_cross(de__d3_sub(de__d3_from(b), de__d3_from(a)),
                                        de__d3_sub(de__d3_from(c), de__d3_from(a)));
        if (!(de__d3_dot(n, j->c->dl) < 0.0))
            return true;
    }
    const de_vec3 pts[3] = {a, b, c};
    de__gjk_proxy pa;
    de__gjk_proxy_points(&pa, pts, 3, NULL);
    ca_hit h;
    if (ca_run(j->c, &pa, 0.0f, sub, &h) && ca_better(&h, j->found, &j->best)) {
        j->found = true;
        j->best = h;
    }
    return true;
}

/* The convex shape `cast` from `start` along `delta` against target side `tgt` (any kind). */
static bool cast_side(const de__shape *cast, de__pose start, de_dvec3 delta, f64 max_t, u32 flags,
                      const de__qside *tgt, ca_hit *out) {
    const de__shape *ts = tgt->shape;
    ca_cast c;
    ca_cast_init(&c, cast, tgt->pose, start, delta, max_t, flags);
    if (ts->kind == DE_SHAPE_COMPOUND) {
        /* Children whose box (compound frame) meets the swept box; ties to the lower child. */
        const de_aabb swept = ca_swept_box(&c);
        const de__compound *cp = &ts->u.compound;
        bool found = false;
        ca_hit best;
        memset(&best, 0, sizeof best);
        for (u32 i = 0; i < cp->child_count; ++i) {
            if (!de__aabb_overlap(&cp->children[i].bounds, &swept))
                continue;
            const de__qside child = de__q_child_side(tgt, i);
            ca_hit h;
            if (!cast_side(cast, start, delta, max_t, flags, &child, &h))
                continue;
            h.sub = i;
            if (ca_better(&h, found, &best)) {
                found = true;
                best = h;
            }
        }
        if (found)
            *out = best;
        return found;
    }
    if (ts->kind == DE_SHAPE_MESH || ts->kind == DE_SHAPE_HEIGHTFIELD) {
        tri_job j;
        memset(&j, 0, sizeof j);
        j.c = &c;
        de__q_triangles(ts, ca_swept_box(&c), tri_cb, &j);
        if (j.found)
            *out = j.best;
        return j.found;
    }
    de__gjk_proxy pa;
    de__gjk_proxy_shape(&pa, ts, true, NULL);
    return ca_run(&c, &pa, de__shape_margin(ts), DE_SUB_SHAPE_NONE, out);
}

/* ---- Casts ---------------------------------------------------------------------------- */

bool de__qcast_init(de__qcast *c, const de__shape *shape, const de_transform *from, de_vec3 dir,
                    f32 max_dist) {
    if (shape == NULL || from == NULL || !castable(shape) || !de__d3_finite(from->position) ||
        !de__f32_finite(from->rotation.x) || !de__f32_finite(from->rotation.y) ||
        !de__f32_finite(from->rotation.z) || !de__f32_finite(from->rotation.w))
        return false;
    de__qray ray;
    if (!de__qray_init(&ray, from->position, dir, max_dist))
        return false;
    c->shape = shape;
    c->position = from->position;
    c->rotation = de__q_quat_normalize(from->rotation);
    c->delta = ray.delta;
    c->dir = ray.dir;
    c->start = de__shape_world_aabb(shape, c->position, c->rotation);
    const de_aabb end = de__shape_world_aabb(shape, de__d3_add(c->position, c->delta), c->rotation);
    c->swept.min = de__v3_min(c->start.min, end.min);
    c->swept.max = de__v3_max(c->start.max, end.max);
    return true;
}

bool de__query_cast_body(const de_physics_world *p, u32 body, const de__qcast *c, f64 max_t,
                         u32 flags, de_ray_hit *out) {
    de__qside tgt;
    tgt.shape = de__shape_get(p, p->bodies.shape[body]);
    if (tgt.shape == NULL)
        return false;
    tgt.pose.position = p->bodies.position[body];
    tgt.pose.rotation = p->bodies.rotation[body];
    tgt.sub = DE_SUB_SHAPE_NONE;
    /* Each convex part of the cast shape; ties to the lower cast child. */
    bool found = false;
    ca_hit best;
    memset(&best, 0, sizeof best);
    const de__qside whole = {c->shape, {c->position, c->rotation}, DE_SUB_SHAPE_NONE};
    const u32 parts = c->shape->kind == DE_SHAPE_COMPOUND ? c->shape->u.compound.child_count : 1u;
    for (u32 i = 0; i < parts; ++i) {
        const de__qside part =
            c->shape->kind == DE_SHAPE_COMPOUND ? de__q_child_side(&whole, i) : whole;
        ca_hit h;
        if (!cast_side(part.shape, part.pose, c->delta, max_t, flags, &tgt, &h))
            continue;
        if (!found || h.t < best.t) {
            found = true;
            best = h;
        }
    }
    if (!found)
        return false;
    out->entity = p->bodies.owner[body];
    out->body = de__body_handle(p, body);
    out->sub_shape = best.sub;
    if (best.inside) {
        out->point = c->position;
        out->normal = de__v3(-c->dir.x, -c->dir.y, -c->dir.z);
        out->fraction = 0.0f;
        return true;
    }
    out->point = best.point;
    out->normal = de__q_to_f32(best.normal);
    out->fraction = (f32)best.t;
    return true;
}

/* Every hit with t <= 1 (see raycast.c). */
#define Q_CAST_LIMIT 1.0000001f

typedef struct cast_ctx {
    const de_physics_world *p;
    const de__qcast *c;
    u32 flags;
    de_dvec3 center; /* the start box's center (world) */
    de_dvec3 half;   /* the start box's half extents */
    bool found;
    de_ray_hit best;
} cast_ctx;

static bool cast_cb(void *user, u32 body) {
    cast_ctx *x = (cast_ctx *)user;
    const f64 bound = x->found ? (f64)de__q_bound(x->best.fraction, Q_CAST_LIMIT) : 1.0;
    /* The start box swept along the cast meets the body's box: a ray from the start box's
     * center against the body's box grown by the start box's half extents. */
    const de_aabb *wb = &x->p->bodies.world_aabb[body];
    f64 te = 0.0;
    if (!de__q_slab(x->center, x->c->delta, de__d3_sub(de__d3_from(wb->min), x->half),
                    de__d3_add(de__d3_from(wb->max), x->half), bound + 1e-6, &te))
        return true;
    de_ray_hit h;
    if (!de__query_cast_body(x->p, body, x->c, de__mind(bound, 1.0), x->flags, &h))
        return true;
    if (!x->found || de__q_hit_less(&h, &x->best)) {
        x->found = true;
        x->best = h;
    }
    return true;
}

static bool shape_cast(const de_physics_world *p, de_shape shape, const de_transform *from,
                       de_vec3 dir, f32 max_dist, const de__qfilter *f, de_ray_hit *hit) {
    if (p == NULL || hit == NULL)
        return false;
    de__qcast c;
    if (!de__qcast_init(&c, de__shape_get(p, shape), from, dir, max_dist))
        return false;
    cast_ctx x;
    memset(&x, 0, sizeof x);
    x.p = p;
    x.c = &c;
    x.flags = f->flags;
    x.center = de__d3(((f64)c.start.min.x + c.start.max.x) * 0.5,
                      ((f64)c.start.min.y + c.start.max.y) * 0.5,
                      ((f64)c.start.min.z + c.start.max.z) * 0.5);
    x.half = de__d3(((f64)c.start.max.x - c.start.min.x) * 0.5,
                    ((f64)c.start.max.y - c.start.min.y) * 0.5,
                    ((f64)c.start.max.z - c.start.min.z) * 0.5);
    de__query_each_aabb(p, c.swept, f, cast_cb, &x);
    if (x.found)
        *hit = x.best;
    return x.found;
}

bool de_physics_shape_cast(const de_physics_world *p, de_shape shape, const de_transform *from,
                           de_vec3 dir, f32 max_dist, de_collision_filter filter, de_ray_hit *hit) {
    de__qfilter f;
    de__qfilter_plain(filter, &f);
    return shape_cast(p, shape, from, dir, max_dist, &f, hit);
}

bool de_physics_shape_cast_ex(const de_physics_world *p, de_shape shape, const de_transform *from,
                              de_vec3 dir, f32 max_dist, const de_physics_query_filter *filter,
                              de_ray_hit *hit) {
    if (p == NULL)
        return false;
    de__qfilter f;
    de__qfilter_init(p, filter, &f);
    return shape_cast(p, shape, from, dir, max_dist, &f, hit);
}
