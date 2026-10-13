/*
 * ragdoll.c: ragdolls built from joints (DENG-224; SPEC §13.4, §13.2, §5.7). See the Ragdolls
 * block of de_physics.h and ragdoll_internal.h.
 *
 * Frames. A bone's body sits at its shape: body = bone * offset (offset = shape_offset). The joint
 * of bone i is at the bone origin with rotation bind_i * joint_rotation_i. Its frame on each body
 * is computed from the model-space bind poses (never from f64 world positions, which may be
 * 100 km out), so the joint coordinates are exactly zero in the bind pose and limits and motor
 * targets are relative to it.
 *
 * Motor targets. For a target rotation L of bone i relative to its parent, the joint's relative
 * rotation is conj(frame_parent) * L * frame_bone; its swing-twist split (the joints' own,
 * de__joint_swing_twist) gives the coordinates the position motors drive: the hinge angle (axis
 * 0), or the twist and the two swing components (axes 3, 4, 5 of the 6-DOF motor joint).
 *
 * Determinism (SPEC §5.7): + - * /, de_dsqrt, de_dnormalize_quat (through the joints' helpers)
 * only; everything in bone order.
 */
#include "physics/ragdoll_internal.h"

#include "physics/joints/joint_internal.h"

/* ================================ Transforms (f64 positions) ============================= */

static de_quat rd_quat(de_quat q) {
    if (q.x == 0.0f && q.y == 0.0f && q.z == 0.0f && q.w == 0.0f)
        return de__jq(0.0f, 0.0f, 0.0f, 1.0f);
    return de_dnormalize_quat(q);
}

static bool rd_quat_finite(de_quat q) {
    return de__f32_finite(q.x) && de__f32_finite(q.y) && de__f32_finite(q.z) && de__f32_finite(q.w);
}

static bool rd_xform_finite(const de_transform *t) {
    return de__d3_finite(t->position) && rd_quat_finite(t->rotation);
}

static de_transform rd_xform(de_dvec3 pos, de_quat rot) {
    de_transform t;
    memset(&t, 0, sizeof t);
    t.position = pos;
    t.rotation = rot;
    t.scale = de__v3(1.0f, 1.0f, 1.0f);
    return t;
}

/* a * b (apply b, then a). Rotations normalized. */
static de_transform rd_mul(const de_transform *a, const de_transform *b) {
    const de__dmat3 m = de__dmat3_from_quat(a->rotation);
    return rd_xform(de__d3_add(a->position, de__dmat3_mul(&m, b->position)),
                    de_dnormalize_quat(de__jq_mul(a->rotation, b->rotation)));
}

/* a^-1 * b. */
static de_transform rd_inv_mul(const de_transform *a, const de_transform *b) {
    const de__dmat3 m = de__dmat3_from_quat(a->rotation);
    return rd_xform(de__dmat3_tmul(&m, de__d3_sub(b->position, a->position)),
                    de_dnormalize_quat(de__jq_mul(de__jq_conj(a->rotation), b->rotation)));
}

/* A user transform with its rotation normalized (zero = identity) and scale 1. */
static de_transform rd_clean(const de_transform *t) {
    return rd_xform(t->position, rd_quat(t->rotation));
}

/* ================================ Pool ================================================== */

static void rd_free_bones(const de_allocator *a, de__ragdoll *r) {
    if (r->bones != NULL)
        DE_FREE(a, r->bones, sizeof(de__ragdoll_bone) * (size_t)r->bone_count);
    r->bones = NULL;
}

static void rd_pool_destroy(de_physics_world *p, void *state) {
    (void)p;
    de__ragdolls *rs = (de__ragdolls *)state;
    const de_allocator *a = rs->alloc;
    for (u32 i = 0; i < rs->high_water; ++i)
        if (rs->items[i].alive)
            rd_free_bones(a, &rs->items[i]); /* bodies, joints and shapes go with the world */
    if (rs->items != NULL)
        DE_FREE(a, rs->items, sizeof(de__ragdoll) * (size_t)rs->capacity);
    if (rs->free_list != NULL)
        DE_FREE(a, rs->free_list, sizeof(u32) * (size_t)rs->capacity);
    DE_FREE(a, rs, sizeof *rs);
}

