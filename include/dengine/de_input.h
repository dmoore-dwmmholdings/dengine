/* de_input.h — devices, action maps, rebinding, haptics, gyro, recording/replay. SPEC.md §15. */
#ifndef DE_INPUT_H
#define DE_INPUT_H
#include "de_base.h"
#include "de_math.h"
#include "de_ecs.h"
#include "de_platform.h"
DE_EXTERN_C_BEGIN

/*
 * Input state (S21, DENG-83).
 *
 * de_input turns the raw device events of the platform layer (de_platform_input_events(), in
 * engine numbering, no SDL types) into an engine-owned state: keys, mouse, up to
 * DE_INPUT_MAX_PADS gamepads with their sensors, and text input. Action maps, rebinding, haptics,
 * gyro aiming, glyphs and recording build on it.
 *
 * Frame flow (SPEC §10.2 step 1): the host pumps platform events, passes each app event to
 * de_input_app_event() (window focus), then calls de_input_begin_frame() once. begin_frame clears
 * last frame's edges and deltas, applies every raw event that arrived since the previous call in
 * order, and then updates gyro and haptics. All queries below read the state of the current frame
 * until the next begin_frame. de_input_attach() also makes the state an ECS resource and registers
 * the action systems (PreUpdate and FixedUpdate).
 *
 * Edges: `pressed` is true only in the frame of the down event and `released` only in the frame of
 * the up event. A down and an up inside one frame give pressed and released true and down false.
 * OS key repeats do not change the state (no new pressed edge).
 *
 * Conventions:
 * - Mouse position and motion are in window logical points, origin top left, +y down. The wheel
 *   is in notches, +y away from the user (up), +x right, the same on every OS.
 * - Sticks are in [-1, 1] with +x right and +y UP (pushed away from the player, "forward"); the
 *   platform's +y down is flipped here. Triggers are in [0, 1]. No dead zone is applied (dead
 *   zones are action modifiers).
 * - Gyro in rad/s, accelerometer in m/s² (gravity included). Axes for a pad held level: +x
 *   right, +y up, +z towards the player; positive rotation is counter-clockwise around an axis.
 *
 * Gamepad handles carry a generation: after a disconnect the handle is stale, and every query
 * with it returns false or zero. A pad that connects again gets a new handle.
 *
 * Threading: main thread only (like the platform devices), except the const queries, which may
 * run on any thread while nothing calls a non-const function (systems in a schedule).
 */

typedef struct de_input de_input;
DE_HANDLE(de_action);
DE_HANDLE(de_action_map);
DE_HANDLE(de_gamepad);

/** Most gamepads tracked at once. More connected pads are ignored (with a warning). */
#define DE_INPUT_MAX_PADS 8
/** Capacity of one frame's committed text, in bytes (UTF-8, without the NUL). Text beyond it is
 * dropped whole (at a raw-event boundary, so the kept text stays valid UTF-8). */
#define DE_INPUT_TEXT_BYTES 256
/** Capacity of the IME composition string, in bytes (UTF-8, without the NUL). */
#define DE_INPUT_COMPOSITION_BYTES 31

/**
 * de_input_create() parameters. `(de_input_desc){0}` is valid.
 *
 * - `allocator`: every allocation of the input state (one block at create). NULL means
 *   de_heap_allocator(). Must outlive the input state.
 * - `window`: the window that relative mouse mode and text input apply to. Zero means the window
 *   that last had keyboard focus (de_input_app_event(), or the window of a key or mouse event).
 * - `pad_sensors_off`: true keeps gamepad gyros and accelerometers off. By default de_input turns
 *   them on when a pad that has them connects, so de_pad_gyro() / de_pad_accel() get samples.
 */
typedef struct de_input_desc {
    const de_allocator *allocator;
    de_window window;
    bool pad_sensors_off;
} de_input_desc;

