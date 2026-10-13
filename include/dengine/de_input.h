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
    DE_PAD_CAP_LED = 1u << 6,
    /** HD (voice-coil) haptics (DENG-87). Never set yet: SDL3 has no API for it, so no pad
     * reports it and there is no HD haptics call. */
    DE_PAD_CAP_HD_HAPTICS = 1u << 7
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

/* ---------- Haptics and sensors ----------
 * Haptics (DENG-87). The engine owns effect durations: de_input_begin_frame() counts them down
 * in frame time and stops an effect when it ends. A new call on the same pad replaces the
 * running effect of that kind. Calls on a stale handle, or on a pad without the capability
 * (de_gamepad_info.caps), do nothing (a Dev build logs the missing capability once per pad).
 * Strengths are scaled by the saved cvars `input.haptics_scale` (float, [0, 1], default 1) and
 * `input.haptics_enabled` (bool, default true); a change applies to running effects in the next
 * frame. Main thread only. */

/** Longest rumble or trigger rumble, in seconds; longer durations are cut to it. */
#define DE_PAD_RUMBLE_MAX_SECONDS 60.0f

/** Starts the rumble motors: `low` (left, low frequency) and `high` (right, high frequency) in
 * [0, 1] (clamped, NaN is 0) for `seconds` of frame time. `seconds` <= 0 stops the rumble. Needs
 * DE_PAD_CAP_RUMBLE. */
DE_API void de_pad_rumble(de_input *in, de_gamepad p, f32 low, f32 high, f32 seconds);
/** Rumble in the triggers (Xbox One / Series impulse triggers), like de_pad_rumble(). Needs
 * DE_PAD_CAP_TRIGGER_RUMBLE. */
DE_API void de_pad_trigger_rumble(de_input *in, de_gamepad p, f32 left, f32 right, f32 seconds);
/** An adaptive trigger effect (de_pad_adaptive_trigger()). The meaning and the valid range of
 * each field depend on `mode` (de_trigger_mode); fields a mode does not use are ignored. */
typedef struct de_trigger_effect {
    u8 mode;
    u8 start, end;
    u8 strength;
    u8 frequency;
} de_trigger_effect;
/**
 * Sets the adaptive trigger effect of one trigger (`right` or left) on a DualSense
 * (DE_PAD_CAP_ADAPTIVE_TRIGGERS). The effect stays until the next call; NULL `fx` turns the
 * trigger effect off. An effect with a field out of its range (de_trigger_mode) is ignored and
 * logged. The engine builds the DualSense effect packet and sends it through
 * de_platform_pad_send_effect().
 */
DE_API void de_pad_adaptive_trigger(de_input *in, de_gamepad p, bool right,
                                    const de_trigger_effect *fx);
/** The pad's gyro, rad/s, with the calibrated bias removed (de_pad_gyro_calibrate(); the raw
 * value until a bias is known). The latest sample of the frame. False (and `*rad_per_sec` zero)
 * for a stale handle or a pad that has sent no gyro sample yet. */
DE_API bool de_pad_gyro(const de_input *in, de_gamepad p, de_vec3 *rad_per_sec);
/** The pad's latest accelerometer sample, m/s², gravity included. False (and zero) for a stale
 * handle or a pad that has sent no accelerometer sample yet. */
DE_API bool de_pad_accel(const de_input *in, de_gamepad p, de_vec3 *m_per_s2);
/**
 * Measures the gyro bias of pad `p` over the next `seconds` of frame time (clamped to [0.1, 10]).
 * The pad should lie still: if its gyro moves (a per-axis standard deviation above 0.03 rad/s, or
 * a mean above 0.15 rad/s) the calibration is rejected and the previous bias stays. On success
 * the mean becomes the bias that de_pad_gyro() and de_pad_gyro_aim() remove. `seconds` <= 0 (or
 * NaN) clears the bias. A call while a calibration runs restarts it. Progress and result:
 * de_pad_gyro_calibration_state().
 */
DE_API void de_pad_gyro_calibrate(de_input *in, de_gamepad p, f32 seconds);

/* ---- DENG-87: adaptive trigger modes, gyro calibration state and gyro aiming ---- */

/**
 * Adaptive trigger modes (de_trigger_effect.mode). Positions are trigger zones 0 (released) to
 * 9 (fully pulled); strengths 1 (weak) to 8 (strong).
 *
 * | mode                   | fields used                                      | DualSense effect |
 * |------------------------|--------------------------------------------------|------------------|
 * | DE_TRIGGER_OFF         | none                                             | 0x05             |
 * | DE_TRIGGER_FEEDBACK    | start 0..9, strength 1..8: resistance from start | 0x21             |
 * | DE_TRIGGER_WEAPON      | start 2..7, end start+1..8, strength 1..8: a     | 0x25             |
 * |                        | resistance that gives way at `end` (a trigger    |                  |
 * |                        | break)                                           |                  |
 * | DE_TRIGGER_VIBRATION   | start 0..9, strength (amplitude) 1..8, frequency | 0x26             |
 * |                        | 1..255 Hz: vibration from start                  |                  |
 *
 * Strength 0 (and frequency 0 for vibration) is the same as DE_TRIGGER_OFF.
 */
