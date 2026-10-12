/* action_map.c: action map storage, handles, validation and iteration (DENG-84).
 *
 * All maps, actions and bindings of one de_input live in one block (struct de__actions), which
 * the first de_action_map_create allocates. Nothing is ever freed before de_input_destroy, so
 * slots are never reused and every handle keeps generation 1; the generation bits are there for
 * a later remove. Arrays are in creation order, which is the evaluation order. */
#include "action_internal.h"

#include <dengine/de_memory.h>

#include <stdio.h>
#include <string.h>

/* ---------- State ---------- */

struct de__actions *de__actions_get_or_create(de_input *in) {
    if (in == NULL)
        return NULL;
    if (in->actions != NULL)
        return in->actions;
    struct de__actions *s = (struct de__actions *)DE_ALLOC(
        &in->alloc, sizeof *s, DE_ALIGNOF(struct de__actions), in->mem_tag);
    if (s == NULL) {
        DE_FAIL(DE_ERR_OUT_OF_MEMORY, "action maps: %u bytes", (unsigned)sizeof *s);
        return NULL;
    }
    memset(s, 0, sizeof *s);
    in->actions = s;
    de__context_init(in);
    return s;
}

struct de__actions *de__actions_state(de_input *in) {
    return in != NULL ? in->actions : NULL;
}

const struct de__actions *de__actions_state_const(const de_input *in) {
    return in != NULL ? in->actions : NULL;
}

/* ---------- Handles ---------- */

static u32 handle_id(u32 index, u32 generation) {
    return (generation << DE__ACTION_INDEX_BITS) | (index + 1u);
}

u32 de__action_map_index(const de_input *in, de_action_map m) {
    const struct de__actions *s = de__actions_state_const(in);
    if (s == NULL || m.id == 0)
        return DE__ACTION_NONE;
    const u32 slot = m.id & DE__ACTION_INDEX_MASK;
    if (slot == 0 || slot > s->map_count)
        return DE__ACTION_NONE;
    const u32 i = slot - 1u;
    if ((m.id >> DE__ACTION_INDEX_BITS) != s->maps[i].generation)
        return DE__ACTION_NONE;
    return i;
}

u32 de__action_index(const de_input *in, de_action a) {
    const struct de__actions *s = de__actions_state_const(in);
    if (s == NULL || a.id == 0)
        return DE__ACTION_NONE;
    const u32 slot = a.id & DE__ACTION_INDEX_MASK;
    if (slot == 0 || slot > s->action_count)
        return DE__ACTION_NONE;
    const u32 i = slot - 1u;
    if ((a.id >> DE__ACTION_INDEX_BITS) != s->actions[i].generation)
        return DE__ACTION_NONE;
    return i;
}

de_action_map de__action_map_handle(const de_input *in, u32 i) {
    de_action_map m = {0};
    const struct de__actions *s = de__actions_state_const(in);
    if (s != NULL && i < s->map_count)
        m.id = handle_id(i, s->maps[i].generation);
    return m;
}

de_action de__action_handle(const de_input *in, u32 i) {
    de_action a = {0};
    const struct de__actions *s = de__actions_state_const(in);
    if (s != NULL && i < s->action_count)
        a.id = handle_id(i, s->actions[i].generation);
    return a;
}

de__action *de__action_get(de_input *in, de_action a) {
    const u32 i = de__action_index(in, a);
    return i != DE__ACTION_NONE ? &in->actions->actions[i] : NULL;
}

const de__action *de__action_get_const(const de_input *in, de_action a) {
    const u32 i = de__action_index(in, a);
    return i != DE__ACTION_NONE ? &in->actions->actions[i] : NULL;
}

/* ---------- Validation ---------- */

static bool finite_f32(f32 v) {
    return v == v && v <= 3.0e38f && v >= -3.0e38f;
}

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

