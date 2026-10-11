/* gamepad_sdl.c: gamepads by SDL instance id: open on connect, info, rumble, trigger rumble,
 * LED, raw effect packets and sensor control (SPEC §6, §15; DENG-246).
 *
 * SDL only sends button/axis events for opened gamepads, so input_sdl.c opens each one when its
 * SDL_EVENT_GAMEPAD_ADDED is routed and closes it on SDL_EVENT_GAMEPAD_REMOVED. The table keeps
 * the pads in connection order without gaps (de_platform_pads() lists them in that order).
 */
#include "platform/sdl/sdl_internal.h"

#include <string.h>

static de__platform_sdl_devices *de__devices(void) {
    de__platform_sdl_state *st = de__platform_sdl_get();
    if (!st || !st->devices)
        return NULL;
    DE__PLATFORM_SDL_ASSERT_MAIN(st);
    return st->devices;
}

static i32 de__find(const de__platform_sdl_devices *dv, SDL_JoystickID id) {
    if (id == 0)
        return -1;
    for (u32 i = 0; i < dv->pad_count; i++)
        if (dv->pads[i].id == id)
            return (i32)i;
    return -1;
}

bool de__platform_sdl_pad_known(const de__platform_sdl_devices *dv, SDL_JoystickID id) {
    return de__find(dv, id) >= 0;
}

bool de__platform_sdl_pad_added(de__platform_sdl_devices *dv, SDL_JoystickID id) {
    if (!dv->pads_ok || de__find(dv, id) >= 0)
        return false; /* a repeated ADDED for an open pad is not news */
    if (dv->pad_count == DE_PLATFORM_PAD_MAX) {
        DE_WARN(DE_LOG_CAT_PLATFORM, "gamepad %u ignored: %d gamepads are already open",
                (unsigned)id, DE_PLATFORM_PAD_MAX);
        return false;
    }
    SDL_Gamepad *pad = SDL_OpenGamepad(id);
    if (!pad) {
        DE_WARN(DE_LOG_CAT_PLATFORM, "gamepad %u: open failed: %s", (unsigned)id, SDL_GetError());
        return false;
    }
    dv->pads[dv->pad_count].id = id;
    dv->pads[dv->pad_count].pad = pad;
    dv->pad_count++;
    return true;
}

bool de__platform_sdl_pad_removed(de__platform_sdl_devices *dv, SDL_JoystickID id) {
    i32 i = de__find(dv, id);
    if (i < 0)
        return false;
    SDL_CloseGamepad(dv->pads[i].pad);
    u32 rest = dv->pad_count - (u32)i - 1u;
    memmove(&dv->pads[i], &dv->pads[i + 1], rest * sizeof dv->pads[0]);
    dv->pad_count--;
    memset(&dv->pads[dv->pad_count], 0, sizeof dv->pads[0]);
    return true;
}

void de__platform_sdl_pads_close_all(de__platform_sdl_devices *dv) {
    for (u32 i = 0; i < dv->pad_count; i++)
        SDL_CloseGamepad(dv->pads[i].pad);
    memset(dv->pads, 0, sizeof dv->pads);
    dv->pad_count = 0;
}

de_platform_sensor_type de__platform_sdl_sensor_type_from_sdl(SDL_SensorType t) {
    switch (t) {
    case SDL_SENSOR_ACCEL:
        return DE_PLATFORM_SENSOR_ACCEL;
    case SDL_SENSOR_GYRO:
        return DE_PLATFORM_SENSOR_GYRO;
    case SDL_SENSOR_ACCEL_L:
        return DE_PLATFORM_SENSOR_ACCEL_L;
    case SDL_SENSOR_GYRO_L:
        return DE_PLATFORM_SENSOR_GYRO_L;
    case SDL_SENSOR_ACCEL_R:
        return DE_PLATFORM_SENSOR_ACCEL_R;
    case SDL_SENSOR_GYRO_R:
        return DE_PLATFORM_SENSOR_GYRO_R;
    default:
        return DE_PLATFORM_SENSOR_UNKNOWN;
    }
}

