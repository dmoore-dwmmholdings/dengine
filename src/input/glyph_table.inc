/* glyph_table.inc: the prompt glyph IDs of DENG-88, as data. Included once, by glyphs.c; S37
 * (Steam Input) and UI (S29) use the same IDs.
 *
 * The IDs are a stable contract: never rename or remove an ID, only add new ones. Every string in
 * this file is listed in docs/input/glyph-ids.md (test glyphs.ids_documented checks both ways),
 * and the key IDs are "kbm/key_" + de_key_name() (glyphs.ids_documented checks every key).
 *
 * Pad rows are indexed by de_glyph_family (the KBM row is empty: pad bindings never use it). A
 * family without a control uses the "generic/..." ID of that control. Face buttons are positional:
 * DE_PAD_SOUTH is the bottom button, labelled A on Xbox and Steam Deck, cross on PlayStation and
 * B on Nintendo. Paddles: PADDLE1 right upper, PADDLE2 left upper, PADDLE3 right lower, PADDLE4
 * left lower (Xbox Elite P1, P3, P2, P4; Steam Deck R4, L4, R5, L5). */

#define GLYPH_PAD_BUTTONS_GENERIC                                                                  \
    [DE_PAD_SOUTH] = "generic/south", [DE_PAD_EAST] = "generic/east",                              \
    [DE_PAD_WEST] = "generic/west", [DE_PAD_NORTH] = "generic/north",                              \
    [DE_PAD_BACK] = "generic/back", [DE_PAD_GUIDE] = "generic/guide",                              \
    [DE_PAD_START] = "generic/start", [DE_PAD_LSTICK] = "generic/stick_left_press",                \
    [DE_PAD_RSTICK] = "generic/stick_right_press", [DE_PAD_LSHOULDER] = "generic/shoulder_left",   \
    [DE_PAD_RSHOULDER] = "generic/shoulder_right", [DE_PAD_DPAD_UP] = "generic/dpad_up",           \
    [DE_PAD_DPAD_DOWN] = "generic/dpad_down", [DE_PAD_DPAD_LEFT] = "generic/dpad_left",            \
    [DE_PAD_DPAD_RIGHT] = "generic/dpad_right", [DE_PAD_MISC1] = "generic/misc1",                  \
    [DE_PAD_PADDLE1] = "generic/paddle1", [DE_PAD_PADDLE2] = "generic/paddle2",                    \
    [DE_PAD_PADDLE3] = "generic/paddle3", [DE_PAD_PADDLE4] = "generic/paddle4",                    \
    [DE_PAD_TOUCHPAD] = "generic/touchpad"

/* PS4 and PS5 share these; PS5 overrides back, misc1 and touchpad. */
#define GLYPH_PAD_BUTTONS_PS                                                                       \
    [DE_PAD_SOUTH] = "ps/cross", [DE_PAD_EAST] = "ps/circle", [DE_PAD_WEST] = "ps/square",         \
    [DE_PAD_NORTH] = "ps/triangle", [DE_PAD_GUIDE] = "ps/ps", [DE_PAD_START] = "ps/options",       \
    [DE_PAD_LSTICK] = "ps/l3", [DE_PAD_RSTICK] = "ps/r3", [DE_PAD_LSHOULDER] = "ps/l1",            \
    [DE_PAD_RSHOULDER] = "ps/r1", [DE_PAD_DPAD_UP] = "ps/dpad_up",                                 \
    [DE_PAD_DPAD_DOWN] = "ps/dpad_down", [DE_PAD_DPAD_LEFT] = "ps/dpad_left",                      \
    [DE_PAD_DPAD_RIGHT] = "ps/dpad_right", [DE_PAD_PADDLE1] = "generic/paddle1",                   \
    [DE_PAD_PADDLE2] = "generic/paddle2", [DE_PAD_PADDLE3] = "generic/paddle3",                    \
    [DE_PAD_PADDLE4] = "generic/paddle4"