static const de__phys_ext_vtbl k_ragdoll_vtbl = {rd_pool_destroy, NULL, NULL};

static de__ragdolls *rd_pool(de_physics_world *p) {
    de__ragdolls *rs = de__ragdolls_get(p);
    if (rs != NULL)
        return rs;
    rs = (de__ragdolls *)DE_ALLOC(p->alloc, sizeof *rs, DE_ALIGNOF(de__ragdolls), DE_MEM_PHYSICS);
    if (rs == NULL)
        return NULL;
    memset(rs, 0, sizeof *rs);
    rs->alloc = p->alloc;
    de__phys_ext_set(p, DE__PHYS_EXT_RAGDOLL, rs, &k_ragdoll_vtbl);
    return rs;
}

/* A free slot index (growing the pool by doubling); false when out of memory or handles. */
static bool rd_slot(de__ragdolls *rs, u32 *out) {
    if (rs->free_count > 0u) {
        *out = rs->free_list[--rs->free_count];
        return true;
    }
    if (rs->high_water == rs->capacity) {
        const u32 cap = rs->capacity == 0u ? 8u : rs->capacity * 2u;
        if (cap > DE__HANDLE_INDEX_MASK - 1u)
            return false;
        const de_allocator *a = rs->alloc;
        de__ragdoll *items = (de__ragdoll *)DE_ALLOC(a, sizeof(de__ragdoll) * (size_t)cap,
                                                     DE_ALIGNOF(de__ragdoll), DE_MEM_PHYSICS);
        u32 *fl = (u32 *)DE_ALLOC(a, sizeof(u32) * (size_t)cap, DE_ALIGNOF(u32), DE_MEM_PHYSICS);
        if (items == NULL || fl == NULL) {
            if (items != NULL)
                DE_FREE(a, items, sizeof(de__ragdoll) * (size_t)cap);
            if (fl != NULL)
                DE_FREE(a, fl, sizeof(u32) * (size_t)cap);
            return false;
        }
        memset(items, 0, sizeof(de__ragdoll) * (size_t)cap);
        for (u32 i = 0; i < cap; ++i)
            items[i].generation = 1u;
        if (rs->items != NULL) {
            memcpy(items, rs->items, sizeof(de__ragdoll) * (size_t)rs->capacity);
            memcpy(fl, rs->free_list, sizeof(u32) * (size_t)rs->capacity);
            DE_FREE(a, rs->items, sizeof(de__ragdoll) * (size_t)rs->capacity);
            DE_FREE(a, rs->free_list, sizeof(u32) * (size_t)rs->capacity);
        }
        rs->items = items;
        rs->free_list = fl;
        rs->capacity = cap;
    }
    *out = rs->high_water++;
    return true;
}

de__ragdoll *de__ragdoll_resolve(const de_physics_world *p, de_ragdoll r) {
    if (p == NULL || r.id == 0u)
        return NULL;
    const de__ragdolls *rs = de__ragdolls_get(p);
    if (rs == NULL)
        return NULL;
    const u32 i = de__handle_index(r.id);
    if (i >= rs->high_water || !rs->items[i].alive ||
        rs->items[i].generation != de__handle_gen(r.id))
        return NULL;
    return &rs->items[i];
}

/* ================================ Validation ============================================ */

/* The code of the error a failed body, shape or joint call just set. */
static de_result rd_last_error(void) {
    const de_error *e = de_error_last();
    return (e != NULL && e->result != DE_OK) ? e->result : DE_ERR_INVALID_ARGUMENT;
}

static bool rd_nonneg(f32 v) {
    return de__f32_finite(v) && v >= 0.0f;
}

