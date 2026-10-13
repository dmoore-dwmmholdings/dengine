/*
 * systems.c: the two ECS systems of 3D physics (DENG-223, SPEC §13.1, §7.3, §10.2, §5.7).
 *
 * physics.step (DE_SCHEDULE_FIXED_UPDATE, DE_SYSTEM_DETERMINISTIC) runs the passes of one tick in
 * this order: validate (entity index order), sync (change queries in archetype / row order),
 * character moves (DeCharacter query order), the poses before the step, de_physics_step,
 * kinematic targets, write-back (entity index order), events (manifold key order, then joint
 * index order). physics.interpolate (DE_SCHEDULE_POST_UPDATE, after transform propagation) is in
 * interpolation.c.
 *
 * Access. The step's queries hold read terms only, so iterating them marks nothing; the writes
 * (DeRigidBody.body, the write-back, the DeCharacter outputs) go through mutable gets of single
 * entities, or through the DeCharacter query, which marks every character (they move every tick
 * anyway). The first query is never iterated: it declares the write access of those gets.
 */
#include "physics/ecs/phys_ecs.h"

/* ================================ Characters =========================================== */

typedef struct char_pass {
    de__pecs *s;
    f32 dt;
} char_pass;

static void move_chunk(const de_query_chunk *chunk, void *user) {
    const char_pass *x = (const char_pass *)user;
    DeCharacter *chars = (DeCharacter *)chunk->columns[0];
    for (u32 r = 0; r < chunk->count; ++r) {
        const de__pecs_entry *en = de__pecs_find(x->s, chunk->entities[r]);
        if (en == NULL || en->kind != DE__PECS_CHARACTER)
            continue;
        if (de_character_move(x->s->p, en->body, &chars[r], x->dt, NULL) != DE_OK) {
            const de_error *err = de_error_last();
            DE_WARN(de__physics_log(), "physics.step: entity %u:%u: character move failed: %s",
                    de_entity_index(en->entity), de_entity_generation(en->entity),
                    err != NULL ? err->message : "?");
            de_error_clear();
        }
    }
}

/* ================================ physics.step ======================================== */

void de__pecs_step_system(de_system_ctx *ctx) {
    de__pecs *s = (de__pecs *)ctx->user;
    const f32 dt = ctx->dt;
    if (!(dt > 0.0f) || !de__f32_finite(dt))
        return;
    de__pecs_validate(s);
    de__pecs_sync(s, ctx, dt);

    char_pass cp;
    cp.s = s;
    cp.dt = dt;
    de_query_each(s->w, ctx->queries[DE__PECS_Q_ALL_CHAR], move_chunk, &cp);

    de__pecs_capture_prev(s);
    de_physics_step(s->p, dt);
    de__pecs_finish_targets(s);
    de__pecs_write_back(s);
    s->stepped = true;
    de__pecs_send_events(s);
}

/* ================================ Registration ======================================== */

