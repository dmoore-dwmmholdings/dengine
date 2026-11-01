#include "render/post/jitter.h"
#include <math.h>
u32 de__jitter_phase_count(u32 iw, u32 ih, u32 ow, u32 oh) {
    if (!iw || !ih || !ow || !oh)
        return 0;
    const f64 phases = ceil(8.0 * ((f64)ow / iw) * ((f64)oh / ih));
    return phases < 8 ? 8 : phases > 64 ? 64 : (u32)phases;
}
static f32 radical_inverse(u32 index, u32 base) {
    f32 result = 0, factor = 1;
    while (index) {
        factor /= (f32)base;
        result += factor * (f32)(index % base);
        index /= base;
    }
    return result;
}
de_vec2 de__jitter_sample(u64 frame, u32 phases) {
    if (!phases || phases > 64)
        return (de_vec2){0};
    const u32 index = (u32)(frame % phases) + 1;
    return (de_vec2){radical_inverse(index, 2) - .5f, radical_inverse(index, 3) - .5f};
}
de_mat4 de__jitter_projection(de_mat4 p, de_vec2 jitter, u32 width, u32 height) {
    if (!width || !height || !isfinite(jitter.x) || !isfinite(jitter.y))
        return p;
    const f32 x = -2 * jitter.x / (f32)width, y = 2 * jitter.y / (f32)height;
    for (u32 col = 0; col < 4; ++col) {
        p.m[col * 4] += x * p.m[col * 4 + 3];
        p.m[col * 4 + 1] += y * p.m[col * 4 + 3];
    }
    return p;
}