static de_result rd_validate(const de_ragdoll_desc *d) {
    if (d->bones == NULL || d->bone_count == 0u)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_ragdoll_create: no bones");
    if (d->bone_count > DE_RAGDOLL_MAX_BONES)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_ragdoll_create: %u bones (most %u)",
                       d->bone_count, DE_RAGDOLL_MAX_BONES);
    if (!rd_xform_finite(&d->transform) || !de__v3_finite(d->linear_velocity) ||
        !de__v3_finite(d->angular_velocity) || !rd_nonneg(d->linear_damping) ||
        !rd_nonneg(d->angular_damping) || !rd_nonneg(d->motor_hertz) ||
        !rd_nonneg(d->motor_damping_ratio) || !rd_nonneg(d->material.friction) ||
        !rd_nonneg(d->material.restitution) || !rd_nonneg(d->material.density))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_ragdoll_create: non-finite or negative transform, velocity, damping, "
                       "motor or material");
    for (u32 i = 0; i < d->bone_count; ++i) {
        const de_ragdoll_bone *b = &d->bones[i];
        if (i == 0u ? b->parent != DE_RAGDOLL_NO_PARENT : b->parent >= i)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "de_ragdoll_create: bone %u: parent %u (bone 0 is the root, every "
                           "other bone needs a lower index)",
                           i, b->parent);
        if (!rd_xform_finite(&b->bind) || !rd_xform_finite(&b->shape_offset) ||
            !rd_quat_finite(b->joint_rotation) || !rd_nonneg(b->mass) ||
            !rd_nonneg(b->motor_max_force))
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "de_ragdoll_create: bone %u: non-finite pose or negative mass or "
                           "motor force",
                           i);
        bool shape_ok;
        switch (b->shape) {
        case DE_RAGDOLL_SHAPE_CAPSULE:
            shape_ok = rd_nonneg(b->radius) && b->radius > 0.0f && rd_nonneg(b->half_height);
            break;
        case DE_RAGDOLL_SHAPE_BOX:
            shape_ok = de__v3_finite(b->half_extents) && b->half_extents.x > 0.0f &&
                       b->half_extents.y > 0.0f && b->half_extents.z > 0.0f;
            break;
        case DE_RAGDOLL_SHAPE_SPHERE:
            shape_ok = rd_nonneg(b->radius) && b->radius > 0.0f;
            break;
        default:
            shape_ok = false;
            break;
        }
        if (!shape_ok)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "de_ragdoll_create: bone %u: unknown shape or a size that is not > 0",
                           i);
        if (i == 0u)
            continue;
        if ((u32)b->joint > (u32)DE_RAGDOLL_JOINT_HINGE || !de__f32_finite(b->limit_min) ||
            !de__f32_finite(b->limit_max) || b->limit_min > b->limit_max ||
            !rd_nonneg(b->swing_y) || !rd_nonneg(b->swing_z))
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "de_ragdoll_create: bone %u: unknown joint kind, limit_min > "
                           "limit_max, or a negative swing",
                           i);
    }
    return DE_OK;
}

/* ================================ Create / destroy ====================================== */

static de_shape rd_shape(de_physics_world *p, const de_ragdoll_bone *b) {
    de_shape_desc sd;
    memset(&sd, 0, sizeof sd);
    switch (b->shape) {
    case DE_RAGDOLL_SHAPE_BOX:
        sd.kind = DE_SHAPE_BOX;
        sd.half_extents = b->half_extents;
        break;
    case DE_RAGDOLL_SHAPE_SPHERE:
        sd.kind = DE_SHAPE_SPHERE;
        sd.radius = b->radius;
        break;
    default:
        sd.kind = DE_SHAPE_CAPSULE;
        sd.radius = b->radius;
        sd.half_height = b->half_height;
        break;
    }
    return de_shape_create(p, &sd);
}