typedef enum de_trigger_mode {
    DE_TRIGGER_OFF,
    DE_TRIGGER_FEEDBACK,
    DE_TRIGGER_WEAPON,
    DE_TRIGGER_VIBRATION,
    DE_TRIGGER_MODE_COUNT_
} de_trigger_mode;

/** Gyro calibration state of a pad (de_pad_gyro_calibration_state()). */
typedef enum de_gyro_calibration {
    DE_GYRO_CALIB_NONE,               /**< no bias known: the gyro is used raw */
    DE_GYRO_CALIB_RUNNING,            /**< de_pad_gyro_calibrate() is collecting samples */
    DE_GYRO_CALIB_DONE,               /**< a bias (manual or automatic calibration) is applied */
    DE_GYRO_CALIB_REJECTED_MOTION,    /**< the last calibration saw motion; old bias kept */
    DE_GYRO_CALIB_REJECTED_NO_SAMPLES /**< the last calibration got no gyro sample */
} de_gyro_calibration;

/** The calibration state of pad `p`; `bias` (may be NULL) gets the bias in rad/s (zero when
 * none). DE_GYRO_CALIB_NONE for a stale handle. */
DE_API de_gyro_calibration de_pad_gyro_calibration_state(const de_input *in, de_gamepad p,
                                                         de_vec3 *bias);

/** Which rotation turns the aim left and right (de_gyro_settings.yaw_axis). */
typedef enum de_gyro_yaw_axis {
    DE_GYRO_YAW_LOCAL_Y, /**< turn the pad around its own up axis (yaw) */
    DE_GYRO_YAW_LOCAL_Z, /**< tilt the pad sideways (roll), like a steering wheel */
    /** Turn around the real-world up axis, found from the accelerometer, so yaw and roll both
     * work however the pad is held ("player space"). Falls back to LOCAL_Y without
     * accelerometer samples. */
    DE_GYRO_YAW_PLAYER
} de_gyro_yaw_axis;

/** What the ratchet buttons and action do (de_gyro_settings.ratchet_mode). */
typedef enum de_gyro_ratchet_mode {
    DE_GYRO_RATCHET_HOLD_TO_PAUSE, /**< no aim output while the ratchet is held */
    DE_GYRO_RATCHET_HOLD_TO_AIM    /**< aim output only while the ratchet is held */
} de_gyro_ratchet_mode;

/**
 * Gyro processing and aim settings of a pad (de_pad_gyro_configure()). Start from
 * de_gyro_settings_default(). Rates are physical rotation in rad/s, before the sensitivity.
 *
 * Per gyro sample the aim takes the yaw and pitch rates (yaw_axis), applies the tiered smoothing,
 * then the tightening, then sensitivity and inversion, and integrates over the sample interval.
 */
typedef struct de_gyro_settings {
    /** Aim radians per radian turned: x = yaw, y = pitch. Default 1, 1. */
    de_vec2 sensitivity;
    bool invert_x, invert_y;
    de_gyro_yaw_axis yaw_axis; /**< default DE_GYRO_YAW_LOCAL_Y */
    /** Below this rate the aim is scaled down linearly (rate / tightening), to hide small hand
     * shake. 0 (default) is off. */
    f32 tightening;
    /** Tiered smoothing: rates below half this threshold are fully smoothed, rates above it are
     * not smoothed, rates in between are blended. 0 (default) is off. */
    f32 smoothing_threshold;
    /** Smoothing window, seconds (at most 64 samples). Default 0.125. Clamped to [0, 1]. */
    f32 smoothing_time;
    /** Ratchet buttons: bit (1u << de_pad_button) per button. 0 (default) is no ratchet. While
     * the ratchet suppresses the aim, de_pad_gyro_aim() gives exactly zero, and when it lets go,
     * the aim resumes from the next sample, without the motion made meanwhile. */
    u32 ratchet_buttons;
    de_gyro_ratchet_mode ratchet_mode; /**< default DE_GYRO_RATCHET_HOLD_TO_PAUSE */
    /** Ratchet action: an action (any binding, any device) that counts as held while
     * de_action_held() is true in the frame view. Zero (default) is none. Works together with
     * `ratchet_buttons` (either one holds). The aim of a frame is exactly zero as soon as the
     * action is held in that frame's evaluation; actions are evaluated after the gyro update, so
     * the integration itself (and the smoothing reset) follows the action one frame later. */
    de_action ratchet_action;
    /** Learn the bias whenever the pad lies still for a second (gyro and accelerometer steady).
     * Default false: a slow, steady turn can look like a still pad. */
    bool auto_calibrate;
} de_gyro_settings;

/** The default gyro settings (see de_gyro_settings). */
DE_API de_gyro_settings de_gyro_settings_default(void);

/**
 * Sets the gyro settings of pad `p`. An invalid handle (id 0) sets them for every connected pad
 * and makes them the settings that pads get when they connect. A stale handle does nothing.
 * NULL `s` means de_gyro_settings_default(). NaN fields take their default value.
 */
DE_API void de_pad_gyro_configure(de_input *in, de_gamepad p, const de_gyro_settings *s);
/** The gyro settings of pad `p`, or of new pads for an invalid handle (id 0). False (and the
 * defaults in `*out`) for a stale handle. */
