/* context_stack.c: context stacks and named context sets (DENG-85; replaces the DENG-84 minimal
 * version and keeps the de__context_* signatures of action_internal.h, so action_eval.c does not
 * change). State: de__context_state (context_internal.h), inside struct de__actions.
 *
 * Per frame (de__actions_evaluate calls the hooks in this order):
 *   1. de__context_frame_begin: the requested stack becomes the applied stack. A map that left
 *      starts releasing; a map that joined latches the bindings whose input is still held from
 *      before. Releasing maps whose actions are idle (and whose last trigger reached a tick) stop
 *      releasing; evaluation then resets them like any inactive map. The consumed inputs clear.
 *   2. de__context_active_maps: the applied stack from the top down, then the releasing maps.
 *   3. Per map, per action, per binding: de__context_binding_consumed before the binding is read,
 *      de__context_binding_used after its action when its raw input was active. The inputs that
 *      a blocking map uses collect in `pending`; when evaluation moves on to the next map they
 *      join `consumed`, which the maps below read. So maps never block themselves.
 *
 * Latches. A latched binding reads 0 until its input is released. A binding latches when its map
 * joins while its input is held (down, and not pressed in this frame: a press after the request
 * is meant for the new context), and when a higher map consumes its input while it is down (so
 * the button that closes a menu does not then fire the action below). Mouse motion, the wheel and
 * the gyro never latch. A latched binding of a blocking map still consumes its input.
 *
 * Nothing here allocates; all state is in fixed arrays, walked in index or stack order. */
#include "action_internal.h"

#include <dengine/de_math.h>

#include <string.h>

#if defined(DE_DEBUG) || defined(DE_DEV)
#define CTX_DEV_WARN(in, ...) DE_WARN((in)->log, __VA_ARGS__)
#else
#define CTX_DEV_WARN(in, ...) ((void)0)
#endif

static de__context_state *ctx_of(de_input *in) {
    return &in->actions->context;
}

/* ---------- Raw inputs as (source, code) bits ---------- */

static void bit_set(de__context_inputs *s, u32 src, u32 code) {
    if (src < (u32)DE_BIND_SOURCE_COUNT_ && code < DE__CONTEXT_CODES)
        s->bits[src][code / 64u] |= (u64)1 << (code % 64u);
}

static bool bit_get(const de__context_inputs *s, u32 src, u32 code) {
    if (src >= (u32)DE_BIND_SOURCE_COUNT_ || code >= DE__CONTEXT_CODES)
        return false;
    return (s->bits[src][code / 64u] & ((u64)1 << (code % 64u))) != 0;
}

/* Marks the input of `b`, and the codes that overlap it: a whole stick and its two axes, XY and
 * its X and Y. Marking is symmetric, so a lookup only tests the exact code. */
static void mark_input(de__context_inputs *s, const de_binding *b) {
    const u32 src = (u32)b->source;
    const u32 code = b->code;
    bit_set(s, src, code);
    switch (b->source) {
    case DE_BIND_PAD_AXIS:
        if (code == DE_BIND_LEFT_STICK) {
            bit_set(s, src, DE_AXIS_LEFT_X);
            bit_set(s, src, DE_AXIS_LEFT_Y);
        } else if (code == DE_BIND_RIGHT_STICK) {
            bit_set(s, src, DE_AXIS_RIGHT_X);
            bit_set(s, src, DE_AXIS_RIGHT_Y);
        } else if (code == DE_AXIS_LEFT_X || code == DE_AXIS_LEFT_Y) {
            bit_set(s, src, DE_BIND_LEFT_STICK);
        } else if (code == DE_AXIS_RIGHT_X || code == DE_AXIS_RIGHT_Y) {
            bit_set(s, src, DE_BIND_RIGHT_STICK);
        }
        break;
    case DE_BIND_MOUSE_DELTA:
    case DE_BIND_MOUSE_WHEEL:
    case DE_BIND_GYRO:
        if (code == DE_BIND_AXIS_XY) {
            bit_set(s, src, DE_BIND_AXIS_X);
            bit_set(s, src, DE_BIND_AXIS_Y);
        } else if (code == DE_BIND_AXIS_X || code == DE_BIND_AXIS_Y) {
            bit_set(s, src, DE_BIND_AXIS_XY);
        }
        break;
    default:
        break;
    }
}

static void inputs_or(de__context_inputs *dst, const de__context_inputs *src) {
    for (u32 s = 0; s < (u32)DE_BIND_SOURCE_COUNT_; ++s)
        for (u32 w = 0; w < DE__CONTEXT_CODE_WORDS; ++w)
            dst->bits[s][w] |= src->bits[s][w];
}

