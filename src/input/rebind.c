/* rebind.c: runtime rebinding (DENG-86): listen mode, capture rules, suppression of the listening
 * map and of the input that ended listening, conflicts, de_rebind_apply policies, default
 * snapshots and resets. bindings_io.c saves and loads the user bindings on top of this file.
 *
 * State: one block (struct de__rebind) behind de__actions.rebind, allocated by the first call
 * that needs it (begin, apply, clear, load, cancel-input setup) and freed with the action block.
 * Everything is in fixed arrays; scans go over the frame's raw events in arrival order and over
 * maps, actions and bindings in declaration order, so the outcome never depends on memory or
 * hash order (SPEC §5.7). Nothing here allocates per frame.
 *
 * Defaults: the first time rebinding changes an action, its bindings are copied into a pool
 * (`pool`) as its defaults, with a hash of the bindings as rebinding left them. If the hash no
 * longer matches, something else rewrote the bindings (de_action_add of a reloaded map), and the
 * current bindings become the defaults again. */
#include "action_internal.h"
#include "context_internal.h"

#include <dengine/de_memory.h>

#include <string.h>

/* Modifier keys: DE_KEY_LCTRL .. DE_KEY_RGUI are 224 .. 231 (HID). */
#define RB_MOD_FIRST ((u32)DE_KEY_LCTRL)
#define RB_MOD_COUNT 8u

DE_STATIC_ASSERT((u32)DE_KEY_RGUI == RB_MOD_FIRST + RB_MOD_COUNT - 1u, "modifier keys 224..231");
DE_STATIC_ASSERT(DE_BIND_SOURCE_COUNT_ <= 32, "allowed sources fit a u32 mask");

/* Sources allowed when de_rebind_begin gets no list: not the mouse delta and the gyro, which
 * move all the time. */
#define RB_DEFAULT_ALLOWED                                                                         \
    ((1u << DE_BIND_KEY) | (1u << DE_BIND_MOUSE_BUTTON) | (1u << DE_BIND_MOUSE_WHEEL) |            \
     (1u << DE_BIND_PAD_BUTTON) | (1u << DE_BIND_PAD_AXIS))

/** The default bindings of one action (pool[first .. first + count)). */
typedef struct rb_default {
    u32 first, count, capacity;
    u64 hash; /**< bindings hash as rebinding last left them */
    bool valid;
} rb_default;

/** One input: what was captured, what is swallowed, what cancels. */
typedef struct rb_input {
    de_binding_source source;
    u32 code;
    u32 modifier;  /**< modifier key of a chord, 0 = none */
    bool negative; /**< 1D analog: pushed towards negative values */
} rb_input;

/** Most inputs swallowed at once: the input that ended listening and its chord modifier. */
#define RB_MAX_SWALLOW 2u

struct de__rebind {
    /* Listen mode. */
    de_rebind_state state;
    u32 action;        /**< action index of the slot */
    u32 map;           /**< its map index */
    u32 binding_index; /**< the slot */
    u32 allowed;       /**< bit per de_binding_source */
    u64 begin_frame;   /**< in->frame at de_rebind_begin: only later frames are scanned */
    u64 scanned_frame; /**< the last frame whose events were scanned */
    u64 end_frame;     /**< in->frame when a capture or cancel input ended listening; 0 = none */
    bool result_ready; /**< de_rebind_poll has not returned the capture yet */
    de_binding captured;
    de_action conflict;

    u32 mods_held;  /**< bit per modifier key, held as far as the scan knows */
    u32 mods_fresh; /**< pressed while listening, with no other input since */
    bool axis_armed[DE_INPUT_MAX_PADS][DE_PAD_AXIS_COUNT_]; /**< seen at rest since begin */
    bool gyro_armed[DE_INPUT_MAX_PADS];
    f32 wheel[2]; /**< notches since begin (x, y) */
    f32 delta[2]; /**< mouse points since begin (x, y; +y up) */

    /* The input that ended listening, held idle until released. */
    rb_input swallow[RB_MAX_SWALLOW];
    u32 swallow_count;

    /* Cancel inputs (de_rebind_set_cancel_inputs); the defaults while !cancel_custom. */
    bool cancel_custom;
    u32 cancel_count;
    rb_input cancel[DE_REBIND_MAX_CANCEL_INPUTS];

    /* Defaults. */
    rb_default defaults[DE_ACTION_MAX_ACTIONS];
    u32 pool_count;
    de_binding pool[DE_ACTION_MAX_BINDINGS_TOTAL];
};

static f32 absf(f32 v) {
    return v < 0.0f ? -v : v;
}

/* ---------- State ---------- */

struct de__rebind *de__rebind_get_or_create(de_input *in) {
    struct de__actions *s = de__actions_state(in);
    if (s == NULL) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "rebinding: the input state has no action maps");
        return NULL;
    }
    if (s->rebind != NULL)
        return s->rebind;
    struct de__rebind *rb = (struct de__rebind *)DE_ALLOC(
        &in->alloc, sizeof *rb, DE_ALIGNOF(struct de__rebind), in->mem_tag);
    if (rb == NULL) {
        DE_FAIL(DE_ERR_OUT_OF_MEMORY, "rebinding: %u bytes", (unsigned)sizeof *rb);
        return NULL;
    }
    memset(rb, 0, sizeof *rb);
    rb->state = DE_REBIND_IDLE;
    s->rebind = rb;
    return rb;
}

void de__rebind_shutdown(de_input *in) {
    struct de__actions *s = de__actions_state(in);
    if (s == NULL || s->rebind == NULL)
        return;
    DE_FREE(&in->alloc, s->rebind, sizeof *s->rebind);
    s->rebind = NULL;
}

static struct de__rebind *rb_state(const de_input *in) {
    const struct de__actions *s = de__actions_state_const(in);
    return s != NULL ? s->rebind : NULL;
}

/* ---------- Binding helpers ---------- */

static bool rb_is_2d_code(de_binding_source src, u32 code) {
    switch (src) {
    case DE_BIND_PAD_AXIS:
        return code == DE_BIND_LEFT_STICK || code == DE_BIND_RIGHT_STICK;
    case DE_BIND_MOUSE_DELTA:
    case DE_BIND_MOUSE_WHEEL:
    case DE_BIND_GYRO:
        return code == DE_BIND_AXIS_XY;
    default:
        return false;
    }
}

