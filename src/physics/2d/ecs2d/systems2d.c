#include "physics/2d/ecs2d/ecs2d_internal.h"
#include <dengine/de_debug.h>

static de__ecs2d_entry *find(de__ecs2d *s, de_entity entity) {
    for (u32 i = 0; i < s->capacity; ++i)
        if (s->entries[i].active && s->entries[i].entity.bits == entity.bits)
            return &s->entries[i];
    return NULL;
}
static void erase(de__ecs2d *s, de__ecs2d_entry *e) {
    if (e->character)
        de_character2d_destroy(s->p, e->body);
    else
        de_body2d_destroy(s->p, e->body);
    if (e->owned_shape.id)
        de_shape2d_destroy(s->p, e->owned_shape);
    e->active = false;
    e->owned_shape.id = 0;
}
static de_shape shape(de__ecs2d *s, const DeCollider2D *col, bool *owned) {
    *owned = false;
    if (col->shape.id)
        return col->shape;
    de_shape2d_desc d = {0};
    d.kind = col->kind;
    d.half_extents = col->half_extents;
    d.radius = col->radius;
    d.points = col->points;
    d.point_count = col->point_count;
    if (d.kind == DE_SHAPE2D_SEGMENT || d.kind == DE_SHAPE2D_CAPSULE) {
        d.p1 = col->points[0];
        d.p2 = col->points[1];
    }
    de_shape result = de_shape2d_create(s->p, &d);
    *owned = result.id != 0;
    return result;
}
static void sync_chunk(const de_query_chunk *chunk, void *v) {
    de_system_ctx *ctx = v;
    de__ecs2d *s = ctx->user;
    de_transform *transforms = chunk->columns[0];
    DeRigidBody2D *rbs = chunk->columns[1];
    const DeCollider2D *cols = chunk->columns[2];
    DeCharacter2D *chars = chunk->columns[3];
    for (u32 r = 0; r < chunk->count; ++r) {
        de_entity entity = chunk->entities[r];
        de__ecs2d_entry *e = find(s, entity);
        if (!chars && !cols)
            continue;
        DeRigidBody2D rb = {0};
        if (rbs)
            rb = rbs[r];
        bool character = chars != NULL;
        if (e && (e->character != character ||
                  (character && memcmp(&e->character_size, &chars[r].size, sizeof(de_vec2)) != 0) ||
                  (!character &&
                   (memcmp(&e->col, &cols[r], sizeof *cols) != 0 || rb.type != e->rb.type ||
                    rb.mass != e->rb.mass || rb.fixed_rotation != e->rb.fixed_rotation ||
                    rb.bullet != e->rb.bullet || rb.gravity_scale != e->rb.gravity_scale ||
                    rb.linear_damping != e->rb.linear_damping ||
                    rb.angular_damping != e->rb.angular_damping)))) {
            erase(s, e);
            e = NULL;
        }
        if (!e) {
            de_transform2d xf = de__plane2d_from_world(s->p->plane, &transforms[r]);
            de_body body = {0};
            de_shape owned = {0};
            if (character) {
                de_dpoint2d point = {xf.x, xf.y};
                body = de_character2d_create(s->p, &chars[r], point, DE_CHARACTER2D_PROXY_CAPSULE,
                                             entity);
            } else {
                bool own = false;
                de_shape collider = shape(s, &cols[r], &own);
                if (!collider.id)
                    continue;
                de_body2d_desc d = {0};
                d.type = rb.type;
                d.shape = collider;
                d.transform = xf;
                d.mass = rb.mass;
                d.linear_velocity = rb.linear_velocity;
                d.angular_velocity = rb.angular_velocity;
                d.linear_damping = rb.linear_damping;
                d.angular_damping = rb.angular_damping;
                d.gravity_scale = rb.gravity_scale;
                d.material = cols[r].material;
                d.filter = cols[r].filter;
                d.owner = entity;
                d.flags = DE_BODY_FLAG_ALLOW_SLEEP;
                if (rb.fixed_rotation)
                    d.flags |= DE_BODY2D_FLAG_FIXED_ROTATION;
                if (rb.bullet)
                    d.flags |= DE_BODY2D_FLAG_BULLET;
                if (cols[r].is_trigger)
                    d.flags |= DE_BODY_FLAG_TRIGGER;
                body = de_body2d_create(s->p, &d);
                if (own)
                    owned = collider;
                if (!body.id && owned.id)
                    de_shape2d_destroy(s->p, owned);
            }
            if (!body.id)
                continue;
            u32 i = de__handle_index(body.id);
            e = &s->entries[i];
            e->active = true;
            e->entity = entity;
            e->body = body;
            e->owned_shape = owned;
            e->character = character;
            if (character)
                e->character_size = chars[r].size;
            e->prev = e->curr = transforms[r];
            if (cols)
                e->col = cols[r];
            e->rb = rb;
        } else {
            /* Only game-authored pose/velocity changes enter physics; writeback caches match. */
            if (memcmp(&transforms[r].position, &e->curr.position, sizeof(de_dvec3)) != 0 ||
                memcmp(&transforms[r].rotation, &e->curr.rotation, sizeof(de_quat)) != 0) {
                de_transform2d xf = de__plane2d_from_world(s->p->plane, &transforms[r]);
                (void)de_body2d_set_transform(s->p, e->body, &xf);
                e->prev = e->curr = transforms[r];
            }
            if (rbs && (memcmp(&rb.linear_velocity, &e->rb.linear_velocity, sizeof(de_vec2)) != 0 ||
                        rb.angular_velocity != e->rb.angular_velocity))
                (void)de_body2d_set_velocity(s->p, e->body, rb.linear_velocity,
                                             rb.angular_velocity);
        }
        e->seen = true;
        e->prev = transforms[r];
        if (rbs)
            rbs[r].body = e->body;
        if (character)
            (void)de_character2d_move(s->p, e->body, &chars[r], ctx->dt, NULL);
    }
}
static de_entity owner(de__ecs2d *s, u32 i, u32 generation) {
    if (i >= s->capacity)
        return DE_ENTITY_NULL;
    if (de__body2d_alive(s->p, i) && s->p->bodies.generation[i] == generation)
        return s->p->bodies.owner[i];
    return s->entries[i].grave_generation == generation ? s->entries[i].grave : DE_ENTITY_NULL;
}
static de_dvec3 point(de__ecs2d *s, u32 i, de_vec2 local) {
    de_vec2 r = de__rot2d_apply(s->p->bodies.rotation[i], local);
    de_transform2d p = {s->p->bodies.position[i].x + r.x, s->p->bodies.position[i].y + r.y, {1, 0}};
    de_transform keep = de_transform_identity();
    const de_transform *t = de_entity_get_const(s->w, s->p->bodies.owner[i], s->transform);
    if (t)
        keep = *t;
    return de__plane2d_to_world(s->p->plane, p, keep).position;
}
static void end_event(de__ecs2d *s, const de__contact_end2d *m) {
    de_contact_event e = {0};
    e.kind = (m->flags & DE__CONTACT2D_END_TRIGGER) ? DE_TRIGGER_EXIT : DE_CONTACT_END;
    e.a = owner(s, m->key.a, m->gen_a);
    e.b = owner(s, m->key.b, m->gen_b);
    if (e.a.bits || e.b.bits)
        de_event_send(s->w, s->contact, &e);
}
static void begin_event(de__ecs2d *s, const de__manifold2d *m) {
    de_contact_event e = {0};
    e.kind = (m->flags & DE__MANIFOLD2D_TRIGGER) ? DE_TRIGGER_ENTER : DE_CONTACT_BEGIN;
    e.a = owner(s, m->key.a, m->gen_a);
    e.b = owner(s, m->key.b, m->gen_b);
    e.normal = de__plane2d_vector(s->p->plane, m->normal);
    de_vec2 local = {0};
    if (m->point_count)
        local = m->points[0].local_a;
    e.point = point(s, m->key.a, local);
    for (u32 i = 0; i < m->point_count; ++i)
        e.impulse += m->points[i].normal_impulse * s->p->substeps;
    if (e.a.bits || e.b.bits)
        de_event_send(s->w, s->contact, &e);
}
static u64 pair(u32 a, u32 b) {
    return ((u64)a << 32) | b;
}
static void events(de__ecs2d *s) {
    const de__manifold2d *m = NULL;
    const de__contact_end2d *e = NULL;
    u32 nm = 0, ne = 0;
    de__contacts2d_manifolds(s->p, &m, &nm);
    de__contacts2d_ended(s->p, &e, &ne);
    u32 i = 0, j = 0;
    while (i < nm || j < ne) {
        while (i < nm && !(m[i].flags & DE__MANIFOLD2D_PAIR_BEGIN))
            ++i;
        while (j < ne && !(e[j].flags & DE__CONTACT2D_END_PAIR))
            ++j;
        if (i == nm && j == ne)
            break;
        if (j < ne && (i == nm || pair(e[j].key.a, e[j].key.b) <= pair(m[i].key.a, m[i].key.b)))
            end_event(s, &e[j++]);
        else
            begin_event(s, &m[i++]);
    }
}
void de__ecs2d_step(de_system_ctx *ctx) {
    de__ecs2d *s = ctx->user;
    if (!(ctx->dt > 0))
        return;
    /* Remove despawns before creates, so a full world can recycle its slots in the same tick. */
    for (u32 i = 0; i < s->capacity; ++i) {
        de__ecs2d_entry *e = &s->entries[i];
        if (!e->active)
            continue;
        e->seen = false;
        u32 resolved;
        if (!de__body2d_resolve(s->p, e->body, &resolved) || !de_entity_alive(s->w, e->entity) ||
            !de_entity_has(s->w, e->entity, s->transform) ||
            (!de_entity_has(s->w, e->entity, s->col) &&
             !de_entity_has(s->w, e->entity, s->character)))
            erase(s, e);
    }
    de_query_each(s->w, ctx->queries[0], sync_chunk, ctx);
    for (u32 i = 0; i < s->capacity; ++i)
        if (s->entries[i].active && !s->entries[i].seen)
            erase(s, &s->entries[i]);
    de_physics2d_step(s->p, ctx->dt);
    for (u32 i = 0; i < s->capacity; ++i) {
        de__ecs2d_entry *e = &s->entries[i];
        if (!e->active)
            continue;
        de_transform *t = de_entity_get(s->w, e->entity, s->transform);
        if (!t)
            continue;
        de_transform2d pose;
        if (!de_body2d_get_transform(s->p, e->body, &pose))
            continue;
        *t = de__plane2d_to_world(s->p->plane, pose, *t);
        e->curr = *t;
        DeRigidBody2D *rb = de_entity_get(s->w, e->entity, s->rb);
        if (rb) {
            (void)de_body2d_get_velocity(s->p, e->body, &rb->linear_velocity,
                                         &rb->angular_velocity);
            rb->body = e->body;
            e->rb = *rb;
        }
    }
    events(s);
}
void de__ecs2d_interpolate(de_system_ctx *ctx) {
    de__ecs2d *s = ctx->user;
    f32 alpha = ctx->alpha < 0 ? 0 : (ctx->alpha > 1 ? 1 : ctx->alpha);
    for (u32 i = 0; i < s->capacity; ++i) {
        de__ecs2d_entry *e = &s->entries[i];
        if (!e->active || !de_entity_alive(s->w, e->entity))
            continue;
        de_transform *global = de_entity_get(s->w, e->entity, s->global);
        if (!global)
            continue;
        de_transform t = e->curr;
        t.position.x = e->prev.position.x + (e->curr.position.x - e->prev.position.x) * alpha;
        t.position.y = e->prev.position.y + (e->curr.position.y - e->prev.position.y) * alpha;
        t.position.z = e->prev.position.z + (e->curr.position.z - e->prev.position.z) * alpha;
        de_quat a = e->prev.rotation, b = e->curr.rotation;
        f32 dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
        f32 sb = dot < 0 ? -alpha : alpha;
        t.rotation = de_dnormalize_quat(
            (de_quat){a.x * (1 - alpha) + b.x * sb, a.y * (1 - alpha) + b.y * sb,
                      a.z * (1 - alpha) + b.z * sb, a.w * (1 - alpha) + b.w * sb});
        *global = t;
    }
}
