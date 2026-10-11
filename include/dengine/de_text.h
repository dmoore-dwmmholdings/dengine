/* de_text.h — fonts (MSDF + bitmap), shaping, layout, localization. SPEC.md §17.3. */
#ifndef DE_TEXT_H
#define DE_TEXT_H
#include "de_base.h"
#include "de_math.h"
#include "de_asset.h"
DE_EXTERN_C_BEGIN

typedef struct de_text_system de_text_system;
typedef struct de_text_layout de_text_layout;

DE_API de_result de_text_create(const de_allocator *a, de_asset_system *assets,
                                de_text_system **out);
DE_API void de_text_destroy(de_text_system *t);

/* Fallback chain: glyphs missing in font[0] are looked up in font[1], etc. */
DE_API void de_text_set_fallbacks(de_text_system *t, de_font primary, const de_font *fallbacks,
                                  u32 count);

/* ---------- Shaping and layout ---------- */
typedef enum de_text_align {
    DE_ALIGN_START,
    DE_ALIGN_CENTER,
    DE_ALIGN_END,
    DE_ALIGN_JUSTIFY
} de_text_align;
typedef enum de_text_dir { DE_DIR_AUTO, DE_DIR_LTR, DE_DIR_RTL } de_text_dir;

typedef struct de_text_style {
    de_font font;
    f32 size_px; /* or meters for world text */
    de_vec4 color;
    f32 letter_spacing, line_height;
    f32 outline_width;
    de_vec4 outline_color; /* MSDF */
    f32 shadow_offset[2];
    de_vec4 shadow_color;
    const char *language; /* BCP-47; affects shaping and line breaking */
} de_text_style;

typedef struct de_text_layout_desc {
    de_str utf8;
    const de_text_style *style;
    f32 max_width; /* 0 = no wrap */
    de_text_align align;
    de_text_dir direction;
    bool pixel_snap; /* bitmap fonts / pixel art */
} de_text_layout_desc;

/* Bidi (UAX #9), shaping, line breaking (UAX #14), fallback resolution. Result is cached. */
DE_API de_result de_text_layout_build(de_text_system *t, const de_text_layout_desc *desc,
                                      const de_allocator *a, de_text_layout **out);
DE_API void de_text_layout_free(de_text_layout *l);
DE_API de_vec2 de_text_layout_size(const de_text_layout *l);
DE_API u32 de_text_layout_hit_test(const de_text_layout *l, de_vec2 point);    /* byte offset */
DE_API de_vec4 de_text_layout_caret(const de_text_layout *l, u32 byte_offset); /* x, y, w, h */

typedef struct de_glyph_quad {
    de_vec4 pos;
    de_vec4 uv;
    u32 atlas_page;
    de_vec4 color;
} de_glyph_quad;
DE_API u32 de_text_layout_quads(const de_text_layout *l, de_glyph_quad *out, u32 max);

/* ---------- Localization ---------- */
DE_API de_result de_loc_set_locale(de_text_system *t,
                                   const char *locale); /* also updates asset overrides */
DE_API const char *de_loc_locale(const de_text_system *t);
/* Looks up a string ID in the active string table. Missing IDs return "[[id]]" in Dev. */
DE_API de_str de_loc(const de_text_system *t, de_name id);
typedef struct de_loc_arg {
    const char *name;
    enum { DE_LOC_ARG_STR, DE_LOC_ARG_INT, DE_LOC_ARG_FLOAT } kind;
    const char *s;
    i64 i;
    f64 f;
} de_loc_arg;
/* "{count} {count, plural, one{item} other{items}}" with CLDR plural rules. */
DE_API u32 de_loc_format(const de_text_system *t, de_name id, const de_loc_arg *args, u32 arg_count,
                         char *out, u32 out_bytes);
DE_API void de_loc_pseudo(de_text_system *t, bool enabled); /* pseudo-localization, Dev */

DE_EXTERN_C_END
#endif
