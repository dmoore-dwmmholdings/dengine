/* input_script.h: the scripted input session behind the input recording test vector (DENG-89).
 *
 * The script is the same on every platform: integer-hashed key, mouse and pad events per frame,
 * fed through injection (no devices, no SDL), evaluated by the real action maps (composites,
 * deadzones, a power curve, every interaction), with 0, 1 or 2 fixed ticks per frame. The
 * checked-in tests/determinism/input/input_replay.deir is this script recorded live; the unit tests
 * (tests/unit/input/test_record_replay.c) and the determinism scenario (test_det_input.c) use it.
 *
 * Never change the script or the schema without regenerating the golden files (see
 * test_det_input.c). */
#ifndef DE_INPUT_SCRIPT_H
#define DE_INPUT_SCRIPT_H
#include "input/input_internal.h"

/** Fixed ticks of the scripted session. */
#define INPUT_SCRIPT_TICKS 1000u
/** Tick number of the first recorded tick (the recording's start tick). */
#define INPUT_SCRIPT_FIRST_TICK 100u
/** Fixed ticks per second (header of the recording). */
#define INPUT_SCRIPT_TICK_RATE 60u
/** Device id of the scripted gamepad. */
#define INPUT_SCRIPT_PAD 7u
/** Actions of the schema, in declaration order. */
#define INPUT_SCRIPT_ACTIONS 10u

/** Creates the schema on `in`: maps "gameplay" (8 actions) and "menu" (2 actions), both pushed.
 * False on failure. */
bool input_script_schema(de_input *in);
/** The action handles of the schema, in declaration order (INPUT_SCRIPT_ACTIONS). */
void input_script_actions(const de_input *in, de_action *out);

/** Called after the latch of every scripted fixed tick: `index` counts from 0, `tick` from
 * INPUT_SCRIPT_FIRST_TICK. */
typedef void (*input_script_tick_fn)(de_input *in, u64 tick, u32 index, void *user);
/**
 * Runs `ticks` scripted fixed ticks live on `in` (schema already built): per frame it queues the
 * frame's events, starts the frame (de__input_begin_frame_dt), evaluates the actions, then runs
 * the frame's fixed ticks (de__actions_fixed_tick) and calls `fn` after each.
 */
void input_script_run_live(de_input *in, u32 ticks, input_script_tick_fn fn, void *user);

/** The scripted raw events of frame `frame` (queued with de__input_inject). Exposed so tests can
 * feed "live" noise during a replay. */
void input_script_events(de_input *in, u32 frame);

#endif
