/*
 * interpolation.c: fixed-step interpolation of rendered body poses (DENG-223, SPEC §10.2, §13.1).
 *
 * physics.step stores, per body index, the pose before the step (de__pecs_capture_prev, after
 * the sync, so a body created or teleported this tick does not slide from its old place) and the
 * pose after it (de__pecs_write_back). physics.interpolate (DE_SCHEDULE_POST_UPDATE, after
 * DE_TRANSFORM_PROPAGATE_SYSTEM) writes DeGlobalTransform = lerp(prev, curr, alpha) for every
 * dynamic and kinematic body (unless DeRigidBody.no_interpolate) and character that has no
 * parent, then recomputes the DeGlobalTransform of its descendants from their DeTransform.
 *
 * Presentation only: it reads the physics state and DeTransform and writes DeGlobalTransform,
 * which no FixedUpdate system of physics reads and which is not in snapshots or the world
 * checksum, so the simulation is the same with it or without it. It is still written with the
 * deterministic operations (+ - * / sqrt) because the module's sources follow SPEC §5.7.
 */
#include "physics/ecs/phys_ecs.h"

/* Deepest descendant level re-propagated below an interpolated body (deeper ones keep the
 * transform propagation's result for this frame). */
#define DE__PECS_INTERP_MAX_DEPTH 32u

void de__pecs_capture_prev(de__pecs *s) {
    if (!de__pecs_poses_ensure(s)) {
        de_error_clear();
        return;
    }
    const de__body_pool *b = &s->p->bodies;
    for (u32 k = 0; k < s->count; ++k) {
        const de__pecs_entry *en = &s->map[k];
        u32 i;
        if (en->body_type == DE_BODY_STATIC || !de__body_resolve(s->p, en->body, &i))
            continue;
        s->poses[i].prev.position = b->position[i];
        s->poses[i].prev.rotation = b->rotation[i];
    }
}

/* nlerp on the shorter arc, normalized in f64 with de_dnormalize_quat. */
static de_quat nlerp(de_quat a, de_quat b, f32 t) {
    const f32 dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    const f32 sb = dot < 0.0f ? -t : t;
    const f32 sa = 1.0f - t;
    de_quat q;
    q.x = a.x * sa + b.x * sb;
    q.y = a.y * sa + b.y * sb;
    q.z = a.z * sa + b.z * sb;
    q.w = a.w * sa + b.w * sb;
    return de_dnormalize_quat(q);
}

static bool transform_equal(const de_transform *a, const de_transform *b) {
    return memcmp(&a->position, &b->position, sizeof a->position) == 0 &&
           memcmp(&a->rotation, &b->rotation, sizeof a->rotation) == 0 &&
           memcmp(&a->scale, &b->scale, sizeof a->scale) == 0;
}

/* Recomputes the globals of `root`'s descendants (depth first, child order) from `root_global`.
 * An entity without DeTransform (or DeGlobalTransform) stops the walk below it, as transform
 * propagation does. */
static void propagate_children(de__pecs *s, de_entity root, const de_transform *root_global) {
    typedef struct frame {
        de_transform global;
        de_entity next;
    } frame;
    frame stack[DE__PECS_INTERP_MAX_DEPTH];
    u32 depth = 0;
    stack[0].global = *root_global;
    stack[0].next = de_entity_children_first(s->w, root);
    depth = 1;
    while (depth > 0) {
        frame *f = &stack[depth - 1u];
        const de_entity c = f->next;
        if (c.bits == 0) {
            --depth;
            continue;
        }
        f->next = de_entity_children_next(s->w, c);
        const de_transform *local = (const de_transform *)de_entity_get_const(s->w, c, s->c_xform);
        if (local == NULL || !de_entity_has(s->w, c, s->c_global))
            continue;
        const de_transform g = de_transform_mul(&f->global, local);
        const de_transform *cur = (const de_transform *)de_entity_get_const(s->w, c, s->c_global);
        if (!transform_equal(cur, &g)) {
            de_transform *wg = (de_transform *)de_entity_get(s->w, c, s->c_global);
            *wg = g;
        }
        if (depth < DE__PECS_INTERP_MAX_DEPTH) {
            const de_entity first = de_entity_children_first(s->w, c);
            if (first.bits != 0) {
                stack[depth].global = g;
                stack[depth].next = first;
                ++depth;
            }
        }
    }
}

void de__pecs_interpolate_system(de_system_ctx *ctx) {
    de__pecs *s = (de__pecs *)ctx->user;
    if (!s->stepped)
        return;
    f32 alpha = ctx->alpha;
    if (!(alpha > 0.0f))
        alpha = 0.0f; /* also NaN */
    const bool last = alpha >= 1.0f;
    for (u32 k = 0; k < s->count; ++k) {
        const de__pecs_entry *en = &s->map[k];
        if (en->body_type == DE_BODY_STATIC || (en->flags & DE__PECS_NO_INTERP) != 0)
            continue;
        const u32 i = de__handle_index(en->body.id);
        if (en->body.id == 0 || i >= s->pose_cap)
            continue;
        if (de_entity_has(s->w, en->entity, s->c_child_of))
            continue; /* DeTransform is parent-relative: not a physics pose */
        const de_transform *local =
            (const de_transform *)de_entity_get_const(s->w, en->entity, s->c_xform);
        const de_transform *cur =
            (const de_transform *)de_entity_get_const(s->w, en->entity, s->c_global);
        if (local == NULL || cur == NULL)
            continue; /* transform propagation adds DeGlobalTransform at its next run */
        const de__pecs_pose_pair *pp = &s->poses[i];
        de_transform g;
        g.scale = local->scale;
        if (last || alpha == 0.0f) {
            const de__pecs_pose *src = last ? &pp->curr : &pp->prev;
            g.position = src->position;
            g.rotation = src->rotation;
        } else {
            const f64 a = (f64)alpha;
            g.position =
                de__d3_add(pp->prev.position,
                           de__d3_scale(de__d3_sub(pp->curr.position, pp->prev.position), a));
            g.rotation = nlerp(pp->prev.rotation, pp->curr.rotation, alpha);
        }
        if (!transform_equal(cur, &g)) {
            de_transform *wg = (de_transform *)de_entity_get(s->w, en->entity, s->c_global);
            *wg = g;
        }
        if (de_entity_has(s->w, en->entity, s->c_children))
            propagate_children(s, en->entity, &g);
    }
}
