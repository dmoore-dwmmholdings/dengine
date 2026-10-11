/* sensor_sdl.c: device motion sensors (tablets, some laptops and handhelds) through SDL_Sensor
 * (SPEC §6; DENG-246). Gamepad sensors are in gamepad_sdl.c.
 *
 * Sensors are listed by SDL instance id. An open sensor is updated by every SDL_PumpEvents()
 * (inside de_platform_pump()); de_platform_sensor_read() returns its latest sample. Its
 * SDL_EVENT_SENSOR_UPDATE events are not forwarded to the raw ring (input_sdl.c).
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

static de__platform_sdl_sensor *de__open_slot(de__platform_sdl_devices *dv, SDL_SensorID id) {
    for (u32 i = 0; i < DE__PLATFORM_SDL_SENSOR_MAX; i++)
        if (dv->sensors[i].id == id && dv->sensors[i].sensor)
            return &dv->sensors[i];
    return NULL;
}

void de__platform_sdl_sensors_close_all(de__platform_sdl_devices *dv) {
    for (u32 i = 0; i < DE__PLATFORM_SDL_SENSOR_MAX; i++)
        if (dv->sensors[i].sensor)
            SDL_CloseSensor(dv->sensors[i].sensor);
    memset(dv->sensors, 0, sizeof dv->sensors);
}

u32 de_platform_sensors(de_platform_sensor_info *out, u32 max) {
    de__platform_sdl_devices *dv = de__devices();
    if (!dv || !dv->sensors_ok)
        return 0;
    int count = 0;
    SDL_SensorID *ids = SDL_GetSensors(&count);
    if (!ids)
        return 0;
    for (int i = 0; out && i < count && (u32)i < max; i++) {
        de_platform_sensor_info *o = &out[i];
        memset(o, 0, sizeof *o);
        o->id = ids[i];
        o->type = de__platform_sdl_sensor_type_from_sdl(SDL_GetSensorTypeForID(ids[i]));
        const char *name = SDL_GetSensorNameForID(ids[i]);
        if (name) {
            size_t n = strlen(name);
            if (n >= sizeof o->name)
                n = sizeof o->name - 1u;
            while (n > 0 && ((unsigned char)name[n] & 0xC0u) == 0x80u)
                n--;
            memcpy(o->name, name, n);
        }
    }
    SDL_free(ids);
    return count > 0 ? (u32)count : 0u;
}

de_result de_platform_sensor_enable(u32 sensor, bool on) {
    de__platform_sdl_devices *dv = de__devices();
    if (!dv || !dv->sensors_ok)
        return DE_FAIL(DE_ERR_UNSUPPORTED, "sensors: not available");
    de__platform_sdl_sensor *s = de__open_slot(dv, (SDL_SensorID)sensor);
    if (!on) {
        if (s) {
            SDL_CloseSensor(s->sensor);
            memset(s, 0, sizeof *s);
        }
        return DE_OK;
    }
    if (s)
        return DE_OK;
    for (u32 i = 0; i < DE__PLATFORM_SDL_SENSOR_MAX; i++) {
        if (dv->sensors[i].sensor)
            continue;
        SDL_Sensor *h = SDL_OpenSensor((SDL_SensorID)sensor);
        if (!h)
            return DE_FAIL(DE_ERR_NOT_FOUND, "sensor %u: %s", (unsigned)sensor, SDL_GetError());
        dv->sensors[i].id = (SDL_SensorID)sensor;
        dv->sensors[i].sensor = h;
        return DE_OK;
    }
    return DE_FAIL(DE_ERR_BUSY, "sensors: %d already open", DE__PLATFORM_SDL_SENSOR_MAX);
}

de_result de_platform_sensor_read(u32 sensor, f32 out[3]) {
    de__platform_sdl_devices *dv = de__devices();
    if (!out)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "sensor read: NULL out");
    de__platform_sdl_sensor *s = dv ? de__open_slot(dv, (SDL_SensorID)sensor) : NULL;
    if (!s)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "sensor %u is not open", (unsigned)sensor);
    float v[3] = {0.0f, 0.0f, 0.0f};
    if (!SDL_GetSensorData(s->sensor, v, 3))
        return DE_FAIL(DE_ERR_IO, "sensor %u read: %s", (unsigned)sensor, SDL_GetError());
    out[0] = v[0];
    out[1] = v[1];
    out[2] = v[2];
    return DE_OK;
}
