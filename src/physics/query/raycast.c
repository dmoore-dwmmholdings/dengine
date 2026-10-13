/*
 * raycast.c: query filters, candidate enumeration, and de_physics_raycast / raycast_all
 * (DENG-137).
 *
 * Candidates come from the broadphase trees (de__broadphase_query_aabb / de__broadphase_raycast)
 * when the broadphase exists, otherwise from every body in ascending index. Either way a body is
 * reported by the per-body test (pure) and selected with fixed keys, so the visiting order never
 * shows in the results.
 */
#include "physics/query/query_internal.h"

/* ---- Filters ------------------------------------------------------------------------------ */

static de_collision_filter resolve(de_collision_filter f) {
    if (f.layer == 0u && f.mask == 0u && f.group == 0u) {
        f.layer = DE_COLLISION_FILTER_DEFAULT_LAYER;
        f.mask = DE_COLLISION_FILTER_DEFAULT_MASK;
    }
    return f;
}

void de__qfilter_plain(de_collision_filter filter, de__qfilter *out) {
    out->filter = resolve(filter);
    out->flags = 0u;
    out->ignore_index = DE__Q_NO_BODY;
}

void de__qfilter_init(const de_physics_world *p, const de_physics_query_filter *in,
                      de__qfilter *out) {
    de_collision_filter zero;
    memset(&zero, 0, sizeof zero);
    de__qfilter_plain(in != NULL ? in->filter : zero, out);
    if (in == NULL)
        return;
    out->flags = in->flags;
    u32 index;
    if (de__body_resolve(p, in->ignore_body, &index))
        out->ignore_index = index;
}

de_quat de__q_quat_mul(de_quat a, de_quat b) {
    const f64 ax = a.x, ay = a.y, az = a.z, aw = a.w;
    const f64 bx = b.x, by = b.y, bz = b.z, bw = b.w;
    const f64 x = aw * bx + ax * bw + ay * bz - az * by;
    const f64 y = aw * by - ax * bz + ay * bw + az * bx;
    const f64 z = aw * bz + ax * by - ay * bx + az * bw;
    const f64 w = aw * bw - ax * bx - ay * by - az * bz;
    const f64 n = x * x + y * y + z * z + w * w;
    de_quat r;
    if (!(n > 0.0)) {
        r.x = r.y = r.z = 0.0f;
        r.w = 1.0f;
        return r;
    }
    const f64 k = 1.0 / de_dsqrt64(n);
    r.x = (f32)(x * k);
    r.y = (f32)(y * k);
    r.z = (f32)(z * k);
    r.w = (f32)(w * k);
    return r;
}

/* ---- Candidate enumeration -------------------------------------------------------------- */

typedef struct aabb_ctx {
    const de_physics_world *p;
    const de__qfilter *f;
    de_aabb box;
    de__q_body_fn fn;
    void *user;
} aabb_ctx;

static bool aabb_cb(void *user, u32 body) {
    const aabb_ctx *c = (const aabb_ctx *)user;
    /* The tree holds fat boxes: test the body's own box too. */
    if (!de__query_accept(c->p, body, c->f) ||
        !de__aabb_overlap(&c->p->bodies.world_aabb[body], &c->box))
        return true;
    return c->fn(c->user, body);
}

void de__query_each_aabb(const de_physics_world *p, de_aabb box, const de__qfilter *f,
                         de__q_body_fn fn, void *user) {
    aabb_ctx c = {p, f, box, fn, user};
    if (de__broadphase_get(p) != NULL) {
        (void)de__broadphase_query_aabb(p, box, NULL, aabb_cb, &c);
        return;
    }
    for (u32 i = 0; i < p->bodies.high_water; ++i)
        if (!aabb_cb(&c, i))
            return;
}

/** Called for each accepted body the segment may hit within `bound`; returns the new bound. */
typedef f32 (*q_ray_fn)(void *user, u32 body, f32 bound);

typedef struct ray_ctx {
    const de_physics_world *p;
    const de__qfilter *f;
    const de__qray *ray;
    q_ray_fn fn;
    void *user;
} ray_ctx;