/* Releases what bones[0..bone_count) hold, in reverse creation order. */
static void rd_release(de_physics_world *p, de__ragdoll *r) {
    for (u32 i = r->bone_count; i-- > 0;) {
        de_joint_destroy(p, r->bones[i].motor);
        de_joint_destroy(p, r->bones[i].joint);
    }
    for (u32 i = r->bone_count; i-- > 0;)
        de_body_destroy(p, r->bones[i].body);
    for (u32 i = r->bone_count; i-- > 0;)
        if (DE_HANDLE_VALID(r->bones[i].shape))
            (void)de_shape_destroy(p, r->bones[i].shape);
}

static void rd_free_slot(de__ragdolls *rs, u32 index) {
    de__ragdoll *r = &rs->items[index];
    rd_free_bones(rs->alloc, r);
    r->alive = 0;
    r->bone_count = 0;
    r->generation = de__gen_next(r->generation);
    rs->free_list[rs->free_count++] = index;
}

/* The collision filter of bone b. */
static de_collision_filter rd_filter(const de_ragdoll_desc *d, const de_ragdoll_bone *b,
                                     u32 group) {
    de_collision_filter f = d->filter;
    if (f.layer == 0u && f.mask == 0u) {
        f.layer = DE_COLLISION_FILTER_DEFAULT_LAYER;
        f.mask = DE_COLLISION_FILTER_DEFAULT_MASK;
    }
    f.group = group;
    if (b->filter.layer != 0u || b->filter.mask != 0u) {
        f.layer = b->filter.layer;
        f.mask = b->filter.mask;
    }
    if (b->filter.group != 0u)
        f.group = b->filter.group;
    return f;
}

/* Center of mass of a body (world, f64). */
static de_dvec3 rd_com(const de_physics_world *p, u32 body) {
    const de__dmat3 m = de__dmat3_from_quat(p->bodies.rotation[body]);
    return de__d3_add(p->bodies.position[body],
                      de__dmat3_mul(&m, de__d3_from(p->bodies.local_com[body])));
}

/* Sets the rigid-body velocity (linear at the root's center of mass, angular) on every bone.
 * Every bone must be live. */
static void rd_apply_velocity(de_physics_world *p, const de__ragdoll *r, de_vec3 v, de_vec3 w) {
    u32 root;
    if (!de__body_resolve(p, r->bones[0].body, &root))
        return;
    const de_dvec3 c0 = rd_com(p, root);
    for (u32 i = 0; i < r->bone_count; ++i) {
        u32 bi;
        if (!de__body_resolve(p, r->bones[i].body, &bi))
            continue;
        const de_dvec3 dc = de__d3_sub(rd_com(p, bi), c0);
        const de_vec3 rel = de__v3((f32)dc.x, (f32)dc.y, (f32)dc.z);
        (void)de_body_set_velocity(p, r->bones[i].body, de__v3_add(v, de__v3_cross(w, rel)), w);
    }
}

/* Joint coordinates (twist, swing y, swing z) of target `local` for bone b. */
static void rd_target_coords(const de__ragdoll_bone *b, de_quat local, f32 *twist, f32 *sy,
                             f32 *sz) {
    de_quat q = de__jq_mul(de__jq_conj(b->frame_parent), de__jq_mul(local, b->frame_bone));
    q = de_dnormalize_quat(q);
    if (q.w < 0.0f)
        q = de__jq(-q.x, -q.y, -q.z, -q.w);
    f32 swing;
    de__joint_swing_twist(q, twist, sy, sz, &swing);
}