/* ---------- Is the input of a binding down? ---------- */

/* A pad axis binding is down while its value after the modifiers reaches the action's
 * press_threshold on some pad (the same actuation test as evaluation, per binding). */
static bool pad_axis_down(const de_input *in, const de__action *act, const de_binding *b) {
    const bool floor0 = act->type == DE_ACTION_BUTTON || b->composite_slot >= 0;
    for (u32 k = 0; k < in->pad_count; ++k) {
        const de__input_pad *p = &in->pads[in->pad_order[k]];
        if (p->device == 0)
            continue;
        f32 m;
        if (b->code == DE_BIND_LEFT_STICK || b->code == DE_BIND_RIGHT_STICK) {
            const u32 ax =
                b->code == DE_BIND_LEFT_STICK ? (u32)DE_AXIS_LEFT_X : (u32)DE_AXIS_RIGHT_X;
            const de_vec2 raw = {p->axes[ax], p->axes[ax + 1u]};
            const de_vec2 v = de__modifiers_apply_2d(b, raw, true);
            m = de_dsqrt(v.x * v.x + v.y * v.y);
        } else if (b->code < (u32)DE_PAD_AXIS_COUNT_) {
            const f32 v = de__modifiers_apply_1d(b, p->axes[b->code], true);
            m = floor0 ? (v > 0.0f ? v : 0.0f) : (v < 0.0f ? -v : v);
        } else {
            continue;
        }
        if (m > 0.0f && m >= act->press_threshold)
            return true;
    }
    return false;
}

/* True while the input of `b` is down. `continuous`: an input that went down in this frame does
 * not count (it was not held from before). Motion inputs (mouse delta, wheel, gyro) never count:
 * they have no held state to carry over. */
static bool input_down(const de_input *in, const de__action *act, const de_binding *b,
                       bool continuous) {
    switch (b->source) {
    case DE_BIND_KEY:
        if (b->code >= (u32)DE_KEY_COUNT_)
            return false;
        return de_key_down(in, (de_key)b->code) &&
               !(continuous && de_key_pressed(in, (de_key)b->code));
    case DE_BIND_MOUSE_BUTTON:
        if (b->code >= (u32)DE_MOUSE_BUTTON_COUNT_)
            return false;
        return de_mouse_down(in, (de_mouse_button)b->code) &&
               !(continuous && de_mouse_pressed(in, (de_mouse_button)b->code));
    case DE_BIND_PAD_BUTTON: {
        if (b->code >= 32u)
            return false;
        const u32 bit = 1u << b->code;
        for (u32 k = 0; k < in->pad_count; ++k) {
            const de__input_pad *p = &in->pads[in->pad_order[k]];
            if (p->device != 0 && (p->buttons & bit) != 0 &&
                !(continuous && (p->pressed & bit) != 0))
                return true;
        }
        return false;
    }
    case DE_BIND_PAD_AXIS:
        return pad_axis_down(in, act, b);
    default:
        return false;
    }
}

/* ---------- Applying the requested stack ---------- */

static bool requested_has_map(const de__context_state *c, u32 map) {
    for (u32 i = 0; i < c->requested_count; ++i)
        if (c->requested[i].map == map)
            return true;
    return false;
}

/* Latches every binding of `map` whose input is held from before the map joined. */
static void latch_held(de_input *in, struct de__actions *s, u32 map) {
    de__context_state *c = &s->context;
    for (u32 i = s->maps[map].first_action; i != DE__ACTION_NONE; i = s->actions[i].next_in_map) {
        const de__action *act = &s->actions[i];
        u16 mask = 0;
        for (u32 j = 0; j < act->binding_count; ++j)
            if (input_down(in, act, &s->bindings[act->first_binding + j], true))
                mask = (u16)(mask | (1u << j));
        c->latched[i] = mask;
    }
}

/* A releasing map is done when none of its actions is actuated or has a trigger waiting for a
 * tick (so the tick view still sees the release trigger). */
static bool map_drained(const struct de__actions *s, u32 map) {
    for (u32 i = s->maps[map].first_action; i != DE__ACTION_NONE; i = s->actions[i].next_in_map)
        if (s->actions[i].track.actuated || s->actions[i].pending_trigger)
            return false;
    return true;
}