SDL_SensorType de__platform_sdl_sensor_type_to_sdl(de_platform_sensor_type t) {
    switch (t) {
    case DE_PLATFORM_SENSOR_ACCEL:
        return SDL_SENSOR_ACCEL;
    case DE_PLATFORM_SENSOR_GYRO:
        return SDL_SENSOR_GYRO;
    case DE_PLATFORM_SENSOR_ACCEL_L:
        return SDL_SENSOR_ACCEL_L;
    case DE_PLATFORM_SENSOR_GYRO_L:
        return SDL_SENSOR_GYRO_L;
    case DE_PLATFORM_SENSOR_ACCEL_R:
        return SDL_SENSOR_ACCEL_R;
    case DE_PLATFORM_SENSOR_GYRO_R:
        return SDL_SENSOR_GYRO_R;
    default:
        return SDL_SENSOR_INVALID;
    }
}

/* ---------- public API ---------- */

u32 de_platform_pads(u32 *out, u32 max) {
    de__platform_sdl_devices *dv = de__devices();
    if (!dv)
        return 0;
    for (u32 i = 0; out && i < max && i < dv->pad_count; i++)
        out[i] = dv->pads[i].id;
    return dv->pad_count;
}

/* The open pad `id`, or NULL with DE_ERR_NOT_FOUND in `*r`. */
static SDL_Gamepad *de__pad(u32 id, de_result *r) {
    de__platform_sdl_devices *dv = de__devices();
    i32 i = dv ? de__find(dv, (SDL_JoystickID)id) : -1;
    if (i < 0) {
        *r = DE_FAIL(DE_ERR_NOT_FOUND, "gamepad %u is not connected", (unsigned)id);
        return NULL;
    }
    return dv->pads[i].pad;
}

static de_platform_pad_type de__pad_type(SDL_GamepadType t) {
    switch (t) {
    case SDL_GAMEPAD_TYPE_XBOX360:
    case SDL_GAMEPAD_TYPE_XBOXONE:
        return DE_PLATFORM_PAD_XBOX;
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
        return DE_PLATFORM_PAD_PS4; /* same face-button glyphs */
    case SDL_GAMEPAD_TYPE_PS5:
        return DE_PLATFORM_PAD_PS5;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
        return DE_PLATFORM_PAD_SWITCH;
    case SDL_GAMEPAD_TYPE_STEAM:
        return DE_PLATFORM_PAD_STEAM;
    default:
        return DE_PLATFORM_PAD_GENERIC;
    }
}

static bool de__cap(SDL_Gamepad *pad, const char *prop) {
    return SDL_GetBooleanProperty(SDL_GetGamepadProperties(pad), prop, false);
}

de_result de_platform_pad_info_get(u32 id, de_platform_pad_info *out) {
    if (!out)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "gamepad info: NULL out");
    memset(out, 0, sizeof *out);
    de_result r = DE_OK;
    SDL_Gamepad *pad = de__pad(id, &r);
    if (!pad)
        return r;
    out->id = id;
    out->type = de__pad_type(SDL_GetGamepadType(pad));
    out->vendor = SDL_GetGamepadVendor(pad);
    out->product = SDL_GetGamepadProduct(pad);
    out->player_index = SDL_GetGamepadPlayerIndex(pad);
    int touchpads = SDL_GetNumGamepadTouchpads(pad);
    out->touchpads = touchpads > 0 ? (u32)touchpads : 0u;
    u32 caps = 0;
    if (de__cap(pad, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN))
        caps |= DE_PLATFORM_PAD_CAP_RUMBLE;
    if (de__cap(pad, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN))
        caps |= DE_PLATFORM_PAD_CAP_TRIGGER_RUMBLE;
    if (de__cap(pad, SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN) ||
        de__cap(pad, SDL_PROP_GAMEPAD_CAP_MONO_LED_BOOLEAN))
        caps |= DE_PLATFORM_PAD_CAP_LED;
    if (SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO))
        caps |= DE_PLATFORM_PAD_CAP_GYRO;
    if (SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL))
        caps |= DE_PLATFORM_PAD_CAP_ACCEL;
    if (touchpads > 0)
        caps |= DE_PLATFORM_PAD_CAP_TOUCHPAD;
    out->caps = caps;
    const char *name = SDL_GetGamepadName(pad);
    if (name) {
        size_t n = strlen(name);
        if (n >= sizeof out->name)
            n = sizeof out->name - 1u;
        /* Cut at a code point boundary. */
        while (n > 0 && ((unsigned char)name[n] & 0xC0u) == 0x80u)
            n--;
        memcpy(out->name, name, n);
    }
    return DE_OK;
}