de_result de__pecs_add_systems(de__pecs *s) {
    de_physics_world *p = s->p;

    /* physics.step: queries in DE__PECS_Q_* order. */
    const de_query_term decl_terms[] = {
        {s->c_rb, DE_OPTIONAL_WRITE},
        {s->c_col, DE_OPTIONAL_READ},
        {s->c_char, DE_OPTIONAL_WRITE},
        {s->c_xform, DE_OPTIONAL_WRITE},
    };
    const de_query_term t_rb[] = {{s->c_rb, DE_READ}};
    const de_query_term t_col[] = {{s->c_col, DE_READ}};
    const de_query_term t_char_r[] = {{s->c_char, DE_READ}};
    const de_query_term t_char_w[] = {{s->c_char, DE_WRITE}};
    const de_query_term t_xform[] = {{s->c_xform, DE_READ}};
    const de_query_filter f_rb[] = {{s->c_rb, DE_CHANGED}};
    const de_query_filter f_col[] = {{s->c_col, DE_CHANGED}};
    const de_query_filter f_char[] = {{s->c_char, DE_CHANGED}};
    const de_query_filter f_xform_col[] = {{s->c_xform, DE_CHANGED}, {s->c_col, DE_WITH}};
    const de_query_filter f_xform_char[] = {{s->c_xform, DE_CHANGED}, {s->c_char, DE_WITH}};
    de_query_desc q[DE__PECS_Q_COUNT];
    memset(q, 0, sizeof q);
    q[DE__PECS_Q_DECL].terms = decl_terms;
    q[DE__PECS_Q_DECL].term_count = 4;
    q[DE__PECS_Q_RB_CHANGED].terms = t_rb;
    q[DE__PECS_Q_RB_CHANGED].term_count = 1;
    q[DE__PECS_Q_RB_CHANGED].filters = f_rb;
    q[DE__PECS_Q_RB_CHANGED].filter_count = 1;
    q[DE__PECS_Q_COL_CHANGED].terms = t_col;
    q[DE__PECS_Q_COL_CHANGED].term_count = 1;
    q[DE__PECS_Q_COL_CHANGED].filters = f_col;
    q[DE__PECS_Q_COL_CHANGED].filter_count = 1;
    q[DE__PECS_Q_CHAR_CHANGED].terms = t_char_r;
    q[DE__PECS_Q_CHAR_CHANGED].term_count = 1;
    q[DE__PECS_Q_CHAR_CHANGED].filters = f_char;
    q[DE__PECS_Q_CHAR_CHANGED].filter_count = 1;
    q[DE__PECS_Q_XFORM_COL].terms = t_xform;
    q[DE__PECS_Q_XFORM_COL].term_count = 1;
    q[DE__PECS_Q_XFORM_COL].filters = f_xform_col;
    q[DE__PECS_Q_XFORM_COL].filter_count = 2;
    q[DE__PECS_Q_XFORM_CHAR].terms = t_xform;
    q[DE__PECS_Q_XFORM_CHAR].term_count = 1;
    q[DE__PECS_Q_XFORM_CHAR].filters = f_xform_char;
    q[DE__PECS_Q_XFORM_CHAR].filter_count = 2;
    q[DE__PECS_Q_ALL_COL].terms = t_col;
    q[DE__PECS_Q_ALL_COL].term_count = 1;
    q[DE__PECS_Q_ALL_CHAR].terms = t_char_w;
    q[DE__PECS_Q_ALL_CHAR].term_count = 1;

    const de_resource_access res_w[] = {{p->res_physics_world, DE_WRITE}};
    const de_event_type events[] = {s->ev_contact, s->ev_break};

    de_system_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.name = DE_PHYSICS_STEP_SYSTEM;
    sd.schedule = DE_SCHEDULE_FIXED_UPDATE;
    sd.fn = de__pecs_step_system;
    sd.user = s;
    sd.flags = DE_SYSTEM_DETERMINISTIC;
    sd.queries = q;
    sd.query_count = DE__PECS_Q_COUNT;
    sd.resources = res_w;
    sd.resource_count = 1;
    sd.writes_events = events;
    sd.writes_event_count = 2;
    s->sys_step = de_system_add(s->w, &sd);
    if (!DE_HANDLE_VALID(s->sys_step))
        return DE_FAIL_WRAP(de_error_last() != NULL ? de_error_last()->result : DE_ERR_INTERNAL,
                            "de_physics_attach: adding " DE_PHYSICS_STEP_SYSTEM);

    /* physics.interpolate: access declaration only (it works through single-entity gets). */
    const de_query_term interp_terms[] = {
        {s->c_rb, DE_OPTIONAL_READ},       {s->c_xform, DE_OPTIONAL_READ},
        {s->c_global, DE_OPTIONAL_WRITE},  {s->c_child_of, DE_OPTIONAL_READ},
        {s->c_children, DE_OPTIONAL_READ},
    };
    de_query_desc iq;
    memset(&iq, 0, sizeof iq);
    iq.terms = interp_terms;
    iq.term_count = 5;
    const de_resource_access res_r[] = {{p->res_physics_world, DE_READ}};
    const char *const after[] = {DE_TRANSFORM_PROPAGATE_SYSTEM};
    memset(&sd, 0, sizeof sd);
    sd.name = DE_PHYSICS_INTERPOLATE_SYSTEM;
    sd.schedule = DE_SCHEDULE_POST_UPDATE;
    sd.fn = de__pecs_interpolate_system;
    sd.user = s;
    sd.queries = &iq;
    sd.query_count = 1;
    sd.resources = res_r;
    sd.resource_count = 1;
    sd.after = after;
    sd.after_count = 1;
    s->sys_interp = de_system_add(s->w, &sd);
    if (!DE_HANDLE_VALID(s->sys_interp)) {
        const de_result r = de_error_last() != NULL ? de_error_last()->result : DE_ERR_INTERNAL;
        de_system_remove(s->w, s->sys_step);
        s->sys_step = (de_system){0};
        return DE_FAIL_WRAP(r, "de_physics_attach: adding " DE_PHYSICS_INTERPOLATE_SYSTEM);
    }
    return DE_OK;
}
