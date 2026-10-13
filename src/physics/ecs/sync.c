/*
 * sync.c: ECS -> physics and back (DENG-223, SPEC §13.1, §5.7). See phys_ecs.h and the "ECS
 * integration" block of de_physics.h.
 *
 * Pass 1, de__pecs_validate: entity index order over the map. An entry whose entity is dead,
 * whose kind no longer fits its components, or whose body went stale is dropped and its body
 * destroyed; an alive entity that still qualifies for another kind sets `rescan`.
 *
 * Pass 2, de__pecs_sync: the change queries in a fixed order (DeRigidBody, DeCollider,
 * DeCharacter, then DeTransform with DeCollider and with DeCharacter), each in archetype / row
 * order. A row whose entity has no entry creates its body (once per tick: the marks); a row of an
 * entry applies the change. Added components count as changed, so new entities come through the
 * same queries. Then, when `rescan` is set, every DeCollider and DeCharacter entity without an
 * entry gets its body. The new entries are merged into the map in entity index order.
 *
 * Changes are compared bit for bit against the body (or, for the fields that re-create the body,
 * against a hash taken at create), so a component that was only marked changed never touches the
 * body: sleeping bodies are not woken by writes of unchanged values (DENG-218).
 *
 * Write-back (after the step): entity index order; a pose or velocity is written (with a mutable
 * get, which marks only that component of that entity) only when it differs bit for bit.
 */
#include "physics/ecs/phys_ecs.h"

/* ================================ Small helpers ======================================== */

static de_quat quat_mul(de_quat a, de_quat b) {
    de_quat r;
    r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    return r;
}

static bool quat_is_zero(de_quat q) {
    return q.x == 0.0f && q.y == 0.0f && q.z == 0.0f && q.w == 0.0f;
}

static bool quat_is_identity(de_quat q) {
    return q.x == 0.0f && q.y == 0.0f && q.z == 0.0f && q.w == 1.0f;
}

/* The rotation a body would store for `q`. */
static de_quat body_rotation(de_quat q) {
    if (quat_is_zero(q)) {
        de_quat id = {0.0f, 0.0f, 0.0f, 1.0f};
        return id;
    }
    return de_dnormalize_quat(q);
}

static bool pose_equal(const de_physics_world *p, u32 i, const de_transform *t) {
    return memcmp(&p->bodies.position[i], &t->position, sizeof t->position) == 0 &&
           memcmp(&p->bodies.rotation[i], &t->rotation, sizeof t->rotation) == 0;
}

static bool v3_equal(de_vec3 a, de_vec3 b) {
    return memcmp(&a, &b, sizeof a) == 0;
}

static de_transform identity_transform(void) {
    de_transform t;
    memset(&t, 0, sizeof t);
    t.rotation.w = 1.0f;
    t.scale = de__v3(1.0f, 1.0f, 1.0f);
    return t;
}

u8 de__pecs_kind_of(const de__pecs *s, de_entity e) {
    if (de_entity_has(s->w, e, s->c_char))
        return DE__PECS_CHARACTER;
    if (de_entity_has(s->w, e, s->c_col))
        return de_entity_has(s->w, e, s->c_rb) ? (u8)DE__PECS_RIGID : (u8)DE__PECS_STATIC;
    return 0;
}

/* Byte buffer for the re-create key (in-process only, never persisted). */
typedef struct key_buf {
    u8 b[128];
    u32 n;
} key_buf;

static void key_put(key_buf *k, const void *v, u32 n) {
    DE_ASSERT(k->n + n <= sizeof k->b);
    memcpy(k->b + k->n, v, n);
    k->n += n;
}

/* The fields whose change re-creates the body: the kind, DeRigidBody.type and .mass, and every
 * DeCollider field but the offset's scale (ignored). Hashed field by field (no padding bytes). */
