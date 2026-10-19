#ifndef DE_TEXT_INTERNAL_H
#define DE_TEXT_INTERNAL_H
#include <dengine/de_text.h>
#include <dengine/de_memory.h>
struct de_text_system {
    const de_allocator *alloc;
    de_asset_system *assets;
    struct de__text_runtime *runtime;
    struct de__shape_system *shape;
    struct de__glyph_cache *glyph_cache;
    struct de__text_render *render;
    struct de__layout_cache *layout;
    de_str (*localize)(const de_text_system *, de_name);
};
#endif
