/* actions.c: the action hooks of input.c, the tick view and the public queries (DENG-84;
 * replaces the DENG-83 stub, same hook signatures).
 *
 * Frame view: de__actions_frame_update (system input.actions, PreUpdate) evaluates the active
 * maps (action_eval.c). Tick view: de__actions_fixed_tick (system input.fixed_tick, FixedUpdate)
 * latches it. A trigger of the frame view sets `pending_trigger`; the next tick reports it and
 * clears it. So a trigger shows in exactly one tick: with 0 ticks in its frame it waits for the
 * next one, with N ticks only the first reports it. Two triggers before one tick merge into one.
 * `held` and `value` of the tick view are the frame view's at the tick.
 *
 * Queries pick the view by the calling thread: a FixedUpdate system reads the tick view, every
 * other caller the frame view (de__ecs_current_schedule). */
#include "action_internal.h"

#include "ecs/in_schedule_internal.h"

#include <dengine/de_math.h>

#include <string.h>

/* ---------- Hooks (input.c) ---------- */

de_result de__actions_init(de_input *in) {
    /* The action state is allocated by the first de_action_map_create, so an input state
     * without action maps stays one block. */
    in->actions = NULL;
    return DE_OK;
}

void de__actions_shutdown(de_input *in) {
    if (in == NULL || in->actions == NULL)
        return;
    de__context_shutdown(in);
    DE_FREE(&in->alloc, in->actions, sizeof *in->actions);
    in->actions = NULL;
}

void de__actions_frame_update(de_input *in, f32 dt) {
    struct de__actions *s = de__actions_state(in);
    if (s == NULL)
        return;
    s->frames++;
    de__actions_evaluate(in, dt);
}

void de__actions_fixed_tick(de_input *in, u64 tick) {
    struct de__actions *s = de__actions_state(in);
    if (s == NULL)
        return;
    s->ticks++;
    s->last_tick = tick;
    for (u32 i = 0; i < s->action_count; ++i) {
        de__action *act = &s->actions[i];
        act->tick.triggered = act->pending_trigger;
        act->pending_trigger = false;
        act->tick.held = act->frame.held;
        act->tick.value = act->frame.value;
    }
    if (s->tick_override != NULL)
        s->tick_override(in, tick, s->tick_override_user);
}

/* ---------- Tick view (internal) ---------- */

bool de__action_tick_state_get(const de_input *in, de_action a, de__action_state *out) {
    if (out == NULL)
        return false;
    const de__action *act = de__action_get_const(in, a);
    if (act == NULL) {
        memset(out, 0, sizeof *out);
        return false;
    }
    *out = act->tick;
    return true;
}

bool de__action_tick_state_set(de_input *in, de_action a, const de__action_state *st) {
    de__action *act = de__action_get(in, a);
    if (act == NULL || st == NULL)
        return false;
    act->tick = *st;
    return true;
}

bool de__action_frame_state_get(const de_input *in, de_action a, de__action_state *out) {
    if (out == NULL)
        return false;
    const de__action *act = de__action_get_const(in, a);
    if (act == NULL) {
        memset(out, 0, sizeof *out);
        return false;
    }
    *out = act->frame;
    return true;
}

void de__action_tick_override(de_input *in, void (*fn)(de_input *in, u64 tick, void *user),
                              void *user) {
    struct de__actions *s = de__actions_get_or_create(in);
    if (s == NULL)
        return;
    s->tick_override = fn;
    s->tick_override_user = fn != NULL ? user : NULL;
}

bool de__actions_in_fixed_update(void) {
    return de__ecs_current_schedule() == (int)DE_SCHEDULE_FIXED_UPDATE;
}

/* ---------- Queries ---------- */

static const de__action_state *view_of(const de_input *in, de_action a, de_action_type *type) {
    const de__action *act = de__action_get_const(in, a);
    if (act == NULL)
        return NULL;
    if (type != NULL)
        *type = act->type;
    return de__actions_in_fixed_update() ? &act->tick : &act->frame;
}

bool de_action_triggered(const de_input *in, de_action a) {
    const de__action_state *v = view_of(in, a, NULL);
    return v != NULL && v->triggered;
}

bool de_action_held(const de_input *in, de_action a) {
    const de__action_state *v = view_of(in, a, NULL);
    return v != NULL && v->held;
}

f32 de_action_value(const de_input *in, de_action a) {
    de_action_type type = DE_ACTION_BUTTON;
    const de__action_state *v = view_of(in, a, &type);
    if (v == NULL)
        return 0.0f;
    if (type == DE_ACTION_AXIS2D)
        return de_dsqrt(v->value.x * v->value.x + v->value.y * v->value.y);
    return v->value.x;
}

de_vec2 de_action_value2(const de_input *in, de_action a) {
    const de__action_state *v = view_of(in, a, NULL);
    if (v == NULL) {
        const de_vec2 zero = {0.0f, 0.0f};
        return zero;
    }
    return v->value;
}
