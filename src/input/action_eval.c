/* action_eval.c: per-frame action evaluation into the frame view (DENG-84).
 *
 * Order (deterministic, SPEC §5.7): the active maps in de__context_active_maps order, the actions
 * of each map in declaration order, the bindings of each action in index order, pads in
 * connection order. Steps per action (de_input.h, "Action maps"):
 *   1. read each binding's source in action space (+y up), unless the context stack says the
 *      input is consumed, its chord modifier is not held, or an active chord of the same map
 *      suppresses it;
 *   2. modifiers (action_modifiers.c);
 *   3. composite slots -> one composite candidate;
 *   4. the candidate with the largest magnitude wins; ties go to the lower binding index;
 *   5. actuation (magnitude >= press_threshold) and the interaction -> `triggered`.
 * Nothing here allocates: the chord list is on the stack, everything else is in the action
 * state block. */
#include "action_internal.h"

#include <dengine/de_math.h>

#include <string.h>

/* Active chords collected per map; more chords than this in one map stop suppressing. */
#define EVAL_MAX_CHORDS 128u

typedef struct eval_chord {
    de_binding_source source;
    u32 code;
} eval_chord;

static f32 absf(f32 v) {
    return v < 0.0f ? -v : v;
}

static f32 len2(de_vec2 v) {
    return v.x * v.x + v.y * v.y;
}

/* ---------- Sources ---------- */

typedef struct eval_raw {
    f32 v;      /**< 1D value */
    de_vec2 v2; /**< 2D value (2D codes) */
    bool pulse; /**< a button went down and up inside this frame */
} eval_raw;

static bool binding_is_2d(const de_binding *b) {
    switch (b->source) {
    case DE_BIND_PAD_AXIS:
        return b->code == DE_BIND_LEFT_STICK || b->code == DE_BIND_RIGHT_STICK;
    case DE_BIND_MOUSE_DELTA:
    case DE_BIND_MOUSE_WHEEL:
    case DE_BIND_GYRO:
        return b->code == DE_BIND_AXIS_XY;
    default:
        return false;
    }
}

static void read_digital(bool down, bool pressed, eval_raw *r) {
    r->v = down ? 1.0f : 0.0f;
    r->pulse = !down && pressed;
}

/* An axis-like 2D value: X, Y or XY of `v` (already in action space). */
static void read_vec(de_vec2 v, u32 code, eval_raw *r) {
    if (code == DE_BIND_AXIS_X)
        r->v = v.x;
    else if (code == DE_BIND_AXIS_Y)
        r->v = v.y;
    else
        r->v2 = v;
}

static void read_pads(const de_input *in, const de_binding *b, eval_raw *r) {
    bool any_down = false, any_pulse = false;
    f32 best = -1.0f;
    for (u32 k = 0; k < in->pad_count; ++k) {
        const de__input_pad *p = &in->pads[in->pad_order[k]];
        if (p->device == 0)
            continue;
        switch (b->source) {
        case DE_BIND_PAD_BUTTON: {
            const u32 bit = 1u << b->code;
            if ((p->buttons & bit) != 0)
                any_down = true;
            else if ((p->pressed & bit) != 0)
                any_pulse = true;
            break;
        }
        case DE_BIND_PAD_AXIS:
            if (b->code == DE_BIND_LEFT_STICK || b->code == DE_BIND_RIGHT_STICK) {
                const u32 ax =
                    b->code == DE_BIND_LEFT_STICK ? (u32)DE_AXIS_LEFT_X : (u32)DE_AXIS_RIGHT_X;
                const de_vec2 v = {p->axes[ax], p->axes[ax + 1u]};
                const f32 m = len2(v);
                if (m > best) {
                    best = m;
                    r->v2 = v;
                }
            } else {
                const f32 v = p->axes[b->code];
                if (absf(v) > best) {
                    best = absf(v);
                    r->v = v;
                }
            }
            break;
        case DE_BIND_GYRO: {
            if (!p->has_gyro)
                break;
            /* XY: aim, x = turn right (-yaw rate), y = tilt up (pitch rate). */
            const de_vec3 g = p->gyro;
            if (b->code == DE_BIND_AXIS_XY) {
                const de_vec2 v = {-g.y, g.x};
                const f32 m = len2(v);
                if (m > best) {
                    best = m;
                    r->v2 = v;
                }
            } else {
                const f32 v = b->code == DE_BIND_AXIS_X   ? g.x
                              : b->code == DE_BIND_AXIS_Y ? g.y
                                                          : g.z;
                if (absf(v) > best) {
                    best = absf(v);
                    r->v = v;
                }
            }
            break;
        }
        default:
            break;
        }
    }
    if (b->source == DE_BIND_PAD_BUTTON)
        read_digital(any_down, any_pulse, r);
}

