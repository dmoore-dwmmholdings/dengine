/*
 * source.h: the internal audio source interface (DENG-308). Include as "audio/source.h".
 *
 * A source produces interleaved float32 frames for one voice. DENG-313 builds the real sources
 * (in-memory PCM/ADPCM, streamed Vorbis and Opus, procedural callbacks) behind this interface;
 * the mixer and the voices (voice.c, DENG-312) only call it through the vtable. Tests implement
 * their own sources.
 *
 * Ownership and threads:
 *   - Something outside the audio thread creates the source (de__sound_source_open in
 *     sound_asset.c, or a test) and hands it to a voice (de__voice_play_source). From then on the
 *     voice owns it.
 *   - `read` and `seek` run on the audio thread only, so they must not allocate, lock, log or
 *     wait for I/O. A streaming source reads ahead on another thread and only copies here.
 *   - `close` runs once, never on the audio thread: on the device control thread (after the voice
 *     ended or was stopped), on a thread that calls de_audio_play (which collects ended voices when
 *     it needs a slot), or in de_audio_destroy. It may allocate, lock and free.
 *
 * Sources embed `de__audio_source` as their first member.
 */
#ifndef DE_AUDIO_SOURCE_H
#define DE_AUDIO_SOURCE_H

#include <dengine/de_base.h>

typedef struct de__audio_source de__audio_source;

/** Flags of de__audio_source.flags. */
typedef enum de__audio_source_flags {
    DE__SOURCE_SEEKABLE = 1u << 0,  /**< seek works (loops need it) */
    DE__SOURCE_STREAMING = 1u << 1, /**< decodes ahead on another thread (DENG-313) */
    DE__SOURCE_ENDLESS = 1u << 2,   /**< never ends by itself (procedural, test tones) */
} de__audio_source_flags;

typedef struct de__audio_source_vtbl {
    /**
     * Audio thread. Writes up to `frames` frames of `channels` interleaved samples to `out` and
     * returns how many it wrote. `channels` is always the source's own `channels`: it is the
     * stride of `out`. Fewer frames than asked means the source has ended (the voice then loops
     * with `seek(0)` or ends). Never allocates, locks or blocks.
     */
    u32 (*read)(de__audio_source *s, f32 *out, u32 frames, u32 channels);
    /** Audio thread. Moves the read position to frame `frame`. Returns false when the source
     * cannot seek (a looping voice then ends). May be NULL for sources that cannot seek. */
    bool (*seek)(de__audio_source *s, u64 frame);
    /** Not the audio thread. Releases the source. May be NULL (the caller owns the memory). */
    void (*close)(de__audio_source *s);
} de__audio_source_vtbl;

struct de__audio_source {
    const de__audio_source_vtbl *vt;
    u32 channels; /**< 1 .. DE__AUDIO_MAX_SOURCE_CHANNELS */
    u32 rate;     /**< Hz. The S25 core does not resample: the voice reads at the mixer rate
                       (the resampler comes with DENG-312) */
    u32 flags;    /**< de__audio_source_flags */
    u32 reserved_;
    u64 length_frames; /**< total length, or 0 when unknown or endless */
};

#endif /* DE_AUDIO_SOURCE_H */
