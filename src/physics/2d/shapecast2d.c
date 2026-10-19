/*
 * shapecast2d.c: 2D GJK distance, shape casts by conservative advancement and the convex overlap
 * test of the 2D scene queries (DENG-258; see query2d_internal.h). de_physics2d_shape_cast.
 *
 * GJK. Box2D's b2Distance on the cores (points and radius): a simplex of up to 3 Minkowski
 * difference points w = wB - wA, barycentric solves for 2 and 3 points, support points with
 * ties to the lowest vertex index, at most DE__Q2_GJK_MAX_ITER iterations, stopped when a support
 * pair repeats or a new support point gets no closer to the origin (parallel features); the
 * simplex is always solved before the result is read. A 3-point simplex encloses the origin
 * (overlap). The normal (A toward B) of a 2-point simplex is the perpendicular of its edge, which
 * stays accurate when the cores almost touch; of a 1-point simplex, the direction between the
 * witness points.
 *
 * Casts. The cast shape A moves by t * delta, t in [0, 1]; the distance between the surfaces
 * (core distance minus the summed radii) of two convex shapes under a linear motion is a convex
 * function of t, so the Newton step t += separation / dot(delta, normal) never passes the first
 * contact (conservative advancement); it aims at half of DE__Q2_CAST_TOL so f32 rounding does not
 * carry it across. The cast stops when the separation is at most DE__Q2_CAST_TOL, reports a miss
 * when the motion does not approach (dot <= 0) or when the step leaves [0, 1], and after
 * DE__Q2_CAST_MAX_ITER steps reports the last (conservative) t.
 *
 * Determinism (SPEC §5.7): f32 GJK in the query frame, f64 for t and offsets, fixed caps, ties to
 * the lowest index. Nothing allocates.
 */
#include "physics/2d/query2d_internal.h"

/* ================================ Proxies =============================================== */

void de__q2proxy_shape(de__q2proxy *out, const de__shape2d *s, de_rot2d rot, de__d2 offset) {
    memset(out, 0, sizeof *out);
    de_vec2 pts[DE_SHAPE2D_MAX_POLYGON_VERTICES];
    u32 n = 0;
    switch (s->kind) {
    case DE_SHAPE2D_CIRCLE:
        pts[0] = s->u.circle.center;
        n = 1;
        out->radius = s->u.circle.radius;
        break;
    case DE_SHAPE2D_CAPSULE:
        pts[0] = s->u.capsule.p1;
        pts[1] = s->u.capsule.p2;
        n = 2;
        out->radius = s->u.capsule.radius;
        break;
    case DE_SHAPE2D_SEGMENT:
        pts[0] = s->u.segment.p1;
        pts[1] = s->u.segment.p2;
        n = 2;
        break;
    case DE_SHAPE2D_BOX:
    case DE_SHAPE2D_POLYGON:
        n = s->u.polygon.count;
        for (u32 i = 0; i < n; ++i)
            pts[i] = s->u.polygon.vertices[i];
        out->radius = s->u.polygon.radius;
        break;
    default:
        pts[0] = de__v2(0.0f, 0.0f);
        n = 1;
        break;
    }
    for (u32 i = 0; i < n; ++i) {
        const de__d2 w = de__d2_add(offset, de__rot2d_apply_d(rot, de__d2_from(pts[i])));
        out->v[i] = de__v2((f32)w.x, (f32)w.y);
    }
    out->count = n;
}

void de__q2proxy_segment(de__q2proxy *out, de_vec2 a, de_vec2 b) {
    memset(out, 0, sizeof *out);
    out->v[0] = a;
    out->v[1] = b;
    out->count = 2;
    out->radius = 0.0f;
}

/* ================================ GJK =================================================== */

typedef struct sv {
    de_vec2 wa, wb, w; /* support points of A and B, w = wb - wa */
    f32 a;             /* barycentric coordinate */
    u32 ia, ib;
} sv;

static u32 support(const de__q2proxy *p, de_vec2 d) {
    u32 best = 0;
    f32 bd = de__v2_dot(p->v[0], d);
    for (u32 i = 1; i < p->count; ++i) {
        const f32 x = de__v2_dot(p->v[i], d);
        if (x > bd) {
            bd = x;
            best = i;
        }
    }
    return best;
}