DE_API bool de_pad_gyro_settings(const de_input *in, de_gamepad p, de_gyro_settings *out);

/**
 * Gyro aim of this frame: how far to turn the aim, in radians, from every gyro sample of the
 * frame. +x is right and +y is up, like the sticks. The bias is removed and the settings of
 * de_pad_gyro_configure() applied. False (and zero) for a stale handle or a pad without gyro
 * samples.
 */
DE_API bool de_pad_gyro_aim(const de_input *in, de_gamepad p, de_vec2 *delta);
/* ---- end DENG-87 ---- */

/* ---------- Action maps ---------- */

/*
 * Action maps (S21, DENG-84). Games read named actions, not devices. An action map is a named
 * list of actions; each action has a type, an interaction and up to DE_ACTION_MAX_BINDINGS
 * bindings to keys, mouse, pad buttons and axes, or the gyro. Maps are inactive until
 * de_action_map_push(); de_action_map_pop() deactivates a map again. Active maps form a context
 * stack (DENG-85, see "Context stacks" below).
 *
 * Evaluation (system "input.actions", PreUpdate, after de_input_begin_frame), per active map from
 * the top of the context stack down, per action in the order it was added, per binding in index
 * order:
 *   1. Each binding reads its source. Every value is in "action space": +x right, +y UP. Keys
 *      and buttons give 0 or 1, pad axes their value (sticks [-1, 1], triggers [0, 1]), the
 *      mouse delta its motion this frame in points (+y up, so the platform's +y down is
 *      flipped), the wheel its notches, the gyro rad/s (de_pad_gyro). Pad sources read every
 *      connected pad; the largest magnitude wins, ties go to the pad connected first.
 *   2. Chords: a binding with `modifier_key` reads 0 unless that key is held. While a chord's
 *      modifier is held, it suppresses every binding of the same map that has the same source
 *      and code and no modifier (Shift+E suppresses plain E in that map).
 *   3. Modifiers, in this fixed order: deadzone -> curve -> scale -> invert (see de_binding).
 *   4. Composites: the bindings with a composite slot of one action form one composite. Each
 *      slot is the largest value (floored at 0) of its bindings. 2D: (right - left, up - down),
 *      scaled down to length 1 when it is longer (W+D gives (0.7071, 0.7071)). 1D:
 *      positive - negative.
 *   5. Combination: each other binding, and the composite, is a candidate; the candidate with
 *      the largest magnitude wins. Ties go to the lower binding index (the composite has the
 *      index of its first binding). So the result does not depend on hash or memory order.
 *   6. The action is actuated while its magnitude (button value, |1D value|, 2D length) is at
 *      least `press_threshold` (0.5 by default). The interaction turns actuation over time into
 *      `triggered` (see de_interaction).
 *
 * Two views. The frame view is computed once per frame by "input.actions" (PreUpdate). The tick
 * view is latched per fixed tick by "input.fixed_tick" (FixedUpdate): a trigger of the frame view
 * since the previous tick is reported in exactly one tick, also when FixedUpdate runs 0 times in
 * a frame (it then shows in the next tick) or N times (only the first of them). Queries from a
 * FixedUpdate system read the tick view; every other caller reads the frame view. Systems that
 * read actions declare DE_READ on de_input_resource, so they run after the input systems.
 *
 * Handles carry a generation. Maps and actions live as long as the input state; a stale or
 * foreign handle reads as an idle action. Main thread only, except the const queries.
 */

/** Most action maps, actions and bindings one input state holds (all maps together). */
#define DE_ACTION_MAX_MAPS           32
#define DE_ACTION_MAX_ACTIONS        256
#define DE_ACTION_MAX_BINDINGS_TOTAL 1024
/** Most bindings of one action. */
#define DE_ACTION_MAX_BINDINGS 16
/** Longest map or action name, in bytes without the NUL. */
#define DE_ACTION_NAME_MAX 63

/** Defaults that a zero field of de_action_desc stands for. */
#define DE_ACTION_DEFAULT_HOLD_SECONDS       0.5f
#define DE_ACTION_DEFAULT_TAP_SECONDS        0.2f
#define DE_ACTION_DEFAULT_DOUBLE_TAP_SECONDS 0.3f
#define DE_ACTION_DEFAULT_PRESS_THRESHOLD    0.5f

typedef enum de_action_type { DE_ACTION_BUTTON, DE_ACTION_AXIS1D, DE_ACTION_AXIS2D } de_action_type;
/**
 * When an action is `triggered` (de_action_triggered). "Press" is the frame the action becomes
 * actuated, "release" the frame it stops; a press and a release inside one frame (a key that
 * went down and up) count as both, with a held time of 0.
 * - PRESS: in the press frame.
 * - RELEASE: in the release frame.
 * - HOLD: once per press, in the frame the action has been actuated for `hold_seconds`.
 * - TAP: in the release frame, when it was actuated for less than `tap_seconds`.
 * - DOUBLE_TAP: in the release frame of a tap whose press came at most `double_tap_seconds`
 *   after the release of the previous tap. A third tap starts a new pair.
 */
typedef enum de_interaction {
    DE_INTERACT_PRESS,
    DE_INTERACT_RELEASE,
    DE_INTERACT_HOLD,
    DE_INTERACT_TAP,
    DE_INTERACT_DOUBLE_TAP
} de_interaction;