static void apply_requested(de_input *in, struct de__actions *s) {
    de__context_state *c = &s->context;
    const bool changed =
        c->requested_count != c->applied_count ||
        memcmp(c->requested, c->applied, sizeof c->requested[0] * c->requested_count) != 0;

    bool now[DE_ACTION_MAX_MAPS];
    memset(now, 0, sizeof now);
    for (u32 i = 0; i < c->requested_count; ++i)
        now[c->requested[i].map] = true;

    /* Transitions, in map index order (each map is independent). */
    for (u32 m = 0; m < s->map_count; ++m) {
        if (c->on_stack[m] && !now[m]) {
            c->releasing[m] = true;
        } else if (!c->on_stack[m] && now[m]) {
            c->releasing[m] = false;
            latch_held(in, s, m);
        }
        c->on_stack[m] = now[m];
        c->blocks[m] = false;
    }
    memcpy(c->applied, c->requested, sizeof c->requested[0] * c->requested_count);
    c->applied_count = c->requested_count;

    /* Blocking: a map counts at its topmost entry, so walk up and let later entries win. */
    for (u32 i = 0; i < c->applied_count; ++i) {
        const de__context_entry *e = &c->applied[i];
        u32 flags = c->map_flags[e->map];
        if (e->context != DE__CONTEXT_NONE && e->context < c->set_count)
            flags |= c->sets[e->context].flags;
        c->blocks[e->map] = (flags & DE_INPUT_CONTEXT_PASSTHROUGH) == 0;
    }

    /* The DENG-84 fields: each applied map once, by its topmost entry, bottom to top. */
    s->active_count = 0;
    for (u32 i = 0; i < c->applied_count; ++i) {
        bool topmost = true;
        for (u32 k = i + 1u; k < c->applied_count; ++k)
            if (c->applied[k].map == c->applied[i].map)
                topmost = false;
        if (topmost)
            s->active[s->active_count++] = c->applied[i].map;
    }

    for (u32 m = 0; m < s->map_count; ++m)
        if (c->releasing[m] && map_drained(s, m))
            c->releasing[m] = false;
    if (changed)
        c->applies++;
}

/* ---------- Hooks (action_internal.h) ---------- */

void de__context_init(de_input *in) {
    struct de__actions *s = in->actions;
    memset(&s->context, 0, sizeof s->context);
    s->context.eval_map = DE__CONTEXT_NONE;
    s->active_count = 0;
}

void de__context_shutdown(de_input *in) {
    (void)in;
}

void de__context_frame_begin(de_input *in) {
    struct de__actions *s = in->actions;
    apply_requested(in, s);
    de__context_state *c = &s->context;
    memset(&c->consumed, 0, sizeof c->consumed);
    memset(&c->pending, 0, sizeof c->pending);
    c->eval_map = DE__CONTEXT_NONE;
}

u32 de__context_active_maps(de_input *in, u32 *out, u32 max) {
    const struct de__actions *s = in->actions;
    const de__context_state *c = &s->context;
    bool seen[DE_ACTION_MAX_MAPS];
    memset(seen, 0, sizeof seen);
    u32 n = 0;
    for (u32 i = c->applied_count; i-- > 0 && n < max;) {
        const u32 m = c->applied[i].map;
        if (!seen[m]) {
            seen[m] = true;
            out[n++] = m;
        }
    }
    for (u32 m = 0; m < s->map_count && n < max; ++m)
        if (c->releasing[m] && !seen[m])
            out[n++] = m;
    return n;
}

/* Evaluation moved on to `map`: the inputs the previous map used now block the maps below. */
static void enter_map(de__context_state *c, u32 map) {
    inputs_or(&c->consumed, &c->pending);
    memset(&c->pending, 0, sizeof c->pending);
    c->eval_map = map;
}

bool de__context_binding_consumed(de_input *in, u32 map, u32 action, u32 binding,
                                  const de_binding *b) {
    struct de__actions *s = in->actions;
    de__context_state *c = &s->context;
    if (map != c->eval_map)
        enter_map(c, map);
    if (map >= DE_ACTION_MAX_MAPS || action >= DE_ACTION_MAX_ACTIONS || binding >= 16u)
        return false;
    if (!c->on_stack[map])
        return c->releasing[map]; /* releasing: every input reads released */
    const de__action *act = &s->actions[action];
    const u16 bit = (u16)(1u << binding);
    if ((c->latched[action] & bit) != 0) {
        if (input_down(in, act, b, true)) {
            if (c->blocks[map])
                mark_input(&c->pending, b);
            return true;
        }
        c->latched[action] = (u16)(c->latched[action] & ~bit);
    }
    if (bit_get(&c->consumed, (u32)b->source, b->code)) {
        if (input_down(in, act, b, false))
            c->latched[action] = (u16)(c->latched[action] | bit);
        return true;
    }
    return false;
}

