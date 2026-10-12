/* de_reflect.h — type registry, reflection, serialization. SPEC.md §9.
 * One description per type feeds: ECS, serialization, netcode, inspector, editor, scripting. */
#ifndef DE_REFLECT_H
#define DE_REFLECT_H
#include "de_base.h"
#include "de_memory.h"
DE_EXTERN_C_BEGIN

/**
 * A type registry: the reflection data of every registered struct type and enum (SPEC §9.1).
 *
 * Threading: registration (de_type_register*, de_enum_register) and destroy are single-threaded,
 * normally on the main thread at startup or during a hot reload (SPEC §10.1). Every other
 * function only reads, takes no lock and may run on any number of threads at once, as long as no
 * registration runs at the same time. The app host orders the two (registration happens between
 * frames).
 *
 * Memory: every description is deep-copied (field array and strings) into blocks taken from the
 * registry's allocator. Nothing is freed before de_type_registry_destroy, so every pointer the
 * registry returns stays valid until then, also after the type is registered again.
 */
typedef struct de_type_registry de_type_registry;

/** A registered struct type or enum. Low 20 bits: index + 1; high 12 bits: generation. */
DE_HANDLE(de_type_id);

typedef enum de_field_kind {
    DE_FK_BOOL,  /**< bool, 1 byte */
    DE_FK_I8,    /**< i8 */
    DE_FK_I16,   /**< i16 */
    DE_FK_I32,   /**< i32 */
    DE_FK_I64,   /**< i64 */
    DE_FK_U8,    /**< u8 */
    DE_FK_U16,   /**< u16 */
    DE_FK_U32,   /**< u32 */
    DE_FK_U64,   /**< u64 */
    DE_FK_F32,   /**< f32 */
    DE_FK_F64,   /**< f64 */
    DE_FK_VEC2,  /**< de_vec2, 8 bytes; components .x .y */
    DE_FK_VEC3,  /**< de_vec3, 12 bytes; components .x .y .z */
    DE_FK_VEC4,  /**< de_vec4, 16 bytes; components .x .y .z .w */
    DE_FK_DVEC3, /**< de_dvec3, 24 bytes; f64 components .x .y .z */
    DE_FK_QUAT,  /**< de_quat, 16 bytes; components .x .y .z .w (w is the scalar part) */
    DE_FK_MAT4,  /**< de_mat4, 64 bytes, column-major; no path components */
    DE_FK_COLOR, /**< four f32 (linear RGBA), 16 bytes; components .r .g .b .a (or .x .y .z .w) */
    DE_FK_NAME,  /**< de_name, 8 bytes */
    /** Inline fixed `char[N]` buffer (N = field size >= 1), NUL-terminated. Components are POD
     * (SPEC §7.1), so a string never points outside the object. */
    DE_FK_STRING,
    DE_FK_ENTITY, /**< de_entity, 8 bytes */
    /** Typed asset handle (a DE_HANDLE, u32), 4 bytes. attrs.nested_type may name the asset type
     * ("mesh"); it is passed to the de_ser_context asset callbacks. */
    DE_FK_ASSET,
    /** Integer of 1, 2, 4 or 8 bytes holding a value of the enum attrs.nested_type, which must be
     * registered with de_enum_register(..., is_flags = false) first. */
    DE_FK_ENUM,
    /** Like DE_FK_ENUM, for an enum registered with is_flags = true (a bit set). */
    DE_FK_FLAGS,
    /** A nested struct: attrs.nested_type names a registered type whose size is the field size. */
    DE_FK_STRUCT,
    /**
     * `elem[attrs.array_count]`; field size = array_count * element size. The element is named by
     * attrs.nested_type: a registered struct type or enum, or one of the built-in kind names
     * "bool" "i8" "i16" "i32" "i64" "u8" "u16" "u32" "u64" "f32" "f64" "vec2" "vec3" "vec4"
     * "dvec3" "quat" "mat4" "color" "name" "string" "entity" "asset". With nested_type NULL the
     * element is an unsigned integer of the element size (1, 2, 4 or 8 bytes). Arrays of arrays
     * are not supported. de_type_array_element() gives the element's description.
     */
    DE_FK_FIXED_ARRAY
} de_field_kind;

