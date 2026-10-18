/*
 * raycast2d.c: rays of the 2D scene queries (DENG-258; see query2d_internal.h):
 * de_physics2d_raycast, de_physics2d_raycast_ex and de_physics2d_raycast_all.
 *
 * Each ray is tested in the body's local frame, in f64 (origin offset in f64, then rotated), with
 * closed-form tests:
 * - circle: the quadratic; capsule: its two end circles and its two side lines;
 * - polygon: radius 0 by Cyrus-Beck clipping against the edge planes; a rounded polygon is the
 *   union of its core and the capsules of its edges, and a ray from outside enters the union
 *   first through one of the edge capsules, so its hit is the nearest edge-capsule hit;
 * - segment: two-sided line crossing; chain: each segment the ray's box walk finds
 *   (de__chain2d_raycast), front side only.
 * A ray that starts inside a solid hits at distance 0 (inside). The world point of a hit is
 * origin + s * dir in f64 (not rotated back), so it lies on the ray exactly.
 *
 * Order: (public f32 fraction, body index, segment) for both functions. The closest walk clips
 * its bound to the best hit so far, rounded up plus one f32 step, so a body whose hit ties the
 * best one is still visited and the tie goes to the lower index. raycast_all keeps the caller's
 * array sorted by insertion (no scratch memory).
 */
#include "physics/2d/query2d_internal.h"

/* ================================ Local-frame tests ===================================== */

typedef struct lray {
    de__d2 o; /* origin, local */
    de__d2 d; /* unit dir, local */
    f64 len;  /* max distance */
} lray;

typedef struct lhit {
    f64 s;       /* distance along the ray */
    de__d2 n;    /* local normal (unit) */
    bool inside; /* started inside a solid */
} lhit;

static f64 len2(de__d2 v) {
    return de__d2_dot(v, v);
}

/* Distance from q to segment a..b, squared. */
static f64 seg_dist2(de__d2 q, de__d2 a, de__d2 b) {
    const de__d2 e = de__d2_sub(b, a);
    const f64 ee = de__d2_dot(e, e);
    f64 t = ee > 0.0 ? de__d2_dot(de__d2_sub(q, a), e) / ee : 0.0;
    if (t < 0.0)
        t = 0.0;
    if (t > 1.0)
        t = 1.0;
    return len2(de__d2_sub(q, de__d2_add(a, de__d2_scale(e, t))));
}

/* Ray (origin outside) against the circle c, r. */
static bool ray_circle(const lray *r, de__d2 c, f64 rad, lhit *out) {
    const de__d2 m = de__d2_sub(r->o, c);
    const f64 cc = de__d2_dot(m, m) - rad * rad;
    const f64 b = de__d2_dot(m, r->d);
    if (b > 0.0)
        return false; /* outside and pointing away */
    const f64 disc = b * b - cc;
    if (disc < 0.0)
        return false;
    f64 s = -b - de_dsqrt64(disc);
    if (s < 0.0)
        s = 0.0;
    if (s > r->len)
        return false;
    const de__d2 q = de__d2_add(m, de__d2_scale(r->d, s));
    const f64 ql = de_dsqrt64(len2(q));
    out->s = s;
    out->n = ql > 0.0 ? de__d2_scale(q, 1.0 / ql) : de__d2_scale(r->d, -1.0);
    out->inside = false;
    return true;
}

/* Ray (origin outside) against the capsule p1..p2 swept by rad. */
static bool ray_capsule(const lray *r, de__d2 p1, de__d2 p2, f64 rad, lhit *out) {
    const de__d2 e = de__d2_sub(p2, p1);
    const f64 el = de_dsqrt64(len2(e));
    if (!(el > 0.0))
        return ray_circle(r, p1, rad, out);
    bool found = false;
    lhit best;
    memset(&best, 0, sizeof best);
    lhit h;
    memset(&h, 0, sizeof h);
    if (ray_circle(r, p1, rad, &h)) {
        best = h;
        found = true;
    }
    if (ray_circle(r, p2, rad, &h) && (!found || h.s < best.s)) {
        best = h;
        found = true;
    }
    const de__d2 u = de__d2_scale(e, 1.0 / el);
    const de__d2 nrm = de__d2_make(u.y, -u.x);
    for (u32 side = 0; side < 2u; ++side) {
        const de__d2 sn = side == 0u ? nrm : de__d2_scale(nrm, -1.0);
        const f64 den = de__d2_dot(r->d, sn);
        if (!(den < 0.0))
            continue;
        const f64 d0 = de__d2_dot(de__d2_sub(r->o, p1), sn) - rad;
        if (d0 < 0.0)
            continue; /* the origin is past this side's line: it cannot enter through it */
        const f64 s = d0 / -den;
        if (s > r->len)
            continue;
        const de__d2 q = de__d2_sub(de__d2_add(r->o, de__d2_scale(r->d, s)), p1);
        const f64 along = de__d2_dot(q, u);
        if (along < 0.0 || along > el)
            continue;
        if (!found || s < best.s) {
            best.s = s;
            best.n = sn;
            best.inside = false;
            found = true;
        }
    }
    if (found)
        *out = best;
    return found;
}