/* Builds the joints of bone i (bones and bodies exist). */
static de_result rd_joints(de_physics_world *p, const de_ragdoll_desc *d, de__ragdoll *r, u32 i,
                           const de_transform *body_model) {
    const de_ragdoll_bone *src = &d->bones[i];
    de__ragdoll_bone *b = &r->bones[i];
    const u32 parent = src->parent;
    /* The joint frame in model space: the bone origin, rotated by bind * joint_rotation. */
    const de_transform bind = rd_clean(&src->bind);
    const de_transform jr = rd_xform(de__d3(0.0, 0.0, 0.0), b->frame_bone);
    const de_transform joint_model = rd_mul(&bind, &jr);
    const de_transform parent_bind = rd_clean(&d->bones[parent].bind);
    b->frame_parent =
        de_dnormalize_quat(de__jq_mul(de__jq_conj(parent_bind.rotation), joint_model.rotation));

    de_joint_desc jd;
    memset(&jd, 0, sizeof jd);
    jd.body_a = r->bones[parent].body;
    jd.body_b = b->body;
    jd.frame_a = rd_inv_mul(&body_model[parent], &joint_model);
    jd.frame_b = rd_inv_mul(&body_model[i], &joint_model);
    jd.motor_hertz = d->motor_hertz;
    jd.motor_damping_ratio = d->motor_damping_ratio;
    if (src->joint == DE_RAGDOLL_JOINT_HINGE) {
        jd.kind = DE_JOINT_HINGE;
        jd.enable_limit = true;
        jd.limits_min[0] = src->limit_min;
        jd.limits_max[0] = src->limit_max;
        if (b->powered) {
            jd.motor_mode[0] = DE_JOINT_MOTOR_POSITION;
            jd.motor_max_force[0] = src->motor_max_force;
        }
    } else {
        jd.kind = DE_JOINT_CONE_TWIST;
        jd.limits_min[3] = src->limit_min;
        jd.limits_max[3] = src->limit_max;
        jd.limits_max[4] = src->swing_y;
        jd.limits_max[5] = src->swing_z;
    }
    b->joint = de_joint_create(p, &jd);
    if (!DE_HANDLE_VALID(b->joint))
        return rd_last_error();
    if (b->powered && src->joint != DE_RAGDOLL_JOINT_HINGE) {
        /* The motor joint: same frames, every axis free, position motors on the angular axes. */
        de_joint_desc md;
        memset(&md, 0, sizeof md);
        md.kind = DE_JOINT_6DOF;
        md.body_a = jd.body_a;
        md.body_b = jd.body_b;
        md.frame_a = jd.frame_a;
        md.frame_b = jd.frame_b;
        md.motor_hertz = d->motor_hertz;
        md.motor_damping_ratio = d->motor_damping_ratio;
        for (u32 k = 0; k < 6; ++k)
            md.axis_mode[k] = DE_JOINT_AXIS_FREE;
        for (u32 k = 3; k < 6; ++k) {
            md.motor_mode[k] = DE_JOINT_MOTOR_POSITION;
            md.motor_max_force[k] = src->motor_max_force;
        }
        b->motor = de_joint_create(p, &md);
        if (!DE_HANDLE_VALID(b->motor))
            return rd_last_error();
    }
    return DE_OK;
}