static f32 ray_cb(void *user, const de__tree_ray *tr, u32 body, f32 bound) {
    const ray_ctx *c = (const ray_ctx *)user;
    (void)tr;
    f64 te = 0.0;
    if (!de__query_accept(c->p, body, c->f) ||
        !de__q_slab(c->ray->origin, c->ray->delta, de__d3_from(c->p->bodies.world_aabb[body].min),
                    de__d3_from(c->p->bodies.world_aabb[body].max), (f64)bound, &te))
        return bound;
    return c->fn(c->user, body, bound);
}

/* Visits the candidates of the segment, starting with bound `bound`. */
static void each_ray(const de_physics_world *p, const de__qray *ray, f32 bound,
                     const de__qfilter *f, q_ray_fn fn, void *user) {
    ray_ctx c = {p, f, ray, fn, user};
    if (de__broadphase_get(p) != NULL) {
        de__tree_ray tr;
        tr.origin = ray->origin;
        tr.delta = ray->delta32;
        tr.max_fraction = bound;
        (void)de__broadphase_raycast(p, &tr, NULL, ray_cb, &c);
        return;
    }
    for (u32 i = 0; i < p->bodies.high_water; ++i) {
        bound = ray_cb(&c, NULL, i, bound);
        if (!(bound > 0.0f))
            return;
    }
}

/* ---- Rays --------------------------------------------------------------------------------- */

bool de__qray_init(de__qray *r, de_dvec3 origin, de_vec3 dir, f32 max_dist) {
    if (!de__d3_finite(origin) || !de__v3_finite(dir) || !de__f32_finite(max_dist) ||
        !(max_dist > 0.0f))
        return false;
    const de_dvec3 d = de__d3_from(dir);
    const f64 l2 = de__d3_dot(d, d);
    if (!(l2 > 0.0) || !de__f64_finite(l2))
        return false;
    const de_dvec3 u = de__d3_scale(d, 1.0 / de_dsqrt64(l2));
    r->origin = origin;
    r->dir = de__q_to_f32(u);
    r->delta32 = de__q_to_f32(de__d3_scale(u, (f64)max_dist));
    r->delta = de__d3_from(r->delta32);
    return de__v3_finite(r->delta32);
}

bool de__query_ray_body(const de_physics_world *p, u32 body, const de__qray *ray, f64 max_t,
                        u32 flags, de_ray_hit *out) {
    const de__shape *s = de__shape_get(p, p->bodies.shape[body]);
    if (s == NULL)
        return false;
    const de__dmat3 rot = de__dmat3_from_quat(p->bodies.rotation[body]);
    /* The offset from the body in f64 first (large worlds), then into the body frame. */
    const de_dvec3 o = de__dmat3_tmul(&rot, de__d3_sub(ray->origin, p->bodies.position[body]));
    const de_dvec3 d = de__dmat3_tmul(&rot, ray->delta);
    de__qlocal_hit h;
    if (!de__ray_shape(s, o, d, max_t, flags, &h))
        return false;
    out->entity = p->bodies.owner[body];
    out->body = de__body_handle(p, body);
    if (h.inside) {
        out->point = ray->origin;
        out->normal = de__v3(-ray->dir.x, -ray->dir.y, -ray->dir.z);
        out->fraction = 0.0f;
        out->sub_shape = h.sub; /* the compound child it starts in; else none */
        return true;
    }
    out->point = de__d3_add(ray->origin, de__d3_scale(ray->delta, h.t));
    out->normal = de__q_to_f32(de__q_dnorm(de__dmat3_mul(&rot, h.normal)));
    out->fraction = (f32)h.t;
    out->sub_shape = h.sub;
    return true;
}

/* Every hit with t <= 1; the traversal starts just above 1 so a hit at the very end is seen. */
#define Q_RAY_LIMIT 1.0000001f

typedef struct closest_ctx {
    const de_physics_world *p;
    const de__qray *ray;
    u32 flags;
    bool found;
    de_ray_hit best;
} closest_ctx;

