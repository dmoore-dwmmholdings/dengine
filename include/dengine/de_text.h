/* de_text.h — fonts (MSDF + bitmap), shaping, layout, localization. SPEC.md §17.3. */
#ifndef DE_TEXT_H
#define DE_TEXT_H
#include "de_base.h"
#include "de_math.h"
#include "de_asset.h"
#include "de_render.h"
DE_EXTERN_C_BEGIN

typedef struct de_text_system de_text_system;
typedef struct de_text_layout de_text_layout;

DE_API de_result de_text_create(const de_allocator *a, de_asset_system *assets,
                                de_text_system **out);
DE_API void de_text_destroy(de_text_system *t);

/* Fallback chain: glyphs missing in font[0] are looked up in font[1], etc. */
DE_API void de_text_set_fallbacks(de_text_system *t, de_font primary, const de_font *fallbacks,
                                  u32 count);

/** Call once per frame after asset updates and before submitting text worker jobs. */
DE_API void de_text_update(de_text_system *t);
typedef struct de_font_metrics {
    f32 ascent, descent, line_gap;
    u32 generation;
} de_font_metrics;
/** Pixel metrics at size_px. NOT_FOUND for stale or not-yet-ready fonts. */
DE_API de_result de_text_font_metrics(de_text_system *t, de_font font, f32 size_px,
                                      de_font_metrics *out);

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
    u32 disable_features; /**< Bitmask DE_TEXT_DISABLE_*, zero enables defaults. */
} de_text_style;

enum {
    DE_TEXT_DISABLE_KERN = 1u,
    DE_TEXT_DISABLE_LIGA = 2u,
    DE_TEXT_DISABLE_CLIG = 4u,
    DE_TEXT_DISABLE_CALT = 8u
};

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
    de_font font; /**< Per-glyph fallback font; zero uses draw options.font. */
} de_glyph_quad;
DE_API u32 de_text_layout_quads(const de_text_layout *l, de_glyph_quad *out, u32 max);

/** Rendering is optional. A NULL renderer detaches and keeps CPU text usable. */
DE_API de_result de_text_attach(de_text_system *t, de_renderer *renderer, de_world *world);
typedef struct de_text_draw_opts {
    de_font font;
    de_vec4 clip_px; /**< x,y,width,height; zero width/height means the whole view. */
    f32 outline_width;
    de_vec4 outline_color;
    de_vec2 shadow_offset;
    de_vec4 shadow_color;
    bool pixel_snap;
} de_text_draw_opts;
/** Quad pos is x,y,width,height; uv is min-x,min-y,max-x,max-y. Submission order is stable. */
DE_API de_result de_text_draw(de_text_system *t, de_view view, const de_glyph_quad *quads,
                              u32 count, de_vec2 origin_px, const de_text_draw_opts *options);
DE_API de_gpu_texture de_text_page_texture(de_text_system *t, de_font font, u32 page);
typedef struct DeWorldText {
    de_name loc_id;
    char literal[128];
    de_text_style style;
    f32 size_m;
    de_text_align align;
    bool billboard, lit, depth_test;
} DeWorldText;
typedef struct de_text_render_stats {
    u64 uploaded_bytes;
    u32 uploads, draw_calls, glyphs;
} de_text_render_stats;
DE_API de_text_render_stats de_text_get_render_stats(const de_text_system *t);

typedef struct de_text_stats {
    u32 runtime_pending, runtime_generated, runtime_pages, cap_warnings, background_jobs;
} de_text_stats;
/** A snapshot of background glyph work and runtime atlas usage. */
DE_API de_text_stats de_text_get_stats(const de_text_system *t);

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