static bool rb_code_ok(const de_binding *b) {
    switch (b->source) {
    case DE_BIND_KEY:
        return b->code > 0 && b->code < (u32)DE_KEY_COUNT_;
    case DE_BIND_MOUSE_BUTTON:
        return b->code < (u32)DE_MOUSE_BUTTON_COUNT_;
    case DE_BIND_PAD_BUTTON:
        return b->code < (u32)DE_PAD_BUTTON_COUNT_;
    case DE_BIND_PAD_AXIS:
        return b->code < (u32)DE_PAD_AXIS_COUNT_ || b->code == DE_BIND_LEFT_STICK ||
               b->code == DE_BIND_RIGHT_STICK;
    case DE_BIND_MOUSE_DELTA:
    case DE_BIND_MOUSE_WHEEL:
        return b->code == DE_BIND_AXIS_X || b->code == DE_BIND_AXIS_Y || b->code == DE_BIND_AXIS_XY;
    case DE_BIND_GYRO:
        return b->code <= DE_BIND_AXIS_XY;
    default:
        return false;
    }
}

bool de__binding_fits(de_action_type type, const de_binding *b) {
    if (b == NULL || (u32)b->source >= (u32)DE_BIND_SOURCE_COUNT_ || !rb_code_ok(b))
        return false;
    if (b->composite_slot < DE_COMPOSITE_NONE || b->composite_slot > DE_COMPOSITE_RIGHT)
        return false;
    if (b->modifier_key >= (u32)DE_KEY_COUNT_)
        return false;
    const bool is2d = rb_is_2d_code(b->source, b->code);
    const bool composite = type != DE_ACTION_BUTTON && b->composite_slot >= 0;
    switch (type) {
    case DE_ACTION_BUTTON:
        return true;
    case DE_ACTION_AXIS1D:
        return !is2d && b->composite_slot <= DE_COMPOSITE_NEGATIVE;
    case DE_ACTION_AXIS2D:
        return composite != is2d;
    default:
        return false;
    }
}

bool de__binding_same(de_action_type type, const de_binding *a, const de_binding *b) {
    if (a->source != b->source || a->code != b->code || a->modifier_key != b->modifier_key ||
        a->invert != b->invert || a->scale != b->scale || a->deadzone != b->deadzone)
        return false;
    if (type != DE_ACTION_BUTTON && a->composite_slot != b->composite_slot)
        return false;
    if (a->curve.type != b->curve.type)
        return false;
    if (a->curve.type == DE_CURVE_POWER && a->curve.exponent != b->curve.exponent)
        return false;
    if (a->curve.type == DE_CURVE_CUSTOM) {
        if (a->curve.point_count != b->curve.point_count)
            return false;
        for (u32 i = 0; i < a->curve.point_count && i < DE_CURVE_MAX_POINTS; ++i)
            if (a->curve.points[i].x != b->curve.points[i].x ||
                a->curve.points[i].y != b->curve.points[i].y)
                return false;
    }
    return true;
}

bool de_binding_is_unbound(const de_binding *b) {
    return b != NULL && b->source == DE_BIND_KEY && b->code == DE_BIND_UNBOUND_KEY;
}

/* The direction of a 1D analog binding: -1 when it reads the negative side of its input. */
static int rb_direction(const de_binding *b) {
    const bool neg = b->invert != (b->scale < 0.0f);
    return neg ? -1 : 1;
}

/* The stick (1 left, 2 right) of a pad axis code, or 0 for a trigger. */
static u32 rb_stick_of(u32 code) {
    switch (code) {
    case DE_AXIS_LEFT_X:
    case DE_AXIS_LEFT_Y:
    case DE_BIND_LEFT_STICK:
        return 1;
    case DE_AXIS_RIGHT_X:
    case DE_AXIS_RIGHT_Y:
    case DE_BIND_RIGHT_STICK:
        return 2;
    default:
        return 0;
    }
}

/* Whether codes `a` and `b` of source `src` read the same physical input (ignoring direction):
 * equal codes, or a 2D code and one of its axes. */
static bool rb_codes_touch(de_binding_source src, u32 a, u32 b) {
    if (a == b)
        return true;
    switch (src) {
    case DE_BIND_PAD_AXIS:
        if (rb_is_2d_code(src, a) || rb_is_2d_code(src, b))
            return rb_stick_of(a) != 0 && rb_stick_of(a) == rb_stick_of(b);
        return false;
    case DE_BIND_MOUSE_DELTA:
    case DE_BIND_MOUSE_WHEEL:
    case DE_BIND_GYRO:
        /* XY covers X and Y (gyro XY aims with the pad's x and y rates), not Z. */
        if (a == DE_BIND_AXIS_XY)
            return b == DE_BIND_AXIS_X || b == DE_BIND_AXIS_Y;
        if (b == DE_BIND_AXIS_XY)
            return a == DE_BIND_AXIS_X || a == DE_BIND_AXIS_Y;
        return false;
    default:
        return false;
    }
}

static bool rb_is_trigger(de_binding_source src, u32 code) {
    return src == DE_BIND_PAD_AXIS &&
           (code == DE_AXIS_LEFT_TRIGGER || code == DE_AXIS_RIGHT_TRIGGER);
}

static bool rb_is_analog_1d(de_binding_source src, u32 code) {
    switch (src) {
    case DE_BIND_PAD_AXIS:
    case DE_BIND_MOUSE_DELTA:
    case DE_BIND_MOUSE_WHEEL:
    case DE_BIND_GYRO:
        return !rb_is_2d_code(src, code);
    default:
        return false;
    }
}

/* Conflict rule: two bindings read the same input, with the same modifier, and 1D axes in the
 * same direction (triggers have one). Unbound slots never conflict. */
static bool rb_overlap(const de_binding *a, const de_binding *b) {
    if (de_binding_is_unbound(a) || de_binding_is_unbound(b))
        return false;
    if (a->source != b->source || a->modifier_key != b->modifier_key)
        return false;
    if (!rb_codes_touch(a->source, a->code, b->code))
        return false;
    if (a->code == b->code && rb_is_analog_1d(a->source, a->code) &&
        !rb_is_trigger(a->source, a->code))
        return rb_direction(a) == rb_direction(b);
    return true;
}

