/* input_internal.h: the input state behind de_input.h, shared by the files of src/input/
 * (DENG-83). Action maps (DENG-84), gyro and haptics (DENG-87), rebinding (DENG-86) and
 * recording (DENG-89) read it through this header.
 *
 * Everything is in fixed arrays, in a fixed order (pad slots by index, pad connection order in
 * `pad_order`, the frame's raw events in arrival order). There is no hash map, so code built on
 * this state (the deterministic replay stream of DENG-89) iterates it in a stable order. */
#ifndef DE_INPUT_INTERNAL_H
#define DE_INPUT_INTERNAL_H
#include <dengine/de_input.h>
#include <dengine/de_platform.h>

DE_EXTERN_C_BEGIN

/** Raw events kept per frame (de_input.frame_events). More events in one frame still update the
 * state, but are not listed (counted in frame_events_dropped). */
#define DE__INPUT_FRAME_EVENTS DE_PLATFORM_INPUT_RING
/** Capacity of the test injection queue (de__input_inject). */
#define DE__INPUT_INJECT_CAP 256
/** 64-bit words of a key bitset. */
#define DE__INPUT_KEY_WORDS (DE_KEY_COUNT_ / 64)
/** |axis| at which a pad counts as the device in use (de_input_last_device). */
#define DE__INPUT_ACTIVITY_AXIS 0.5f
/** Bits of a de_gamepad id that hold the slot index + 1; the generation is above them. */
#define DE__INPUT_PAD_SLOT_BITS 4u

/** An input event: the platform's raw event, in de_input numbering (DENG-246). Tests and replay
 * inject the same type (de__input_inject). */
typedef de_raw_input_event de__input_event;

/** Hook state of later tasks (opaque here; each owner allocates its own in its init hook). */
struct de__actions;
struct de__gyro;
struct de__haptics;

/** One gamepad slot. A free slot has device 0. */
typedef struct de__input_pad {
    u32 device;     /**< platform instance id; 0 = free slot */
    u32 generation; /**< handle generation of the pad in this slot (moves on at each connect) */
    u64 connect_seq;
    de_gamepad_info info;
    u32 buttons;                  /**< held, bit per de_pad_button */
    u32 pressed;                  /**< went down this frame */
    u32 released;                 /**< went up this frame */
    f32 axes[DE_PAD_AXIS_COUNT_]; /**< engine convention: sticks +y up, triggers [0, 1] */
    /* Sensors. `*_time_ns` is the host receive time (de_time_now_ns timebase), `*_sensor_ns` the
     * device clock; `*_samples` counts this frame's samples (all of them are in frame_events). */
    de_vec3 gyro_raw;
    u64 gyro_time_ns, gyro_sensor_ns;
    u32 gyro_samples;
    bool has_gyro;
    /** Processed gyro, rad/s: what de_pad_gyro() returns. de__gyro_update() fills it each frame
     * (the DENG-83 stub copies gyro_raw; DENG-87 writes calibrated values). */
    de_vec3 gyro;
    de_vec3 accel;
    u64 accel_time_ns, accel_sensor_ns;
    u32 accel_samples;
    bool has_accel;
} de__input_pad;

struct de_input {
    de_allocator alloc;
    u32 mem_tag;
    de_log_category log;
    de_window window;       /**< de_input_desc.window (0 = follow focus) */
    de_window focus_window; /**< the window that last had keyboard focus */
    bool pad_sensors_off;

    u64 frame;         /**< begin_frame count */
    u64 last_frame_ns; /**< de_time_now_ns() at the last de_input_begin_frame (0 before) */

    /* Keyboard: bit k of word k / 64. */
    u64 keys_down[DE__INPUT_KEY_WORDS];
    u64 keys_pressed[DE__INPUT_KEY_WORDS];
    u64 keys_released[DE__INPUT_KEY_WORDS];
    u16 key_mods; /**< de_key_mod flags of the last key event */

    /* Mouse: bit per de_mouse_button. */
    u8 mouse_down, mouse_pressed, mouse_released;
    de_vec2 mouse_pos, mouse_delta, mouse_wheel;
    bool relative;       /**< requested relative mode */
    bool relative_dirty; /**< not yet applied to a window */

    /* Gamepads. pad_order[0 .. pad_count) are slot indices in connection order. */
    de__input_pad pads[DE_INPUT_MAX_PADS];
    u8 pad_order[DE_INPUT_MAX_PADS];
    u32 pad_count;
    u64 pad_seq;
    u64 pads_ignored; /**< pads that connected while every slot was taken */

