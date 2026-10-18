/*
 * audio_internal.h: internal state of the audio module (DENG-308). Include as
 * "audio/audio_internal.h". Tests include it too (src/ is an include root for them).
 *
 * Threads and who owns what:
 *   - Callers (any thread): de_audio_play / stop / ... allocate voice slots from `free_slots`
 *     and push fixed-size commands into `cmd_queue`. Neither ever blocks.
 *   - Audio thread: the mixer thread (`de-audio-mix`), or the thread inside de__audio_null_pump
 *     in manual mode. It is the only consumer of `cmd_queue`, the only producer of the output
 *     `ring`, and the only thread that touches `voices`, `active`, `pending` and the mix buffers.
 *   - Device thread: the backend's render callback (de__audio_ring_read), the consumer of `ring`.
 *     During a device switch the control thread holds that role (de__audio_ring_flush).
 *   - Control thread (`de-audio-ctl`, device.c): reopens streams on device events, closes the
 *     sources of ended voices (`retired`) and gives their slots back to `free_slots`.
 *
 * Extension rule (merge hazard): DENG-312 (buses, fades, stealing) and the S26 tasks (spatial,
 * virtualization) extend de__voice, de__audio_cmd and struct de_audio by appending fields in a
 * block marked with their key, at the end of each struct, so nobody edits the same lines.
 */
#ifndef DE_AUDIO_INTERNAL_H
#define DE_AUDIO_INTERNAL_H

#include <dengine/de_audio.h>
#include <dengine/de_platform.h>

#include "audio/backend.h"
#include "audio/source.h"
#include "audio/dsp/resample.h"
#include "core/de_mpmc.h"
#include "platform/de_atomic.h"

/* ---- Limits and defaults --------------------------------------------------------------------- */

#define DE__AUDIO_CHANNELS            2u /* mixer output: stereo in S25 (channel maps: DENG-312) */
#define DE__AUDIO_MAX_SOURCE_CHANNELS 8u
#define DE__AUDIO_DEFAULT_RATE        48000u
#define DE__AUDIO_DEFAULT_BLOCK       256u
#define DE__AUDIO_DEFAULT_VOICES      128u
#define DE__AUDIO_DEFAULT_COMMANDS    4096u
#define DE__AUDIO_MAX_COMMANDS        (1u << 20)
#define DE__AUDIO_DEFAULT_RING_BLOCKS 3u
#define DE__AUDIO_MIN_RING_BLOCKS     2u
#define DE__AUDIO_MAX_RING_BLOCKS     16u

/* Voice handles: id = (generation << 12) | (slot + 1). 4095 slots, generations 1 .. 2^20 - 1
 * (0 is skipped when the counter wraps). A stale handle is one whose generation differs from the
 * slot's current one. */
#define DE__VOICE_SLOT_BITS 12u
#define DE__VOICE_MAX_SLOTS ((1u << DE__VOICE_SLOT_BITS) - 1u)
#define DE__VOICE_GEN_MASK  ((1u << (32u - DE__VOICE_SLOT_BITS)) - 1u)

static inline u32 de__voice_slot(de_voice v) {
    return (v.id & ((1u << DE__VOICE_SLOT_BITS) - 1u)) - 1u; /* UINT32_MAX for the null handle */
}
static inline u32 de__voice_gen(de_voice v) {
    return v.id >> DE__VOICE_SLOT_BITS;
}
static inline de_voice de__voice_make(u32 slot, u32 gen) {
    de_voice v;
    v.id = (gen << DE__VOICE_SLOT_BITS) | (slot + 1u);
    return v;
}
static inline u32 de__voice_next_gen(u32 gen) {
    gen = (gen + 1u) & DE__VOICE_GEN_MASK;
    return gen == 0 ? 1u : gen;
}

/* ---- Commands -------------------------------------------------------------------------------- */