/* Whether binding `b` reads input `x` at all (any modifier, any direction): used to swallow. */
static bool rb_touches(const de_binding *b, const rb_input *x) {
    if (de_binding_is_unbound(b) || b->source != x->source)
        return false;
    return rb_codes_touch(b->source, b->code, x->code);
}

/* The binding for input `c` in a slot that holds `slot` now, for an action of `type`: the slot's
 * composite slot, its tuning when the source stays the same, the captured direction as `invert`
 * for 1D analog codes, and the 2D code of an axis for a whole-2D slot. */
static de_binding rb_fit(const de_binding *slot, const rb_input *c, de_action_type type) {
    de_binding b;
    memset(&b, 0, sizeof b);
    b.source = c->source;
    b.code = c->code;
    b.modifier_key = c->modifier;
    b.composite_slot = type == DE_ACTION_BUTTON ? DE_COMPOSITE_NONE : slot->composite_slot;
    const bool whole_2d = type == DE_ACTION_AXIS2D && slot->composite_slot < 0;
    if (whole_2d) {
        if (c->source == DE_BIND_PAD_AXIS && rb_stick_of(c->code) == 1)
            b.code = DE_BIND_LEFT_STICK;
        else if (c->source == DE_BIND_PAD_AXIS && rb_stick_of(c->code) == 2)
            b.code = DE_BIND_RIGHT_STICK;
        else if (c->source == DE_BIND_MOUSE_DELTA || c->source == DE_BIND_MOUSE_WHEEL ||
                 (c->source == DE_BIND_GYRO && c->code != DE_BIND_AXIS_Z))
            b.code = DE_BIND_AXIS_XY;
    }
    const bool is2d = rb_is_2d_code(b.source, b.code);
    if (!is2d && rb_is_analog_1d(b.source, b.code))
        b.invert = c->negative;
    if (slot->source == b.source && !de_binding_is_unbound(slot)) {
        /* Same source: keep the slot's tuning. The scale's sign is a direction, which the
         * capture decides, so only its size is kept. */
        b.scale = absf(slot->scale);
        b.deadzone = slot->deadzone;
        b.curve = slot->curve;
        if (is2d)
            b.invert = slot->invert;
    }
    return b;
}

/* The input a binding reads (for SWAP: a binding's input moved to another slot). */
static rb_input rb_input_of(const de_binding *b) {
    rb_input x;
    memset(&x, 0, sizeof x);
    x.source = b->source;
    x.code = b->code;
    x.modifier = b->modifier_key;
    x.negative = rb_is_analog_1d(b->source, b->code) && rb_direction(b) < 0;
    return x;
}

static de_binding rb_unbound_in(const de_binding *slot, de_action_type type) {
    de_binding b;
    memset(&b, 0, sizeof b);
    b.source = DE_BIND_KEY;
    b.code = DE_BIND_UNBOUND_KEY;
    b.composite_slot = type == DE_ACTION_BUTTON ? DE_COMPOSITE_NONE : slot->composite_slot;
    return b;
}

/* ---------- Defaults ---------- */

static u64 rb_mix(u64 h, u32 v) {
    for (u32 i = 0; i < 4; ++i) {
        h ^= (u64)((v >> (8u * i)) & 0xFFu);
        h *= 1099511628211ull; /* FNV-1a */
    }
    return h;
}

static u32 rb_f32_bits(f32 v) {
    u32 u;
    memcpy(&u, &v, sizeof u);
    return u;
}

/* Hash of an action's definition as far as rebinding cares: type, rebindable and every field of
 * every binding. Field by field, so padding bytes do not count. */
static u64 rb_hash(const struct de__actions *s, const de__action *act) {
    u64 h = 14695981039346656037ull;
    h = rb_mix(h, (u32)act->type);
    h = rb_mix(h, act->rebindable ? 1u : 0u);
    h = rb_mix(h, act->binding_count);
    for (u32 i = 0; i < act->binding_count; ++i) {
        const de_binding *b = &s->bindings[act->first_binding + i];
        h = rb_mix(h, (u32)b->source);
        h = rb_mix(h, b->code);
        h = rb_mix(h, (u32)(i32)b->composite_slot);
        h = rb_mix(h, rb_f32_bits(b->scale));
        h = rb_mix(h, rb_f32_bits(b->deadzone));
        h = rb_mix(h, b->invert ? 1u : 0u);
        h = rb_mix(h, b->modifier_key);
        h = rb_mix(h, (u32)b->curve.type);
        h = rb_mix(h, rb_f32_bits(b->curve.exponent));
        h = rb_mix(h, b->curve.point_count);
        for (u32 k = 0; k < DE_CURVE_MAX_POINTS; ++k) {
            h = rb_mix(h, rb_f32_bits(b->curve.points[k].x));
            h = rb_mix(h, rb_f32_bits(b->curve.points[k].y));
        }
    }
    return h;
}

/* Moves every default range except action `skip`'s to the front of the pool, in pool order. */
static void rb_compact(struct de__rebind *rb, u32 action_count, u32 skip) {
    u32 cursor = 0;
    for (;;) {
        /* The live range with the lowest start at or after `cursor` (at most 256 ranges, and this
         * runs only when the pool is full). */
        u32 best = DE__ACTION_NONE;
        for (u32 i = 0; i < action_count; ++i) {
            const rb_default *d = &rb->defaults[i];
            if (!d->valid || i == skip || d->first < cursor)
                continue;
            if (best == DE__ACTION_NONE || d->first < rb->defaults[best].first)
                best = i;
        }
        if (best == DE__ACTION_NONE)
            break;
        rb_default *d = &rb->defaults[best];
        if (d->first != cursor && d->count > 0)
            memmove(&rb->pool[cursor], &rb->pool[d->first], d->count * sizeof rb->pool[0]);
        d->first = cursor;
        d->capacity = d->count;
        cursor += d->count;
    }
    if (skip < action_count) {
        rb->defaults[skip].valid = false;
        rb->defaults[skip].capacity = 0;
    }
    rb->pool_count = cursor;
}

/* Makes sure action `ai` has a valid default snapshot (taken now when it has none, or when its
 * bindings were changed by something else than rebinding). */
