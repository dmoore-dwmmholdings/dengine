/*
 * backend.h: the audio output backend interface (DENG-308). Include as "audio/backend.h".
 *
 * One vtable per OS backend: WASAPI (DENG-309), PipeWire and ALSA (DENG-310), Core Audio
 * (DENG-311), and the null backend (backend_null.c, always built). The device manager (device.c)
 * is the only caller. A backend is compiled in when src/audio/CMakeLists.txt sets
 * DE_AUDIO_HAS_<X>=1 on de_audio and adds its sources; the matching vtable is declared below.
 *
 * Contract:
 *   - `create` makes a backend instance. It keeps `desc->notify` and calls it, from any thread
 *     (often an OS notification thread), when devices come or go, the system default changes, or
 *     an open stream fails. `notify` only sets flags and wakes the control thread; the backend
 *     never reopens a stream by itself.
 *   - `enumerate`, `open`, `start`, `stop`, `close` and `destroy` run on non-real-time threads
 *     (the control thread, or a caller of de_audio_set_device / de_audio_list_devices), one at a
 *     time per instance (the device manager holds its mutex around them).
 *   - `open` asks for `desc->sample_rate` Hz, `desc->channels` float32 interleaved channels,
 *     converted by the OS when the device differs (WASAPI autoconvert, AudioUnit converter,
 *     PipeWire resampling): the mixer never resamples device output. `block_frames` is a hint for
 *     the device period.
 *   - After `start`, the stream calls `desc->render(desc->user, out, frames)` from its device
 *     thread for every period, with any `frames`. That call is real-time: it is the consumer side
 *     of the mixer's output ring (de__audio_ring_read). The backend must not allocate or lock on
 *     that thread either.
 *   - `stop` returns only when no render call is running and none will start, so the device
 *     manager can then take over the consumer side of the ring (flush on switch).
 */
#ifndef DE_AUDIO_BACKEND_H
#define DE_AUDIO_BACKEND_H

#include <dengine/de_audio.h>
#include <dengine/de_memory.h>

#ifndef DE_AUDIO_HAS_WASAPI
#define DE_AUDIO_HAS_WASAPI 0
#endif
#ifndef DE_AUDIO_HAS_PIPEWIRE
#define DE_AUDIO_HAS_PIPEWIRE 0
#endif
#ifndef DE_AUDIO_HAS_ALSA
#define DE_AUDIO_HAS_ALSA 0
#endif
#ifndef DE_AUDIO_HAS_COREAUDIO
#define DE_AUDIO_HAS_COREAUDIO 0
#endif

/** Bits passed to de__audio_notify_fn. */
typedef enum de__audio_device_event {
    DE__AUDIO_EV_DEVICES_CHANGED = 1u << 0, /**< a device was added or removed */
    DE__AUDIO_EV_DEFAULT_CHANGED = 1u << 1, /**< the system default output changed */
    DE__AUDIO_EV_STREAM_LOST = 1u << 2,     /**< the open stream failed (device gone, error) */
} de__audio_device_event;

/** Device change notification: `events` is a set of de__audio_device_event bits. Any thread. */
typedef void (*de__audio_notify_fn)(void *user, u32 events);

/** Real-time render callback of a stream: write `frames` interleaved frames to `out`. */
typedef void (*de__audio_render_fn)(void *user, f32 *out, u32 frames);

typedef struct de__audio_backend_desc {
    const de_allocator *allocator; /**< never NULL */
    de__audio_notify_fn notify;
    void *notify_user;
} de__audio_backend_desc;

typedef struct de__audio_stream_desc {
    u32 sample_rate;
    u32 channels;
    u32 block_frames;
    de__audio_render_fn render;
    void *user;
    void (*underrun)(void *user); /**< optional atomic accounting for a backend XRUN */
} de__audio_stream_desc;

/** An open output stream. Backends put this first in their own stream struct. */
typedef struct de__audio_stream {
    const struct de__audio_backend_vtbl *vt;
    void *backend; /**< the instance that opened it */
} de__audio_stream;

typedef struct de__audio_backend_vtbl {
    const char *name; /**< "null", "wasapi", "pipewire", "alsa", "coreaudio" */
    de_audio_backend kind;
    /** Creates an instance in `*out`. Fails when the OS service is missing (the caller may then
     * fall back to null). */
    de_result (*create)(const de__audio_backend_desc *desc, void **out);
    void (*destroy)(void *backend);
    /** Same contract as de_audio_list_devices: writes up to `max`, returns the total. */
    u32 (*enumerate)(void *backend, de_audio_device_info *out, u32 max);
    /** Opens device `device_id`, or the system default when NULL. Does not start it. */
    de_result (*open)(void *backend, const char *device_id, const de__audio_stream_desc *desc,
                      de__audio_stream **out);
    de_result (*start)(de__audio_stream *s);
    /** Returns when no render call runs any more (see the file comment). */
    void (*stop)(de__audio_stream *s);
    void (*close)(de__audio_stream *s);
    void (*poll_devices)(void *backend); /**< optional control-thread poll, never RT */
} de__audio_backend_vtbl;

extern const de__audio_backend_vtbl de__audio_backend_null;
#if DE_AUDIO_HAS_WASAPI
extern const de__audio_backend_vtbl de__audio_backend_wasapi;
#endif
#if DE_AUDIO_HAS_PIPEWIRE
extern const de__audio_backend_vtbl de__audio_backend_pipewire;
#endif
#if DE_AUDIO_HAS_ALSA
extern const de__audio_backend_vtbl de__audio_backend_alsa;
#endif
#if DE_AUDIO_HAS_COREAUDIO
extern const de__audio_backend_vtbl de__audio_backend_coreaudio;
#endif

#endif /* DE_AUDIO_BACKEND_H */