static Uint16 de__u16(f32 v) {
    if (!(v > 0.0f)) /* also NaN */
        return 0;
    if (v >= 1.0f)
        return 0xFFFF;
    return (Uint16)(v * 65535.0f + 0.5f);
}

de_result de_platform_pad_rumble(u32 id, f32 low, f32 high, u32 duration_ms) {
    de_result r = DE_OK;
    SDL_Gamepad *pad = de__pad(id, &r);
    if (!pad)
        return r;
    if (!de__cap(pad, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN))
        return DE_FAIL(DE_ERR_UNSUPPORTED, "gamepad %u has no rumble", (unsigned)id);
    if (!SDL_RumbleGamepad(pad, de__u16(low), de__u16(high), duration_ms))
        return DE_FAIL(DE_ERR_IO, "gamepad %u rumble: %s", (unsigned)id, SDL_GetError());
    return DE_OK;
}

de_result de_platform_pad_trigger_rumble(u32 id, f32 left, f32 right, u32 duration_ms) {
    de_result r = DE_OK;
    SDL_Gamepad *pad = de__pad(id, &r);
    if (!pad)
        return r;
    if (!de__cap(pad, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN))
        return DE_FAIL(DE_ERR_UNSUPPORTED, "gamepad %u has no trigger rumble", (unsigned)id);
    if (!SDL_RumbleGamepadTriggers(pad, de__u16(left), de__u16(right), duration_ms))
        return DE_FAIL(DE_ERR_IO, "gamepad %u trigger rumble: %s", (unsigned)id, SDL_GetError());
    return DE_OK;
}

de_result de_platform_pad_set_led(u32 id, u8 red, u8 green, u8 blue) {
    de_result r = DE_OK;
    SDL_Gamepad *pad = de__pad(id, &r);
    if (!pad)
        return r;
    if (!de__cap(pad, SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN) &&
        !de__cap(pad, SDL_PROP_GAMEPAD_CAP_MONO_LED_BOOLEAN))
        return DE_FAIL(DE_ERR_UNSUPPORTED, "gamepad %u has no LED", (unsigned)id);
    if (!SDL_SetGamepadLED(pad, red, green, blue))
        return DE_FAIL(DE_ERR_IO, "gamepad %u LED: %s", (unsigned)id, SDL_GetError());
    return DE_OK;
}

de_result de_platform_pad_send_effect(u32 id, const void *data, u32 size) {
    if (!data || size == 0 || size > (u32)0x7FFFFFFF)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "gamepad effect: no data");
    de_result r = DE_OK;
    SDL_Gamepad *pad = de__pad(id, &r);
    if (!pad)
        return r;
    /* SDL has no capability flag for effect packets: only drivers that take them (PS4, PS5,
     * virtual pads with a SendEffect callback) succeed; every other driver reports
     * "unsupported", which is the common failure. */
    if (!SDL_SendGamepadEffect(pad, data, (int)size))
        return DE_FAIL(DE_ERR_UNSUPPORTED, "gamepad %u effect: %s", (unsigned)id, SDL_GetError());
    return DE_OK;
}

bool de_platform_pad_has_sensor(u32 id, de_platform_sensor_type type) {
    de__platform_sdl_devices *dv = de__devices();
    i32 i = dv ? de__find(dv, (SDL_JoystickID)id) : -1;
    SDL_SensorType t = de__platform_sdl_sensor_type_to_sdl(type);
    return i >= 0 && t != SDL_SENSOR_INVALID && SDL_GamepadHasSensor(dv->pads[i].pad, t);
}

de_result de_platform_pad_sensor_enable(u32 id, de_platform_sensor_type type, bool on) {
    de_result r = DE_OK;
    SDL_Gamepad *pad = de__pad(id, &r);
    if (!pad)
        return r;
    SDL_SensorType t = de__platform_sdl_sensor_type_to_sdl(type);
    if (t == SDL_SENSOR_INVALID || !SDL_GamepadHasSensor(pad, t))
        return DE_FAIL(DE_ERR_UNSUPPORTED, "gamepad %u has no sensor of type %d", (unsigned)id,
                       (int)type);
    if (!SDL_SetGamepadSensorEnabled(pad, t, on))
        return DE_FAIL(DE_ERR_IO, "gamepad %u sensor: %s", (unsigned)id, SDL_GetError());
    return DE_OK;
}