/**
 * Binding sources and the meaning of de_binding.code:
 * - KEY: a de_key. MOUSE_BUTTON: a de_mouse_button. PAD_BUTTON: a de_pad_button.
 * - PAD_AXIS: a de_pad_axis, or DE_BIND_LEFT_STICK / DE_BIND_RIGHT_STICK (2D, both axes).
 * - MOUSE_DELTA, MOUSE_WHEEL: DE_BIND_AXIS_X, DE_BIND_AXIS_Y or DE_BIND_AXIS_XY (2D).
 * - GYRO: DE_BIND_AXIS_X, _Y, _Z (rad/s around that pad axis) or DE_BIND_AXIS_XY (2D aim: x =
 *   -gyro.y (turn right), y = gyro.x (tilt up)).
 * A 2D code (stick, XY) binds a whole 2D action without a composite slot, or a button action
 * (by its length). It is refused in a 1D action and in a composite slot. Keys and buttons in a
 * 2D action need a composite slot.
 */
typedef enum de_binding_source {
    DE_BIND_KEY,
    DE_BIND_MOUSE_BUTTON,
    DE_BIND_MOUSE_DELTA,
    DE_BIND_MOUSE_WHEEL,
    DE_BIND_PAD_BUTTON,
    DE_BIND_PAD_AXIS,
    DE_BIND_GYRO,
    DE_BIND_SOURCE_COUNT_
} de_binding_source;

/** de_binding.code values of the MOUSE_DELTA, MOUSE_WHEEL and GYRO sources. */
#define DE_BIND_AXIS_X  0u
#define DE_BIND_AXIS_Y  1u
#define DE_BIND_AXIS_Z  2u
#define DE_BIND_AXIS_XY 3u
/** de_binding.code values of DE_BIND_PAD_AXIS for a whole stick (2D). */
#define DE_BIND_LEFT_STICK  6u
#define DE_BIND_RIGHT_STICK 7u

/** de_binding.composite_slot values. 2D: up, down, left, right. 1D: positive, negative. */
#define DE_COMPOSITE_NONE     (-1)
#define DE_COMPOSITE_UP       0
#define DE_COMPOSITE_DOWN     1
#define DE_COMPOSITE_LEFT     2
#define DE_COMPOSITE_RIGHT    3
#define DE_COMPOSITE_POSITIVE 0
#define DE_COMPOSITE_NEGATIVE 1

/** Response curve shapes (de_response_curve.type). */
typedef enum de_curve_type {
    DE_CURVE_LINEAR, /**< out = in */
    DE_CURVE_POWER,  /**< out = in ^ exponent (exponent > 0) */
    DE_CURVE_CUSTOM  /**< piecewise linear through `points` (x strictly increasing) */
} de_curve_type;
/** Most points of a custom curve; at least 2. */
#define DE_CURVE_MAX_POINTS 8

/**
 * A response curve, applied to a binding's magnitude (|value| in 1D, the length in 2D; the sign
 * or direction is kept). A custom curve is clamped to its first and last point outside them.
 * `(de_response_curve){0}` is linear.
 */
typedef struct de_response_curve {
    de_curve_type type;
    f32 exponent;    /**< DE_CURVE_POWER */
    u32 point_count; /**< DE_CURVE_CUSTOM: 2 .. DE_CURVE_MAX_POINTS */
    de_vec2 points[DE_CURVE_MAX_POINTS];
} de_response_curve;

/**
 * One binding of an action. Modifiers apply in this fixed order: deadzone -> curve -> scale ->
 * invert.
 * - `deadzone` in [0, 1): magnitudes up to it give 0, and the rest is rescaled so the output
 *   starts at 0 and reaches 1 at full deflection: (m - dz) / (1 - dz). Radial (on the length) for
 *   2D codes, axial for 1D values. Pad axes are clamped to magnitude 1 after it.
 * - `curve`: de_response_curve.
 * - `scale`: multiplies the value; 0 means 1.
 * - `invert`: negates a 1D value; negates y of a 2D value ("invert look").
 * `(de_binding){0}` is key code 0 in composite slot 0: set `composite_slot` to DE_COMPOSITE_NONE
 * for a binding that is not part of a composite.
 */
typedef struct de_binding {
    de_binding_source source;
    u32 code;          /* key / button / axis */
    i8 composite_slot; /* -1 none; 0..3 = up/down/left/right for 2D composites */
    f32 scale, deadzone;
    bool invert;
    u32 modifier_key;        /* 0 = none (e.g. Shift+key) */
    de_response_curve curve; /**< DENG-84: linear when zeroed */
} de_binding;

/**
 * de_action_add() parameters. Zero thresholds and times mean the DE_ACTION_DEFAULT_* values.
 * `bindings` (`binding_count` <= DE_ACTION_MAX_BINDINGS) are copied.
 */
typedef struct de_action_desc {
    const char *name;
    de_action_type type;
    de_interaction interaction;
    f32 hold_seconds;
    const de_binding *bindings;
    u32 binding_count;
    bool rebindable;
    f32 tap_seconds;        /**< DENG-84: TAP and DOUBLE_TAP; 0 = 0.2 s */
    f32 double_tap_seconds; /**< DENG-84: DOUBLE_TAP window; 0 = 0.3 s */
    f32 press_threshold;    /**< DENG-84: actuation magnitude; 0 = 0.5 */
} de_action_desc;