void de__context_binding_used(de_input *in, u32 map, u32 action, u32 binding, const de_binding *b) {
    (void)action;
    (void)binding;
    de__context_state *c = ctx_of(in);
    if (map != c->eval_map)
        enter_map(c, map);
    if (map < DE_ACTION_MAX_MAPS && c->on_stack[map] && c->blocks[map])
        mark_input(&c->pending, b);
}

bool de__context_map_active(const de_input *in, u32 map) {
    const struct de__actions *s = de__actions_state_const(in);
    return s != NULL && map < DE_ACTION_MAX_MAPS && s->context.on_stack[map];
}

/* ---------- Queries (context_internal.h) ---------- */

static bool set_has(const de__context_set *set, u32 map) {
    for (u32 i = 0; i < set->map_count; ++i)
        if (set->maps[i] == map)
            return true;
    return false;
}

bool de__context_maps_share_set(const de_input *in, u32 a, u32 b) {
    const struct de__actions *s = de__actions_state_const(in);
    if (s == NULL || a >= s->map_count || b >= s->map_count)
        return false;
    if (a == b)
        return true;
    for (u32 i = 0; i < s->context.set_count; ++i)
        if (set_has(&s->context.sets[i], a) && set_has(&s->context.sets[i], b))
            return true;
    return false;
}

u32 de__context_find(const de_input *in, de_name id) {
    const struct de__actions *s = de__actions_state_const(in);
    if (s == NULL)
        return DE__CONTEXT_NONE;
    for (u32 i = 0; i < s->context.set_count; ++i)
        if (s->context.sets[i].id.hash == id.hash)
            return i;
    return DE__CONTEXT_NONE;
}

u32 de__context_requested_depth(const de_input *in) {
    const struct de__actions *s = de__actions_state_const(in);
    return s != NULL ? s->context.requested_count : 0u;
}

/* ---------- Public: maps ---------- */

static void stack_full(de_input *in, de__context_state *c, const char *what, u32 need) {
    c->overflows++;
    DE_ERROR(in->log, "%s: the context stack is full (%u of %d entries, %u more needed)", what,
             (unsigned)c->requested_count, DE_INPUT_CONTEXT_STACK_MAX, (unsigned)need);
}

static void remove_requested(de__context_state *c, u32 i) {
    for (u32 k = i + 1u; k < c->requested_count; ++k)
        c->requested[k - 1u] = c->requested[k];
    c->requested_count--;
}

void de_action_map_push(de_input *in, de_action_map m) {
    const u32 mi = de__action_map_index(in, m);
    if (mi == DE__ACTION_NONE)
        return;
    de__context_state *c = ctx_of(in);
    if (requested_has_map(c, mi))
        return;
    if (c->requested_count >= DE_INPUT_CONTEXT_STACK_MAX) {
        stack_full(in, c, "de_action_map_push", 1u);
        (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_action_map_push: '%s': the context stack is full",
                      in->actions->maps[mi].name);
        return;
    }
    c->requested[c->requested_count].map = mi;
    c->requested[c->requested_count].context = DE__CONTEXT_NONE;
    c->requested_count++;
}

void de_action_map_pop(de_input *in, de_action_map m) {
    const u32 mi = de__action_map_index(in, m);
    if (mi == DE__ACTION_NONE)
        return;
    de__context_state *c = ctx_of(in);
    for (u32 i = c->requested_count; i-- > 0;) {
        if (c->requested[i].map != mi)
            continue;
        if (i + 1u != c->requested_count)
            CTX_DEV_WARN(in,
                         "de_action_map_pop: '%s' is not on top of the context stack; removed "
                         "in place",
                         in->actions->maps[mi].name);
        remove_requested(c, i);
        return;
    }
}

void de_action_map_set_flags(de_input *in, de_action_map m, u32 flags) {
    const u32 mi = de__action_map_index(in, m);
    if (mi != DE__ACTION_NONE)
        ctx_of(in)->map_flags[mi] = flags & DE_INPUT_CONTEXT_PASSTHROUGH;
}

u32 de_action_map_flags(const de_input *in, de_action_map m) {
    const u32 mi = de__action_map_index(in, m);
    return mi != DE__ACTION_NONE ? in->actions->context.map_flags[mi] : 0u;
}

bool de_action_map_active(const de_input *in, de_action_map m) {
    const u32 mi = de__action_map_index(in, m);
    return mi != DE__ACTION_NONE && de__context_map_active(in, mi);
}