static de_result rb_sync(de_input *in, struct de__rebind *rb, u32 ai) {
    struct de__actions *s = in->actions;
    const de__action *act = &s->actions[ai];
    rb_default *d = &rb->defaults[ai];
    const u64 h = rb_hash(s, act);
    if (d->valid && d->hash == h)
        return DE_OK;
    const u32 n = act->binding_count;
    if (!d->valid || d->capacity < n) {
        if (rb->pool_count + n > DE_ACTION_MAX_BINDINGS_TOTAL)
            rb_compact(rb, s->action_count, ai);
        if (rb->pool_count + n > DE_ACTION_MAX_BINDINGS_TOTAL)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "rebinding: no room for the defaults of '%s'",
                           act->name);
        d->first = rb->pool_count;
        d->capacity = n;
        rb->pool_count += n;
    }
    if (n > 0)
        memcpy(&rb->pool[d->first], &s->bindings[act->first_binding], n * sizeof rb->pool[0]);
    d->count = n;
    d->hash = h;
    d->valid = true;
    return DE_OK;
}

u32 de__rebind_defaults(const de_input *in, u32 ai, de_binding *out, u32 max) {
    const struct de__actions *s = de__actions_state_const(in);
    if (s == NULL || ai >= s->action_count)
        return 0;
    const de__action *act = &s->actions[ai];
    const struct de__rebind *rb = s->rebind;
    const de_binding *src = &s->bindings[act->first_binding];
    u32 n = act->binding_count;
    if (rb != NULL && rb->defaults[ai].valid && rb->defaults[ai].hash == rb_hash(s, act)) {
        src = &rb->pool[rb->defaults[ai].first];
        n = rb->defaults[ai].count;
    }
    for (u32 i = 0; i < n && i < max; ++i)
        out[i] = src[i];
    return n;
}

de_result de__rebind_set(de_input *in, u32 ai, u32 index, const de_binding *b) {
    struct de__rebind *rb = de__rebind_get_or_create(in);
    if (rb == NULL)
        return de_error_last() != NULL ? de_error_last()->result : DE_ERR_OUT_OF_MEMORY;
    struct de__actions *s = in->actions;
    if (ai >= s->action_count)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "rebinding: bad action index %u", (unsigned)ai);
    DE_TRY(rb_sync(in, rb, ai));
    DE_TRY(de__action_set_binding(in, de__action_handle(in, ai), index, b));
    rb->defaults[ai].hash = rb_hash(s, &s->actions[ai]);
    return DE_OK;
}

/* Puts action `ai` back to its defaults (nothing when rebinding never changed it). */
static de_result rb_reset_action(de_input *in, struct de__rebind *rb, u32 ai) {
    struct de__actions *s = in->actions;
    rb_default *d = &rb->defaults[ai];
    if (!d->valid)
        return DE_OK;
    const de__action *act = &s->actions[ai];
    if (d->hash != rb_hash(s, act)) {
        d->valid = false; /* added again: its bindings are the defaults already */
        return DE_OK;
    }
    /* Copy out of the pool first: set_bindings validates before it writes. */
    de_binding tmp[DE_ACTION_MAX_BINDINGS];
    const u32 n = d->count <= DE_ACTION_MAX_BINDINGS ? d->count : DE_ACTION_MAX_BINDINGS;
    memcpy(tmp, &rb->pool[d->first], n * sizeof tmp[0]);
    DE_TRY(de__action_set_bindings(in, de__action_handle(in, ai), tmp, n));
    d->hash = rb_hash(s, &s->actions[ai]);
    return DE_OK;
}

/* ---------- Conflicts ---------- */

/* The maps whose actions can conflict with a binding of map `map`: the map itself first, then
 * every other map that shares a named context set with it (DENG-85, context_internal.h), in map
 * order. */
static u32 rb_conflict_maps(const de_input *in, u32 map, u32 *out, u32 max) {
    const struct de__actions *s = in->actions;
    u32 n = 0;
    if (n < max)
        out[n++] = map;
    for (u32 m = 0; m < s->map_count && n < max; ++m)
        if (m != map && de__context_maps_share_set(in, map, m))
            out[n++] = m;
    return n;
}

/* Calls `fn` for every binding of another rebindable action, in the maps that can conflict with
 * action `ai`, that conflicts with `b`, in order (maps, actions in declaration order, binding
 * index). Stops when `fn` returns false. */
typedef bool (*rb_conflict_fn)(de_input *in, u32 ai, u32 index, void *user);

static void rb_each_conflict(de_input *in, u32 target, const de_binding *b, rb_conflict_fn fn,
                             void *user) {
    const struct de__actions *s = in->actions;
    u32 maps[DE_ACTION_MAX_MAPS];
    const u32 nm = rb_conflict_maps(in, s->actions[target].map, maps, DE_ACTION_MAX_MAPS);
    for (u32 k = 0; k < nm; ++k) {
        if (maps[k] >= s->map_count)
            continue;
        for (u32 i = s->maps[maps[k]].first_action; i != DE__ACTION_NONE;
             i = s->actions[i].next_in_map) {
            const de__action *act = &s->actions[i];
            if (i == target || !act->rebindable)
                continue;
            for (u32 j = 0; j < act->binding_count; ++j)
                if (rb_overlap(&s->bindings[act->first_binding + j], b) && !fn(in, i, j, user))
                    return;
        }
    }
}

static bool rb_first_conflict_fn(de_input *in, u32 ai, u32 index, void *user) {
    (void)index;
    *(de_action *)user = de__action_handle(in, ai);
    return false;
}

static de_action rb_first_conflict(de_input *in, u32 target, const de_binding *b) {
    de_action c = {0};
    rb_each_conflict(in, target, b, rb_first_conflict_fn, &c);
    return c;
}

/* ---------- Listen mode ---------- */

static bool rb_allowed(const struct de__rebind *rb, de_binding_source src) {
    return (rb->allowed & (1u << (u32)src)) != 0;
}

static bool rb_is_modifier(u32 code) {
    return code >= RB_MOD_FIRST && code < RB_MOD_FIRST + RB_MOD_COUNT;
}

/* The chord modifier: the held modifier key with the lowest code, or 0. */
static u32 rb_modifier(const struct de__rebind *rb) {
    for (u32 i = 0; i < RB_MOD_COUNT; ++i)
        if ((rb->mods_held & (1u << i)) != 0)
            return RB_MOD_FIRST + i;
    return 0;
}

