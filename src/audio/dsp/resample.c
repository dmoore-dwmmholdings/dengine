#include "audio/dsp/resample.h"
#include <dengine/de_math.h>
#include <string.h>
#define HALF   96
#define TAPS   (HALF * 2)
#define PHASES 1024
#define RING   8192
typedef struct de__resample_filter {
    f32 window[(PHASES + 1) * TAPS];
    f32 sinc[HALF * PHASES + 1];
} de__resample_filter;
struct de__resampler {
    const de_allocator *a;
    u32 channels;
    f64 position, ratio, target, delta;
    u32 ramp;
    /* Exact steady-rate phase relative to the last ratio change/ramp end. */
    f64 clock_origin;
    u64 clock_whole;
    u64 clock_numerator, clock_denominator, clock_remainder;
    u64 received;
    bool finished;
    de__resample_filter *filter;
    bool owns_filter;
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
/* Recover a sample-rate fraction that round-trips to the API's double ratio. If no
 * convergent fits u32 exactly, retain its binary fraction rather than rounding a rate
 * close to (but distinct from) an integer. The valid ratio range keeps both within u64. */
static void clock_reset(de__resampler *s) {
    u64 bits;
    memcpy(&bits, &s->ratio, sizeof bits);
    s->clock_numerator = (bits & ((1ull << 52) - 1)) | (1ull << 52);
    s->clock_denominator = 1ull << (1023 + 52 - ((bits >> 52) & 0x7ff));
    u64 p0 = 0, p1 = 1, q0 = 1, q1 = 0;
    f64 x = s->ratio;
    for (u32 i = 0; i < 64; ++i) {
        if (x > UINT32_MAX)
            break;
        u64 a = (u64)x, p = a * p1 + p0, q = a * q1 + q0;
        if (p > UINT32_MAX || q > UINT32_MAX)
            break;
        p0 = p1;
        p1 = p;
        q0 = q1;
        q1 = q;
        if ((f64)p / (f64)q == s->ratio) {
            s->clock_numerator = p;
            s->clock_denominator = q;
            break;
        }
        if (x == (f64)a)
            break;
        x = 1 / (x - (f64)a);
    }
    s->clock_whole = 0;
    s->clock_remainder = 0;
    s->clock_origin = s->position;
}
static void clock_advance(de__resampler *s) {
    if (s->ramp) {
        s->position += s->ratio;
        s->ratio += s->delta;
        if (--s->ramp == 0) {
            s->ratio = s->target;
            clock_reset(s);
        }
    } else {
        u64 phase = (u64)s->clock_remainder + s->clock_numerator;
        s->clock_whole += phase / s->clock_denominator;
        s->clock_remainder = phase % s->clock_denominator;
        s->position =
            s->clock_origin + (f64)s->clock_whole + (f64)s->clock_remainder / s->clock_denominator;
    }
}
static bool clock_before_end(const de__resampler *s) {
    if (s->ramp)
        return s->position < (f64)s->received;
    u64 origin = (u64)s->clock_origin;
    if (origin >= s->received || s->clock_whole >= s->received - origin)
        return false;
    u64 left = s->received - origin - s->clock_whole;
    f64 fraction = s->clock_origin - (f64)origin;
    /* Do not round whole+phase to the EOF integer before deciding whether to emit. */
    return left > 1 || fraction == 0 ||
           (f64)s->clock_remainder / s->clock_denominator < 1 - fraction;
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
    clock_reset(s);
    s->filter = DE_ALLOC(a, sizeof *s->filter, 16, DE_MEM_AUDIO);
    if (!s->filter) {
        DE_FREE(a, s, sizeof *s);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "resample filter tables");
    }
    s->owns_filter = true;
    f64 denom = i0(10);
    for (u32 p = 0; p <= PHASES; ++p) {
        f64 frac = (f64)p / PHASES;
        for (i32 t = 0; t < TAPS; ++t) {
            f64 x = (f64)(t - HALF + 1) - frac, z = x / HALF;
            s->filter->window[p * TAPS + (u32)t] =
                (f32)(i0(10 * de_dsqrt64(z * z < 1 ? 1 - z * z : 0)) / denom);
        }
    }
    s->filter->sinc[0] = 1;
    for (u32 i = 1; i <= HALF * PHASES; ++i) {
        f64 x = 3.141592653589793 * i / PHASES;
        s->filter->sinc[i] = (f32)(de_dsin64(x) / x);
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
    clock_reset(s);
    s->filter = bank->filter;
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
        if (s->owns_filter)
            DE_FREE(s->a, s->filter, sizeof *s->filter);
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
    clock_reset(s);
    memset(s->samples, 0, sizeof s->samples);
}
void de__resample_ratio(de__resampler *s, f64 ratio, u32 ramp) {
    if (!s || !valid_ratio(ratio))
        return;
    s->target = ratio;
    s->ramp = ramp;
    s->delta = ramp ? (ratio - s->ratio) / ramp : 0;
    if (!ramp) {
        s->ratio = ratio;
        clock_reset(s);
    }
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
    while (n < frames && clock_before_end(s)) {
        i64 center = (i64)s->position;
        if (!s->finished && (u64)center + HALF >= s->received)
            break;
        f64 coordinate = (s->position - (f64)center) * PHASES;
        u32 phase = (u32)coordinate;
        if (phase >= PHASES)
            phase = PHASES - 1;
        f64 blend = coordinate - phase;
        /* Evaluate the current rate's low-pass kernel from immutable tables. Computing
         * it once per frame also shares the weights across interleaved channels. */
        f64 cutoff = 0.94 / (s->ratio > 1 ? s->ratio : 1);
        f64 fraction = s->position - (f64)center, total = 0;
        f64 weights[TAPS];
        for (i32 t = 0; t < TAPS; ++t) {
            f64 x = (f64)(t - HALF + 1) - fraction;
            f64 coordinate_sinc = (x < 0 ? -x : x) * cutoff * PHASES;
            u32 at = (u32)coordinate_sinc;
            f64 mix = coordinate_sinc - at;
            f64 sinc = s->filter->sinc[at] * (1 - mix) + s->filter->sinc[at + 1] * mix;
            f64 window = s->filter->window[phase * TAPS + (u32)t] * (1 - blend) +
                         s->filter->window[(phase + 1) * TAPS + (u32)t] * blend;
            weights[t] = window * sinc * cutoff;
            total += weights[t];
        }
        for (u32 c = 0; c < s->channels; ++c) {
            f64 sample = 0;
            for (i32 t = 0; t < TAPS; ++t) {
                i64 i = center + t - HALF + 1;
                if (i >= 0 && (u64)i < s->received)
                    sample += (f64)s->samples[((u64)i % RING) * s->channels + c] * weights[t];
            }
            out[n * s->channels + c] = (f32)(sample / total);
        }
        ++n;
        clock_advance(s);
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