static u64 entry_key(u8 kind, const DeRigidBody *rb, const DeCollider *col) {
    key_buf k;
    k.n = 0;
    key_put(&k, &kind, 1);
    if (rb != NULL) {
        const u32 type = (u32)rb->type;
        key_put(&k, &type, 4);
        key_put(&k, &rb->mass, 4);
    }
    if (col != NULL) {
        key_put(&k, &col->shape.id, 4);
        key_put(&k, &col->offset.position, sizeof col->offset.position);
        key_put(&k, &col->offset.rotation, sizeof col->offset.rotation);
        key_put(&k, &col->material.friction, 4);
        key_put(&k, &col->material.restitution, 4);
        key_put(&k, &col->material.density, 4);
        key_put(&k, &col->filter.layer, 4);
        key_put(&k, &col->filter.mask, 4);
        key_put(&k, &col->filter.group, 4);
        const u8 trig = col->is_trigger ? 1u : 0u;
        key_put(&k, &trig, 1);
    }
    return de_hash64(k.b, k.n, 0x5ECD223ull);
}

/* de_body_flags from the components (DE_BODY_FLAG_TRIGGER from the collider). */
static u32 body_flags(const DeRigidBody *rb, const DeCollider *col) {
    u32 f = 0;
    if (rb != NULL) {
        if (rb->ccd)
            f |= DE_BODY_FLAG_CCD;
        if (rb->allow_sleep)
            f |= DE_BODY_FLAG_ALLOW_SLEEP;
        if (rb->lock_rot_x)
            f |= DE_BODY_FLAG_LOCK_ROT_X;
        if (rb->lock_rot_y)
            f |= DE_BODY_FLAG_LOCK_ROT_Y;
        if (rb->lock_rot_z)
            f |= DE_BODY_FLAG_LOCK_ROT_Z;
    }
    if (col != NULL && col->is_trigger)
        f |= DE_BODY_FLAG_TRIGGER;
    return f;
}

/* ================================ Create and destroy =================================== */

void de__pecs_destroy_body(de__pecs *s, de__pecs_entry *en) {
    de_physics_world *p = s->p;
    if (de_body_valid(p, en->body)) {
        if (en->kind == DE__PECS_CHARACTER)
            de_character_destroy(p, en->body);
        else
            de_body_destroy(p, en->body);
        ++s->stats.destroyed;
    }
    if (DE_HANDLE_VALID(en->owned_shape))
        (void)de_shape_destroy(p, en->owned_shape);
    en->body = (de_body){0};
    en->owned_shape = (de_shape){0};
}

/* Failures logged before the log goes quiet (a component that stays invalid would otherwise
 * warn on every change). stats.failed keeps counting. */
#define DE__PECS_MAX_FAIL_WARNINGS 32u

static void warn_failed(de__pecs *s, de_entity e, const char *what) {
    if (s->stats.failed < DE__PECS_MAX_FAIL_WARNINGS) {
        const de_error *err = de_error_last();
        DE_WARN(de__physics_log(), "physics.step: entity %u:%u gets no %s: %s%s",
                de_entity_index(e), de_entity_generation(e), what,
                err != NULL && err->result != DE_OK ? err->message : "invalid components",
                s->stats.failed + 1u == DE__PECS_MAX_FAIL_WARNINGS
                    ? " (further failures are not logged)"
                    : "");
    }
    ++s->stats.failed;
    de_error_clear();
}

/*
 * Makes the body of `e` (kind RIGID or STATIC) into `en` from the current components. The pose is
 * DeTransform (identity when absent); the velocities are DeRigidBody's. false: no body (warned).
 */