/**
 * Creates the input state. All storage (key and button arrays, DE_INPUT_MAX_PADS pad slots, the
 * per-frame raw event list, the text buffers) is allocated here, once; nothing is allocated
 * after create. `desc` may be NULL (all defaults).
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (`out` NULL) or DE_ERR_OUT_OF_MEMORY.
 */
DE_API de_result de_input_create(const de_input_desc *desc, de_input **out);
/** Frees the input state. NULL is ignored. Does not touch an attached world: call
 * de_input_detach() first, or destroy the world first. */
DE_API void de_input_destroy(de_input *in);

/** The ECS resource that de_input_attach() inserts: struct type "de_input_resource" in the
 * world's type registry. Systems find it with de_resource_find() and declare DE_READ on it. */
typedef struct de_input_resource {
    de_input *input;
} de_input_resource;

/**
 * Makes `in` part of world `w`: registers the type de_input_resource and inserts it as a
 * resource, and adds two systems that write that resource: "input.actions" in
 * DE_SCHEDULE_PRE_UPDATE (action evaluation per frame) and "input.fixed_tick" in
 * DE_SCHEDULE_FIXED_UPDATE (per-tick action sampling). Gameplay systems that read the resource
 * therefore run after them. Attaching to the same world again only re-inserts the resource.
 * Single-threaded; not while a schedule of `w` runs.
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (NULL argument, or `in` is attached to another world);
 *         or the error of the type registration, resource insert or system add.
 */
DE_API de_result de_input_attach(de_input *in, de_world *w);
/** Removes the two systems and the resource from the attached world. Nothing happens when `in`
 * is not attached. Single-threaded; not while a schedule of the world runs. */
DE_API void de_input_detach(de_input *in);

/**
 * Passes one app event of de_platform_pump() to the input state. Only DE_EV_WINDOW_FOCUS matters
 * now: a focus loss (`a` = 0) releases every held key, mouse button and pad button, with released
 * edges, and centres the pad axes, in the next de_input_begin_frame(), so nothing stays stuck
 * while the window is in the background. A focus gain records the focus window. Other kinds are
 * ignored. Call it between the pump and de_input_begin_frame().
 */
DE_API void de_input_app_event(de_input *in, const de_app_event *ev);

/**
 * Starts a new input frame. Called by the host once per frame after pumping platform events
 * (SPEC §10.2 step 1): clears the edges, mouse delta and wheel and the frame's text, applies every
 * raw event that arrived since the last call (de_platform_input_events()) in order, applies a
 * pending focus loss, then updates the gyro and haptics. Allocates nothing. Main thread only.
 */
DE_API void de_input_begin_frame(de_input *in);

/* ---------- Raw device state ---------- */

/**
 * Physical keys, as USB HID usage codes of the keyboard page (0x07): the numbering of SDL3
 * scancodes and of de_raw_key.code. They name a key position, not what the layout prints on it:
 * DE_KEY_A is the key right of Caps Lock, labelled Q on an AZERTY board. Codes 257..290 continue
 * SDL3's numbering for keys outside page 0x07 (media and application keys).
 */
