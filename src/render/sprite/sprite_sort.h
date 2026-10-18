#ifndef DE_RENDER_SPRITE_SORT_H
#define DE_RENDER_SPRITE_SORT_H
#include <dengine/de_base.h>
/** Ascending key: authored layer/order, then descending view depth. NaN draws first. */
u64 de__sprite_sort_key(i16 layer, i16 order, f32 depth);
#endif
