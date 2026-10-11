/* de_input.h — devices, action maps, rebinding, haptics, gyro, recording/replay. SPEC.md §15. */
#ifndef DE_INPUT_H
#define DE_INPUT_H
#include "de_base.h"
#include "de_math.h"
#include "de_ecs.h"
DE_EXTERN_C_BEGIN

typedef struct de_input de_input;
DE_HANDLE(de_action);
DE_HANDLE(de_action_map);
DE_HANDLE(de_gamepad);

DE_API de_result de_input_create(const de_allocator *a, de_input **out);
DE_API void de_input_destroy(de_input *in);
DE_API void de_input_attach(de_input *in, de_world *w); /* resource + PreUpdate system */
DE_API void de_input_begin_frame(de_input *in); /* called by the host after pumping events */

/* ---------- Raw device state ---------- */
typedef enum de_key {
    DE_KEY_UNKNOWN = 0,
    DE_KEY_A = 4,
    /* ... USB HID usage codes ... */ DE_KEY_COUNT_ = 512
} de_key;
typedef enum de_mouse_button {
    DE_MOUSE_LEFT,
    DE_MOUSE_RIGHT,
    DE_MOUSE_MIDDLE,
    DE_MOUSE_X1,
    DE_MOUSE_X2
} de_mouse_button;
typedef enum de_pad_button {
    DE_PAD_SOUTH,
    DE_PAD_EAST,
    DE_PAD_WEST,
    DE_PAD_NORTH,
    DE_PAD_BACK,
    DE_PAD_GUIDE,
    DE_PAD_START,
    DE_PAD_LSTICK,
    DE_PAD_RSTICK,
    DE_PAD_LSHOULDER,
    DE_PAD_RSHOULDER,
    DE_PAD_DPAD_UP,
    DE_PAD_DPAD_DOWN,
    DE_PAD_DPAD_LEFT,
    DE_PAD_DPAD_RIGHT,
    DE_PAD_MISC1,
    DE_PAD_PADDLE1,
    DE_PAD_PADDLE2,
    DE_PAD_PADDLE3,
    DE_PAD_PADDLE4,
    DE_PAD_TOUCHPAD,
    DE_PAD_BUTTON_COUNT_
} de_pad_button;
typedef enum de_pad_axis {
    DE_AXIS_LEFT_X,
    DE_AXIS_LEFT_Y,
    DE_AXIS_RIGHT_X,
    DE_AXIS_RIGHT_Y,
    DE_AXIS_LEFT_TRIGGER,
    DE_AXIS_RIGHT_TRIGGER,
    DE_PAD_AXIS_COUNT_
} de_pad_axis;

DE_API bool de_key_down(const de_input *in, de_key k);
DE_API bool de_key_pressed(const de_input *in, de_key k); /* this frame */
DE_API de_vec2 de_mouse_position(const de_input *in);
DE_API de_vec2 de_mouse_delta(const de_input *in);
DE_API void de_mouse_set_relative(de_input *in, bool relative);
DE_API u32 de_gamepad_list(const de_input *in, de_gamepad *out, u32 max);
DE_API bool de_pad_down(const de_input *in, de_gamepad p, de_pad_button b);
DE_API f32 de_pad_axis_value(const de_input *in, de_gamepad p, de_pad_axis a);

/* ---------- Haptics and sensors ---------- */
DE_API void de_pad_rumble(de_input *in, de_gamepad p, f32 low, f32 high, f32 seconds);
DE_API void de_pad_trigger_rumble(de_input *in, de_gamepad p, f32 left, f32 right, f32 seconds);
typedef struct de_trigger_effect {
    u8 mode;
    u8 start, end;
    u8 strength;
    u8 frequency;
} de_trigger_effect;
DE_API void de_pad_adaptive_trigger(de_input *in, de_gamepad p, bool right,
                                    const de_trigger_effect *fx);