/* ---------- Public: named contexts ---------- */

de_result de_input_context_define(de_input *in, const char *name, const de_action_map *maps,
                                  u32 count, u32 flags) {
    if (in == NULL || name == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_input_context_define: NULL %s",
                       in == NULL ? "input state" : "name");
    const size_t len = strlen(name);
    if (len == 0 || len > DE_ACTION_NAME_MAX)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_input_context_define: the name must have 1 to %d bytes",
                       DE_ACTION_NAME_MAX);
    if (maps == NULL || count == 0 || count > DE_INPUT_CONTEXT_MAX_MAPS)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_input_context_define: '%s': %u maps (1 to %d)",
                       name, (unsigned)(maps == NULL ? 0u : count), DE_INPUT_CONTEXT_MAX_MAPS);
    u32 idx[DE_INPUT_CONTEXT_MAX_MAPS];
    for (u32 i = 0; i < count; ++i) {
        idx[i] = de__action_map_index(in, maps[i]);
        if (idx[i] == DE__ACTION_NONE)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "de_input_context_define: '%s': map %u is not a valid handle", name,
                           (unsigned)i);
        for (u32 k = 0; k < i; ++k)
            if (idx[k] == idx[i])
                return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                               "de_input_context_define: '%s': map '%s' is listed twice", name,
                               in->actions->maps[idx[i]].name);
    }
    de__context_state *c = ctx_of(in);
    const de_name id = de_name_from(name);
    u32 si = de__context_find(in, id);
    if (si == DE__CONTEXT_NONE) {
        if (c->set_count >= DE_INPUT_MAX_CONTEXTS)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY,
                           "de_input_context_define: '%s': %d named contexts already (max)", name,
                           DE_INPUT_MAX_CONTEXTS);
        si = c->set_count++;
    }
    de__context_set *set = &c->sets[si];
    memset(set, 0, sizeof *set);
    memcpy(set->name, name, len);
    set->id = id;
    set->flags = flags & DE_INPUT_CONTEXT_PASSTHROUGH;
    set->map_count = count;
    memcpy(set->maps, idx, sizeof idx[0] * count);
    return DE_OK;
}

/* DE_ERR_NOT_FOUND naming the context (its string where the build keeps names). */
static de_result unknown_context(de_name name, const char *what) {
    const char *str = de_name_str(name);
    if (str != NULL)
        return DE_FAIL(DE_ERR_NOT_FOUND, "%s: unknown context '%s'", what, str);
    return DE_FAIL(DE_ERR_NOT_FOUND, "%s: unknown context %016llx", what,
                   (unsigned long long)name.hash);
}

de_result de_input_context_push(de_input *in, de_name name) {
    const u32 si = de__context_find(in, name);
    if (si == DE__CONTEXT_NONE)
        return unknown_context(name, "de_input_context_push");
    de__context_state *c = ctx_of(in);
    for (u32 i = 0; i < c->requested_count; ++i)
        if (c->requested[i].context == si)
            return DE_OK; /* on the stack already */
    const de__context_set *set = &c->sets[si];
    if (c->requested_count + set->map_count > DE_INPUT_CONTEXT_STACK_MAX) {
        stack_full(in, c, "de_input_context_push", set->map_count);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY,
                       "de_input_context_push: '%s': %u maps do not fit on the context stack "
                       "(%u of %d entries used)",
                       set->name, (unsigned)set->map_count, (unsigned)c->requested_count,
                       DE_INPUT_CONTEXT_STACK_MAX);
    }
    for (u32 i = 0; i < set->map_count; ++i) {
        c->requested[c->requested_count].map = set->maps[i];
        c->requested[c->requested_count].context = si;
        c->requested_count++;
    }
    return DE_OK;
}

de_result de_input_context_pop(de_input *in, de_name name) {
    const u32 si = de__context_find(in, name);
    if (si == DE__CONTEXT_NONE)
        return unknown_context(name, "de_input_context_pop");
    de__context_state *c = ctx_of(in);
    u32 w = 0;
    bool found = false, below_top = false;
    for (u32 i = 0; i < c->requested_count; ++i) {
        if (c->requested[i].context == si) {
            found = true;
            continue;
        }
        if (found)
            below_top = true; /* an entry of another push is above the group */
        c->requested[w++] = c->requested[i];
    }
    if (below_top)
        CTX_DEV_WARN(in,
                     "de_input_context_pop: '%s' is not on top of the context stack; removed "
                     "in place",
                     c->sets[si].name);
    c->requested_count = w;
    return DE_OK;
}
