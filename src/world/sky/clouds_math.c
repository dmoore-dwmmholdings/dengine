/*
 * clouds_math.c: the CPU side of the volumetric clouds (DENG-208) that every build has: the
 * DeSky layer resolved with its defaults, the quality table and the double-precision tile
 * offsets. Pure; no allocation. See clouds_internal.h.
 */
#include "world/sky/clouds_internal.h"

#include <math.h>

static f32 clamp01(f32 v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

de__clouds_settings de__clouds_resolve(const DeSky *sky) {
    de__clouds_settings c = {0};
    if (sky == NULL)
        return c;
    c.coverage = clamp01(sky->cloud_coverage == sky->cloud_coverage ? sky->cloud_coverage : 0.0f);
    c.on = sky->clouds && c.coverage > 0.0f;
    c.bottom_m = sky->cloud_bottom_m > 0.0f ? sky->cloud_bottom_m : DE__CLOUDS_BOTTOM_M;
    c.top_m = sky->cloud_top_m > 0.0f ? sky->cloud_top_m : DE__CLOUDS_TOP_M;
    if (c.top_m < c.bottom_m + DE__CLOUDS_MIN_DEPTH_M)
        c.top_m = c.bottom_m + DE__CLOUDS_MIN_DEPTH_M;
    c.density = sky->cloud_density > 0.0f ? sky->cloud_density : 1.0f;
    c.erosion = sky->cloud_erosion > 0.0f ? clamp01(sky->cloud_erosion) : DE__CLOUDS_EROSION;
    const f32 speed = sky->cloud_wind_speed_mps > 0.0f ? sky->cloud_wind_speed_mps : 0.0f;
    const f64 a = (f64)sky->cloud_wind_dir_deg * (3.14159265358979323846 / 180.0);
    c.wind_x = (f32)(sin(a) * (f64)speed); /* clockwise from north (+y) */
    c.wind_y = (f32)(cos(a) * (f64)speed);
    return c;
}

bool de__clouds_enabled(const de__clouds_settings *c, const de_render_settings *rs) {
    return c != NULL && rs != NULL && c->on && c->coverage > 0.0f &&
           rs->volumetrics != DE_QUALITY_OFF;
}

de__clouds_quality de__clouds_quality_for(de_quality q) {
    switch (q) {
    case DE_QUALITY_LOW:
        return (de__clouds_quality){48u, 4u, 2u, 8u};
    case DE_QUALITY_MEDIUM:
        return (de__clouds_quality){64u, 5u, 2u, 12u};
    case DE_QUALITY_HIGH:
        return (de__clouds_quality){96u, 6u, 3u, 16u};
    case DE_QUALITY_ULTRA:
        return (de__clouds_quality){128u, 6u, 3u, 24u};
    default:
        return (de__clouds_quality){0u, 0u, 0u, 0u};
    }
}

f32 de__clouds_tile_offset(f64 world, f64 drift, f64 tile) {
    if (!(tile > 0.0))
        return 0.0f;
    f64 r = fmod(world + drift, tile);
    if (r < 0.0)
        r += tile;
    if (r >= tile)
        r -= tile;
    f32 o = (f32)r;
    return o >= (f32)tile ? 0.0f : o;
}
