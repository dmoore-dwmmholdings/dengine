/*
 * settings_hash.c: the canonical hash of an asset's import settings (DENG-209, SPEC §11.5). The
 * `.decache` key (DENG-210) includes it, so the encoding (de_cooker.h, de_cook_settings_hash)
 * must not change without a cache version bump.
 *
 * The values are streamed into XXH3-64 (seed 0) in key order; nothing is allocated, so a long
 * settings list costs O(n^2) comparisons (a manifest asset has a handful of settings).
 */

#include "cooker_internal.h"

#include <dengine/de_base.h>

#include <string.h>

/* The streaming state (XXH3_state_t) is declared for static linking only; de_tp_xxhash is a
 * static library of the same version. */
#define XXH_STATIC_LINKING_ONLY
#include <xxhash.h>

enum {
    TAG_BOOL = 1,
    TAG_INT = 2,
    TAG_FLOAT = 3,
    TAG_STRING = 4,
    TAG_STRING_ARRAY = 5,
    TAG_ARRAY = 6,
    TAG_TABLE = 7,
    SETTINGS_HASH_VERSION = 1
};

static void put_u8(XXH3_state_t *st, u8 v) {
    (void)XXH3_64bits_update(st, &v, 1);
}

static void put_u32(XXH3_state_t *st, u32 v) {
    u8 b[4];
    for (u32 i = 0; i < 4; ++i)
        b[i] = (u8)(v >> (8u * i));
    (void)XXH3_64bits_update(st, b, sizeof b);
}

static void put_u64(XXH3_state_t *st, u64 v) {
    u8 b[8];
    for (u32 i = 0; i < 8; ++i)
        b[i] = (u8)(v >> (8u * i));
    (void)XXH3_64bits_update(st, b, sizeof b);
}

static void put_str(XXH3_state_t *st, const char *s) {
    if (s == NULL)
        s = "";
    size_t n = strlen(s);
    put_u32(st, (u32)n);
    if (n != 0)
        (void)XXH3_64bits_update(st, s, n);
}

static void put_value(XXH3_state_t *st, const de__manifest_value *v) {
    switch (v->type) {
    case DE__MANIFEST_BOOL:
        put_u8(st, TAG_BOOL);
        put_u8(st, v->u.b ? 1u : 0u);
        break;
    case DE__MANIFEST_INT:
        put_u8(st, TAG_INT);
        put_u64(st, (u64)v->u.i);
        break;
    case DE__MANIFEST_FLOAT: {
        u64 bits = 0;
        memcpy(&bits, &v->u.f, sizeof bits);
        put_u8(st, TAG_FLOAT);
        put_u64(st, bits);
        break;
    }
    case DE__MANIFEST_STRING:
        put_u8(st, TAG_STRING);
        put_str(st, v->u.s);
        break;
    case DE__MANIFEST_STRING_ARRAY:
        put_u8(st, TAG_STRING_ARRAY);
        put_u32(st, v->u.arr.count);
        for (u32 i = 0; i < v->u.arr.count; ++i)
            put_str(st, v->u.arr.items[i]);
        break;
    case DE__MANIFEST_ARRAY:
        put_u8(st, TAG_ARRAY);
        put_u32(st, v->u.values.count);
        for (u32 i = 0; i < v->u.values.count; ++i)
            put_value(st, &v->u.values.items[i]);
        break;
    case DE__MANIFEST_TABLE: {
        put_u8(st, TAG_TABLE);
        put_u32(st, v->u.table.count);
        /* External callers may supply unsorted views. Select (key,index) order
         * without mutating borrowed storage or allocating hash scratch. */
        u32 last = UINT32_MAX;
        for (u32 done = 0; done < v->u.table.count; ++done) {
            u32 pick = UINT32_MAX;
            for (u32 i = 0; i < v->u.table.count; ++i) {
                int after =
                    last == UINT32_MAX ? 1 : strcmp(v->u.table.keys[i], v->u.table.keys[last]);
                if (last != UINT32_MAX && (after < 0 || (after == 0 && i <= last)))
                    continue;
                int order =
                    pick == UINT32_MAX ? -1 : strcmp(v->u.table.keys[i], v->u.table.keys[pick]);
                if (pick == UINT32_MAX || order < 0 || (order == 0 && i < pick))
                    pick = i;
            }
            put_str(st, v->u.table.keys[pick]);
            put_value(st, &v->u.table.items[pick]);
            last = pick;
        }
        break;
    }
    default:
        /* Not a manifest type: hash the raw tag so it still changes the hash. */
        put_u8(st, (u8)(0x80u | ((u32)v->type & 0x7Fu)));
        break;
    }
}

/* (key, index) order: bytewise keys; equal keys keep their list order. */
static bool before(const de__manifest_kv *s, u32 a, u32 b) {
    int c = strcmp(s[a].key != NULL ? s[a].key : "", s[b].key != NULL ? s[b].key : "");
    return c < 0 || (c == 0 && a < b);
}

u64 de_cook_settings_hash(const de__manifest_kv *settings, u32 count) {
    XXH3_state_t st;
    XXH3_INITSTATE(&st);
    (void)XXH3_64bits_reset(&st);
    static const char k_magic[4] = {'D', 'E', 'S', 'H'};
    (void)XXH3_64bits_update(&st, k_magic, sizeof k_magic);
    put_u32(&st, SETTINGS_HASH_VERSION);
    if (settings == NULL)
        count = 0;
    put_u32(&st, count);
    /* Selection in (key, index) order: each step takes the smallest entry after the last one. */
    u32 last = UINT32_MAX;
    for (u32 done = 0; done < count; ++done) {
        u32 pick = UINT32_MAX;
        for (u32 i = 0; i < count; ++i) {
            if (last != UINT32_MAX && !before(settings, last, i))
                continue;
            if (pick == UINT32_MAX || before(settings, i, pick))
                pick = i;
        }
        put_str(&st, settings[pick].key);
        put_value(&st, &settings[pick].value);
        last = pick;
    }
    return (u64)XXH3_64bits_digest(&st);
}