static bool code_ok(const de_binding *b) {
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

de_result de__binding_validate(de_action_type type, const de_binding *b, const char *what) {
    if (b == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s: NULL binding", what);
    if ((u32)b->source >= (u32)DE_BIND_SOURCE_COUNT_)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s: unknown source %u", what, (unsigned)b->source);
    if (!code_ok(b))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s: code %u is not valid for source %s", what,
                       (unsigned)b->code, de__binding_source_name(b->source));
    if (b->composite_slot < DE_COMPOSITE_NONE || b->composite_slot > DE_COMPOSITE_RIGHT)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s: composite slot %d (expected -1 to 3)", what,
                       (int)b->composite_slot);
    const bool is2d = binding_is_2d(b);
    /* Button actions ignore the composite slot (a zeroed binding has slot 0). */
    const bool composite = type != DE_ACTION_BUTTON && b->composite_slot >= 0;
    switch (type) {
    case DE_ACTION_BUTTON:
        break;
    case DE_ACTION_AXIS1D:
        if (is2d)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s: a 2D code in a 1D action", what);
        if (b->composite_slot > DE_COMPOSITE_NEGATIVE)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "%s: a 1D composite has slots 0 (positive) and 1 (negative), not %d",
                           what, (int)b->composite_slot);
        break;
    case DE_ACTION_AXIS2D:
        if (composite && is2d)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s: a 2D code in a composite slot", what);
        if (!composite && !is2d)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "%s: a 2D action needs a composite slot or a 2D code (stick, xy)", what);
        break;
    default:
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s: unknown action type %d", what, (int)type);
    }
    if (!finite_f32(b->scale))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s: scale is not finite", what);
    if (!finite_f32(b->deadzone) || b->deadzone < 0.0f || b->deadzone >= 1.0f)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s: deadzone %g is outside [0, 1)", what,
                       (double)b->deadzone);
    if (b->modifier_key >= (u32)DE_KEY_COUNT_)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s: modifier key %u is out of range", what,
                       (unsigned)b->modifier_key);
    return de__curve_validate(&b->curve, what);
}

static bool name_ok(const char *name) {
    if (name == NULL)
        return false;
    const size_t n = strlen(name);
    return n > 0 && n <= DE_ACTION_NAME_MAX;
}

static f32 positive_or(f32 v, f32 def) {
    return v > 0.0f ? v : def;
}

static de_result desc_validate(const de_action_desc *d) {
    if (d == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_action_add: desc is NULL");
    if (!name_ok(d->name))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_action_add: the name must have 1 to %d bytes",
                       DE_ACTION_NAME_MAX);
    if ((u32)d->type > (u32)DE_ACTION_AXIS2D)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_action_add: '%s': unknown type %d", d->name,
                       (int)d->type);
    if ((u32)d->interaction > (u32)DE_INTERACT_DOUBLE_TAP)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_action_add: '%s': unknown interaction %d",
                       d->name, (int)d->interaction);
    if (d->binding_count > DE_ACTION_MAX_BINDINGS)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_action_add: '%s': %u bindings (max %d)",
                       d->name, (unsigned)d->binding_count, DE_ACTION_MAX_BINDINGS);
    if (d->binding_count > 0 && d->bindings == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_action_add: '%s': bindings is NULL", d->name);
    const f32 times[4] = {d->hold_seconds, d->tap_seconds, d->double_tap_seconds,
                          d->press_threshold};
    for (u32 i = 0; i < 4; ++i)
        if (!finite_f32(times[i]) || times[i] < 0.0f)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "de_action_add: '%s': times and thresholds must be >= 0", d->name);
    for (u32 i = 0; i < d->binding_count; ++i) {
        char what[DE_ACTION_NAME_MAX + 48];
        const int wn =
            snprintf(what, sizeof what, "binding %u of action '%s'", (unsigned)i, d->name);
        if (wn < 0)
            what[0] = '\0';
        DE_TRY(de__binding_validate(d->type, &d->bindings[i], what));
    }
    return DE_OK;
}

/* ---------- Bindings storage ---------- */

/* Reserves `count` binding slots for `act`: in place when they fit its range, else a new range
 * at the end of the pool (the old one is left unused). */
static de_result reserve_bindings(struct de__actions *s, de__action *act, u32 count) {
    if (count <= act->binding_capacity)
        return DE_OK;
    if (s->binding_count + count > DE_ACTION_MAX_BINDINGS_TOTAL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "action '%s': the binding pool is full (%d)",
                       act->name, DE_ACTION_MAX_BINDINGS_TOTAL);
    act->first_binding = s->binding_count;
    act->binding_capacity = count;
    s->binding_count += count;
    return DE_OK;
}

