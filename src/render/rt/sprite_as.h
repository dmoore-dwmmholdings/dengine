#ifndef DE_RENDER_SPRITE_AS_H
#define DE_RENDER_SPRITE_AS_H
#include "render/rt/rt_internal.h"
de_result de__sprite_as_prepare(de_renderer *r);
void de__sprite_as_destroy(de_renderer *r);
bool de__sprite_as_metadata(de_renderer *r, u32 mesh_slot, de__rt_mesh_gpu *out);
#endif