typedef enum de_key {
    DE_KEY_UNKNOWN = 0,
    DE_KEY_A = 4,
    DE_KEY_B = 5,
    DE_KEY_C = 6,
    DE_KEY_D = 7,
    DE_KEY_E = 8,
    DE_KEY_F = 9,
    DE_KEY_G = 10,
    DE_KEY_H = 11,
    DE_KEY_I = 12,
    DE_KEY_J = 13,
    DE_KEY_K = 14,
    DE_KEY_L = 15,
    DE_KEY_M = 16,
    DE_KEY_N = 17,
    DE_KEY_O = 18,
    DE_KEY_P = 19,
    DE_KEY_Q = 20,
    DE_KEY_R = 21,
    DE_KEY_S = 22,
    DE_KEY_T = 23,
    DE_KEY_U = 24,
    DE_KEY_V = 25,
    DE_KEY_W = 26,
    DE_KEY_X = 27,
    DE_KEY_Y = 28,
    DE_KEY_Z = 29,
    DE_KEY_1 = 30,
    DE_KEY_2 = 31,
    DE_KEY_3 = 32,
    DE_KEY_4 = 33,
    DE_KEY_5 = 34,
    DE_KEY_6 = 35,
    DE_KEY_7 = 36,
    DE_KEY_8 = 37,
    DE_KEY_9 = 38,
    DE_KEY_0 = 39,
    DE_KEY_RETURN = 40,
    DE_KEY_ESCAPE = 41,
    DE_KEY_BACKSPACE = 42,
    DE_KEY_TAB = 43,
    DE_KEY_SPACE = 44,
    DE_KEY_MINUS = 45,
    DE_KEY_EQUALS = 46,
    DE_KEY_LEFTBRACKET = 47,
    DE_KEY_RIGHTBRACKET = 48,
    DE_KEY_BACKSLASH = 49,
    DE_KEY_NONUSHASH = 50,
    DE_KEY_SEMICOLON = 51,
    DE_KEY_APOSTROPHE = 52,
    DE_KEY_GRAVE = 53,
    DE_KEY_COMMA = 54,
    DE_KEY_PERIOD = 55,
    DE_KEY_SLASH = 56,
    DE_KEY_CAPSLOCK = 57,
    DE_KEY_F1 = 58,
    DE_KEY_F2 = 59,
    DE_KEY_F3 = 60,
    DE_KEY_F4 = 61,
    DE_KEY_F5 = 62,
    DE_KEY_F6 = 63,
    DE_KEY_F7 = 64,
    DE_KEY_F8 = 65,
    DE_KEY_F9 = 66,
    DE_KEY_F10 = 67,
    DE_KEY_F11 = 68,
    DE_KEY_F12 = 69,
    DE_KEY_PRINTSCREEN = 70,
    DE_KEY_SCROLLLOCK = 71,
    DE_KEY_PAUSE = 72,
    DE_KEY_INSERT = 73,
    DE_KEY_HOME = 74,
    DE_KEY_PAGEUP = 75,
    DE_KEY_DELETE = 76,
    DE_KEY_END = 77,
    DE_KEY_PAGEDOWN = 78,
    DE_KEY_RIGHT = 79,
    DE_KEY_LEFT = 80,
    DE_KEY_DOWN = 81,
    DE_KEY_UP = 82,
    DE_KEY_NUMLOCKCLEAR = 83,
    DE_KEY_KP_DIVIDE = 84,
    DE_KEY_KP_MULTIPLY = 85,
    DE_KEY_KP_MINUS = 86,
    DE_KEY_KP_PLUS = 87,
    DE_KEY_KP_ENTER = 88,
    DE_KEY_KP_1 = 89,
    DE_KEY_KP_2 = 90,
    DE_KEY_KP_3 = 91,
    DE_KEY_KP_4 = 92,
    DE_KEY_KP_5 = 93,
    DE_KEY_KP_6 = 94,
    DE_KEY_KP_7 = 95,
    DE_KEY_KP_8 = 96,
    DE_KEY_KP_9 = 97,
    DE_KEY_KP_0 = 98,
    DE_KEY_KP_PERIOD = 99,
    DE_KEY_NONUSBACKSLASH = 100,
    DE_KEY_APPLICATION = 101,
    DE_KEY_POWER = 102,
    DE_KEY_KP_EQUALS = 103,
    DE_KEY_F13 = 104,
    DE_KEY_F14 = 105,
    DE_KEY_F15 = 106,
    DE_KEY_F16 = 107,
    DE_KEY_F17 = 108,
    DE_KEY_F18 = 109,
    DE_KEY_F19 = 110,
    DE_KEY_F20 = 111,
    DE_KEY_F21 = 112,
    DE_KEY_F22 = 113,
    DE_KEY_F23 = 114,
    DE_KEY_F24 = 115,
    DE_KEY_EXECUTE = 116,
    DE_KEY_HELP = 117,
    DE_KEY_MENU = 118,
    DE_KEY_SELECT = 119,
    DE_KEY_STOP = 120,
    DE_KEY_AGAIN = 121,
    DE_KEY_UNDO = 122,
    DE_KEY_CUT = 123,
    DE_KEY_COPY = 124,
    DE_KEY_PASTE = 125,
    DE_KEY_FIND = 126,
    DE_KEY_MUTE = 127,
    DE_KEY_VOLUMEUP = 128,
    DE_KEY_VOLUMEDOWN = 129,
    DE_KEY_KP_COMMA = 133,
    DE_KEY_KP_EQUALSAS400 = 134,
    DE_KEY_INTERNATIONAL1 = 135,
    DE_KEY_INTERNATIONAL2 = 136,
    DE_KEY_INTERNATIONAL3 = 137,
    DE_KEY_INTERNATIONAL4 = 138,
    DE_KEY_INTERNATIONAL5 = 139,
    DE_KEY_INTERNATIONAL6 = 140,
    DE_KEY_INTERNATIONAL7 = 141,
    DE_KEY_INTERNATIONAL8 = 142,
    DE_KEY_INTERNATIONAL9 = 143,
    DE_KEY_LANG1 = 144,
    DE_KEY_LANG2 = 145,
    DE_KEY_LANG3 = 146,
    DE_KEY_LANG4 = 147,
    DE_KEY_LANG5 = 148,
    DE_KEY_LANG6 = 149,
    DE_KEY_LANG7 = 150,
    DE_KEY_LANG8 = 151,
    DE_KEY_LANG9 = 152,
    DE_KEY_ALTERASE = 153,
    DE_KEY_SYSREQ = 154,
    DE_KEY_CANCEL = 155,
    DE_KEY_CLEAR = 156,
    DE_KEY_PRIOR = 157,
    DE_KEY_RETURN2 = 158,
    DE_KEY_SEPARATOR = 159,
    DE_KEY_OUT = 160,
    DE_KEY_OPER = 161,
    DE_KEY_CLEARAGAIN = 162,
    DE_KEY_CRSEL = 163,
    DE_KEY_EXSEL = 164,
    DE_KEY_FRONT = 165,
    DE_KEY_KP_00 = 176,
    DE_KEY_KP_000 = 177,
    DE_KEY_THOUSANDSSEPARATOR = 178,
    DE_KEY_DECIMALSEPARATOR = 179,
    DE_KEY_CURRENCYUNIT = 180,
    DE_KEY_CURRENCYSUBUNIT = 181,
    DE_KEY_KP_LEFTPAREN = 182,
    DE_KEY_KP_RIGHTPAREN = 183,
    DE_KEY_KP_LEFTBRACE = 184,
    DE_KEY_KP_RIGHTBRACE = 185,
    DE_KEY_KP_TAB = 186,
    DE_KEY_KP_BACKSPACE = 187,
    DE_KEY_KP_A = 188,
    DE_KEY_KP_B = 189,
    DE_KEY_KP_C = 190,
    DE_KEY_KP_D = 191,
    DE_KEY_KP_E = 192,
    DE_KEY_KP_F = 193,
    DE_KEY_KP_XOR = 194,
    DE_KEY_KP_POWER = 195,
    DE_KEY_KP_PERCENT = 196,
    DE_KEY_KP_LESS = 197,
    DE_KEY_KP_GREATER = 198,
    DE_KEY_KP_AMPERSAND = 199,
    DE_KEY_KP_DBLAMPERSAND = 200,
    DE_KEY_KP_VERTICALBAR = 201,
    DE_KEY_KP_DBLVERTICALBAR = 202,
    DE_KEY_KP_COLON = 203,
    DE_KEY_KP_HASH = 204,
    DE_KEY_KP_SPACE = 205,
    DE_KEY_KP_AT = 206,
    DE_KEY_KP_EXCLAM = 207,
    DE_KEY_KP_MEMSTORE = 208,
    DE_KEY_KP_MEMRECALL = 209,
    DE_KEY_KP_MEMCLEAR = 210,
    DE_KEY_KP_MEMADD = 211,
    DE_KEY_KP_MEMSUBTRACT = 212,
    DE_KEY_KP_MEMMULTIPLY = 213,
    DE_KEY_KP_MEMDIVIDE = 214,
    DE_KEY_KP_PLUSMINUS = 215,
    DE_KEY_KP_CLEAR = 216,
    DE_KEY_KP_CLEARENTRY = 217,
    DE_KEY_KP_BINARY = 218,
    DE_KEY_KP_OCTAL = 219,
    DE_KEY_KP_DECIMAL = 220,
    DE_KEY_KP_HEXADECIMAL = 221,
    DE_KEY_LCTRL = 224,
    DE_KEY_LSHIFT = 225,
    DE_KEY_LALT = 226,
    DE_KEY_LGUI = 227,
    DE_KEY_RCTRL = 228,
    DE_KEY_RSHIFT = 229,
    DE_KEY_RALT = 230,
    DE_KEY_RGUI = 231,
    DE_KEY_MODE = 257,
    DE_KEY_SLEEP = 258,
    DE_KEY_WAKE = 259,
    DE_KEY_CHANNEL_INCREMENT = 260,
    DE_KEY_CHANNEL_DECREMENT = 261,
    DE_KEY_MEDIA_PLAY = 262,
    DE_KEY_MEDIA_PAUSE = 263,
    DE_KEY_MEDIA_RECORD = 264,
    DE_KEY_MEDIA_FAST_FORWARD = 265,
    DE_KEY_MEDIA_REWIND = 266,
    DE_KEY_MEDIA_NEXT_TRACK = 267,
    DE_KEY_MEDIA_PREVIOUS_TRACK = 268,
    DE_KEY_MEDIA_STOP = 269,
    DE_KEY_MEDIA_EJECT = 270,
    DE_KEY_MEDIA_PLAY_PAUSE = 271,
    DE_KEY_MEDIA_SELECT = 272,
    DE_KEY_AC_NEW = 273,
    DE_KEY_AC_OPEN = 274,
    DE_KEY_AC_CLOSE = 275,
    DE_KEY_AC_EXIT = 276,
    DE_KEY_AC_SAVE = 277,
    DE_KEY_AC_PRINT = 278,
    DE_KEY_AC_PROPERTIES = 279,
    DE_KEY_AC_SEARCH = 280,
    DE_KEY_AC_HOME = 281,
    DE_KEY_AC_BACK = 282,
    DE_KEY_AC_FORWARD = 283,
    DE_KEY_AC_STOP = 284,
    DE_KEY_AC_REFRESH = 285,
    DE_KEY_AC_BOOKMARKS = 286,
    DE_KEY_SOFTLEFT = 287,
    DE_KEY_SOFTRIGHT = 288,
    DE_KEY_CALL = 289,
    DE_KEY_ENDCALL = 290,
    DE_KEY_COUNT_ = 512
} de_key;
typedef enum de_mouse_button {
    DE_MOUSE_LEFT,
    DE_MOUSE_RIGHT,
    DE_MOUSE_MIDDLE,
    DE_MOUSE_X1,
    DE_MOUSE_X2,
    DE_MOUSE_BUTTON_COUNT_
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
    DE_PAD_PADDLE1, /**< right upper */
    DE_PAD_PADDLE2, /**< left upper */
    DE_PAD_PADDLE3, /**< right lower */
    DE_PAD_PADDLE4, /**< left lower */
    DE_PAD_TOUCHPAD,
    DE_PAD_BUTTON_COUNT_
} de_pad_button;
/** Gamepad axes. Sticks in [-1, 1], +x right, +y up (forward); triggers in [0, 1]. */
typedef enum de_pad_axis {
    DE_AXIS_LEFT_X,
    DE_AXIS_LEFT_Y,
    DE_AXIS_RIGHT_X,
    DE_AXIS_RIGHT_Y,
    DE_AXIS_LEFT_TRIGGER,
    DE_AXIS_RIGHT_TRIGGER,
    DE_PAD_AXIS_COUNT_
} de_pad_axis;

