/*
 * fuzz_reflect.h: shared setup of the reflect fuzz targets (DENG-297): fuzz_text_reader.c
 * (single objects and multi-object documents) and fuzz_binary_reader.c.
 *
 * Two registries are built once, on the first input, and only read afterwards:
 *   - "current": the test types of tests/unit/reflect/test_types.h, as the unit tests use them;
 *   - "next":    the same type names with changed layouts and versions (renamed, removed, added
 *                and resized fields, a kind change, an enum size change) and migrate hooks, so
 *                data written for "current" goes through version migration and, for binary, the
 *                stored-schema path.
 * Every reader runs against both. The migrate hook reads every byte of every old field it is
 * given, so AddressSanitizer reports an old_fields description that points outside old_data.
 *
 * After a successful read, both targets check that the loaded object re-encodes stably: write
 * (bytes1) -> read -> write (bytes2) gives bytes1 == bytes2, for text and binary. A difference is
 * a bug: the writer produced data its own reader loads differently.
 */
#ifndef DE_FUZZ_REFLECT_H
#define DE_FUZZ_REFLECT_H

#include "de_fuzz.h"
#include "test_types.h"

#include <dengine/de_memory.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Largest object of any fuzz type (TAllKinds is the biggest), with room to spare. */
#define FZ_OBJ_MAX 1024u

static inline void fz_check(int ok, const char *what) {
    if (ok)
        return;
    const de_error *e = de_error_last();
    fprintf(stderr, "fuzz reflect: %s (last error: %s)\n", what,
            e && e->result != DE_OK ? e->message : "none");
    abort();
}

/* ---- The "next" layouts --------------------------------------------------------------------- */

typedef struct FInventory2 {
    u16 count;
    TItem items[3];
    u32 added;
} FInventory2;

static const de_field_info k_finventory2_fields[] = {
    DE_FIELD(FInventory2, count, DE_FK_U16, DE_FIELD_DEFAULT),
    DE_FIELD_EX(FInventory2, items, DE_FK_FIXED_ARRAY, DE_FIELD_DEFAULT, {0, 0, 0, 0, "TItem", 3}),
    DE_FIELD(FInventory2, added, DE_FK_U32, DE_FIELD_DEFAULT),
};

typedef struct FEntityData2 {
    TTransform transform;
    FInventory2 inventory;
    u16 mode; /* TMode, 2 bytes now */
} FEntityData2;

static const de_field_info k_fentitydata2_fields[] = {
    DE_FIELD_EX(FEntityData2, transform, DE_FK_STRUCT, DE_FIELD_DEFAULT,
                {0, 0, 0, 0, "TTransform", 0}),
    DE_FIELD_EX(FEntityData2, inventory, DE_FK_STRUCT, DE_FIELD_DEFAULT,
                {0, 0, 0, 0, "TInventory", 0}),
    DE_FIELD_EX(FEntityData2, mode, DE_FK_ENUM, DE_FIELD_DEFAULT, {0, 0, 0, 0, "TMode", 0}),
};

typedef struct FAllKinds2 {
    de_mat4 m4;
    f64 f64v;
    i64 i32v; /* I32 -> I64: a kind change */
    u64 entity;
    de_name name;
    u32 renamed; /* was u32v */
    u32 mode;    /* TMode, 4 bytes now */
    TMeshHandle mesh;
    TVec vec;
    de_vec3 v3;
    f32 samples[6]; /* was 4 */
    TItem items[1]; /* was 2 */
    u8 layers;      /* TLayers, 1 byte now */
    bool b;
    char str[8];   /* was 32 */
    char extra[4]; /* new */
} FAllKinds2;

