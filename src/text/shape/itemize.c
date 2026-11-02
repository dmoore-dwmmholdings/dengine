#include "shaper_internal.h"
#include "text/fallback_internal.h"
#include <string.h>
u32 de__text_utf8(const char *s, u32 n, u32 *off) {
    u32 at = *off;
    if (at >= n)
        return 0;
    u8 b = (u8)s[at++];
    if (b < 128) {
        *off = at;
        return b;
    }
    u32 cp, k, min;
    if (b >= 0xc2 && b <= 0xdf) {
        cp = b & 31;
        k = 1;
        min = 0x80;
    } else if (b >= 0xe0 && b <= 0xef) {
        cp = b & 15;
        k = 2;
        min = 0x800;
    } else if (b >= 0xf0 && b <= 0xf4) {
        cp = b & 7;
        k = 3;
        min = 0x10000;
    } else {
        ++*off;
        return 0xfffd;
    }
    if (k > n - at) {
        ++*off;
        return 0xfffd;
    }
    for (u32 i = 0; i < k; ++i) {
        u8 q = (u8)s[at++];
        if ((q & 192) != 128) {
            ++*off;
            return 0xfffd;
        }
        cp = cp << 6 | (q & 63);
    }
    if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
        ++*off;
        return 0xfffd;
    }
    *off = at;
    return cp;
}
de_result de_text__itemize(de_text_system *t, de_font primary, de_str text, const de_allocator *a,
                           de_text_item **out, u32 *count) {
    if (!out || !count || (!text.ptr && text.len) || text.len > UINT32_MAX)
        return DE_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *count = 0;
    if (!text.len)
        return DE_OK;
    de_text_item *items =
        DE_ALLOC(a, (u64)text.len * sizeof *items, _Alignof(de_text_item), DE_MEM_UI);
    if (!items)
        return DE_ERR_OUT_OF_MEMORY;
    /* Leading weak characters all look toward the same first strong script.
     * Scan once, outside the fallback lock, rather than rescanning every suffix. */
    hb_script_t leading = HB_SCRIPT_UNKNOWN;
    for (u32 look = 0; look < text.len;) {
        u32 cp = de__text_utf8(text.ptr, (u32)text.len, &look);
        hb_script_t script = hb_unicode_script(hb_unicode_funcs_get_default(), cp);
        if (script != HB_SCRIPT_COMMON && script != HB_SCRIPT_INHERITED &&
            script != HB_SCRIPT_UNKNOWN) {
            leading = script;
            break;
        }
    }
    u32 at = 0;
    u32 batch_count = 0;
    hb_script_t previous = HB_SCRIPT_UNKNOWN;
    de_font previous_font = primary;
    de__text_resolver resolver = de__text_resolve_begin(t, primary);
    while (at < text.len) {
        u32 start = at, cp = de__text_utf8(text.ptr, (u32)text.len, &at);
        de_font font;
        u32 glyph;
        (void)de__text_resolve_next(&resolver, cp, &font, &glyph);
        hb_script_t script = hb_unicode_script(hb_unicode_funcs_get_default(), cp);
        bool weak = script == HB_SCRIPT_COMMON || script == HB_SCRIPT_INHERITED ||
                    script == HB_SCRIPT_UNKNOWN;
        if (weak) {
            if (previous != HB_SCRIPT_UNKNOWN) {
                bool inherited = script == HB_SCRIPT_INHERITED;
                script = previous;
                /* Keep a covered mark in its base's shaping run; otherwise retain fallback. */
                if (cp == 0x20 || cp == 0x09 ||
                    (inherited && de_text__codepoint(de_text__font_get(t, previous_font), cp)))
                    font = previous_font;
            } else if (leading != HB_SCRIPT_UNKNOWN)
                script = leading;
        }
        if (*count && items[*count - 1].font.id == font.id && items[*count - 1].script == script)
            items[*count - 1].end = at;
        else
            items[(*count)++] = (de_text_item){start, at, font, script};
        if (!weak) {
            previous = script;
            previous_font = font;
        }
        /* Release periodically so long strings do not monopolize worker lookups
         * or fallback-chain updates. No prepared chain survives the unlock. */
        if (++batch_count == 128 && at < text.len) {
            de__text_resolve_end(&resolver);
            resolver = de__text_resolve_begin(t, primary);
            batch_count = 0;
        }
    }
    de__text_resolve_end(&resolver);
    *out = items;
    return DE_OK;
}