static bool rb_is_cancel(const struct de__rebind *rb, de_binding_source src, u32 code) {
    if (!rb->cancel_custom)
        return (src == DE_BIND_KEY && code == (u32)DE_KEY_ESCAPE) ||
               (src == DE_BIND_PAD_BUTTON &&
                (code == (u32)DE_PAD_START || code == (u32)DE_PAD_GUIDE));
    for (u32 i = 0; i < rb->cancel_count; ++i)
        if (rb->cancel[i].source == src && rb->cancel[i].code == code)
            return true;
    return false;
}

static u32 rb_pad_slot(const de_input *in, u32 device) {
    if (device == 0)
        return DE_INPUT_MAX_PADS;
    for (u32 i = 0; i < DE_INPUT_MAX_PADS; ++i)
        if (in->pads[i].device == device)
            return i;
    return DE_INPUT_MAX_PADS;
}

/* Ends listening: the input (and its chord modifier) is swallowed until released. */
static void rb_end(de_input *in, struct de__rebind *rb, de_rebind_state state,
                   const rb_input *input) {
    rb->state = state;
    rb->end_frame = in->frame;
    rb->result_ready = state == DE_REBIND_CAPTURED;
    rb->swallow_count = 0;
    rb->swallow[rb->swallow_count++] = *input;
    if (input->modifier != 0) {
        rb_input m;
        memset(&m, 0, sizeof m);
        m.source = DE_BIND_KEY;
        m.code = input->modifier;
        rb->swallow[rb->swallow_count++] = m;
    }
}

/* Tries to capture input `c` for the slot. False when it does not fit the slot (listening goes
 * on). */
static bool rb_try(de_input *in, struct de__rebind *rb, const rb_input *c) {
    const struct de__actions *s = in->actions;
    const de__action *act = &s->actions[rb->action];
    if (rb->binding_index >= act->binding_count) {
        /* The action was added again with fewer bindings: nothing left to bind. */
        rb->state = DE_REBIND_CANCELLED;
        rb->end_frame = in->frame;
        return true;
    }
    const de_binding slot = s->bindings[act->first_binding + rb->binding_index];
    const de_binding b = rb_fit(&slot, c, act->type);
    if (!de__binding_fits(act->type, &b))
        return false;
    rb->captured = b;
    rb->conflict = rb_first_conflict(in, rb->action, &b);
    rb_end(in, rb, DE_REBIND_CAPTURED, c);
    return true;
}

static bool rb_try_input(de_input *in, struct de__rebind *rb, de_binding_source src, u32 code,
                         u32 modifier, bool negative) {
    rb_input c;
    memset(&c, 0, sizeof c);
    c.source = src;
    c.code = code;
    c.modifier = modifier;
    c.negative = negative;
    return rb_try(in, rb, &c);
}

static bool rb_cancel_by(de_input *in, struct de__rebind *rb, de_binding_source src, u32 code) {
    if (!rb_is_cancel(rb, src, code))
        return false;
    rb_input c;
    memset(&c, 0, sizeof c);
    c.source = src;
    c.code = code;
    rb_end(in, rb, DE_REBIND_CANCELLED, &c);
    return true;
}

/* An accumulated 2D amount (wheel notches, mouse points): the larger axis past `threshold`. */
static bool rb_try_accum(de_input *in, struct de__rebind *rb, de_binding_source src, f32 *acc,
                         f32 threshold) {
    const u32 axis = absf(acc[1]) >= absf(acc[0]) ? 1u : 0u;
    if (!(absf(acc[axis]) > threshold))
        return false;
    const bool neg = acc[axis] < 0.0f;
    acc[0] = acc[1] = 0.0f; /* a refused amount starts over */
    rb->mods_fresh = 0;
    return rb_try_input(in, rb, src, axis == 0 ? DE_BIND_AXIS_X : DE_BIND_AXIS_Y, rb_modifier(rb),
                        neg);
}

/* One raw event. True when listening ended. */
static bool rb_event(de_input *in, struct de__rebind *rb, const de__input_event *ev) {
    switch (ev->kind) {
    case DE_RAW_KEY: {
        const de_raw_key *k = &ev->key;
        if (k->repeat || k->code == 0 || k->code >= (u32)DE_KEY_COUNT_)
            return false;
        const u32 code = k->code;
        const u32 bit = rb_is_modifier(code) ? 1u << (code - RB_MOD_FIRST) : 0u;
        if (k->down) {
            if (rb_cancel_by(in, rb, DE_BIND_KEY, code))
                return true;
            if (bit != 0) {
                rb->mods_held |= bit;
                rb->mods_fresh |= bit;
                return false;
            }
            rb->mods_fresh = 0;
            return rb_allowed(rb, DE_BIND_KEY) &&
                   rb_try_input(in, rb, DE_BIND_KEY, code, rb_modifier(rb), false);
        }
        if (bit == 0)
            return false;
        /* A modifier released with nothing pressed since it went down: the modifier itself. */
        const bool fresh = (rb->mods_fresh & bit) != 0;
        rb->mods_held &= ~bit;
        rb->mods_fresh &= ~bit;
        return fresh && rb_allowed(rb, DE_BIND_KEY) &&
               rb_try_input(in, rb, DE_BIND_KEY, code, rb_modifier(rb), false);
    }
    case DE_RAW_MOUSE_BUTTON: {
        const de_raw_mouse_button *b = &ev->mouse_button;
        if (!b->down || b->button >= (u32)DE_MOUSE_BUTTON_COUNT_)
            return false;
        if (rb_cancel_by(in, rb, DE_BIND_MOUSE_BUTTON, b->button))
            return true;
        rb->mods_fresh = 0;
        return rb_allowed(rb, DE_BIND_MOUSE_BUTTON) &&
               rb_try_input(in, rb, DE_BIND_MOUSE_BUTTON, b->button, rb_modifier(rb), false);
    }
    case DE_RAW_MOUSE_WHEEL:
        if (!rb_allowed(rb, DE_BIND_MOUSE_WHEEL))
            return false;
        rb->wheel[0] += ev->mouse_wheel.dx;
        rb->wheel[1] += ev->mouse_wheel.dy;
        return rb_try_accum(in, rb, DE_BIND_MOUSE_WHEEL, rb->wheel, DE_REBIND_AXIS_THRESHOLD);
    case DE_RAW_MOUSE_MOVE:
        if (!rb_allowed(rb, DE_BIND_MOUSE_DELTA))
            return false;
        rb->delta[0] += ev->mouse_move.dx;
        rb->delta[1] -= ev->mouse_move.dy; /* action space is +y up */
        return rb_try_accum(in, rb, DE_BIND_MOUSE_DELTA, rb->delta,
                            DE_REBIND_MOUSE_DELTA_THRESHOLD);
    case DE_RAW_PAD_ADDED: {
        const u32 slot = rb_pad_slot(in, ev->device);
        if (slot < DE_INPUT_MAX_PADS) {
            for (u32 a = 0; a < (u32)DE_PAD_AXIS_COUNT_; ++a)
                rb->axis_armed[slot][a] = true;
            rb->gyro_armed[slot] = true;
        }
        return false;
    }
    case DE_RAW_PAD_BUTTON: {
        const de_raw_pad_button *b = &ev->pad_button;
        if (!b->down || b->button >= (u32)DE_PAD_BUTTON_COUNT_)
            return false;
        if (rb_cancel_by(in, rb, DE_BIND_PAD_BUTTON, b->button))
            return true;
        return rb_allowed(rb, DE_BIND_PAD_BUTTON) &&
               rb_try_input(in, rb, DE_BIND_PAD_BUTTON, b->button, 0, false);
    }
    case DE_RAW_PAD_AXIS: {
        const de_raw_pad_axis *a = &ev->pad_axis;
        const u32 slot = rb_pad_slot(in, ev->device);
        if (a->axis >= (u32)DE_PAD_AXIS_COUNT_ || slot >= DE_INPUT_MAX_PADS)
            return false;
        f32 v = a->value == a->value ? a->value : 0.0f; /* NaN reads as rest */
        if (a->axis == DE_AXIS_LEFT_Y || a->axis == DE_AXIS_RIGHT_Y)
            v = -v; /* the platform's +y down; action space is +y up */
        bool *armed = &rb->axis_armed[slot][a->axis];
        if (!(absf(v) > DE_REBIND_AXIS_THRESHOLD)) {
            *armed = true;
            return false;
        }
        if (!*armed || !rb_allowed(rb, DE_BIND_PAD_AXIS))
            return false;
        *armed = false; /* one capture attempt per push */
        return rb_try_input(in, rb, DE_BIND_PAD_AXIS, a->axis, 0, v < 0.0f);
    }
    default:
        return false;
    }
}