/** Stable lower-case name of a key ("a", "1", "return", "lshift", "kp_enter", "media_play"), for
 * binding files and debug output; NULL for codes without a name. */
DE_API const char *de_key_name(de_key k);
/** The key named `name` (as de_key_name() gives it; ASCII case-insensitive), or DE_KEY_UNKNOWN. */
DE_API de_key de_key_from_name(const char *name);

DE_API bool de_key_down(const de_input *in, de_key k);
DE_API bool de_key_pressed(const de_input *in, de_key k);  /* this frame */
DE_API bool de_key_released(const de_input *in, de_key k); /* this frame */

DE_API bool de_mouse_down(const de_input *in, de_mouse_button b);
DE_API bool de_mouse_pressed(const de_input *in, de_mouse_button b);  /* this frame */
DE_API bool de_mouse_released(const de_input *in, de_mouse_button b); /* this frame */
/** Cursor position in the focus window, logical points, origin top left. Not updated while
 * relative mode is on. */
DE_API de_vec2 de_mouse_position(const de_input *in);
/** Mouse motion this frame (sum of the frame's events), logical points, +y down. Also in relative
 * mode. */
DE_API de_vec2 de_mouse_delta(const de_input *in);
/** Wheel motion this frame, in notches: +y away from the user, +x right. */
DE_API de_vec2 de_mouse_wheel(const de_input *in);
/** Relative mouse mode (hidden, confined cursor; unbounded motion for camera control) on the
 * input's window. The setting is kept and applied as soon as a window is known. */