typedef enum de__audio_cmd_op {
    DE__ACMD_NOP = 0,
    DE__ACMD_PLAY, /* u.play; the voice slot was allocated by the caller */
    DE__ACMD_STOP, /* u.stop */
    DE__ACMD_FN,   /* u.fn: run a callback on the audio thread at the sample (tests, tools) */
    /* DENG-312 appends its ops (volume, pitch, bus, fades) here. */
    DE__ACMD_VOLUME,
    DE__ACMD_PITCH,
    DE__ACMD_POSITION,
    DE__ACMD_BUS_CREATE,
    DE__ACMD_BUS_VOLUME,
    DE__ACMD_BUS_MUTE,
    DE__ACMD_OP_COUNT_
} de__audio_cmd_op;

/** Callback of DE__ACMD_FN. Runs on the audio thread at `sample` (the mixer sample where the
 * command took effect): real-time rules apply. */
typedef void (*de__audio_cmd_fn)(de_audio *a, void *user, u64 arg, u64 sample);

typedef struct de__audio_cmd {
    u32 op;        /* de__audio_cmd_op */
    u32 voice;     /* de_voice.id, for voice commands */
    u64 at_sample; /* 0 = at the start of the next block */
    u64 seq;       /* arrival order, set by the audio thread when it pops the command */
    union {
        struct {
            de__audio_source *src;
            u32 bus;
            f32 volume, pitch;
            u8 loop, priority;
            f32 fade;
            bool spatial;
            de_dvec3 position;
        } play;
        struct {
            f32 fade;
        } stop;
        struct {
            f32 value, fade;
            u32 parent;
            de_dvec3 position;
            de_vec3 velocity;
        } param;
        struct {
            de__audio_cmd_fn fn;
            void *user;
            u64 arg;
        } fn;
    } u;
} de__audio_cmd;

/* ---- Voices ---------------------------------------------------------------------------------- */

typedef enum de__voice_state {
    DE__VOICE_FREE = 0,  /* no voice; or ended and waiting for the control thread */
    DE__VOICE_SCHEDULED, /* play command waits in the pending list */
    DE__VOICE_PLAYING,
} de__voice_state;

/** Audio-thread state of one voice slot. `src` is handed to the control thread through
 * `retired` when the voice ends (the slot is not touched again until it is reallocated). */
typedef struct de__audio_ramp {
    f32 value, target, delta;
    u32 remaining;
} de__audio_ramp;
typedef struct de__audio_bus {
    u32 parent;
    bool active, mute;
    char name[64];
    de__audio_ramp gain;
} de__audio_bus;
typedef struct de__voice_reservation {
    de_atomic_u32 priority, busy;
    de_atomic_u64 age;
} de__voice_reservation;
typedef struct de__voice {
    u32 state; /* de__voice_state */
    u32 gen;   /* generation of the handle this state belongs to */
    de__audio_source *src;
    u32 active_index; /* position in de_audio.active while PLAYING */
    u32 bus;
    f32 volume, pitch;
    u8 loop, priority;
    u8 reserved_[2];
    u64 start_sample;  /* mixer sample of the first frame */
    u64 frames_played; /* frames read from the source, loops included */
    /* DENG-312: audio-thread state only; caller reservations are separate atomics. */
    de__audio_ramp gain;
    de__resampler *resampler;
    bool stopping, source_eof, resampling, spatial;
    de_dvec3 position;
    de_vec3 velocity;
} de__voice;

/* ---- Output ring (SPSC of whole blocks) ------------------------------------------------------ */

typedef struct de__audio_ring {
    f32 *data;        /* blocks * block_samples */
    u64 *block_start; /* mixer sample of each slot's first frame */
    u32 blocks;
    u32 block_samples; /* block_frames * channels */
    /* Producer (audio thread) advances write_pos, consumer (device thread) read_pos; both count
     * blocks and only grow. seq_cst on the read side pairs with the mixer's wait flag. */
    de_atomic_u64 write_pos;
    de_atomic_u64 read_pos;
    u32 read_offset; /* consumer only: frames already read from block read_pos */
} de__audio_ring;

/* ---- Device manager (device.c) --------------------------------------------------------------- */

