/* action_internal.h: action maps behind de_input.h (DENG-84), and the contract that the later
 * input tasks build on: context stacks (DENG-85), rebinding (DENG-86), glyphs (DENG-88) and
 * recording / replay / netcode packing (DENG-89). Those tasks add declarations here; they do not
 * edit action_eval.c.
 *
 * Files:
 *   - action_map.c: the storage (one block, allocated by the first de_action_map_create), maps,
 *     actions and bindings, handles, validation, iteration, get / set binding.
 *   - action_eval.c: the per-frame evaluation (sources, chords, composites, combination,
 *     interactions) into the frame view.
 *   - action_modifiers.c: deadzone -> curve -> scale -> invert.
 *   - actions.c: the hooks of input.c (init, shutdown, frame update, fixed tick), the tick view
 *     latch and its override hook, and the public queries.
 *   - context_stack.c: which maps are active and in which order, and whether an input is
 *     consumed. DENG-85 replaced the DENG-84 minimal version (context_internal.h): a stack with
 *     the top map evaluated first, blocking of lower maps, named context sets, and transitions;
 *     the de__context_* signatures below are unchanged.
 *   - action_toml.c: de_action_maps_load, and the name tables of binding sources, codes and
 *     composite slots (shared with DENG-86 save and DENG-88 glyphs).
 *
 * Determinism (SPEC §5.7): everything is in fixed arrays in declaration order. Maps, actions and
 * bindings are evaluated in that order; names are looked up by a linear scan, never by iterating
 * a hash map. Modifiers use de_dmath, so the same raw inputs give the same values on every
 * architecture. */
#ifndef DE_INPUT_ACTION_INTERNAL_H
#define DE_INPUT_ACTION_INTERNAL_H
#include "context_internal.h" /* DENG-85: de__context_state */
#include "input_internal.h"

DE_EXTERN_C_BEGIN

/** Bits of a de_action / de_action_map id that hold the slot index + 1; the generation is
 * above them. */
#define DE__ACTION_INDEX_BITS 12u
#define DE__ACTION_INDEX_MASK ((1u << DE__ACTION_INDEX_BITS) - 1u)
/** Index meaning "none" (no binding won, no map). */
#define DE__ACTION_NONE 0xFFFFFFFFu

DE_STATIC_ASSERT(DE_ACTION_MAX_ACTIONS < DE__ACTION_INDEX_MASK, "action index fits the handle");
DE_STATIC_ASSERT(DE_ACTION_MAX_MAPS < DE__ACTION_INDEX_MASK, "map index fits the handle");

/** One view of an action's state (frame view or tick view). */
typedef struct de__action_state {
    bool triggered; /**< the interaction fired (this frame, or since the previous tick) */
    bool held;      /**< actuated: magnitude >= press_threshold */
    de_vec2 value;  /**< button / 1D: (value, 0); 2D: (x, y), +y up */
} de__action_state;

/** Interaction tracking of one action (action_eval.c). */
typedef struct de__action_track {
    bool actuated;   /**< actuated at the last frame update */
    bool hold_fired; /**< HOLD fired for the current press */
    f32 held_time;   /**< seconds since the current press (0 in the press frame) */
    u32 tap_count;   /**< DOUBLE_TAP: 1 after a first tap, waiting for the second */
    f32 since_tap;   /**< DOUBLE_TAP: seconds since the release of the first tap */
    bool second_ok;  /**< DOUBLE_TAP: the current press came inside the window */
} de__action_track;

typedef struct de__action_map {
    char name[DE_ACTION_NAME_MAX + 1];
    de_name id;       /**< de_name_from(name): lookup only */
    u32 generation;   /**< handle generation (1 ..) */
    u32 first_action; /**< first action of the map in declaration order, or DE__ACTION_NONE */
    u32 last_action;
    u32 action_count;
} de__action_map;

typedef struct de__action {
    char name[DE_ACTION_NAME_MAX + 1];
    de_name id;
    u32 generation;
    u32 map;         /**< map index */
    u32 next_in_map; /**< next action of the map, or DE__ACTION_NONE */
    de_action_type type;
    de_interaction interaction;
    f32 hold_seconds, tap_seconds, double_tap_seconds, press_threshold; /**< defaults resolved */
    bool rebindable;
    /* Bindings: bindings[first_binding .. first_binding + binding_count) of de__actions;
     * binding_capacity slots are reserved there (a replace that needs more moves the range). */
    u32 first_binding;
    u32 binding_count;
    u32 binding_capacity;

    de__action_state frame; /**< frame view (de__actions_frame_update) */
    de__action_state tick;  /**< tick view (de__actions_fixed_tick) */
    bool pending_trigger;   /**< triggered in a frame since the last tick latched */
    de__action_track track;
    u32 winning_binding; /**< binding index that won this frame, or DE__ACTION_NONE */
} de__action;