/* Ray against a convex polygon (CCW, outward normals) of radius 0: Cyrus-Beck. */
static bool ray_polygon_core(const lray *r, const de__polygon2d *poly, lhit *out) {
    f64 lower = 0.0, upper = r->len;
    u32 index = DE__Q2_NO_BODY;
    for (u32 i = 0; i < poly->count; ++i) {
        const de__d2 n = de__d2_from(poly->normals[i]);
        const de__d2 v = de__d2_from(poly->vertices[i]);
        /* p = o + s d is inside edge i when dot(n, p - v) <= 0. */
        const f64 num = de__d2_dot(n, de__d2_sub(v, r->o));
        const f64 den = de__d2_dot(n, r->d);
        if (den == 0.0) {
            if (num < 0.0)
                return false;
        } else if (den < 0.0 && num < lower * den) {
            lower = num / den;
            index = i;
        } else if (den > 0.0 && num < upper * den) {
            upper = num / den;
        }
        if (upper < lower)
            return false;
    }
    if (index == DE__Q2_NO_BODY) {
        out->s = 0.0;
        out->n = de__d2_scale(r->d, -1.0);
        out->inside = true;
        return true;
    }
    out->s = lower;
    out->n = de__d2_from(poly->normals[index]);
    out->inside = false;
    return true;
}

static bool point_in_polygon(de__d2 q, const de__polygon2d *poly, f64 rad) {
    bool in_core = true;
    for (u32 i = 0; i < poly->count; ++i) {
        const de__d2 n = de__d2_from(poly->normals[i]);
        if (de__d2_dot(n, de__d2_sub(q, de__d2_from(poly->vertices[i]))) > 0.0) {
            in_core = false;
            break;
        }
    }
    if (in_core)
        return true;
    if (!(rad > 0.0))
        return false;
    for (u32 i = 0; i < poly->count; ++i) {
        const de__d2 a = de__d2_from(poly->vertices[i]);
        const de__d2 b = de__d2_from(poly->vertices[(i + 1u) % poly->count]);
        if (seg_dist2(q, a, b) <= rad * rad)
            return true;
    }
    return false;
}

static bool ray_polygon(const lray *r, const de__polygon2d *poly, lhit *out) {
    const f64 rad = (f64)poly->radius;
    if (!(rad > 0.0))
        return ray_polygon_core(r, poly, out);
    if (point_in_polygon(r->o, poly, rad)) {
        out->s = 0.0;
        out->n = de__d2_scale(r->d, -1.0);
        out->inside = true;
        return true;
    }
    bool found = false;
    lhit best;
    memset(&best, 0, sizeof best);
    for (u32 i = 0; i < poly->count; ++i) {
        lhit h;
        memset(&h, 0, sizeof h);
        const de__d2 a = de__d2_from(poly->vertices[i]);
        const de__d2 b = de__d2_from(poly->vertices[(i + 1u) % poly->count]);
        if (ray_capsule(r, a, b, rad, &h) && (!found || h.s < best.s)) {
            best = h;
            found = true;
        }
    }
    if (found)
        *out = best;
    return found;
}