/** Number of de_field_kind values. */
#define DE_FIELD_KIND_COUNT ((u32)DE_FK_FIXED_ARRAY + 1u)

typedef enum de_field_flags {
    DE_FIELD_SERIALIZE = 1u << 0,
    DE_FIELD_REPLICATE = 1u << 1,
    DE_FIELD_SNAPSHOT = 1u << 2,
    DE_FIELD_EDITOR_HIDDEN = 1u << 3,
    DE_FIELD_READONLY = 1u << 4,
    DE_FIELD_ASSET_REF = 1u << 5,
    DE_FIELD_DEFAULT = DE_FIELD_SERIALIZE | DE_FIELD_SNAPSHOT
} de_field_flags;

/** The flags that are part of the schema hash. The others are editor metadata. */
#define DE_FIELD_SCHEMA_FLAGS (DE_FIELD_SERIALIZE | DE_FIELD_REPLICATE | DE_FIELD_SNAPSHOT)

typedef struct de_field_attrs {
    f64 min, max;      /* range, if min < max */
    const char *units; /* "m", "kg", "rad", "lux" */
    const char *tooltip;
    const char *nested_type; /* for STRUCT / ENUM / FLAGS / FIXED_ARRAY / ASSET (asset type name) */
    u32 array_count;         /* for FIXED_ARRAY */
} de_field_attrs;

typedef struct de_field_info {
    const char *name; /**< C identifier: [A-Za-z_][A-Za-z0-9_]* */
    u32 offset, size; /**< byte offset in the type, and byte size */
    de_field_kind kind;
    u32 flags; /* de_field_flags */
    de_field_attrs attrs;
} de_field_info;

typedef struct de_type_hooks {
    void (*construct)(void *obj);
    void (*destruct)(void *obj);
    void (*copy)(void *dst, const void *src);
    /* Upgrade data saved with an older version. Return false to drop the object. */
    bool (*migrate)(void *obj, u32 from_version, const void *old_data,
                    const de_field_info *old_fields, u32 old_field_count);
} de_type_hooks;

typedef struct de_type_desc {
    const char *name; /**< unique in the registry (struct types and enums share one namespace) */
    u32 size, align, version; /**< align 0 means 1; size must be a multiple of align */
    const de_field_info *fields;
    u32 field_count;
    de_type_hooks hooks;
    bool is_pod; /* enables memcpy snapshot/serialize fast paths */
} de_type_desc;

typedef struct de_enum_value {
    const char *name;
    i64 value;
} de_enum_value;

/** A registered enum, from de_enum_get(). The strings and the array belong to the registry. */
typedef struct de_enum_info {
    const char *name;
    const de_enum_value *values; /**< in registration order */
    u32 count;
    bool is_flags;
} de_enum_info;

/**
 * Creates an empty registry. `a` NULL means de_heap_allocator(); it must outlive the registry.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (`out` NULL) or DE_ERR_OUT_OF_MEMORY.
 */
DE_API de_result de_type_registry_create(const de_allocator *a, de_type_registry **out);
/** Frees the registry and every description it holds. NULL is ignored. */
DE_API void de_type_registry_destroy(de_type_registry *r);

/**
 * Validates `desc`, deep-copies it and returns its id. On failure returns an invalid id
 * (`.id == 0`) and sets the error context (see de_type_register_ex for the codes). Registering a
 * name again is a hot reload: see de_type_register_ex.
 */
DE_API de_type_id de_type_register(de_type_registry *r, const de_type_desc *desc);

/**
 * de_type_register with the result code and the hot-reload outcome (SPEC §10.1).
 *
 * Validation (each failure is DE_ERR_INVALID_ARGUMENT, with a message that names the type and
 * the field): the type name is a non-empty identifier and not a built-in kind name; align is 0 or
 * a power of two and divides size; every field has a unique identifier name; offset + size <=
 * type size; no two fields overlap; the size fits the kind (see de_field_kind; STRING >= 1 byte,
 * ENUM / FLAGS 1, 2, 4 or 8 bytes); ENUM, FLAGS and STRUCT name a registered enum, flags enum or
 * struct type (STRUCT: of the field's size); FIXED_ARRAY has array_count >= 1, a size that is
 * array_count times the element size, and a valid element type.
 *
 * When a type of the same name exists:
 * - same schema hash as at its last registration: the stored description is replaced by the new
 *   copy (new hooks, tooltips and other attributes), the id stays, `*out_layout_changed = false`;
 * - different schema hash: the description is replaced in place (same id), the old one stays
 *   available through de_type_get_previous(), `*out_layout_changed = true`, so the host can run
 *   the migration (S06).
 * A failed validation leaves the registered type unchanged. A name held by an enum gives
 * DE_ERR_ALREADY_EXISTS.
 *
 * @param out_id             the id, or an invalid id on failure. May be NULL.
 * @param out_layout_changed true only for a re-registration that changed the layout. May be NULL.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT, DE_ERR_ALREADY_EXISTS or DE_ERR_OUT_OF_MEMORY.
 */