static const de_field_info k_fallkinds2_fields[] = {
    DE_FIELD(FAllKinds2, m4, DE_FK_MAT4, DE_FIELD_DEFAULT),
    DE_FIELD(FAllKinds2, f64v, DE_FK_F64, DE_FIELD_DEFAULT),
    DE_FIELD(FAllKinds2, i32v, DE_FK_I64, DE_FIELD_DEFAULT),
    DE_FIELD(FAllKinds2, entity, DE_FK_ENTITY, DE_FIELD_DEFAULT),
    DE_FIELD(FAllKinds2, name, DE_FK_NAME, DE_FIELD_DEFAULT),
    DE_FIELD(FAllKinds2, renamed, DE_FK_U32, DE_FIELD_DEFAULT),
    DE_FIELD_EX(FAllKinds2, mode, DE_FK_ENUM, DE_FIELD_DEFAULT, {0, 0, 0, 0, "TMode", 0}),
    DE_FIELD_EX(FAllKinds2, mesh, DE_FK_ASSET, DE_FIELD_DEFAULT | DE_FIELD_ASSET_REF,
                {0, 0, 0, 0, "mesh", 0}),
    DE_FIELD_EX(FAllKinds2, vec, DE_FK_STRUCT, DE_FIELD_DEFAULT, {0, 0, 0, 0, "TVec", 0}),
    DE_FIELD(FAllKinds2, v3, DE_FK_VEC3, DE_FIELD_DEFAULT),
    DE_FIELD_EX(FAllKinds2, samples, DE_FK_FIXED_ARRAY, DE_FIELD_DEFAULT, {0, 0, 0, 0, "f32", 6}),
    DE_FIELD_EX(FAllKinds2, items, DE_FK_FIXED_ARRAY, DE_FIELD_DEFAULT, {0, 0, 0, 0, "TItem", 1}),
    DE_FIELD_EX(FAllKinds2, layers, DE_FK_FLAGS, DE_FIELD_DEFAULT, {0, 0, 0, 0, "TLayers", 0}),
    DE_FIELD(FAllKinds2, b, DE_FK_BOOL, DE_FIELD_DEFAULT | DE_FIELD_REPLICATE),
    DE_FIELD(FAllKinds2, str, DE_FK_STRING, DE_FIELD_DEFAULT),
    DE_FIELD(FAllKinds2, extra, DE_FK_STRING, DE_FIELD_SERIALIZE),
};

/*
 * The migrate hook of every "next" type. It touches every byte the old fields describe and every
 * string they name (ASan catches a description that points outside the old data), copies the
 * old `u32v` into `renamed` when it fits, and drops the object for one stored version in three,
 * so both outcomes are exercised.
 */
static volatile u8 g_fz_sink;

static inline bool fz_migrate_common(u32 from_version, const void *old_data,
                                     const de_field_info *old, u32 n) {
    const u8 *base = (const u8 *)old_data;
    u8 acc = 0;
    for (u32 i = 0; i < n; ++i) {
        fz_check(old[i].name != NULL, "migrate: old field without a name");
        acc ^= (u8)strlen(old[i].name);
        if (old[i].attrs.nested_type != NULL)
            acc ^= (u8)strlen(old[i].attrs.nested_type);
        for (u32 b = 0; b < old[i].size; ++b)
            acc ^= base[old[i].offset + b];
    }
    g_fz_sink = acc;
    return from_version % 3u != 2u;
}

static inline bool fz_migrate_allkinds(void *obj, u32 from_version, const void *old_data,
                                       const de_field_info *old, u32 n) {
    bool keep = fz_migrate_common(from_version, old_data, old, n);
    FAllKinds2 *o = (FAllKinds2 *)obj;
    for (u32 i = 0; i < n; ++i) {
        if (strcmp(old[i].name, "u32v") != 0)
            continue;
        /* Text gives integers as I64, binary keeps U32: read either, little-endian. */
        if (old[i].size == 4 || old[i].size == 8) {
            u64 v = 0;
            memcpy(&v, (const u8 *)old_data + old[i].offset, old[i].size);
            o->renamed = (u32)v;
        }
    }
    return keep;
}

static inline bool fz_migrate_plain(void *obj, u32 from_version, const void *old_data,
                                    const de_field_info *old, u32 n) {
    (void)obj;
    return fz_migrate_common(from_version, old_data, old, n);
}

/* ---- Registries ----------------------------------------------------------------------------- */

#define FZ_TYPE_COUNT 6u

typedef struct fz_world {
    de_type_registry *reg[2]; /* current, next */
    de_type_id types[2][FZ_TYPE_COUNT];
    de_log_category log;
} fz_world;

static fz_world g_fz;

static inline void fz_register_next(de_type_registry *r, de_type_id *out) {
    fz_check(de_enum_register(r, "TMode", k_tmode_values, TEST_COUNT_OF(k_tmode_values), false) ==
                 DE_OK,
             "next: TMode");
    fz_check(de_enum_register(r, "TLayers", k_tlayers_values, TEST_COUNT_OF(k_tlayers_values),
                              true) == DE_OK,
             "next: TLayers");
    de_type_desc d;
    /* TVec: same layout, version 2, no hook (the lenient text path, binary name matching). */
    d = DE_TYPE_DESC(TVec, 2, k_tvec_fields);
    fz_check(de_type_register_ex(r, &d, &out[0], NULL) == DE_OK, "next: TVec");
    d = DE_TYPE_DESC(TTransform, 1, k_ttransform_fields);
    fz_check(de_type_register_ex(r, &d, &out[1], NULL) == DE_OK, "next: TTransform");
    d = DE_TYPE_DESC(TItem, 1, k_titem_fields);
    fz_check(de_type_register_ex(r, &d, &out[2], NULL) == DE_OK, "next: TItem");
    d = DE_TYPE_DESC(FInventory2, 2, k_finventory2_fields);
    d.name = "TInventory";
    d.hooks.migrate = fz_migrate_plain;
    fz_check(de_type_register_ex(r, &d, &out[3], NULL) == DE_OK, "next: TInventory");
    d = DE_TYPE_DESC(FEntityData2, 3, k_fentitydata2_fields);
    d.name = "TEntityData";
    d.hooks.migrate = fz_migrate_plain;
    fz_check(de_type_register_ex(r, &d, &out[4], NULL) == DE_OK, "next: TEntityData");
    d = DE_TYPE_DESC(FAllKinds2, 2, k_fallkinds2_fields);
    d.name = "TAllKinds";
    d.hooks.migrate = fz_migrate_allkinds;
    fz_check(de_type_register_ex(r, &d, &out[5], NULL) == DE_OK, "next: TAllKinds");
}

