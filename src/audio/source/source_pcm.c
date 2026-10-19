#include "audio/source/stream_internal.h"
typedef struct resident_source {
    de__audio_source base;
    de_audio *audio;
    de__sound_asset *asset;
    u64 frame, cached_block;
    bool cache_valid;
    f32 decoded[1017 * 8];
} resident_source;
static f32 sample(resident_source *s, u64 frame, u32 c) {
    const de__sound_view *v = &s->asset->view;
    if (v->codec == DE__DSND_PCM_F32) {
        u32 bits = de__snd_u32(v->data + (frame * v->channels + c) * 4);
        f32 value;
        memcpy(&value, &bits, 4);
        return value;
    }
    if (v->codec == DE__DSND_PCM16) {
        const u8 *p = v->data + (frame * v->channels + c) * 2;
        return (i16)((u16)p[0] | (u16)p[1] << 8) / 32768.0f;
    }
    u64 block = frame / 1017;
    if (!s->cache_valid || s->cached_block != block) {
        for (u32 ch = 0; ch < v->channels; ++ch) {
            const u8 *p = v->data + (block * v->channels + ch) * 512;
            i32 predictor = (i16)((u16)p[0] | (u16)p[1] << 8), index = p[2];
            s->decoded[ch] = predictor / 32768.0f;
            for (u32 i = 1; i < 1017; ++i) {
                u32 at = i - 1;
                (void)de__ima_decode((p[4 + at / 2] >> ((at & 1) * 4)) & 15, &predictor, &index);
                s->decoded[i * v->channels + ch] = predictor / 32768.0f;
            }
        }
        s->cached_block = block;
        s->cache_valid = true;
    }
    return s->decoded[(frame % 1017) * v->channels + c];
}
static u32 read(de__audio_source *base, f32 *out, u32 frames, u32 channels) {
    resident_source *s = (resident_source *)base;
    const de__sound_view *v = &s->asset->view;
    u32 n = 0;
    bool loop = de_atomic_load_u32(&base->loop_enabled, DE_ATOMIC_RELAXED) != 0;
    u64 end = loop && (v->flags & DE__DSND_LOOP) ? v->loop_end : v->frames;
    while (n < frames) {
        if (s->frame >= end) {
            if (!loop)
                break;
            s->frame = (v->flags & DE__DSND_LOOP) ? v->loop_start : 0;
        }
        for (u32 c = 0; c < channels; ++c)
            out[(size_t)n * channels + c] = sample(s, s->frame, c);
        ++n;
        ++s->frame;
    }
    return n;
}
static bool seek(de__audio_source *base, u64 frame) {
    resident_source *s = (resident_source *)base;
    if (frame > s->asset->view.frames)
        return false;
    s->frame = frame;
    return true;
}
static void close(de__audio_source *base) {
    resident_source *s = (resident_source *)base;
    const de_allocator *a = &s->audio->alloc;
    de__sound_asset_release(s->asset);
    DE_FREE(a, s, sizeof *s);
}
static const de__audio_source_vtbl vt = {read, seek, close};
de_result de__source_resident(de_audio *a, de__sound_asset *asset, de__audio_source **out) {
    resident_source *s = DE_ALLOC(&a->alloc, sizeof *s, DE_ALIGNOF(resident_source), DE_MEM_AUDIO);
    if (!s)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "resident sound source");
    memset(s, 0, sizeof *s);
    s->base.vt = &vt;
    s->base.channels = asset->view.channels;
    s->base.rate = asset->view.rate;
    s->base.length_frames = asset->view.frames;
    s->base.flags = DE__SOURCE_SEEKABLE | DE__SOURCE_LOOP_AWARE;
    de_atomic_init_u32(&s->base.loop_enabled, 0);
    de_atomic_init_u32(&s->base.loop_enabled, 0);
    s->audio = a;
    s->asset = asset;
    de__sound_asset_ref(asset);
    *out = &s->base;
    return DE_OK;
}
