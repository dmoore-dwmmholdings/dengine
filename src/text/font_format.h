#ifndef DE_FONT_FORMAT_H
#define DE_FONT_FORMAT_H
#include <dengine/de_base.h>
/* DFNT v1 is little endian, fieldwise encoded (no native struct dumps).
 Header 64 bytes: magic/version/mode/glyphs/cmap/kerns/pages/source_bytes,
 em_px/range/ascent/descent/line_gap, page width/height/channels.
 Glyph 48 bytes: index/page/x/y/w/h then plane bounds, advance, bearing.
 Cmap 8 bytes: codepoint/glyph. Kern 12 bytes: left/right/amount.
 Then source bytes then raw uniform-size pages. All floats IEEE754 binary32. */
#define DE_FONT_VERSION     1u
#define DE_FONT_HEADER_SIZE 64u
#define DE_FONT_GLYPH_SIZE  48u
typedef enum de__font_mode { DE_FONT_MSDF, DE_FONT_BITMAP, DE_FONT_PIXEL } de__font_mode;
typedef struct de__font_glyph {
    u32 index, page, x, y, w, h;
    f32 left, bottom, right, top, advance, bearing;
} de__font_glyph;
typedef struct de__font_cmap {
    u32 codepoint, glyph;
} de__font_cmap;
typedef struct de__font_kern {
    u32 left, right;
    f32 amount;
} de__font_kern;
static inline u32 de__font_u32(const u8 *p) {
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}
#endif