/* The gyro, per pad in connection order, from its processed rate of this frame. */
static bool rb_gyro(de_input *in, struct de__rebind *rb) {
    for (u32 k = 0; k < in->pad_count; ++k) {
        const u32 slot = in->pad_order[k];
        const de__input_pad *p = &in->pads[slot];
        if (p->device == 0 || !p->has_gyro)
            continue;
        const f32 g[3] = {p->gyro.x, p->gyro.y, p->gyro.z};
        u32 best = 0;
        for (u32 i = 1; i < 3; ++i)
            if (absf(g[i]) > absf(g[best]))
                best = i;
        if (!(absf(g[best]) > DE_REBIND_GYRO_THRESHOLD)) {
            rb->gyro_armed[slot] = true;
            continue;
        }
        if (!rb->gyro_armed[slot])
            continue;
        rb->gyro_armed[slot] = false;
        if (rb_try_input(in, rb, DE_BIND_GYRO, best, 0, g[best] < 0.0f))
            return true;
    }
    return false;
}

/* Scans the current frame once, if listening started in an earlier frame. */
static void rb_scan(de_input *in, struct de__rebind *rb) {
    if (rb->state != DE_REBIND_LISTENING || in->frame <= rb->begin_frame ||
        rb->scanned_frame >= in->frame)
        return;
    rb->scanned_frame = in->frame;
    for (u32 i = 0; i < in->frame_event_count; ++i)
        if (rb_event(in, rb, &in->frame_events[i]))
            return;
    if (rb_allowed(rb, DE_BIND_GYRO))
        (void)rb_gyro(in, rb);
}

/* Whether a swallowed input is still active: held, or pressed this frame. The wheel and the
 * mouse delta have no release; they count only in the frame that ended listening. */
static bool rb_input_active(const de_input *in, const struct de__rebind *rb, const rb_input *x) {
    switch (x->source) {
    case DE_BIND_KEY:
        return de_key_down(in, (de_key)x->code) || de_key_pressed(in, (de_key)x->code);
    case DE_BIND_MOUSE_BUTTON: {
        const u32 bit = 1u << x->code;
        return ((in->mouse_down | in->mouse_pressed) & bit) != 0;
    }
    case DE_BIND_PAD_BUTTON:
        for (u32 k = 0; k < in->pad_count; ++k) {
            const de__input_pad *p = &in->pads[in->pad_order[k]];
            if (((p->buttons | p->pressed) & (1u << x->code)) != 0)
                return true;
        }
        return false;
    case DE_BIND_PAD_AXIS:
        for (u32 k = 0; k < in->pad_count; ++k) {
            const de__input_pad *p = &in->pads[in->pad_order[k]];
            if (x->code < (u32)DE_PAD_AXIS_COUNT_ &&
                absf(p->axes[x->code]) > DE_REBIND_AXIS_THRESHOLD)
                return true;
        }
        return false;
    case DE_BIND_GYRO:
        for (u32 k = 0; k < in->pad_count; ++k) {
            const de__input_pad *p = &in->pads[in->pad_order[k]];
            if (absf(p->gyro.x) > DE_REBIND_GYRO_THRESHOLD ||
                absf(p->gyro.y) > DE_REBIND_GYRO_THRESHOLD ||
                absf(p->gyro.z) > DE_REBIND_GYRO_THRESHOLD)
                return true;
        }
        return false;
    default:
        return rb->end_frame == in->frame;
    }
}

static void rb_reset_map(struct de__actions *s, u32 map) {
    if (map >= s->map_count)
        return;
    for (u32 i = s->maps[map].first_action; i != DE__ACTION_NONE; i = s->actions[i].next_in_map)
        de__action_reset(&s->actions[i]);
}

