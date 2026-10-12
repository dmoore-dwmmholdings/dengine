/*
 * asset_id.c: asset IDs (SPEC §11.1; DENG-139).
 *
 * An asset ID is de_hash64 (XXH3-64, seed 0) of the exact bytes of its path, so it is the same
 * on every platform and never depends on where the source file is. The hash is part of the pack
 * format (S08): see Trama "Decision: Names and asset IDs are XXH3-64 of the exact bytes". The
 * path is checked, never normalised: two spellings of one asset would be two IDs, so a spelling
 * that is not canonical is refused instead.
 *
 * Called from game code, so it allocates nothing and touches no shared state (DE_FAIL only
 * writes the calling thread's error context).
 */

#include "asset/asset_internal.h"
#include "asset/resolve_internal.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>

#include <string.h>

/* Length of the UTF-8 sequence at `s` (at most `n` bytes available), or 0 if it is not valid
 * UTF-8: no overlong forms, no surrogates, nothing above U+10FFFF. */
static size_t id_utf8_len(const unsigned char *s, size_t n) {
    unsigned char c = s[0];
    if (c < 0x80)
        return 1;
    size_t len;
    unsigned char lo = 0x80, hi = 0xBF; /* allowed range of the second byte */
    if (c >= 0xC2 && c <= 0xDF) {
        len = 2;
    } else if (c >= 0xE0 && c <= 0xEF) {
        len = 3;
        if (c == 0xE0)
            lo = 0xA0; /* overlong */
        else if (c == 0xED)
            hi = 0x9F; /* surrogates */
    } else if (c >= 0xF0 && c <= 0xF4) {
        len = 4;
        if (c == 0xF0)
            lo = 0x90; /* overlong */
        else if (c == 0xF4)
            hi = 0x8F; /* above U+10FFFF */
    } else {
        return 0;
    }
    if (len > n || s[1] < lo || s[1] > hi)
        return 0;
    for (size_t i = 2; i < len; ++i)
        if (s[i] < 0x80 || s[i] > 0xBF)
            return 0;
    return len;
}

/* Checks the characters of `len` bytes: valid UTF-8, no control characters, and none of the
 * bytes that have another meaning in IDs or bundle entries (`\` is not a separator; `*` and `?`
 * would make a bundle entry a glob). NULL if fine. */
static const char *id_check_chars(const char *p, size_t len) {
    const unsigned char *s = (const unsigned char *)p;
    for (size_t i = 0; i < len;) {
        unsigned char c = s[i];
        if (c < 0x20 || c == 0x7F)
            return "control character";
        if (c == '\\')
            return "'\\' (the separator is '/')";
        if (c == '*' || c == '?')
            return "glob character ('*' or '?')";
        size_t n = id_utf8_len(s + i, len - i);
        if (n == 0)
            return "invalid UTF-8";
        i += n;
    }
    return NULL;
}

const char *de__asset_id_check(const char *path, size_t len) {
    if (path == NULL || len == 0)
        return "empty ID";
    const char *hash = memchr(path, '#', len);
    size_t path_len = hash != NULL ? (size_t)(hash - path) : len;
    if (hash != NULL) {
        size_t sub_len = len - path_len - 1;
        if (sub_len == 0)
            return "empty sub-asset name after '#'";
        if (memchr(hash + 1, '#', sub_len) != NULL)
            return "more than one '#'";
        const char *why = id_check_chars(hash + 1, sub_len);
        if (why != NULL)
            return why;
    }
    if (path_len == 0)
        return "empty path before '#'";
    if (path[0] == '/')
        return "leading '/'";
    if (path[path_len - 1] == '/')
        return "trailing '/'";
    const char *why = id_check_chars(path, path_len);
    if (why != NULL)
        return why;
    /* Segments: none empty, none "." or "..". */
    size_t start = 0;
    for (size_t i = 0; i <= path_len; ++i) {
        if (i < path_len && path[i] != '/')
            continue;
        size_t seg = i - start;
        if (seg == 0)
            return "empty segment ('//')";
        if ((seg == 1 && path[start] == '.') ||
            (seg == 2 && path[start] == '.' && path[start + 1] == '.'))
            return "'.' or '..' segment";
        start = i + 1;
    }
    return NULL;
}

de_asset_id de_asset_id_from(const char *path) {
    if (path == NULL) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_asset_id_from: path is NULL");
        return (de_asset_id){0};
    }
    size_t len = strlen(path);
    const char *why = de__asset_id_check(path, len);
    if (why != NULL) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_asset_id_from: \"%.200s\" is not an asset ID: %s",
                path, why);
        return (de_asset_id){0};
    }
    return de__asset_id_make(de_hash64(path, len, 0));
}

const char *de_asset_id_path(const de_asset_system *s, de_asset_id id) {
    return s != NULL ? de__resolved_path(s->resolved, id) : NULL;
}
