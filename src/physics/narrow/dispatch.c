/*
 * dispatch.c: de__collide (DENG-134). Puts the pair in canonical order, recurses into compound
 * children, computes the pair frame (B in A, f64 then f32), picks the pair function from the
 * convex dispatch table or the midphase, and converts pair-frame manifolds into contact sets.
 */
#include "physics/narrow/narrowphase.h"

/* ---- Small helpers --------------------------------------------------------------------- */

de_vec3 de__np_normalize(de_vec3 a, de_vec3 fallback) {
    const f64 l2 = (f64)a.x * a.x + (f64)a.y * a.y + (f64)a.z * a.z;
    if (!(l2 > 0.0) || !de__f64_finite(l2))
        return fallback;
    const f64 k = 1.0 / de_dsqrt64(l2);
    return de__v3((f32)(a.x * k), (f32)(a.y * k), (f32)(a.z * k));
}

void de__np_rel_init(de__np_rel *rel, de__pose xa, de__pose xb, f32 speculative) {
    const de__dmat3 wa = de__dmat3_from_quat(xa.rotation);
    const de__dmat3 wb = de__dmat3_from_quat(xb.rotation);
    rel->wa = wa;
    const de_dvec3 t = de__dmat3_tmul(&wa, de__d3_sub(xb.position, xa.position));
    rel->t = de__v3((f32)t.x, (f32)t.y, (f32)t.z);
    /* r = wa^T * wb: column j is wa^T times column j of wb. */
    for (u32 j = 0; j < 3; ++j) {
        const de_dvec3 c =
            de__dmat3_tmul(&wa, de__d3(wb.m[j * 3 + 0], wb.m[j * 3 + 1], wb.m[j * 3 + 2]));
        rel->r.m[j * 3 + 0] = (f32)c.x;
        rel->r.m[j * 3 + 1] = (f32)c.y;
        rel->r.m[j * 3 + 2] = (f32)c.z;
    }
    /* World +Z in A = row 2 of wa. */
    rel->up = de__v3((f32)wa.m[2], (f32)wa.m[5], (f32)wa.m[8]);
    rel->spec = speculative;
}

de_aabb de__np_box_in_a(const de__np_rel *rel, de_aabb local, f32 grow) {
    de__dmat3 r;
    for (u32 i = 0; i < 9; ++i)
        r.m[i] = rel->r.m[i];
    de_aabb box = de__aabb_transform(local, de__d3_from(rel->t), &r);
    box.min = de__v3_sub(box.min, de__v3(grow, grow, grow));
    box.max = de__v3_add(box.max, de__v3(grow, grow, grow));
    return box;
}

/* ---- Dispatch table -------------------------------------------------------------------- */

/* [A kind][B kind] for convex kinds with A >= B (sphere 0, capsule 1, box 2, cylinder 3,
 * convex 4). Cylinders have no specialized path: GJK/EPA. */
static const de__np_pair_fn pair_table[5][5] = {
    {de__np_sphere_sphere, NULL, NULL, NULL, NULL},
    {de__np_capsule_sphere, de__np_capsule_capsule, NULL, NULL, NULL},
    {de__np_box_sphere, de__np_box_capsule, de__np_box_box, NULL, NULL},
    {de__np_convex_gjk, de__np_convex_gjk, de__np_convex_gjk, de__np_convex_gjk, NULL},
    {de__np_hull_sphere, de__np_hull_capsule, de__np_hull_box, de__np_convex_gjk, de__np_hull_hull},
};

de__np_pair_fn de__np_pair_function(de_shape_kind ka, de_shape_kind kb) {
    if ((u32)ka > 4u || (u32)kb > (u32)ka)
        return NULL;
    return pair_table[ka][kb];
}

/* ---- Output ---------------------------------------------------------------------------- */

static de_vec3 to_top(const de__np_side *s, de_vec3 p) {
    if (!s->has_off)
        return p;
    return de__v3_add(de__quat_rotate(s->off_rot, p), s->off_pos);
}

void de__np_emit(de__np_ctx *ctx, const de__np_manifold *m, const de__np_side *a,
                 const de__np_side *b, const de__np_rel *rel, u32 sub_a, bool swapped) {
    if (m->count == 0)
        return;
    de__contact_buffer *o = ctx->out;
    if (o->count >= o->capacity) {
        o->overflow = true;
        return;
    }
    de__contact_set *s = &o->sets[o->count++];
    memset(s, 0, sizeof *s);
    const de_dvec3 nw = de__dmat3_mul(&rel->wa, de__d3_from(m->normal));
    s->normal = de__v3((f32)nw.x, (f32)nw.y, (f32)nw.z);
    s->point_count = m->count;
    s->child_a = a->child;
    s->child_b = b->child;
    s->sub_a = sub_a;
    s->sub_b = DE__SUB_NONE;
    for (u32 i = 0; i < m->count; ++i) {
        de__contact_point *c = &s->points[i];
        c->local_a = to_top(a, m->pts[i].pa);
        c->local_b = to_top(b, de__np_to_b(rel, m->pts[i].pb));
        c->separation = m->pts[i].sep;
        c->feature_id = m->pts[i].feature;
    }
    if (swapped) {
        s->normal = de__v3(-s->normal.x, -s->normal.y, -s->normal.z);
        u32 t = s->child_a;
        s->child_a = s->child_b;
        s->child_b = t;
        t = s->sub_a;
        s->sub_a = s->sub_b;
        s->sub_b = t;
        for (u32 i = 0; i < s->point_count; ++i) {
            de__contact_point *c = &s->points[i];
            const de_vec3 la = c->local_a;
            c->local_a = c->local_b;
            c->local_b = la;
            c->feature_id = de__feature_flip(c->feature_id);
        }
    }
    ctx->added++;
}

