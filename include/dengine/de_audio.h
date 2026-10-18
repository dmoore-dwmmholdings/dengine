/* de_audio.h — own audio: OS backends, mixer, buses, DSP, 3D spatialization. SPEC.md §14.
 *
 * Threading (DENG-308). Every function here may be called from any thread, unless it says
 * otherwise, and returns at once: calls that change what plays become fixed-size commands in a
 * bounded lock-free queue that the mixer drains at the start of each block. The mixer runs on its
 * own real-time thread and never allocates, locks or waits on the caller. When the queue is full,
 * the command is dropped and counted (de_audio_stats.commands_dropped); a call never blocks.
 *
 * Timeline. The mixer renders float32 blocks of `block_frames` frames at `sample_rate`. Sample
 * numbers count mixer frames since de_audio_create (the first frame is sample 0). A command with a
 * sample time (de_play_desc.start_at_sample) takes effect on exactly that sample; the mixer splits
 * the block there. A time that has already been rendered applies at the start of the next block.
 * de_audio_clock() is the sample number of the next frame the device will play. */
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

/** Output backend. Only the backends compiled in (DE_AUDIO_HAS_<X>, src/audio/CMakeLists.txt)
 * can be chosen; DE_AUDIO_NULL is always there. */
typedef enum de_audio_backend {
    /** The OS backend: WASAPI (Windows), PipeWire then ALSA (Linux), Core Audio (macOS). Falls
     * back to DE_AUDIO_NULL, with a warning, when none is compiled in or none can be opened. */
    DE_AUDIO_DEFAULT,
    DE_AUDIO_WASAPI,
    DE_AUDIO_PIPEWIRE,
    DE_AUDIO_ALSA,
    DE_AUDIO_COREAUDIO,
    /** No device: a timer thread consumes the output at the real-time rate and drops it. For
     * headless runs, servers and tests. */
    DE_AUDIO_NULL
} de_audio_backend;

/** de_audio_create() settings. `(de_audio_desc){0}` gives the defaults. */
typedef struct de_audio_desc {
    const de_allocator *allocator; /**< every allocation of the system; NULL = the heap */
    de_asset_system *assets;       /**< where de_audio_play finds sounds (DENG-313); may be NULL */
    de_audio_backend backend;
    u32 sample_rate;  /**< mixer rate in Hz; 0 = 48000; 8000 .. 192000 */
    u32 block_frames; /**< frames per mixer block; 0 = 256; 16 .. 4096 */
    u32 max_voices;   /**< real voices mixed; full pools steal lowest priority then oldest. 0 = 128; max 4095 */
    bool hrtf;        /**< S26; ignored until then */
    u32 command_capacity; /**< commands in flight to the mixer; 0 = 4096; rounded up to a power of 2
                           */
} de_audio_desc;

/**
 * Creates the audio system: allocates everything it will ever use (command queue, voices, pending
 * list, output ring), opens the output device and starts the mixer thread (`de-audio-mix`,
 * DE_THREAD_REALTIME) and the device control thread (`de-audio-ctl`).
 *
 * The output ring holds `audio.buffer_blocks` blocks (cvar, default 3, read here). Output latency
 * is about that many blocks plus the device's own buffer.
 *
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (`desc` or `out` NULL, a setting out of range),
 *         DE_ERR_UNSUPPORTED (an explicit backend that is not compiled in), DE_ERR_OUT_OF_MEMORY,
 *         DE_ERR_INTERNAL (a thread could not start), or the backend's error.
 */
DE_API de_result de_audio_create(const de_audio_desc *desc, de_audio **out);
/** Stops the threads, closes the device and every source still playing or queued, and frees all
 * memory. NULL is ignored. Not thread-safe against other calls on `a`. */
DE_API void de_audio_destroy(de_audio *a);

/** The ECS resource that de_audio_attach() inserts: struct type "de_audio_resource" in the
 * world's type registry. Systems find it with de_resource_find(). */
typedef struct de_audio_resource {
    de_audio *audio;
} de_audio_resource;

/**
 * Inserts `a` into `w` as the resource de_audio_resource (registering that type in the world's
 * registry first), replacing an earlier one. The DeAudioSource / DeAudioListener systems come in
 * S26. Single-threaded, like de_resource_insert().
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT, or the error of the registration or insert.
 */
DE_API de_result de_audio_attach(de_audio *a, de_world *w);

typedef struct de_audio_device_info {
    char name[128];
    char id[128];
    u32 channels;
    bool is_default;
} de_audio_device_info;
/** Writes up to `max` output devices of the backend to `out` (may be NULL when `max` is 0) and
 * returns how many there are, which may be more than `max`. Not for the audio thread. */
DE_API u32 de_audio_list_devices(de_audio *a, de_audio_device_info *out, u32 max);
/**
 * Switches the output to device `id` (an id from de_audio_list_devices), or to the system default
 * when `id` is NULL; with NULL the output also follows later default changes. The switch is done
 * when the call returns. The mixer keeps running, the audio still in the output ring is dropped
 * and de_audio_clock() jumps forward over it (it never goes back). When the chosen device goes
 * away the output moves to the default and comes back when the device returns. Not for the audio
 * thread.
 * @return DE_OK, DE_ERR_NOT_FOUND (no device has that id), or the backend's error when the device
 *         cannot be opened (the output then stays on, or falls back to, the default).
 */
