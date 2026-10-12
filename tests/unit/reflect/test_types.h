/*
 * test_types.h: shared reflection test types (DENG-294). Used by the registry and path tests
 * and by the serializer tests (DENG-295 text, DENG-296 binary) and property tests (DENG-297).
 *
 * STABLE: later tasks depend on these layouts, and test_registry.c checks a golden schema hash
 * of TTransform. Add new types; do not change existing ones.
 *
 * test_types_register() registers everything below in a fixed order and fills a
 * test_type_ids. Every type is POD.
 */
#ifndef DE_TEST_REFLECT_TYPES_H
#define DE_TEST_REFLECT_TYPES_H

#include <dengine/de_math.h>
#include <dengine/de_reflect.h>

/* ---- Enums --------------------------------------------------------------------------------- */

typedef enum TMode { TMODE_IDLE = 0, TMODE_WALK = 1, TMODE_RUN = 2, TMODE_NEG = -1 } TMode;

static const de_enum_value k_tmode_values[] = {
    {"Idle", TMODE_IDLE},
    {"Walk", TMODE_WALK},
    {"Run", TMODE_RUN},
    {"Negative", TMODE_NEG},
};

enum { TLAYER_GROUND = 1u << 0, TLAYER_WATER = 1u << 1, TLAYER_AIR = 1u << 2 };

static const de_enum_value k_tlayers_values[] = {
    {"Ground", TLAYER_GROUND},
    {"Water", TLAYER_WATER},
    {"Air", TLAYER_AIR},
};

/* ---- Structs ------------------------------------------------------------------------------- */

typedef struct TVec {
    f32 x, y, z;
} TVec;

static const de_field_info k_tvec_fields[] = {
    DE_FIELD(TVec, x, DE_FK_F32, DE_FIELD_DEFAULT),
    DE_FIELD(TVec, y, DE_FK_F32, DE_FIELD_DEFAULT),
    DE_FIELD(TVec, z, DE_FK_F32, DE_FIELD_DEFAULT),
};

typedef struct TTransform {
    de_dvec3 position;
    de_quat rotation;
    de_vec3 scale;
} TTransform;

static const de_field_info k_ttransform_fields[] = {
    DE_FIELD_EX(TTransform, position, DE_FK_DVEC3, DE_FIELD_DEFAULT | DE_FIELD_REPLICATE,
                {0, 0, "m", "World position", 0, 0}),
    DE_FIELD(TTransform, rotation, DE_FK_QUAT, DE_FIELD_DEFAULT | DE_FIELD_REPLICATE),
    DE_FIELD_EX(TTransform, scale, DE_FK_VEC3, DE_FIELD_DEFAULT, {0.001, 1000, 0, 0, 0, 0}),
};

typedef struct TItem {
    u32 id;
    i32 count;
} TItem;

static const de_field_info k_titem_fields[] = {
    DE_FIELD(TItem, id, DE_FK_U32, DE_FIELD_DEFAULT),
    DE_FIELD(TItem, count, DE_FK_I32, DE_FIELD_DEFAULT),
};

typedef struct TInventory {
    u32 count;
    TItem items[8];
} TInventory;

static const de_field_info k_tinventory_fields[] = {
    DE_FIELD(TInventory, count, DE_FK_U32, DE_FIELD_DEFAULT),
    DE_FIELD_EX(TInventory, items, DE_FK_FIXED_ARRAY, DE_FIELD_DEFAULT, {0, 0, 0, 0, "TItem", 8}),
};

/* A component-like type with a nested transform and inventory (paths "transform.position.x",
 * "inventory.items[3].count"). */
typedef struct TEntityData {
    TTransform transform;
    TInventory inventory;
    u8 mode; /* TMode */
} TEntityData;

static const de_field_info k_tentitydata_fields[] = {
    DE_FIELD_EX(TEntityData, transform, DE_FK_STRUCT, DE_FIELD_DEFAULT,
                {0, 0, 0, 0, "TTransform", 0}),
    DE_FIELD_EX(TEntityData, inventory, DE_FK_STRUCT, DE_FIELD_DEFAULT,
                {0, 0, 0, 0, "TInventory", 0}),
    DE_FIELD_EX(TEntityData, mode, DE_FK_ENUM, DE_FIELD_DEFAULT, {0, 0, 0, 0, "TMode", 0}),
};

/* One field of every de_field_kind (for the serializers' round-trip tests). */
DE_HANDLE(TMeshHandle);

typedef struct TAllKinds {
    bool b;
    i8 i8v;
    i16 i16v;
    i32 i32v;
    i64 i64v;
    u8 u8v;
    u16 u16v;
    u32 u32v;
    u64 u64v;
    f32 f32v;
    f64 f64v;
    de_vec2 v2;
    de_vec3 v3;
    de_vec4 v4;
    de_dvec3 dv3;
    de_quat q;
    de_mat4 m4;
    de_vec4 color;
    de_name name;
    char str[32];
    u64 entity; /* de_entity bits */
    TMeshHandle mesh;
    u8 mode;    /* TMode */
    u32 layers; /* TLayers flags */
    TVec vec;
    f32 samples[4];
    TItem items[2];
} TAllKinds;

