/*
 * query2d.c: filters and candidate walks of the 2D scene queries, per-body closest point and
 * overlap tests, de_physics2d_overlap / overlap_aabb / closest_point (DENG-258; see
 * query2d_internal.h).
 *
 * Closest points are exact, in the body's local frame in f64: circles and capsules by their core
 * point / segment, polygons by the core polygon (inside test on the edge normals, else the
 * nearest edge) minus the rounding radius, segments and chains as two-sided lines.
 *
 * Overlap results are body indices in ascending order. The caller's arrays receive the lowest
 * `max` indices: candidates are collected in passes of up to DE__Q2_CHUNK sorted indices on the
 * stack, each pass starting above the last index of the previous one (as the 3D overlap), so
 * nothing is allocated and the output does not depend on the tree's visiting order.
 */
#include "physics/2d/query2d_internal.h"

/* ================================ Filters =============================================== */

void de__q2filter_init(const de_physics2d_world *p, const de_physics_query_filter *f,
                       de__q2filter *out) {
    de_physics_query_filter d;
    memset(&d, 0, sizeof d);
    if (f != NULL)
        d = *f;
    out->filter = d.filter;
    if (d.filter.layer == 0u && d.filter.mask == 0u && d.filter.group == 0u) {
        out->filter.layer = DE_COLLISION_FILTER_DEFAULT_LAYER;
        out->filter.mask = DE_COLLISION_FILTER_DEFAULT_MASK;
    }
    out->flags = d.flags;
    out->ignore = DE__Q2_NO_BODY;
    u32 i;
    if (DE_HANDLE_VALID(d.ignore_body) && de__body2d_resolve(p, d.ignore_body, &i))
        out->ignore = i;
}

bool de__q2_candidate(const de_physics2d_world *p, u32 b, const de__q2filter *f) {
    if (!de__body2d_alive(p, b) || b == f->ignore || !DE_HANDLE_VALID(p->bodies.shape[b]))
        return false;
    if ((p->bodies.flags[b] & (u32)DE_BODY_FLAG_TRIGGER) != 0u &&
        (f->flags & (u32)DE_PHYSICS_QUERY_INCLUDE_TRIGGERS) == 0u)
        return false;
    return de__p2d_filter_test(f->filter, p->bodies.filter[b]);
}

/* ================================ Candidate walks ======================================= */

typedef struct each_ctx {
    const de_physics2d_world *p;
    const de__q2filter *f;
    de__q2_body_fn fn;
    void *user;
} each_ctx;

static bool each_cb(void *user, u32 body) {
    each_ctx *c = (each_ctx *)user;
    if (!de__q2_candidate(c->p, body, c->f))
        return true;
    return c->fn(c->user, body);
}

bool de__q2_each_aabb(const de_physics2d_world *p, de_aabb2d box, const de__q2filter *f,
                      de__q2_body_fn fn, void *user) {
    if (de__aabb2d_is_empty(box))
        return true;
    if (de__bp2d_get(p) != NULL) {
        each_ctx c = {p, f, fn, user};
        return de__bp2d_query_aabb(p, box, &f->filter, each_cb, &c);
    }
    /* No broadphase yet: every body, by its current box. */
    for (u32 i = 0; i < p->bodies.high_water; ++i) {
        if (!de__q2_candidate(p, i, f) || !de__aabb2d_overlap(&p->bodies.world_aabb[i], &box))
            continue;
        if (!fn(user, i))
            return false;
    }
    return true;
}

/* ================================ Hits ================================================== */

de_ray_hit2d de__q2hit_public(const de_physics2d_world *p, const de__q2hit *h) {
    de_ray_hit2d r;
    memset(&r, 0, sizeof r);
    r.entity = p->bodies.owner[h->body];
    r.body = de__body2d_handle(p, h->body);
    r.point.x = h->point.x;
    r.point.y = h->point.y;
    r.normal = h->normal;
    r.fraction = (f32)h->t;
    r.sub_shape = h->sub;
    return r;
}