static void store_bindings(struct de__actions *s, de__action *act, const de_binding *b, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        de_binding c = b[i];
        if (act->type == DE_ACTION_BUTTON)
            c.composite_slot = DE_COMPOSITE_NONE;
        s->bindings[act->first_binding + i] = c;
    }
    act->binding_count = count;
}

void de__action_reset(de__action *act) {
    memset(&act->frame, 0, sizeof act->frame);
    memset(&act->tick, 0, sizeof act->tick);
    memset(&act->track, 0, sizeof act->track);
    act->pending_trigger = false;
    act->winning_binding = DE__ACTION_NONE;
}

/* ---------- Public: maps and actions ---------- */

static u32 find_map(const struct de__actions *s, const char *name) {
    for (u32 i = 0; i < s->map_count; ++i)
        if (strcmp(s->maps[i].name, name) == 0)
            return i;
    return DE__ACTION_NONE;
}

static u32 find_action(const struct de__actions *s, u32 map, const char *name) {
    for (u32 i = s->maps[map].first_action; i != DE__ACTION_NONE; i = s->actions[i].next_in_map)
        if (strcmp(s->actions[i].name, name) == 0)
            return i;
    return DE__ACTION_NONE;
}

de_action_map de_action_map_create(de_input *in, const char *name) {
    de_action_map m = {0};
    if (in == NULL || !name_ok(name)) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                "de_action_map_create: needs an input state and a name of 1 to %d bytes",
                DE_ACTION_NAME_MAX);
        return m;
    }
    struct de__actions *s = de__actions_get_or_create(in);
    if (s == NULL)
        return m;
    u32 i = find_map(s, name);
    if (i == DE__ACTION_NONE) {
        if (s->map_count >= DE_ACTION_MAX_MAPS) {
            DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_action_map_create: '%s': %d maps already (max)", name,
                    DE_ACTION_MAX_MAPS);
            return m;
        }
        i = s->map_count++;
        de__action_map *mp = &s->maps[i];
        memset(mp, 0, sizeof *mp);
        memcpy(mp->name, name, strlen(name) + 1);
        mp->id = de_name_from(name);
        mp->generation = 1;
        mp->first_action = DE__ACTION_NONE;
        mp->last_action = DE__ACTION_NONE;
    }
    return de__action_map_handle(in, i);
}

de_action de_action_add(de_input *in, de_action_map m, const de_action_desc *desc) {
    de_action a = {0};
    const u32 mi = de__action_map_index(in, m);
    if (mi == DE__ACTION_NONE) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_action_add: invalid input state or map handle");
        return a;
    }
    if (desc_validate(desc) != DE_OK)
        return a;
    struct de__actions *s = in->actions;
    u32 i = find_action(s, mi, desc->name);
    de__action *act;
    if (i == DE__ACTION_NONE) {
        if (s->action_count >= DE_ACTION_MAX_ACTIONS) {
            DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_action_add: '%s': %d actions already (max)",
                    desc->name, DE_ACTION_MAX_ACTIONS);
            return a;
        }
        if (s->binding_count + desc->binding_count > DE_ACTION_MAX_BINDINGS_TOTAL) {
            DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_action_add: '%s': the binding pool is full (%d)",
                    desc->name, DE_ACTION_MAX_BINDINGS_TOTAL);
            return a;
        }
        i = s->action_count++;
        act = &s->actions[i];
        memset(act, 0, sizeof *act);
        memcpy(act->name, desc->name, strlen(desc->name) + 1);
        act->id = de_name_from(desc->name);
        act->generation = 1;
        act->map = mi;
        act->next_in_map = DE__ACTION_NONE;
        act->first_binding = s->binding_count;
        de__action_map *mp = &s->maps[mi];
        if (mp->last_action == DE__ACTION_NONE)
            mp->first_action = i;
        else
            s->actions[mp->last_action].next_in_map = i;
        mp->last_action = i;
        mp->action_count++;
    } else {
        act = &s->actions[i];
    }
    if (reserve_bindings(s, act, desc->binding_count) != DE_OK)
        return a; /* only a replace can get here; the action keeps its old bindings */
    act->type = desc->type;
    act->interaction = desc->interaction;
    act->hold_seconds = positive_or(desc->hold_seconds, DE_ACTION_DEFAULT_HOLD_SECONDS);
    act->tap_seconds = positive_or(desc->tap_seconds, DE_ACTION_DEFAULT_TAP_SECONDS);
    act->double_tap_seconds =
        positive_or(desc->double_tap_seconds, DE_ACTION_DEFAULT_DOUBLE_TAP_SECONDS);
    act->press_threshold = positive_or(desc->press_threshold, DE_ACTION_DEFAULT_PRESS_THRESHOLD);
    act->rebindable = desc->rebindable;
    store_bindings(s, act, desc->bindings, desc->binding_count);
    de__action_reset(act);
    return de__action_handle(in, i);
}

