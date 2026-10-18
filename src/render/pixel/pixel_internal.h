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
#endif
