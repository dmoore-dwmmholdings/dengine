#ifndef DE_FONT_IMPORT_H
#define DE_FONT_IMPORT_H
#include <dengine/de_asset.h>
#include "text/font_format.h"
/** Font cooker entry: mode msdf (default), bitmap BMFont v3, or pixel PNG + map TOML.
 * charset accepts a name or list; chars adds UTF-8; ranges accepts inclusive strings
 * such as ["0x100-0x24f", "1024-1279"]. em_px=48, px_range=6, page_size=1024.
 * Pixel sidecar: cell=[8,8], chars="AB", widths=[8,7] (optional).
 * DFNT owns original outline bytes; unsupported color-emoji features are not synthesized.
 */
de_result de_font_import(de_asset_import_ctx *ctx);
de_asset_type_desc de_font_import_type_desc(void);
/* Bounded standalone parser also used by the fuzz target. */
typedef struct de__bmfont {
    u32 width, height, pages, count, kern_count;
    f32 size, ascent, line_height;
    de__font_glyph *glyphs;
    de__font_cmap *cmap;
    de__font_kern *kerns;
    char (*files)[256];
} de__bmfont;
de_result de__bmfont_parse(const void *data, u64 size, const de_allocator *a, de__bmfont *out);
de_result de__font_charset(const char *name, u8 *bits, u32 bytes);
#endif
