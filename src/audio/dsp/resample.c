#include "audio/dsp/resample.h"
#include <dengine/de_math.h>
#include <string.h>
#define HALF   96
#define TAPS   (HALF * 2)
#define PHASES 1024
#define RING   8192
struct de__resampler {
    const de_allocator *a;
    u32 channels;
    f64 position, ratio, target, delta;
    u32 ramp;
    u64 received;
    bool finished;
    f32 *coeff;
    bool owns_coeff;
    f32 samples[RING * 8];
};
static f64 i0(f64 x) {
    f64 sum = 1, term = 1;
    for (u32 k = 1; k < 32; ++k) {
        term *= x * x / (4 * (f64)k * k);
        sum += term;
    }
    return sum;
}
static bool valid_ratio(f64 r) {
    return r >= 0.125 && r <= 8;
}
de_result de__resample_create(const de_allocator *a, u32 channels, f64 ratio, de__resampler **out) {
    if (out)
        *out = NULL;
    if (!out || channels < 1 || channels > 8 || !valid_ratio(ratio))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "invalid resampler description");
    if (!a)
        a = de_heap_allocator();
    de__resampler *s = DE_ALLOC(a, sizeof *s, DE_ALIGNOF(de__resampler), DE_MEM_AUDIO);
    if (!s)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "resampler state");
    memset(s, 0, sizeof *s);
    s->a = a;
    s->channels = channels;
    s->ratio = s->target = ratio;
    s->coeff = DE_ALLOC(a, (PHASES + 1) * TAPS * sizeof(f32), 16, DE_MEM_AUDIO);
    if (!s->coeff) {
        DE_FREE(a, s, sizeof *s);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "resample coefficient bank");
    }
    s->owns_coeff = true;
    f64 cutoff = 0.94 / (ratio > 1 ? ratio : 1), denom = i0(10);
    for (u32 p = 0; p <= PHASES; ++p) {
        f64 total = 0;
        f64 frac = (f64)p / PHASES;
        for (i32 t = 0; t < TAPS; ++t) {
            f64 x = (f64)(t - HALF + 1) - frac, z = x / HALF;
            f64 window = i0(10 * de_dsqrt64(z * z < 1 ? 1 - z * z : 0)) / denom;
            f64 sinc = x == 0 ? cutoff
                              : de_dsin64(3.141592653589793 * cutoff * x) / (3.141592653589793 * x);
            s->coeff[p * TAPS + (u32)t] = (f32)(window * sinc);
            total += window * sinc;
        }
        for (u32 t = 0; t < TAPS; ++t)
            s->coeff[p * TAPS + t] = (f32)(s->coeff[p * TAPS + t] / total);
    }
    *out = s;
    return DE_OK;
}
de_result de__resample_create_shared(const de_allocator *a, u32 channels, f64 ratio,
                                     const de__resampler *bank, de__resampler **out) {
    if (out)
        *out = NULL;
    if (!out || !bank || channels < 1 || channels > 8 || !valid_ratio(ratio))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "invalid shared resampler");
    if (!a)
        a = de_heap_allocator();
    de__resampler *s = DE_ALLOC(a, sizeof *s, DE_ALIGNOF(de__resampler), DE_MEM_AUDIO);
    if (!s)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "shared resampler state");
    memset(s, 0, sizeof *s);
    s->a = a;
    s->channels = channels;
    s->ratio = s->target = ratio;
    s->coeff = bank->coeff;
    *out = s;
    return DE_OK;
}
void de__resample_format(de__resampler *s, u32 channels, f64 ratio) {
    if (!s || channels < 1 || channels > 8 || !valid_ratio(ratio))
        return;
    s->channels = channels;
    s->target = ratio;
    de__resample_reset(s);
}
void de__resample_destroy(de__resampler *s) {
    if (s) {
        if (s->owns_coeff)
            DE_FREE(s->a, s->coeff, (PHASES + 1) * TAPS * sizeof(f32));
        DE_FREE(s->a, s, sizeof *s);
    }
}
void de__resample_reset(de__resampler *s) {
    if (!s)
        return;
    s->position = 0;
    s->received = 0;
    s->finished = false;
    s->ramp = 0;
    s->ratio = s->target;
    memset(s->samples, 0, sizeof s->samples);
}
void de__resample_ratio(de__resampler *s, f64 ratio, u32 ramp) {
    if (!s || !valid_ratio(ratio))
        return;
    s->target = ratio;
    s->ramp = ramp;
    s->delta = ramp ? (ratio - s->ratio) / ramp : 0;
    if (!ramp)
        s->ratio = ratio;
}
u32 de__resample_push(de__resampler *s, const f32 *in, u32 frames) {
    if (!s || !in || s->finished)
        return 0;
    u64 keep = s->position > HALF ? (u64)s->position - HALF : 0;
    u64 room = RING - (s->received - keep);
    u32 n = frames < room ? frames : (u32)room;
    for (u32 f = 0; f < n; ++f)
        memcpy(&s->samples[((s->received + f) % RING) * s->channels], &in[f * s->channels],
               s->channels * sizeof(f32));
    s->received += n;
    return n;
}
void de__resample_finish(de__resampler *s) {
    if (s)
        s->finished = true;
}
u32 de__resample_read(de__resampler *s, f32 *out, u32 frames) {
    if (!s || !out)
        return 0;
    u32 n = 0;
    while (n < frames && s->position < (f64)s->received) {
        i64 center = (i64)s->position;
        if (!s->finished && (u64)center + HALF >= s->received)
            break;
        f64 coordinate = (s->position - (f64)center) * PHASES;
        u32 phase = (u32)coordinate;
        if (phase >= PHASES)
            phase = PHASES - 1;
        f64 blend = coordinate - phase;
        for (u32 c = 0; c < s->channels; ++c) {
            f64 sample = 0;
            for (i32 t = 0; t < TAPS; ++t) {
                i64 i = center + t - HALF + 1;
                if (i >= 0 && (u64)i < s->received)
                    sample += (f64)s->samples[((u64)i % RING) * s->channels + c] *
                              (s->coeff[phase * TAPS + (u32)t] * (1 - blend) +
                               s->coeff[(phase + 1) * TAPS + (u32)t] * blend);
            }
            out[n * s->channels + c] = (f32)sample;
        }
        ++n;
        s->position += s->ratio;
        if (s->ramp) {
            s->ratio += s->delta;
            if (--s->ramp == 0)
                s->ratio = s->target;
        }
    }
    return n;
}
de_result de__resample_buffer(const de_allocator *a, const f32 *in, u64 frames, u32 channels,
                              u32 input_rate, u32 output_rate, f32 **out, u64 *count) {
    if (out)
        *out = NULL;
    if (count)
        *count = 0;
    if (!out || !count || !in || !frames || !input_rate || !output_rate || frames > UINT32_MAX)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "invalid resample buffer");
    if (!a)
        a = de_heap_allocator();
    de__resampler *s = NULL;
    DE_TRY(de__resample_create(a, channels, (f64)input_rate / output_rate, &s));
    u64 capacity = (frames * (u64)output_rate + input_rate - 1) / input_rate;
    if (capacity > UINT32_MAX || capacity > SIZE_MAX / (channels * sizeof(f32))) {
        de__resample_destroy(s);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "resample size overflow");
    }
    f32 *result =
        DE_ALLOC(a, (size_t)capacity * channels * sizeof(f32), DE_ALIGNOF(f32), DE_MEM_AUDIO);
    if (!result) {
        de__resample_destroy(s);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "resample output");
    }
    u64 pushed = 0, written = 0;
    while (written < capacity) {
        if (pushed < frames)
            pushed += de__resample_push(s, in + pushed * channels, (u32)(frames - pushed));
        if (pushed == frames)
            de__resample_finish(s);
        u32 n = de__resample_read(s, result + written * channels, (u32)(capacity - written));
        written += n;
        if (!n && pushed == frames)
            break;
    }
    de__resample_destroy(s);
    *out = result;
    *count = written;
    return DE_OK;
}
