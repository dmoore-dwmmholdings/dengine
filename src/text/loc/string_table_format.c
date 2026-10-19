#include "text/loc/string_table_format.h"
#include "text/loc/loc_internal.h"
#include <stdlib.h>
#include <string.h>
static bool utf8(const u8 *p, size_t n) {
    for (size_t i = 0; i < n;) {
        u32 c = p[i++], need = 0, min = 0;
        if (c < 0x80) {
            if (!c)
                return false;
            continue;
        }
        if (c >= 0xc2 && c <= 0xdf) {
            need = 1;
            min = 0x80;
            c &= 31;
        } else if (c >= 0xe0 && c <= 0xef) {
            need = 2;
            min = 0x800;
            c &= 15;
        } else if (c >= 0xf0 && c <= 0xf4) {
            need = 3;
            min = 0x10000;
            c &= 7;
        } else
            return false;
        if (need > n - i)
            return false;
        while (need--) {
            u32 b = p[i++];
            if ((b & 0xc0) != 0x80)
                return false;
            c = (c << 6) | (b & 63);
        }
        if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
            return false;
    }
    return true;
}
bool de__loc_valid_locale(const char *locale) {
    if (!locale)
        return false;
    size_t n = strlen(locale);
    if (!n || n > 35 || locale[0] == '-' || locale[n - 1] == '-')
        return false;
    for (size_t i = 0; i < n; ++i)
        if (!((locale[i] >= 'a' && locale[i] <= 'z') || (locale[i] >= 'A' && locale[i] <= 'Z') ||
              (locale[i] >= '0' && locale[i] <= '9') ||
              (locale[i] == '-' && i && locale[i - 1] != '-')))
            return false;
    return true;
}
de_result de__loc_table_parse(const void *bytes, u64 size, de__loc_table *out) {
    if (out)
        memset(out, 0, sizeof *out);
    if (!bytes || !out || size < 32 || size > UINT32_MAX)
        return DE_FAIL(DE_ERR_PARSE, "invalid locale table size");
    const u8 *p = bytes;
    u32 count = de__loc_u32(p + 8), blob = de__loc_u32(p + 16), blob_size = de__loc_u32(p + 20);
    if (memcmp(p, "DLOC", 4) || de__loc_u32(p + 4) != 1 || de__loc_u32(p + 12) != 32 ||
        count > (size - 32) / 16 || blob != 32 + (u64)count * 16 || blob_size != size - blob ||
        de__loc_u32(p + 24) != size || de__loc_u32(p + 28))
        return DE_FAIL(DE_ERR_PARSE, "invalid locale table header");
    u64 previous = 0;
    for (u32 i = 0; i < count; ++i) {
        const u8 *entry = p + 32 + (u64)i * 16;
        u64 hash = de__loc_u64(entry);
        u32 offset = de__loc_u32(entry + 8), length = de__loc_u32(entry + 12);
        if ((i && hash <= previous) || offset >= blob_size || length >= blob_size - offset ||
            p[blob + offset + length] || !utf8(p + blob + offset, length))
            return DE_FAIL(DE_ERR_PARSE, "invalid locale table entry");
        size_t error = 0;
        if (de__loc_validate((const char *)p + blob + offset, length, &error) != DE_OK)
            return DE_FAIL(DE_ERR_PARSE, "invalid cooked message at entry %u byte %zu", i, error);
        previous = hash;
    }
    *out = (de__loc_table){p, size, count, blob};
    return DE_OK;
}
de_str de__loc_table_lookup(const de__loc_table *table, de_name id) {
    if (!table || !table->data)
        return (de_str){NULL, 0};
    u32 lo = 0, hi = table->count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        const u8 *entry = table->data + 32 + (u64)mid * 16;
        u64 hash = de__loc_u64(entry);
        if (hash == id.hash)
            return (de_str){(const char *)table->data + table->blob + de__loc_u32(entry + 8),
                            de__loc_u32(entry + 12)};
        if (hash < id.hash)
            lo = mid + 1;
        else
            hi = mid;
    }
    return (de_str){NULL, 0};
}
static int compare(const void *a, const void *b) {
    u64 x = ((const de__loc_pair *)a)->hash, y = ((const de__loc_pair *)b)->hash;
    return x < y ? -1 : x > y ? 1 : 0;
}
de_result de__loc_table_build(const de_allocator *a, de__loc_pair *pairs, u32 count, void **out,
                              u64 *bytes) {
    *out = NULL;
    *bytes = 0;
    u64 blob = 32 + (u64)count * 16, total = blob;
    for (u32 i = 0; i < count; ++i) {
        pairs[i].hash = de_hash64_str(pairs[i].key);
        total += pairs[i].length + 1;
    }
    if (total > UINT32_MAX)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "locale table exceeds 4GiB");
    qsort(pairs, count, sizeof *pairs, compare);
    for (u32 i = 1; i < count; ++i)
        if (pairs[i].hash == pairs[i - 1].hash)
            return DE_FAIL(DE_ERR_ALREADY_EXISTS, "duplicate/colliding string key '%s'",
                           pairs[i].key);
    u8 *p = DE_ALLOC(a, (size_t)total, 16, DE_MEM_UI);
    if (!p)
        return DE_ERR_OUT_OF_MEMORY;
    memset(p, 0, 32);
    memcpy(p, "DLOC", 4);
    de__loc_w32(p + 4, 1);
    de__loc_w32(p + 8, count);
    de__loc_w32(p + 12, 32);
    de__loc_w32(p + 16, (u32)blob);
    de__loc_w32(p + 20, (u32)(total - blob));
    de__loc_w32(p + 24, (u32)total);
    u64 offset = 0;
    for (u32 i = 0; i < count; ++i) {
        u8 *entry = p + 32 + (u64)i * 16;
        de__loc_w64(entry, pairs[i].hash);
        de__loc_w32(entry + 8, (u32)offset);
        de__loc_w32(entry + 12, (u32)pairs[i].length);
        memcpy(p + blob + offset, pairs[i].value, pairs[i].length);
        p[blob + offset + pairs[i].length] = 0;
        offset += pairs[i].length + 1;
    }
    de__loc_table check;
    de_result r = de__loc_table_parse(p, total, &check);
    if (r != DE_OK) {
        DE_FREE(a, p, (size_t)total);
        return r;
    }
    *out = p;
    *bytes = total;
    return DE_OK;
}
