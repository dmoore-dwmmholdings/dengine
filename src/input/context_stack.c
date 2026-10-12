/* context_stack.c: which action maps are active (DENG-84, minimal version).
 *
 * Pushed maps are active, in push order; popping a map deactivates it and resets its actions.
 * No input is ever consumed. DENG-85 replaces this file with context stacks (named context sets,
 * priorities, blocking) and keeps the de__context_* signatures of action_internal.h, so the
 * evaluation in action_eval.c does not change. */
#include "action_internal.h"

void de__context_init(de_input *in) {
    in->actions->active_count = 0;
}

void de__context_shutdown(de_input *in) {
    (void)in;
}

u32 de__context_active_maps(de_input *in, u32 *out, u32 max) {
    const struct de__actions *s = in->actions;
    u32 n = s->active_count < max ? s->active_count : max;
    for (u32 i = 0; i < n; ++i)
        out[i] = s->active[i];
    return n;
}

void de__context_frame_begin(de_input *in) {
    (void)in;
}

bool de__context_binding_consumed(de_input *in, u32 map, u32 action, u32 binding,
                                  const de_binding *b) {
    (void)in;
    (void)map;
    (void)action;
    (void)binding;
    (void)b;
    return false;
}

void de__context_binding_used(de_input *in, u32 map, u32 action, u32 binding, const de_binding *b) {
    (void)in;
    (void)map;
    (void)action;
    (void)binding;
    (void)b;
}

bool de__context_map_active(const de_input *in, u32 map) {
    const struct de__actions *s = de__actions_state_const(in);
    if (s == NULL)
        return false;
    for (u32 i = 0; i < s->active_count; ++i)
        if (s->active[i] == map)
            return true;
    return false;
}

void de_action_map_push(de_input *in, de_action_map m) {
    const u32 mi = de__action_map_index(in, m);
    if (mi == DE__ACTION_NONE || de__context_map_active(in, mi))
        return;
    struct de__actions *s = in->actions;
    if (s->active_count < DE_ACTION_MAX_MAPS)
        s->active[s->active_count++] = mi;
}

void de_action_map_pop(de_input *in, de_action_map m) {
    const u32 mi = de__action_map_index(in, m);
    if (mi == DE__ACTION_NONE)
        return;
    struct de__actions *s = in->actions;
    u32 w = 0;
    for (u32 i = 0; i < s->active_count; ++i)
        if (s->active[i] != mi)
            s->active[w++] = s->active[i];
    if (w == s->active_count)
        return; /* was not active */
    s->active_count = w;
    for (u32 i = s->maps[mi].first_action; i != DE__ACTION_NONE; i = s->actions[i].next_in_map)
        de__action_reset(&s->actions[i]);
}
