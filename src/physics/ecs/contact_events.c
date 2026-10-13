/*
 * contact_events.c: contact and joint break events of physics.step (DENG-223, SPEC §13.1, §7.1,
 * §5.7). See the "ECS integration" block of de_physics.h for what each event holds.
 *
 * Contact events are per body pair and come from the pair-level touch flags of the contact
 * manifolds (DENG-135): a manifold with DE__MANIFOLD_PAIR_BEGIN begins a pair (it is the pair's
 * first manifold in key order; the rest of the pair's manifolds follow it), an end record with
 * DE__CONTACT_END_PAIR ends one. Each flag is set exactly once per transition, so each event is
 * sent once. The two sorted lists are merged by body pair (an end before a begin of the same
 * pair), so the events come in manifold key order whatever the worker count.
 *
 * Impulses: the manifold points hold the accumulated normal impulse of one substep (the warm
 * start of the next), so the impulse of the step is that times the substep count (DENG-217).
 *
 * Owners: a body destroyed before the step (its entity despawned) is gone when the end records
 * are read; its owner comes from the graveyard (entity_map.c).
 */
#include "physics/contact/manifold.h"
#include "physics/ecs/phys_ecs.h"

static u64 pair_key(u32 a, u32 b) {
    return ((u64)a << 32) | (u64)b;
}

/* World position of a point given in body `i`'s frame (relative to its origin), in f64. */
static de_dvec3 world_point(const de_physics_world *p, u32 i, de_vec3 local) {
    const de__dmat3 m = de__dmat3_from_quat(p->bodies.rotation[i]);
    return de__d3_add(p->bodies.position[i], de__dmat3_mul(&m, de__d3_from(local)));
}

static void send(de__pecs *s, const de_contact_event *ev) {
    if (ev->a.bits == 0 && ev->b.bits == 0)
        return;
    de_event_send(s->w, s->ev_contact, ev);
    ++s->stats.contact_events;
}

/* The begin event of the pair whose manifolds are m[0..n) (all alive, same a and b). */
static void send_begin(de__pecs *s, const de__manifold *m, u32 n) {
    const de_physics_world *p = s->p;
    de_contact_event ev;
    memset(&ev, 0, sizeof ev);
    const u32 a = m[0].key.a, b = m[0].key.b;
    ev.a = p->bodies.owner[a];
    ev.b = p->bodies.owner[b];
    if (m[0].flags & (u32)DE__MANIFOLD_TRIGGER) {
        ev.kind = DE_TRIGGER_ENTER;
        /* The visitor: the body that is not the trigger (b when both are). */
        const bool a_trigger = (p->bodies.flags[a] & DE_BODY_FLAG_TRIGGER) != 0;
        const bool b_trigger = (p->bodies.flags[b] & DE_BODY_FLAG_TRIGGER) != 0;
        ev.point =
            p->bodies.position[a_trigger && !b_trigger ? b : (b_trigger && !a_trigger ? a : b)];
        send(s, &ev);
        return;
    }
    ev.kind = DE_CONTACT_BEGIN;
    ev.normal = m[0].normal;
    de_dvec3 sum = de__d3(0.0, 0.0, 0.0);
    u32 points = 0;
    f32 impulse = 0.0f;
    for (u32 k = 0; k < n; ++k) {
        for (u32 j = 0; j < m[k].point_count; ++j) {
            const de__contact *c = &m[k].points[j];
            const de_dvec3 wa = world_point(p, a, c->local_a);
            const de_dvec3 wb = world_point(p, b, c->local_b);
            sum = de__d3_add(sum, de__d3_scale(de__d3_add(wa, wb), 0.5));
            impulse += c->normal_impulse;
            ++points;
        }
    }
    if (points > 0) {
        ev.point = de__d3_scale(sum, 1.0 / (f64)points);
    } else {
        ev.point = de__d3_scale(de__d3_add(p->bodies.position[a], p->bodies.position[b]), 0.5);
    }
    ev.impulse = impulse * (f32)p->substeps;
    send(s, &ev);
}

static void send_end(de__pecs *s, const de__contact_end *e) {
    de_contact_event ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = (e->flags & (u32)DE__CONTACT_END_TRIGGER) ? DE_TRIGGER_EXIT : DE_CONTACT_END;
    ev.a = de__pecs_owner(s, e->key.a, e->gen_a);
    ev.b = de__pecs_owner(s, e->key.b, e->gen_b);
    send(s, &ev);
}

static void send_contact_events(de__pecs *s) {
    const de__manifold *m = NULL;
    const de__contact_end *e = NULL;
    u32 nm = 0, ne = 0;
    de__contacts_manifolds(s->p, &m, &nm);
    de__contacts_ended(s->p, &e, &ne);
    u32 i = 0, j = 0;
    for (;;) {
        while (i < ne && (e[i].flags & (u32)DE__CONTACT_END_PAIR) == 0)
            ++i;
        while (j < nm && (m[j].flags & (u32)DE__MANIFOLD_PAIR_BEGIN) == 0)
            ++j;
        if (i == ne && j == nm)
            break;
        const bool take_end = j == nm || (i < ne && pair_key(e[i].key.a, e[i].key.b) <=
                                                        pair_key(m[j].key.a, m[j].key.b));
        if (take_end) {
            send_end(s, &e[i++]);
            continue;
        }
        u32 n = 1;
        while (j + n < nm && m[j + n].key.a == m[j].key.a && m[j + n].key.b == m[j].key.b)
            ++n;
        send_begin(s, &m[j], n);
        j += n;
    }
}

static void send_break_events(de__pecs *s) {
    de_physics_world *p = s->p;
    const u32 n = de_physics_joint_breaks(p, NULL, 0);
    if (n == 0)
        return;
    if (!de__pecs_grow(s, (void **)&s->breaks, &s->break_cap, n, sizeof *s->breaks)) {
        DE_WARN(de__physics_log(), "physics.step: out of memory for %u joint break events", n);
        de_error_clear();
        return;
    }
    const u32 got = de_physics_joint_breaks(p, s->breaks, n);
    for (u32 k = 0; k < got && k < n; ++k) {
        de_joint_break_event *ev = &s->breaks[k];
        u32 i;
        if (ev->a.bits == 0 && de__body_resolve(p, ev->body_a, &i))
            ev->a = p->bodies.owner[i];
        if (ev->b.bits == 0 && de__body_resolve(p, ev->body_b, &i))
            ev->b = p->bodies.owner[i];
        de_event_send(s->w, s->ev_break, ev);
        ++s->stats.break_events;
    }
}

void de__pecs_send_events(de__pecs *s) {
    if (de__contacts_get(s->p) != NULL)
        send_contact_events(s);
    send_break_events(s);
    de__pecs_graves_clear(s);
}