de_aabb2d de__q2_box_in_frame(de_aabb2d box, de__d2 pos, de_rot2d rot, f32 grow) {
    if (de__aabb2d_is_empty(box))
        return box;
    const f64 cx = 0.5 * ((f64)box.min.x + (f64)box.max.x) - pos.x;
    const f64 cy = 0.5 * ((f64)box.min.y + (f64)box.max.y) - pos.y;
    const f64 hx = 0.5 * ((f64)box.max.x - (f64)box.min.x);
    const f64 hy = 0.5 * ((f64)box.max.y - (f64)box.min.y);
    const f64 c = rot.c, s = rot.s;
    /* Inverse rotation of the center; extents of the rotated box. */
    const f64 lx = c * cx + s * cy;
    const f64 ly = -s * cx + c * cy;
    const f64 ex = de__absd(c) * hx + de__absd(s) * hy + (f64)grow;
    const f64 ey = de__absd(s) * hx + de__absd(c) * hy + (f64)grow;
    return de__aabb2d_from_d(de__d2_make(lx - ex, ly - ey), de__d2_make(lx + ex, ly + ey));
}

/* ================================ Closest points (f64, local frame) ===================== */

/* Closest point to q on segment a..b. */
static de__d2 closest_on_segment(de__d2 q, de__d2 a, de__d2 b) {
    const de__d2 e = de__d2_sub(b, a);
    const f64 ee = de__d2_dot(e, e);
    if (!(ee > 0.0))
        return a;
    f64 t = de__d2_dot(de__d2_sub(q, a), e) / ee;
    if (t <= 0.0)
        return a;
    if (t >= 1.0)
        return b;
    return de__d2_add(a, de__d2_scale(e, t));
}

static f64 d2_len(de__d2 v) {
    return de_dsqrt64(de__d2_dot(v, v));
}

/* A local closest-point result. */
typedef struct pt_res {
    f64 dist;
    de__d2 point;
    de__d2 normal; /* unit or zero */
} pt_res;

/* q against a core point / segment c..d swept by r (circle: c == d). */
static void pt_rounded_segment(de__d2 q, de__d2 c, de__d2 d, f64 r, pt_res *out) {
    const de__d2 k = closest_on_segment(q, c, d);
    const de__d2 v = de__d2_sub(q, k);
    const f64 l = d2_len(v);
    if (l <= r) {
        out->dist = 0.0;
        out->point = q;
        out->normal = de__d2_make(0.0, 0.0);
        return;
    }
    const de__d2 n = de__d2_scale(v, 1.0 / l);
    out->dist = l - r;
    out->point = de__d2_add(k, de__d2_scale(n, r));
    out->normal = n;
}

/* q against a two-sided line segment a..b with right normal `nrm`. */
static void pt_line(de__d2 q, de__d2 a, de__d2 b, de__d2 nrm, pt_res *out) {
    const de__d2 k = closest_on_segment(q, a, b);
    const de__d2 v = de__d2_sub(q, k);
    const f64 l = d2_len(v);
    out->dist = l;
    out->point = k;
    out->normal = l > 0.0 ? de__d2_scale(v, 1.0 / l) : nrm;
}

static void pt_polygon(de__d2 q, const de__polygon2d *poly, pt_res *out) {
    bool inside = true;
    for (u32 i = 0; i < poly->count; ++i) {
        const de__d2 n = de__d2_from(poly->normals[i]);
        const de__d2 v = de__d2_from(poly->vertices[i]);
        if (de__d2_dot(n, de__d2_sub(q, v)) > 0.0) {
            inside = false;
            break;
        }
    }
    if (inside) {
        out->dist = 0.0;
        out->point = q;
        out->normal = de__d2_make(0.0, 0.0);
        return;
    }
    f64 best = 0.0;
    de__d2 bk = de__d2_make(0.0, 0.0);
    for (u32 i = 0; i < poly->count; ++i) {
        const de__d2 a = de__d2_from(poly->vertices[i]);
        const de__d2 b = de__d2_from(poly->vertices[(i + 1u) % poly->count]);
        const de__d2 k = closest_on_segment(q, a, b);
        const de__d2 v = de__d2_sub(q, k);
        const f64 d2 = de__d2_dot(v, v);
        if (i == 0u || d2 < best) {
            best = d2;
            bk = k;
        }
    }
    pt_rounded_segment(q, bk, bk, (f64)poly->radius, out);
}