static const char *const k_glyph_pad_button[DE_GLYPH_FAMILY_COUNT_][DE_PAD_BUTTON_COUNT_] = {
    [DE_GLYPH_FAMILY_XBOX] =
        {
            [DE_PAD_SOUTH] = "xbox/a",
            [DE_PAD_EAST] = "xbox/b",
            [DE_PAD_WEST] = "xbox/x",
            [DE_PAD_NORTH] = "xbox/y",
            [DE_PAD_BACK] = "xbox/view",
            [DE_PAD_GUIDE] = "xbox/xbox",
            [DE_PAD_START] = "xbox/menu",
            [DE_PAD_LSTICK] = "xbox/ls",
            [DE_PAD_RSTICK] = "xbox/rs",
            [DE_PAD_LSHOULDER] = "xbox/lb",
            [DE_PAD_RSHOULDER] = "xbox/rb",
            [DE_PAD_DPAD_UP] = "xbox/dpad_up",
            [DE_PAD_DPAD_DOWN] = "xbox/dpad_down",
            [DE_PAD_DPAD_LEFT] = "xbox/dpad_left",
            [DE_PAD_DPAD_RIGHT] = "xbox/dpad_right",
            [DE_PAD_MISC1] = "xbox/share",
            [DE_PAD_PADDLE1] = "xbox/p1",
            [DE_PAD_PADDLE2] = "xbox/p3",
            [DE_PAD_PADDLE3] = "xbox/p2",
            [DE_PAD_PADDLE4] = "xbox/p4",
            [DE_PAD_TOUCHPAD] = "generic/touchpad",
        },
    [DE_GLYPH_FAMILY_PS] =
        {
            GLYPH_PAD_BUTTONS_PS,
            [DE_PAD_BACK] = "ps/share",
            [DE_PAD_MISC1] = "generic/misc1",
            [DE_PAD_TOUCHPAD] = "ps/touchpad",
        },
    [DE_GLYPH_FAMILY_PS5] =
        {
            GLYPH_PAD_BUTTONS_PS,
            [DE_PAD_BACK] = "ps5/create",
            [DE_PAD_MISC1] = "ps5/mute",
            [DE_PAD_TOUCHPAD] = "ps5/touchpad",
        },
    [DE_GLYPH_FAMILY_SWITCH] =
        {
            [DE_PAD_SOUTH] = "switch/b",
            [DE_PAD_EAST] = "switch/a",
            [DE_PAD_WEST] = "switch/y",
            [DE_PAD_NORTH] = "switch/x",
            [DE_PAD_BACK] = "switch/minus",
            [DE_PAD_GUIDE] = "switch/home",
            [DE_PAD_START] = "switch/plus",
            [DE_PAD_LSTICK] = "switch/ls",
            [DE_PAD_RSTICK] = "switch/rs",
            [DE_PAD_LSHOULDER] = "switch/l",
            [DE_PAD_RSHOULDER] = "switch/r",
            [DE_PAD_DPAD_UP] = "switch/dpad_up",
            [DE_PAD_DPAD_DOWN] = "switch/dpad_down",
            [DE_PAD_DPAD_LEFT] = "switch/dpad_left",
            [DE_PAD_DPAD_RIGHT] = "switch/dpad_right",
            [DE_PAD_MISC1] = "switch/capture",
            [DE_PAD_PADDLE1] = "generic/paddle1",
            [DE_PAD_PADDLE2] = "generic/paddle2",
            [DE_PAD_PADDLE3] = "generic/paddle3",
            [DE_PAD_PADDLE4] = "generic/paddle4",
            [DE_PAD_TOUCHPAD] = "generic/touchpad",
        },
    [DE_GLYPH_FAMILY_DECK] =
        {
            [DE_PAD_SOUTH] = "deck/a",
            [DE_PAD_EAST] = "deck/b",
            [DE_PAD_WEST] = "deck/x",
            [DE_PAD_NORTH] = "deck/y",
            [DE_PAD_BACK] = "deck/view",
            [DE_PAD_GUIDE] = "deck/steam",
            [DE_PAD_START] = "deck/menu",
            [DE_PAD_LSTICK] = "deck/l3",
            [DE_PAD_RSTICK] = "deck/r3",
            [DE_PAD_LSHOULDER] = "deck/l1",
            [DE_PAD_RSHOULDER] = "deck/r1",
            [DE_PAD_DPAD_UP] = "deck/dpad_up",
            [DE_PAD_DPAD_DOWN] = "deck/dpad_down",
            [DE_PAD_DPAD_LEFT] = "deck/dpad_left",
            [DE_PAD_DPAD_RIGHT] = "deck/dpad_right",
            [DE_PAD_MISC1] = "deck/quick_access",
            [DE_PAD_PADDLE1] = "deck/r4",
            [DE_PAD_PADDLE2] = "deck/l4",
            [DE_PAD_PADDLE3] = "deck/r5",
            [DE_PAD_PADDLE4] = "deck/l5",
            [DE_PAD_TOUCHPAD] = "generic/touchpad",
        },
    [DE_GLYPH_FAMILY_GENERIC] = {GLYPH_PAD_BUTTONS_GENERIC},
};