/** The action state of one de_input (de_input.actions; NULL until the first map). */
struct de__actions {
    de__action_map maps[DE_ACTION_MAX_MAPS];
    u32 map_count;
    de__action actions[DE_ACTION_MAX_ACTIONS];
    u32 action_count;
    de_binding bindings[DE_ACTION_MAX_BINDINGS_TOTAL];
    u32 binding_count; /**< used slots of `bindings` (reserved ranges included) */

    u64 frames; /**< de__actions_frame_update calls */
    u64 ticks;  /**< de__actions_fixed_tick calls */
    u64 last_tick;

    /* Tick override (DENG-89 replay, netcode unpack): runs after each tick latch. */
    void (*tick_override)(de_input *in, u64 tick, void *user);
    void *tick_override_user;

    /* Context stack (context_stack.c). DENG-85 may add fields below. */
    u32 active[DE_ACTION_MAX_MAPS]; /**< active map indices, in push order */
    u32 active_count;
    /* DENG-85: `active` is the applied stack, bottom to top, each map once (kept for readers of
     * the DENG-84 fields); the full state is here (context_internal.h). */
    de__context_state context;
};

/* ---------- Storage, handles, iteration (action_map.c) ---------- */

/** The action state, allocating it on first use. NULL (error context set) when out of memory. */
struct de__actions *de__actions_get_or_create(de_input *in);
/** The action state, or NULL before the first map. */
struct de__actions *de__actions_state(de_input *in);
const struct de__actions *de__actions_state_const(const de_input *in);

/** Index of a live map / action handle, or DE__ACTION_NONE when stale, invalid or foreign. */
u32 de__action_map_index(const de_input *in, de_action_map m);
u32 de__action_index(const de_input *in, de_action a);
/** Handles of slot `i` (must exist). */
de_action_map de__action_map_handle(const de_input *in, u32 i);
de_action de__action_handle(const de_input *in, u32 i);
/** The action of a handle, or NULL. */
de__action *de__action_get(de_input *in, de_action a);
const de__action *de__action_get_const(const de_input *in, de_action a);

/** Number of maps; maps are numbered 0 .. count - 1 in creation order. */
u32 de__action_map_count(const de_input *in);
/** Writes up to `max` actions of map `m` to `out`, in declaration order, and returns how many
 * the map has. */
u32 de__action_map_actions(const de_input *in, de_action_map m, de_action *out, u32 max);
/** The map of action `a`, or an invalid handle. */
de_action_map de__action_map_of(const de_input *in, de_action a);

/** Number of bindings of action `a` (0 for an invalid handle). */
u32 de__action_binding_count(const de_input *in, de_action a);
/** Copies binding `index` of `a` to `*out`. False for a bad handle or index. */
bool de__action_get_binding(const de_input *in, de_action a, u32 index, de_binding *out);
/** Replaces binding `index` of `a` (DENG-86 rebinding). The binding is validated against the
 * action type like de_action_add. DE_ERR_INVALID_ARGUMENT for a bad handle, index or binding. */
de_result de__action_set_binding(de_input *in, de_action a, u32 index, const de_binding *b);
/** Replaces all bindings of `a` (DENG-86 load). `count` <= DE_ACTION_MAX_BINDINGS. */
de_result de__action_set_bindings(de_input *in, de_action a, const de_binding *b, u32 count);
/** Checks binding `b` for an action of `type`. DE_OK, or DE_ERR_INVALID_ARGUMENT with a message
 * that names `what` ("binding 2 of action 'move'"). */
de_result de__binding_validate(de_action_type type, const de_binding *b, const char *what);
/** Resets an action's views and interaction tracking to idle. */
void de__action_reset(de__action *act);

/* ---------- Frame evaluation (action_eval.c) ---------- */

/** Evaluates every active map into the frame view (called by de__actions_frame_update) and
 * resets the actions of inactive maps. Allocates nothing. */
void de__actions_evaluate(de_input *in, f32 dt);

/* ---------- Modifiers (action_modifiers.c) ---------- */

/** deadzone -> curve -> scale -> invert on a 1D value. `bounded` (pad axes) clamps the magnitude
 * to 1 after the deadzone. */