typedef struct chain_pt_job {
    const de__chain2d *chain;
    de__d2 q;
    f64 max_dist;
    bool found;
    u32 seg;
    pt_res best;
} chain_pt_job;

static bool chain_pt_cb(void *user, u32 segment) {
    chain_pt_job *j = (chain_pt_job *)user;
    const de__chain_segment *s = &j->chain->segments[segment];
    pt_res r;
    memset(&r, 0, sizeof r);
    pt_line(j->q, de__d2_from(s->p1), de__d2_from(s->p2), de__d2_from(s->normal), &r);
    if (r.dist > j->max_dist)
        return true;
    if (!j->found || r.dist < j->best.dist || (r.dist == j->best.dist && segment < j->seg)) {
        j->found = true;
        j->seg = segment;
        j->best = r;
    }
    return true;
}

bool de__q2_point_body(const de_physics2d_world *p, u32 b, de__d2 q, f64 max_dist, de__q2hit *out) {
    const de__shape2d *s = de__shape2d_get(p, p->bodies.shape[b]);
    if (s == NULL)
        return false;
    const de__d2 pos = p->bodies.position[b];
    const de_rot2d rot = p->bodies.rotation[b];
    const de__d2 rel = de__d2_sub(q, pos);
    const f64 c = rot.c, sn = rot.s;
    const de__d2 ql = de__d2_make(c * rel.x + sn * rel.y, -sn * rel.x + c * rel.y);
    pt_res r;
    memset(&r, 0, sizeof r);
    u32 sub = DE_SUB_SHAPE_NONE;
    switch (s->kind) {
    case DE_SHAPE2D_CIRCLE: {
        const de__d2 cc = de__d2_from(s->u.circle.center);
        pt_rounded_segment(ql, cc, cc, (f64)s->u.circle.radius, &r);
        break;
    }
    case DE_SHAPE2D_CAPSULE:
        pt_rounded_segment(ql, de__d2_from(s->u.capsule.p1), de__d2_from(s->u.capsule.p2),
                           (f64)s->u.capsule.radius, &r);
        break;
    case DE_SHAPE2D_BOX:
    case DE_SHAPE2D_POLYGON:
        pt_polygon(ql, &s->u.polygon, &r);
        break;
    case DE_SHAPE2D_SEGMENT:
        pt_line(ql, de__d2_from(s->u.segment.p1), de__d2_from(s->u.segment.p2),
                de__d2_from(s->u.segment.normal), &r);
        break;
    case DE_SHAPE2D_CHAIN: {
        chain_pt_job j;
        memset(&j, 0, sizeof j);
        j.chain = &s->u.chain;
        j.q = ql;
        j.max_dist = max_dist;
        const f64 m = max_dist + 1e-6;
        const de_aabb2d box =
            de__aabb2d_from_d(de__d2_make(ql.x - m, ql.y - m), de__d2_make(ql.x + m, ql.y + m));
        (void)de__chain2d_query(&s->u.chain, box, chain_pt_cb, &j);
        if (!j.found)
            return false;
        r = j.best;
        sub = j.seg;
        break;
    }
    default:
        return false;
    }
    /* Back to the world: rotate the normal and the offset of the point. */
    const de__d2 pw = de__rot2d_apply_d(rot, r.point);
    const de__d2 nw = de__rot2d_apply_d(rot, r.normal);
    out->t = r.dist;
    out->point = r.dist == 0.0 && (s->has_area) ? q : de__d2_add(pos, pw);
    out->normal =
        r.normal.x == 0.0 && r.normal.y == 0.0 ? de__v2(0.0f, 0.0f) : de__v2_normalize_d(nw);
    out->body = b;
    out->sub = sub;
    out->inside = r.dist == 0.0 && s->has_area;
    return true;
}

/* ================================ Overlap test ========================================== */

typedef struct chain_ov_job {
    const de__chain2d *chain;
    de_rot2d rot;
    de__d2 off; /* chain body position - query origin */
    const de__q2proxy *q;
    bool hit;
} chain_ov_job;