/* Ray against the line segment a..b (right normal nrm). one_sided: front (nrm side) only. */
static bool ray_line(const lray *r, de__d2 a, de__d2 b, de__d2 nrm, bool one_sided, lhit *out) {
    const f64 den = de__d2_dot(r->d, nrm);
    if (den == 0.0 || (one_sided && den > 0.0))
        return false;
    const f64 s = de__d2_dot(de__d2_sub(a, r->o), nrm) / den;
    if (s < 0.0 || s > r->len)
        return false;
    const de__d2 e = de__d2_sub(b, a);
    const f64 ee = de__d2_dot(e, e);
    const de__d2 q = de__d2_sub(de__d2_add(r->o, de__d2_scale(r->d, s)), a);
    const f64 along = de__d2_dot(q, e);
    if (along < 0.0 || along > ee)
        return false;
    out->s = s;
    out->n = den < 0.0 ? nrm : de__d2_scale(nrm, -1.0);
    out->inside = false;
    return true;
}

/* ================================ Chains ================================================ */

typedef struct chain_ray_job {
    const de__chain2d *chain;
    const lray *r;
    bool found;
    u32 seg;
    lhit best;
} chain_ray_job;

static f32 chain_ray_cb(void *user, const de__tree2d_ray *ray, u32 segment, f32 max_fraction) {
    chain_ray_job *j = (chain_ray_job *)user;
    (void)ray;
    const de__chain_segment *s = &j->chain->segments[segment];
    lhit h;
    memset(&h, 0, sizeof h);
    if (!ray_line(j->r, de__d2_from(s->p1), de__d2_from(s->p2), de__d2_from(s->normal), true, &h))
        return max_fraction;
    if (!j->found || h.s < j->best.s || (h.s == j->best.s && segment < j->seg)) {
        j->found = true;
        j->seg = segment;
        j->best = h;
    }
    /* Clip, rounded up and one step more so a segment that ties is still visited. */
    const f32 f = de__f32_next_up(de__f32_round_up(j->best.s / j->r->len));
    return f < max_fraction ? f : max_fraction;
}

/* ================================ Per body ============================================== */

bool de__q2ray_init(de__q2ray *r, de__d2 origin, de_vec2 dir, f32 max_dist) {
    if (!de__f64_finite(origin.x) || !de__f64_finite(origin.y) || !de__v2_finite(dir) ||
        !de__f32_finite(max_dist) || !(max_dist > 0.0f))
        return false;
    const f64 dx = dir.x, dy = dir.y;
    const f64 l2 = dx * dx + dy * dy;
    if (!(l2 > 0.0))
        return false;
    const f64 inv = 1.0 / de_dsqrt64(l2);
    r->origin = origin;
    r->dir = de__d2_make(dx * inv, dy * inv);
    r->length = (f64)max_dist;
    return true;
}

bool de__q2_ray_body(const de_physics2d_world *p, u32 b, const de__q2ray *r, f64 max_t,
                     de__q2hit *out) {
    const de__shape2d *s = de__shape2d_get(p, p->bodies.shape[b]);
    if (s == NULL)
        return false;
    const de__d2 pos = p->bodies.position[b];
    const de_rot2d rot = p->bodies.rotation[b];
    const f64 c = rot.c, sn = rot.s;
    const de__d2 rel = de__d2_sub(r->origin, pos);
    lray lr;
    lr.o = de__d2_make(c * rel.x + sn * rel.y, -sn * rel.x + c * rel.y);
    lr.d = de__d2_make(c * r->dir.x + sn * r->dir.y, -sn * r->dir.x + c * r->dir.y);
    lr.len = r->length * (max_t < 1.0 ? max_t : 1.0);
    lhit h;
    memset(&h, 0, sizeof h);
    u32 sub = DE_SUB_SHAPE_NONE;
    bool hit = false;
    switch (s->kind) {
    case DE_SHAPE2D_CIRCLE: {
        const de__d2 cc = de__d2_from(s->u.circle.center);
        const f64 rad = (f64)s->u.circle.radius;
        if (len2(de__d2_sub(lr.o, cc)) <= rad * rad) {
            h.s = 0.0;
            h.inside = true;
            hit = true;
        } else {
            hit = ray_circle(&lr, cc, rad, &h);
        }
        break;
    }
    case DE_SHAPE2D_CAPSULE: {
        const de__d2 p1 = de__d2_from(s->u.capsule.p1), p2 = de__d2_from(s->u.capsule.p2);
        const f64 rad = (f64)s->u.capsule.radius;
        if (seg_dist2(lr.o, p1, p2) <= rad * rad) {
            h.s = 0.0;
            h.inside = true;
            hit = true;
        } else {
            hit = ray_capsule(&lr, p1, p2, rad, &h);
        }
        break;
    }
    case DE_SHAPE2D_BOX:
    case DE_SHAPE2D_POLYGON:
        hit = ray_polygon(&lr, &s->u.polygon, &h);
        break;
    case DE_SHAPE2D_SEGMENT:
        hit = ray_line(&lr, de__d2_from(s->u.segment.p1), de__d2_from(s->u.segment.p2),
                       de__d2_from(s->u.segment.normal), false, &h);
        break;
    case DE_SHAPE2D_CHAIN: {
        chain_ray_job j;
        memset(&j, 0, sizeof j);
        j.chain = &s->u.chain;
        j.r = &lr;
        de__tree2d_ray tr;
        tr.origin = lr.o;
        tr.delta = de__v2((f32)(lr.d.x * lr.len), (f32)(lr.d.y * lr.len));
        tr.max_fraction = 1.0f;
        (void)de__chain2d_raycast(&s->u.chain, &tr, chain_ray_cb, &j);
        if (j.found) {
            h = j.best;
            sub = j.seg;
            hit = true;
        }
        break;
    }
    default:
        break;
    }
    if (!hit)
        return false;
    out->t = h.s / r->length;
    out->body = b;
    out->sub = sub;
    out->inside = h.inside;
    if (h.inside) {
        out->t = 0.0;
        out->point = r->origin;
        out->normal = de__v2((f32)-r->dir.x, (f32)-r->dir.y);
    } else {
        out->point = de__d2_add(r->origin, de__d2_scale(r->dir, h.s));
        out->normal = de__v2_normalize_d(de__rot2d_apply_d(rot, h.n));
    }
    return true;
}