DE_API void de_mouse_set_relative(de_input *in, bool relative);
/** The relative mouse mode last set with de_mouse_set_relative(). */
DE_API bool de_mouse_relative(const de_input *in);

/** Gamepad families. SWITCH covers the Switch Pro controller and Joy-Cons, STEAM the Steam Deck
 * and the Steam Controller. Same values as de_platform_pad_type. */
typedef enum de_pad_type {
    DE_PAD_TYPE_GENERIC,
    DE_PAD_TYPE_XBOX,
    DE_PAD_TYPE_PS4,
    DE_PAD_TYPE_PS5,
    DE_PAD_TYPE_SWITCH,
    DE_PAD_TYPE_STEAM
} de_pad_type;

/** Gamepad capability flags (de_gamepad_info.caps). */
typedef enum de_pad_cap {
    DE_PAD_CAP_RUMBLE = 1u << 0,
    DE_PAD_CAP_TRIGGER_RUMBLE = 1u << 1,    /**< Xbox One / Series impulse triggers */
    DE_PAD_CAP_ADAPTIVE_TRIGGERS = 1u << 2, /**< DualSense */
    DE_PAD_CAP_GYRO = 1u << 3,
    DE_PAD_CAP_ACCEL = 1u << 4,
    DE_PAD_CAP_TOUCHPAD = 1u << 5,
    DE_PAD_CAP_LED = 1u << 6
} de_pad_cap;