static bool create_rigid(de__pecs *s, de_entity e, u8 kind, de__pecs_entry *en) {
    de_physics_world *p = s->p;
    const DeRigidBody *rb =
        kind == DE__PECS_RIGID ? (const DeRigidBody *)de_entity_get_const(s->w, e, s->c_rb) : NULL;
    const DeCollider *col = (const DeCollider *)de_entity_get_const(s->w, e, s->c_col);
    const de_transform *xf = (const de_transform *)de_entity_get_const(s->w, e, s->c_xform);
    if (col == NULL || (kind == DE__PECS_RIGID && rb == NULL))
        return false;
    if (!de_shape_valid(p, col->shape)) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "DeCollider.shape is not a live shape");
        warn_failed(s, e, "body");
        return false;
    }

    de_shape shape = col->shape, owned = {0};
    const bool offset =
        col->offset.position.x != 0.0 || col->offset.position.y != 0.0 ||
        col->offset.position.z != 0.0 ||
        !(quat_is_zero(col->offset.rotation) || quat_is_identity(col->offset.rotation));
    if (offset) {
        de_shape_desc sd;
        memset(&sd, 0, sizeof sd);
        sd.kind = DE_SHAPE_COMPOUND;
        sd.children = &col->shape;
        sd.child_xforms = &col->offset;
        sd.child_count = 1;
        owned = de_shape_create(p, &sd);
        if (!DE_HANDLE_VALID(owned)) {
            warn_failed(s, e, "body (DeCollider.offset compound)");
            return false;
        }
        shape = owned;
    }

    de_body_desc d;
    memset(&d, 0, sizeof d);
    d.type = rb != NULL ? rb->type : DE_BODY_STATIC;
    d.transform = xf != NULL ? *xf : identity_transform();
    d.shape = shape;
    d.material = col->material;
    d.filter = col->filter;
    d.flags = body_flags(rb, col);
    d.owner = e;
    if (rb != NULL) {
        d.mass = rb->mass;
        d.linear_velocity = rb->linear_velocity;
        d.angular_velocity = rb->angular_velocity;
        d.linear_damping = rb->linear_damping;
        d.angular_damping = rb->angular_damping;
        d.gravity_scale = rb->gravity_scale;
    }
    const de_body b = de_body_create(p, &d);
    if (!DE_HANDLE_VALID(b)) {
        if (DE_HANDLE_VALID(owned))
            (void)de_shape_destroy(p, owned);
        warn_failed(s, e, "body");
        return false;
    }
    ++s->stats.created;
    memset(en, 0, sizeof *en);
    en->entity = e;
    en->body = b;
    en->owned_shape = owned;
    en->kind = kind;
    en->body_type = (u8)d.type;
    en->flags = (rb != NULL && rb->no_interpolate) ? (u8)DE__PECS_NO_INTERP : (u8)0;
    en->key = entry_key(kind, rb, col);
    if (rb != NULL && rb->body.id != b.id) {
        DeRigidBody *wr = (DeRigidBody *)de_entity_get(s->w, e, s->c_rb);
        wr->body = b;
    }
    return true;
}

static bool create_character(de__pecs *s, de_entity e, de__pecs_entry *en) {
    const DeCharacter *c = (const DeCharacter *)de_entity_get_const(s->w, e, s->c_char);
    const de_transform *xf = (const de_transform *)de_entity_get_const(s->w, e, s->c_xform);
    if (c == NULL)
        return false;
    const de_transform at = xf != NULL ? *xf : identity_transform();
    const de_body b = de_character_create(s->p, c, &at, e);
    if (!DE_HANDLE_VALID(b)) {
        warn_failed(s, e, "character");
        return false;
    }
    ++s->stats.created;
    memset(en, 0, sizeof *en);
    en->entity = e;
    en->body = b;
    en->kind = DE__PECS_CHARACTER;
    en->body_type = (u8)DE_BODY_KINEMATIC;
    return true;
}

/* A new entity (no entry yet): its body, added to this tick's batch. */
static void create_entity(de__pecs *s, de_entity e) {
    const u8 kind = de__pecs_kind_of(s, e);
    if (kind == 0)
        return;
    de__pecs_entry en;
    const bool ok =
        kind == DE__PECS_CHARACTER ? create_character(s, e, &en) : create_rigid(s, e, kind, &en);
    if (!ok)
        return;
    if (!de__pecs_batch_add(s, &en)) {
        DE_WARN(de__physics_log(),
                "physics.step: out of memory for the entity map; the body of "
                "entity %u:%u is made again next tick",
                de_entity_index(e), de_entity_generation(e));
        de_error_clear();
        de__pecs_destroy_body(s, &en);
        s->rescan = true;
    }
}

/* A component change that needs a new body: destroy, then create from the components. On
 * failure the entry keeps an invalid body and DE__PECS_FAILED; the next validation drops it
 * without a rescan, and the next change of its components tries again. */
static void recreate(de__pecs *s, de__pecs_entry *en) {
    const de_entity e = en->entity;
    const u8 kind = en->kind;
    de__pecs_destroy_body(s, en);
    de__pecs_entry fresh;
    if (create_rigid(s, e, kind, &fresh)) {
        *en = fresh;
        ++s->stats.recreated;
    } else {
        en->flags |= (u8)DE__PECS_FAILED; /* retried when a component changes again */
    }
}

