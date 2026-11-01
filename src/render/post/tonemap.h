#ifndef DE_RENDER_POST_TONEMAP_H
#define DE_RENDER_POST_TONEMAP_H

#include <dengine/de_render.h>

typedef struct de__tonemappers de__tonemappers;
de_result de__tonemappers_create(de_renderer *r);
void de__tonemappers_destroy(de_renderer *r);
/** Main thread, with render recording idle, before handing a frame to the render thread. */
void de__tonemappers_prepare(de_renderer *r);
/** Declares a pass using only stable pipeline handles and copied constants. */
de_rg_resource de__tonemap_add(de_renderer *r, de_rg *g, de_arena *arena, de_rg_resource src,
                               u32 width, u32 height, de_tonemapper mapper, const f32 params[8],
                               f32 exposure_multiplier, bool hdr_output,
                               const de_hdr_settings *hdr);

#endif