/** A connected gamepad (de_pad_info()). */
typedef struct de_gamepad_info {
    de_pad_type type;
    u32 caps;            /**< de_pad_cap flags */
    u16 vendor, product; /**< USB ids; 0 when unknown */
    i32 player_index;    /**< the index the OS or the pad shows; -1 = none */
    char name[64];       /**< UTF-8 */
} de_gamepad_info;

/** Writes up to `max` connected gamepads to `out`, in connection order (oldest first), and returns
 * how many are connected, so `de_gamepad_list(in, NULL, 0)` counts them. */
DE_API u32 de_gamepad_list(const de_input *in, de_gamepad *out, u32 max);
/** True while `p` is a connected gamepad (false once it is stale). */
DE_API bool de_pad_connected(const de_input *in, de_gamepad p);
/** Describes gamepad `p`. False (and `*out` zeroed) for a stale handle or NULL `out`. */
DE_API bool de_pad_info(const de_input *in, de_gamepad p, de_gamepad_info *out);
DE_API bool de_pad_down(const de_input *in, de_gamepad p, de_pad_button b);
DE_API bool de_pad_pressed(const de_input *in, de_gamepad p, de_pad_button b);  /* this frame */
DE_API bool de_pad_released(const de_input *in, de_gamepad p, de_pad_button b); /* this frame */
/** Axis value: sticks [-1, 1] (+y up), triggers [0, 1]; 0 for a stale handle. */
DE_API f32 de_pad_axis_value(const de_input *in, de_gamepad p, de_pad_axis a);