typedef struct de__audio_devmgr {
    const de__audio_backend_vtbl *vt; /* the backend in use */
    void *backend;
    const de__audio_backend_vtbl *fallback_vt; /* null, when `vt` is an OS backend */
    void *fallback;
    de__audio_stream *stream; /* current stream, or NULL (manual mode, or none could open) */
    bool stream_is_fallback;
    de_mutex lock; /* control-side state; never taken by the audio or device thread */
    bool follow_default;
    char requested_id[128]; /* de_audio_set_device's id, when !follow_default */
    char current_id[128];   /* id the stream was opened with ("" = system default) */
    de_thread *thread;
    de_sem wake;
    de_atomic_u32 events; /* de__audio_device_event bits not handled yet */
    de_atomic_u32 quit;
    de_atomic_u32 opens; /* streams opened so far (switch counter for tests) */
} de__audio_devmgr;

/* ---- The system ------------------------------------------------------------------------------ */

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push)
#pragma warning(disable : 4324) /* structure was padded due to alignment specifier: intended */
#endif

struct de_audio {
    /* Queues (cache-line aligned). */
    de_mpmc cmd_queue;  /* de__audio_cmd, callers -> audio thread */
    de_mpmc free_slots; /* u32 slot, control thread / callers -> callers */
    de_mpmc retired;    /* u32 slot, audio thread -> control thread / callers */

    de_allocator alloc; /* copy of the desc allocator */
    de_asset_system *assets;
    de_log_category log;
    u32 sample_rate;
    u32 block_frames;
    u32 channels;
    u32 voice_count; /* slots = max_voices */
    u32 command_capacity;
    bool manual; /* null backend in manual pump mode: no mixer thread, no stream */

    /* Voices. `voice_gen[i]` is the generation of slot i's current (or next) handle; callers read
     * it, the owner of the slot writes it. */
    de__voice *voices;
    de_atomic_u32 *voice_gen;
    u32 *active; /* PLAYING slots, audio thread */
    u32 active_count;

    /* DENG-312: preallocated bus graph and stolen-voice tails. */
    de__voice *tails;
    de__voice_reservation *reservations;
    de_mpmc retired_tails;
    de_atomic_u64 play_sequence;
    de__resampler *resample_bank;
    f32 *resample_buf, *bus_buf;
    de__audio_bus buses[64];
    de_atomic_u32 bus_count;
    de_atomic_ptr stream_manager;

    /* Pending commands with a future sample, sorted by (at_sample, seq). Audio thread. */
    de__audio_cmd *pending;
    u32 pending_count;
    u64 next_seq;

    /* Mixer (audio thread). */
    u64 mix_pos;  /* sample of the next block to render */
    f32 *src_buf; /* block_frames * DE__AUDIO_MAX_SOURCE_CHANNELS */
    f32 dsp_load; /* moving average, published in dsp_load_bits */
    bool retired_this_block;

    de__audio_ring ring;

    /* Mixer thread. */
    de_thread *mixer;
    de_sem mixer_wake;
    de_atomic_u32 mixer_waiting;
    de_atomic_u32 mixer_quit;
    de_atomic_u32 test_stall; /* tests: the mixer renders nothing while set */

    /* Counters (any thread reads). */
    de_atomic_u64 frames_mixed;
    de_atomic_u64 play_pos; /* de_audio_clock */
    de_atomic_u32 underruns;
    de_atomic_u32 cmds_dropped;
    de_atomic_u32 cmds_late; /* applied after their sample (it was already rendered) */
    de_atomic_u32 voices_playing;
    de_atomic_u32 dsp_load_bits;

    de__audio_devmgr dev;
    /* DENG-312 appends the bus tree here; S26 appends listener, spatial and virtual voices. */
};

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif

/* ---- audio.c --------------------------------------------------------------------------------- */

/** Test-only create options. */
typedef struct de__audio_create_opts {
    /** DE_AUDIO_NULL only: no mixer thread and no device thread; the output is produced and
     * consumed inside de__audio_null_pump, on the caller's thread. Deterministic. */
    bool null_manual;
} de__audio_create_opts;

de_result de__audio_create_ex(const de_audio_desc *desc, const de__audio_create_opts *opts,
                              de_audio **out);

/* ---- command_queue.c ------------------------------------------------------------------------- */

