# Prompt glyph IDs

A UI prompt ("Press [A] to jump") shows the glyph of the input that an action is bound to, on
the device the player uses now. The input module gives the glyph as a **glyph ID**: a static
string that UI (S29) maps to an image. This file lists every glyph ID. The API is the "Prompt
glyphs" section of `include/dengine/de_input.h`; the IDs are data in `src/input/glyph_table.inc`.

The test `glyphs.ids_documented` (`tests/unit/input/test_glyphs.c`) reads this file. It checks
that every ID in the table is in this file, and that every ID in this file is in the table. An ID
counts when it is written in backticks and has the form `<family>/<control>`.

## Rules

- An ID is `<family>/<control>`, in lower case: letters, digits and `_`.
- **IDs are a stable contract.** Never rename or remove an ID after it is merged. Only add new
  IDs. Add a new ID to `glyph_table.inc` and to this file in the same change.
- Face buttons are **positional**. `DE_PAD_SOUTH` is the bottom button: `xbox/a`, `ps/cross`,
  `switch/b` (Nintendo prints B at the south position) and `deck/a`.
- Key IDs name the key **position** (`de_key`, a USB HID usage), not the character that the
  user's layout prints on it. The ID is `kbm/key_<name>`, where `<name>` is `de_key_name()`:
  the same name that action-map files and saved bindings use. `de_input_key_label()` gives the
  text for the user's layout (a callback set with `de_input_set_key_label_fn()`; localized
  names are S28).
- When a family does not have a control, the ID is the `generic/...` ID of that control. Gyro
  IDs are always `generic/...`.

## Families

The family follows `de_input_last_device()`. Keyboard and mouse give `kbm`. A pad gives the
family of its `de_pad_type`. Before any input, and after the last used pad disconnects, the
family is that of the first connected pad, or `kbm` when no pad is connected.
`de_input_glyph_family_changed()` is true in the one frame in which the family changes.

| Family (`de_glyph_family`) | Prefix | Pads (`de_pad_type`) |
|---|---|---|
| `DE_GLYPH_FAMILY_KBM` | kbm | keyboard and mouse |
| `DE_GLYPH_FAMILY_XBOX` | xbox | `DE_PAD_TYPE_XBOX` |
| `DE_GLYPH_FAMILY_PS` | ps | `DE_PAD_TYPE_PS4` |
| `DE_GLYPH_FAMILY_PS5` | ps, ps5 | `DE_PAD_TYPE_PS5`: the `ps/...` IDs, except `ps5/create`, `ps5/mute` and `ps5/touchpad` |
| `DE_GLYPH_FAMILY_SWITCH` | switch | `DE_PAD_TYPE_SWITCH` (Switch Pro controller, Joy-Cons) |
| `DE_GLYPH_FAMILY_DECK` | deck | `DE_PAD_TYPE_STEAM` (Steam Deck, Steam Controller) |
| `DE_GLYPH_FAMILY_GENERIC` | generic | `DE_PAD_TYPE_GENERIC`, and family-neutral IDs |

## Lookup

`de_action_glyph_id(in, action)` returns the ID for the current family.
`de_action_glyph_id_family()` takes the family as an argument. The order is:

1. The glyph provider (`de_input_set_glyph_provider()`, for S37 Steam Input), when it returns
   a string that is not NULL.
2. The first binding of the action, in binding order, that belongs to the device class of the
   family. Keyboard, mouse button, mouse motion and wheel bindings are the `kbm` class. Pad
   button, pad axis and gyro bindings are the pad class.
3. The first binding of the other class. For `kbm`, pad IDs then use the family of the first
   connected pad, or `generic`.
4. `generic/unbound`.

A binding in a 2D composite gives the ID of the whole composite when its four slots form a known
set: W/S/A/D give `kbm/wasd`, the arrow keys give `kbm/arrows`, and the four D-pad buttons give
`<family>/dpad`. Otherwise the binding gives its own ID. A stick axis or the wheel in a composite
slot gives its direction (`xbox/stick_left_up`, `kbm/mouse_wheel_down`). The modifier key of a
chord is not part of the ID.

## Pad buttons

`ps` is `DE_GLYPH_FAMILY_PS` (DualShock 4) and `ps5` is `DE_GLYPH_FAMILY_PS5` (DualSense). The
paddles are, in order, right upper, left upper, right lower and left lower: Xbox Elite P1, P3, P2
and P4, Steam Deck R4, L4, R5 and L5.

