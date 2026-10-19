#ifndef DE_TEXT_SHAPER_INTERNAL_H
#define DE_TEXT_SHAPER_INTERNAL_H
#include "text/font_runtime_internal.h"
#include <hb.h>
typedef struct de_text_item {
    u32 start, end;
    de_font font;
    hb_script_t script;
} de_text_item;
typedef struct de_shaped_glyph {
    u32 glyph, cluster;
    de_font font;
    f32 x_advance, y_advance, x_offset, y_offset;
} de_shaped_glyph;
typedef struct de_shaped_run {
    de_shaped_glyph *glyphs;
    u32 count;
    const de_allocator *alloc;
    u64 bytes;
} de_shaped_run;
de_result de_text__itemize(de_text_system *t, de_font primary, de_str utf8, const de_allocator *a,
                           de_text_item **out, u32 *count);
de_result de_text__shape(de_text_system *t, de_str utf8, const de_text_style *style,
                         de_text_dir dir, const de_allocator *a, de_shaped_run *out);
void de_text__shape_free(de_shaped_run *run);
de_result de_text__shape_init(de_text_system *t);
void de_text__shape_destroy(de_text_system *t);
u32 de__text_utf8(const char *s, u32 size, u32 *offset);
#endif