DE_API bool de_pad_gyro(const de_input *in, de_gamepad p, de_vec3 *rad_per_sec);
DE_API bool de_pad_accel(const de_input *in, de_gamepad p, de_vec3 *m_per_s2);
DE_API void de_pad_gyro_calibrate(de_input *in, de_gamepad p, f32 seconds);

/* ---------- Action maps ---------- */
typedef enum de_action_type { DE_ACTION_BUTTON, DE_ACTION_AXIS1D, DE_ACTION_AXIS2D } de_action_type;
typedef enum de_interaction {
    DE_INTERACT_PRESS,
    DE_INTERACT_RELEASE,
    DE_INTERACT_HOLD,
    DE_INTERACT_TAP,
    DE_INTERACT_DOUBLE_TAP
} de_interaction;

typedef enum de_binding_source {
    DE_BIND_KEY,
    DE_BIND_MOUSE_BUTTON,
    DE_BIND_MOUSE_DELTA,
    DE_BIND_MOUSE_WHEEL,
    DE_BIND_PAD_BUTTON,
    DE_BIND_PAD_AXIS,
    DE_BIND_GYRO
} de_binding_source;

typedef struct de_binding {
    de_binding_source source;
    u32 code;          /* key / button / axis */
    i8 composite_slot; /* -1 none; 0..3 = up/down/left/right for 2D composites */
    f32 scale, deadzone;
    bool invert;
    u32 modifier_key; /* 0 = none (e.g. Shift+key) */
} de_binding;

typedef struct de_action_desc {
    const char *name;
    de_action_type type;
    de_interaction interaction;
    f32 hold_seconds;
    const de_binding *bindings;
    u32 binding_count;
    bool rebindable;
} de_action_desc;

DE_API de_action_map de_action_map_create(de_input *in, const char *name);
DE_API de_action de_action_add(de_input *in, de_action_map m, const de_action_desc *desc);
DE_API de_result de_action_maps_load(de_input *in, const char *vpath); /* TOML */
DE_API void de_action_map_push(de_input *in, de_action_map m);         /* context stack */
DE_API void de_action_map_pop(de_input *in, de_action_map m);

DE_API bool de_action_triggered(const de_input *in, de_action a);
DE_API bool de_action_held(const de_input *in, de_action a);
DE_API f32 de_action_value(const de_input *in, de_action a);
DE_API de_vec2 de_action_value2(const de_input *in, de_action a);

/* ---------- Rebinding ---------- */
DE_API void de_rebind_begin(de_input *in, de_action a, u32 binding_index,
                            const de_binding_source *allowed, u32 allowed_count);
DE_API bool de_rebind_poll(de_input *in, de_binding *captured,
                           de_action *conflict); /* true when done */
DE_API void de_rebind_cancel(de_input *in);
DE_API de_result de_bindings_save(const de_input *in,
                                  const char *vpath); /* "user:/bindings.toml" */
DE_API de_result de_bindings_load(de_input *in, const char *vpath);

/* Prompt glyphs (Xbox, PlayStation, Switch, Steam Deck, keyboard) for UI. */
DE_API const char *de_action_glyph_id(const de_input *in, de_action a);

/* ---------- Recording / replay (per fixed tick, resolved actions) ---------- */
typedef struct de_input_recording de_input_recording;
DE_API de_result de_input_record_begin(de_input *in, const de_allocator *a,
                                       de_input_recording **out);
DE_API void de_input_record_tick(de_input *in, de_input_recording *rec, u64 tick);
DE_API de_result de_input_record_save(const de_input_recording *rec, const char *vpath);
DE_API de_result de_input_replay_load(de_input *in, const char *vpath, de_input_recording **out);
DE_API bool de_input_replay_tick(de_input *in, de_input_recording *rec,
                                 u64 tick); /* false at end */

/* Serialized action state for netcode (rollback/lockstep exchange inputs, not raw devices). */
DE_API u32 de_input_pack_actions(const de_input *in, de_action_map m, void *dst, u32 max_bytes);
DE_API void de_input_unpack_actions(de_input *in, de_action_map m, u32 player, const void *src,
                                    u32 bytes);

DE_EXTERN_C_END
#endif