void de__rebind_after_evaluate(de_input *in) {
    struct de__actions *s = de__actions_state(in);
    struct de__rebind *rb = s != NULL ? s->rebind : NULL;
    if (rb == NULL)
        return;
    rb_scan(in, rb);

    /* The listening map reads as idle while listening, and in the frame listening ended. */
    const bool ended_now = rb->end_frame != 0 && rb->end_frame == in->frame &&
                           rb->state != DE_REBIND_IDLE && rb->state != DE_REBIND_LISTENING;
    if (rb->state == DE_REBIND_LISTENING || ended_now)
        rb_reset_map(s, rb->map);

    /* Swallowed inputs: every action bound to one reads as idle until all are released. */
    if (rb->swallow_count == 0)
        return;
    bool active = false;
    for (u32 k = 0; k < rb->swallow_count; ++k)
        active = active || rb_input_active(in, rb, &rb->swallow[k]);
    if (!active) {
        rb->swallow_count = 0;
        return;
    }
    for (u32 i = 0; i < s->action_count; ++i) {
        de__action *act = &s->actions[i];
        bool hit = false;
        for (u32 j = 0; j < act->binding_count && !hit; ++j)
            for (u32 k = 0; k < rb->swallow_count && !hit; ++k)
                hit = rb_touches(&s->bindings[act->first_binding + j], &rb->swallow[k]);
        if (hit)
            de__action_reset(act);
    }
}

/* ---------- Public: listen mode ---------- */

static f32 rb_axis_abs(const de__input_pad *p, u32 a) {
    return absf(p->axes[a]);
}

de_result de_rebind_begin(de_input *in, de_action a, u32 binding_index,
                          const de_binding_source *allowed, u32 allowed_count) {
    if (in == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_rebind_begin: NULL input state");
    const u32 ai = de__action_index(in, a);
    if (ai == DE__ACTION_NONE)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_rebind_begin: invalid action handle");
    const de__action *act = &in->actions->actions[ai];
    if (!act->rebindable)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_rebind_begin: action '%s' is not rebindable",
                       act->name);
    if (binding_index >= act->binding_count)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_rebind_begin: action '%s' has %u bindings, not index %u", act->name,
                       (unsigned)act->binding_count, (unsigned)binding_index);
    if (allowed_count > 0 && allowed == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_rebind_begin: allowed is NULL");
    u32 mask = allowed_count == 0 ? RB_DEFAULT_ALLOWED : 0u;
    for (u32 i = 0; i < allowed_count; ++i) {
        if ((u32)allowed[i] >= (u32)DE_BIND_SOURCE_COUNT_)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_rebind_begin: unknown source %u",
                           (unsigned)allowed[i]);
        mask |= 1u << (u32)allowed[i];
    }
    struct de__rebind *rb = de__rebind_get_or_create(in);
    if (rb == NULL)
        return de_error_last() != NULL ? de_error_last()->result : DE_ERR_OUT_OF_MEMORY;

    rb->state = DE_REBIND_LISTENING;
    rb->action = ai;
    rb->map = act->map;
    rb->binding_index = binding_index;
    rb->allowed = mask;
    rb->begin_frame = in->frame;
    rb->scanned_frame = in->frame;
    rb->end_frame = 0;
    rb->result_ready = false;
    memset(&rb->captured, 0, sizeof rb->captured);
    rb->conflict = (de_action){0};
    rb->mods_held = 0;
    rb->mods_fresh = 0;
    for (u32 i = 0; i < RB_MOD_COUNT; ++i)
        if (de_key_down(in, (de_key)(RB_MOD_FIRST + i)))
            rb->mods_held |= 1u << i;
    /* Axes that are pushed now must come back to rest before they can be captured. */
    for (u32 slot = 0; slot < DE_INPUT_MAX_PADS; ++slot) {
        const de__input_pad *p = &in->pads[slot];
        bool gyro_rest = true;
        for (u32 x = 0; x < (u32)DE_PAD_AXIS_COUNT_; ++x)
            rb->axis_armed[slot][x] =
                p->device == 0 || !(rb_axis_abs(p, x) > DE_REBIND_AXIS_THRESHOLD);
        if (p->device != 0 && p->has_gyro)
            gyro_rest = !(absf(p->gyro.x) > DE_REBIND_GYRO_THRESHOLD) &&
                        !(absf(p->gyro.y) > DE_REBIND_GYRO_THRESHOLD) &&
                        !(absf(p->gyro.z) > DE_REBIND_GYRO_THRESHOLD);
        rb->gyro_armed[slot] = gyro_rest;
    }
    rb->wheel[0] = rb->wheel[1] = 0.0f;
    rb->delta[0] = rb->delta[1] = 0.0f;
    /* The listening map is idle from now on (like a pop: no release trigger). */
    rb_reset_map(in->actions, rb->map);
    return DE_OK;
}

bool de_rebind_poll(de_input *in, de_binding *captured, de_action *conflict) {
    if (captured != NULL)
        memset(captured, 0, sizeof *captured);
    if (conflict != NULL)
        *conflict = (de_action){0};
    struct de__rebind *rb = rb_state(in);
    if (rb == NULL)
        return false;
    rb_scan(in, rb); /* when the poll runs before input.actions in this frame */
    if (rb->state != DE_REBIND_CAPTURED || !rb->result_ready)
        return false;
    rb->result_ready = false;
    if (captured != NULL)
        *captured = rb->captured;
    if (conflict != NULL)
        *conflict = rb->conflict;
    return true;
}

void de_rebind_cancel(de_input *in) {
    struct de__rebind *rb = rb_state(in);
    if (rb == NULL)
        return;
    rb->state = DE_REBIND_IDLE;
    rb->result_ready = false;
    rb->end_frame = 0;
}

de_rebind_state de_rebind_status(const de_input *in) {
    const struct de__rebind *rb = rb_state(in);
    return rb != NULL ? rb->state : DE_REBIND_IDLE;
}

de_result de_rebind_set_cancel_inputs(de_input *in, const de_binding *inputs, u32 count) {
    if (in == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_rebind_set_cancel_inputs: NULL input state");
    if (inputs != NULL && count > DE_REBIND_MAX_CANCEL_INPUTS)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_rebind_set_cancel_inputs: %u inputs (max %d)",
                       (unsigned)count, DE_REBIND_MAX_CANCEL_INPUTS);
    for (u32 i = 0; inputs != NULL && i < count; ++i) {
        const de_binding *b = &inputs[i];
        const bool ok = (b->source == DE_BIND_KEY || b->source == DE_BIND_MOUSE_BUTTON ||
                         b->source == DE_BIND_PAD_BUTTON) &&
                        rb_code_ok(b);
        if (!ok)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "de_rebind_set_cancel_inputs: input %u is not a key, mouse button or "
                           "pad button",
                           (unsigned)i);
    }
    struct de__rebind *rb = de__rebind_get_or_create(in);
    if (rb == NULL)
        return de_error_last() != NULL ? de_error_last()->result : DE_ERR_OUT_OF_MEMORY;
    rb->cancel_custom = inputs != NULL;
    rb->cancel_count = 0;
    for (u32 i = 0; inputs != NULL && i < count; ++i) {
        rb_input *c = &rb->cancel[rb->cancel_count++];
        memset(c, 0, sizeof *c);
        c->source = inputs[i].source;
        c->code = inputs[i].code;
    }
    return DE_OK;
}

