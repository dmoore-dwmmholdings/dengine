/*
 * fuzz_text_reader: the TOML text reader of the reflection serializer (DENG-295) on untrusted
 * text (DENG-297; SPEC §19.1: scenes, prefabs and saves can come from mods or downloads).
 *
 * Built twice (tests/fuzz/reflect/CMakeLists.txt):
 *   - fuzz_reflect_text: de_deserialize_object(DE_SER_TEXT) of the input into every test type,
 *     in the "current" and the "next" registry (fuzz_reflect.h), with and without a
 *     de_ser_context and with a required_flags filter, through a reader that hands out the text
 *     in uneven chunks;
 *   - fuzz_reflect_text_doc (FZ_TEXT_DOC): the multi-object document reader: de_text_doc_parse,
 *     then _count / _type / _read_object for the keys scene files use, each entry read as the
 *     type its __type names and as TAllKinds, on a counting allocator that must be empty after
 *     de_text_doc_destroy. An input that starts with the comment "#!" also makes one
 *     allocation fail: number 1 + (third byte & 63), counted over parse and reads.
 *
 * Every object that loads must re-encode stably (fuzz_reflect.h). Nothing survives between
 * inputs except the two registries, which are only read.
 */
#include "fuzz_reflect.h"

static const u32 k_req[] = {0, DE_FIELD_SERIALIZE, DE_FIELD_REPLICATE};

#ifndef FZ_TEXT_DOC

static void read_one(const fz_world *wd, u32 v, u32 t, const u8 *data, size_t size, u32 variant) {
    const de_type_registry *r = wd->reg[v];
    de_type_id id = wd->types[v][t];
    _Alignas(16) u8 obj[FZ_OBJ_MAX];
    fz_check(de_type_construct(r, id, obj) == DE_OK, "construct");
    de_ser_options o = {DE_SER_TEXT, k_req[variant % 3u], (variant & 1u) ? &g_fz_ctx : NULL};
    fz_reader fr;
    de_reader rd = fz_reader_make(&fr, data, size, variant % 5u == 4u ? 0u : 1u + variant % 13u);
    de_result res = de_deserialize_object(r, id, obj, &o, &rd);
    if (res == DE_OK) {
        fz_text_stable(r, id, obj, &o);
        fz_binary_stable(r, id, obj, &o, DE_SER_BINARY_SELF_DESCRIBING);
    }
    de_error_clear();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    const fz_world *wd = fz_world_get();
    /* Inputs are parsed once per (registry, type): 12 parses. The variant spreads the context,
     * filter and chunking choices over the types, the same way for every input. */
    for (u32 v = 0; v < 2; ++v)
        for (u32 t = 0; t < FZ_TYPE_COUNT; ++t)
            read_one(wd, v, t, data, size, (u32)(size + v * 7u + t));
    return 0;
}

#else /* FZ_TEXT_DOC */

static const char *const k_keys[] = {NULL, "entity", "prefab", "object"};

#define FZ_DOC_MAX_ENTRIES 32u

static void read_entry(const fz_world *wd, const de_text_doc *doc, const char *key, u32 index,
                       u32 v, de_type_id id, u32 variant) {
    const de_type_registry *r = wd->reg[v];
    _Alignas(16) u8 obj[FZ_OBJ_MAX];
    fz_check(de_type_construct(r, id, obj) == DE_OK, "construct");
    de_ser_options o = {DE_SER_TEXT, k_req[variant % 3u], (variant & 1u) ? &g_fz_ctx : NULL};
    de_result res = de_text_doc_read_object(doc, key, index, r, id, obj, &o);
    if (res == DE_OK) {
        fz_text_stable(r, id, obj, &o);
        fz_binary_stable(r, id, obj, &o, DE_SER_BINARY_COMPACT);
    }
    de_error_clear();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    const fz_world *wd = fz_world_get();
    fz_alloc counter = {0, 0, 0};
    if (size >= 3 && data[0] == '#' && data[1] == '!')
        counter.fail_at = 1u + (data[2] & 63u);
    de_allocator a = {fz_alloc_fn, fz_realloc_fn, fz_free_fn, &counter};

    de_text_doc *doc = NULL;
    de_result res = de_text_doc_parse(&a, (const char *)data, size, "fuzz", &doc);
    if (res != DE_OK) {
        fz_check(doc == NULL, "de_text_doc_parse failed but returned a document");
        fz_check(counter.live == 0, "de_text_doc_parse leaked after a failure");
        de_error_clear();
        return 0;
    }
    fz_check(doc != NULL, "de_text_doc_parse returned DE_OK without a document");

    u32 variant = (u32)size;
    for (u32 k = 0; k < sizeof k_keys / sizeof k_keys[0]; ++k) {
        const char *key = k_keys[k];
        u32 count = de_text_doc_count(doc, key);
        fz_check(key != NULL || count == 1, "the top-level table must count as one entry");
        /* Past the end: no entry. */
        fz_check(de_text_doc_type(doc, key, count) == NULL, "an entry past the end has a type");
        u32 n = count < FZ_DOC_MAX_ENTRIES ? count : FZ_DOC_MAX_ENTRIES;
        for (u32 i = 0; i < n; ++i) {
            const char *type_name = de_text_doc_type(doc, key, i);
            for (u32 v = 0; v < 2; ++v) {
                ++variant;
                if (type_name != NULL) {
                    de_type_id id = de_type_find(wd->reg[v], type_name);
                    if (de_type_get(wd->reg[v], id) != NULL)
                        read_entry(wd, doc, key, i, v, id, variant);
                }
                /* Every entry also as the type with every field kind. */
                if (i < 4)
                    read_entry(wd, doc, key, i, v, wd->types[v][5], variant + 1u);
            }
        }
        _Alignas(16) u8 obj[FZ_OBJ_MAX];
        fz_check(de_text_doc_read_object(doc, key, count, wd->reg[0], wd->types[0][0], obj, NULL) ==
                     DE_ERR_INVALID_ARGUMENT,
                 "reading an entry past the end must fail with DE_ERR_INVALID_ARGUMENT");
        de_error_clear();
    }
    de_text_doc_destroy(doc);
    fz_check(counter.live == 0, "de_text_doc_destroy left allocations behind");
    return 0;
}

#endif /* FZ_TEXT_DOC */