de_ragdoll de_ragdoll_create(de_physics_world *p, const de_ragdoll_desc *desc) {
    const de_ragdoll none = {0};
    if (p == NULL || desc == NULL) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_ragdoll_create: NULL world or desc");
        return none;
    }
    if (rd_validate(desc) != DE_OK)
        return none;
    de__ragdolls *rs = rd_pool(p);
    u32 index;
    if (rs == NULL || !rd_slot(rs, &index)) {
        DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_ragdoll_create: ragdoll pool");
        return none;
    }
    de__ragdoll *r = &rs->items[index];
    const u32 n = desc->bone_count;
    r->bones = (de__ragdoll_bone *)DE_ALLOC(rs->alloc, sizeof(de__ragdoll_bone) * (size_t)n,
                                            DE_ALIGNOF(de__ragdoll_bone), DE_MEM_PHYSICS);
    /* A transform per bone on the stack: the body poses in model space. */
    de_transform body_model[DE_RAGDOLL_MAX_BONES];
    if (r->bones == NULL) {
        rs->free_list[rs->free_count++] = index;
        DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_ragdoll_create: %u bones", n);
        return none;
    }
    memset(r->bones, 0, sizeof(de__ragdoll_bone) * (size_t)n);
    r->bone_count = n;
    r->alive = 1u;
    if (desc->self_collision)
        r->group = desc->filter.group;
    else
        r->group = desc->filter.group != 0u ? desc->filter.group
                                            : (DE_RAGDOLL_AUTO_GROUP_BIT | (index + 1u));

    for (u32 i = 0; i < n; ++i) {
        de__ragdoll_bone *b = &r->bones[i];
        const de_ragdoll_bone *src = &desc->bones[i];
        b->parent = src->parent;
        b->kind = (u8)src->joint;
        b->powered = (i > 0u && src->motor_max_force > 0.0f) ? 1u : 0u;
        b->offset = rd_clean(&src->shape_offset);
        const de_transform id = rd_xform(de__d3(0.0, 0.0, 0.0), de__jq(0.0f, 0.0f, 0.0f, 1.0f));
        b->inv_off = rd_inv_mul(&b->offset, &id);
        b->frame_bone = rd_quat(src->joint_rotation);
        b->frame_parent = de__jq(0.0f, 0.0f, 0.0f, 1.0f);
        b->target = de__jq(0.0f, 0.0f, 0.0f, 1.0f);
        const de_transform bind = rd_clean(&src->bind);
        body_model[i] = rd_mul(&bind, &b->offset);
        if (i > 0u) {
            /* The bind pose relative to the parent: the motors' initial target. */
            const de_transform pb = rd_clean(&desc->bones[src->parent].bind);
            b->target = de_dnormalize_quat(de__jq_mul(de__jq_conj(pb.rotation), bind.rotation));
        }
    }

    de_result res = DE_OK;
    /* Shapes, then bodies, in bone order. */
    for (u32 i = 0; i < n && res == DE_OK; ++i) {
        r->bones[i].shape = rd_shape(p, &desc->bones[i]);
        if (!DE_HANDLE_VALID(r->bones[i].shape))
            res = rd_last_error();
    }
    const de_transform model = rd_clean(&desc->transform);
    for (u32 i = 0; i < n && res == DE_OK; ++i) {
        de_body_desc bd;
        memset(&bd, 0, sizeof bd);
        bd.type = DE_BODY_DYNAMIC;
        bd.transform = rd_mul(&model, &body_model[i]);
        bd.shape = r->bones[i].shape;
        bd.mass = desc->bones[i].mass;
        bd.material = desc->material;
        bd.filter = rd_filter(desc, &desc->bones[i], r->group);
        bd.linear_damping = desc->linear_damping;
        bd.angular_damping = desc->angular_damping;
        bd.gravity_scale = 1.0f;
        bd.flags = desc->flags;
        bd.owner = desc->owner;
        r->bones[i].body = de_body_create(p, &bd);
        if (!DE_HANDLE_VALID(r->bones[i].body))
            res = rd_last_error();
    }
    /* Joints in bone order. */
    for (u32 i = 1; i < n && res == DE_OK; ++i)
        res = rd_joints(p, desc, r, i, body_model);
    if (res != DE_OK) {
        rd_release(p, r);
        rd_free_slot(rs, index);
        DE_FAIL_WRAP(res, "de_ragdoll_create: building the bone bodies and joints failed");
        return none;
    }
    rd_apply_velocity(p, r, desc->linear_velocity, desc->angular_velocity);
    ++rs->live_count;
    de_ragdoll h;
    h.id = de__handle_make(index, r->generation);
    return h;
}

void de_ragdoll_destroy(de_physics_world *p, de_ragdoll h) {
    de__ragdoll *r = de__ragdoll_resolve(p, h);
    if (r == NULL)
        return;
    de__ragdolls *rs = de__ragdolls_get(p);
    rd_release(p, r);
    rd_free_slot(rs, de__handle_index(h.id));
    --rs->live_count;
}

