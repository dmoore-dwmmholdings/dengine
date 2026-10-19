#ifndef DE_TEXT_LAYOUT_INTERNAL_H
#define DE_TEXT_LAYOUT_INTERNAL_H
#include "text/shape/shaper_internal.h"
#include <SheenBidi.h>
/* Unicode properties generated from the committed 15.1 inputs. */
typedef enum de__lb {
    LB_XX,
    LB_AI,
    LB_AK,
    LB_AL,
    LB_AP,
    LB_AS,
    LB_B2,
    LB_BA,
    LB_BB,
    LB_BK,
    LB_CB,
    LB_CJ,
    LB_CL,
    LB_CM,
    LB_CP,
    LB_CR,
    LB_EB,
    LB_EM,
    LB_EX,
    LB_GL,
    LB_H2,
    LB_H3,
    LB_HL,
    LB_HY,
    LB_ID,
    LB_IN,
    LB_IS,
    LB_JL,
    LB_JT,
    LB_JV,
    LB_LF,
    LB_NL,
    LB_NS,
    LB_NU,
    LB_OP,
    LB_PO,
    LB_PR,
    LB_QU,
    LB_RI,
    LB_SA,
    LB_SG,
    LB_SP,
    LB_SY,
    LB_VF,
    LB_VI,
    LB_WJ,
    LB_ZW,
    LB_ZWJ
} de__lb;
typedef struct de__unicode_props {
    u8 lb, grapheme, incb;
    bool east_asian, initial_quote, final_quote, mark, unassigned, pictographic;
} de__unicode_props;
de__unicode_props de__text_unicode(u32 cp);
/* Breaks[i] describes boundary before cp[i]; 2 mandatory, 1 allowed, 0 prohibited. */
de_result de__text_linebreak(const u32 *cp, u32 count, u8 *breaks, const de_allocator *alloc);
void de__text_graphemes(const u32 *cp, u32 count, u8 *breaks);
typedef struct de__bidi {
    SBAlgorithmRef algorithm;
    SBParagraphRef paragraph;
} de__bidi;
de_result de__text_bidi(const u32 *cp, u32 count, de_text_dir dir, de__bidi *out);
void de__text_bidi_free(de__bidi *b);

typedef struct de__layout_position {
    de_shaped_glyph shape;
    f32 x, y;
    u32 cp, next;
} de__layout_position;
typedef struct de__layout_cluster {
    u32 start, end, line;
    f32 x0, x1;
    bool rtl;
} de__layout_cluster;
typedef struct de__layout_font {
    de_font font;
    u32 generation;
    const de_text_font *identity;
} de__layout_font;
typedef struct de__layout_key {
    de_text_style style;
    char language[64];
    f32 max_width;
    u32 align, direction, fallback_epoch;
    bool pixel_snap;
    const de_allocator *alloc;
} de__layout_key;
struct de_text_layout {
    const de_allocator *alloc;
    de_atomic_u32 refs;
    de__layout_key key;
    char *utf8;
    u32 byte_count, glyph_count, glyph_capacity, cluster_count, line_count;
    de__layout_position *glyphs;
    de__layout_cluster *clusters;
    de_glyph_quad *quads;
    u32 quad_count;
    de__layout_font fonts[16];
    u32 font_count;
    de_vec2 size;
    f32 line_height, ascent;
};
de_result de__text_paragraph(de_text_system *t, const de_text_layout_desc *desc,
                             const de__layout_key *key, de_text_layout **out);
de_result de__text_layout_refresh(de_text_system *t, const de_text_layout *source,
                                  de_text_layout **out);
void de__text_layout_quads_build(de_text_system *t, de_text_layout *layout);
bool de__text_layout_stale(de_text_system *t, const de_text_layout *layout);
de_result de__text_layout_init(de_text_system *t);
void de__text_layout_destroy(de_text_system *t);
#endif
