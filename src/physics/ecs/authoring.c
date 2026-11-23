/*
 * authoring.c: the editor-authored physics components (DENG-766, see "Physics authoring" in
 * de_physics.h). Builds DeCollider base shapes from shape_kind, and keeps one joint per
 * DeHingeJoint / DeConeTwistJoint / DeFixedJoint and one ragdoll per DeRagdoll.
 *
 * State: s->auth, sorted by (entity index, kind): the handle made for each component and the key
 * (hash of the fields, and the body ids or asset stamp) it was made from. Each tick of
 * physics.step, after the body sync: removals in list (entity index) order, then the four
 * queries in query order (a changed key re-creates), then the handle write-back in list order.
 * Failures keep their key, so they are not retried until a field (or a body) changes.
 */
#include "physics/ecs/phys_ecs.h"

enum { AUTH_HINGE = 1, AUTH_CONE = 2, AUTH_FIXED = 3, AUTH_RAGDOLL = 4 };

typedef struct de__pecs_auth {
    de_entity entity;
    u32 handle; /**< de_joint.id or de_ragdoll.id; 0 = none */
    u8 kind;    /**< AUTH_* */
    u8 pad_[3];
    u64 key;
} de__pecs_auth;

/* ================================ Base shapes ========================================== */

de_shape de__pecs_make_base_shape(de__pecs *s, de_entity e, const DeCollider *col) {
    de_shape_desc sd;
    memset(&sd, 0, sizeof sd);
    switch (col->shape_kind) {
    case DE_COLLIDER_SHAPE_SPHERE:
        sd.kind = DE_SHAPE_SPHERE;
        sd.radius = col->radius;
        break;
    case DE_COLLIDER_SHAPE_CAPSULE:
        sd.kind = DE_SHAPE_CAPSULE;
        sd.radius = col->radius;
        sd.half_height = col->half_height;
        break;
    case DE_COLLIDER_SHAPE_BOX:
        sd.kind = DE_SHAPE_BOX;
        sd.half_extents = col->half_extents;
        break;
    case DE_COLLIDER_SHAPE_CONVEX: {
        const DeConvexHull *h =
            DE_HANDLE_VALID(s->c_hull)
                ? (const DeConvexHull *)de_entity_get_const(s->w, e, s->c_hull)
                : NULL;
        if (h == NULL) {
            DE_FAIL(DE_ERR_INVALID_ARGUMENT, "DE_COLLIDER_SHAPE_CONVEX needs a DeConvexHull");
            return (de_shape){0};
        }
        if (h->point_count > DE_CONVEX_HULL_MAX_POINTS) {
            DE_FAIL(DE_ERR_INVALID_ARGUMENT, "DeConvexHull.point_count %u > %u", h->point_count,
                    DE_CONVEX_HULL_MAX_POINTS);
            return (de_shape){0};
        }
        sd.kind = DE_SHAPE_CONVEX;
        sd.points = h->points;
        sd.point_count = h->point_count;
        sd.hull_tolerance = h->tolerance;
        break;
    }
    default:
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "unknown DeCollider.shape_kind %d", (int)col->shape_kind);
        return (de_shape){0};
    }
    return de_shape_create(s->p, &sd);
}

/* ================================ List ================================================= */

void de__pecs_authoring_free(de__pecs *s) {
    if (s->auth != NULL)
        DE_FREE(s->alloc, s->auth, sizeof(de__pecs_auth) * (size_t)s->auth_cap);
    s->auth = NULL;
    s->auth_count = s->auth_cap = 0;
}

static bool auth_less(de_entity e, u8 kind, const de__pecs_auth *a) {
    const u32 i = de_entity_index(e), j = de_entity_index(a->entity);
    return i < j || (i == j && kind < a->kind);
}

/* Index of the first entry not less than (e, kind). */
static u32 auth_lower(const de__pecs *s, de_entity e, u8 kind) {
    u32 lo = 0, hi = s->auth_count;
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2u;
        if (auth_less(s->auth[mid].entity, s->auth[mid].kind, &(de__pecs_auth){e, 0, kind, {0}, 0}))
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo;
}

