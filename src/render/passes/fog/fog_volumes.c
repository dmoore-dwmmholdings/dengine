/*
 * fog_volumes.c: the CPU side of the froxel fog that needs no device (DENG-207): the grid per
 * quality, the exponential slice distribution and the camera-relative packing of the render
 * world's DeFogVolume snapshots. See fog_internal.h.
 */
#include "render/passes/fog/fog_internal.h"

#if DE_HAS_RENDER

#include "render/world/render_world.h"

#include <math.h>
#include <string.h>

de__fog_grid de__fog_grid_for(de_quality q) {
    switch (q) {
    case DE_QUALITY_LOW:
        return (de__fog_grid){80u, 45u, 32u};
    case DE_QUALITY_MEDIUM:
        return (de__fog_grid){120u, 68u, 48u};
    case DE_QUALITY_HIGH:
        return (de__fog_grid){160u, 90u, 64u};
    case DE_QUALITY_ULTRA:
        return (de__fog_grid){160u, 90u, 128u};
    case DE_QUALITY_OFF:
    default:
        return (de__fog_grid){0u, 0u, 0u};
    }
}

void de__fog_atlas_size(de__fog_grid g, u32 *w, u32 *h) {
    const u32 rows = (g.d + DE__FOG_ATLAS_COLUMNS - 1u) / DE__FOG_ATLAS_COLUMNS;
    *w = g.w * DE__FOG_ATLAS_COLUMNS;
    *h = g.h * rows;
}

f32 de__fog_slice_depth(f32 s, u32 d, f32 near_m, f32 far_m) {
    if (d == 0u || !(near_m > 0.0f) || !(far_m > near_m))
        return near_m;
    return near_m * powf(far_m / near_m, s / (f32)d);
}

f32 de__fog_slice_of(f32 z, u32 d, f32 near_m, f32 far_m) {
    if (d == 0u || !(near_m > 0.0f) || !(far_m > near_m))
        return 0.0f;
    if (!(z > 0.0f))
        return -(f32)d; /* nearer than anything */
    return (f32)d * logf(z / near_m) / logf(far_m / near_m);
}

bool de__fog_enabled(const de_render_settings *s, const de__fog_snapshot *volumes, u32 n,
                     f32 max_distance) {
    if (s == NULL || de__fog_grid_for(s->volumetrics).d == 0u || volumes == NULL ||
        !(max_distance > DE__FOG_NEAR))
        return false;
    for (u32 i = 0; i < n; ++i)
        if (volumes[i].volume.density > 0.0f)
            return true;
    return false;
}

static f32 clamp01(f32 v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

u32 de__fog_pack_volumes(const de__fog_snapshot *src, u32 n, de_vec3 camera,
                         de__fog_volume_gpu *out, u32 cap) {
    u32 count = 0;
    for (u32 i = 0; src != NULL && out != NULL && i < n && count < cap; ++i) {
        const DeFogVolume *v = &src[i].volume;
        if (!(v->density > 0.0f))
            continue;
        de__fog_volume_gpu *o = &out[count++];
        memset(o, 0, sizeof *o);
        /* The transform relative to the camera: translation - camera. */
        de_mat4 m = de_affine_to_m4(&src[i].transform);
        m.m[12] -= camera.x;
        m.m[13] -= camera.y;
        m.m[14] -= camera.z;
        const f32 hx = fabsf(v->extent.x) * 0.5f, hy = fabsf(v->extent.y) * 0.5f,
                  hz = fabsf(v->extent.z) * 0.5f;
        const bool global = hx == 0.0f && hy == 0.0f && hz == 0.0f;
        const de_mat4 inv = de_m4_inverse_affine(&m);
        for (u32 r = 0; r < 3u; ++r)
            for (u32 c = 0; c < 4u; ++c)
                o->inv[r][c] = inv.m[c * 4u + r];
        o->extent_density[0] = global ? -1.0f : hx;
        o->extent_density[1] = hy;
        o->extent_density[2] = hz;
        o->extent_density[3] = v->density;
        o->albedo_g[0] = fmaxf(v->albedo.x, 0.0f);
        o->albedo_g[1] = fmaxf(v->albedo.y, 0.0f);
        o->albedo_g[2] = fmaxf(v->albedo.z, 0.0f);
        o->albedo_g[3] = fminf(fmaxf(v->anisotropy, -0.95f), 0.95f);
        o->params[0] = global ? fmaxf(v->height_falloff, 0.0f) : 0.0f;
        o->params[1] = clamp01(v->noise);
        o->params[2] = fmaxf(v->edge_fade, 0.0f);
        o->params[3] = m.m[14]; /* the entity's height, camera-relative */
    }
    return count;
}

#endif /* DE_HAS_RENDER */
