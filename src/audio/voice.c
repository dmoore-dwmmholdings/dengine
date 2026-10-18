/* DENG-312: voice graph, ramps, rate conversion and stealing. Callers reserve handles
 * through atomics and queue commands. The audio thread owns voice/bus state; stolen sources
 * fade in preallocated tail slots and are closed by the control thread. No mixer allocations. */
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
    if (desc && (!(desc->volume >= 0) || desc->volume > 1000 || !(desc->pitch >= 0) ||
                 desc->pitch > 8 || !(desc->fade_in >= 0) || desc->fade_in > 3600)) {
        close_source(src);
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "invalid audio play parameters");
        return none;
    }
    u32 slot, gen;
    bool stolen = false;
    if (!de__audio_slot_alloc(a, &slot, &gen)) {
        u32 chosen = UINT32_MAX, priority = 0;
        u64 oldest = UINT64_MAX;
        for (u32 i = 0; i < a->voice_count; ++i) {
            if (de_atomic_load_u32(&a->reservations[i].busy, DE_ATOMIC_ACQUIRE))
                continue;
            u32 p = de_atomic_load_u32(&a->reservations[i].priority, DE_ATOMIC_RELAXED);
            u64 age = de_atomic_load_u64(&a->reservations[i].age, DE_ATOMIC_RELAXED);
            if (chosen == UINT32_MAX || p > priority || (p == priority && age < oldest)) {
                chosen = i;
                priority = p;
                oldest = age;
            }
        }
        if (chosen != UINT32_MAX) {
            u32 expected = 0;
            if (de_atomic_cas_u32(&a->reservations[chosen].busy, &expected, 1, DE_ATOMIC_ACQ_REL,
                                  DE_ATOMIC_ACQUIRE)) {
                slot = chosen;
                u32 old = de_atomic_load_u32(&a->voice_gen[slot], DE_ATOMIC_ACQUIRE);
                gen = de__voice_next_gen(old);
                stolen = de_atomic_cas_u32(&a->voice_gen[slot], &old, gen, DE_ATOMIC_ACQ_REL,
                                           DE_ATOMIC_ACQUIRE);
                if (!stolen)
                    de_atomic_store_u32(&a->reservations[slot].busy, 0, DE_ATOMIC_RELEASE);
            }
        }
        if (!stolen) {
            close_source(src);
            DE_FAIL(DE_ERR_BUSY, "audio: no stealable voice");
            return none;
        }
    }
    de_atomic_store_u32(&a->reservations[slot].priority, desc ? desc->priority : 0,
                        DE_ATOMIC_RELAXED);
    de_atomic_store_u64(&a->reservations[slot].age,
                        de_atomic_fetch_add_u64(&a->play_sequence, 1, DE_ATOMIC_RELAXED),
                        DE_ATOMIC_RELAXED);
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
    c.u.play.fade = desc ? desc->fade_in : 0;
    c.u.play.spatial = desc && desc->spatial;
    c.u.play.position = desc ? desc->position : (de_dvec3){0, 0, 0};
    if (!de__audio_cmd_push(a, &c)) {
        if (!stolen)
            de__audio_slot_release(a, slot);
        else {
            u32 expected = gen;
            u32 prior = gen == 1 ? DE__VOICE_GEN_MASK : gen - 1;
            (void)de_atomic_cas_u32(&a->voice_gen[slot], &expected, prior, DE_ATOMIC_ACQ_REL,
                                    DE_ATOMIC_ACQUIRE);
            de_atomic_store_u32(&a->reservations[slot].busy, 0, DE_ATOMIC_RELEASE);
        }
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
    c.u.stop.fade = -1; /* private sample-accurate immediate stop, used by scheduling tests */
    de__audio_cmd_push(a, &c);
}