/* Builds both registries on the first call (libFuzzer has no init hook in the standalone
 * runner). Also silences the "reflect" warnings that malformed input produces. */
static inline fz_world *fz_world_get(void) {
    if (g_fz.reg[0] != NULL)
        return &g_fz;
    g_fz.log = de_log_register_category("reflect", DE_LOG_INFO);
    de_log_set_level(g_fz.log, DE_LOG_FATAL);
    test_type_ids ids;
    fz_check(de_type_registry_create(NULL, &g_fz.reg[0]) == DE_OK, "registry");
    fz_check(test_types_register(g_fz.reg[0], &ids) == DE_OK, "current types");
    g_fz.types[0][0] = ids.vec;
    g_fz.types[0][1] = ids.transform;
    g_fz.types[0][2] = ids.item;
    g_fz.types[0][3] = ids.inventory;
    g_fz.types[0][4] = ids.entity_data;
    g_fz.types[0][5] = ids.all_kinds;
    fz_check(de_type_registry_create(NULL, &g_fz.reg[1]) == DE_OK, "registry");
    fz_register_next(g_fz.reg[1], g_fz.types[1]);
    for (u32 v = 0; v < 2; ++v)
        for (u32 t = 0; t < FZ_TYPE_COUNT; ++t)
            fz_check(de_type_get(g_fz.reg[v], g_fz.types[v][t])->size <= FZ_OBJ_MAX,
                     "fuzz type larger than FZ_OBJ_MAX");
    return &g_fz;
}

/* ---- Entity and asset mapping --------------------------------------------------------------- */

typedef struct fz_map {
    u32 calls;
} fz_map;

static fz_map g_fz_map;

static inline void fz_map_user(void *user) {
    fz_check(user == &g_fz_map, "a de_ser_context callback did not get ctx->user");
    g_fz_map.calls++;
}
static inline u64 fz_e2s(u64 bits, void *user) {
    fz_map_user(user);
    return bits ^ 0x00ff00ff00ff00ffull;
}
static inline u64 fz_s2e(u64 id, void *user) {
    fz_map_user(user);
    return id ^ 0x00ff00ff00ff00ffull;
}
static inline u64 fz_a2i(u32 h, const char *type, void *user) {
    (void)type;
    fz_map_user(user);
    return ((u64)h << 20) | 0xABCDEu;
}
static inline u32 fz_i2a(u64 id, const char *type, void *user) {
    (void)type;
    fz_map_user(user);
    return (u32)(id >> 20);
}

static const de_ser_context g_fz_ctx = {fz_e2s, fz_s2e, fz_a2i, fz_i2a, &g_fz_map};

/* ---- A reader that hands out the input in uneven chunks -------------------------------------- */

typedef struct fz_reader {
    const u8 *data;
    size_t size, pos;
    u32 step; /* 0: everything that is asked for; otherwise chunks of 1..step bytes */
} fz_reader;

static inline de_result fz_read(void *user, void *dst, size_t bytes, size_t *got) {
    fz_reader *r = (fz_reader *)user;
    size_t left = r->size - r->pos;
    size_t n = bytes < left ? bytes : left;
    if (r->step != 0) {
        size_t chunk = 1u + (r->pos * 7u + 3u) % r->step;
        if (n > chunk)
            n = chunk;
    }
    if (n > 0)
        memcpy(dst, r->data + r->pos, n);
    r->pos += n;
    *got = n;
    return DE_OK;
}

static inline de_reader fz_reader_make(fz_reader *r, const void *data, size_t size, u32 step) {
    r->data = (const u8 *)data;
    r->size = size;
    r->pos = 0;
    r->step = step;
    de_reader rd = {fz_read, r};
    return rd;
}