/** Caller side: pushes `cmd`. On a full queue counts the drop, warns (Dev, rate-limited) and
 * returns false. Never blocks. */
bool de__audio_cmd_push(de_audio *a, const de__audio_cmd *cmd);
/** Caller side: runs `fn(a, user, arg, sample)` on the audio thread at `at_sample` (0 = next
 * block). False when the queue was full. */
bool de__audio_post_fn(de_audio *a, de__audio_cmd_fn fn, void *user, u64 arg, u64 at_sample);
/** Caller side: takes a free voice slot, collecting ended voices first when none is free.
 * Returns false when every slot is in use. `*gen` is the new handle's generation. */
bool de__audio_slot_alloc(de_audio *a, u32 *slot, u32 *gen);
/** Gives a slot back that the caller allocated but never handed to the audio thread. */
void de__audio_slot_release(de_audio *a, u32 slot);
/** Any non-audio thread: closes the sources of ended voices and frees their slots. Returns how
 * many it collected. */
u32 de__audio_collect(de_audio *a);
/** Audio thread: pops every queued command; applies due ones at `block_start`, keeps future
 * ones in the pending list. */
void de__audio_cmd_drain(de_audio *a, u64 block_start);
/** Audio thread: the sample of the first pending command, or UINT64_MAX. */
u64 de__audio_pending_next(const de_audio *a);
/** Audio thread: applies every pending command whose sample is <= `sample`, in order. */
void de__audio_pending_apply(de_audio *a, u64 sample);
/** Audio thread: applies one command at mixer sample `sample`. */
void de__audio_cmd_apply(de_audio *a, const de__audio_cmd *cmd, u64 sample);
/** Destroy: closes the sources held by commands still queued or pending. */
void de__audio_cmd_discard_all(de_audio *a);

/* ---- voice.c --------------------------------------------------------------------------------- */

/** Starts `src` on a new voice (the voice owns `src` from here on, even on failure, where it is
 * closed). Same rules as de_audio_play. */
de_voice de__voice_play_source(de_audio *a, de__audio_source *src, const de_play_desc *desc);
/** Stops `v` on sample `at_sample` exactly (0 = next block). */
void de__voice_stop_at(de_audio *a, de_voice v, u64 at_sample);
/** Audio thread: command handlers. */
void de__voice_on_play_queued(de_audio *a, const de__audio_cmd *cmd);
void de__voice_on_play(de_audio *a, const de__audio_cmd *cmd, u64 sample);
void de__voice_on_stop(de_audio *a, const de__audio_cmd *cmd);
/** Audio thread: mixes every playing voice into `out` (frames x channels, already zeroed or
 * holding earlier voices) for the mixer samples [sample, sample + frames). */
void de__voice_mix_all(de_audio *a, f32 *out, u32 frames, u64 sample);
/** Audio thread: ends the voice in slot `slot` and hands its source to the control thread. */
void de__voice_retire(de_audio *a, u32 slot);

/* ---- mixer.c --------------------------------------------------------------------------------- */

/** Audio thread: renders the next block into the ring. The ring must have a free block. */
void de__mixer_render_block(de_audio *a);

/* ---- mixer_thread.c -------------------------------------------------------------------------- */

de_result de__audio_ring_init(de_audio *a, u32 blocks);
void de__audio_ring_free(de_audio *a);
/** Producer: free blocks in the ring. */
u32 de__audio_ring_space(de_audio *a);
/** Producer: the next block's memory (ring must have space). */
f32 *de__audio_ring_write_ptr(de_audio *a);
/** Producer: publishes the block written at write_ptr, starting at mixer sample `start`. */
void de__audio_ring_publish(de_audio *a, u64 start);
/** Consumer (device thread): `frames` frames into `out`; silence and an underrun when the ring
 * runs dry. Real-time. The de__audio_render_fn of every stream. */
void de__audio_ring_read(void *audio, f32 *out, u32 frames);
/** Consumer (control thread, no stream running): drops everything queued; the clock jumps to
 * the end of what was dropped. */