    /* Text. */
    char text[DE_INPUT_TEXT_BYTES + 1];
    u32 text_len;
    bool text_dropped;
    char composition[DE_INPUT_COMPOSITION_BYTES + 1];
    i32 comp_cursor, comp_length;
    bool text_active;

    de_input_device last_device;
    de_gamepad last_pad;
    bool focus_lost_pending;

    /* This frame's raw events in arrival order (injected first, then the platform ring). For
     * DENG-84 evaluation, DENG-86 listen mode and DENG-87 gyro integration. */
    de__input_event frame_events[DE__INPUT_FRAME_EVENTS];
    u32 frame_event_count;
    u32 frame_events_dropped; /**< this frame */
    u64 events_dropped_total;

    /* Test / replay injection: applied, in order, at the next begin_frame. */
    de__input_event inject[DE__INPUT_INJECT_CAP];
    u32 inject_count;
    u64 inject_dropped;
    /* Metadata for injected pads, used by their DE_RAW_PAD_ADDED (by device id). */
    de_platform_pad_info inject_pad_info[DE_INPUT_MAX_PADS];
    u32 inject_pad_info_count;

    /* ECS (de_input_attach). */
    de_world *world;
    de_resource_id resource;
    de_system sys_actions; /**< "input.actions", PreUpdate */
    de_system sys_fixed;   /**< "input.fixed_tick", FixedUpdate */
    u64 action_frame_runs; /**< runs of input.actions (tests) */
    u64 action_fixed_runs; /**< runs of input.fixed_tick (tests) */

    /* Hook state of later tasks. */
    struct de__actions *actions;
    struct de__gyro *gyro;
    struct de__haptics *haptics;
};

/* ---------- Pads ---------- */

/** The slot of live pad handle `p`, or NULL when `p` is stale or invalid. */
de__input_pad *de__input_pad_get(de_input *in, de_gamepad p);
const de__input_pad *de__input_pad_get_const(const de_input *in, de_gamepad p);
/** The handle of the pad in slot `slot` (must be live). */
de_gamepad de__input_pad_handle(const de_input *in, u32 slot);

/* ---------- Frame ---------- */

/** de_input_begin_frame() with an explicit frame delta (seconds), for tests and replay. */
void de__input_begin_frame_dt(de_input *in, f32 dt);
/** Applies one event to the state and lists it in frame_events (input_state.c). */
void de__input_apply(de_input *in, const de__input_event *ev);
/** Clears this frame's edges, deltas and text (input_state.c). */
void de__input_clear_frame(de_input *in);
/** Releases every held key, mouse button and pad button (with released edges) and centres the
 * pad axes (input_state.c). */
void de__input_release_all(de_input *in);

/* ---------- Test and replay injection ---------- */

/**
 * Queues `ev` to be applied at the next begin_frame, before the platform's events, so tests never
 * need SDL or real devices. A full queue (DE__INPUT_INJECT_CAP) drops the event and counts it.
 */
void de__input_inject(de_input *in, const de__input_event *ev);
/** Metadata for an injected pad: used when the DE_RAW_PAD_ADDED of `info->id` is applied
 * (instead of asking the platform). At most DE_INPUT_MAX_PADS pending entries. */
void de__input_inject_pad_info(de_input *in, const de_platform_pad_info *info);

/* ---------- Hooks (no-op stubs in DENG-83; their owners replace the files) ---------- */

/** actions.c (DENG-84): allocate / free the action state (called by create / destroy). */
de_result de__actions_init(de_input *in);
void de__actions_shutdown(de_input *in);
/** actions.c (DENG-84): evaluate the action maps for this frame (system input.actions,
 * PreUpdate). */
void de__actions_frame_update(de_input *in, f32 dt);
/** actions.c (DENG-84): sample the actions for fixed tick `tick` (system input.fixed_tick,
 * FixedUpdate). */
void de__actions_fixed_tick(de_input *in, u64 tick);

/** gyro.c (DENG-87): allocate / free gyro state. */
de_result de__gyro_init(de_input *in);
void de__gyro_shutdown(de_input *in);
/** gyro.c (DENG-87): fill de__input_pad.gyro from the raw samples (de_input_begin_frame). */
void de__gyro_update(de_input *in, f32 dt);

/** haptics.c (DENG-87): allocate / free haptics state. */
de_result de__haptics_init(de_input *in);
void de__haptics_shutdown(de_input *in);
/** haptics.c (DENG-87): advance rumble timers and send effects (de_input_begin_frame). */
void de__haptics_update(de_input *in, f32 dt);

DE_EXTERN_C_END
#endif
