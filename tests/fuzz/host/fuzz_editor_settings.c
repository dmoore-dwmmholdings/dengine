#include "de_fuzz.h"
#include "host/editor/editor_settings_value.h"
#include <stdlib.h>
#include <string.h>

static const u8 *begin, *end;
static void range(const void *ptr, size_t bytes) {
    uintptr_t p = (uintptr_t)ptr, lo = (uintptr_t)begin, hi = (uintptr_t)end;
    if (p < lo || p > hi || bytes > hi - p)
        abort();
}
static void string(const char *s) {
    range(s, 1);
    if (!memchr(s, 0, (size_t)(end - (const u8 *)s)))
        abort();
}
static void inspect(const de__manifest_value *v, u32 depth) {
    if (depth > 16)
        abort();
    range(v, sizeof *v);
    if (v->type == DE__MANIFEST_STRING)
        string(v->u.s);
    else if (v->type == DE__MANIFEST_STRING_ARRAY) {
        if (v->u.arr.count > 4096)
            abort();
        range(v->u.arr.items, (size_t)v->u.arr.count * sizeof(char *));
        for (u32 i = 0; i < v->u.arr.count; ++i)
            string(v->u.arr.items[i]);
    } else if (v->type == DE__MANIFEST_ARRAY) {
        if (v->u.values.count > 4096)
            abort();
        range(v->u.values.items, (size_t)v->u.values.count * sizeof *v);
        for (u32 i = 0; i < v->u.values.count; ++i)
            inspect(&v->u.values.items[i], depth + 1);
    } else if (v->type == DE__MANIFEST_TABLE) {
        if (v->u.table.count > 256)
            abort();
        range(v->u.table.keys, (size_t)v->u.table.count * sizeof(char *));
        range(v->u.table.items, (size_t)v->u.table.count * sizeof *v);
        for (u32 i = 0; i < v->u.table.count; ++i) {
            string(v->u.table.keys[i]);
            inspect(&v->u.table.items[i], depth + 1);
        }
    }
}
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 256u * 1024)
        return 0;
    u8 *storage = malloc(1u << 20);
    if (!storage)
        abort();
    begin = storage;
    end = storage + (1u << 20);
    de_ep_arena arena = {storage, 1u << 20, 0};
    de_ep_kv_raw item = {.key = {"fuzz", 4}, .value = {(const char *)data, size}};
    de_ep_map_raw input = {&item, 1};
    de__manifest_kv *out = NULL;
    de_result result = de__ed_settings_values(&input, &arena, &out);
    if (arena.used > arena.size)
        abort();
    if (result == DE_OK) {
        range(out, sizeof *out);
        string(out->key);
        inspect(&out->value, 0);
    } else if (out || (result != DE_ERR_INVALID_ARGUMENT && result != DE_ERR_OUT_OF_MEMORY))
        abort();
    free(storage);
    begin = end = NULL;
    return 0;
}
