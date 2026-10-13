/*
 * overlap.c: de_physics_overlap (DENG-137).
 *
 * Candidates: the bodies whose world box overlaps the query shape's world box (broadphase tree
 * query). Narrow test: GJK distance between the cores (narrow/gjk.c) against the summed radii
 * (overlap or touching counts); for meshes and heightfields every triangle in the query's box,
 * two-sided, until one overlaps; compounds child by child on either side.
 *
 * Results are body indices in ascending order. The caller's arrays receive the lowest `max`
 * indices: candidates are collected in passes of up to Q_CHUNK sorted indices on the stack,
 * each pass starting above the last index of the previous one, so nothing is allocated and the
 * output does not depend on the tree's visiting order.
 */
#include "physics/query/query_internal.h"

/* Sorted indices per pass. */
#define Q_CHUNK 64u
/* Grows the query box for triangle culling (touching counts). */
#define Q_TOUCH_SLACK 1e-4f

static bool convex_overlap(const de__qside *a, const de__qside *b) {
    de__np_rel rel;
    de__np_rel_init(&rel, a->pose, b->pose, 0.0f);
    de__gjk_proxy pa, pb;
    de__gjk_proxy_shape(&pa, a->shape, true, NULL);
    de__gjk_proxy_shape(&pb, b->shape, true, &rel);
    de__gjk_result g;
    de__gjk_distance(&pa, &pb, &g);
    return g.overlap ||
           g.distance <= (f64)de__shape_margin(a->shape) + (f64)de__shape_margin(b->shape);
}

typedef struct tri_job {
    const de__np_rel *rel;
    const de__shape *q;
    bool hit;
} tri_job;

static bool tri_cb(void *user, u32 sub, de_vec3 a, de_vec3 b, de_vec3 c) {
    tri_job *j = (tri_job *)user;
    (void)sub;
    const de_vec3 pts[3] = {a, b, c};
    de__gjk_proxy pa, pb;
    de__gjk_proxy_points(&pa, pts, 3, NULL);
    de__gjk_proxy_shape(&pb, j->q, true, j->rel);
    de__gjk_result g;
    de__gjk_distance(&pa, &pb, &g);
    if (g.overlap || g.distance <= (f64)de__shape_margin(j->q)) {
        j->hit = true;
        return false;
    }
    return true;
}

/* Target side `t` (any kind) against convex query side `q`. */
static bool side_overlap(const de__qside *t, const de__qside *q) {
    const de_shape_kind k = t->shape->kind;
    if (k == DE_SHAPE_COMPOUND || k == DE_SHAPE_MESH || k == DE_SHAPE_HEIGHTFIELD) {
        de__np_rel rel;
        de__np_rel_init(&rel, t->pose, q->pose, 0.0f);
        const de_aabb box = de__np_box_in_a(&rel, q->shape->local_aabb, Q_TOUCH_SLACK);
        if (k == DE_SHAPE_COMPOUND) {
            const de__compound *cp = &t->shape->u.compound;
            for (u32 i = 0; i < cp->child_count; ++i) {
                if (!de__aabb_overlap(&cp->children[i].bounds, &box))
                    continue;
                const de__qside child = de__q_child_side(t, i);
                if (side_overlap(&child, q))
                    return true;
            }
            return false;
        }
        tri_job j = {&rel, q->shape, false};
        de__q_triangles(t->shape, box, tri_cb, &j);
        return j.hit;
    }
    return convex_overlap(t, q);
}

bool de__qshape_init(de__qshape *q, const de__shape *shape, const de_transform *at) {
    if (shape == NULL || at == NULL || !de__d3_finite(at->position) ||
        !de__f32_finite(at->rotation.x) || !de__f32_finite(at->rotation.y) ||
        !de__f32_finite(at->rotation.z) || !de__f32_finite(at->rotation.w))
        return false;
    if (!de__shape_kind_is_convex(shape->kind)) {
        if (shape->kind != DE_SHAPE_COMPOUND || shape->u.compound.child_count == 0)
            return false;
        for (u32 i = 0; i < shape->u.compound.child_count; ++i)
            if (!de__shape_kind_is_convex(shape->u.compound.children[i].shape->kind))
                return false;
    }
    q->shape = shape;
    q->pose.position = at->position;
    q->pose.rotation = de__q_quat_normalize(at->rotation);
    q->box = de__shape_world_aabb(shape, q->pose.position, q->pose.rotation);
    return true;
}