/* ---- Recursion ------------------------------------------------------------------------- */

static de_quat quat_mul_d(de_quat a, de_quat b) {
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

typedef struct child_iter {
    de__np_ctx *ctx;
    const de__np_side *comp;
    const de__np_side *other;
    bool comp_is_a;
    bool swapped;
} child_iter;

static void child_cb(void *user, u32 i) {
    const child_iter *it = (const child_iter *)user;
    const de__compound_child *c = &it->comp->shape->u.compound.children[i];
    const de__dmat3 w = de__dmat3_from_quat(it->comp->pose.rotation);
    de__np_side side;
    memset(&side, 0, sizeof side);
    side.shape = c->shape;
    side.pose.position =
        de__d3_add(it->comp->pose.position, de__dmat3_mul(&w, de__d3_from(c->position)));
    side.pose.rotation = quat_mul_d(it->comp->pose.rotation, c->rotation);
    side.has_off = true;
    side.off_pos = c->position;
    side.off_rot = c->rotation;
    side.child = i;
    if (it->comp_is_a)
        de__np_collide_sides(it->ctx, &side, it->other, it->swapped);
    else
        de__np_collide_sides(it->ctx, it->other, &side, it->swapped);
}

static bool is_terrain(de_shape_kind k) {
    return k == DE_SHAPE_MESH || k == DE_SHAPE_HEIGHTFIELD;
}

void de__np_collide_sides(de__np_ctx *ctx, const de__np_side *a, const de__np_side *b,
                          bool swapped) {
    const bool ca = a->shape->kind == DE_SHAPE_COMPOUND;
    if (ca || b->shape->kind == DE_SHAPE_COMPOUND) {
        /* A's children first when both are compounds. */
        child_iter it = {ctx, ca ? a : b, ca ? b : a, ca, swapped};
        de__np_rel rel;
        de__np_rel_init(&rel, it.comp->pose, it.other->pose, ctx->spec);
        const de_aabb box = de__np_box_in_a(&rel, it.other->shape->local_aabb, ctx->spec);
        de__np_compound_children(it.comp->shape, box, child_cb, &it);
        return;
    }
    if (a->shape->kind < b->shape->kind) {
        const de__np_side *t = a;
        a = b;
        b = t;
        swapped = !swapped;
    }
    if (is_terrain(b->shape->kind))
        return; /* mesh / heightfield against mesh / heightfield: both static */
    de__np_rel rel;
    de__np_rel_init(&rel, a->pose, b->pose, ctx->spec);
    if (is_terrain(a->shape->kind)) {
        de__np_midphase(ctx, a, b, &rel, swapped);
        return;
    }
    const de__np_pair_fn fn = de__np_pair_function(a->shape->kind, b->shape->kind);
    if (fn == NULL)
        return;
    de__np_manifold m;
    m.count = 0;
    if (fn(a->shape, b->shape, &rel, &m))
        de__np_emit(ctx, &m, a, b, &rel, DE__SUB_NONE, swapped);
}

/* ---- Canonical order ------------------------------------------------------------------- */

/* -1 when x sorts first, 1 when y does, 0 when equal (NaN compares equal). */
static int cmp_f64(f64 x, f64 y) {
    return x < y ? -1 : (y < x ? 1 : 0);
}

/* true when (b, xb) must be A: the higher kind is A, then the lower shape id, then the pose. */
static bool must_swap(const de__shape *a, de__pose xa, const de__shape *b, de__pose xb) {
    if (a->kind != b->kind)
        return a->kind < b->kind;
    if (a->handle.id != b->handle.id)
        return a->handle.id > b->handle.id;
    const f64 ka[7] = {xa.position.x, xa.position.y, xa.position.z, xa.rotation.x,
                       xa.rotation.y, xa.rotation.z, xa.rotation.w};
    const f64 kb[7] = {xb.position.x, xb.position.y, xb.position.z, xb.rotation.x,
                       xb.rotation.y, xb.rotation.z, xb.rotation.w};
    for (u32 i = 0; i < 7; ++i) {
        const int c = cmp_f64(ka[i], kb[i]);
        if (c != 0)
            return c > 0;
    }
    return false;
}

u32 de__collide(const de__shape *a, de__pose xa, const de__shape *b, de__pose xb, f32 speculative,
                de__contact_buffer *out) {
    if (a == NULL || b == NULL || out == NULL)
        return 0;
    de__np_ctx ctx;
    ctx.out = out;
    ctx.spec = speculative > 0.0f ? speculative : 0.0f;
    ctx.added = 0;
    de__np_side sa, sb;
    memset(&sa, 0, sizeof sa);
    memset(&sb, 0, sizeof sb);
    sa.shape = a;
    sa.pose = xa;
    sa.child = DE__SUB_NONE;
    sb.shape = b;
    sb.pose = xb;
    sb.child = DE__SUB_NONE;
    if (must_swap(a, xa, b, xb))
        de__np_collide_sides(&ctx, &sb, &sa, true);
    else
        de__np_collide_sides(&ctx, &sa, &sb, false);
    return ctx.added;
}
