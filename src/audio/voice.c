/* voice.c: minimal voices (DENG-308). DENG-312 takes this file over (buses, volume, pitch,
 * fades, stealing); keep de__voice_play_source and the command handlers' contracts.
 *
 * S25 core behaviour: a voice reads its source at the mixer rate and adds it to the master at
 * unity gain. Mono goes to both channels, stereo maps straight through, and a source with more
 * channels contributes its first two (channel maps come with DENG-312). A looping voice seeks back
 * to frame 0 when the source ends. A voice ends when its source ends (not looping), when it is
 * stopped, or when a looping source cannot seek.
 *
 * Caller side: de_audio_play / de__voice_play_source take a slot and its generation, and push a
 * PLAY command that carries the source. If the push fails the caller gives the slot back and
 * closes the source itself. Audio side: the PLAY command marks the slot SCHEDULED when it is
 * popped (so a STOP can cancel a voice that waits for its start sample) and PLAYING at its
 * sample. Ending a voice bumps the slot's generation (de_audio_playing turns false at once) and
 * hands the slot to the control thread, which closes the source and frees the slot.
 */
#include "audio/audio_internal.h"

#include <string.h>

/* ---- Caller side ----------------------------------------------------------------------------- */

static void close_source(de__audio_source *src) {
    if (src != NULL && src->vt != NULL && src->vt->close != NULL)
        src->vt->close(src);
}

de_voice de__voice_play_source(de_audio *a, de__audio_source *src, const de_play_desc *desc) {
    de_voice none = {0};
    if (a == NULL || src == NULL || src->vt == NULL || src->vt->read == NULL ||
        src->channels == 0 || src->channels > DE__AUDIO_MAX_SOURCE_CHANNELS) {
        close_source(src);
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "audio: invalid source (channels 1..%u, a read function)",
                DE__AUDIO_MAX_SOURCE_CHANNELS);
        return none;
    }
    u32 slot, gen;
    if (!de__audio_slot_alloc(a, &slot, &gen)) {
        close_source(src);
        DE_FAIL(DE_ERR_BUSY, "audio: all %u voices are in use", a->voice_count);
        return none;
    }
    de__audio_cmd c;
    memset(&c, 0, sizeof c);
    c.op = DE__ACMD_PLAY;
    c.voice = de__voice_make(slot, gen).id;
    c.at_sample = desc != NULL ? desc->start_at_sample : 0;
    c.u.play.src = src;
    c.u.play.bus = desc != NULL ? desc->bus.id : 0;
    c.u.play.volume = desc != NULL && desc->volume != 0.0f ? desc->volume : 1.0f;
    c.u.play.pitch = desc != NULL && desc->pitch != 0.0f ? desc->pitch : 1.0f;
    c.u.play.loop = desc != NULL && desc->loop ? 1 : 0;
    c.u.play.priority = desc != NULL ? desc->priority : 0;
    if (!de__audio_cmd_push(a, &c)) {
        de__audio_slot_release(a, slot);
        close_source(src);
        DE_FAIL(DE_ERR_BUSY, "audio: command queue full, play dropped");
        return none;
    }
    return de__voice_make(slot, gen);
}

de_voice de_audio_play(de_audio *a, const de_play_desc *desc) {
    de_voice none = {0};
    if (a == NULL || desc == NULL) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_audio_play: a and desc are required");
        return none;
    }
    de__audio_source *src = NULL;
    de_result r = de__sound_source_open(a, desc->sound, &src);
    if (r != DE_OK || src == NULL)
        return none; /* error context set by the opener */
    return de__voice_play_source(a, src, desc);
}

void de__voice_stop_at(de_audio *a, de_voice v, u64 at_sample) {
    if (a == NULL || !de_audio_playing(a, v))
        return;
    de__audio_cmd c;
    memset(&c, 0, sizeof c);
    c.op = DE__ACMD_STOP;
    c.voice = v.id;
    c.at_sample = at_sample;
    de__audio_cmd_push(a, &c);
}

void de_audio_stop(de_audio *a, de_voice v, f32 fade_seconds) {
    (void)fade_seconds; /* fades: DENG-312 */
    de__voice_stop_at(a, v, 0);
}

bool de_audio_playing(de_audio *a, de_voice v) {
    if (a == NULL || !DE_HANDLE_VALID(v))
        return false;
    u32 slot = de__voice_slot(v);
    if (slot >= a->voice_count)
        return false;
    return de_atomic_load_u32(&a->voice_gen[slot], DE_ATOMIC_ACQUIRE) == de__voice_gen(v);
}