/** Which kind of device the player used last (for prompt glyphs). */
typedef enum de_input_device {
    DE_INPUT_DEVICE_NONE,           /**< nothing yet, or the last used pad disconnected */
    DE_INPUT_DEVICE_KEYBOARD_MOUSE, /**< a key or mouse button went down, the mouse moved or
                                         the wheel turned */
    DE_INPUT_DEVICE_GAMEPAD         /**< a pad button went down or an axis passed 0.5 */
} de_input_device;
/** The device class used last; for DE_INPUT_DEVICE_GAMEPAD, `pad` (may be NULL) gets the pad. */
DE_API de_input_device de_input_last_device(const de_input *in, de_gamepad *pad);

/* ---------- Text input ---------- */

/** Text typed this frame and the current IME composition (de_text_input_get()). The strings
 * belong to the input state and are valid until the next de_input_begin_frame(). */
typedef struct de_text_input_state {
    const char *text;        /**< committed UTF-8 text of this frame, NUL-terminated ("" if none) */
    u32 text_bytes;          /**< its length in bytes */
    bool text_dropped;       /**< some of this frame's text did not fit DE_INPUT_TEXT_BYTES */
    const char *composition; /**< the IME composition (pre-edit) string, "" when none */
    i32 composition_cursor;  /**< edit cursor, in code points of the composition; -1 unknown */
    i32 composition_length;  /**< selected code points after the cursor; -1 unknown */
    bool active;             /**< de_text_input_start() is in effect */
} de_text_input_state;

/**
 * Starts text input on the input's window (de_platform_text_input_start()): the platform then
 * sends committed text and IME composition, and on-screen keyboards may show. `area` (may be NULL)
 * places the IME candidate window.
 * @return DE_OK, or the platform's error (DE_ERR_INVALID_ARGUMENT when no window is known yet,
 *         DE_ERR_UNSUPPORTED when headless).
 */
DE_API de_result de_text_input_start(de_input *in, const de_platform_text_area *area);
/** Stops text input and clears the composition. */
DE_API void de_text_input_stop(de_input *in);
/** This frame's text and the current composition. Committed text is collected also when text
 * input was not started through de_input (the platform decides what it sends). */
DE_API void de_text_input_get(const de_input *in, de_text_input_state *out);

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
/** The pad's gyro, rad/s, after calibration (DENG-87; until then the latest raw sample). False
 * (and `*rad_per_sec` zero) for a stale handle or a pad that has sent no gyro sample yet. */
DE_API bool de_pad_gyro(const de_input *in, de_gamepad p, de_vec3 *rad_per_sec);
/** The pad's latest accelerometer sample, m/s², gravity included. False (and zero) for a stale
 * handle or a pad that has sent no accelerometer sample yet. */
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