/** Creates the map `name` (1..DE_ACTION_NAME_MAX bytes), or returns the existing map of that
 * name. The map starts inactive. Invalid handle (and the error context set) on a bad name or
 * when DE_ACTION_MAX_MAPS are taken. Allocates the action storage on first use. */
DE_API de_action_map de_action_map_create(de_input *in, const char *name);
/**
 * Adds an action to map `m`, copying `desc` and its bindings. An action of the same name in the
 * map is replaced in place and keeps its handle (reloading a file). Invalid handle (and the error
 * context set, DE_ERR_INVALID_ARGUMENT) for a bad desc: an unknown type, interaction or source,
 * a code out of range, a binding that does not fit the action type (see de_binding_source), a
 * deadzone outside [0, 1), a bad curve; or when the pools are full.
 */
DE_API de_action de_action_add(de_input *in, de_action_map m, const de_action_desc *desc);
/**
 * Loads action maps from a TOML file (DENG-2 Q9) and adds them like de_action_map_create /
 * de_action_add (an existing map or action of the same name is updated):
 *
 *     [maps.gameplay.actions.move]
 *     type = "axis2d"                       # "button" (default), "axis1d", "axis2d"
 *     bindings = [ { key = "W", composite = "up" }, { key = "S", composite = "down" },
 *                  { pad_axis = "left_stick", deadzone = 0.2 } ]
 *     [maps.gameplay.actions.jump]
 *     interaction = "press"                 # "release", "hold", "tap", "double_tap"
 *     hold_seconds = 0.5                    # also tap_seconds, double_tap_seconds,
 *     rebindable = true                     #   press_threshold
 *     bindings = [ { key = "space" }, { pad_button = "south" }, { key = "e", modifier = "lshift" }
 * ]
 *
 * A binding has one source key (key, mouse_button, mouse_delta, mouse_wheel, pad_button,
 * pad_axis, gyro) with its name, and optionally composite ("up", "down", "left", "right",
 * "positive", "negative"), modifier (a key name), scale, deadzone, invert, curve ("linear",
 * "power", "custom"), exponent (power; default 2) and points ([[x, y], ...]; custom). Key names
 * are de_key_name() names, ASCII case-insensitive. The whole file is checked before anything is
 * added. Unknown keys log a warning (file and line) and are ignored.
 *
 * Named context sets (DENG-85; de_input_context_define) are tables of `contexts`. `maps` lists
 * maps of the file or maps that exist, bottom to top; `blocks_lower = false` makes the set
 * passthrough (default true):
 *
 *     [contexts.driving]
 *     maps = ["gameplay", "vehicle"]
 *     blocks_lower = true
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT for a bad value (the message names the file, line and
 *         key: "game:/input.toml:12: maps.gameplay.actions.jump.type: ..."); DE_ERR_PARSE for
 *         invalid TOML; or the error of reading the file.
 */
DE_API de_result de_action_maps_load(de_input *in, const char *vpath); /* TOML */
/** Pushes map `m` on top of the context stack (see "Context stacks"): it becomes active, with
 * the highest priority, at the next frame update. Pushing a map that is already on the stack
 * does nothing. A full stack (DE_INPUT_CONTEXT_STACK_MAX) refuses the push: it logs an error and
 * sets the error context (DE_ERR_OUT_OF_MEMORY). */
DE_API void de_action_map_push(de_input *in, de_action_map m); /* context stack */
/** Removes the topmost entry of map `m` from the context stack at the next frame update; its
 * actions then see their inputs released (see "Context stacks"). A map below the top is removed
 * in place (Debug and Dev builds log a warning). A map that is not on the stack: nothing. */
DE_API void de_action_map_pop(de_input *in, de_action_map m);

/** True in the frame (or tick, see above) the action's interaction fires. */
DE_API bool de_action_triggered(const de_input *in, de_action a);
/** True while the action is actuated (magnitude >= press_threshold), whatever its interaction. */
DE_API bool de_action_held(const de_input *in, de_action a);
/** Button: its value (>= 0). 1D: its value. 2D: the length of its value. */
DE_API f32 de_action_value(const de_input *in, de_action a);
/** 2D: its value (+y up). Button and 1D: (value, 0). */
DE_API de_vec2 de_action_value2(const de_input *in, de_action a);

/** The map named `name`, or an invalid handle. */
DE_API de_action_map de_action_map_find(const de_input *in, const char *name);
/** The action named `name` in map `m`, or an invalid handle. */
DE_API de_action de_action_find(const de_input *in, de_action_map m, const char *name);
/** The name of an action or map ("" for an invalid handle). Valid while the input state lives. */
DE_API const char *de_action_name(const de_input *in, de_action a);
DE_API const char *de_action_map_name(const de_input *in, de_action_map m);

/* ---------- Context stacks ---------- */

