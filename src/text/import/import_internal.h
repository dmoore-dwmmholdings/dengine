#ifndef DE_FONT_IMPORT_INTERNAL_H
#define DE_FONT_IMPORT_INTERNAL_H
#include "font_import.h"
#include <dengine/de_memory.h>
typedef struct de__font_build {
    u32 mode, count, cmap_count, kern_count, pages, width, height, channels;
    f32 em, range, ascent, descent, gap;
    const void *source;
    u32 source_size;
    de__font_glyph *glyphs;
    de__font_cmap *cmap;
    de__font_kern *kerns;
    u8 **pixels;
} de__font_build;
de_result de__font_write(de_asset_import_ctx *c, const de__font_build *b);
de_result de__font_bitmap(de_asset_import_ctx *c, bool pixel);
de_result de__font_pack(de_asset_import_ctx *c, de__font_build *b);
#endif