/* ---------- Public: apply, clear, reset ---------- */

/* Common checks of apply / clear: a live, rebindable action and an index in range. */
static de_result rb_check_slot(de_input *in, de_action a, u32 index, const char *fn, u32 *ai) {
    if (in == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s: NULL input state", fn);
    *ai = de__action_index(in, a);
    if (*ai == DE__ACTION_NONE)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s: invalid action handle", fn);
    const de__action *act = &in->actions->actions[*ai];
    if (!act->rebindable)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s: action '%s' is not rebindable", fn, act->name);
    if (index >= act->binding_count)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s: action '%s' has %u bindings, not index %u", fn,
                       act->name, (unsigned)act->binding_count, (unsigned)index);
    return DE_OK;
}

typedef struct rb_apply_ctx {
    de_rebind_policy policy;
    de_binding previous; /**< the slot's binding before the change */
    bool check;          /**< pass 1: only check that SWAP fits */
    de_result result;
} rb_apply_ctx;

static de_binding rb_conflict_replacement(const de__action *act, const de_binding *cur,
                                          const rb_apply_ctx *c) {
    if (c->policy == DE_REBIND_CLEAR_OTHER || de_binding_is_unbound(&c->previous))
        return rb_unbound_in(cur, act->type);
    const rb_input x = rb_input_of(&c->previous);
    return rb_fit(cur, &x, act->type);
}

static bool rb_apply_fn(de_input *in, u32 ai, u32 index, void *user) {
    rb_apply_ctx *c = (rb_apply_ctx *)user;
    const struct de__actions *s = in->actions;
    const de__action *act = &s->actions[ai];
    const de_binding cur = s->bindings[act->first_binding + index];
    const de_binding nb = rb_conflict_replacement(act, &cur, c);
    if (c->check) {
        if (!de__binding_fits(act->type, &nb)) {
            c->result = DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                                "de_rebind_apply: binding %u of action '%s' cannot take %s",
                                (unsigned)index, act->name,
                                de_binding_is_unbound(&nb) ? "an unbound slot (a whole stick or "
                                                             "xy slot cannot be unbound)"
                                                           : "the previous input (SWAP)");
            return false;
        }
        return true;
    }
    c->result = de__rebind_set(in, ai, index, &nb);
    return c->result == DE_OK;
}

de_result de_rebind_apply(de_input *in, de_action a, u32 binding_index, const de_binding *b,
                          de_rebind_policy policy) {
    u32 ai = 0;
    DE_TRY(rb_check_slot(in, a, binding_index, "de_rebind_apply", &ai));
    if (b == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_rebind_apply: NULL binding");
    if ((u32)policy >= (u32)DE_REBIND_POLICY_COUNT_)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_rebind_apply: unknown policy %d", (int)policy);
    const de__action *act = &in->actions->actions[ai];
    DE_TRY(de__binding_validate(act->type, b, act->name));
    if (de__rebind_get_or_create(in) == NULL)
        return de_error_last() != NULL ? de_error_last()->result : DE_ERR_OUT_OF_MEMORY;

    rb_apply_ctx c;
    memset(&c, 0, sizeof c);
    c.policy = policy;
    c.previous = in->actions->bindings[act->first_binding + binding_index];
    c.result = DE_OK;
    if (policy != DE_REBIND_ALLOW) {
        c.check = true;
        rb_each_conflict(in, ai, b, rb_apply_fn, &c);
        DE_TRY(c.result);
    }
    DE_TRY(de__rebind_set(in, ai, binding_index, b));
    if (policy != DE_REBIND_ALLOW) {
        c.check = false;
        rb_each_conflict(in, ai, b, rb_apply_fn, &c);
    }
    return c.result;
}

de_result de_rebind_clear(de_input *in, de_action a, u32 binding_index) {
    u32 ai = 0;
    DE_TRY(rb_check_slot(in, a, binding_index, "de_rebind_clear", &ai));
    const de__action *act = &in->actions->actions[ai];
    const de_binding cur = in->actions->bindings[act->first_binding + binding_index];
    const de_binding nb = rb_unbound_in(&cur, act->type);
    if (!de__binding_fits(act->type, &nb))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_rebind_clear: binding %u of action '%s' is a whole stick or xy slot, "
                       "which cannot be unbound",
                       (unsigned)binding_index, act->name);
    return de__rebind_set(in, ai, binding_index, &nb);
}

de_result de_bindings_reset_action(de_input *in, de_action a) {
    const u32 ai = de__action_index(in, a);
    if (ai == DE__ACTION_NONE)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_bindings_reset_action: invalid action handle");
    struct de__rebind *rb = rb_state(in);
    return rb != NULL ? rb_reset_action(in, rb, ai) : DE_OK;
}

de_result de_bindings_reset_map(de_input *in, de_action_map m) {
    const u32 mi = de__action_map_index(in, m);
    if (mi == DE__ACTION_NONE)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_bindings_reset_map: invalid map handle");
    struct de__rebind *rb = rb_state(in);
    if (rb == NULL)
        return DE_OK;
    const struct de__actions *s = in->actions;
    de_result r = DE_OK;
    for (u32 i = s->maps[mi].first_action; i != DE__ACTION_NONE; i = s->actions[i].next_in_map) {
        const de_result ri = rb_reset_action(in, rb, i);
        if (r == DE_OK)
            r = ri;
    }
    return r;
}

void de_bindings_reset_all(de_input *in) {
    struct de__rebind *rb = rb_state(in);
    if (rb == NULL)
        return;
    for (u32 i = 0; i < in->actions->action_count; ++i)
        (void)rb_reset_action(in, rb, i);
}