/* ================================ Validation ========================================== */

void de__pecs_validate(de__pecs *s) {
    u32 out = 0;
    for (u32 i = 0; i < s->count; ++i) {
        de__pecs_entry en = s->map[i];
        const bool alive = de_entity_alive(s->w, en.entity);
        const u8 want = alive ? de__pecs_kind_of(s, en.entity) : 0u;
        if (alive && want == en.kind && de_body_valid(s->p, en.body)) {
            s->map[out++] = en;
            continue;
        }
        de__pecs_destroy_body(s, &en);
        if (want != 0 && (en.flags & DE__PECS_FAILED) == 0) {
            s->rescan = true;
            ++s->stats.rescans;
        }
    }
    s->count = out;
}

/* ================================ Changes ============================================= */

enum { CH_RB = 1u, CH_COL = 2u, CH_CHAR = 4u, CH_XFORM = 8u };

/* DeRigidBody fields applied in place: flags, damping, gravity scale; then the velocities. */
static void apply_rigid_body(de__pecs *s, de__pecs_entry *en, const DeRigidBody *rb,
                             const DeCollider *col) {
    de_physics_world *p = s->p;
    u32 i;
    if (!de__body_resolve(p, en->body, &i))
        return;
    en->flags = rb->no_interpolate ? (u8)DE__PECS_NO_INTERP : (u8)0;
    de__body_pool *b = &p->bodies;
    bool changed = false;
    const u32 flags = body_flags(rb, col);
    if (b->flags[i] != flags) {
        b->flags[i] = flags;
        changed = true;
    }
    if (en->body_type == DE_BODY_DYNAMIC) {
        const f32 ld = rb->linear_damping, ad = rb->angular_damping, gs = rb->gravity_scale;
        if (de__f32_finite(ld) && de__f32_finite(ad) && de__f32_finite(gs) &&
            (memcmp(&b->linear_damping[i], &ld, 4) != 0 ||
             memcmp(&b->angular_damping[i], &ad, 4) != 0 ||
             memcmp(&b->gravity_scale[i], &gs, 4) != 0)) {
            b->linear_damping[i] = ld;
            b->angular_damping[i] = ad;
            b->gravity_scale[i] = gs;
            changed = true;
        }
    }
    if (changed)
        de_body_wake(p, en->body);
    if (en->body_type != DE_BODY_STATIC &&
        (!v3_equal(b->linear_velocity[i], rb->linear_velocity) ||
         !v3_equal(b->angular_velocity[i], rb->angular_velocity))) {
        if (de_body_set_velocity(p, en->body, rb->linear_velocity, rb->angular_velocity))
            ++s->stats.velocities;
    }
}

/* The velocities that carry kinematic body `i` to pose `t` in dt (center of mass and rotation). */
static void target_velocity(const de_physics_world *p, u32 i, const de__pecs_pose *t, f32 dt,
                            de_vec3 *lin, de_vec3 *ang) {
    const de__body_pool *b = &p->bodies;
    const de_vec3 lc = b->local_com[i];
    const de_vec3 r0 = de__quat_rotate(b->rotation[i], lc);
    const de_vec3 r1 = de__quat_rotate(t->rotation, lc);
    const de_dvec3 c0 = de__d3_add(b->position[i], de__d3_from(r0));
    const de_dvec3 c1 = de__d3_add(t->position, de__d3_from(r1));
    const de_dvec3 dv = de__d3_scale(de__d3_sub(c1, c0), 1.0 / (f64)dt);
    *lin = de__v3((f32)dv.x, (f32)dv.y, (f32)dv.z);

    de_quat inv = b->rotation[i];
    inv.x = -inv.x;
    inv.y = -inv.y;
    inv.z = -inv.z;
    de_quat q = quat_mul(t->rotation, inv);
    if (q.w < 0.0f) {
        q.x = -q.x;
        q.y = -q.y;
        q.z = -q.z;
        q.w = -q.w;
    }
    const f32 sn = de_dsqrt(q.x * q.x + q.y * q.y + q.z * q.z);
    if (sn > 0.0f) {
        const f32 angle = 2.0f * de_datan2(sn, q.w);
        const f32 k = angle / (sn * dt);
        *ang = de__v3(q.x * k, q.y * k, q.z * k);
    } else {
        *ang = de__v3(0.0f, 0.0f, 0.0f);
    }
}