/* ---- Allocation counting (and failure injection) ------------------------------------------- */

typedef struct fz_alloc {
    i64 live;
    u32 calls, fail_at; /* fail_at 0: never fail */
} fz_alloc;

static inline void *fz_alloc_fn(void *user, size_t size, size_t align, u32 tag) {
    fz_alloc *a = (fz_alloc *)user;
    if (a->fail_at != 0 && ++a->calls == a->fail_at)
        return NULL;
    const de_allocator *h = de_heap_allocator();
    void *p = DE_ALLOC(h, size, align, tag);
    if (p != NULL)
        a->live++;
    return p;
}

static inline void *fz_realloc_fn(void *user, void *ptr, size_t old_size, size_t new_size,
                                  size_t align, u32 tag) {
    fz_alloc *a = (fz_alloc *)user;
    if (a->fail_at != 0 && ++a->calls == a->fail_at)
        return NULL;
    const de_allocator *h = de_heap_allocator();
    void *p = DE_REALLOC(h, ptr, old_size, new_size, align, tag);
    if (p != NULL && ptr == NULL)
        a->live++;
    if (p != NULL && new_size == 0)
        a->live--;
    return p;
}

static inline void fz_free_fn(void *user, void *ptr, size_t size) {
    fz_alloc *a = (fz_alloc *)user;
    if (ptr == NULL)
        return;
    a->live--;
    const de_allocator *h = de_heap_allocator();
    DE_FREE(h, ptr, size);
}

/* ---- Re-encoding checks --------------------------------------------------------------------- */

/* Text: write the loaded object, read it into a fresh object, write again: same bytes. A write
 * may fail on data the text format cannot hold (a string that is not UTF-8): that is fine. */
static inline void fz_text_stable(const de_type_registry *r, de_type_id t, const void *obj,
                                  const de_ser_options *o) {
    de_mem_writer w1, w2;
    de_mem_writer_init(&w1, NULL);
    de_mem_writer_init(&w2, NULL);
    de_writer w = de_writer_from_mem(&w1);
    de_ser_options to = *o;
    to.format = DE_SER_TEXT;
    if (de_serialize_object(r, t, obj, &to, &w) == DE_OK) {
        _Alignas(16) u8 again[FZ_OBJ_MAX];
        fz_check(de_type_construct(r, t, again) == DE_OK, "construct");
        de_mem_reader mr;
        de_mem_reader_init(&mr, w1.data, w1.size);
        de_reader rd = de_reader_from_mem(&mr);
        fz_check(de_deserialize_object(r, t, again, &to, &rd) == DE_OK,
                 "text written for a loaded object does not read back");
        w = de_writer_from_mem(&w2);
        fz_check(de_serialize_object(r, t, again, &to, &w) == DE_OK, "text rewrite failed");
        fz_check(w1.size == w2.size && memcmp(w1.data, w2.data, w1.size) == 0,
                 "text write -> read -> write is not stable");
    }
    de_error_clear();
    de_mem_writer_free(&w1);
    de_mem_writer_free(&w2);
}

/* Binary: the same, in both schema modes. Every loaded object must be writable. */
static inline void fz_binary_stable(const de_type_registry *r, de_type_id t, const void *obj,
                                    const de_ser_options *o, de_ser_binary_schema schema) {
    de_mem_writer w1, w2;
    de_mem_writer_init(&w1, NULL);
    de_mem_writer_init(&w2, NULL);
    de_ser_binary_options bo = {schema, NULL, false};
    de_writer w = de_writer_from_mem(&w1);
    fz_check(de_serialize_binary(r, t, obj, o, &bo, &w) == DE_OK,
             "binary write of a loaded object");
    _Alignas(16) u8 again[FZ_OBJ_MAX];
    fz_check(de_type_construct(r, t, again) == DE_OK, "construct");
    de_mem_reader mr;
    de_mem_reader_init(&mr, w1.data, w1.size);
    de_reader rd = de_reader_from_mem(&mr);
    fz_check(de_deserialize_binary(r, t, again, o, &bo, &rd) == DE_OK,
             "binary written for a loaded object does not read back");
    fz_check(mr.pos == mr.size, "binary read did not consume the whole object");
    w = de_writer_from_mem(&w2);
    fz_check(de_serialize_binary(r, t, again, o, &bo, &w) == DE_OK, "binary rewrite failed");
    fz_check(w1.size == w2.size && memcmp(w1.data, w2.data, w1.size) == 0,
             "binary write -> read -> write is not stable");
    de_mem_writer_free(&w1);
    de_mem_writer_free(&w2);
}

#endif /* DE_FUZZ_REFLECT_H */
