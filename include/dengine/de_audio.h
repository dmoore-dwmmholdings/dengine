/* de_audio.h — own audio: OS backends, mixer, buses, DSP, 3D spatialization. SPEC.md §14.
 * All calls are thread-safe and lock-free toward the audio thread (command queue). */
#ifndef DE_AUDIO_H
#define DE_AUDIO_H
#include "de_base.h"
#include "de_math.h"
#include "de_ecs.h"
#include "de_asset.h"
DE_EXTERN_C_BEGIN

typedef struct de_audio de_audio;
DE_HANDLE(de_voice);
DE_HANDLE(de_bus);
DE_HANDLE(de_effect);

typedef enum de_audio_backend { DE_AUDIO_DEFAULT, DE_AUDIO_WASAPI, DE_AUDIO_PIPEWIRE, DE_AUDIO_ALSA,
                                DE_AUDIO_COREAUDIO, DE_AUDIO_NULL } de_audio_backend;

typedef struct de_audio_desc {
    const de_allocator *allocator;
    de_asset_system    *assets;
    de_audio_backend    backend;
    u32  sample_rate;        /* default 48000 */
    u32  block_frames;       /* default 256 */
    u32  max_voices;         /* real voices mixed; extra voices are virtualized */
    bool hrtf;
} de_audio_desc;

DE_API de_result de_audio_create(const de_audio_desc *desc, de_audio **out);
DE_API void      de_audio_destroy(de_audio *a);
DE_API void      de_audio_attach(de_audio *a, de_world *w);   /* DeAudioSource / DeAudioListener systems */

typedef struct de_audio_device_info { char name[128]; char id[128]; u32 channels; bool is_default; } de_audio_device_info;
DE_API u32       de_audio_list_devices(de_audio *a, de_audio_device_info *out, u32 max);
DE_API de_result de_audio_set_device(de_audio *a, const char *id);   /* NULL = follow system default */

/* ---------- Buses and effects ---------- */
DE_API de_bus    de_audio_master(de_audio *a);
DE_API de_bus    de_audio_bus_create(de_audio *a, const char *name, de_bus parent);
DE_API void      de_audio_bus_volume(de_audio *a, de_bus b, f32 linear, f32 fade_seconds);
DE_API void      de_audio_bus_mute(de_audio *a, de_bus b, bool mute);

typedef enum de_effect_kind { DE_FX_EQ, DE_FX_LOWPASS, DE_FX_HIGHPASS, DE_FX_COMPRESSOR, DE_FX_LIMITER,
                              DE_FX_REVERB, DE_FX_CONVOLUTION, DE_FX_DELAY, DE_FX_DUCKER } de_effect_kind;
DE_API de_effect de_audio_effect_add(de_audio *a, de_bus b, de_effect_kind kind);
DE_API void      de_audio_effect_param(de_audio *a, de_effect e, de_name param, f32 value);
DE_API void      de_audio_ducker_sidechain(de_audio *a, de_effect ducker, de_bus key);

/* ---------- Voices ---------- */
typedef struct de_play_desc {
    de_sound sound;
    de_bus   bus;            /* zero = master */
    f32      volume, pitch;  /* 0 → 1.0 */
    f32      fade_in;
    bool     loop;
    u8       priority;       /* virtualization order; 0 = highest */
    bool     spatial;
    de_dvec3 position;
    u64      start_at_sample;   /* 0 = immediately; sample-accurate scheduling */
} de_play_desc;

DE_API de_voice de_audio_play(de_audio *a, const de_play_desc *desc);
DE_API void     de_audio_stop(de_audio *a, de_voice v, f32 fade_seconds);
DE_API void     de_audio_set_volume(de_audio *a, de_voice v, f32 linear, f32 fade_seconds);
DE_API void     de_audio_set_pitch(de_audio *a, de_voice v, f32 pitch);
DE_API void     de_audio_set_position(de_audio *a, de_voice v, de_dvec3 pos, de_vec3 velocity);
DE_API bool     de_audio_playing(de_audio *a, de_voice v);
DE_API u64      de_audio_clock(const de_audio *a);   /* samples since start; A/V sync master */

/* Procedural source: called on the audio thread. No allocation or locks allowed. */
typedef void (*de_audio_gen_fn)(f32 *interleaved, u32 frames, u32 channels, void *user);
DE_API de_voice de_audio_play_procedural(de_audio *a, de_audio_gen_fn fn, void *user, u32 channels, de_bus bus);

/* ---------- 3D ---------- */
typedef enum de_attenuation { DE_ATTEN_INVERSE, DE_ATTEN_LINEAR, DE_ATTEN_EXPONENTIAL, DE_ATTEN_CURVE } de_attenuation;

typedef struct DeAudioListener { bool active; } DeAudioListener;

typedef struct DeAudioSource {
    de_sound sound;
    de_bus   bus;
    f32 volume, pitch;
    f32 min_distance, max_distance;
    de_attenuation attenuation;
    f32 cone_inner, cone_outer, cone_outer_gain;
    f32 doppler;
    bool play_on_spawn, loop, occlusion;   /* occlusion uses physics raycasts */
    de_voice voice;                        /* internal */
} DeAudioSource;

typedef struct DeReverbZone { de_vec3 extent; f32 blend_distance; de_name preset; f32 wet; } DeReverbZone;

/* ---------- Mix snapshots ---------- */
DE_API void de_audio_snapshot_apply(de_audio *a, de_name snapshot, f32 fade_seconds);

typedef struct de_audio_stats { u32 voices_real, voices_virtual; f32 dsp_load; u32 underruns; } de_audio_stats;
DE_API de_audio_stats de_audio_get_stats(const de_audio *a);

DE_EXTERN_C_END
#endif