/* ---- Audio thread ---------------------------------------------------------------------------- */

void de__voice_on_play_queued(de_audio *a, const de__audio_cmd *cmd) {
    de_voice h = {cmd->voice};
    de__voice *v = &a->voices[de__voice_slot(h)];
    v->state = DE__VOICE_SCHEDULED;
    v->gen = de__voice_gen(h);
    v->src = cmd->u.play.src;
    v->bus = cmd->u.play.bus;
    v->volume = cmd->u.play.volume;
    v->pitch = cmd->u.play.pitch;
    v->loop = cmd->u.play.loop;
    v->priority = cmd->u.play.priority;
    v->frames_played = 0;
    v->start_sample = 0;
}

void de__voice_on_play(de_audio *a, const de__audio_cmd *cmd, u64 sample) {
    de_voice h = {cmd->voice};
    u32 slot = de__voice_slot(h);
    de__voice *v = &a->voices[slot];
    /* A stop that came first ended the scheduled voice: the slot has moved on. */
    if (v->gen != de__voice_gen(h) || v->state != DE__VOICE_SCHEDULED)
        return;
    v->state = DE__VOICE_PLAYING;
    v->start_sample = sample;
    v->active_index = a->active_count;
    a->active[a->active_count++] = slot;
}

void de__voice_on_stop(de_audio *a, const de__audio_cmd *cmd) {
    de_voice h = {cmd->voice};
    u32 slot = de__voice_slot(h);
    if (slot >= a->voice_count)
        return;
    de__voice *v = &a->voices[slot];
    if (v->gen != de__voice_gen(h) || v->state == DE__VOICE_FREE)
        return;
    de__voice_retire(a, slot);
}

void de__voice_retire(de_audio *a, u32 slot) {
    de__voice *v = &a->voices[slot];
    if (v->state == DE__VOICE_PLAYING) {
        /* Swap-remove from the active list. */
        u32 last = a->active[--a->active_count];
        a->active[v->active_index] = last;
        a->voices[last].active_index = v->active_index;
    }
    v->state = DE__VOICE_FREE;
    /* The handle goes stale now; the slot itself is free only after the control thread closed
     * the source. Release: the source pointer in `v` is visible to whoever pops `retired`. */
    de_atomic_store_u32(&a->voice_gen[slot], de__voice_next_gen(v->gen), DE_ATOMIC_RELEASE);
    bool ok = de_mpmc_try_push(&a->retired, &slot);
    DE_ASSERT(ok); /* capacity >= slot count, and a slot is retired at most once per use */
    (void)ok;
    a->retired_this_block = true;
}

/* Adds `frames` frames of `src` (sch channels, interleaved) to `out` (stereo). */
static void mix_into(f32 *out, const f32 *src, u32 frames, u32 sch) {
    if (sch == 1) {
        for (u32 i = 0; i < frames; ++i) {
            out[2 * i] += src[i];
            out[2 * i + 1] += src[i];
        }
    } else {
        for (u32 i = 0; i < frames; ++i) {
            out[2 * i] += src[(size_t)i * sch];
            out[2 * i + 1] += src[(size_t)i * sch + 1];
        }
    }
}

/* Mixes one voice; returns false when it ended inside this range. */
static bool mix_voice(de_audio *a, de__voice *v, f32 *out, u32 frames) {
    de__audio_source *src = v->src;
    const u32 sch = src->channels;
    u32 done = 0;
    bool restarted = false; /* a loop that reads nothing after a seek would never end */
    while (done < frames) {
        u32 want = frames - done;
        u32 got = src->vt->read(src, a->src_buf, want, sch);
        if (got > want)
            got = want;
        if (got > 0) {
            mix_into(out + (size_t)done * DE__AUDIO_CHANNELS, a->src_buf, got, sch);
            done += got;
            v->frames_played += got;
            restarted = false;
        }
        if (got < want) {
            if (!v->loop || restarted || src->vt->seek == NULL || !src->vt->seek(src, 0))
                return false;
            restarted = true;
        }
    }
    return true;
}

void de__voice_mix_all(de_audio *a, f32 *out, u32 frames, u64 sample) {
    (void)sample;
    /* Backwards, so a swap-remove of voice i does not skip the voice moved into its place. */
    for (u32 i = a->active_count; i-- > 0;) {
        u32 slot = a->active[i];
        if (!mix_voice(a, &a->voices[slot], out, frames))
            de__voice_retire(a, slot);
    }
}