static void read_source(const de_input *in, const de_binding *b, eval_raw *r) {
    memset(r, 0, sizeof *r);
    switch (b->source) {
    case DE_BIND_KEY:
        read_digital(de_key_down(in, (de_key)b->code), de_key_pressed(in, (de_key)b->code), r);
        break;
    case DE_BIND_MOUSE_BUTTON:
        read_digital(de_mouse_down(in, (de_mouse_button)b->code),
                     de_mouse_pressed(in, (de_mouse_button)b->code), r);
        break;
    case DE_BIND_MOUSE_DELTA: {
        /* Action space is +y up; the mouse reports +y down. */
        const de_vec2 d = {in->mouse_delta.x, -in->mouse_delta.y};
        read_vec(d, b->code, r);
        break;
    }
    case DE_BIND_MOUSE_WHEEL:
        read_vec(in->mouse_wheel, b->code, r);
        break;
    case DE_BIND_PAD_BUTTON:
    case DE_BIND_PAD_AXIS:
    case DE_BIND_GYRO:
        read_pads(in, b, r);
        break;
    default:
        break;
    }
}

/* ---------- Chords ---------- */

static u32 collect_chords(const de_input *in, const struct de__actions *s, u32 map,
                          eval_chord *out) {
    u32 n = 0;
    for (u32 i = s->maps[map].first_action; i != DE__ACTION_NONE; i = s->actions[i].next_in_map) {
        const de__action *act = &s->actions[i];
        for (u32 j = 0; j < act->binding_count; ++j) {
            const de_binding *b = &s->bindings[act->first_binding + j];
            if (b->modifier_key == 0 || !de_key_down(in, (de_key)b->modifier_key))
                continue;
            if (n < EVAL_MAX_CHORDS) {
                out[n].source = b->source;
                out[n].code = b->code;
                ++n;
            }
        }
    }
    return n;
}

static bool chord_suppresses(const eval_chord *chords, u32 n, const de_binding *b) {
    for (u32 i = 0; i < n; ++i)
        if (chords[i].source == b->source && chords[i].code == b->code)
            return true;
    return false;
}

/* ---------- Interactions ---------- */

/* Updates the interaction tracking of `act` for this frame and returns `triggered`. `now`: the
 * action is actuated after this frame; `pulse`: it was actuated and released inside the frame. */
static bool interact(de__action *act, bool now, bool pulse, f32 dt) {
    de__action_track *t = &act->track;
    const bool prev = t->actuated;
    const bool rise = !prev && now;
    const bool fall = prev && !now;
    pulse = pulse && !prev && !now;

    if (prev)
        t->held_time += dt;
    if (t->tap_count > 0)
        t->since_tap += dt;
    if (rise) {
        t->held_time = 0.0f;
        t->hold_fired = false;
    }

    bool trig = false;
    switch (act->interaction) {
    case DE_INTERACT_PRESS:
        trig = rise || pulse;
        break;
    case DE_INTERACT_RELEASE:
        trig = fall || pulse;
        break;
    case DE_INTERACT_HOLD:
        if (now && !t->hold_fired && t->held_time >= act->hold_seconds) {
            trig = true;
            t->hold_fired = true;
        }
        break;
    case DE_INTERACT_TAP:
        trig = (fall && t->held_time < act->tap_seconds) || pulse;
        break;
    case DE_INTERACT_DOUBLE_TAP: {
        if (rise || pulse) {
            t->second_ok = t->tap_count == 1u && t->since_tap <= act->double_tap_seconds;
            if (!t->second_ok)
                t->tap_count = 0;
        }
        const bool tap = (fall && t->held_time < act->tap_seconds) || pulse;
        if (tap) {
            if (t->second_ok) {
                trig = true;
                t->tap_count = 0;
            } else {
                t->tap_count = 1;
                t->since_tap = 0.0f;
            }
            t->second_ok = false;
        } else if (fall) {
            t->tap_count = 0; /* a long press ends the pair */
            t->second_ok = false;
        }
        if (t->tap_count == 1u && !now && t->since_tap > act->double_tap_seconds)
            t->tap_count = 0;
        break;
    }
    default:
        break;
    }
    t->actuated = now;
    return trig;
}

/* ---------- One action ---------- */

