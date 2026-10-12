/* mixer.c: renders one block (DENG-308; SPEC §14 "Mixer").
 *
 * A block is `block_frames` float32 stereo frames, rendered straight into the next free block of
 * the output ring. Before mixing, the mixer drains the command queue: commands that are due apply
 * at the block start, commands for a later sample wait in the sorted pending list. The block is
 * then cut at every pending command that falls inside it, so each command takes effect on its
 * exact sample: voices are mixed up to the cut, the commands at that sample apply, and mixing
 * goes on from there.
 *
 * Everything runs on the audio thread and touches only memory allocated at create.
 */
#include "audio/audio_internal.h"

#include <string.h>

/* Moving average of the render time per block time (about 32 blocks). */
#define DE__DSP_LOAD_ALPHA 0.03f

void de__mixer_render_block(de_audio *a) {
    const u64 t0 = de_time_now_ns();
    const u32 frames = a->block_frames;
    const u32 ch = a->channels;
    const u64 start = a->mix_pos;
    const u64 end = start + frames;
    f32 *out = de__audio_ring_write_ptr(a);
    memset(out, 0, (size_t)frames * ch * sizeof(f32));

    a->retired_this_block = false;
    /* Commands whose sample has come (or passed) apply at the block start, after any pending
     * command due there, in arrival order; later ones go to the pending list. */
    de__audio_pending_apply(a, start);
    de__audio_cmd_drain(a, start);

    u64 pos = start;
    while (pos < end) {
        u64 next = de__audio_pending_next(a);
        u64 stop = next < end ? next : end;
        if (stop > pos) {
            de__voice_mix_all(a, out + (size_t)(pos - start) * ch, (u32)(stop - pos), pos);
            pos = stop;
        }
        if (next == pos && pos < end)
            de__audio_pending_apply(a, pos);
    }

    a->mix_pos = end;
    /* Before the publish, so de_audio_clock() never runs ahead of frames_mixed. */
    de_atomic_store_u64(&a->frames_mixed, end, DE_ATOMIC_RELAXED);
    de__audio_ring_publish(a, start);
    de_atomic_store_u32(&a->voices_playing, a->active_count, DE_ATOMIC_RELAXED);
    if (a->retired_this_block)
        de__audio_device_wake(a);

    /* dsp_load: render time over the block's duration. */
    const u64 elapsed = de_time_now_ns() - t0;
    const f64 block_ns = (f64)frames * 1e9 / (f64)a->sample_rate;
    const f32 load = (f32)((f64)elapsed / block_ns);
    a->dsp_load += DE__DSP_LOAD_ALPHA * (load - a->dsp_load);
    u32 bits;
    memcpy(&bits, &a->dsp_load, sizeof bits);
    de_atomic_store_u32(&a->dsp_load_bits, bits, DE_ATOMIC_RELAXED);
}