/* Pad axis codes (de_pad_axis 0..5, then DE_BIND_LEFT_STICK and DE_BIND_RIGHT_STICK). */
#define GLYPH_PAD_AXIS_CODES (DE_BIND_RIGHT_STICK + 1u)
#define GLYPH_STICK_AXES(fam)                                                                      \
    [DE_AXIS_LEFT_X] = fam "/stick_left_x", [DE_AXIS_LEFT_Y] = fam "/stick_left_y",                \
    [DE_AXIS_RIGHT_X] = fam "/stick_right_x", [DE_AXIS_RIGHT_Y] = fam "/stick_right_y",            \
    [DE_BIND_LEFT_STICK] = fam "/stick_left", [DE_BIND_RIGHT_STICK] = fam "/stick_right"

static const char *const k_glyph_pad_axis[DE_GLYPH_FAMILY_COUNT_][GLYPH_PAD_AXIS_CODES] = {
    [DE_GLYPH_FAMILY_XBOX] = {GLYPH_STICK_AXES("xbox"), [DE_AXIS_LEFT_TRIGGER] = "xbox/lt",
                              [DE_AXIS_RIGHT_TRIGGER] = "xbox/rt"},
    [DE_GLYPH_FAMILY_PS] = {GLYPH_STICK_AXES("ps"), [DE_AXIS_LEFT_TRIGGER] = "ps/l2",
                            [DE_AXIS_RIGHT_TRIGGER] = "ps/r2"},
    [DE_GLYPH_FAMILY_PS5] = {GLYPH_STICK_AXES("ps"), [DE_AXIS_LEFT_TRIGGER] = "ps/l2",
                             [DE_AXIS_RIGHT_TRIGGER] = "ps/r2"},
    [DE_GLYPH_FAMILY_SWITCH] = {GLYPH_STICK_AXES("switch"), [DE_AXIS_LEFT_TRIGGER] = "switch/zl",
                                [DE_AXIS_RIGHT_TRIGGER] = "switch/zr"},
    [DE_GLYPH_FAMILY_DECK] = {GLYPH_STICK_AXES("deck"), [DE_AXIS_LEFT_TRIGGER] = "deck/l2",
                              [DE_AXIS_RIGHT_TRIGGER] = "deck/r2"},
    [DE_GLYPH_FAMILY_GENERIC] = {GLYPH_STICK_AXES("generic"),
                                 [DE_AXIS_LEFT_TRIGGER] = "generic/trigger_left",
                                 [DE_AXIS_RIGHT_TRIGGER] = "generic/trigger_right"},
};

/* Stick directions, for a stick axis in a composite slot: [family][stick: left, right][direction:
 * up, down, left, right]. */
#define GLYPH_STICK_DIRS(fam)                                                                      \
    {                                                                                              \
        {fam "/stick_left_up", fam "/stick_left_down", fam "/stick_left_left",                     \
         fam "/stick_left_right"},                                                                 \
        {fam "/stick_right_up", fam "/stick_right_down", fam "/stick_right_left",                  \
         fam "/stick_right_right"},                                                                \
    }

static const char *const k_glyph_stick_dir[DE_GLYPH_FAMILY_COUNT_][2][4] = {
    [DE_GLYPH_FAMILY_XBOX] = GLYPH_STICK_DIRS("xbox"),
    [DE_GLYPH_FAMILY_PS] = GLYPH_STICK_DIRS("ps"),
    [DE_GLYPH_FAMILY_PS5] = GLYPH_STICK_DIRS("ps"),
    [DE_GLYPH_FAMILY_SWITCH] = GLYPH_STICK_DIRS("switch"),
    [DE_GLYPH_FAMILY_DECK] = GLYPH_STICK_DIRS("deck"),
    [DE_GLYPH_FAMILY_GENERIC] = GLYPH_STICK_DIRS("generic"),
};