static void eval_action(de_input *in, struct de__actions *s, u32 map, u32 ai,
                        const eval_chord *chords, u32 chord_count, f32 dt) {
    de__action *act = &s->actions[ai];
    f32 parts[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    u32 composite_first = DE__ACTION_NONE;
    f32 best_mag = 0.0f;
    de_vec2 best = {0.0f, 0.0f};
    u32 best_index = DE__ACTION_NONE;
    bool any_pulse = false;
    u32 used_mask = 0; /* bindings whose raw input was active */

    for (u32 j = 0; j < act->binding_count; ++j) {
        const de_binding *b = &s->bindings[act->first_binding + j];
        if (de__context_binding_consumed(in, map, ai, j, b))
            continue;
        if (b->modifier_key != 0) {
            if (!de_key_down(in, (de_key)b->modifier_key))
                continue;
        } else if (chord_count != 0 && chord_suppresses(chords, chord_count, b)) {
            continue;
        }
        eval_raw raw;
        read_source(in, b, &raw);
        const bool is2d = binding_is_2d(b);
        if (raw.pulse || raw.v != 0.0f || raw.v2.x != 0.0f || raw.v2.y != 0.0f)
            used_mask |= 1u << j;
        if (raw.pulse)
            any_pulse = true;
        const bool bounded = b->source == DE_BIND_PAD_AXIS;

        if (act->type != DE_ACTION_BUTTON && b->composite_slot >= 0) {
            f32 v = de__modifiers_apply_1d(b, raw.v, bounded);
            if (v < 0.0f)
                v = 0.0f;
            if (v > parts[b->composite_slot])
                parts[b->composite_slot] = v;
            if (composite_first == DE__ACTION_NONE)
                composite_first = j;
            continue;
        }

        de_vec2 val = {0.0f, 0.0f};
        f32 mag;
        if (is2d) {
            val = de__modifiers_apply_2d(b, raw.v2, bounded);
            mag = de_dsqrt(len2(val));
            if (act->type == DE_ACTION_BUTTON) {
                val.x = mag;
                val.y = 0.0f;
            }
        } else {
            val.x = de__modifiers_apply_1d(b, raw.v, bounded);
            if (act->type == DE_ACTION_BUTTON && val.x < 0.0f)
                val.x = 0.0f;
            mag = absf(val.x);
        }
        /* Strictly larger: on a tie the earlier (lower) binding index keeps the win. */
        if (mag > best_mag) {
            best_mag = mag;
            best = val;
            best_index = j;
        }
    }

    if (composite_first != DE__ACTION_NONE) {
        de_vec2 c = {0.0f, 0.0f};
        f32 mag;
        if (act->type == DE_ACTION_AXIS2D) {
            c.x = parts[DE_COMPOSITE_RIGHT] - parts[DE_COMPOSITE_LEFT];
            c.y = parts[DE_COMPOSITE_UP] - parts[DE_COMPOSITE_DOWN];
            mag = de_dsqrt(len2(c));
            if (mag > 1.0f) {
                c.x /= mag;
                c.y /= mag;
                mag = 1.0f;
            }
        } else {
            c.x = parts[DE_COMPOSITE_POSITIVE] - parts[DE_COMPOSITE_NEGATIVE];
            mag = absf(c.x);
        }
        if (mag > best_mag || (mag == best_mag && mag > 0.0f && composite_first < best_index)) {
            best_mag = mag;
            best = c;
            best_index = composite_first;
        }
    }

    const bool now = best_mag > 0.0f && best_mag >= act->press_threshold;
    const bool trig = interact(act, now, any_pulse, dt);
    act->frame.triggered = trig;
    act->frame.held = now;
    act->frame.value = best;
    act->winning_binding = best_index;
    if (trig)
        act->pending_trigger = true;

    for (u32 j = 0; used_mask != 0 && j < act->binding_count; ++j)
        if ((used_mask & (1u << j)) != 0)
            de__context_binding_used(in, map, ai, j, &s->bindings[act->first_binding + j]);
}

static bool action_idle(const de__action *act) {
    return !act->frame.triggered && !act->frame.held && act->frame.value.x == 0.0f &&
           act->frame.value.y == 0.0f && !act->track.actuated && act->track.tap_count == 0 &&
           !act->pending_trigger && !act->tick.triggered && !act->tick.held;
}

void de__actions_evaluate(de_input *in, f32 dt) {
    struct de__actions *s = de__actions_state(in);
    if (s == NULL)
        return;
    if (!(dt >= 0.0f) || dt > 1.0e6f)
        dt = 0.0f;
    de__context_frame_begin(in);

    u32 active[DE_ACTION_MAX_MAPS];
    const u32 n = de__context_active_maps(in, active, DE_ACTION_MAX_MAPS);
    bool is_active[DE_ACTION_MAX_MAPS];
    memset(is_active, 0, sizeof is_active);

    eval_chord chords[EVAL_MAX_CHORDS];
    for (u32 k = 0; k < n; ++k) {
        const u32 map = active[k];
        if (map >= s->map_count || is_active[map])
            continue;
        is_active[map] = true;
        const u32 chord_count = collect_chords(in, s, map, chords);
        for (u32 i = s->maps[map].first_action; i != DE__ACTION_NONE; i = s->actions[i].next_in_map)
            eval_action(in, s, map, i, chords, chord_count, dt);
    }
    /* Actions of inactive maps read as idle. */
    for (u32 map = 0; map < s->map_count; ++map) {
        if (is_active[map])
            continue;
        for (u32 i = s->maps[map].first_action; i != DE__ACTION_NONE; i = s->actions[i].next_in_map)
            if (!action_idle(&s->actions[i]))
                de__action_reset(&s->actions[i]);
    }
}