de_action_map de_action_map_find(const de_input *in, const char *name) {
    const struct de__actions *s = de__actions_state_const(in);
    if (s == NULL || name == NULL)
        return (de_action_map){0};
    const u32 i = find_map(s, name);
    return i != DE__ACTION_NONE ? de__action_map_handle(in, i) : (de_action_map){0};
}

de_action de_action_find(const de_input *in, de_action_map m, const char *name) {
    const u32 mi = de__action_map_index(in, m);
    if (mi == DE__ACTION_NONE || name == NULL)
        return (de_action){0};
    const u32 i = find_action(in->actions, mi, name);
    return i != DE__ACTION_NONE ? de__action_handle(in, i) : (de_action){0};
}

const char *de_action_name(const de_input *in, de_action a) {
    const de__action *act = de__action_get_const(in, a);
    return act != NULL ? act->name : "";
}

const char *de_action_map_name(const de_input *in, de_action_map m) {
    const u32 mi = de__action_map_index(in, m);
    return mi != DE__ACTION_NONE ? in->actions->maps[mi].name : "";
}

/* ---------- Internal iteration and bindings ---------- */

u32 de__action_map_count(const de_input *in) {
    const struct de__actions *s = de__actions_state_const(in);
    return s != NULL ? s->map_count : 0u;
}

u32 de__action_map_actions(const de_input *in, de_action_map m, de_action *out, u32 max) {
    const u32 mi = de__action_map_index(in, m);
    if (mi == DE__ACTION_NONE)
        return 0;
    const struct de__actions *s = in->actions;
    u32 n = 0;
    for (u32 i = s->maps[mi].first_action; i != DE__ACTION_NONE; i = s->actions[i].next_in_map) {
        if (out != NULL && n < max)
            out[n] = de__action_handle(in, i);
        ++n;
    }
    return n;
}

de_action_map de__action_map_of(const de_input *in, de_action a) {
    const de__action *act = de__action_get_const(in, a);
    return act != NULL ? de__action_map_handle(in, act->map) : (de_action_map){0};
}

u32 de__action_binding_count(const de_input *in, de_action a) {
    const de__action *act = de__action_get_const(in, a);
    return act != NULL ? act->binding_count : 0u;
}

bool de__action_get_binding(const de_input *in, de_action a, u32 index, de_binding *out) {
    const de__action *act = de__action_get_const(in, a);
    if (act == NULL || out == NULL || index >= act->binding_count)
        return false;
    *out = in->actions->bindings[act->first_binding + index];
    return true;
}

de_result de__action_set_binding(de_input *in, de_action a, u32 index, const de_binding *b) {
    de__action *act = de__action_get(in, a);
    if (act == NULL || index >= act->binding_count)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__action_set_binding: bad action or index %u",
                       (unsigned)index);
    DE_TRY(de__binding_validate(act->type, b, act->name));
    de_binding c = *b;
    if (act->type == DE_ACTION_BUTTON)
        c.composite_slot = DE_COMPOSITE_NONE;
    in->actions->bindings[act->first_binding + index] = c;
    return DE_OK;
}

de_result de__action_set_bindings(de_input *in, de_action a, const de_binding *b, u32 count) {
    de__action *act = de__action_get(in, a);
    if (act == NULL || count > DE_ACTION_MAX_BINDINGS || (count > 0 && b == NULL))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__action_set_bindings: bad action or bindings");
    for (u32 i = 0; i < count; ++i)
        DE_TRY(de__binding_validate(act->type, &b[i], act->name));
    DE_TRY(reserve_bindings(in->actions, act, count));
    store_bindings(in->actions, act, b, count);
    return DE_OK;
}