static void solve2(sv *v, u32 *count) {
    const de_vec2 w1 = v[0].w, w2 = v[1].w;
    const de_vec2 e12 = de__v2_sub(w2, w1);
    const f32 d12_2 = -de__v2_dot(w1, e12);
    if (d12_2 <= 0.0f) {
        v[0].a = 1.0f;
        *count = 1;
        return;
    }
    const f32 d12_1 = de__v2_dot(w2, e12);
    if (d12_1 <= 0.0f) {
        v[1].a = 1.0f;
        v[0] = v[1];
        *count = 1;
        return;
    }
    const f32 inv = 1.0f / (d12_1 + d12_2);
    v[0].a = d12_1 * inv;
    v[1].a = d12_2 * inv;
    *count = 2;
}

static void solve3(sv *v, u32 *count) {
    const de_vec2 w1 = v[0].w, w2 = v[1].w, w3 = v[2].w;
    const de_vec2 e12 = de__v2_sub(w2, w1);
    const f32 d12_1 = de__v2_dot(w2, e12);
    const f32 d12_2 = -de__v2_dot(w1, e12);
    const de_vec2 e13 = de__v2_sub(w3, w1);
    const f32 d13_1 = de__v2_dot(w3, e13);
    const f32 d13_2 = -de__v2_dot(w1, e13);
    const de_vec2 e23 = de__v2_sub(w3, w2);
    const f32 d23_1 = de__v2_dot(w3, e23);
    const f32 d23_2 = -de__v2_dot(w2, e23);
    const f32 n123 = de__v2_cross(e12, e13);
    const f32 d123_1 = n123 * de__v2_cross(w2, w3);
    const f32 d123_2 = n123 * de__v2_cross(w3, w1);
    const f32 d123_3 = n123 * de__v2_cross(w1, w2);

    if (d12_2 <= 0.0f && d13_2 <= 0.0f) { /* w1 region */
        v[0].a = 1.0f;
        *count = 1;
        return;
    }
    if (d12_1 > 0.0f && d12_2 > 0.0f && d123_3 <= 0.0f) { /* e12 */
        const f32 inv = 1.0f / (d12_1 + d12_2);
        v[0].a = d12_1 * inv;
        v[1].a = d12_2 * inv;
        *count = 2;
        return;
    }
    if (d13_1 > 0.0f && d13_2 > 0.0f && d123_2 <= 0.0f) { /* e13 */
        const f32 inv = 1.0f / (d13_1 + d13_2);
        v[0].a = d13_1 * inv;
        v[2].a = d13_2 * inv;
        v[1] = v[2];
        *count = 2;
        return;
    }
    if (d12_1 <= 0.0f && d23_2 <= 0.0f) { /* w2 */
        v[1].a = 1.0f;
        v[0] = v[1];
        *count = 1;
        return;
    }
    if (d13_1 <= 0.0f && d23_1 <= 0.0f) { /* w3 */
        v[2].a = 1.0f;
        v[0] = v[2];
        *count = 1;
        return;
    }
    if (d23_1 > 0.0f && d23_2 > 0.0f && d123_1 <= 0.0f) { /* e23 */
        const f32 inv = 1.0f / (d23_1 + d23_2);
        v[1].a = d23_1 * inv;
        v[2].a = d23_2 * inv;
        v[0] = v[2];
        *count = 2;
        return;
    }
    /* Inside the triangle. */
    const f32 inv = 1.0f / (d123_1 + d123_2 + d123_3);
    v[0].a = d123_1 * inv;
    v[1].a = d123_2 * inv;
    v[2].a = d123_3 * inv;
    *count = 3;
}

static de_vec2 search_dir(const sv *v, u32 count) {
    if (count == 1u)
        return de__v2_scale(v[0].w, -1.0f);
    const de_vec2 e12 = de__v2_sub(v[1].w, v[0].w);
    const f32 sgn = de__v2_cross(e12, de__v2_scale(v[0].w, -1.0f));
    /* The origin is left of e12: search left; else right. */
    return sgn > 0.0f ? de__v2_left_perp(e12) : de__v2_right_perp(e12);
}