| Button | xbox | ps | ps5 | switch | deck | generic |
|---|---|---|---|---|---|---|
| `DE_PAD_SOUTH` | `xbox/a` | `ps/cross` | `ps/cross` | `switch/b` | `deck/a` | `generic/south` |
| `DE_PAD_EAST` | `xbox/b` | `ps/circle` | `ps/circle` | `switch/a` | `deck/b` | `generic/east` |
| `DE_PAD_WEST` | `xbox/x` | `ps/square` | `ps/square` | `switch/y` | `deck/x` | `generic/west` |
| `DE_PAD_NORTH` | `xbox/y` | `ps/triangle` | `ps/triangle` | `switch/x` | `deck/y` | `generic/north` |
| `DE_PAD_BACK` | `xbox/view` | `ps/share` | `ps5/create` | `switch/minus` | `deck/view` | `generic/back` |
| `DE_PAD_GUIDE` | `xbox/xbox` | `ps/ps` | `ps/ps` | `switch/home` | `deck/steam` | `generic/guide` |
| `DE_PAD_START` | `xbox/menu` | `ps/options` | `ps/options` | `switch/plus` | `deck/menu` | `generic/start` |
| `DE_PAD_LSTICK` | `xbox/ls` | `ps/l3` | `ps/l3` | `switch/ls` | `deck/l3` | `generic/stick_left_press` |
| `DE_PAD_RSTICK` | `xbox/rs` | `ps/r3` | `ps/r3` | `switch/rs` | `deck/r3` | `generic/stick_right_press` |
| `DE_PAD_LSHOULDER` | `xbox/lb` | `ps/l1` | `ps/l1` | `switch/l` | `deck/l1` | `generic/shoulder_left` |
| `DE_PAD_RSHOULDER` | `xbox/rb` | `ps/r1` | `ps/r1` | `switch/r` | `deck/r1` | `generic/shoulder_right` |
| `DE_PAD_DPAD_UP` | `xbox/dpad_up` | `ps/dpad_up` | `ps/dpad_up` | `switch/dpad_up` | `deck/dpad_up` | `generic/dpad_up` |
| `DE_PAD_DPAD_DOWN` | `xbox/dpad_down` | `ps/dpad_down` | `ps/dpad_down` | `switch/dpad_down` | `deck/dpad_down` | `generic/dpad_down` |
| `DE_PAD_DPAD_LEFT` | `xbox/dpad_left` | `ps/dpad_left` | `ps/dpad_left` | `switch/dpad_left` | `deck/dpad_left` | `generic/dpad_left` |
| `DE_PAD_DPAD_RIGHT` | `xbox/dpad_right` | `ps/dpad_right` | `ps/dpad_right` | `switch/dpad_right` | `deck/dpad_right` | `generic/dpad_right` |
| `DE_PAD_MISC1` | `xbox/share` | `generic/misc1` | `ps5/mute` | `switch/capture` | `deck/quick_access` | `generic/misc1` |
| `DE_PAD_PADDLE1` | `xbox/p1` | `generic/paddle1` | `generic/paddle1` | `generic/paddle1` | `deck/r4` | `generic/paddle1` |
| `DE_PAD_PADDLE2` | `xbox/p3` | `generic/paddle2` | `generic/paddle2` | `generic/paddle2` | `deck/l4` | `generic/paddle2` |
| `DE_PAD_PADDLE3` | `xbox/p2` | `generic/paddle3` | `generic/paddle3` | `generic/paddle3` | `deck/r5` | `generic/paddle3` |
| `DE_PAD_PADDLE4` | `xbox/p4` | `generic/paddle4` | `generic/paddle4` | `generic/paddle4` | `deck/l5` | `generic/paddle4` |
| `DE_PAD_TOUCHPAD` | `generic/touchpad` | `ps/touchpad` | `ps5/touchpad` | `generic/touchpad` | `generic/touchpad` | `generic/touchpad` |

## Pad axes

A single axis gives the axis ID. A whole stick (`DE_BIND_LEFT_STICK`, `DE_BIND_RIGHT_STICK`)
gives the stick ID.