static void destroy_handle(de__pecs *s, const de__pecs_auth *a) {
    if (a->handle == 0u)
        return;
    if (a->kind == AUTH_RAGDOLL)
        de_ragdoll_destroy(s->p, (de_ragdoll){a->handle});
    else
        de_joint_destroy(s->p, (de_joint){a->handle});
}

static void warn_auth(de__pecs *s, de_entity e, const char *what) {
    if (s->auth_warnings < 32u) {
        const de_error *err = de_error_last();
        DE_WARN(de__physics_log(), "physics.step: entity %u:%u gets no %s: %s%s",
                de_entity_index(e), de_entity_generation(e), what,
                err != NULL && err->result != DE_OK ? err->message : "invalid components",
                s->auth_warnings + 1u == 32u ? " (further failures are not logged)" : "");
    }
    ++s->auth_warnings;
    ++s->stats.failed;
    de_error_clear();
}

static de_component comp_of(const de__pecs *s, u8 kind) {
    switch (kind) {
    case AUTH_HINGE:
        return s->c_hinge;
    case AUTH_CONE:
        return s->c_cone;
    case AUTH_FIXED:
        return s->c_fixed;
    default:
        return s->c_ragdoll;
    }
}

/* ================================ Keys ================================================= */

typedef struct key_acc {
    u64 h;
} key_acc;

static void kput(key_acc *k, const void *v, size_t n) {
    k->h = de_hash64(v, n, k->h);
}

static void kput_xform(key_acc *k, const de_transform *t) {
    kput(k, &t->position, sizeof t->position);
    kput(k, &t->rotation, sizeof t->rotation);
}

static void kput_bool(key_acc *k, bool b) {
    const u8 v = b ? 1u : 0u;
    kput(k, &v, 1);
}

/* The body of `e` (invalid when it has none). */
static de_body body_of(const de__pecs *s, de_entity e) {
    return de_physics_entity_body(s->p, e);
}

/* The shared joint parts: the target, frames, break force, collide_connected and both bodies. */
static void kput_joint(key_acc *k, const de__pecs *s, de_entity e, de_entity target,
                       const de_transform *frame, const de_transform *target_frame,
                       f32 break_force, bool collide) {
    const u32 self_id = body_of(s, e).id;
    const u32 target_id = target.bits != 0u ? body_of(s, target).id : 0u;
    kput(k, &self_id, 4);
    kput(k, &target.bits, sizeof target.bits);
    kput(k, &target_id, 4);
    kput_xform(k, frame);
    kput_xform(k, target_frame);
    kput(k, &break_force, 4);
    kput_bool(k, collide);
}

/* ================================ Create =============================================== */

/* The joint desc parts every kind shares. false: a body is missing (no joint for now). */
static bool joint_base(const de__pecs *s, de_entity e, de_entity target, const de_transform *frame,
                       const de_transform *target_frame, f32 break_force, bool collide,
                       de_joint_desc *d) {
    memset(d, 0, sizeof *d);
    d->a = e;
    d->b = target;
    d->body_a = body_of(s, e);
    if (!DE_HANDLE_VALID(d->body_a))
        return false;
    if (target.bits != 0u) {
        d->body_b = body_of(s, target);
        if (!DE_HANDLE_VALID(d->body_b))
            return false;
    }
    d->frame_a = *frame;
    d->frame_b = *target_frame;
    d->break_force = break_force;
    d->collide_connected = collide;
    return true;
}

/* Key and (when `make`) the joint or ragdoll of component `c` of entity `e`. *out is 0 when
 * nothing could be made; *missing is true when a body is missing (not an error). */