void de__q2_gjk(const de__q2proxy *a, const de__q2proxy *b, de__q2dist *out) {
    sv v[3];
    memset(v, 0, sizeof v);
    u32 count = 1;
    v[0].ia = 0;
    v[0].ib = 0;
    v[0].wa = a->v[0];
    v[0].wb = b->v[0];
    v[0].w = de__v2_sub(v[0].wb, v[0].wa);
    v[0].a = 1.0f;

    u32 save_a[3], save_b[3];
    for (u32 iter = 0;; ++iter) {
        const u32 save_count = count;
        for (u32 i = 0; i < save_count; ++i) {
            save_a[i] = v[i].ia;
            save_b[i] = v[i].ib;
        }
        if (count == 2u)
            solve2(v, &count);
        else if (count == 3u)
            solve3(v, &count);
        /* Every exit below leaves a solved simplex. */
        if (count == 3u || iter == DE__Q2_GJK_MAX_ITER)
            break;
        const de_vec2 d = search_dir(v, count);
        if (de__v2_dot(d, d) < FLT_EPSILON * FLT_EPSILON)
            break; /* the origin is on the simplex */
        sv *n = &v[count];
        n->ia = support(a, de__v2_scale(d, -1.0f));
        n->wa = a->v[n->ia];
        n->ib = support(b, d);
        n->wb = b->v[n->ib];
        n->w = de__v2_sub(n->wb, n->wa);
        /* No progress toward the origin (parallel features, ties): the simplex is final. */
        if (!(de__v2_dot(n->w, d) > de__v2_dot(v[0].w, d)))
            break;
        bool dup = false;
        for (u32 i = 0; i < save_count; ++i) {
            if (n->ia == save_a[i] && n->ib == save_b[i]) {
                dup = true;
                break;
            }
        }
        if (dup)
            break;
        ++count;
    }

    memset(out, 0, sizeof *out);
    if (count == 3u) {
        const de_vec2 pa =
            de__v2_add(de__v2_add(de__v2_scale(v[0].wa, v[0].a), de__v2_scale(v[1].wa, v[1].a)),
                       de__v2_scale(v[2].wa, v[2].a));
        out->pa = pa;
        out->pb = pa;
        out->distance = 0.0f;
        out->overlap = true;
        return;
    }
    if (count == 1u) {
        out->pa = v[0].wa;
        out->pb = v[0].wb;
    } else {
        out->pa = de__v2_add(de__v2_scale(v[0].wa, v[0].a), de__v2_scale(v[1].wa, v[1].a));
        out->pb = de__v2_add(de__v2_scale(v[0].wb, v[0].a), de__v2_scale(v[1].wb, v[1].a));
    }
    const de_vec2 dv = de__v2_sub(out->pb, out->pa);
    out->distance = de_dsqrt(de__v2_dot(dv, dv));
    if (!(out->distance > 0.0f)) {
        out->distance = 0.0f;
        out->overlap = true;
        return;
    }
    if (count == 2u) {
        /* The edge's perpendicular, toward the side of the origin's closest point (dot with any
         * point of the edge is the distance). */
        de_vec2 n = de__v2_normalize_d(de__d2_from(de__v2_right_perp(de__v2_sub(v[1].w, v[0].w))));
        if (de__v2_dot(n, v[0].w) < 0.0f)
            n = de__v2_scale(n, -1.0f);
        if (n.x != 0.0f || n.y != 0.0f) {
            out->normal = n;
            return;
        }
    }
    out->normal = de__v2_normalize_d(de__d2_from(dv));
}

bool de__q2_in_front(const de__q2proxy *s, de_vec2 a, de_vec2 b, de_vec2 n) {
    /* The deepest point of the whole shape along -n, against the segment's line. */
    f32 deepest = FLT_MAX;
    for (u32 i = 0; i < s->count; ++i)
        deepest = de__minf(deepest, de__v2_dot(de__v2_sub(s->v[i], a), n));
    if (deepest - s->radius >= -DE__Q2_ONE_WAY_SLOP)
        return true;
    /* Deeper than that: in front only when the core is clear of the segment, on its front side
     * (a shape resting on the segment's end with its rounding hanging below the line). */
    de__q2proxy seg, core;
    de__q2proxy_segment(&seg, a, b);
    core = *s;
    core.radius = 0.0f;
    de__q2dist g;
    de__q2_gjk(&core, &seg, &g);
    return !g.overlap && de__v2_dot(de__v2_sub(g.pa, g.pb), n) > 0.0f;
}

bool de__q2_proxy_overlap(const de__q2proxy *a, const de__q2proxy *b) {
    de__q2dist g;
    de__q2_gjk(a, b, &g);
    return g.overlap || (f64)g.distance <= (f64)a->radius + (f64)b->radius;
}