| Axis code | xbox | ps | ps5 | switch | deck | generic |
|---|---|---|---|---|---|---|
| `DE_AXIS_LEFT_X` | `xbox/stick_left_x` | `ps/stick_left_x` | `ps/stick_left_x` | `switch/stick_left_x` | `deck/stick_left_x` | `generic/stick_left_x` |
| `DE_AXIS_LEFT_Y` | `xbox/stick_left_y` | `ps/stick_left_y` | `ps/stick_left_y` | `switch/stick_left_y` | `deck/stick_left_y` | `generic/stick_left_y` |
| `DE_AXIS_RIGHT_X` | `xbox/stick_right_x` | `ps/stick_right_x` | `ps/stick_right_x` | `switch/stick_right_x` | `deck/stick_right_x` | `generic/stick_right_x` |
| `DE_AXIS_RIGHT_Y` | `xbox/stick_right_y` | `ps/stick_right_y` | `ps/stick_right_y` | `switch/stick_right_y` | `deck/stick_right_y` | `generic/stick_right_y` |
| `DE_AXIS_LEFT_TRIGGER` | `xbox/lt` | `ps/l2` | `ps/l2` | `switch/zl` | `deck/l2` | `generic/trigger_left` |
| `DE_AXIS_RIGHT_TRIGGER` | `xbox/rt` | `ps/r2` | `ps/r2` | `switch/zr` | `deck/r2` | `generic/trigger_right` |
| `DE_BIND_LEFT_STICK` | `xbox/stick_left` | `ps/stick_left` | `ps/stick_left` | `switch/stick_left` | `deck/stick_left` | `generic/stick_left` |
| `DE_BIND_RIGHT_STICK` | `xbox/stick_right` | `ps/stick_right` | `ps/stick_right` | `switch/stick_right` | `deck/stick_right` | `generic/stick_right` |

## Stick directions and the D-pad

A stick axis in a 2D composite slot gives a direction. The direction is the side of the axis
that feeds the slot (the positive side, flipped by `invert` and by a negative `scale`). The last
row is the ID of a 2D composite of the four D-pad buttons.

| Stick direction | xbox | ps | ps5 | switch | deck | generic |
|---|---|---|---|---|---|---|
| left stick up | `xbox/stick_left_up` | `ps/stick_left_up` | `ps/stick_left_up` | `switch/stick_left_up` | `deck/stick_left_up` | `generic/stick_left_up` |
| left stick down | `xbox/stick_left_down` | `ps/stick_left_down` | `ps/stick_left_down` | `switch/stick_left_down` | `deck/stick_left_down` | `generic/stick_left_down` |
| left stick left | `xbox/stick_left_left` | `ps/stick_left_left` | `ps/stick_left_left` | `switch/stick_left_left` | `deck/stick_left_left` | `generic/stick_left_left` |
| left stick right | `xbox/stick_left_right` | `ps/stick_left_right` | `ps/stick_left_right` | `switch/stick_left_right` | `deck/stick_left_right` | `generic/stick_left_right` |
| right stick up | `xbox/stick_right_up` | `ps/stick_right_up` | `ps/stick_right_up` | `switch/stick_right_up` | `deck/stick_right_up` | `generic/stick_right_up` |
| right stick down | `xbox/stick_right_down` | `ps/stick_right_down` | `ps/stick_right_down` | `switch/stick_right_down` | `deck/stick_right_down` | `generic/stick_right_down` |
| right stick left | `xbox/stick_right_left` | `ps/stick_right_left` | `ps/stick_right_left` | `switch/stick_right_left` | `deck/stick_right_left` | `generic/stick_right_left` |
| right stick right | `xbox/stick_right_right` | `ps/stick_right_right` | `ps/stick_right_right` | `switch/stick_right_right` | `deck/stick_right_right` | `generic/stick_right_right` |
| D-pad composite | `xbox/dpad` | `ps/dpad` | `ps/dpad` | `switch/dpad` | `deck/dpad` | `generic/dpad` |

## Gyro

Family-neutral: these IDs are the same for every pad family.

| Code | ID |
|---|---|
| `DE_BIND_AXIS_X` | `generic/gyro_x` |
| `DE_BIND_AXIS_Y` | `generic/gyro_y` |
| `DE_BIND_AXIS_Z` | `generic/gyro_z` |
| `DE_BIND_AXIS_XY` | `generic/gyro` |

## Mouse

| Binding | ID |
|---|---|
| `DE_MOUSE_LEFT` | `kbm/mouse_left` |
| `DE_MOUSE_RIGHT` | `kbm/mouse_right` |
| `DE_MOUSE_MIDDLE` | `kbm/mouse_middle` |
| `DE_MOUSE_X1` | `kbm/mouse_x1` |
| `DE_MOUSE_X2` | `kbm/mouse_x2` |
| mouse motion, `DE_BIND_AXIS_X` | `kbm/mouse_move_x` |
| mouse motion, `DE_BIND_AXIS_Y` | `kbm/mouse_move_y` |
| mouse motion, `DE_BIND_AXIS_XY` | `kbm/mouse_move` |
| wheel, `DE_BIND_AXIS_X` | `kbm/mouse_wheel_x` |
| wheel, `DE_BIND_AXIS_Y` or `DE_BIND_AXIS_XY` | `kbm/mouse_wheel` |
| wheel y in a composite slot, up | `kbm/mouse_wheel_up` |
| wheel y in a composite slot, down | `kbm/mouse_wheel_down` |