static bool chain_ov_cb(void *user, u32 segment) {
    chain_ov_job *j = (chain_ov_job *)user;
    const de__chain_segment *s = &j->chain->segments[segment];
    const de__d2 a = de__d2_add(j->off, de__rot2d_apply_d(j->rot, de__d2_from(s->p1)));
    const de__d2 b = de__d2_add(j->off, de__rot2d_apply_d(j->rot, de__d2_from(s->p2)));
    de__q2proxy seg;
    de__q2proxy_segment(&seg, de__v2((f32)a.x, (f32)a.y), de__v2((f32)b.x, (f32)b.y));
    if (de__q2_proxy_overlap(&seg, j->q)) {
        j->hit = true;
        return false;
    }
    return true;
}

bool de__q2_overlap_body(const de_physics2d_world *p, u32 b, const de__q2proxy *q, de__d2 qo,
                         de_aabb2d qbox) {
    const de__shape2d *s = de__shape2d_get(p, p->bodies.shape[b]);
    if (s == NULL)
        return false;
    const de__d2 pos = p->bodies.position[b];
    const de_rot2d rot = p->bodies.rotation[b];
    const de__d2 off = de__d2_sub(pos, qo);
    if (s->kind == DE_SHAPE2D_CHAIN) {
        chain_ov_job j = {&s->u.chain, rot, off, q, false};
        const de_aabb2d local = de__q2_box_in_frame(qbox, pos, rot, 1e-4f);
        (void)de__chain2d_query(&s->u.chain, local, chain_ov_cb, &j);
        return j.hit;
    }
    de__q2proxy t;
    de__q2proxy_shape(&t, s, rot, off);
    return de__q2_proxy_overlap(&t, q);
}

/* ================================ Overlap collection ==================================== */

typedef struct collect {
    const de_physics2d_world *p;
    const de__q2proxy *q;
    de__d2 qo;
    de_aabb2d qbox;
    u32 lo;                /* only indices >= lo in this pass */
    u32 buf[DE__Q2_CHUNK]; /* ascending */
    u32 n;
    bool more; /* an overlapping index above the chunk was left out */
} collect;

static bool collect_cb(void *user, u32 body) {
    collect *c = (collect *)user;
    if (body < c->lo)
        return true;
    if (c->n == DE__Q2_CHUNK && body > c->buf[DE__Q2_CHUNK - 1u]) {
        c->more = true; /* found again by a later pass, if it overlaps */
        return true;
    }
    if (!de__q2_overlap_body(c->p, body, c->q, c->qo, c->qbox))
        return true;
    if (c->n == DE__Q2_CHUNK) {
        c->more = true;
        --c->n; /* drop the largest */
    }
    u32 j = c->n;
    while (j > 0u && c->buf[j - 1u] > body) {
        c->buf[j] = c->buf[j - 1u];
        --j;
    }
    c->buf[j] = body;
    ++c->n;
    return true;
}

static u32 overlap_collect(const de_physics2d_world *p, const de__q2proxy *q, de__d2 qo,
                           de_aabb2d qbox, const de_physics_query_filter *filter, de_body *bodies,
                           de_entity *entities, u32 max) {
    de__q2filter f;
    de__q2filter_init(p, filter, &f);
    u32 written = 0;
    collect c;
    memset(&c, 0, sizeof c);
    c.p = p;
    c.q = q;
    c.qo = qo;
    c.qbox = qbox;
    c.lo = 0;
    for (;;) {
        c.n = 0;
        c.more = false;
        (void)de__q2_each_aabb(p, qbox, &f, collect_cb, &c);
        for (u32 k = 0; k < c.n && written < max; ++k) {
            const u32 b = c.buf[k];
            if (bodies != NULL)
                bodies[written] = de__body2d_handle(p, b);
            if (entities != NULL)
                entities[written] = p->bodies.owner[b];
            ++written;
        }
        if (!c.more || written >= max || c.n == 0u)
            break;
        c.lo = c.buf[c.n - 1u] + 1u;
    }
    return written;
}