static f32 closest_cb(void *user, u32 body, f32 bound) {
    closest_ctx *c = (closest_ctx *)user;
    de_ray_hit h;
    const f64 max_t = de__mind(1.0, (f64)bound);
    if (!de__query_ray_body(c->p, body, c->ray, max_t, c->flags, &h))
        return bound;
    if (!c->found || de__q_hit_less(&h, &c->best)) {
        c->found = true;
        c->best = h;
    }
    return de__q_bound(c->best.fraction, Q_RAY_LIMIT);
}

static bool raycast(const de_physics_world *p, de_dvec3 origin, de_vec3 dir, f32 max_dist,
                    const de__qfilter *f, de_ray_hit *hit) {
    de__qray ray;
    if (p == NULL || hit == NULL || !de__qray_init(&ray, origin, dir, max_dist))
        return false;
    closest_ctx c;
    memset(&c, 0, sizeof c);
    c.p = p;
    c.ray = &ray;
    c.flags = f->flags;
    each_ray(p, &ray, Q_RAY_LIMIT, f, closest_cb, &c);
    if (c.found)
        *hit = c.best;
    return c.found;
}

typedef struct all_ctx {
    const de_physics_world *p;
    const de__qray *ray;
    u32 flags;
    de_ray_hit *hits;
    u32 max;
    u32 count;
} all_ctx;

static f32 all_cb(void *user, u32 body, f32 bound) {
    all_ctx *c = (all_ctx *)user;
    de_ray_hit h;
    const f64 max_t = de__mind(1.0, (f64)bound);
    if (!de__query_ray_body(c->p, body, c->ray, max_t, c->flags, &h))
        return bound;
    if (c->count == c->max) {
        if (!de__q_hit_less(&h, &c->hits[c->max - 1u]))
            return bound;
        --c->count; /* drop the last */
    }
    u32 j = c->count;
    while (j > 0 && de__q_hit_less(&h, &c->hits[j - 1u])) {
        c->hits[j] = c->hits[j - 1u];
        --j;
    }
    c->hits[j] = h;
    ++c->count;
    /* Full: nothing past the last kept hit can get in any more. */
    return c->count == c->max ? de__q_bound(c->hits[c->max - 1u].fraction, Q_RAY_LIMIT) : bound;
}

static u32 raycast_all(const de_physics_world *p, de_dvec3 origin, de_vec3 dir, f32 max_dist,
                       const de__qfilter *f, de_ray_hit *hits, u32 max_hits) {
    de__qray ray;
    if (p == NULL || hits == NULL || max_hits == 0u || !de__qray_init(&ray, origin, dir, max_dist))
        return 0;
    all_ctx c = {p, &ray, f->flags, hits, max_hits, 0};
    each_ray(p, &ray, Q_RAY_LIMIT, f, all_cb, &c);
    return c.count;
}

/* ---- Public API --------------------------------------------------------------------------- */

bool de_physics_raycast(const de_physics_world *p, de_dvec3 origin, de_vec3 dir, f32 max_dist,
                        de_collision_filter filter, de_ray_hit *hit) {
    de__qfilter f;
    de__qfilter_plain(filter, &f);
    return raycast(p, origin, dir, max_dist, &f, hit);
}

bool de_physics_raycast_ex(const de_physics_world *p, de_dvec3 origin, de_vec3 dir, f32 max_dist,
                           const de_physics_query_filter *filter, de_ray_hit *hit) {
    if (p == NULL)
        return false;
    de__qfilter f;
    de__qfilter_init(p, filter, &f);
    return raycast(p, origin, dir, max_dist, &f, hit);
}

u32 de_physics_raycast_all(const de_physics_world *p, de_dvec3 origin, de_vec3 dir, f32 max_dist,
                           de_collision_filter filter, de_ray_hit *hits, u32 max_hits) {
    de__qfilter f;
    de__qfilter_plain(filter, &f);
    return raycast_all(p, origin, dir, max_dist, &f, hits, max_hits);
}

u32 de_physics_raycast_all_ex(const de_physics_world *p, de_dvec3 origin, de_vec3 dir, f32 max_dist,
                              const de_physics_query_filter *filter, de_ray_hit *hits,
                              u32 max_hits) {
    if (p == NULL)
        return 0;
    de__qfilter f;
    de__qfilter_init(p, filter, &f);
    return raycast_all(p, origin, dir, max_dist, &f, hits, max_hits);
}