void de_audio_stop(de_audio *a, de_voice v, f32 fade_seconds) {
    if (!a || !de_audio_playing(a, v) || !(fade_seconds >= 0) || fade_seconds > 3600)
        return;
    de__audio_cmd c = {0};
    c.op = DE__ACMD_STOP;
    c.voice = v.id;
    c.u.stop.fade = fade_seconds;
    (void)de__audio_cmd_push(a, &c);
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
    u32 slot = de__voice_slot(h);
    if (v->state != DE__VOICE_FREE) {
        de__voice *tail = &a->tails[slot];
        de__resampler *spare = tail->resampler;
        *tail = *v;
        tail->stopping = true;
        de__audio_ramp_set(&tail->gain, 0, a->sample_rate / 200);
        if (v->state == DE__VOICE_PLAYING) {
            u32 last = a->active[--a->active_count];
            a->active[v->active_index] = last;
            a->voices[last].active_index = v->active_index;
        }
        v->resampler = spare;
    }
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
    v->stopping = false;
    v->source_eof = false;
    v->spatial = cmd->u.play.spatial;
    v->position = cmd->u.play.position;
    v->velocity = (de_vec3){0, 0, 0};
    v->gain.value = cmd->u.play.fade > 0 ? 0 : v->volume;
    v->gain.target = v->gain.value;
    v->gain.remaining = 0;
    de__audio_ramp_set(&v->gain, v->volume, (u32)(cmd->u.play.fade * a->sample_rate));
    v->resampling = v->src->rate != a->sample_rate || v->pitch != 1;
    f64 ratio = (f64)(v->src->rate ? v->src->rate : a->sample_rate) / a->sample_rate * v->pitch;
    de__resample_format(v->resampler, v->src->channels, ratio);
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
    if (v->state == DE__VOICE_SCHEDULED || cmd->u.stop.fade < 0) {
        de__voice_retire(a, slot);
        return;
    }
    u32 frames = (u32)(cmd->u.stop.fade * a->sample_rate), minimum = a->sample_rate / 200;
    de__audio_ramp_set(&v->gain, 0, frames > minimum ? frames : minimum);
    v->stopping = true;
}

void de__voice_retire(de_audio *a, u32 slot) {
    de__voice *v = &a->voices[slot];
    u32 expected = v->gen;
    if (!de_atomic_cas_u32(&a->voice_gen[slot], &expected, de__voice_next_gen(v->gen),
                           DE_ATOMIC_ACQ_REL, DE_ATOMIC_ACQUIRE))
        return; /* A caller reserved this slot for a steal; queued PLAY takes ownership. */
    if (v->state == DE__VOICE_PLAYING) {
        /* Swap-remove from the active list. */
        u32 last = a->active[--a->active_count];
        a->active[v->active_index] = last;
        a->voices[last].active_index = v->active_index;
    }
    v->state = DE__VOICE_FREE;
    /* The handle goes stale now; the slot itself is free only after the control thread closed
     * the source. Release: the source pointer in `v` is visible to whoever pops `retired`. */
    /* Generation was changed atomically before removing audio-thread state. */
    bool ok = de_mpmc_try_push(&a->retired, &slot);
    DE_ASSERT(ok); /* capacity >= slot count, and a slot is retired at most once per use */
    (void)ok;
    a->retired_this_block = true;
}

