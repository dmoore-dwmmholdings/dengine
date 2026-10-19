#include "audio/audio_internal.h"
#include <string.h>
static u32 id(de_bus b) {
    return b.id ? b.id - 1 : 0;
}
de_bus de_audio_master(de_audio *a) {
    return (de_bus){a ? 1u : 0u};
}
de_bus de_audio_bus_create(de_audio *a, const char *name, de_bus parent) {
    if (!a || !name || !name[0] || strlen(name) >= 64)
        return (de_bus){0};
    u32 count = de_atomic_load_u32(&a->bus_count, DE_ATOMIC_ACQUIRE), p = id(parent);
    if (p >= count)
        return (de_bus){0};
    while (count < 64 && !de_atomic_cas_u32(&a->bus_count, &count, count + 1, DE_ATOMIC_ACQ_REL,
                                            DE_ATOMIC_ACQUIRE)) {
    }
    if (count >= 64)
        return (de_bus){0};
    /* Unique reserved slot; only this creator writes the name, never read by the mixer. */
    memcpy(a->buses[count].name, name, strlen(name) + 1);
    de__audio_cmd c = {0};
    c.op = DE__ACMD_BUS_CREATE;
    c.voice = count;
    c.u.param.parent = p;
    if (!de__audio_cmd_push(a, &c))
        return (de_bus){0};
    return (de_bus){count + 1};
}
void de_audio_bus_volume(de_audio *a, de_bus b, f32 gain, f32 fade) {
    if (!a || !(gain >= 0) || gain > 1000 || !(fade >= 0) || fade > 3600)
        return;
    de__audio_cmd c = {0};
    c.op = DE__ACMD_BUS_VOLUME;
    c.voice = id(b);
    c.u.param.value = gain;
    c.u.param.fade = fade;
    (void)de__audio_cmd_push(a, &c);
}
void de_audio_bus_mute(de_audio *a, de_bus b, bool mute) {
    if (!a)
        return;
    de__audio_cmd c = {0};
    c.op = DE__ACMD_BUS_MUTE;
    c.voice = id(b);
    c.u.param.value = mute ? 1.f : 0.f;
    (void)de__audio_cmd_push(a, &c);
}
void de__bus_on_control(de_audio *a, const de__audio_cmd *c) {
    if (c->voice >= 64)
        return;
    de__audio_bus *b = &a->buses[c->voice];
    if (c->op == DE__ACMD_BUS_CREATE) {
        b->active = true;
        b->parent = c->u.param.parent;
        b->gain.value = b->gain.target = 1;
    } else if (c->op == DE__ACMD_BUS_MUTE)
        b->mute = c->u.param.value != 0;
    else {
        u32 samples = (u32)(c->u.param.fade * a->sample_rate);
        u32 minimum = a->sample_rate / 200;
        de__audio_ramp_set(&b->gain, c->u.param.value, samples > minimum ? samples : minimum);
    }
}
void de__bus_mix(de_audio *a, f32 *out, u32 frames) {
    /* Parents are allocated before children; reverse ID order is a stable postorder. */
    for (u32 id = 64; id-- > 0;) {
        de__audio_bus *b = &a->buses[id];
        if (!b->active)
            continue;
        f32 *source = a->bus_buf + (size_t)id * a->block_frames * a->channels;
        f32 *dest = id ? a->bus_buf + (size_t)b->parent * a->block_frames * a->channels : out;
        for (u32 f = 0; f < frames; ++f) {
            f32 gain = de__audio_ramp_next(&b->gain);
            if (b->mute)
                gain = 0;
            for (u32 c = 0; c < a->channels; ++c)
                dest[f * a->channels + c] += source[f * a->channels + c] * gain;
        }
    }
}