## Keyboard composites and special IDs

| ID | Meaning |
|---|---|
| `kbm/wasd` | A 2D composite with W up, S down, A left and D right. |
| `kbm/arrows` | A 2D composite of the four arrow keys. |
| `kbm/key_unknown` | A key binding whose code has no `de_key_name()`. |
| `generic/unbound` | The action has no binding that can be shown, or the action is not valid (`DE_GLYPH_ID_UNBOUND`). |

## Keys

Each named `de_key` has the ID `kbm/key_<name>`, where `<name>` is `de_key_name()`.
`de_glyph_id_key()` gives the key of a key ID. The list is in `de_key` order.

`kbm/key_a` · `kbm/key_b` · `kbm/key_c` · `kbm/key_d` · `kbm/key_e` · `kbm/key_f` ·
`kbm/key_g` · `kbm/key_h` · `kbm/key_i` · `kbm/key_j` · `kbm/key_k` · `kbm/key_l` ·
`kbm/key_m` · `kbm/key_n` · `kbm/key_o` · `kbm/key_p` · `kbm/key_q` · `kbm/key_r` ·
`kbm/key_s` · `kbm/key_t` · `kbm/key_u` · `kbm/key_v` · `kbm/key_w` · `kbm/key_x` ·
`kbm/key_y` · `kbm/key_z` · `kbm/key_1` · `kbm/key_2` · `kbm/key_3` · `kbm/key_4` ·
`kbm/key_5` · `kbm/key_6` · `kbm/key_7` · `kbm/key_8` · `kbm/key_9` · `kbm/key_0` ·
`kbm/key_return` · `kbm/key_escape` · `kbm/key_backspace` · `kbm/key_tab` · `kbm/key_space` · `kbm/key_minus` ·
`kbm/key_equals` · `kbm/key_leftbracket` · `kbm/key_rightbracket` · `kbm/key_backslash` · `kbm/key_nonushash` · `kbm/key_semicolon` ·
`kbm/key_apostrophe` · `kbm/key_grave` · `kbm/key_comma` · `kbm/key_period` · `kbm/key_slash` · `kbm/key_capslock` ·
`kbm/key_f1` · `kbm/key_f2` · `kbm/key_f3` · `kbm/key_f4` · `kbm/key_f5` · `kbm/key_f6` ·
`kbm/key_f7` · `kbm/key_f8` · `kbm/key_f9` · `kbm/key_f10` · `kbm/key_f11` · `kbm/key_f12` ·
`kbm/key_printscreen` · `kbm/key_scrolllock` · `kbm/key_pause` · `kbm/key_insert` · `kbm/key_home` · `kbm/key_pageup` ·
`kbm/key_delete` · `kbm/key_end` · `kbm/key_pagedown` · `kbm/key_right` · `kbm/key_left` · `kbm/key_down` ·
`kbm/key_up` · `kbm/key_numlockclear` · `kbm/key_kp_divide` · `kbm/key_kp_multiply` · `kbm/key_kp_minus` · `kbm/key_kp_plus` ·
`kbm/key_kp_enter` · `kbm/key_kp_1` · `kbm/key_kp_2` · `kbm/key_kp_3` · `kbm/key_kp_4` · `kbm/key_kp_5` ·
`kbm/key_kp_6` · `kbm/key_kp_7` · `kbm/key_kp_8` · `kbm/key_kp_9` · `kbm/key_kp_0` · `kbm/key_kp_period` ·
`kbm/key_nonusbackslash` · `kbm/key_application` · `kbm/key_power` · `kbm/key_kp_equals` · `kbm/key_f13` · `kbm/key_f14` ·
`kbm/key_f15` · `kbm/key_f16` · `kbm/key_f17` · `kbm/key_f18` · `kbm/key_f19` · `kbm/key_f20` ·
`kbm/key_f21` · `kbm/key_f22` · `kbm/key_f23` · `kbm/key_f24` · `kbm/key_execute` · `kbm/key_help` ·
`kbm/key_menu` · `kbm/key_select` · `kbm/key_stop` · `kbm/key_again` · `kbm/key_undo` · `kbm/key_cut` ·
`kbm/key_copy` · `kbm/key_paste` · `kbm/key_find` · `kbm/key_mute` · `kbm/key_volumeup` · `kbm/key_volumedown` ·
`kbm/key_kp_comma` · `kbm/key_kp_equalsas400` · `kbm/key_international1` · `kbm/key_international2` · `kbm/key_international3` · `kbm/key_international4` ·
`kbm/key_international5` · `kbm/key_international6` · `kbm/key_international7` · `kbm/key_international8` · `kbm/key_international9` · `kbm/key_lang1` ·
`kbm/key_lang2` · `kbm/key_lang3` · `kbm/key_lang4` · `kbm/key_lang5` · `kbm/key_lang6` · `kbm/key_lang7` ·
`kbm/key_lang8` · `kbm/key_lang9` · `kbm/key_alterase` · `kbm/key_sysreq` · `kbm/key_cancel` · `kbm/key_clear` ·
`kbm/key_prior` · `kbm/key_return2` · `kbm/key_separator` · `kbm/key_out` · `kbm/key_oper` · `kbm/key_clearagain` ·
`kbm/key_crsel` · `kbm/key_exsel` · `kbm/key_front` · `kbm/key_kp_00` · `kbm/key_kp_000` · `kbm/key_thousandsseparator` ·
`kbm/key_decimalseparator` · `kbm/key_currencyunit` · `kbm/key_currencysubunit` · `kbm/key_kp_leftparen` · `kbm/key_kp_rightparen` · `kbm/key_kp_leftbrace` ·
`kbm/key_kp_rightbrace` · `kbm/key_kp_tab` · `kbm/key_kp_backspace` · `kbm/key_kp_a` · `kbm/key_kp_b` · `kbm/key_kp_c` ·
`kbm/key_kp_d` · `kbm/key_kp_e` · `kbm/key_kp_f` · `kbm/key_kp_xor` · `kbm/key_kp_power` · `kbm/key_kp_percent` ·
`kbm/key_kp_less` · `kbm/key_kp_greater` · `kbm/key_kp_ampersand` · `kbm/key_kp_dblampersand` · `kbm/key_kp_verticalbar` · `kbm/key_kp_dblverticalbar` ·
`kbm/key_kp_colon` · `kbm/key_kp_hash` · `kbm/key_kp_space` · `kbm/key_kp_at` · `kbm/key_kp_exclam` · `kbm/key_kp_memstore` ·
`kbm/key_kp_memrecall` · `kbm/key_kp_memclear` · `kbm/key_kp_memadd` · `kbm/key_kp_memsubtract` · `kbm/key_kp_memmultiply` · `kbm/key_kp_memdivide` ·
`kbm/key_kp_plusminus` · `kbm/key_kp_clear` · `kbm/key_kp_clearentry` · `kbm/key_kp_binary` · `kbm/key_kp_octal` · `kbm/key_kp_decimal` ·
`kbm/key_kp_hexadecimal` · `kbm/key_lctrl` · `kbm/key_lshift` · `kbm/key_lalt` · `kbm/key_lgui` · `kbm/key_rctrl` ·
`kbm/key_rshift` · `kbm/key_ralt` · `kbm/key_rgui` · `kbm/key_mode` · `kbm/key_sleep` · `kbm/key_wake` ·
`kbm/key_channel_increment` · `kbm/key_channel_decrement` · `kbm/key_media_play` · `kbm/key_media_pause` · `kbm/key_media_record` · `kbm/key_media_fast_forward` ·
`kbm/key_media_rewind` · `kbm/key_media_next_track` · `kbm/key_media_previous_track` · `kbm/key_media_stop` · `kbm/key_media_eject` · `kbm/key_media_play_pause` ·
`kbm/key_media_select` · `kbm/key_ac_new` · `kbm/key_ac_open` · `kbm/key_ac_close` · `kbm/key_ac_exit` · `kbm/key_ac_save` ·
`kbm/key_ac_print` · `kbm/key_ac_properties` · `kbm/key_ac_search` · `kbm/key_ac_home` · `kbm/key_ac_back` · `kbm/key_ac_forward` ·
`kbm/key_ac_stop` · `kbm/key_ac_refresh` · `kbm/key_ac_bookmarks` · `kbm/key_softleft` · `kbm/key_softright` · `kbm/key_call` ·
`kbm/key_endcall`
