#ifndef DE_FONT_RUNTIME_INTERNAL_H
#define DE_FONT_RUNTIME_INTERNAL_H
#include "text_internal.h"
#include "font_format.h"
#include <stb_truetype.h>
#include "platform/de_atomic.h"
enum {
    DE_FONT_BAKED_PAGES_MAX = 256,
    DE_FONT_RUNTIME_PAGES_MAX = 8,
    DE_FONT_PAGES_MAX = DE_FONT_BAKED_PAGES_MAX + DE_FONT_RUNTIME_PAGES_MAX
};
typedef struct de_text_dirty_rect {
    u32 page, x, y, w, h;
} de_text_dirty_rect;
typedef struct de_text_glyph_entry {
    u32 page, x, y, w, h;
    de_vec4 uv, plane;
    f32 advance;
    bool missing;
    u32 generation;
} de_text_glyph_entry;
typedef struct de_text_font {
    u64 identity; /* Unique decoded generation, independent of allocator address reuse. */
    const de_allocator *alloc;
    u64 block_bytes;
    u32 mode, generation, count, cmap_count, kern_count, pages, width, height, channels;
    f32 em, range, ascent, descent, gap;
    u32 source_size;
    u8 *source;
    de__font_glyph *glyphs;
    de__font_cmap *cmap;
    de__font_kern *kerns;
    u8 **pixels;
    stbtt_fontinfo face;
    de_text_dirty_rect dirty[DE_FONT_PAGES_MAX];
    u32 dirty_count;
} de_text_font;
void de_text__font_dirty(de_text_font *f, de_text_dirty_rect rect);
de_result de_text__font_decode(const de_allocator *a, const void *data, u64 size,
                               de_text_font **out);
void de_text__font_free(de_text_font *f);
de_text_font *de_text__font_get(de_text_system *t, de_font font);
bool de_text__resolve(de_text_system *t, de_font primary, u32 cp, de_font *out, u32 *glyph);
bool de_text__glyph(de_text_system *t, de_font font, u32 glyph, de_text_glyph_entry *out);
u32 de_text__codepoint(const de_text_font *f, u32 cp);
de_result de_text__runtime_init(de_text_system *t);
void de_text__runtime_destroy(de_text_system *t);
void de_text__glyph_miss(de_text_system *t, de_font font, u32 glyph);
void de_text__integrate_glyphs(de_text_system *t);
void de_text__publish_pages(de_text_system *t);
#endif