static const de_field_info k_tallkinds_fields[] = {
    DE_FIELD(TAllKinds, b, DE_FK_BOOL, DE_FIELD_DEFAULT),
    DE_FIELD(TAllKinds, i8v, DE_FK_I8, DE_FIELD_DEFAULT),
    DE_FIELD(TAllKinds, i16v, DE_FK_I16, DE_FIELD_DEFAULT),
    DE_FIELD(TAllKinds, i32v, DE_FK_I32, DE_FIELD_DEFAULT),
    DE_FIELD(TAllKinds, i64v, DE_FK_I64, DE_FIELD_DEFAULT),
    DE_FIELD(TAllKinds, u8v, DE_FK_U8, DE_FIELD_DEFAULT),
    DE_FIELD(TAllKinds, u16v, DE_FK_U16, DE_FIELD_DEFAULT),
    DE_FIELD(TAllKinds, u32v, DE_FK_U32, DE_FIELD_DEFAULT),
    DE_FIELD(TAllKinds, u64v, DE_FK_U64, DE_FIELD_DEFAULT),
    DE_FIELD(TAllKinds, f32v, DE_FK_F32, DE_FIELD_DEFAULT),
    DE_FIELD(TAllKinds, f64v, DE_FK_F64, DE_FIELD_DEFAULT),
    DE_FIELD(TAllKinds, v2, DE_FK_VEC2, DE_FIELD_DEFAULT),
    DE_FIELD(TAllKinds, v3, DE_FK_VEC3, DE_FIELD_DEFAULT),
    DE_FIELD(TAllKinds, v4, DE_FK_VEC4, DE_FIELD_DEFAULT),
    DE_FIELD(TAllKinds, dv3, DE_FK_DVEC3, DE_FIELD_DEFAULT),
    DE_FIELD(TAllKinds, q, DE_FK_QUAT, DE_FIELD_DEFAULT),
    DE_FIELD(TAllKinds, m4, DE_FK_MAT4, DE_FIELD_DEFAULT),
    DE_FIELD(TAllKinds, color, DE_FK_COLOR, DE_FIELD_DEFAULT),
    DE_FIELD(TAllKinds, name, DE_FK_NAME, DE_FIELD_DEFAULT),
    DE_FIELD(TAllKinds, str, DE_FK_STRING, DE_FIELD_DEFAULT),
    DE_FIELD(TAllKinds, entity, DE_FK_ENTITY, DE_FIELD_DEFAULT),
    DE_FIELD_EX(TAllKinds, mesh, DE_FK_ASSET, DE_FIELD_DEFAULT | DE_FIELD_ASSET_REF,
                {0, 0, 0, 0, "mesh", 0}),
    DE_FIELD_EX(TAllKinds, mode, DE_FK_ENUM, DE_FIELD_DEFAULT, {0, 0, 0, 0, "TMode", 0}),
    DE_FIELD_EX(TAllKinds, layers, DE_FK_FLAGS, DE_FIELD_DEFAULT, {0, 0, 0, 0, "TLayers", 0}),
    DE_FIELD_EX(TAllKinds, vec, DE_FK_STRUCT, DE_FIELD_DEFAULT, {0, 0, 0, 0, "TVec", 0}),
    DE_FIELD_EX(TAllKinds, samples, DE_FK_FIXED_ARRAY, DE_FIELD_DEFAULT, {0, 0, 0, 0, "f32", 4}),
    DE_FIELD_EX(TAllKinds, items, DE_FK_FIXED_ARRAY, DE_FIELD_DEFAULT, {0, 0, 0, 0, "TItem", 2}),
};

/* ---- Registration -------------------------------------------------------------------------- */

typedef struct test_type_ids {
    de_type_id mode, layers;
    de_type_id vec, transform, item, inventory, entity_data, all_kinds;
} test_type_ids;

#define TEST_COUNT_OF(a) ((u32)(sizeof(a) / sizeof((a)[0])))

/* Registers the enums and types above, in this order. DE_OK or the first failure. */
static inline de_result test_types_register(de_type_registry *r, test_type_ids *ids) {
    DE_TRY(de_enum_register(r, "TMode", k_tmode_values, TEST_COUNT_OF(k_tmode_values), false));
    DE_TRY(de_enum_register(r, "TLayers", k_tlayers_values, TEST_COUNT_OF(k_tlayers_values), true));
    ids->mode = de_type_find(r, "TMode");
    ids->layers = de_type_find(r, "TLayers");
    DE_TRY(de_type_register_ex(r, &DE_TYPE_DESC(TVec, 1, k_tvec_fields), &ids->vec, NULL));
    DE_TRY(de_type_register_ex(r, &DE_TYPE_DESC(TTransform, 1, k_ttransform_fields),
                               &ids->transform, NULL));
    DE_TRY(de_type_register_ex(r, &DE_TYPE_DESC(TItem, 1, k_titem_fields), &ids->item, NULL));
    DE_TRY(de_type_register_ex(r, &DE_TYPE_DESC(TInventory, 1, k_tinventory_fields),
                               &ids->inventory, NULL));
    DE_TRY(de_type_register_ex(r, &DE_TYPE_DESC(TEntityData, 1, k_tentitydata_fields),
                               &ids->entity_data, NULL));
    DE_TRY(de_type_register_ex(r, &DE_TYPE_DESC(TAllKinds, 1, k_tallkinds_fields), &ids->all_kinds,
                               NULL));
    return DE_OK;
}

#endif /* DE_TEST_REFLECT_TYPES_H */