f32 de__modifiers_apply_1d(const de_binding *b, f32 v, bool bounded);
/** The same on a 2D value: radial deadzone, curve on the length, scale, invert negates y. */
de_vec2 de__modifiers_apply_2d(const de_binding *b, de_vec2 v, bool bounded);
/** The response curve alone, on a magnitude >= 0. */
f32 de__curve_eval(const de_response_curve *c, f32 m);
/** Checks a curve: DE_OK or DE_ERR_INVALID_ARGUMENT (message names `what`). */
de_result de__curve_validate(const de_response_curve *c, const char *what);

/* ---------- Tick view (actions.c) ---------- */

/** Copies the tick view of `a`. False (and `*out` zeroed) for a bad handle. */
bool de__action_tick_state_get(const de_input *in, de_action a, de__action_state *out);
/** Overwrites the tick view of `a` (replay, netcode unpack). False for a bad handle. */
bool de__action_tick_state_set(de_input *in, de_action a, const de__action_state *s);
/** Copies the frame view of `a`. False (and `*out` zeroed) for a bad handle. */
bool de__action_frame_state_get(const de_input *in, de_action a, de__action_state *out);
/**
 * Sets the tick override: `fn(in, tick, user)` runs at the end of every de__actions_fixed_tick,
 * after the tick view is latched from the frame view, and may overwrite tick views with
 * de__action_tick_state_set (DENG-89 replay feeds recorded actions, netcode unpacks remote ones).
 * NULL removes it. One override at a time; the last call wins.
 */
void de__action_tick_override(de_input *in, void (*fn)(de_input *in, u64 tick, void *user),
                              void *user);
/** True when the calling thread runs a FixedUpdate system (queries then read the tick view). */
bool de__actions_in_fixed_update(void);

/* ---------- Context stack (context_stack.c, DENG-85; state in context_internal.h) ---------- */

/** Called once when the action state is created / destroyed. */
void de__context_init(de_input *in);
void de__context_shutdown(de_input *in);
/** Writes the maps to evaluate this frame to `out` (map indices, evaluation order: earlier maps
 * see inputs first) and returns how many. DENG-85: the applied stack from the top down, then the
 * maps that are releasing (they read every input as consumed). */
u32 de__context_active_maps(de_input *in, u32 *out, u32 max);
/** Called at the start of each evaluation, before any map. DENG-85: applies the requested stack
 * (transitions) and resets the consumed inputs. */
void de__context_frame_begin(de_input *in);
/**
 * "Is this input consumed?": asked for every binding before it is read. True makes the binding
 * read 0 this frame. DENG-85: a blocking map above used the input, the binding is latched
 * (held since before its map joined, or since a higher map consumed it), or the map is
 * releasing.
 */
bool de__context_binding_consumed(de_input *in, u32 map, u32 action, u32 binding,
                                  const de_binding *b);
/** Called for every binding of an evaluated map whose raw input is active (non-zero before
 * modifiers), after its action is evaluated. DENG-85: a blocking map marks the input consumed
 * for the maps below it. */
void de__context_binding_used(de_input *in, u32 map, u32 action, u32 binding, const de_binding *b);
/** Whether map index `map` is on the applied stack (DENG-85: not the requested one). */
bool de__context_map_active(const de_input *in, u32 map);

/* ---------- Names (action_toml.c) ---------- */

/** Stable lower-case names of binding sources ("key", "mouse_button", "mouse_delta",
 * "mouse_wheel", "pad_button", "pad_axis", "gyro"); NULL when out of range. */
const char *de__binding_source_name(de_binding_source s);
/** The source named `name` (`len` bytes, exact). False when unknown. */
bool de__binding_source_from_name(const char *name, size_t len, de_binding_source *out);
/** Stable name of `code` for source `s` (de_key_name for keys; "left", "south", "left_x",
 * "left_stick", "x", "xy", ...); NULL when it has none. */
const char *de__binding_code_name(de_binding_source s, u32 code);
/** The code named `name` (`len` bytes; key names ASCII case-insensitive) for source `s`. */
bool de__binding_code_from_name(de_binding_source s, const char *name, size_t len, u32 *out);
/** Composite slot names: "up", "down", "left", "right" (2D), "positive", "negative" (1D). */
const char *de__composite_name(de_action_type t, i8 slot);
bool de__composite_from_name(const char *name, size_t len, de_action_type t, i8 *out);

/* ---------- TOML (action_toml.c) ---------- */

/**
 * de_action_maps_load on text: parses `len` bytes named `source` in messages, checks the whole
 * file, then adds its maps and actions. `unknown_keys` (may be NULL) gets the number of unknown
 * keys that were warned about. Errors as de_action_maps_load.
 */
de_result de__action_maps_parse(de_input *in, const char *text, size_t len, const char *source,
                                u32 *unknown_keys);

DE_EXTERN_C_END
#endif
