/* Six 2D joint kinds on the colored soft-step constraint interface. Scalar rows use
 * accumulated impulses, unilateral limits and bounded motors. No allocations in stages. */
#include "physics/2d/joints2d/joint2d_internal.h"
#include "physics/2d/broadphase2d.h"

typedef struct joint2d {
    de_joint2d_desc d;
    u32 a, b, generation;
    bool alive, broken;
    de_vec2 ra0, rb0, delta, axis;
    f32 angle0;
    f32 impulse[8];
    f32 peak;
} joint2d;
typedef struct joints2d {
    joint2d *items;
    u32 capacity;
} joints2d;
typedef struct jx {
    const de__solver2d_ctx *ctx;
    const de__constraint2d *c;
    de__solver_body2d *a, *b;
    de_vec2 ra, rb, d, axis;
    f32 angle;
} jx;
static f32 clampf(f32 x, f32 lo, f32 hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}
static f32 absf(f32 x) {
    return x < 0 ? -x : x;
}
static f32 cross(de_vec2 a, de_vec2 b) {
    return a.x * b.y - a.y * b.x;
}
static de_vec2 perp(de_vec2 a) {
    return de__v2(-a.y, a.x);
}
static joints2d *state(const de_physics2d_world *p) {
    return p ? (joints2d *)de__p2d_ext_get(p, DE__P2D_EXT_JOINTS) : NULL;
}
static joint2d *resolve(const de_physics2d_world *p, de_joint h) {
    joints2d *s = state(p);
    if (!s || !h.id)
        return NULL;
    u32 i = de__handle_index(h.id);
    if (i >= s->capacity || !s->items[i].alive || s->items[i].generation != de__handle_gen(h.id))
        return NULL;
    return &s->items[i];
}
static void wake(de_physics2d_world *p, const joint2d *j) {
    de__bp2d_invalidate_pairs(p);
    if (j->a != DE__CONSTRAINT2D_NO_BODY)
        de__islands2d_wake_body(p, j->a);
    if (j->b != DE__CONSTRAINT2D_NO_BODY)
        de__islands2d_wake_body(p, j->b);
}
static void gather(de__solver2d *s, const de__solver2d_ctx *ctx, void *v) {
    joints2d *js = v;
    /* Serial stage: refresh pairs next tick, including joints broken by parallel store. */
    de__bp2d_invalidate_pairs(ctx->p);
    for (u32 i = 0; i < js->capacity; ++i) {
        joint2d *j = &js->items[i];
        if (!j->alive || j->broken)
            continue;
        de__constraint2d *c =
            de__solver2d_add_constraint(s, DE__CONSTRAINT2D_CLASS_JOINT, i, j->a, j->b);
        if (c)
            c->data = i;
    }
    (void)ctx;
}
static void side(const de_physics2d_world *p, u32 i, de_vec2 local, de_dpoint2d world, de__d2 *com,
                 de_vec2 *r, f32 *angle) {
    if (i == DE__CONSTRAINT2D_NO_BODY) {
        *com = de__d2_make(world.x, world.y);
        *r = de__v2(0, 0);
        *angle = 0;
        return;
    }
    const de__body2d_pool *b = &p->bodies;
    *r = de__rot2d_apply(b->rotation[i], de__v2_sub(local, b->local_com[i]));
    de_vec2 lc = de__rot2d_apply(b->rotation[i], b->local_com[i]);
    *com = de__d2_add(b->position[i], de__d2_make(lc.x, lc.y));
    *angle = de_datan2(b->rotation[i].s, b->rotation[i].c);
}
static void prepare(const de__solver2d_ctx *ctx, void *v, de__constraint2d *c) {
    joint2d *j = &((joints2d *)v)->items[c->data];
    de__d2 a, b;
    f32 aa, ab;
    side(ctx->p, j->a, j->d.local_anchor_a, j->d.world_anchor_a, &a, &j->ra0, &aa);
    side(ctx->p, j->b, j->d.local_anchor_b, j->d.world_anchor_b, &b, &j->rb0, &ab);
    j->delta = de__v2((f32)(b.x - a.x), (f32)(b.y - a.y));
    j->angle0 = ab - aa - j->d.reference_angle;
    j->axis = j->a == DE__CONSTRAINT2D_NO_BODY
                  ? j->d.axis
                  : de__rot2d_apply(ctx->p->bodies.rotation[j->a], j->d.axis);
    memset(j->impulse, 0, sizeof j->impulse);
    j->peak = 0;
}
static void warm(const de__solver2d_ctx *ctx, void *v, de__constraint2d *c) {
    joint2d *j = &((joints2d *)v)->items[c->data];
    memset(j->impulse, 0, sizeof j->impulse);
    (void)ctx;
}
static jx begin(const de__solver2d_ctx *ctx, const de__constraint2d *c, const joint2d *j) {
    jx x;
    x.ctx = ctx;
    x.c = c;
    x.a = &ctx->bodies[c->solver_a];
    x.b = &ctx->bodies[c->solver_b];
    x.ra = de__rot2d_apply(x.a->dq, j->ra0);
    x.rb = de__rot2d_apply(x.b->dq, j->rb0);
    x.d = de__v2_add(j->delta, de__v2_add(de__v2_sub(x.b->dp, x.a->dp), de__v2_sub(x.rb, x.ra)));
    x.angle = j->angle0 + x.b->da - x.a->da;
    x.axis = de__rot2d_apply(x.a->dq, j->axis);
    return x;
}
/* A row J=(-n,-ra×n,n,rb×n), or an angular row J=(0,-1,0,1). */
static void row(jx *x, joint2d *j, u32 k, de_vec2 n, bool angular, f32 error, f32 target, f32 lo,
                f32 hi, bool bias, f32 hz) {
    f32 ja = angular ? 1.0f : cross(x->ra, n);
    f32 jb = angular ? 1.0f : cross(x->rb, n);
    f32 mass = (angular ? 0 : x->a->inv_mass + x->b->inv_mass) + x->a->inv_inertia * ja * ja +
               x->b->inv_inertia * jb * jb;
    if (!(mass > 0))
        return;
    f32 speed = angular ? x->b->w - x->a->w
                        : de__v2_dot(n, de__v2_sub(x->b->v, x->a->v)) + jb * x->b->w - ja * x->a->w;
    de__soft soft = {0, 1, 0};
    if (bias || hz > 0)
        soft = de__soft_make(hz > 0 ? hz : 0.25f * x->ctx->inv_h, 1.0f, x->ctx->h);
    if (hz > 0)
        soft = de__soft_make(hz, j->d.damping_ratio > 0 ? j->d.damping_ratio : 0.7f, x->ctx->h);
    f32 b = (bias || hz > 0) ? soft.bias_rate * error : 0;
    b = clampf(b, -20, 20);
    f32 imp = -soft.mass_scale * (speed - target + b) / mass - soft.impulse_scale * j->impulse[k];
    f32 next = clampf(j->impulse[k] + imp, lo, hi);
    imp = next - j->impulse[k];
    j->impulse[k] = next;
    if (absf(next) > j->peak)
        j->peak = absf(next);
    if (x->c->flags & DE__CONSTRAINT2D_WRITE_A) {
        if (!angular)
            x->a->v = de__v2_sub(x->a->v, de__v2_scale(n, x->a->inv_mass * imp));
        x->a->w -= x->a->inv_inertia * ja * imp;
    }
    if (x->c->flags & DE__CONSTRAINT2D_WRITE_B) {
        if (!angular)
            x->b->v = de__v2_add(x->b->v, de__v2_scale(n, x->b->inv_mass * imp));
        x->b->w += x->b->inv_inertia * jb * imp;
    }
}
static void limits(jx *x, joint2d *j, de_vec2 n, bool angular, f32 value, bool bias) {
    if (!j->d.enable_limit)
        return;
    if (j->d.lower == j->d.upper) {
        row(x, j, 3, n, angular, value - j->d.lower, 0, -1e30f, 1e30f, bias, j->d.hertz);
        return;
    }
    /* Speculative limit velocity prevents crossing a stop during the substep. */
    f32 lower = value - j->d.lower, upper = j->d.upper - value;
    row(x, j, 3, n, angular, lower < 0 ? lower : 0, lower > 0 ? -lower * x->ctx->inv_h : 0, 0,
        1e30f, bias, 0);
    row(x, j, 4, n, angular, upper < 0 ? -upper : 0, upper > 0 ? upper * x->ctx->inv_h : 0, -1e30f,
        0, bias, 0);
}
static void motor(jx *x, joint2d *j, de_vec2 n, bool angular) {
    if (!j->d.enable_motor)
        return;
    f32 cap = j->d.max_motor_force * x->ctx->h;
    row(x, j, 5, n, angular, 0, j->d.motor_speed, -cap, cap, false, 0);
}
static void solve(const de__solver2d_ctx *ctx, void *v, de__constraint2d *c, bool bias) {
    joint2d *j = &((joints2d *)v)->items[c->data];
    if (j->broken)
        return;
    jx x = begin(ctx, c, j);
    de_vec2 ex = de__v2(1, 0), ey = de__v2(0, 1), zero = de__v2(0, 0);
    const f32 inf = 1e30f;
    switch (j->d.kind) {
    case DE_JOINT2D_REVOLUTE:
    case DE_JOINT2D_WELD:
    case DE_JOINT2D_MOUSE: {
        f32 hz = j->d.hertz;
        f32 cap = inf;
        if (j->d.kind == DE_JOINT2D_MOUSE) {
            if (hz == 0)
                hz = 5;
            cap = (j->d.max_motor_force > 0 ? j->d.max_motor_force : 1000) * ctx->h * 0.70710678f;
        }
        row(&x, j, 0, ex, false, x.d.x, 0, -cap, cap, bias, hz);
        row(&x, j, 1, ey, false, x.d.y, 0, -cap, cap, bias, hz);
        if (j->d.kind == DE_JOINT2D_WELD)
            row(&x, j, 2, zero, true, x.angle, 0, -inf, inf, bias, hz);
        if (j->d.kind == DE_JOINT2D_REVOLUTE) {
            motor(&x, j, zero, true);
            limits(&x, j, zero, true, x.angle, bias);
            if (hz > 0 && !j->d.enable_limit)
                row(&x, j, 2, zero, true, x.angle, 0, -inf, inf, bias, hz);
        }
        break;
    }
    case DE_JOINT2D_PRISMATIC:
    case DE_JOINT2D_WHEEL: {
        de_vec2 normal = perp(x.axis);
        row(&x, j, 0, normal, false, de__v2_dot(x.d, normal), 0, -inf, inf, bias, 0);
        f32 t = de__v2_dot(x.d, x.axis);
        if (j->d.kind == DE_JOINT2D_PRISMATIC) {
            row(&x, j, 1, zero, true, x.angle, 0, -inf, inf, bias, 0);
            motor(&x, j, x.axis, false);
        } else {
            row(&x, j, 1, x.axis, false, t, 0, -inf, inf, bias, j->d.hertz > 0 ? j->d.hertz : 5);
            motor(&x, j, zero, true);
        }
        limits(&x, j, x.axis, false, t, bias);
        if (j->d.kind == DE_JOINT2D_PRISMATIC && j->d.hertz > 0 && !j->d.enable_limit)
            row(&x, j, 2, x.axis, false, t, 0, -inf, inf, bias, j->d.hertz);
        break;
    }
    case DE_JOINT2D_DISTANCE: {
        f32 len = de_dsqrt(de__v2_dot(x.d, x.d));
        de_vec2 n = len > 1e-6f ? de__v2_scale(x.d, 1.0f / len) : ex;
        if (j->d.enable_limit)
            limits(&x, j, n, false, len, bias);
        else
            row(&x, j, 0, n, false, len - j->d.upper, 0, -inf, inf, bias, j->d.hertz);
        motor(&x, j, n, false);
        break;
    }
    }
}
static void store(const de__solver2d_ctx *ctx, void *v, de__constraint2d *c) {
    joint2d *j = &((joints2d *)v)->items[c->data];
    f32 sum = 0;
    for (u32 k = 0; k < 8; ++k)
        sum += absf(j->impulse[k]);
    if (j->d.break_force > 0 && (sum > j->peak ? sum : j->peak) * ctx->inv_h > j->d.break_force) {
        j->broken = true;
        /* Both sides are already awake. Shared world state is touched only by gather. */
    }
}
static void destroy(de_physics2d_world *p, void *v) {
    joints2d *s = v;
    DE_FREE(p->alloc, s->items, sizeof(joint2d) * s->capacity);
    DE_FREE(p->alloc, s, sizeof *s);
    de__p2d_ext_set(p, DE__P2D_EXT_JOINTS, NULL, NULL);
}
static void body_removed(de_physics2d_world *p, void *v, u32 body) {
    joints2d *s = v;
    for (u32 i = 0; i < s->capacity; ++i)
        if (s->items[i].alive && (s->items[i].a == body || s->items[i].b == body)) {
            wake(p, &s->items[i]);
            s->items[i].alive = false;
            s->items[i].generation = s->items[i].generation % 4095u + 1u;
        }
}
static const de__p2d_ext_vtbl ext = {NULL, NULL, body_removed};
static const de__constraint2d_class cls = {"joints2d", gather, prepare, warm,   solve,
                                           NULL,       NULL,   store,   destroy};