static u64 build(de__pecs *s, u8 kind, de_entity e, const void *c, bool make, u32 *out) {
    key_acc k;
    k.h = 0x766A0000ull + kind;
    *out = 0;
    de_joint_desc d;
    bool have = false;
    switch (kind) {
    case AUTH_HINGE: {
        const DeHingeJoint *j = (const DeHingeJoint *)c;
        kput_joint(&k, s, e, j->target, &j->frame, &j->target_frame, j->break_force,
                   j->collide_connected);
        kput_bool(&k, j->enable_limit);
        kput(&k, &j->limit_min, 4);
        kput(&k, &j->limit_max, 4);
        if (make && (have = joint_base(s, e, j->target, &j->frame, &j->target_frame,
                                       j->break_force, j->collide_connected, &d))) {
            d.kind = DE_JOINT_HINGE;
            d.enable_limit = j->enable_limit;
            d.limits_min[0] = j->limit_min;
            d.limits_max[0] = j->limit_max;
        }
        break;
    }
    case AUTH_CONE: {
        const DeConeTwistJoint *j = (const DeConeTwistJoint *)c;
        kput_joint(&k, s, e, j->target, &j->frame, &j->target_frame, j->break_force,
                   j->collide_connected);
        kput(&k, &j->twist_min, 4);
        kput(&k, &j->twist_max, 4);
        kput(&k, &j->swing_y, 4);
        kput(&k, &j->swing_z, 4);
        if (make && (have = joint_base(s, e, j->target, &j->frame, &j->target_frame,
                                       j->break_force, j->collide_connected, &d))) {
            d.kind = DE_JOINT_CONE_TWIST;
            d.limits_min[3] = j->twist_min;
            d.limits_max[3] = j->twist_max;
            d.limits_max[4] = j->swing_y;
            d.limits_max[5] = j->swing_z;
        }
        break;
    }
    case AUTH_FIXED: {
        const DeFixedJoint *j = (const DeFixedJoint *)c;
        kput_joint(&k, s, e, j->target, &j->frame, &j->target_frame, j->break_force,
                   j->collide_connected);
        if (make && (have = joint_base(s, e, j->target, &j->frame, &j->target_frame,
                                       j->break_force, j->collide_connected, &d)))
            d.kind = DE_JOINT_FIXED;
        break;
    }
    default: {
        const DeRagdoll *r = (const DeRagdoll *)c;
        u32 stamp = 0;
        const de_ragdoll_asset *a = de__phys_ragdoll_asset(s->p, r->asset, &stamp);
        kput(&k, &r->asset.hash, 8);
        kput(&k, &stamp, 4);
        kput(&k, &r->material.friction, 4);
        kput(&k, &r->material.restitution, 4);
        kput(&k, &r->material.density, 4);
        const u32 fc = (u32)r->material.friction_combine, rc = (u32)r->material.restitution_combine;
        kput(&k, &fc, 4);
        kput(&k, &rc, 4);
        kput(&k, &r->filter.layer, 4);
        kput(&k, &r->filter.mask, 4);
        kput(&k, &r->filter.group, 4);
        kput(&k, &r->linear_damping, 4);
        kput(&k, &r->angular_damping, 4);
        if (!make)
            return k.h;
        if (a == NULL) {
            if (r->asset.hash != 0u) {
                DE_FAIL(DE_ERR_NOT_FOUND, "DeRagdoll.asset is not a registered ragdoll asset");
                warn_auth(s, e, "ragdoll");
            }
            return k.h;
        }
        const de_transform *xf = (const de_transform *)de_entity_get_const(s->w, e, s->c_xform);
        de_ragdoll_desc rd;
        memset(&rd, 0, sizeof rd);
        rd.bones = a->bones;
        rd.bone_count = a->bone_count;
        if (xf != NULL) {
            rd.transform = *xf;
        } else {
            rd.transform.rotation.w = 1.0f;
            rd.transform.scale = (de_vec3){1.0f, 1.0f, 1.0f};
        }
        rd.material = r->material;
        rd.filter = r->filter;
        rd.self_collision = a->self_collision;
        rd.linear_damping = r->linear_damping;
        rd.angular_damping = r->angular_damping;
        rd.flags = DE_BODY_FLAG_ALLOW_SLEEP;
        rd.owner = e;
        const de_ragdoll h = de_ragdoll_create(s->p, &rd);
        if (!DE_HANDLE_VALID(h))
            warn_auth(s, e, "ragdoll");
        *out = h.id;
        return k.h;
    }
    }
    if (have) {
        const de_joint j = de_joint_create(s->p, &d);
        if (!DE_HANDLE_VALID(j))
            warn_auth(s, e, "joint");
        *out = j.id;
    }
    return k.h;
}

/* ================================ Passes =============================================== */

typedef struct auth_pass {
    de__pecs *s;
    u8 kind;
    size_t stride;
} auth_pass;