/* A DeTransform change: teleport, or a target for a kinematic body. */
static void apply_transform(de__pecs *s, de__pecs_entry *en, f32 dt) {
    de_physics_world *p = s->p;
    u32 i;
    if (!de__body_resolve(p, en->body, &i))
        return;
    const de_transform *xf =
        (const de_transform *)de_entity_get_const(s->w, en->entity, s->c_xform);
    if (xf == NULL || pose_equal(p, i, xf))
        return;
    de__pecs_pose t;
    t.position = xf->position;
    t.rotation = body_rotation(xf->rotation);
    if (!de__d3_finite(t.position) || !de__f32_finite(t.rotation.x) ||
        !de__f32_finite(t.rotation.y) || !de__f32_finite(t.rotation.z) ||
        !de__f32_finite(t.rotation.w))
        return;
    if (memcmp(&p->bodies.position[i], &t.position, sizeof t.position) == 0 &&
        memcmp(&p->bodies.rotation[i], &t.rotation, sizeof t.rotation) == 0)
        return; /* only the rotation's normalization differs */
    if (en->kind == DE__PECS_RIGID && en->body_type == DE_BODY_KINEMATIC) {
        /* Last write of the tick wins (one target per body). */
        for (u32 k = 0; k < s->target_count; ++k) {
            if (s->targets[k].body.id == en->body.id) {
                s->targets[k].pose = t;
                de_vec3 lin, ang;
                target_velocity(p, i, &t, dt, &lin, &ang);
                (void)de_body_set_velocity(p, en->body, lin, ang);
                return;
            }
        }
        if (!de__pecs_grow(s, (void **)&s->targets, &s->target_cap, s->target_count + 1u,
                           sizeof *s->targets)) {
            de_error_clear();
            (void)de_body_set_transform(p, en->body, xf); /* no room: teleport instead */
            ++s->stats.teleports;
            return;
        }
        de__pecs_target *tg = &s->targets[s->target_count++];
        tg->body = en->body;
        tg->entity = en->entity;
        tg->pose = t;
        de_vec3 lin, ang;
        target_velocity(p, i, &t, dt, &lin, &ang);
        (void)de_body_set_velocity(p, en->body, lin, ang);
        ++s->stats.teleports;
        return;
    }
    de_transform tt = *xf;
    tt.rotation = t.rotation;
    if (de_body_set_transform(p, en->body, &tt))
        ++s->stats.teleports;
}

/* One row of a change query. */
static void handle_entity(de__pecs *s, de_entity e, u32 what, f32 dt) {
    if (de__pecs_marked(s, e))
        return; /* created (or tried) this tick: nothing older to apply */
    de__pecs_entry *en = de__pecs_find(s, e);
    if (en == NULL) {
        if (de__pecs_mark(s, e))
            create_entity(s, e);
        return;
    }
    if (en->kind == DE__PECS_CHARACTER) {
        if (what & CH_XFORM)
            apply_transform(s, en, dt);
        return;
    }
    if (what & (CH_RB | CH_COL)) {
        const DeRigidBody *rb = en->kind == DE__PECS_RIGID
                                    ? (const DeRigidBody *)de_entity_get_const(s->w, e, s->c_rb)
                                    : NULL;
        const DeCollider *col = (const DeCollider *)de_entity_get_const(s->w, e, s->c_col);
        if (col == NULL || (en->kind == DE__PECS_RIGID && rb == NULL))
            return; /* cannot happen after validation */
        if (entry_key(en->kind, rb, col) != en->key) {
            recreate(s, en);
            return;
        }
        if (rb != NULL)
            apply_rigid_body(s, en, rb, col);
    }
    if (what & CH_XFORM)
        apply_transform(s, en, dt);
}

typedef struct sync_pass {
    de__pecs *s;
    u32 what;
    f32 dt;
} sync_pass;