/* ================================ Walks ================================================= */

/* Ray order: the public fraction (f32), then body index, then segment. raycast and raycast_all
 * use the same key, so the closest hit is the first entry of raycast_all. */
static bool ray_before(const de__q2hit *a, const de__q2hit *b) {
    const f32 fa = (f32)a->t, fb = (f32)b->t;
    if (fa != fb)
        return fa < fb;
    if (a->body != b->body)
        return a->body < b->body;
    return a->sub < b->sub;
}

/* The walk bound after a hit at t: rounded up and one f32 step more, so every body whose hit
 * rounds to the same fraction is still visited (ties go to the lower index). */
static f32 bound_after(f64 t) {
    return de__f32_next_up(de__f32_round_up(t));
}

typedef struct ray_ctx {
    const de_physics2d_world *p;
    const de__q2filter *f;
    const de__q2ray *r;
    /* closest */
    bool found;
    de__q2hit best;
    /* all */
    de_ray_hit2d *out;
    u32 max_hits;
    u32 count;
} ray_ctx;

static f32 closest_cb(void *user, const de__tree2d_ray *ray, u32 body, f32 max_fraction) {
    ray_ctx *c = (ray_ctx *)user;
    (void)ray;
    if (!de__q2_candidate(c->p, body, c->f))
        return max_fraction;
    de__q2hit h;
    if (de__q2_ray_body(c->p, body, c->r, 1.0, &h) && (!c->found || ray_before(&h, &c->best))) {
        c->found = true;
        c->best = h;
    }
    if (!c->found)
        return max_fraction;
    const f32 f = bound_after(c->best.t);
    return f < max_fraction ? f : max_fraction;
}

/* The order of two public hits (one per body: the body decides ties). */
static bool pub_before(const de_ray_hit2d *a, const de_ray_hit2d *b) {
    if (a->fraction != b->fraction)
        return a->fraction < b->fraction;
    return de__handle_index(a->body.id) < de__handle_index(b->body.id);
}

/* Inserts h into the caller's array, sorted; drops the last when full. */
static void all_insert(ray_ctx *c, const de_ray_hit2d *h) {
    u32 n = c->count;
    if (n == c->max_hits) {
        if (!pub_before(h, &c->out[n - 1u]))
            return;
        n -= 1u;
    }
    u32 k = n;
    while (k > 0u && pub_before(h, &c->out[k - 1u])) {
        c->out[k] = c->out[k - 1u];
        --k;
    }
    c->out[k] = *h;
    c->count = n + 1u;
}

static f32 all_cb(void *user, const de__tree2d_ray *ray, u32 body, f32 max_fraction) {
    ray_ctx *c = (ray_ctx *)user;
    (void)ray;
    de__q2hit h;
    if (de__q2_candidate(c->p, body, c->f) && de__q2_ray_body(c->p, body, c->r, 1.0, &h)) {
        const de_ray_hit2d pub = de__q2hit_public(c->p, &h);
        all_insert(c, &pub);
    }
    return max_fraction;
}

