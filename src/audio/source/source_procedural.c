#include "audio/audio_internal.h"
#include <string.h>
typedef struct procedural_source {
    de__audio_source base;
    de_audio *audio;
    de_audio_gen_fn fn;
    void *user;
} procedural_source;
static u32 read(de__audio_source *base, f32 *out, u32 frames, u32 channels) {
    procedural_source *s = (procedural_source *)base;
    s->fn(out, frames, channels, s->user);
    return frames;
}
static void close(de__audio_source *base) {
    procedural_source *s = (procedural_source *)base;
    DE_FREE(&s->audio->alloc, s, sizeof *s);
}
static const de__audio_source_vtbl vt = {read, NULL, close};
de_voice de_audio_play_procedural(de_audio *a, de_audio_gen_fn fn, void *user, u32 channels,
                                  de_bus bus) {
    if (!a || !fn || channels < 1 || channels > 8) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "invalid procedural audio source");
        return (de_voice){0};
    }
    procedural_source *s =
        DE_ALLOC(&a->alloc, sizeof *s, DE_ALIGNOF(procedural_source), DE_MEM_AUDIO);
    if (!s)
        return (de_voice){0};
    memset(s, 0, sizeof *s);
    s->base.vt = &vt;
    s->base.channels = channels;
    s->base.rate = a->sample_rate;
    s->base.flags = DE__SOURCE_ENDLESS;
    s->audio = a;
    s->fn = fn;
    s->user = user;
    de_play_desc d = {0};
    d.bus = bus;
    return de__voice_play_source(a, &s->base, &d);
}