static void sync_chunk(const de_query_chunk *chunk, void *user) {
    const sync_pass *x = (const sync_pass *)user;
    for (u32 r = 0; r < chunk->count; ++r)
        handle_entity(x->s, chunk->entities[r], x->what, x->dt);
}

void de__pecs_sync(de__pecs *s, const de_system_ctx *ctx, f32 dt) {
    static const struct {
        u32 query, what;
    } k_passes[] = {
        {DE__PECS_Q_RB_CHANGED, CH_RB},     {DE__PECS_Q_COL_CHANGED, CH_COL},
        {DE__PECS_Q_CHAR_CHANGED, CH_CHAR}, {DE__PECS_Q_XFORM_COL, CH_XFORM},
        {DE__PECS_Q_XFORM_CHAR, CH_XFORM},
    };
    sync_pass x;
    x.s = s;
    x.dt = dt;
    for (u32 k = 0; k < sizeof k_passes / sizeof k_passes[0]; ++k) {
        x.what = k_passes[k].what;
        de_query_each(s->w, ctx->queries[k_passes[k].query], sync_chunk, &x);
    }
    if (s->rescan) {
        s->rescan = false;
        x.what = 0;
        de_query_each(s->w, ctx->queries[DE__PECS_Q_ALL_COL], sync_chunk, &x);
        de_query_each(s->w, ctx->queries[DE__PECS_Q_ALL_CHAR], sync_chunk, &x);
    }
    de__pecs_batch_merge(s);
    de__pecs_clear_marks(s);
    s->stats.entries = s->count;
}

/* ================================ After the step ====================================== */

void de__pecs_finish_targets(de__pecs *s) {
    de_physics_world *p = s->p;
    for (u32 k = 0; k < s->target_count; ++k) {
        const de__pecs_target *tg = &s->targets[k];
        if (!de_body_valid(p, tg->body))
            continue;
        de_transform t = identity_transform();
        t.position = tg->pose.position;
        t.rotation = tg->pose.rotation;
        (void)de_body_set_transform(p, tg->body, &t);
        const DeRigidBody *rb = (const DeRigidBody *)de_entity_get_const(s->w, tg->entity, s->c_rb);
        const de_vec3 zero = de__v3(0.0f, 0.0f, 0.0f);
        (void)de_body_set_velocity(p, tg->body, rb != NULL ? rb->linear_velocity : zero,
                                   rb != NULL ? rb->angular_velocity : zero);
    }
    s->target_count = 0;
}

void de__pecs_write_back(de__pecs *s) {
    de_physics_world *p = s->p;
    const de__body_pool *b = &p->bodies;
    const bool poses = de__pecs_poses_ensure(s);
    if (!poses)
        de_error_clear();
    for (u32 k = 0; k < s->count; ++k) {
        const de__pecs_entry *en = &s->map[k];
        if (en->kind == DE__PECS_STATIC ||
            (en->kind == DE__PECS_RIGID && en->body_type == DE_BODY_STATIC))
            continue;
        u32 i;
        if (!de__body_resolve(p, en->body, &i))
            continue;
        if (poses) {
            s->poses[i].curr.position = b->position[i];
            s->poses[i].curr.rotation = b->rotation[i];
        }
        const de_transform *xf =
            (const de_transform *)de_entity_get_const(s->w, en->entity, s->c_xform);
        if (xf != NULL && !pose_equal(p, i, xf)) {
            de_transform *wx = (de_transform *)de_entity_get(s->w, en->entity, s->c_xform);
            wx->position = b->position[i];
            wx->rotation = b->rotation[i];
            ++s->stats.writes;
        }
        if (en->kind != DE__PECS_RIGID || en->body_type != DE_BODY_DYNAMIC)
            continue;
        const DeRigidBody *rb = (const DeRigidBody *)de_entity_get_const(s->w, en->entity, s->c_rb);
        if (rb != NULL && (!v3_equal(rb->linear_velocity, b->linear_velocity[i]) ||
                           !v3_equal(rb->angular_velocity, b->angular_velocity[i]))) {
            DeRigidBody *wr = (DeRigidBody *)de_entity_get(s->w, en->entity, s->c_rb);
            wr->linear_velocity = b->linear_velocity[i];
            wr->angular_velocity = b->angular_velocity[i];
            ++s->stats.writes;
        }
    }
}