/*
 * Context stacks (S21, DENG-85). Games switch between input contexts (gameplay, menu, vehicle) by
 * pushing and popping action maps, one at a time (de_action_map_push / pop) or as named sets
 * (de_input_context_push / pop).
 *
 * - Priority. The top of the stack is evaluated first. A map that blocks lower maps (the default)
 *   consumes each raw input that one of its bindings read as active this frame: maps below it
 *   read that input as 0. Inputs it does not bind pass through. A passthrough map
 *   (DE_INPUT_CONTEXT_PASSTHROUGH, for overlays such as a debug or photo mode) consumes nothing.
 *   An input is one (source, code); a whole stick or an XY code overlaps its single axes.
 * - When. Push and pop change the requested stack at once, in call order, and return errors at
 *   once. The next frame update (system "input.actions") applies it, so every system of a frame
 *   sees the same active maps, and the order of changes is deterministic.
 * - Leaving the stack. The actions of a map that leaves see all their inputs released in that
 *   frame update: `held` goes false and values go to 0, with the release edge an input release
 *   gives (a RELEASE or TAP interaction fires). The map stays in evaluation, with every input
 *   consumed, until its actions are idle and their last trigger has reached a fixed tick.
 * - Joining the stack. A binding of a map that joins ignores its input while it is still held
 *   from before (down and not pressed in this frame; a pad axis past the action's
 *   press_threshold), until it is released. The same holds for an input that a higher map
 *   consumed: when that map leaves, the lower map ignores the input until it is released. So
 *   opening or closing a menu with a button never fires the action of that button below.
 * - Mouse motion, the wheel and the gyro are never held over: they count only in the frame they
 *   move.
 * - Duplicates. Pushing a map that is already on the stack does nothing, and so does pushing a
 *   named context whose group is on the stack. A named context pushes all its maps as one group,
 *   also maps that are already on the stack below; such a map counts at its topmost entry.
 *
 * All storage is fixed (DE_INPUT_CONTEXT_STACK_MAX entries, DE_INPUT_MAX_CONTEXTS named sets);
 * nothing here allocates after the first de_action_map_create. Main thread only.
 */

/** Most entries on the context stack (maps; a named context counts each of its maps). */
#define DE_INPUT_CONTEXT_STACK_MAX 16
/** Most named context sets, and most maps in one set. */
#define DE_INPUT_MAX_CONTEXTS     32
#define DE_INPUT_CONTEXT_MAX_MAPS 8
/** Context flags (de_action_map_set_flags, de_input_context_define). 0: the map blocks lower
 * maps (it consumes the inputs it uses). */
#define DE_INPUT_CONTEXT_BLOCKS_LOWER 0u
/** The map consumes nothing: lower maps see every input (overlays: debug, photo mode). */
#define DE_INPUT_CONTEXT_PASSTHROUGH 0x1u

/** Sets the context flags of map `m` (DE_INPUT_CONTEXT_PASSTHROUGH or 0). They apply from the
 * next frame update. A map pushed by a named context is passthrough when the map or the context
 * is. Ignored for an invalid handle. */
DE_API void de_action_map_set_flags(de_input *in, de_action_map m, u32 flags);
/** The context flags of map `m` (0 for an invalid handle). */
DE_API u32 de_action_map_flags(const de_input *in, de_action_map m);
/** True while map `m` is on the applied context stack (it changes at frame updates). */
DE_API bool de_action_map_active(const de_input *in, de_action_map m);
/**
 * Defines the named context `name` (1..DE_ACTION_NAME_MAX bytes; looked up as
 * de_name_from(name)): `count` (1..DE_INPUT_CONTEXT_MAX_MAPS) distinct maps, bottom to top, and
 * `flags` for all of them. Defining a name again replaces its maps and flags; a group of it that
 * is on the stack keeps the maps it pushed.
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT for a bad name, count, handle or a map listed twice;
 *         DE_ERR_OUT_OF_MEMORY when DE_INPUT_MAX_CONTEXTS names are taken.
 */
DE_API de_result de_input_context_define(de_input *in, const char *name, const de_action_map *maps,
                                         u32 count, u32 flags);
/**
 * Pushes the maps of named context `name` on the stack as one group (atomic: all or none).
 * Pushing a context that is on the stack does nothing.
 * @return DE_OK; DE_ERR_NOT_FOUND for an unknown name; DE_ERR_OUT_OF_MEMORY (and an error log)
 *         when the group does not fit in DE_INPUT_CONTEXT_STACK_MAX.
 */
DE_API de_result de_input_context_push(de_input *in, de_name name);
/**
 * Removes the group of named context `name` from the stack (in place when it is not on top;
 * Debug and Dev builds log a warning). A context that is not on the stack: DE_OK, nothing.
 * @return DE_OK or DE_ERR_NOT_FOUND for an unknown name.
 */
DE_API de_result de_input_context_pop(de_input *in, de_name name);

/* ---------- Rebinding ---------- */

