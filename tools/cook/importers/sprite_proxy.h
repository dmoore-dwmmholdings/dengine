#ifndef DE_COOK_SPRITE_PROXY_H
#define DE_COOK_SPRITE_PROXY_H
#include <dengine/de_memory.h>
#include "render/formats/de_cooked_sprite_proxy.h"
de_result de_sprite_proxy_build(const de_allocator *alloc, const f32 *rgba, u32 width, u32 height,
                                f32 cutoff, de_cooked_sprite_proxy *out);
#endif