static void auth_chunk(const de_query_chunk *chunk, void *user) {
    const auth_pass *x = (const auth_pass *)user;
    de__pecs *s = x->s;
    const u8 *col = (const u8 *)chunk->columns[0];
    for (u32 r = 0; r < chunk->count; ++r) {
        const de_entity e = chunk->entities[r];
        const void *c = col + x->stride * r;
        u32 handle = 0;
        const u32 at = auth_lower(s, e, x->kind);
        const bool found = at < s->auth_count && s->auth[at].entity.bits == e.bits &&
                           s->auth[at].kind == x->kind;
        if (found) {
            if (build(s, x->kind, e, c, false, &handle) == s->auth[at].key)
                continue;
            destroy_handle(s, &s->auth[at]);
            s->auth[at].handle = 0;
            s->auth[at].key = build(s, x->kind, e, c, true, &handle);
            s->auth[at].handle = handle;
            continue;
        }
        if (!de__pecs_grow(s, (void **)&s->auth, &s->auth_cap, s->auth_count + 1u,
                           sizeof(de__pecs_auth))) {
            warn_auth(s, e, x->kind == AUTH_RAGDOLL ? "ragdoll (out of memory)"
                                                    : "joint (out of memory)");
            continue;
        }
        const u64 key = build(s, x->kind, e, c, true, &handle);
        memmove(&s->auth[at + 1u], &s->auth[at], sizeof(de__pecs_auth) * (s->auth_count - at));
        de__pecs_auth *a = &s->auth[at];
        memset(a, 0, sizeof *a);
        a->entity = e;
        a->kind = x->kind;
        a->handle = handle;
        a->key = key;
        ++s->auth_count;
    }
}

/* The handle field of component `c` (kind `kind`). */
static u32 *handle_field(u8 kind, void *c) {
    switch (kind) {
    case AUTH_HINGE:
        return &((DeHingeJoint *)c)->joint.id;
    case AUTH_CONE:
        return &((DeConeTwistJoint *)c)->joint.id;
    case AUTH_FIXED:
        return &((DeFixedJoint *)c)->joint.id;
    default:
        return &((DeRagdoll *)c)->ragdoll.id;
    }
}

void de__pecs_authoring(de__pecs *s, const de_system_ctx *ctx) {
    /* Removals: the entity or its component went. */
    u32 out = 0;
    for (u32 i = 0; i < s->auth_count; ++i) {
        const de__pecs_auth *a = &s->auth[i];
        if (de_entity_alive(s->w, a->entity) && de_entity_has(s->w, a->entity, comp_of(s, a->kind))) {
            s->auth[out++] = *a;
            continue;
        }
        destroy_handle(s, a);
    }
    s->auth_count = out;

    static const struct {
        u32 query;
        u8 kind;
        size_t stride;
    } k_passes[] = {
        {DE__PECS_Q_ALL_HINGE, AUTH_HINGE, sizeof(DeHingeJoint)},
        {DE__PECS_Q_ALL_CONE, AUTH_CONE, sizeof(DeConeTwistJoint)},
        {DE__PECS_Q_ALL_FIXED, AUTH_FIXED, sizeof(DeFixedJoint)},
        {DE__PECS_Q_ALL_RAGDOLL, AUTH_RAGDOLL, sizeof(DeRagdoll)},
    };
    auth_pass x;
    x.s = s;
    for (u32 k = 0; k < sizeof k_passes / sizeof k_passes[0]; ++k) {
        x.kind = k_passes[k].kind;
        x.stride = k_passes[k].stride;
        de_query_each(s->w, ctx->queries[k_passes[k].query], auth_chunk, &x);
    }

    /* Write the handles back (only where they differ, so nothing is marked changed in vain). */
    for (u32 i = 0; i < s->auth_count; ++i) {
        const de__pecs_auth *a = &s->auth[i];
        const de_component c = comp_of(s, a->kind);
        const void *cur = de_entity_get_const(s->w, a->entity, c);
        if (cur == NULL || *handle_field(a->kind, (void *)(uintptr_t)cur) == a->handle)
            continue;
        void *w = de_entity_get(s->w, a->entity, c);
        if (w != NULL)
            *handle_field(a->kind, w) = a->handle;
    }
}