typedef f32 (*walk_fn)(void *user, const de__tree2d_ray *ray, u32 body, f32 max_fraction);

static void walk(const de_physics2d_world *p, const de__q2ray *r, const de__q2filter *f, walk_fn fn,
                 ray_ctx *c) {
    de__tree2d_ray tr;
    tr.origin = r->origin;
    tr.delta = de__v2((f32)(r->dir.x * r->length), (f32)(r->dir.y * r->length));
    tr.max_fraction = 1.0f;
    if (de__bp2d_get(p) != NULL) {
        (void)de__bp2d_raycast(p, &tr, &f->filter, fn, c);
        return;
    }
    /* No broadphase yet: every body whose box the segment enters (ascending index). */
    f32 bound = 1.0f;
    for (u32 i = 0; i < p->bodies.high_water; ++i) {
        if (!de__body2d_alive(p, i) || !DE_HANDLE_VALID(p->bodies.shape[i]))
            continue;
        f64 te;
        if (!de__ray2d_aabb(&tr, bound, &p->bodies.world_aabb[i], &te))
            continue;
        bound = fn(c, &tr, i, bound);
    }
}

/* The closest-hit core shared by the public functions and the character controller. */
static bool raycast_closest(const de_physics2d_world *p, const de__q2ray *r, const de__q2filter *f,
                            de__q2hit *out) {
    ray_ctx c;
    memset(&c, 0, sizeof c);
    c.p = p;
    c.f = f;
    c.r = r;
    walk(p, r, f, closest_cb, &c);
    if (c.found)
        *out = c.best;
    return c.found;
}

bool de_physics2d_raycast_ex(const de_physics2d_world *p, de_dpoint2d origin, de_vec2 dir,
                             f32 max_dist, const de_physics_query_filter *filter,
                             de_ray_hit2d *hit) {
    if (p == NULL || hit == NULL)
        return false;
    de__q2ray r;
    if (!de__q2ray_init(&r, de__d2_make(origin.x, origin.y), dir, max_dist))
        return false;
    de__q2filter f;
    de__q2filter_init(p, filter, &f);
    de__q2hit h;
    if (!raycast_closest(p, &r, &f, &h))
        return false;
    *hit = de__q2hit_public(p, &h);
    return true;
}

bool de_physics2d_raycast(const de_physics2d_world *p, de_vec2 origin, de_vec2 dir, f32 max_dist,
                          de_collision_filter filter, de_entity *hit_entity, de_vec2 *point,
                          de_vec2 *normal) {
    de_physics_query_filter qf;
    memset(&qf, 0, sizeof qf);
    qf.filter = filter;
    de_dpoint2d o;
    o.x = (f64)origin.x;
    o.y = (f64)origin.y;
    de_ray_hit2d h;
    if (!de_physics2d_raycast_ex(p, o, dir, max_dist, &qf, &h))
        return false;
    if (hit_entity != NULL)
        *hit_entity = h.entity;
    if (point != NULL)
        *point = de__v2((f32)h.point.x, (f32)h.point.y);
    if (normal != NULL)
        *normal = h.normal;
    return true;
}

u32 de_physics2d_raycast_all(const de_physics2d_world *p, de_dpoint2d origin, de_vec2 dir,
                             f32 max_dist, const de_physics_query_filter *filter,
                             de_ray_hit2d *hits, u32 max_hits) {
    if (p == NULL || hits == NULL || max_hits == 0u)
        return 0;
    de__q2ray r;
    if (!de__q2ray_init(&r, de__d2_make(origin.x, origin.y), dir, max_dist))
        return 0;
    de__q2filter f;
    de__q2filter_init(p, filter, &f);
    ray_ctx c;
    memset(&c, 0, sizeof c);
    c.p = p;
    c.f = &f;
    c.r = &r;
    c.out = hits;
    c.max_hits = max_hits;
    walk(p, &r, &f, all_cb, &c);
    return c.count;
}

bool de__q2_raycast_closest(const de_physics2d_world *p, const de__q2ray *r, const de__q2filter *f,
                            de__q2hit *out) {
    return raycast_closest(p, r, f, out);
}