/* ================================ Queries and setters =================================== */

bool de_ragdoll_valid(const de_physics_world *p, de_ragdoll r) {
    return de__ragdoll_resolve(p, r) != NULL;
}

u32 de_ragdoll_bone_count(const de_physics_world *p, de_ragdoll h) {
    const de__ragdoll *r = de__ragdoll_resolve(p, h);
    return r != NULL ? r->bone_count : 0u;
}

de_body de_ragdoll_bone_body(const de_physics_world *p, de_ragdoll h, u32 bone) {
    const de__ragdoll *r = de__ragdoll_resolve(p, h);
    const de_body none = {0};
    return (r != NULL && bone < r->bone_count) ? r->bones[bone].body : none;
}

de_joint de_ragdoll_bone_joint(const de_physics_world *p, de_ragdoll h, u32 bone) {
    const de__ragdoll *r = de__ragdoll_resolve(p, h);
    const de_joint none = {0};
    return (r != NULL && bone < r->bone_count) ? r->bones[bone].joint : none;
}

bool de_ragdoll_get_pose(const de_physics_world *p, de_ragdoll h, de_transform *out, u32 count) {
    const de__ragdoll *r = de__ragdoll_resolve(p, h);
    if (r == NULL || out == NULL || count != r->bone_count)
        return false;
    for (u32 i = 0; i < r->bone_count; ++i) {
        de_transform body;
        if (!de_body_get_transform(p, r->bones[i].body, &body))
            return false;
        out[i] = rd_mul(&body, &r->bones[i].inv_off);
    }
    return true;
}

bool de_ragdoll_set_pose(de_physics_world *p, de_ragdoll h, const de_transform *bones, u32 count) {
    const de__ragdoll *r = de__ragdoll_resolve(p, h);
    if (r == NULL || bones == NULL || count != r->bone_count)
        return false;
    for (u32 i = 0; i < count; ++i)
        if (!rd_xform_finite(&bones[i]) || !de_body_valid(p, r->bones[i].body))
            return false;
    for (u32 i = 0; i < count; ++i) {
        const de_transform bone = rd_clean(&bones[i]);
        const de_transform body = rd_mul(&bone, &r->bones[i].offset);
        (void)de_body_set_transform(p, r->bones[i].body, &body);
    }
    return true;
}

bool de_ragdoll_set_velocity(de_physics_world *p, de_ragdoll h, de_vec3 linear, de_vec3 angular) {
    const de__ragdoll *r = de__ragdoll_resolve(p, h);
    if (r == NULL || !de__v3_finite(linear) || !de__v3_finite(angular))
        return false;
    rd_apply_velocity(p, r, linear, angular);
    return true;
}

bool de_ragdoll_set_motor_targets(de_physics_world *p, de_ragdoll h, const de_quat *local,
                                  u32 count) {
    de__ragdoll *r = de__ragdoll_resolve(p, h);
    if (r == NULL || local == NULL || count != r->bone_count)
        return false;
    for (u32 i = 0; i < count; ++i)
        if (!rd_quat_finite(local[i]))
            return false;
    for (u32 i = 1; i < count; ++i) {
        de__ragdoll_bone *b = &r->bones[i];
        if (!b->powered)
            continue;
        b->target = rd_quat(local[i]);
        f32 twist, sy, sz;
        rd_target_coords(b, b->target, &twist, &sy, &sz);
        if (b->kind == (u8)DE_RAGDOLL_JOINT_HINGE) {
            (void)de_joint_set_motor_target(p, b->joint, 0u, twist);
        } else {
            (void)de_joint_set_motor_target(p, b->motor, 3u, twist);
            (void)de_joint_set_motor_target(p, b->motor, 4u, sy);
            (void)de_joint_set_motor_target(p, b->motor, 5u, sz);
        }
    }
    return true;
}