DE_API de_result de_type_register_ex(de_type_registry *r, const de_type_desc *desc,
                                     de_type_id *out_id, bool *out_layout_changed);

/**
 * Registers an enum (or, with `is_flags`, a bit-flag set) so ENUM / FLAGS / FIXED_ARRAY fields
 * can name it. Value names must be unique identifiers and values unique; `count` >= 1. The values
 * and names are copied. Registering the same name again replaces the values in place (same id).
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT, DE_ERR_ALREADY_EXISTS (the name is a struct type) or
 *         DE_ERR_OUT_OF_MEMORY.
 */
DE_API de_result de_enum_register(de_type_registry *r, const char *name,
                                  const de_enum_value *values, u32 count, bool is_flags);

/** The struct type or enum called `name`, or an invalid id. Sets no error. */
DE_API de_type_id de_type_find(const de_type_registry *r, const char *name);
/** The current description of a struct type, or NULL (invalid or stale id, or an enum). */
DE_API const de_type_desc *de_type_get(const de_type_registry *r, de_type_id id);

/**
 * The description that was current before the last re-registration that changed the layout, or
 * NULL if the layout never changed. Its hooks belong to the code that registered it, which a hot
 * reload may have unloaded: never call them.
 */
DE_API const de_type_desc *de_type_get_previous(const de_type_registry *r, de_type_id id);

/** The current fields of a struct type (`*count` set to their number), or NULL with `*count` 0. */
DE_API const de_field_info *de_type_fields(const de_type_registry *r, de_type_id id, u32 *count);

/**
 * The element of FIXED_ARRAY field `field_index` of a struct type: name = the field's name,
 * offset 0, size = element size, kind = element kind, flags = the field's flags,
 * attrs.nested_type = the element's struct or enum name (NULL for built-in kinds), array_count 0.
 * NULL if the id, the index or the kind is wrong.
 */
DE_API const de_field_info *de_type_array_element(const de_type_registry *r, de_type_id id,
                                                  u32 field_index);

/**
 * Schema hash of a struct type or enum: XXH3-64 (de_hash64) over a canonical little-endian
 * encoding of the type name, version, size and, per field in declaration order, name, kind,
 * size, offset, `flags & DE_FIELD_SCHEMA_FLAGS`, the nested type's schema hash (recursively; for
 * an asset type or built-in element name, the hash of the name) and array count. Range, units and
 * tooltip are not part of it. An enum hashes its name, is_flags and its (name, value) pairs. The
 * value is the same in every process and on every platform. 0 for an invalid id.
 */
DE_API u64 de_type_schema_hash(const de_type_registry *r, de_type_id id);

/** The registered enum `id`, or NULL (invalid id, or a struct type). */
DE_API const de_enum_info *de_enum_get(const de_type_registry *r, de_type_id id);
/** Looks up a value by name in enum `id`. false (and `*out_value` untouched) if not found. */
DE_API bool de_enum_value_of(const de_type_registry *r, de_type_id id, const char *name,
                             i64 *out_value);
/** The name of the value equal to `value` in enum `id`, or NULL. Flags are not decomposed. */
DE_API const char *de_enum_name_of(const de_type_registry *r, de_type_id id, i64 value);

/**
 * Initialises `obj` (type size bytes): the construct hook, or zero-fill without one.
 * @return DE_OK or DE_ERR_INVALID_ARGUMENT (invalid id, an enum, or `obj` NULL).
 */