u32 de_physics2d_overlap(const de_physics2d_world *p, de_shape shape, const de_transform2d *at,
                         const de_physics_query_filter *filter, de_body *bodies,
                         de_entity *entities, u32 max) {
    if (p == NULL || at == NULL || max == 0u || (bodies == NULL && entities == NULL))
        return 0;
    const de__shape2d *s = de__shape2d_get(p, shape);
    if (s == NULL || !de__shape2d_kind_is_convex(s->kind) || !de__f64_finite(at->x) ||
        !de__f64_finite(at->y) || !de__rot2d_finite(at->rotation))
        return 0;
    const de__d2 qo = de__d2_make(at->x, at->y);
    const de_rot2d rot = de__rot2d_normalize(at->rotation);
    de__q2proxy q;
    de__q2proxy_shape(&q, s, rot, de__d2_make(0.0, 0.0));
    const de_aabb2d qbox = de__shape2d_world_aabb(s, qo, rot);
    return overlap_collect(p, &q, qo, qbox, filter, bodies, entities, max);
}

u32 de_physics2d_overlap_aabb(const de_physics2d_world *p, de_aabb2d box,
                              const de_physics_query_filter *filter, de_body *bodies,
                              de_entity *entities, u32 max) {
    if (p == NULL || max == 0u || (bodies == NULL && entities == NULL))
        return 0;
    if (!de__v2_finite(box.min) || !de__v2_finite(box.max) || de__aabb2d_is_empty(box))
        return 0;
    /* The box as a 4-vertex proxy around its center (f64 center: large worlds). */
    const de__d2 qo = de__d2_make(0.5 * ((f64)box.min.x + (f64)box.max.x),
                                  0.5 * ((f64)box.min.y + (f64)box.max.y));
    const f32 lx = (f32)((f64)box.min.x - qo.x), hx = (f32)((f64)box.max.x - qo.x);
    const f32 ly = (f32)((f64)box.min.y - qo.y), hy = (f32)((f64)box.max.y - qo.y);
    de__q2proxy q;
    memset(&q, 0, sizeof q);
    q.count = 4;
    q.v[0] = de__v2(lx, ly);
    q.v[1] = de__v2(hx, ly);
    q.v[2] = de__v2(hx, hy);
    q.v[3] = de__v2(lx, hy);
    q.radius = 0.0f;
    return overlap_collect(p, &q, qo, box, filter, bodies, entities, max);
}

/* ================================ Closest point ========================================= */

typedef struct closest_job {
    const de_physics2d_world *p;
    de__d2 q;
    f64 max_dist;
    bool found;
    de__q2hit best;
} closest_job;

static bool closest_cb(void *user, u32 body) {
    closest_job *j = (closest_job *)user;
    de__q2hit h;
    if (!de__q2_point_body(j->p, body, j->q, j->max_dist, &h) || h.t > j->max_dist)
        return true;
    if (!j->found || de__q2hit_before(&h, &j->best)) {
        j->found = true;
        j->best = h;
    }
    return true;
}

bool de_physics2d_closest_point(const de_physics2d_world *p, de_dpoint2d point, f32 max_dist,
                                const de_physics_query_filter *filter, de_ray_hit2d *hit) {
    if (p == NULL || hit == NULL || !de__f64_finite(point.x) || !de__f64_finite(point.y) ||
        !de__f32_finite(max_dist) || max_dist < 0.0f)
        return false;
    de__q2filter f;
    de__q2filter_init(p, filter, &f);
    closest_job j;
    memset(&j, 0, sizeof j);
    j.p = p;
    j.q = de__d2_make(point.x, point.y);
    j.max_dist = (f64)max_dist;
    const f64 m = (f64)max_dist;
    const de_aabb2d box =
        de__aabb2d_from_d(de__d2_make(j.q.x - m, j.q.y - m), de__d2_make(j.q.x + m, j.q.y + m));
    (void)de__q2_each_aabb(p, box, &f, closest_cb, &j);
    if (!j.found)
        return false;
    de__q2hit h = j.best;
    h.t = max_dist > 0.0f ? h.t / (f64)max_dist : 0.0;
    *hit = de__q2hit_public(p, &h);
    return true;
}