/* The resampler reads ahead into its preallocated ring. Source callbacks remain RT safe. */
static u32 read_voice(de_audio *a, de__voice *v, f32 *out, u32 frames) {
    de__audio_source *s = v->src;
    u32 channels = s->channels;
    if (!v->resampling) {
        u32 n = s->vt->read(s, out, frames, channels);
        if (n > frames)
            n = frames;
        if (n < frames && v->loop && s->vt->seek && s->vt->seek(s, 0))
            n += s->vt->read(s, out + (size_t)n * channels, frames - n, channels);
        v->frames_played += n;
        return n;
    }
    u32 done = 0;
    bool restarted = false;
    while (done < frames) {
        u32 got = de__resample_read(v->resampler, out + (size_t)done * channels, frames - done);
        done += got;
        if (done == frames || v->source_eof)
            break;
        u32 n = s->vt->read(s, a->src_buf, a->block_frames, channels);
        if (n > a->block_frames)
            n = a->block_frames;
        if (n) {
            (void)de__resample_push(v->resampler, a->src_buf, n);
            v->frames_played += n;
            restarted = false;
        }
        if (n < a->block_frames) {
            if (v->loop && !restarted && s->vt->seek && s->vt->seek(s, 0)) {
                restarted = true;
                continue;
            }
            v->source_eof = true;
            de__resample_finish(v->resampler);
            done += de__resample_read(v->resampler, out + (size_t)done * channels, frames - done);
            break;
        }
    }
    return done;
}
static bool mix_voice(de_audio *a, de__voice *v, f32 *out, u32 frames) {
    u32 n = read_voice(a, v, a->resample_buf, frames);
    for (u32 f = 0; f < n; ++f) {
        if (v->stopping && v->gain.remaining == 0)
            return false;
        f32 gain = de__audio_ramp_next(&v->gain);
        de__audio_channel_mix(out + (size_t)f * a->channels, a->channels,
                              a->resample_buf + (size_t)f * v->src->channels, v->src->channels,
                              gain);
    }
    return n == frames && !(v->stopping && v->gain.remaining == 0);
}
void de__voice_mix_all(de_audio *a, f32 *out, u32 frames, u64 sample) {
    (void)sample;
    for (u32 id = 0; id < 64; ++id)
        memset(a->bus_buf + (size_t)id * a->block_frames * a->channels, 0,
               (size_t)frames * a->channels * 4);
    for (u32 i = a->active_count; i-- > 0;) {
        u32 slot = a->active[i];
        de__voice *v = &a->voices[slot];
        u32 bus = v->bus ? v->bus - 1 : 0;
        if (bus >= 64 || !a->buses[bus].active)
            bus = 0;
        if (!mix_voice(a, v, a->bus_buf + (size_t)bus * a->block_frames * a->channels, frames))
            de__voice_retire(a, slot);
    }
    for (u32 i = 0; i < a->voice_count; ++i) {
        de__voice *v = &a->tails[i];
        if (v->state == DE__VOICE_FREE)
            continue;
        u32 bus = v->bus ? v->bus - 1 : 0;
        if (bus >= 64 || !a->buses[bus].active)
            bus = 0;
        if (!mix_voice(a, v, a->bus_buf + (size_t)bus * a->block_frames * a->channels, frames)) {
            v->state = DE__VOICE_FREE;
            bool ok = de_mpmc_try_push(&a->retired_tails, &i);
            DE_ASSERT(ok);
            (void)ok;
            a->retired_this_block = true;
        }
    }
    de__bus_mix(a, out, frames);
}
void de_audio_set_volume(de_audio *a, de_voice v, f32 gain, f32 fade) {
    if (!a || !de_audio_playing(a, v) || !(gain >= 0) || gain > 1000 || !(fade >= 0) || fade > 3600)
        return;
    de__audio_cmd c = {0};
    c.op = DE__ACMD_VOLUME;
    c.voice = v.id;
    c.u.param.value = gain;
    c.u.param.fade = fade;
    (void)de__audio_cmd_push(a, &c);
}
void de_audio_set_pitch(de_audio *a, de_voice v, f32 pitch) {
    if (!a || !de_audio_playing(a, v) || !(pitch >= 0.125f && pitch <= 8))
        return;
    de__audio_cmd c = {0};
    c.op = DE__ACMD_PITCH;
    c.voice = v.id;
    c.u.param.value = pitch;
    (void)de__audio_cmd_push(a, &c);
}
void de_audio_set_position(de_audio *a, de_voice v, de_dvec3 pos, de_vec3 velocity) {
    if (!a || !de_audio_playing(a, v))
        return;
    de__audio_cmd c = {0};
    c.op = DE__ACMD_POSITION;
    c.voice = v.id;
    c.u.param.position = pos;
    c.u.param.velocity = velocity;
    (void)de__audio_cmd_push(a, &c);
}
void de__voice_on_control(de_audio *a, const de__audio_cmd *c) {
    de_voice h = {c->voice};
    u32 slot = de__voice_slot(h);
    if (slot >= a->voice_count)
        return;
    de__voice *v = &a->voices[slot];
    if (v->gen != de__voice_gen(h) || v->state == DE__VOICE_FREE)
        return;
    if (c->op == DE__ACMD_VOLUME) {
        u32 n = (u32)(c->u.param.fade * a->sample_rate);
        u32 minimum = a->sample_rate / 200;
        de__audio_ramp_set(&v->gain, c->u.param.value, n > minimum ? n : minimum);
        v->volume = c->u.param.value;
    } else if (c->op == DE__ACMD_POSITION) {
        v->position = c->u.param.position;
        v->velocity = c->u.param.velocity;
    } else {
        v->pitch = c->u.param.value;
        f64 ratio = (f64)v->src->rate / a->sample_rate * v->pitch;
        if (!v->resampling) {
            v->resampling = true;
            de__resample_format(v->resampler, v->src->channels, ratio);
        }
        de__resample_ratio(v->resampler, ratio, a->sample_rate / 200);
    }
}