/*
 * Runtime rebinding (S21, DENG-86; DENG-2 Q17: remapping is the v1 accessibility feature). Every
 * binding is addressed by (action, binding index), and an action can be rebound only when its
 * desc had `rebindable` set. The API has no UI; S29 builds the rebinding screens on it.
 *
 * Listen mode. de_rebind_begin() starts listening for one binding slot. From the next frame on,
 * each frame's raw events are scanned in arrival order (in the "input.actions" system, after the
 * actions are evaluated, or by de_rebind_poll() when that runs first):
 *   - The first down edge of an allowed source is captured: a key, a mouse button, a pad button.
 *     An input that is already held when listening starts is captured only after it is released
 *     and pressed again.
 *   - Analog axes (pad sticks and triggers) are captured when they move past
 *     DE_REBIND_AXIS_THRESHOLD from rest; an axis that is not at rest when listening starts must
 *     come back first. The sign becomes part of the binding: a negative push gives `invert`
 *     (pushing left on a stick gives "left_x, inverted"), so the pushed direction reads positive.
 *     The mouse wheel is captured after DE_REBIND_AXIS_THRESHOLD notches, the same way.
 *   - The mouse delta (DE_REBIND_MOUSE_DELTA_THRESHOLD points) and the gyro
 *     (DE_REBIND_GYRO_THRESHOLD rad/s) are captured only when `allowed` lists them.
 *   - Chords: a key or mouse button pressed while a modifier key (Ctrl, Shift, Alt, GUI; left or
 *     right) is held is captured with that modifier as `modifier_key` (the lowest key code wins
 *     when several are held). A modifier key pressed and released with no other key in between is
 *     captured on its release, as a plain key.
 *   - Cancel inputs (de_rebind_set_cancel_inputs(); Escape, pad Start and pad Guide by default)
 *     end listening without a binding, whatever `allowed` says.
 *   - A captured input that cannot bind the slot (a key for a stick slot of a 2D action, a
 *     trigger for a 2D slot) is ignored and listening goes on. A 1D axis captured for a whole-2D
 *     slot becomes its 2D code (left_x -> left_stick, mouse x -> xy).
 * The captured binding keeps the slot's composite slot, and its scale, deadzone, curve (and, for
 * 2D codes, invert) when the source is the same as before; otherwise those are zero (defaults).
 *
 * Suppression. While listening, the actions of the slot's map read as idle (like an inactive map:
 * no trigger, not held, value 0), so the input being captured does not also play the game. The
 * input that ends listening (the captured one, or the cancel input) is then swallowed: every
 * action in any map with a binding to it reads as idle until it is released, so the newly bound
 * action does not fire for the press that bound it. Other maps (the menu that hosts the
 * rebinding UI) keep working while listening.
 *
 * Conflicts. A conflict is another rebindable action, of the same map or of a map that shares a
 * named context set with it (de_input_context_define; whether or not the set is on the stack),
 * that has a binding to the same input: same source and code, same modifier key, and for 1D axes
 * the same direction (a whole stick or XY overlaps its axes). Unbound slots never conflict. The
 * slot's own map is searched first, then the other maps in creation order.
 *
 * Defaults. The bindings an action has when rebinding first changes it are its defaults. Resets
 * go back to them, and de_bindings_save() writes only the bindings that differ from them. If the
 * game adds an action again (reloading action maps), de_action_add() replaces its bindings and
 * those become the new defaults; load the user bindings again after that.
 *
 * Main thread only. Listening allocates nothing per frame.
 */

/** |value| an analog axis (pad stick or trigger, wheel notches) must pass to be captured. */
#define DE_REBIND_AXIS_THRESHOLD 0.5f
/** Mouse motion, in points summed since listening started, that captures the mouse delta. */
#define DE_REBIND_MOUSE_DELTA_THRESHOLD 16.0f
/** Gyro rate, rad/s, that captures a gyro axis. */
#define DE_REBIND_GYRO_THRESHOLD 1.0f
/** Most cancel inputs (de_rebind_set_cancel_inputs). */
#define DE_REBIND_MAX_CANCEL_INPUTS 8
/**
 * The key code of an unbound slot: a binding { DE_BIND_KEY, DE_BIND_UNBOUND_KEY } never reads a
 * value (HID usage 1, "ErrorRollOver", is not a key; no platform reports it). DE_REBIND_CLEAR_OTHER
 * and de_rebind_clear() unbind a slot this way, so the binding indices of the action stay put. A
 * whole-stick or xy slot of a 2D action cannot be unbound: it needs a 2D code.
 */
#define DE_BIND_UNBOUND_KEY 1u

/** Where listen mode is (de_rebind_status). */
typedef enum de_rebind_state {
    DE_REBIND_IDLE,      /**< not started, or de_rebind_cancel() */
    DE_REBIND_LISTENING, /**< de_rebind_begin() is waiting for an input */
    DE_REBIND_CAPTURED,  /**< an input was captured (de_rebind_poll returns it once) */
    DE_REBIND_CANCELLED  /**< a cancel input ended listening */
} de_rebind_state;

/** What de_rebind_apply() does with conflicting bindings (see "Conflicts" above). */
typedef enum de_rebind_policy {
    DE_REBIND_ALLOW,       /**< set the binding; conflicting bindings stay */
    DE_REBIND_SWAP,        /**< every conflicting binding gets the slot's previous input */
    DE_REBIND_CLEAR_OTHER, /**< every conflicting binding becomes unbound (DE_BIND_UNBOUND_KEY) */
    DE_REBIND_POLICY_COUNT_
} de_rebind_policy;

