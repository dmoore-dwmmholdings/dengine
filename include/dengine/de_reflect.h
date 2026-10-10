/* de_reflect.h — type registry, reflection, serialization. SPEC.md §9.
 * One description per type feeds: ECS, serialization, netcode, inspector, editor, scripting. */
#ifndef DE_REFLECT_H
#define DE_REFLECT_H
#include "de_base.h"
#include "de_memory.h"
DE_EXTERN_C_BEGIN

typedef struct de_type_registry de_type_registry;
DE_HANDLE(de_type_id);

typedef enum de_field_kind {
    DE_FK_BOOL, DE_FK_I8, DE_FK_I16, DE_FK_I32, DE_FK_I64, DE_FK_U8, DE_FK_U16, DE_FK_U32, DE_FK_U64,
    DE_FK_F32, DE_FK_F64, DE_FK_VEC2, DE_FK_VEC3, DE_FK_VEC4, DE_FK_DVEC3, DE_FK_QUAT, DE_FK_MAT4,
    DE_FK_COLOR, DE_FK_NAME, DE_FK_STRING, DE_FK_ENTITY, DE_FK_ASSET, DE_FK_ENUM, DE_FK_FLAGS,
    DE_FK_STRUCT, DE_FK_FIXED_ARRAY
} de_field_kind;

typedef enum de_field_flags {
    DE_FIELD_SERIALIZE     = 1u << 0,
    DE_FIELD_REPLICATE     = 1u << 1,
    DE_FIELD_SNAPSHOT      = 1u << 2,
    DE_FIELD_EDITOR_HIDDEN = 1u << 3,
    DE_FIELD_READONLY      = 1u << 4,
    DE_FIELD_ASSET_REF     = 1u << 5,
    DE_FIELD_DEFAULT       = DE_FIELD_SERIALIZE | DE_FIELD_SNAPSHOT
} de_field_flags;

typedef struct de_field_attrs {
    f64         min, max;        /* range, if min < max */
    const char *units;           /* "m", "kg", "rad", "lux" */
    const char *tooltip;
    const char *nested_type;     /* for STRUCT / ENUM / FLAGS / ASSET (asset type name) */
    u32         array_count;     /* for FIXED_ARRAY */
} de_field_attrs;

typedef struct de_field_info {
    const char    *name;
    u32            offset, size;
    de_field_kind  kind;
    u32            flags;        /* de_field_flags */
    de_field_attrs attrs;
} de_field_info;

typedef struct de_type_hooks {
    void (*construct)(void *obj);
    void (*destruct)(void *obj);
    void (*copy)(void *dst, const void *src);
    /* Upgrade data saved with an older version. Return false to drop the object. */
    bool (*migrate)(void *obj, u32 from_version, const void *old_data, const de_field_info *old_fields, u32 old_field_count);
} de_type_hooks;

typedef struct de_type_desc {
    const char          *name;
    u32                  size, align, version;
    const de_field_info *fields;
    u32                  field_count;
    de_type_hooks        hooks;
    bool                 is_pod;     /* enables memcpy snapshot/serialize fast paths */
} de_type_desc;

typedef struct de_enum_value { const char *name; i64 value; } de_enum_value;

DE_API de_result  de_type_registry_create(const de_allocator *a, de_type_registry **out);
DE_API void       de_type_registry_destroy(de_type_registry *r);
DE_API de_type_id de_type_register(de_type_registry *r, const de_type_desc *desc);
DE_API de_result  de_enum_register(de_type_registry *r, const char *name, const de_enum_value *values, u32 count, bool is_flags);
DE_API de_type_id de_type_find(const de_type_registry *r, const char *name);
DE_API const de_type_desc *de_type_get(const de_type_registry *r, de_type_id id);
DE_API u64        de_type_schema_hash(const de_type_registry *r, de_type_id id);

/* Registration helpers:
 *   static const de_field_info k_fields[] = {
 *       DE_FIELD(Health, current, DE_FK_F32, DE_FIELD_DEFAULT | DE_FIELD_REPLICATE),
 *       DE_FIELD(Health, max,     DE_FK_F32, DE_FIELD_DEFAULT),
 *   };
 *   de_type_register(reg, &DE_TYPE_DESC(Health, 1, k_fields));
 */
#define DE_FIELD(T, member, kind, flags) \
    { #member, (u32)offsetof(T, member), (u32)sizeof(((T *)0)->member), (kind), (flags), { 0, 0, 0, 0, 0, 0 } }
#define DE_FIELD_EX(T, member, kind, flags, ...) \
    { #member, (u32)offsetof(T, member), (u32)sizeof(((T *)0)->member), (kind), (flags), __VA_ARGS__ }
#define DE_TYPE_DESC(T, ver, fields_arr) \
    ((de_type_desc){ #T, (u32)sizeof(T), (u32)DE_ALIGNOF(T), (ver), (fields_arr), \
                     (u32)(sizeof(fields_arr) / sizeof((fields_arr)[0])), { 0, 0, 0, 0 }, true })

/* ---------- Serialization ---------- */
typedef enum de_ser_format { DE_SER_TEXT, DE_SER_BINARY } de_ser_format;

typedef struct de_writer {
    de_result (*write)(void *user, const void *data, size_t bytes);
    void *user;
} de_writer;
typedef struct de_reader {
    de_result (*read)(void *user, void *data, size_t bytes, size_t *read);
    void *user;
} de_reader;

typedef struct de_ser_options {
    de_ser_format format;
    u32           required_flags;   /* only fields with these flags, e.g. DE_FIELD_SERIALIZE */
} de_ser_options;

DE_API de_result de_serialize_object(const de_type_registry *r, de_type_id type, const void *obj,
                                     const de_ser_options *opt, de_writer *w);
DE_API de_result de_deserialize_object(const de_type_registry *r, de_type_id type, void *obj,
                                       const de_ser_options *opt, de_reader *rd);

/* Field access by path ("transform.position.x") for UI binding, inspector, scripting. */
DE_API de_result de_reflect_resolve_path(const de_type_registry *r, de_type_id type, const char *path,
                                         const de_field_info **field, u32 *offset);

DE_EXTERN_C_END
#endif