/* The whole D-pad, for a 2D composite of the four D-pad buttons. */
static const char *const k_glyph_dpad[DE_GLYPH_FAMILY_COUNT_] = {
    [DE_GLYPH_FAMILY_XBOX] = "xbox/dpad", [DE_GLYPH_FAMILY_PS] = "ps/dpad",
    [DE_GLYPH_FAMILY_PS5] = "ps/dpad",    [DE_GLYPH_FAMILY_SWITCH] = "switch/dpad",
    [DE_GLYPH_FAMILY_DECK] = "deck/dpad", [DE_GLYPH_FAMILY_GENERIC] = "generic/dpad",
};

/* Gyro (family-neutral): codes DE_BIND_AXIS_X, _Y, _Z, _XY. */
static const char *const k_glyph_gyro[DE_BIND_AXIS_XY + 1u] = {
    [DE_BIND_AXIS_X] = "generic/gyro_x",
    [DE_BIND_AXIS_Y] = "generic/gyro_y",
    [DE_BIND_AXIS_Z] = "generic/gyro_z",
    [DE_BIND_AXIS_XY] = "generic/gyro",
};

/* Mouse. */
static const char *const k_glyph_mouse_button[DE_MOUSE_BUTTON_COUNT_] = {
    [DE_MOUSE_LEFT] = "kbm/mouse_left",     [DE_MOUSE_RIGHT] = "kbm/mouse_right",
    [DE_MOUSE_MIDDLE] = "kbm/mouse_middle", [DE_MOUSE_X1] = "kbm/mouse_x1",
    [DE_MOUSE_X2] = "kbm/mouse_x2",
};
static const char *const k_glyph_mouse_move[DE_BIND_AXIS_XY + 1u] = {
    [DE_BIND_AXIS_X] = "kbm/mouse_move_x",
    [DE_BIND_AXIS_Y] = "kbm/mouse_move_y",
    [DE_BIND_AXIS_XY] = "kbm/mouse_move",
};
static const char *const k_glyph_mouse_wheel[DE_BIND_AXIS_XY + 1u] = {
    [DE_BIND_AXIS_X] = "kbm/mouse_wheel_x",
    [DE_BIND_AXIS_Y] = "kbm/mouse_wheel",
    [DE_BIND_AXIS_XY] = "kbm/mouse_wheel",
};
/* The wheel's y in a composite slot: up (away from the user), down. */
static const char *const k_glyph_wheel_dir[2] = {"kbm/mouse_wheel_up", "kbm/mouse_wheel_down"};

/* Keyboard composites (2D, all four slots). */
#define GLYPH_ID_WASD   "kbm/wasd"
#define GLYPH_ID_ARROWS "kbm/arrows"
/* A key code without a de_key_name(). */
#define GLYPH_ID_KEY_UNKNOWN "kbm/key_unknown"