bool de__query_overlap_body(const de_physics_world *p, u32 body, const de__qshape *q) {
    de__qside t;
    t.shape = de__shape_get(p, p->bodies.shape[body]);
    if (t.shape == NULL)
        return false;
    t.pose.position = p->bodies.position[body];
    t.pose.rotation = p->bodies.rotation[body];
    t.sub = DE_SUB_SHAPE_NONE;
    const de__qside whole = {q->shape, q->pose, DE_SUB_SHAPE_NONE};
    if (q->shape->kind != DE_SHAPE_COMPOUND)
        return side_overlap(&t, &whole);
    for (u32 i = 0; i < q->shape->u.compound.child_count; ++i) {
        const de__qside part = de__q_child_side(&whole, i);
        if (side_overlap(&t, &part))
            return true;
    }
    return false;
}

/* ---- Collection ------------------------------------------------------------------------- */

typedef struct collect {
    const de_physics_world *p;
    const de__qshape *q;
    u32 lo;           /* only indices >= lo in this pass */
    u32 buf[Q_CHUNK]; /* ascending */
    u32 n;
    bool more; /* an overlapping index above the chunk was left out */
} collect;

static bool collect_cb(void *user, u32 body) {
    collect *c = (collect *)user;
    if (body < c->lo)
        return true;
    if (c->n == Q_CHUNK && body > c->buf[Q_CHUNK - 1u]) {
        c->more = true; /* found again by a later pass, if it overlaps */
        return true;
    }
    if (!de__query_overlap_body(c->p, body, c->q))
        return true;
    if (c->n == Q_CHUNK) {
        c->more = true;
        --c->n; /* drop the largest */
    }
    u32 j = c->n;
    while (j > 0 && c->buf[j - 1u] > body) {
        c->buf[j] = c->buf[j - 1u];
        --j;
    }
    c->buf[j] = body;
    ++c->n;
    return true;
}

static u32 overlap(const de_physics_world *p, de_shape shape, const de_transform *at,
                   const de__qfilter *f, de_body *bodies, de_entity *entities, u32 max) {
    if (p == NULL || max == 0u || (bodies == NULL && entities == NULL))
        return 0;
    de__qshape q;
    if (!de__qshape_init(&q, de__shape_get(p, shape), at))
        return 0;
    u32 written = 0;
    collect c;
    c.p = p;
    c.q = &q;
    c.lo = 0;
    for (;;) {
        c.n = 0;
        c.more = false;
        de__query_each_aabb(p, q.box, f, collect_cb, &c);
        for (u32 i = 0; i < c.n && written < max; ++i, ++written) {
            if (bodies != NULL)
                bodies[written] = de__body_handle(p, c.buf[i]);
            if (entities != NULL)
                entities[written] = p->bodies.owner[c.buf[i]];
        }
        if (!c.more || written == max || c.n == 0)
            break;
        c.lo = c.buf[c.n - 1u] + 1u;
    }
    return written;
}

u32 de_physics_overlap(const de_physics_world *p, de_shape shape, const de_transform *at,
                       de_collision_filter filter, de_entity *out, u32 max) {
    de__qfilter f;
    de__qfilter_plain(filter, &f);
    return overlap(p, shape, at, &f, NULL, out, max);
}

u32 de_physics_overlap_ex(const de_physics_world *p, de_shape shape, const de_transform *at,
                          const de_physics_query_filter *filter, de_body *bodies,
                          de_entity *entities, u32 max) {
    if (p == NULL)
        return 0;
    de__qfilter f;
    de__qfilter_init(p, filter, &f);
    return overlap(p, shape, at, &f, bodies, entities, max);
}