void de__audio_ring_flush(de_audio *a);
de_result de__audio_mixer_start(de_audio *a);
void de__audio_mixer_stop(de_audio *a);

/* ---- device.c -------------------------------------------------------------------------------- */

/** Picks and creates the backend and starts the control thread. */
de_result de__audio_device_init(de_audio *a, de_audio_backend kind);
/** Opens and starts the output stream (nothing in manual mode). After the mixer started. */
de_result de__audio_device_start(de_audio *a);
/** Stops the control thread, closes the stream and the backend. The mixer thread must be stopped
 * first: it wakes the control thread through `dev.wake`. */
void de__audio_device_shutdown(de_audio *a);
/** Backend notification sink (de__audio_notify_fn): `user` is the de_audio. */
void de__audio_device_notify(void *user, u32 events);
/** Wakes the control thread (for example after the mixer retired voices). Any thread. */
void de__audio_device_wake(de_audio *a);
/** Streams opened since create. */
u32 de__audio_device_opens(const de_audio *a);

/* ---- backend_null.c (test hooks) ------------------------------------------------------------- */

/** Manual mode: renders as many blocks as needed and writes the next `frames` output frames to
 * `capture` (frames x channels; may be NULL to drop them). The calling thread is the audio thread
 * for the duration. */
void de__audio_null_pump(de_audio *a, u32 frames, f32 *capture);
/** Simulated hot-plug on the null backend: add a device (optionally as the new default), remove
 * one, or change the default. They notify like an OS would. DE_ERR_UNSUPPORTED on another
 * backend, DE_ERR_NOT_FOUND / DE_ERR_ALREADY_EXISTS / DE_ERR_OUT_OF_MEMORY otherwise. */
de_result de__audio_null_add_device(de_audio *a, const char *id, const char *name,
                                    bool make_default);
de_result de__audio_null_remove_device(de_audio *a, const char *id);
de_result de__audio_null_set_default(de_audio *a, const char *id);
/** Frames the null realtime stream has consumed since create (all its streams). */
u64 de__audio_null_frames_played(const de_audio *a);

/* ---- rt_guard.c ------------------------------------------------------------------------------ */

/** True when the guard is compiled in (Debug and Dev builds). */
bool de__rt_guard_enabled(void);
/** Marks the calling thread as an audio thread (nests). */
void de__rt_guard_enter(void);
void de__rt_guard_leave(void);
bool de__rt_guard_active(void);
/** Installs the allocation and lock hooks (counted: the first install wins, the last remove
 * uninstalls). Not for audio threads. */
void de__rt_guard_install(void);
void de__rt_guard_uninstall(void);
/** Allocations, frees and locks seen on audio threads since the process started. */
u64 de__rt_guard_hits(void);
/** true (default): a hit panics with its kind. false: hits are only counted (tests). */
void de__rt_guard_set_fatal(bool fatal);

/* ---- sound_asset.c (stub; DENG-313 owns it) -------------------------------------------------- */

/** Opens a source for `sound` (caller's thread; may allocate). DE_ERR_UNSUPPORTED until DENG-313.
 * The signature is stable: DENG-313 keeps it. */
de_result de__sound_source_open(de_audio *a, de_sound sound, de__audio_source **out);

/* DENG-312 voice graph. */
void de__audio_ramp_set(de__audio_ramp *r, f32 target, u32 samples);
f32 de__audio_ramp_next(de__audio_ramp *r);
void de__audio_channel_mix(f32 *out, u32 out_channels, const f32 *in, u32 in_channels, f32 gain);
de_result de__voice_storage_init(de_audio *a);
void de__voice_storage_free(de_audio *a);
void de__voice_close_tails(de_audio *a);
u32 de__voice_collect_tails(de_audio *a);
void de__voice_on_control(de_audio *a, const de__audio_cmd *cmd);
void de__bus_on_control(de_audio *a, const de__audio_cmd *cmd);
void de__bus_mix(de_audio *a, f32 *out, u32 frames);
void de__stream_service(de_audio *a);
void de__stream_shutdown(de_audio *a);
#endif /* DE_AUDIO_INTERNAL_H */