/* ================================ Cast of two proxies =================================== */

typedef struct cast_res {
    f64 t;
    de_vec2 normal; /* A toward B (unit) */
    de_vec2 point;  /* on B's surface, query frame */
    bool inside;
} cast_res;

/* A translated by `d` (f32). */
static void proxy_moved(de__q2proxy *out, const de__q2proxy *a, de_vec2 d) {
    *out = *a;
    for (u32 i = 0; i < a->count; ++i)
        out->v[i] = de__v2_add(a->v[i], d);
}

static bool cast_proxies(const de__q2proxy *a, de__d2 delta, const de__q2proxy *b, cast_res *r) {
    const f64 total_r = (f64)a->radius + (f64)b->radius;
    f64 t = 0.0;
    de_vec2 last_n = de__v2(0.0f, 0.0f);
    de_vec2 last_pb = b->v[0];
    de__q2proxy at = *a;
    for (u32 it = 0; it < DE__Q2_CAST_MAX_ITER; ++it) {
        de__q2dist g;
        de__q2_gjk(&at, b, &g);
        const f64 sep = (f64)g.distance - total_r;
        if (g.overlap || sep <= 0.0) {
            if (t == 0.0) {
                r->t = 0.0;
                r->inside = true;
                r->normal = last_n;
                r->point = last_pb;
                return true;
            }
            break; /* reached (rounding): report the last conservative t */
        }
        /* Rounding can carry a step a hair past the contact, where the closest features flip:
         * near contact, keep the previous normal when the new one turns around. */
        if (!(it > 0u && sep <= DE__Q2_CAST_TOL && de__v2_dot(g.normal, last_n) < 0.0f)) {
            last_n = g.normal;
            last_pb = de__v2_sub(g.pb, de__v2_scale(g.normal, b->radius));
        }
        const f64 vn = delta.x * (f64)last_n.x + delta.y * (f64)last_n.y;
        if (sep <= DE__Q2_CAST_TOL) {
            if (t > 0.0 || vn > 0.0)
                break;
            return false; /* touching at the start and not approaching */
        }
        if (!(vn > 0.0))
            return false;
        /* Aim at half the tolerance: the step lands inside it, short of the contact. */
        t += (sep - 0.5 * DE__Q2_CAST_TOL) / vn;
        if (t > 1.0)
            return false;
        proxy_moved(&at, a, de__v2((f32)(delta.x * t), (f32)(delta.y * t)));
    }
    r->t = t;
    r->inside = false;
    r->normal = last_n;
    r->point = last_pb;
    return true;
}

/* ================================ Cast against a body =================================== */

bool de__q2cast_init(de__q2cast *c, const de__shape2d *s, de__d2 position, de_rot2d rot,
                     de__d2 delta) {
    if (s == NULL || !de__shape2d_kind_is_convex(s->kind) || !de__f64_finite(position.x) ||
        !de__f64_finite(position.y) || !de__rot2d_finite(rot) || !de__f64_finite(delta.x) ||
        !de__f64_finite(delta.y))
        return false;
    memset(c, 0, sizeof *c);
    c->shape = s;
    c->origin = position;
    c->rotation = de__rot2d_normalize(rot);
    c->delta = delta;
    c->delta_f = de__v2((f32)delta.x, (f32)delta.y);
    de__q2proxy_shape(&c->proxy, s, c->rotation, de__d2_make(0.0, 0.0));
    const de_aabb2d b0 = de__shape2d_world_aabb(s, position, c->rotation);
    const de_aabb2d b1 = de__shape2d_world_aabb(s, de__d2_add(position, delta), c->rotation);
    c->swept = de__aabb2d_union(&b0, &b1);
    return true;
}

typedef struct chain_cast_job {
    const de__chain2d *chain;
    const de__q2cast *c;
    de_rot2d rot;
    de__d2 off; /* chain body position - cast origin */
    bool found;
    u32 seg;
    cast_res best;
} chain_cast_job;