DE_API de_result de_type_construct(const de_type_registry *r, de_type_id id, void *obj);
/** Calls the destruct hook on `obj`, if the type has one. Ignores invalid ids and NULL. */
DE_API void de_type_destruct(const de_type_registry *r, de_type_id id, void *obj);
/**
 * Copies `src` to `dst`: the copy hook, or memcpy when the type `is_pod`.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT, or DE_ERR_UNSUPPORTED (no copy hook and not POD).
 */
DE_API de_result de_type_copy(const de_type_registry *r, de_type_id id, void *dst, const void *src);

/* Registration helpers:
 *   static const de_field_info k_fields[] = {
 *       DE_FIELD(Health, current, DE_FK_F32, DE_FIELD_DEFAULT | DE_FIELD_REPLICATE),
 *       DE_FIELD(Health, max,     DE_FK_F32, DE_FIELD_DEFAULT),
 *       DE_FIELD_EX(Health, regen, DE_FK_F32, DE_FIELD_DEFAULT, {0, 10, "hp/s", "Regen", 0, 0}),
 *   };
 *   de_type_register(reg, &DE_TYPE_DESC(Health, 1, k_fields));
 *
 * DE_FIELD_EX's last argument is the de_field_attrs initializer
 * {min, max, units, tooltip, nested_type, array_count}.
 *
 * In C, DE_TYPE_DESC is a compound literal, so `&DE_TYPE_DESC(...)` works. In C++17 it is a
 * prvalue `de_type_desc{...}`; take its address through a variable:
 *   const de_type_desc d = DE_TYPE_DESC(Health, 1, k_fields);
 *   de_type_register(reg, &d);
 */
#define DE_FIELD(T, member, kind, flags)                                                           \
    {#member, (u32)offsetof(T, member), (u32)sizeof(((T *)0)->member), (kind),                     \
     (flags), {0, 0, 0, 0, 0, 0}}
