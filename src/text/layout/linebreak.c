/* UAX14 revision51, Unicode15.1; numeric tailoring from §8.2 Example7, as in
 * the official LineBreakTest. Rules are evaluated in specification order. */
#include "layout_internal.h"
#include <string.h>
typedef struct lb_char {
    de__unicode_props props;
    u32 cp, base;
    de__lb raw, cls;
} lb_char;
static bool hard(de__lb c) {
    return c == LB_BK || c == LB_CR || c == LB_LF || c == LB_NL;
}
static bool alpha(de__lb c) {
    return c == LB_AL || c == LB_HL;
}
static bool hangul(de__lb c) {
    return c == LB_JL || c == LB_JV || c == LB_JT || c == LB_H2 || c == LB_H3;
}
static bool aksara(const lb_char *c) {
    return c->cls == LB_AK || c->cls == LB_AS || c->cp == 0x25cc;
}
static i32 previous(const lb_char *c, i32 index) {
    return index < 0 ? -1 : (i32)c[index].base;
}
static u32 next(const lb_char *c, u32 count, u32 index) {
    u32 j = index + 1;
    while (j < count && c[j].base == index)
        ++j;
    return j;
}
static bool numeric_before(const lb_char *c, i32 index) {
    while (index >= 0) {
        de__lb k = c[index].cls;
        if (k == LB_NU)
            return true;
        if (k != LB_SY && k != LB_IS)
            return false;
        index = previous(c, index - 1);
    }
    return false;
}
static u8 boundary(const lb_char *c, u32 count, u32 i) {
    de__lb raw_a = c[i - 1].raw, raw_b = c[i].raw;
    if (raw_a == LB_BK)
        return 2; /* LB4 */
    if (raw_a == LB_CR && raw_b == LB_LF)
        return 0;
    if (raw_a == LB_CR || raw_a == LB_LF || raw_a == LB_NL)
        return 2; /* LB5 */
    if (hard(raw_b))
        return 0; /* LB6 */
    if (raw_b == LB_SP || raw_b == LB_ZW)
        return 0; /* LB7 */
    i32 space = (i32)i - 1;
    while (space >= 0 && c[space].raw == LB_SP)
        --space;
    if (space >= 0 && c[space].raw == LB_ZW)
        return 1; /* LB8 */
    if (raw_a == LB_ZWJ)
        return 0; /* LB8a */
    if (c[i].base != i)
        return 0; /* LB9 */
    i32 ai = previous(c, (i32)i - 1);
    const lb_char *left = &c[ai], *right = &c[i];
    de__lb a = left->cls, b = right->cls;
    if (a == LB_WJ || b == LB_WJ)
        return 0; /* LB11 */
    if (a == LB_GL)
        return 0; /* LB12 */
    if (b == LB_GL && a != LB_SP && a != LB_BA && a != LB_HY)
        return 0; /* LB12a */
    if (b == LB_CL || b == LB_CP || b == LB_EX || b == LB_IS || b == LB_SY)
        return 0; /* LB13 */
    i32 p = ai;
    while (p >= 0 && c[p].cls == LB_SP)
        p = previous(c, p - 1);
    if (p >= 0 && c[p].cls == LB_OP)
        return 0; /* LB14 */
    if (p >= 0 && c[p].cls == LB_QU && c[p].props.initial_quote) {
        i32 before = previous(c, p - 1);
        if (before < 0 || hard(c[before].cls) || c[before].cls == LB_OP || c[before].cls == LB_QU ||
            c[before].cls == LB_GL || c[before].cls == LB_SP || c[before].cls == LB_ZW)
            return 0; /* LB15a */
    }
    if (b == LB_QU && right->props.final_quote) {
        u32 n = next(c, count, i);
        if (n == count)
            return 0;
        de__lb after = c[n].cls;
        if (after == LB_SP || after == LB_GL || after == LB_WJ || after == LB_CL ||
            after == LB_QU || after == LB_CP || after == LB_EX || after == LB_IS ||
            after == LB_SY || hard(after) || after == LB_ZW)
            return 0; /* LB15b */
    }
    if (p >= 0 && (c[p].cls == LB_CL || c[p].cls == LB_CP) && b == LB_NS)
        return 0; /* LB16 */
    if (p >= 0 && c[p].cls == LB_B2 && b == LB_B2)
        return 0; /* LB17 */
    if (a == LB_SP)
        return 1; /* LB18 */
    if (a == LB_QU || b == LB_QU)
        return 0; /* LB19 */
    if (a == LB_CB || b == LB_CB)
        return 1; /* LB20 */
    if (b == LB_BA || b == LB_HY || b == LB_NS || a == LB_BB)
        return 0; /* LB21 */
    i32 prev = previous(c, ai - 1);
    if ((a == LB_HY || a == LB_BA) && prev >= 0 && c[prev].cls == LB_HL)
        return 0; /* LB21a */
    if (a == LB_SY && b == LB_HL)
        return 0; /* LB21b */
    if (b == LB_IN)
        return 0; /* LB22 */
    if ((alpha(a) && b == LB_NU) || (a == LB_NU && alpha(b)))
        return 0; /* LB23 */
    if ((a == LB_PR && (b == LB_ID || b == LB_EB || b == LB_EM)) ||
        ((a == LB_ID || a == LB_EB || a == LB_EM) && b == LB_PO))
        return 0; /* LB23a */
    if (((a == LB_PR || a == LB_PO) && alpha(b)) || (alpha(a) && (b == LB_PR || b == LB_PO)))
        return 0; /* LB24 */
    /* LB25 numeric tailoring, matching the official conformance data. */
    u32 n = next(c, count, i);
    if ((a == LB_PR || a == LB_PO) &&
        (b == LB_NU || ((b == LB_OP || b == LB_HY) && n < count && c[n].cls == LB_NU)))
        return 0;
    if ((a == LB_OP || a == LB_HY) && b == LB_NU)
        return 0;
    if ((b == LB_NU || b == LB_SY || b == LB_IS || b == LB_CL || b == LB_CP) &&
        numeric_before(c, ai))
        return 0;
    if (b == LB_PO || b == LB_PR) {
        i32 number = (a == LB_CL || a == LB_CP) ? previous(c, ai - 1) : ai;
        if (numeric_before(c, number))
            return 0;
    }
    if ((a == LB_JL && (b == LB_JL || b == LB_JV || b == LB_H2 || b == LB_H3)) ||
        ((a == LB_JV || a == LB_H2) && (b == LB_JV || b == LB_JT)) ||
        ((a == LB_JT || a == LB_H3) && b == LB_JT))
        return 0; /* LB26 */
    if ((hangul(a) && b == LB_PO) || (a == LB_PR && hangul(b)))
        return 0; /* LB27 */
    if (alpha(a) && alpha(b))
        return 0; /* LB28 */
    if ((a == LB_AP && aksara(right)) || (aksara(left) && (b == LB_VF || b == LB_VI)) ||
        (a == LB_VI && prev >= 0 && aksara(&c[prev]) && (b == LB_AK || right->cp == 0x25cc)) ||
        (aksara(left) && aksara(right) && n < count && c[n].cls == LB_VF))
        return 0; /* LB28a */
    if (a == LB_IS && alpha(b))
        return 0; /* LB29 */
    if (((alpha(a) || a == LB_NU) && b == LB_OP && !right->props.east_asian) ||
        (a == LB_CP && !left->props.east_asian && (alpha(b) || b == LB_NU)))
        return 0; /* LB30 */
    if (a == LB_RI && b == LB_RI) {
        u32 ri = 0;
        i32 at = ai;
        while (at >= 0 && c[at].cls == LB_RI) {
            ++ri;
            at = previous(c, at - 1);
        }
        if (ri & 1)
            return 0; /* LB30a */
    }
    if (b == LB_EM && (a == LB_EB || (left->props.pictographic && left->props.unassigned)))
        return 0; /* LB30b */
    return 1;     /* LB31 */
}
de_result de__text_linebreak(const u32 *cp, u32 count, u8 *breaks, const de_allocator *alloc) {
    if (!breaks || (!cp && count))
        return DE_ERR_INVALID_ARGUMENT;
    breaks[0] = 0;
    if (!count)
        return DE_OK;
    lb_char small[128], *chars = small;
    if (count > 128u) {
        chars = DE_ALLOC(alloc, (u64)count * sizeof *chars, _Alignof(lb_char), DE_MEM_UI);
        if (!chars)
            return DE_ERR_OUT_OF_MEMORY;
    }
    for (u32 i = 0; i < count; ++i) {
        lb_char *c = &chars[i];
        c->cp = cp[i];
        c->props = de__text_unicode(cp[i]);
        c->raw = (de__lb)c->props.lb;
        if (c->raw == LB_AI || c->raw == LB_SG || c->raw == LB_XX)
            c->raw = LB_AL;
        if (c->raw == LB_SA)
            c->raw = c->props.mark ? LB_CM : LB_AL;
        if (c->raw == LB_CJ)
            c->raw = LB_NS;
        c->base = i;
        c->cls = c->raw;
        if (c->raw == LB_CM || c->raw == LB_ZWJ) {
            if (i && !hard(chars[i - 1].cls) && chars[i - 1].cls != LB_SP &&
                chars[i - 1].cls != LB_ZW) {
                c->cls = chars[i - 1].cls;
                c->base = chars[i - 1].base;
            } else
                c->cls = LB_AL;
        }
    }
    for (u32 i = 1; i < count; ++i)
        breaks[i] = boundary(chars, count, i);
    breaks[count] = 2;
    if (chars != small)
        DE_FREE(alloc, chars, (u64)count * sizeof *chars);
    return DE_OK;
}
enum {
    GB_OTHER,
    GB_CR,
    GB_LF,
    GB_CONTROL,
    GB_EXTEND,
    GB_ZWJ,
    GB_RI,
    GB_PREPEND,
    GB_SPACING,
    GB_L,
    GB_V,
    GB_T,
    GB_LV,
    GB_LVT
};
static bool control(u8 g) {
    return g == GB_CR || g == GB_LF || g == GB_CONTROL;
}
void de__text_graphemes(const u32 *cp, u32 count, u8 *breaks) {
    breaks[0] = breaks[count] = 1;
    for (u32 i = 1; i < count; ++i) {
        de__unicode_props a = de__text_unicode(cp[i - 1]), b = de__text_unicode(cp[i]);
        u8 x = a.grapheme, y = b.grapheme;
        bool allow = true;
        if (x == GB_CR && y == GB_LF)
            allow = false;
        else if (control(x) || control(y))
            allow = true;
        else if (x == GB_L && (y == GB_L || y == GB_V || y == GB_LV || y == GB_LVT))
            allow = false;
        else if ((x == GB_LV || x == GB_V) && (y == GB_V || y == GB_T))
            allow = false;
        else if ((x == GB_LVT || x == GB_T) && y == GB_T)
            allow = false;
        else if (y == GB_EXTEND || y == GB_ZWJ || y == GB_SPACING || x == GB_PREPEND)
            allow = false;
        else if (b.incb == 1) {
            i32 j = (i32)i - 1;
            bool linker = false;
            while (j >= 0) {
                de__unicode_props p = de__text_unicode(cp[j]);
                if (p.incb == 3)
                    linker = true;
                else if (p.incb != 2)
                    break;
                --j;
            }
            if (linker && j >= 0 && de__text_unicode(cp[j]).incb == 1)
                allow = false;
        }
        if (allow && b.pictographic && x == GB_ZWJ) {
            i32 j = (i32)i - 2;
            while (j >= 0 && de__text_unicode(cp[j]).grapheme == GB_EXTEND)
                --j;
            if (j >= 0 && de__text_unicode(cp[j]).pictographic)
                allow = false;
        }
        if (allow && x == GB_RI && y == GB_RI) {
            u32 n = 0, j = i;
            while (j && de__text_unicode(cp[j - 1]).grapheme == GB_RI) {
                ++n;
                --j;
            }
            if (n & 1)
                allow = false;
        }
        breaks[i] = (u8)allow;
    }
}