/**
 * Starts listening for binding `binding_index` of action `a` (see "Listen mode"); a listen that
 * runs is replaced. `allowed` lists the sources that may be captured (`allowed_count` entries);
 * NULL and 0 allow keys, mouse buttons, the mouse wheel, pad buttons and pad axes (not the mouse
 * delta and the gyro). The actions of the slot's map read as idle from this call on.
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT for an invalid handle, an index out of range, an action
 *         that is not rebindable, or a bad `allowed` list; DE_ERR_OUT_OF_MEMORY when the
 *         rebinding state (about 110 KiB, allocated once) cannot be allocated.
 */
DE_API de_result de_rebind_begin(de_input *in, de_action a, u32 binding_index,
                                 const de_binding_source *allowed, u32 allowed_count);
/**
 * True once, after an input was captured: `*captured` (may be NULL) gets the binding fitted to
 * the slot, and `*conflict` (may be NULL) the first conflicting action in declaration order, or
 * an invalid handle. The binding is not applied: the game chooses with de_rebind_apply(). False
 * while listening, after a cancel, and on every later call.
 */
DE_API bool de_rebind_poll(de_input *in, de_binding *captured, de_action *conflict);
/** Stops listening (no binding, nothing swallowed); the status becomes DE_REBIND_IDLE. */
DE_API void de_rebind_cancel(de_input *in);
/** Where listen mode is. */
DE_API de_rebind_state de_rebind_status(const de_input *in);
/**
 * Sets the inputs that cancel listening: keys, mouse buttons or pad buttons (`source` and `code`
 * of each binding; other fields are ignored). NULL restores the defaults (Escape, pad Start, pad
 * Guide); a non-NULL list with `count` 0 means no input cancels (de_rebind_cancel() still does).
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT for more than DE_REBIND_MAX_CANCEL_INPUTS inputs or an
 *         input of another source; DE_ERR_OUT_OF_MEMORY as de_rebind_begin().
 */
DE_API de_result de_rebind_set_cancel_inputs(de_input *in, const de_binding *inputs, u32 count);
/**
 * Sets binding `binding_index` of rebindable action `a` to `b`, and handles the conflicting
 * bindings (computed with `b`) by `policy`. SWAP gives each conflicting binding the slot's
 * previous input (source, code, modifier, direction), keeping its own composite slot. The whole
 * change is checked first: on an error nothing changes.
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT for an invalid handle or index, an action that is not
 *         rebindable, a binding that does not fit the action, a conflicting slot that cannot take
 *         the previous input (SWAP) or cannot be unbound (CLEAR_OTHER on a whole-stick slot), or
 *         an unknown policy; DE_ERR_OUT_OF_MEMORY as de_rebind_begin().
 */
DE_API de_result de_rebind_apply(de_input *in, de_action a, u32 binding_index, const de_binding *b,
                                 de_rebind_policy policy);
/** Unbinds binding `binding_index` of rebindable action `a` (DE_BIND_UNBOUND_KEY, keeping its
 * composite slot). Errors as de_rebind_apply(); a whole-stick or xy slot cannot be unbound. */
DE_API de_result de_rebind_clear(de_input *in, de_action a, u32 binding_index);
/** True for an unbound slot (DE_BIND_KEY with code DE_BIND_UNBOUND_KEY). */
DE_API bool de_binding_is_unbound(const de_binding *b);

/** Puts the bindings of action `a` back to its defaults. DE_ERR_INVALID_ARGUMENT for an invalid
 * handle. An action that was never rebound is left as it is. */
DE_API de_result de_bindings_reset_action(de_input *in, de_action a);
/** de_bindings_reset_action() for every action of map `m`. */
DE_API de_result de_bindings_reset_map(de_input *in, de_action_map m);
/** de_bindings_reset_action() for every action. */
DE_API void de_bindings_reset_all(de_input *in);

/**
 * Writes the user bindings to `vpath` ("user:/bindings.toml"): only the bindings that differ from
 * the defaults, by map, action and binding index, with the key and button names of action-map
 * files. The file is replaced atomically (de_file_write_all_atomic), so a crash leaves the old or
 * the new file. A file with no override is still written.
 *
 *     version = 1
 *     [maps.gameplay.actions.jump]
 *     bindings = [ { index = 0, key = "f" }, { index = 1, pad_button = "east" } ]
 *     [maps.gameplay.actions.move]
 *     bindings = [ { index = 0, key = "i", composite = "up" }, { index = 3, unbound = true } ]
 *
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (NULL argument, bad path); or the error of writing.
 */
DE_API de_result de_bindings_save(const de_input *in, const char *vpath);
/**
 * Reads user bindings written by de_bindings_save(): every action goes back to its defaults, then
 * the file's bindings are applied. The whole file is checked first. Entries for a map or action
 * that does not exist, an action that is not rebindable, an index past the action's bindings, or
 * a binding that does not fit the action (its type changed) are skipped with a warning. A key
 * that is not known is also skipped with a warning.
 * @return DE_OK; DE_ERR_PARSE for invalid TOML and DE_ERR_INVALID_ARGUMENT for a bad value
 *         ("user:/bindings.toml:4: maps.gameplay.actions.jump.bindings[0].index: ..."), and then
 *         no binding changes; or the error of reading the file (DE_ERR_NOT_FOUND when there is
 *         none yet).
 */
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
