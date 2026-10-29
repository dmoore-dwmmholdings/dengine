#include "editor_settings_value.h"
#include <math.h>
#include <string.h>

static void *alloc(de_ep_arena *a, size_t bytes) {
    const size_t begin = (a->used + 15u) & ~(size_t)15u;
    if (begin > a->size || bytes > a->size - begin)
        return NULL;
    void *p = a->base + begin;
    a->used = begin + bytes;
    memset(p, 0, bytes);
    return p;
}
static char *string(mpack_reader_t *r, mpack_tag_t *tag, de_ep_arena *a, u32 limit) {
    if (mpack_tag_type(tag) != mpack_type_str)
        return NULL;
    u32 n = mpack_tag_str_length(tag);
    if (n > limit)
        return NULL;
    char *s = alloc(a, (size_t)n + 1);
    if (!s)
        return NULL;
    mpack_read_bytes(r, s, n);
    mpack_done_str(r);
    if (mpack_reader_error(r) != mpack_ok || memchr(s, 0, n))
        return NULL;
    return s;
}
static bool value(mpack_reader_t *r, de_ep_arena *a, de__manifest_value *out, u32 depth,
                  u32 *nodes) {
    if (depth > 16 || ++*nodes > 4096)
        return false;
    mpack_tag_t tag = mpack_read_tag(r);
    switch (mpack_tag_type(&tag)) {
    case mpack_type_bool:
        out->type = DE__MANIFEST_BOOL;
        out->u.b = mpack_tag_bool_value(&tag);
        break;
    case mpack_type_int:
        out->type = DE__MANIFEST_INT;
        out->u.i = mpack_tag_int_value(&tag);
        break;
    case mpack_type_uint: {
        u64 n = mpack_tag_uint_value(&tag);
        if (n > INT64_MAX)
            return false;
        out->type = DE__MANIFEST_INT;
        out->u.i = (i64)n;
        break;
    }
    case mpack_type_float:
    case mpack_type_double:
        out->type = DE__MANIFEST_FLOAT;
        out->u.f = mpack_tag_type(&tag) == mpack_type_float ? (f64)mpack_tag_float_value(&tag)
                                                            : mpack_tag_double_value(&tag);
        if (!isfinite(out->u.f))
            return false;
        break;
    case mpack_type_str:
        out->type = DE__MANIFEST_STRING;
        out->u.s = string(r, &tag, a, 65536);
        if (!out->u.s)
            return false;
        break;
    case mpack_type_array: {
        const u32 n = mpack_tag_array_count(&tag);
        if (n > 4096)
            return false;
        de__manifest_value *items = alloc(a, (size_t)n * sizeof *items);
        if (!items)
            return false;
        bool strings = true;
        for (u32 i = 0; i < n; ++i) {
            if (!value(r, a, &items[i], depth + 1, nodes))
                return false;
            strings &= items[i].type == DE__MANIFEST_STRING;
        }
        mpack_done_array(r);
        if (strings) {
            const char **s = alloc(a, (size_t)n * sizeof *s);
            if (!s)
                return false;
            for (u32 i = 0; i < n; ++i)
                s[i] = items[i].u.s;
            out->type = DE__MANIFEST_STRING_ARRAY;
            out->u.arr.items = s;
            out->u.arr.count = n;
        } else {
            out->type = DE__MANIFEST_ARRAY;
            out->u.values.items = items;
            out->u.values.count = n;
        }
        break;
    }
    case mpack_type_map: {
        const u32 n = mpack_tag_map_count(&tag);
        if (n > 256)
            return false;
        const char **keys = alloc(a, (size_t)n * sizeof *keys);
        de__manifest_value *items = alloc(a, (size_t)n * sizeof *items);
        if (!keys || !items)
            return false;
        for (u32 i = 0; i < n; ++i) {
            mpack_tag_t key = mpack_read_tag(r);
            keys[i] = string(r, &key, a, 1024);
            if (!keys[i] || !value(r, a, &items[i], depth + 1, nodes))
                return false;
            for (u32 j = 0; j < i; ++j)
                if (!strcmp(keys[i], keys[j]))
                    return false;
        }
        mpack_done_map(r);
        for (u32 i = 1; i < n; ++i) {
            const char *key = keys[i];
            const de__manifest_value v = items[i];
            u32 j = i;
            while (j && strcmp(keys[j - 1], key) > 0) {
                keys[j] = keys[j - 1];
                items[j] = items[j - 1];
                --j;
            }
            keys[j] = key;
            items[j] = v;
        }
        out->type = DE__MANIFEST_TABLE;
        out->u.table.keys = keys;
        out->u.table.items = items;
        out->u.table.count = n;
        break;
    }
    default:
        return false;
    }
    return mpack_reader_error(r) == mpack_ok;
}
de_result de__ed_settings_values(const de_ep_map_raw *input, de_ep_arena *arena,
                                 de__manifest_kv **settings) {
    if (!input || !arena || !settings || input->count > 256)
        return DE_ERR_INVALID_ARGUMENT;
    de__manifest_kv *out = alloc(arena, (size_t)input->count * sizeof *out);
    if (!out)
        return DE_ERR_OUT_OF_MEMORY;
    u32 nodes = 0;
    for (u32 i = 0; i < input->count; ++i) {
        const de_ep_kv_raw *item = &input->items[i];
        if (!item->key.len || item->key.len > 1024 || memchr(item->key.ptr, 0, item->key.len))
            return DE_ERR_INVALID_ARGUMENT;
        char *key = alloc(arena, (size_t)item->key.len + 1);
        if (!key)
            return DE_ERR_OUT_OF_MEMORY;
        memcpy(key, item->key.ptr, item->key.len);
        out[i].key = key;
        for (u32 j = 0; j < i; ++j)
            if (!strcmp(out[j].key, key))
                return DE_ERR_INVALID_ARGUMENT;
        mpack_reader_t r;
        mpack_reader_init_data(&r, item->value.ptr, item->value.len);
        bool ok = value(&r, arena, &out[i].value, 0, &nodes);
        if (mpack_reader_remaining(&r, NULL) != 0)
            ok = false;
        if (mpack_reader_destroy(&r) != mpack_ok || !ok)
            return DE_ERR_INVALID_ARGUMENT;
    }
    *settings = out;
    return DE_OK;
}
