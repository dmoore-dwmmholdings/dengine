#include "pixel_internal.h"
#include <math.h>
/* The 32-cube LUT is built once per palette change; distance is Euclidean OKLab. */
static de_vec3 oklab(de_vec3 c) {
    f32 l = cbrtf(.4122214708f * c.x + .5363325363f * c.y + .0514459929f * c.z);
    f32 m = cbrtf(.2119034982f * c.x + .6806995451f * c.y + .1073969566f * c.z);
    f32 s = cbrtf(.0883024619f * c.x + .2817188376f * c.y + .6299787005f * c.z);
    return (de_vec3){.2104542553f * l + .793617785f * m - .0040720468f * s,
                     1.9779984951f * l - 2.428592205f * m + .4505937099f * s,
                     .0259040371f * l + .7827717662f * m - .808675766f * s};
}
u32 de__palette_nearest(const de_vec4 *colors, u32 count, de_vec3 rgb) {
    de_vec3 lab = oklab(rgb);
    f32 distance = INFINITY;
    u32 nearest = 0;
    for (u32 i = 0; i < count; ++i) {
        de_vec3 other = oklab((de_vec3){colors[i].x, colors[i].y, colors[i].z});
        f32 x = lab.x - other.x, y = lab.y - other.y, z = lab.z - other.z,
            d = x * x + y * y + z * z;
        if (d < distance) {
            distance = d;
            nearest = i;
        }
    }
    return nearest;
}
void de__palette_build(const de_vec4 *colors, u32 count, de_vec4 *lut) {
    for (u32 z = 0; z < 32; ++z)
        for (u32 y = 0; y < 32; ++y)
            for (u32 x = 0; x < 32; ++x)
                lut[(z * 32 + y) * 32 + x] = colors[de__palette_nearest(
                    colors, count, (de_vec3){x / 31.f, y / 31.f, z / 31.f})];
}
u32 de__pixel_bayer(u32 x, u32 y) {
    u32 value = 0;
    for (u32 bit = 0; bit < 3; ++bit) {
        u32 bx = (x >> bit) & 1u, by = (y >> bit) & 1u;
        value |= ((bx ^ by) * 2 + by) << (4 - bit * 2);
    }
    return value;
}