static de_result ensure(de_physics2d_world *p) {
    if (state(p))
        return DE_OK;
    joints2d *s = DE_ALLOC(p->alloc, sizeof *s, DE_ALIGNOF(joints2d), DE_MEM_PHYSICS);
    if (!s)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "2D joint pool");
    memset(s, 0, sizeof *s);
    s->capacity = p->max_bodies * 4u;
    s->items =
        DE_ALLOC(p->alloc, sizeof(joint2d) * s->capacity, DE_ALIGNOF(joint2d), DE_MEM_PHYSICS);
    if (!s->items) {
        DE_FREE(p->alloc, s, sizeof *s);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "2D joint slots");
    }
    memset(s->items, 0, sizeof(joint2d) * s->capacity);
    for (u32 i = 0; i < s->capacity; ++i)
        s->items[i].generation = 1;
    de_result r =
        de__solver2d_register_class(p, DE__CONSTRAINT2D_CLASS_JOINT, &cls, s, s->capacity);
    if (r != DE_OK) {
        destroy(p, s);
        return r;
    }
    de__p2d_ext_set(p, DE__P2D_EXT_JOINTS, s, &ext);
    (void)de__solver2d_set_class_iterations(p, DE__CONSTRAINT2D_CLASS_JOINT, 4);
    return DE_OK;
}
de_result de_joint2d_create(de_physics2d_world *p, const de_joint2d_desc *d, de_joint *out) {
    if (out)
        out->id = 0;
    if (!p || !d || !out || d->kind > DE_JOINT2D_MOUSE || d->kind < DE_JOINT2D_REVOLUTE)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "invalid 2D joint description");
    u32 a = DE__CONSTRAINT2D_NO_BODY, b = DE__CONSTRAINT2D_NO_BODY;
    de_body ha = d->body_a, hb = d->body_b;
    if (!ha.id && d->entity_a.bits) {
        for (u32 i = 0; i < p->bodies.high_water; ++i)
            if (p->bodies.alive[i] && p->bodies.owner[i].bits == d->entity_a.bits) {
                ha = de__body2d_handle(p, i);
                break;
            }
        if (!ha.id)
            return DE_FAIL(DE_ERR_NOT_FOUND, "2D joint entity A has no body");
    }
    if (!hb.id && d->entity_b.bits) {
        for (u32 i = 0; i < p->bodies.high_water; ++i)
            if (p->bodies.alive[i] && p->bodies.owner[i].bits == d->entity_b.bits) {
                hb = de__body2d_handle(p, i);
                break;
            }
        if (!hb.id)
            return DE_FAIL(DE_ERR_NOT_FOUND, "2D joint entity B has no body");
    }
    if ((ha.id && !de__body2d_resolve(p, ha, &a)) || (hb.id && !de__body2d_resolve(p, hb, &b)) ||
        a == b)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "invalid 2D joint bodies");
    if (!de__v2_finite(d->axis) || !de__v2_finite(d->local_anchor_a) ||
        !de__v2_finite(d->local_anchor_b) || !de__f32_finite(d->hertz) ||
        !de__f32_finite(d->damping_ratio) || d->hertz < 0 || d->damping_ratio < 0 ||
        !de__f32_finite(d->lower) || !de__f32_finite(d->upper) || d->lower > d->upper ||
        !de__f32_finite(d->motor_speed) || !de__f32_finite(d->max_motor_force) ||
        d->max_motor_force < 0 || !de__f32_finite(d->break_force) || d->break_force < 0 ||
        !de__f32_finite(d->reference_angle) || !de__f64_finite(d->world_anchor_a.x) ||
        !de__f64_finite(d->world_anchor_a.y) || !de__f64_finite(d->world_anchor_b.x) ||
        !de__f64_finite(d->world_anchor_b.y))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "nonfinite or negative 2D joint settings");
    de_result r = ensure(p);
    if (r != DE_OK)
        return r;
    joints2d *s = state(p);
    for (u32 i = 0; i < s->capacity; ++i)
        if (!s->items[i].alive) {
            joint2d *j = &s->items[i];
            u32 gen = j->generation;
            memset(j, 0, sizeof *j);
            j->generation = gen;
            j->alive = true;
            j->a = a;
            j->b = b;
            j->d = *d;
            f32 len = de_dsqrt(de__v2_dot(j->d.axis, j->d.axis));
            j->d.axis = len > 0 ? de__v2_scale(j->d.axis, 1.0f / len) : de__v2(1, 0);
            out->id = de__handle_make(i, gen);
            wake(p, j);
            return DE_OK;
        }
    return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "2D joint pool full");
}
void de_joint2d_destroy(de_physics2d_world *p, de_joint h) {
    joint2d *j = resolve(p, h);
    if (!j)
        return;
    wake(p, j);
    j->alive = false;
    j->generation = j->generation % 4095u + 1u;
}
de_result de_joint2d_set_motor(de_physics2d_world *p, de_joint h, f32 speed, f32 max_force) {
    joint2d *j = resolve(p, h);
    if (!j || !de__f32_finite(speed) || !de__f32_finite(max_force) || max_force < 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "invalid 2D joint motor");
    j->d.enable_motor = true;
    j->d.motor_speed = speed;
    j->d.max_motor_force = max_force;
    wake(p, j);
    return DE_OK;
}
de_result de_joint2d_mouse_set_target(de_physics2d_world *p, de_joint h, de_dpoint2d target) {
    joint2d *j = resolve(p, h);
    if (!j || j->d.kind != DE_JOINT2D_MOUSE || j->a != DE__CONSTRAINT2D_NO_BODY ||
        !de__f64_finite(target.x) || !de__f64_finite(target.y))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "invalid 2D mouse target");
    j->d.world_anchor_a = target;
    wake(p, j);
    return DE_OK;
}
bool de_joint2d_is_broken(const de_physics2d_world *p, de_joint h) {
    joint2d *j = resolve(p, h);
    return j && j->broken;
}
bool de__joints2d_collide(const de_physics2d_world *p, u32 a, u32 b) {
    joints2d *s = state(p);
    if (!s)
        return true;
    for (u32 i = 0; i < s->capacity; ++i) {
        const joint2d *j = &s->items[i];
        if (j->alive && !j->broken && !j->d.collide_connected &&
            ((j->a == a && j->b == b) || (j->a == b && j->b == a)))
            return false;
    }
    return true;
}
typedef struct joint_checksum_bytes {
    u8 bytes[256];
    u32 count;
} joint_checksum_bytes;
static void jck_u32(joint_checksum_bytes *b, u32 value) {
    DE_ASSERT(b->count + 4 <= sizeof b->bytes);
    for (u32 i = 0; i < 4; ++i)
        b->bytes[b->count++] = (u8)(value >> (8 * i));
}
static void jck_f32(joint_checksum_bytes *b, f32 value) {
    u32 bits;
    memcpy(&bits, &value, sizeof bits);
    jck_u32(b, bits);
}
static void jck_f64(joint_checksum_bytes *b, f64 value) {
    u64 bits;
    memcpy(&bits, &value, sizeof bits);
    jck_u32(b, (u32)bits);
    jck_u32(b, (u32)(bits >> 32));
}
static void jck_v2(joint_checksum_bytes *b, de_vec2 value) {
    jck_f32(b, value.x);
    jck_f32(b, value.y);
}
u64 de__joints2d_checksum(const de_physics2d_world *p, u64 h) {
    joints2d *s = state(p);
    if (!s)
        return h;
    /* Explicit words, never structure padding; float bit patterns are encoded little endian. */
    for (u32 i = 0; i < s->capacity; ++i) {
        const joint2d *j = &s->items[i];
        if (!j->alive)
            continue;
        joint_checksum_bytes bytes = {.count = 0};
        jck_u32(&bytes, i);
        jck_u32(&bytes, j->generation);
        jck_u32(&bytes, j->a);
        jck_u32(&bytes, j->b);
        jck_u32(&bytes, (u32)j->d.kind);
        jck_u32(&bytes, j->broken ? 1u : 0u);
        for (u32 k = 0; k < 8; ++k)
            jck_f32(&bytes, j->impulse[k]);
        /* Body/entity selectors are resolved at creation; the authoritative indices
         * above identify the bodies. Prepared rows/peak force are recomputed each tick. */
        jck_v2(&bytes, j->d.local_anchor_a);
        jck_v2(&bytes, j->d.local_anchor_b);
        jck_f64(&bytes, j->d.world_anchor_a.x);
        jck_f64(&bytes, j->d.world_anchor_a.y);
        jck_f64(&bytes, j->d.world_anchor_b.x);
        jck_f64(&bytes, j->d.world_anchor_b.y);
        jck_v2(&bytes, j->d.axis);
        jck_f32(&bytes, j->d.reference_angle);
        jck_u32(&bytes, j->d.enable_limit ? 1u : 0u);
        jck_u32(&bytes, j->d.enable_motor ? 1u : 0u);
        jck_u32(&bytes, j->d.collide_connected ? 1u : 0u);
        jck_f32(&bytes, j->d.lower);
        jck_f32(&bytes, j->d.upper);
        jck_f32(&bytes, j->d.motor_speed);
        jck_f32(&bytes, j->d.max_motor_force);
        jck_f32(&bytes, j->d.hertz);
        jck_f32(&bytes, j->d.damping_ratio);
        jck_f32(&bytes, j->d.break_force);
        h = de_hash64(bytes.bytes, bytes.count, h);
    }
    return h;
}