de_result de__voice_storage_init(de_audio *a) {
    const de_allocator *al = &a->alloc;
    u32 n = a->voice_count;
    a->tails = DE_ALLOC(al, sizeof(de__voice) * n, DE_ALIGNOF(de__voice), DE_MEM_AUDIO);
    if (a->tails)
        memset(a->tails, 0, sizeof(de__voice) * n);
    a->reservations = DE_ALLOC(al, sizeof(de__voice_reservation) * n,
                               DE_ALIGNOF(de__voice_reservation), DE_MEM_AUDIO);
    a->resample_buf = DE_ALLOC(al, (size_t)a->block_frames * 8 * 4, 16, DE_MEM_AUDIO);
    a->bus_buf = DE_ALLOC(al, (size_t)a->block_frames * a->channels * 64 * 4, 16, DE_MEM_AUDIO);
    if (!a->tails || !a->reservations || !a->resample_buf || !a->bus_buf)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "audio voice graph");
    de_mpmc_desc q = {0};
    q.allocator = al;
    q.tag = DE_MEM_AUDIO;
    q.elem_size = sizeof(u32);
    q.capacity = 1;
    while (q.capacity < n)
        q.capacity *= 2;
    DE_TRY(de_mpmc_init(&a->retired_tails, &q));
    DE_TRY(de__resample_create(al, 1, 1, &a->resample_bank));
    for (u32 i = 0; i < n; ++i) {
        de_atomic_init_u32(&a->reservations[i].priority, 0);
        de_atomic_init_u32(&a->reservations[i].busy, 0);
        de_atomic_init_u64(&a->reservations[i].age, 0);
        DE_TRY(de__resample_create_shared(al, 8, 1, a->resample_bank, &a->voices[i].resampler));
        DE_TRY(de__resample_create_shared(al, 8, 1, a->resample_bank, &a->tails[i].resampler));
    }
    de_atomic_init_u64(&a->play_sequence, 1);
    de_atomic_init_u32(&a->bus_count, 1);
    a->buses[0].active = true;
    a->buses[0].gain.value = a->buses[0].gain.target = 1;
    return DE_OK;
}
void de__voice_storage_free(de_audio *a) {
    const de_allocator *al = &a->alloc;
    for (u32 i = 0; i < a->voice_count; ++i) {
        if (a->voices)
            de__resample_destroy(a->voices[i].resampler);
        if (a->tails)
            de__resample_destroy(a->tails[i].resampler);
    }
    de__resample_destroy(a->resample_bank);
    if (a->tails)
        DE_FREE(al, a->tails, sizeof(de__voice) * a->voice_count);
    if (a->reservations)
        DE_FREE(al, a->reservations, sizeof(de__voice_reservation) * a->voice_count);
    if (a->resample_buf)
        DE_FREE(al, a->resample_buf, (size_t)a->block_frames * 8 * 4);
    if (a->bus_buf)
        DE_FREE(al, a->bus_buf, (size_t)a->block_frames * a->channels * 64 * 4);
    de_mpmc_destroy(&a->retired_tails);
}
u32 de__voice_collect_tails(de_audio *a) {
    if (!a->retired_tails.cells)
        return 0;
    u32 slot, n = 0;
    while (de_mpmc_try_pop(&a->retired_tails, &slot)) {
        de__audio_source *src = a->tails[slot].src;
        a->tails[slot].src = NULL;
        close_source(src);
        de_atomic_store_u32(&a->reservations[slot].busy, 0, DE_ATOMIC_RELEASE);
        ++n;
    }
    return n;
}
void de__voice_close_tails(de_audio *a) {
    if (!a->tails)
        return;
    for (u32 i = 0; i < a->voice_count; ++i) {
        close_source(a->tails[i].src);
        a->tails[i].src = NULL;
    }
}