static bool chain_cast_cb(void *user, u32 segment) {
    chain_cast_job *j = (chain_cast_job *)user;
    const de__chain_segment *s = &j->chain->segments[segment];
    const de__d2 a = de__d2_add(j->off, de__rot2d_apply_d(j->rot, de__d2_from(s->p1)));
    const de__d2 b = de__d2_add(j->off, de__rot2d_apply_d(j->rot, de__d2_from(s->p2)));
    const de_vec2 n = de__rot2d_apply(j->rot, s->normal);
    const de__q2cast *c = j->c;
    /* One-sided: only a motion against the normal, from in front of the segment. */
    if (!(de__v2_dot(c->delta_f, n) < 0.0f))
        return true;
    const de_vec2 af = de__v2((f32)a.x, (f32)a.y);
    const de_vec2 bf = de__v2((f32)b.x, (f32)b.y);
    if (!de__q2_in_front(&c->proxy, af, bf, n))
        return true;
    de__q2proxy seg;
    de__q2proxy_segment(&seg, af, bf);
    cast_res r;
    memset(&r, 0, sizeof r);
    if (!cast_proxies(&c->proxy, c->delta, &seg, &r))
        return true;
    if (!j->found || r.t < j->best.t || (r.t == j->best.t && segment < j->seg)) {
        j->found = true;
        j->seg = segment;
        j->best = r;
    }
    return true;
}

bool de__q2_cast_body(const de_physics2d_world *p, u32 b, const de__q2cast *c, de__q2hit *out) {
    const de__shape2d *s = de__shape2d_get(p, p->bodies.shape[b]);
    if (s == NULL)
        return false;
    const de__d2 pos = p->bodies.position[b];
    const de_rot2d rot = p->bodies.rotation[b];
    const de__d2 off = de__d2_sub(pos, c->origin);
    cast_res r;
    memset(&r, 0, sizeof r);
    u32 sub = DE_SUB_SHAPE_NONE;
    if (s->kind == DE_SHAPE2D_CHAIN) {
        chain_cast_job j;
        memset(&j, 0, sizeof j);
        j.chain = &s->u.chain;
        j.c = c;
        j.rot = rot;
        j.off = off;
        const de_aabb2d local = de__q2_box_in_frame(c->swept, pos, rot, 1e-4f);
        (void)de__chain2d_query(&s->u.chain, local, chain_cast_cb, &j);
        if (!j.found)
            return false;
        r = j.best;
        sub = j.seg;
    } else {
        de__q2proxy t;
        de__q2proxy_shape(&t, s, rot, off);
        if (!cast_proxies(&c->proxy, c->delta, &t, &r))
            return false;
    }
    out->t = r.t;
    out->body = b;
    out->sub = sub;
    out->inside = r.inside;
    if (r.inside) {
        out->point = c->origin;
        const de_vec2 d = de__v2_normalize_d(c->delta);
        out->normal = de__v2_scale(d, -1.0f);
    } else {
        out->point = de__d2_add(c->origin, de__d2_from(r.point));
        out->normal = de__v2_scale(r.normal, -1.0f);
    }
    return true;
}

/* ================================ Public cast =========================================== */

typedef struct cast_job {
    const de_physics2d_world *p;
    const de__q2cast *c;
    bool found;
    de__q2hit best;
} cast_job;

static bool cast_cb(void *user, u32 body) {
    cast_job *j = (cast_job *)user;
    de__q2hit h;
    if (!de__q2_cast_body(j->p, body, j->c, &h))
        return true;
    if (!j->found || de__q2hit_before(&h, &j->best)) {
        j->found = true;
        j->best = h;
    }
    return true;
}

bool de_physics2d_shape_cast(const de_physics2d_world *p, de_shape shape,
                             const de_transform2d *from, de_vec2 dir, f32 max_dist,
                             const de_physics_query_filter *filter, de_ray_hit2d *hit) {
    if (p == NULL || from == NULL || hit == NULL)
        return false;
    de__q2ray r;
    if (!de__q2ray_init(&r, de__d2_make(from->x, from->y), dir, max_dist))
        return false;
    de__q2cast c;
    if (!de__q2cast_init(&c, de__shape2d_get(p, shape), de__d2_make(from->x, from->y),
                         from->rotation, de__d2_scale(r.dir, r.length)))
        return false;
    de__q2filter f;
    de__q2filter_init(p, filter, &f);
    cast_job j;
    memset(&j, 0, sizeof j);
    j.p = p;
    j.c = &c;
    (void)de__q2_each_aabb(p, c.swept, &f, cast_cb, &j);
    if (!j.found)
        return false;
    *hit = de__q2hit_public(p, &j.best);
    return true;
}