DE_API de_result de_audio_set_device(de_audio *a, const char *id);

/* ---------- Buses and effects ---------- */
DE_API de_bus de_audio_master(de_audio *a);
DE_API de_bus de_audio_bus_create(de_audio *a, const char *name, de_bus parent);
DE_API void de_audio_bus_volume(de_audio *a, de_bus b, f32 linear, f32 fade_seconds);
DE_API void de_audio_bus_mute(de_audio *a, de_bus b, bool mute);

typedef enum de_effect_kind {
    DE_FX_EQ,
    DE_FX_LOWPASS,
    DE_FX_HIGHPASS,
    DE_FX_COMPRESSOR,
    DE_FX_LIMITER,
    DE_FX_REVERB,
    DE_FX_CONVOLUTION,
    DE_FX_DELAY,
    DE_FX_DUCKER
} de_effect_kind;
DE_API de_effect de_audio_effect_add(de_audio *a, de_bus b, de_effect_kind kind);
DE_API void de_audio_effect_param(de_audio *a, de_effect e, de_name param, f32 value);
DE_API void de_audio_ducker_sidechain(de_audio *a, de_effect ducker, de_bus key);

/* ---------- Voices ---------- */
typedef struct de_play_desc {
    de_sound sound;
    de_bus bus;        /* zero = master */
    f32 volume, pitch; /* 0 → 1.0 */
    f32 fade_in;
    bool loop;
    u8 priority; /* Lower number = higher priority; ties steal the oldest voice. */
    bool spatial;
    de_dvec3 position;
    u64 start_at_sample; /* 0 = immediately; sample-accurate scheduling */
} de_play_desc;

/**
 * Starts sound `desc->sound` and returns its voice at once; the mixer starts it at the start of
 * its next block, or on sample `desc->start_at_sample` exactly. Returns an invalid handle (error
 * context set) when the sound cannot be opened, no voice can be reserved, or the command queue is
 * full. Full pools steal the lowest priority (largest number), then oldest voice with a 5 ms
 * outgoing fade. Bus, volume, pitch, fade and looping are handled by the audio thread; spatial
 * parameters are retained for S26. Until DENG-313 asset sounds cannot be opened.
 */
DE_API de_voice de_audio_play(de_audio *a, const de_play_desc *desc);
/** Starts a stop fade at the next block. At least 5 ms prevents discontinuities. A stale handle
 * is ignored; the voice becomes inactive at the end of the fade. */
DE_API void de_audio_stop(de_audio *a, de_voice v, f32 fade_seconds);
DE_API void de_audio_set_volume(de_audio *a, de_voice v, f32 linear, f32 fade_seconds);
DE_API void de_audio_set_pitch(de_audio *a, de_voice v, f32 pitch);
DE_API void de_audio_set_position(de_audio *a, de_voice v, de_dvec3 pos, de_vec3 velocity);
/** True from de_audio_play until the voice has ended or been stopped (a scheduled voice counts as
 * playing). False for an invalid or stale handle. */
DE_API bool de_audio_playing(de_audio *a, de_voice v);
/** The sample number of the next frame the device will play (samples since start): the A/V sync
 * master. Monotonic, also across device switches; it stands still during an underrun. */
DE_API u64 de_audio_clock(const de_audio *a);

/* Procedural source: called on the audio thread. No allocation or locks allowed. */
typedef void (*de_audio_gen_fn)(f32 *interleaved, u32 frames, u32 channels, void *user);
DE_API de_voice de_audio_play_procedural(de_audio *a, de_audio_gen_fn fn, void *user, u32 channels,
                                         de_bus bus);

/* ---------- 3D ---------- */
typedef enum de_attenuation {
    DE_ATTEN_INVERSE,
    DE_ATTEN_LINEAR,
    DE_ATTEN_EXPONENTIAL,
    DE_ATTEN_CURVE
} de_attenuation;

typedef struct DeAudioListener {
    bool active;
} DeAudioListener;

typedef struct DeAudioSource {
    de_sound sound;
    de_bus bus;
    f32 volume, pitch;
    f32 min_distance, max_distance;
    de_attenuation attenuation;
    f32 cone_inner, cone_outer, cone_outer_gain;
    f32 doppler;
    bool play_on_spawn, loop, occlusion; /* occlusion uses physics raycasts */
    de_voice voice;                      /* internal */
} DeAudioSource;

typedef struct DeReverbZone {
    de_vec3 extent;
    f32 blend_distance;
    de_name preset;
    f32 wet;
} DeReverbZone;

/* ---------- Mix snapshots ---------- */
DE_API void de_audio_snapshot_apply(de_audio *a, de_name snapshot, f32 fade_seconds);

typedef struct de_audio_stats {
    u32 voices_real, voices_virtual; /**< voices mixed in the last block; virtualized ones (S26) */
    f32 dsp_load;         /**< mixer time / block time, moving average (1.0 = no headroom) */
    u32 underruns;        /**< device reads that found the output ring empty (silence played) */
    u32 commands_dropped; /**< commands dropped because the queue or the pending list was full */
} de_audio_stats;
/** Counters since create. Any thread. */
DE_API de_audio_stats de_audio_get_stats(const de_audio *a);

DE_EXTERN_C_END
#endif