/* Keys: "kbm/key_" + de_key_name(k), generated from keys.c (glyphs.ids_documented checks it). */
static const char *const k_glyph_key[DE_KEY_COUNT_] = {
    [DE_KEY_A] = "kbm/key_a",
    [DE_KEY_B] = "kbm/key_b",
    [DE_KEY_C] = "kbm/key_c",
    [DE_KEY_D] = "kbm/key_d",
    [DE_KEY_E] = "kbm/key_e",
    [DE_KEY_F] = "kbm/key_f",
    [DE_KEY_G] = "kbm/key_g",
    [DE_KEY_H] = "kbm/key_h",
    [DE_KEY_I] = "kbm/key_i",
    [DE_KEY_J] = "kbm/key_j",
    [DE_KEY_K] = "kbm/key_k",
    [DE_KEY_L] = "kbm/key_l",
    [DE_KEY_M] = "kbm/key_m",
    [DE_KEY_N] = "kbm/key_n",
    [DE_KEY_O] = "kbm/key_o",
    [DE_KEY_P] = "kbm/key_p",
    [DE_KEY_Q] = "kbm/key_q",
    [DE_KEY_R] = "kbm/key_r",
    [DE_KEY_S] = "kbm/key_s",
    [DE_KEY_T] = "kbm/key_t",
    [DE_KEY_U] = "kbm/key_u",
    [DE_KEY_V] = "kbm/key_v",
    [DE_KEY_W] = "kbm/key_w",
    [DE_KEY_X] = "kbm/key_x",
    [DE_KEY_Y] = "kbm/key_y",
    [DE_KEY_Z] = "kbm/key_z",
    [DE_KEY_1] = "kbm/key_1",
    [DE_KEY_2] = "kbm/key_2",
    [DE_KEY_3] = "kbm/key_3",
    [DE_KEY_4] = "kbm/key_4",
    [DE_KEY_5] = "kbm/key_5",
    [DE_KEY_6] = "kbm/key_6",
    [DE_KEY_7] = "kbm/key_7",
    [DE_KEY_8] = "kbm/key_8",
    [DE_KEY_9] = "kbm/key_9",
    [DE_KEY_0] = "kbm/key_0",
    [DE_KEY_RETURN] = "kbm/key_return",
    [DE_KEY_ESCAPE] = "kbm/key_escape",
    [DE_KEY_BACKSPACE] = "kbm/key_backspace",
    [DE_KEY_TAB] = "kbm/key_tab",
    [DE_KEY_SPACE] = "kbm/key_space",
    [DE_KEY_MINUS] = "kbm/key_minus",
    [DE_KEY_EQUALS] = "kbm/key_equals",
    [DE_KEY_LEFTBRACKET] = "kbm/key_leftbracket",
    [DE_KEY_RIGHTBRACKET] = "kbm/key_rightbracket",
    [DE_KEY_BACKSLASH] = "kbm/key_backslash",
    [DE_KEY_NONUSHASH] = "kbm/key_nonushash",
    [DE_KEY_SEMICOLON] = "kbm/key_semicolon",
    [DE_KEY_APOSTROPHE] = "kbm/key_apostrophe",
    [DE_KEY_GRAVE] = "kbm/key_grave",
    [DE_KEY_COMMA] = "kbm/key_comma",
    [DE_KEY_PERIOD] = "kbm/key_period",
    [DE_KEY_SLASH] = "kbm/key_slash",
    [DE_KEY_CAPSLOCK] = "kbm/key_capslock",
    [DE_KEY_F1] = "kbm/key_f1",
    [DE_KEY_F2] = "kbm/key_f2",
    [DE_KEY_F3] = "kbm/key_f3",
    [DE_KEY_F4] = "kbm/key_f4",
    [DE_KEY_F5] = "kbm/key_f5",
    [DE_KEY_F6] = "kbm/key_f6",
    [DE_KEY_F7] = "kbm/key_f7",
    [DE_KEY_F8] = "kbm/key_f8",
    [DE_KEY_F9] = "kbm/key_f9",
    [DE_KEY_F10] = "kbm/key_f10",
    [DE_KEY_F11] = "kbm/key_f11",
    [DE_KEY_F12] = "kbm/key_f12",
    [DE_KEY_PRINTSCREEN] = "kbm/key_printscreen",
    [DE_KEY_SCROLLLOCK] = "kbm/key_scrolllock",
    [DE_KEY_PAUSE] = "kbm/key_pause",
    [DE_KEY_INSERT] = "kbm/key_insert",
    [DE_KEY_HOME] = "kbm/key_home",
    [DE_KEY_PAGEUP] = "kbm/key_pageup",
    [DE_KEY_DELETE] = "kbm/key_delete",
    [DE_KEY_END] = "kbm/key_end",
    [DE_KEY_PAGEDOWN] = "kbm/key_pagedown",
    [DE_KEY_RIGHT] = "kbm/key_right",
    [DE_KEY_LEFT] = "kbm/key_left",
    [DE_KEY_DOWN] = "kbm/key_down",
    [DE_KEY_UP] = "kbm/key_up",
    [DE_KEY_NUMLOCKCLEAR] = "kbm/key_numlockclear",
    [DE_KEY_KP_DIVIDE] = "kbm/key_kp_divide",
    [DE_KEY_KP_MULTIPLY] = "kbm/key_kp_multiply",
    [DE_KEY_KP_MINUS] = "kbm/key_kp_minus",
    [DE_KEY_KP_PLUS] = "kbm/key_kp_plus",
    [DE_KEY_KP_ENTER] = "kbm/key_kp_enter",
    [DE_KEY_KP_1] = "kbm/key_kp_1",
    [DE_KEY_KP_2] = "kbm/key_kp_2",
    [DE_KEY_KP_3] = "kbm/key_kp_3",
    [DE_KEY_KP_4] = "kbm/key_kp_4",
    [DE_KEY_KP_5] = "kbm/key_kp_5",
    [DE_KEY_KP_6] = "kbm/key_kp_6",
    [DE_KEY_KP_7] = "kbm/key_kp_7",
    [DE_KEY_KP_8] = "kbm/key_kp_8",
    [DE_KEY_KP_9] = "kbm/key_kp_9",
    [DE_KEY_KP_0] = "kbm/key_kp_0",
    [DE_KEY_KP_PERIOD] = "kbm/key_kp_period",
    [DE_KEY_NONUSBACKSLASH] = "kbm/key_nonusbackslash",
    [DE_KEY_APPLICATION] = "kbm/key_application",
    [DE_KEY_POWER] = "kbm/key_power",
    [DE_KEY_KP_EQUALS] = "kbm/key_kp_equals",
    [DE_KEY_F13] = "kbm/key_f13",
    [DE_KEY_F14] = "kbm/key_f14",
    [DE_KEY_F15] = "kbm/key_f15",
    [DE_KEY_F16] = "kbm/key_f16",
    [DE_KEY_F17] = "kbm/key_f17",
    [DE_KEY_F18] = "kbm/key_f18",
    [DE_KEY_F19] = "kbm/key_f19",
    [DE_KEY_F20] = "kbm/key_f20",
    [DE_KEY_F21] = "kbm/key_f21",
    [DE_KEY_F22] = "kbm/key_f22",
    [DE_KEY_F23] = "kbm/key_f23",
    [DE_KEY_F24] = "kbm/key_f24",
    [DE_KEY_EXECUTE] = "kbm/key_execute",
    [DE_KEY_HELP] = "kbm/key_help",
    [DE_KEY_MENU] = "kbm/key_menu",
    [DE_KEY_SELECT] = "kbm/key_select",
    [DE_KEY_STOP] = "kbm/key_stop",
    [DE_KEY_AGAIN] = "kbm/key_again",
    [DE_KEY_UNDO] = "kbm/key_undo",
    [DE_KEY_CUT] = "kbm/key_cut",
    [DE_KEY_COPY] = "kbm/key_copy",
    [DE_KEY_PASTE] = "kbm/key_paste",
    [DE_KEY_FIND] = "kbm/key_find",
    [DE_KEY_MUTE] = "kbm/key_mute",
    [DE_KEY_VOLUMEUP] = "kbm/key_volumeup",
    [DE_KEY_VOLUMEDOWN] = "kbm/key_volumedown",
    [DE_KEY_KP_COMMA] = "kbm/key_kp_comma",
    [DE_KEY_KP_EQUALSAS400] = "kbm/key_kp_equalsas400",
    [DE_KEY_INTERNATIONAL1] = "kbm/key_international1",
    [DE_KEY_INTERNATIONAL2] = "kbm/key_international2",
    [DE_KEY_INTERNATIONAL3] = "kbm/key_international3",
    [DE_KEY_INTERNATIONAL4] = "kbm/key_international4",
    [DE_KEY_INTERNATIONAL5] = "kbm/key_international5",
    [DE_KEY_INTERNATIONAL6] = "kbm/key_international6",
    [DE_KEY_INTERNATIONAL7] = "kbm/key_international7",
    [DE_KEY_INTERNATIONAL8] = "kbm/key_international8",
    [DE_KEY_INTERNATIONAL9] = "kbm/key_international9",
    [DE_KEY_LANG1] = "kbm/key_lang1",
    [DE_KEY_LANG2] = "kbm/key_lang2",
    [DE_KEY_LANG3] = "kbm/key_lang3",
    [DE_KEY_LANG4] = "kbm/key_lang4",
    [DE_KEY_LANG5] = "kbm/key_lang5",
    [DE_KEY_LANG6] = "kbm/key_lang6",
    [DE_KEY_LANG7] = "kbm/key_lang7",
    [DE_KEY_LANG8] = "kbm/key_lang8",
    [DE_KEY_LANG9] = "kbm/key_lang9",
    [DE_KEY_ALTERASE] = "kbm/key_alterase",
    [DE_KEY_SYSREQ] = "kbm/key_sysreq",
    [DE_KEY_CANCEL] = "kbm/key_cancel",
    [DE_KEY_CLEAR] = "kbm/key_clear",
    [DE_KEY_PRIOR] = "kbm/key_prior",
    [DE_KEY_RETURN2] = "kbm/key_return2",
    [DE_KEY_SEPARATOR] = "kbm/key_separator",
    [DE_KEY_OUT] = "kbm/key_out",
    [DE_KEY_OPER] = "kbm/key_oper",
    [DE_KEY_CLEARAGAIN] = "kbm/key_clearagain",
    [DE_KEY_CRSEL] = "kbm/key_crsel",
    [DE_KEY_EXSEL] = "kbm/key_exsel",
    [DE_KEY_FRONT] = "kbm/key_front",
    [DE_KEY_KP_00] = "kbm/key_kp_00",
    [DE_KEY_KP_000] = "kbm/key_kp_000",
    [DE_KEY_THOUSANDSSEPARATOR] = "kbm/key_thousandsseparator",
    [DE_KEY_DECIMALSEPARATOR] = "kbm/key_decimalseparator",
    [DE_KEY_CURRENCYUNIT] = "kbm/key_currencyunit",
    [DE_KEY_CURRENCYSUBUNIT] = "kbm/key_currencysubunit",
    [DE_KEY_KP_LEFTPAREN] = "kbm/key_kp_leftparen",
    [DE_KEY_KP_RIGHTPAREN] = "kbm/key_kp_rightparen",
    [DE_KEY_KP_LEFTBRACE] = "kbm/key_kp_leftbrace",
    [DE_KEY_KP_RIGHTBRACE] = "kbm/key_kp_rightbrace",
    [DE_KEY_KP_TAB] = "kbm/key_kp_tab",
    [DE_KEY_KP_BACKSPACE] = "kbm/key_kp_backspace",
    [DE_KEY_KP_A] = "kbm/key_kp_a",
    [DE_KEY_KP_B] = "kbm/key_kp_b",
    [DE_KEY_KP_C] = "kbm/key_kp_c",
    [DE_KEY_KP_D] = "kbm/key_kp_d",
    [DE_KEY_KP_E] = "kbm/key_kp_e",
    [DE_KEY_KP_F] = "kbm/key_kp_f",
    [DE_KEY_KP_XOR] = "kbm/key_kp_xor",
    [DE_KEY_KP_POWER] = "kbm/key_kp_power",
    [DE_KEY_KP_PERCENT] = "kbm/key_kp_percent",
    [DE_KEY_KP_LESS] = "kbm/key_kp_less",
    [DE_KEY_KP_GREATER] = "kbm/key_kp_greater",
    [DE_KEY_KP_AMPERSAND] = "kbm/key_kp_ampersand",
    [DE_KEY_KP_DBLAMPERSAND] = "kbm/key_kp_dblampersand",
    [DE_KEY_KP_VERTICALBAR] = "kbm/key_kp_verticalbar",
    [DE_KEY_KP_DBLVERTICALBAR] = "kbm/key_kp_dblverticalbar",
    [DE_KEY_KP_COLON] = "kbm/key_kp_colon",
    [DE_KEY_KP_HASH] = "kbm/key_kp_hash",
    [DE_KEY_KP_SPACE] = "kbm/key_kp_space",
    [DE_KEY_KP_AT] = "kbm/key_kp_at",
    [DE_KEY_KP_EXCLAM] = "kbm/key_kp_exclam",
    [DE_KEY_KP_MEMSTORE] = "kbm/key_kp_memstore",
    [DE_KEY_KP_MEMRECALL] = "kbm/key_kp_memrecall",
    [DE_KEY_KP_MEMCLEAR] = "kbm/key_kp_memclear",
    [DE_KEY_KP_MEMADD] = "kbm/key_kp_memadd",
    [DE_KEY_KP_MEMSUBTRACT] = "kbm/key_kp_memsubtract",
    [DE_KEY_KP_MEMMULTIPLY] = "kbm/key_kp_memmultiply",
    [DE_KEY_KP_MEMDIVIDE] = "kbm/key_kp_memdivide",
    [DE_KEY_KP_PLUSMINUS] = "kbm/key_kp_plusminus",
    [DE_KEY_KP_CLEAR] = "kbm/key_kp_clear",
    [DE_KEY_KP_CLEARENTRY] = "kbm/key_kp_clearentry",
    [DE_KEY_KP_BINARY] = "kbm/key_kp_binary",
    [DE_KEY_KP_OCTAL] = "kbm/key_kp_octal",
    [DE_KEY_KP_DECIMAL] = "kbm/key_kp_decimal",
    [DE_KEY_KP_HEXADECIMAL] = "kbm/key_kp_hexadecimal",
    [DE_KEY_LCTRL] = "kbm/key_lctrl",
    [DE_KEY_LSHIFT] = "kbm/key_lshift",
    [DE_KEY_LALT] = "kbm/key_lalt",
    [DE_KEY_LGUI] = "kbm/key_lgui",
    [DE_KEY_RCTRL] = "kbm/key_rctrl",
    [DE_KEY_RSHIFT] = "kbm/key_rshift",
    [DE_KEY_RALT] = "kbm/key_ralt",
    [DE_KEY_RGUI] = "kbm/key_rgui",
    [DE_KEY_MODE] = "kbm/key_mode",
    [DE_KEY_SLEEP] = "kbm/key_sleep",
    [DE_KEY_WAKE] = "kbm/key_wake",
    [DE_KEY_CHANNEL_INCREMENT] = "kbm/key_channel_increment",
    [DE_KEY_CHANNEL_DECREMENT] = "kbm/key_channel_decrement",
    [DE_KEY_MEDIA_PLAY] = "kbm/key_media_play",
    [DE_KEY_MEDIA_PAUSE] = "kbm/key_media_pause",
    [DE_KEY_MEDIA_RECORD] = "kbm/key_media_record",
    [DE_KEY_MEDIA_FAST_FORWARD] = "kbm/key_media_fast_forward",
    [DE_KEY_MEDIA_REWIND] = "kbm/key_media_rewind",
    [DE_KEY_MEDIA_NEXT_TRACK] = "kbm/key_media_next_track",
    [DE_KEY_MEDIA_PREVIOUS_TRACK] = "kbm/key_media_previous_track",
    [DE_KEY_MEDIA_STOP] = "kbm/key_media_stop",
    [DE_KEY_MEDIA_EJECT] = "kbm/key_media_eject",
    [DE_KEY_MEDIA_PLAY_PAUSE] = "kbm/key_media_play_pause",
    [DE_KEY_MEDIA_SELECT] = "kbm/key_media_select",
    [DE_KEY_AC_NEW] = "kbm/key_ac_new",
    [DE_KEY_AC_OPEN] = "kbm/key_ac_open",
    [DE_KEY_AC_CLOSE] = "kbm/key_ac_close",
    [DE_KEY_AC_EXIT] = "kbm/key_ac_exit",
    [DE_KEY_AC_SAVE] = "kbm/key_ac_save",
    [DE_KEY_AC_PRINT] = "kbm/key_ac_print",
    [DE_KEY_AC_PROPERTIES] = "kbm/key_ac_properties",
    [DE_KEY_AC_SEARCH] = "kbm/key_ac_search",
    [DE_KEY_AC_HOME] = "kbm/key_ac_home",
    [DE_KEY_AC_BACK] = "kbm/key_ac_back",
    [DE_KEY_AC_FORWARD] = "kbm/key_ac_forward",
    [DE_KEY_AC_STOP] = "kbm/key_ac_stop",
    [DE_KEY_AC_REFRESH] = "kbm/key_ac_refresh",
    [DE_KEY_AC_BOOKMARKS] = "kbm/key_ac_bookmarks",
    [DE_KEY_SOFTLEFT] = "kbm/key_softleft",
    [DE_KEY_SOFTRIGHT] = "kbm/key_softright",
    [DE_KEY_CALL] = "kbm/key_call",
    [DE_KEY_ENDCALL] = "kbm/key_endcall",
};
