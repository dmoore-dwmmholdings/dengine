#ifndef DE_PIXEL_INTERNAL_H
#define DE_PIXEL_INTERNAL_H
#include "render/views_internal.h"
typedef struct de__pixel_layout {
    f32 scale, x, y, width, height;
} de__pixel_layout;
de__pixel_layout de__pixel_layout_make(u32 iw, u32 ih, u32 ow, u32 oh, bool integer);
de_dvec3 de__pixel_snap(de_dvec3 position, de_plane_2d plane, f64 texel, de_vec2 *residual);
typedef struct de__pixel de__pixel;
de_result de__pixel_create(de_renderer *r, de__pixel **out);
void de__pixel_destroy(de_renderer *r, de__pixel *p);
u32 de__palette_nearest(const de_vec4 *colors, u32 count, de_vec3 rgb);
void de__palette_build(const de_vec4 *colors, u32 count, de_vec4 *lut);
u32 de__pixel_bayer(u32 x, u32 y);
typedef struct de__pixel_snapshots de__pixel_snapshots;
de_result de__pixel_snapshots_create(de_renderer *, de__pixel_snapshots **);
void de__pixel_snapshots_destroy(de_renderer *, de__pixel_snapshots *);
typedef struct de__palette_runtime de__palette_runtime;
de_result de__palette_runtime_create(de_renderer *, de__palette_runtime **);
void de__palette_runtime_destroy(de_renderer *, de__palette_runtime *);
void de__palette_prepare(de__palette_runtime *, de_texture);
void de__pixel_prepare_palette(de__pixel *, de_texture);
#endif