#define DE_FIELD_EX(T, member, kind, flags, ...)                                                   \
    {#member, (u32)offsetof(T, member), (u32)sizeof(((T *)0)->member), (kind), (flags), __VA_ARGS__}
#ifdef __cplusplus
#define DE_TYPE_DESC(T, ver, fields_arr)                                                           \
    (de_type_desc{#T,                                                                              \
                  (u32)sizeof(T),                                                                  \
                  (u32)DE_ALIGNOF(T),                                                              \
                  (ver),                                                                           \
                  (fields_arr),                                                                    \
                  (u32)(sizeof(fields_arr) / sizeof((fields_arr)[0])),                             \
                  {0, 0, 0, 0},                                                                    \
                  true})
#else
#define DE_TYPE_DESC(T, ver, fields_arr)                                                           \
    ((de_type_desc){#T,                                                                            \
                    (u32)sizeof(T),                                                                \
                    (u32)DE_ALIGNOF(T),                                                            \
                    (ver),                                                                         \
                    (fields_arr),                                                                  \
                    (u32)(sizeof(fields_arr) / sizeof((fields_arr)[0])),                           \
                    {0, 0, 0, 0},                                                                  \
                    true})
#endif

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

/**
 * Maps run-time references to stable IDs while serializing, and back while deserializing (SPEC
 * §9.2): entities to 64-bit scene IDs, asset handles to asset IDs. Every callback receives
 * `user`. A NULL context or a NULL callback means the raw value is written and read.
 * `asset_type` is the field's attrs.nested_type (may be NULL). Shared by the text and binary
 * serializers; do not change its shape without updating both.
 */
typedef struct de_ser_context {
    u64 (*entity_to_scene_id)(u64 entity_bits, void *user);
    u64 (*scene_id_to_entity)(u64 scene_id, void *user);
    u64 (*asset_to_id)(u32 handle, const char *asset_type, void *user);
    u32 (*id_to_asset)(u64 id, const char *asset_type, void *user);
    void *user;
} de_ser_context;

typedef struct de_ser_options {
    de_ser_format format;
    u32 required_flags;        /* only fields with these flags, e.g. DE_FIELD_SERIALIZE */
    const de_ser_context *ctx; /**< entity/asset mapping; NULL = raw values */
} de_ser_options;

DE_API de_result de_serialize_object(const de_type_registry *r, de_type_id type, const void *obj,
                                     const de_ser_options *opt, de_writer *w);
DE_API de_result de_deserialize_object(const de_type_registry *r, de_type_id type, void *obj,
                                       const de_ser_options *opt, de_reader *rd);

/* ---- Text format (DENG-295) ---- */

/*
 * DE_SER_TEXT is TOML 1.0 (DENG-2 Q9), written by hand and read with the vendored tomlc17.
 *
 * de_serialize_object(..., {DE_SER_TEXT, ...}) writes one object: the keys `__type = "Name"` and
 * `__version = N`, then each field whose flags include every bit of `opt->required_flags`
 * (0 = all fields), in declaration order, `\n` line endings, no trailing spaces. The output for
 * an object is always the same bytes, on every platform and in every C locale. A NULL `opt` means
 * DE_SER_TEXT, all fields, no context.
 *
 * Field kinds: BOOL `true`/`false`; integers in decimal, except a U64 above INT64_MAX, which is a
 * string "0x%016llx"; F32/F64 as the shortest decimal that reads back to the same bits, and
 * `nan`, `inf`, `-inf`; VEC2/3/4, QUAT, COLOR, MAT4 (column-major) and DVEC3 as arrays of
 * floats; NAME as its string where the build keeps names (DE_NAME_LOOKUP), else its hash as
 * "0x..."; STRING as a TOML string; ENUM as the value's name (an integer if the value has none);
 * FLAGS as an array of value names (plus an integer for bits without a name); STRUCT as dotted
 * keys (`transform.position = [...]`), or an inline table inside an array; FIXED_ARRAY as an
 * array (struct elements one inline table per line); ENTITY as its scene ID "0x..."; ASSET as
 * `{ id = "0x...", path = "player/mesh" }`, the path only when de_name_str() knows the ID.
 * Entities and assets go through `opt->ctx` (de_ser_context; NULL = raw values).
 *
 * de_deserialize_object(..., {DE_SER_TEXT, ...}) reads all of `rd` (until it reports 0 bytes),
 * parses it and fills the fields named in it; `obj` should be constructed first
 * (de_type_construct), because a field the text does not name keeps its value. Fields match by
 * name. An unknown field is skipped with one warning (log category "reflect"). A value of the
 * wrong type, an integer out of range, a string that does not fit, or an unknown enum name gives
 * DE_ERR_PARSE with the field path ("items[2].count") and the line. Integers are accepted for
 * float fields, decimal integers or "0x..." strings for U64 and ENTITY, any string for NAME (a
 * "0x" and 1-16 hex digits string is read as the hash itself), and a plain path or "0x..."
 * string for ASSET (the ID of a path is de_hash64 of its bytes). `__type`, if present, must name
 * the type; a missing `__version` means the current version.
 *
 * Versions: a newer `__version` than the type's gives DE_ERR_VERSION_MISMATCH. An older one is
 * read leniently: fields still match by name, and fields that no longer fit are skipped. Then, if
 * the type has a `migrate` hook, it is called with the stored data: every stored value as one
 * synthesized de_field_info (see below) over a scratch blob. If it returns false the object is
 * dropped: DE_ERR_VERSION_MISMATCH, which callers treat as "skip this object". Without a hook,
 * the read warns once and keeps what matched.
 *
 * Synthesized old fields (migrate): name = the key, with nested tables flattened to dotted names
 * ("transform.position") and arrays of tables to "items[2].count"; booleans BOOL (1 byte),
 * integers I64, floats F64, "0x..." strings U64 (raw IDs: entities and assets are not mapped),
 * other strings STRING (NUL-terminated, length + 1 bytes), arrays of numbers / booleans / strings
 * FIXED_ARRAY with attrs.nested_type "i64", "f64", "bool" or "string" (every element padded to
 * the longest string + 1) and attrs.array_count. Offsets are 8-byte aligned; flags are 0. Mixed
 * arrays and dates are not passed. The data is valid only during the call.
 *
 * Allocation: the writer allocates nothing. The reader takes its buffers from
 * de_heap_allocator(); use de_text_doc_parse() to choose the allocator.
 */

/** A new stable scene ID (SPEC §9.2): 64 bits from the OS random source, never 0. Thread-safe. */
DE_API u64 de_scene_id_new(void);

/**
 * Writes many objects into one TOML document, each as an entry of an array of tables:
 *
 *     [[entity]]
 *     __type = "TTransform"
 *     __version = 1
 *     position = [0.0, 1.0, 2.0]
 *
 * Entries are separated by an empty line. Plain value; `(de_text_doc_writer){0}` is not ready
 * until de_text_doc_begin().
 */
typedef struct de_text_doc_writer {
    de_writer *w;
    u32 object_count; /**< objects written so far */
} de_text_doc_writer;

/** Starts a document on `w` (which must outlive it). Writes nothing yet. */
DE_API de_result de_text_doc_begin(de_text_doc_writer *doc, de_writer *w);

/**
 * Appends `obj` as a `[[table_key]]` entry, written like de_serialize_object (DE_SER_TEXT;
 * `opt->format` is ignored). `table_key` is a TOML bare key: [A-Za-z0-9_-]+.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT, or the writer's failure.
 */
DE_API de_result de_text_doc_write_object(de_text_doc_writer *doc, const char *table_key,
                                          const de_type_registry *r, de_type_id type,
                                          const void *obj, const de_ser_options *opt);

/** Ends the document. Writes nothing; returns DE_OK unless `doc` is NULL. */
DE_API de_result de_text_doc_end(de_text_doc_writer *doc);

/** A parsed TOML document, for reading objects back. Read-only after parsing: thread-safe. */
typedef struct de_text_doc de_text_doc;

/**
 * Parses `len` bytes of TOML. `a` NULL = de_heap_allocator(); it serves every allocation of the
 * document and of de_text_doc_read_object, and must outlive the document. `source` names the text
 * in messages (copied; NULL = "text").
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT, DE_ERR_PARSE (the message has the line) or
 *         DE_ERR_OUT_OF_MEMORY.
 */
DE_API de_result de_text_doc_parse(const de_allocator *a, const char *text, size_t len,
                                   const char *source, de_text_doc **out);

/** Frees the document. NULL is ignored. */
DE_API void de_text_doc_destroy(de_text_doc *doc);

/**
 * Number of `[[table_key]]` entries, or 0 if the key is missing or is not an array of tables.
 * `table_key` NULL means the top-level table itself (a single-object document): 1.
 */
DE_API u32 de_text_doc_count(const de_text_doc *doc, const char *table_key);

/** The `__type` string of entry `index` of `[[table_key]]` (NULL key = top level), or NULL. */
DE_API const char *de_text_doc_type(const de_text_doc *doc, const char *table_key, u32 index);

/**
 * Reads entry `index` of `[[table_key]]` (NULL key = the top-level table) into `obj`, with the
 * rules of de_deserialize_object.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (bad id, NULL argument, no such entry), DE_ERR_PARSE,
 *         DE_ERR_VERSION_MISMATCH (newer version, or dropped by migrate) or DE_ERR_OUT_OF_MEMORY.
 */
DE_API de_result de_text_doc_read_object(const de_text_doc *doc, const char *table_key, u32 index,
                                         const de_type_registry *r, de_type_id type, void *obj,
                                         const de_ser_options *opt);

/**
 * Field access by path, for UI binding, the inspector and scripting. A path is segments joined
 * by '.': a field name of the current struct, optionally followed by `[n]` on a FIXED_ARRAY
 * field; or, after a vector field (VEC2/3/4, DVEC3, QUAT, COLOR), a component `x` `y` `z` `w`
 * (`r` `g` `b` `a` for COLOR). Examples: "transform.position.x", "items[3].count".
 *
 * On success `*field` gets the description of the last segment and `*offset` its byte offset
 * from the start of the object. A component or an array element gets a description the registry
 * synthesised (offset relative to its parent, flags inherited from the field it belongs to).
 *
 * @return DE_OK; DE_ERR_NOT_FOUND when a segment names no field or component, or an index is out
 *         of range; DE_ERR_INVALID_ARGUMENT for a bad id, NULL arguments or a malformed path.
 *         The error message quotes the failing segment. Outputs are written only on DE_OK.
 */
DE_API de_result de_reflect_resolve_path(const de_type_registry *r, de_type_id type,
                                         const char *path, const de_field_info **field,
                                         u32 *offset);

DE_EXTERN_C_END
#endif
