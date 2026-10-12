/*
 * glob.c: bundle globs on asset IDs (SPEC §11.2; DENG-139). Globs match asset IDs, never files.
 *
 * Syntax (resolve_internal.h): `*` = any run except '/', `**` = any run, `?` = one character
 * except '/', anything else literal. A sub-asset ID ("model#Node") never matches.
 *
 * The matcher is the classic iterative wildcard match with backtracking, extended to two kinds of
 * star. It keeps one resume point per kind:
 *   - the most recent `*`: on a mismatch it takes one more character, unless that character is
 *     '/' (a `*` never crosses a segment);
 *   - the most recent `**`: when the `*` cannot grow (or there is none after the `**`), the `**`
 *     takes one more character and matching restarts after it.
 * Earlier stars never need to be retried: a later `**` can absorb anything an earlier star
 * could, and an earlier `*` is cut off by the '/' that the pattern matched after it. This keeps
 * the match polynomial (no exponential backtracking on patterns like `*a*a*a*b`) and
 * allocation-free. tests/unit/asset/test_glob.c checks it against a brute-force matcher.
 */

#include "asset/resolve_internal.h"

#include <dengine/de_base.h>

#include <string.h>

/* Bytes of the character at `s` (a UTF-8 sequence, cut short at a NUL or a non-continuation
 * byte). `*s` must not be NUL. */
static size_t glob_char_len(const char *p) {
    const unsigned char *s = (const unsigned char *)p;
    size_t n = 1;
    if (s[0] >= 0xF0)
        n = 4;
    else if (s[0] >= 0xE0)
        n = 3;
    else if (s[0] >= 0xC0)
        n = 2;
    for (size_t i = 1; i < n; ++i)
        if ((s[i] & 0xC0) != 0x80)
            return i;
    return n;
}

bool de__glob_is_pattern(const char *s) {
    return s != NULL && strpbrk(s, "*?") != NULL;
}

bool de__glob_match(const char *glob, const char *id) {
    if (glob == NULL || id == NULL || strchr(id, '#') != NULL)
        return false;
    const char *g = glob, *s = id;
    const char *star_g = NULL, *star_s = NULL;   /* after the last `*`; where its text ends */
    const char *dstar_g = NULL, *dstar_s = NULL; /* the same for the last `**` */
    for (;;) {
        if (*g == '*') {
            size_t stars = 0;
            while (*g == '*') {
                ++g;
                ++stars;
            }
            if (stars >= 2) {
                dstar_g = g;
                dstar_s = s;
                star_g = NULL; /* the `**` subsumes every earlier star */
            } else {
                star_g = g;
                star_s = s;
            }
            continue;
        }
        if (*s == '\0')
            return *g == '\0'; /* stars only ever take more text: no retry can help */
        if (*g == '?' && *s != '/') {
            ++g;
            s += glob_char_len(s);
            continue;
        }
        if (*g != '\0' && *g != '?' && *g == *s) {
            ++g;
            ++s;
            continue;
        }
        /* Mismatch: let the most recent star take one more character. */
        if (star_g != NULL && *star_s != '/') {
            star_s += glob_char_len(star_s);
            s = star_s;
            g = star_g;
            continue;
        }
        if (dstar_g != NULL && *dstar_s != '\0') {
            dstar_s += glob_char_len(dstar_s);
            s = dstar_s;
            g = dstar_g;
            star_g = NULL;
            continue;
        }
        return false;
    }
}
