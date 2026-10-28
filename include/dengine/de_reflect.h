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

/* ---------- Inspector hints (DENG-555, DENG-624) ----------
 * Editor metadata for one field, kept beside the registry (never part of the schema hash).
 * Zero means none. */
typedef struct de_field_hints {
    const char *group;          /**< a heading the field sits under */
    i32 order;                  /**< display order, lower first (0 = declaration order) */
    const char *show_if;        /**< "mode == 2", "kind != point", "enabled", "!enabled" */
    f64 slider_min, slider_max; /**< a slider with this range, if min < max */
    bool multiline;             /**< a string edited in a text area */
    bool color;                 /**< a vec3 / vec4 edited as a colour */
} de_field_hints;

/** Sets the hints of field `field` of the struct type called `type_name` (strings copied).
 * Main thread, like registration. DE_ERR_OUT_OF_MEMORY or DE_ERR_INVALID_ARGUMENT. */
DE_API de_result de_field_hints_set(const char *type_name, const char *field,
                                    const de_field_hints *hints);
/** The hints of a field, or NULL. */
DE_API const de_field_hints *de_field_hints_get(const char *type_name, const char *field);

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

/** Monotonic publication revision: starts at0 and increments once per successful struct or
 * enum registration/replacement, including unchanged schemas with new metadata/hooks.
 * Failed registration and lookups do not change it. Caches should re-resolve after a change,
 * including previously invalid paths. No allocation; NULL returns0. Registration is exclusive
 * (as for the other registry operations); query while no thread is registering. */
DE_API u64 de_type_registry_revision(const de_type_registry *r);

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

/** Writes deterministic debugger definitions from the current registry (DENG-586).
 * Each non-NULL writer receives a complete Natvis XML or LLDB Python module. At least one
 * writer is required. Registration must be idle; the registry is unchanged. Sorting uses
 * the registry allocator and is freed before return. Writer failures propagate; callers
 * must discard partial output. Generated formatters never execute debuggee functions.
 * Core string/container summaries are bounded; registered structs retain normal expansion.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT, DE_ERR_OUT_OF_MEMORY, or a writer error.
 * Shipping returns DE_ERR_UNSUPPORTED and contains no embedded Python formatter.
 */
DE_API de_result de_debug_visualizers_write(const de_type_registry *r, de_writer *natvis,
                                           de_writer *lldb);
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
 * Reads the table `member` of entry `index` of `[[table_key]]` (NULL key = the top-level table)
 * into `obj`, with the rules of de_deserialize_object (DENG-215). A scene entity keeps one such
 * table per component:
 *
 *     [[entity]]
 *     id = "0x00000000000000a1"
 *     [entity.Health]
 *     current = 5.0
 *
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (bad id, NULL argument, no such entry), DE_ERR_NOT_FOUND
 *         (the entry has no key `member`), DE_ERR_PARSE (also: `member` is not a table),
 *         DE_ERR_VERSION_MISMATCH or DE_ERR_OUT_OF_MEMORY.
 */
DE_API de_result de_text_doc_read_member(const de_text_doc *doc, const char *table_key, u32 index,
                                         const char *member, const de_type_registry *r,
                                         de_type_id type, void *obj, const de_ser_options *opt);

/**
 * Field access by path, for UI binding, the inspector and scripting. A path is segments joined
 * by '.': a field name of the current struct, optionally followed by `[n]` on a FIXED_ARRAY
 * field; or, after a vector field (VEC2/3/4, DVEC3, QUAT, COLOR), a component `x` `y` `z` `w`
 * (`r` `g` `b` `a` for COLOR), or `[n]` for component n. Examples: "transform.position.x",
 * "transform.position[0]" (the same), "items[3].count".
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

/* ---------- Serialization: binary format (DENG-296) ----------
 * The shipping format of SPEC §9.2: versioned, little-endian, with the schema hash of the type.
 * The byte layout is documented in src/reflect/ser_binary_format.h. */

/** Whether a binary object carries a schema block (de_ser_binary_options.schema). */
typedef enum de_ser_binary_schema {
    /** Self-describing in Debug and Dev builds (DE_DEBUG / DE_DEV), compact otherwise. */
    DE_SER_BINARY_SCHEMA_DEFAULT = 0,
    /** Write a schema block (field names, kinds, sizes, offsets, nested types). A reader whose
     * type changed since (different schema hash) matches fields by name and runs `migrate`. */
    DE_SER_BINARY_SELF_DESCRIBING,
    /** No schema block: smaller, but only a reader with the same schema hash can load it
     * (otherwise DE_ERR_VERSION_MISMATCH). For data that is rebuilt with the game (cooked). */
    DE_SER_BINARY_COMPACT
} de_ser_binary_schema;

/** Counters the binary serializer adds to (never resets). For tests, tools and profiling. */
typedef struct de_ser_binary_stats {
    u64 objects_written;
    u64 objects_read;   /**< objects that loaded (DE_OK) */
    u64 fast_path;      /**< objects written or read as one memcpy of the whole object */
    u64 field_wise;     /**< objects written, or read with a matching schema hash, per field */
    u64 schema_matched; /**< objects read through their stored schema (schema hash differed) */
    u64 migrations;     /**< migrate hook calls, nested types included */
    u64 skipped_fields; /**< stored fields not loaded: unknown, or kind or size changed */
    u64 warnings;       /**< warnings logged (category "reflect"; at most one per object read) */
} de_ser_binary_stats;

/** Binary-only options. NULL or `(de_ser_binary_options){0}` gives the defaults. */
typedef struct de_ser_binary_options {
    de_ser_binary_schema schema; /**< writing only */
    de_ser_binary_stats *stats;  /**< optional; counters are added to */
    /** Never take the memcpy fast path (tests and benchmarks compare the two paths). */
    bool disable_fast_path;
} de_ser_binary_options;

/**
 * Writes `obj` (an object of struct type `type`) as one binary object to `w`: header, optional
 * schema block, then the fields selected by `opt->required_flags` (a field is written when
 * `(flags & required_flags) == required_flags`, also inside nested structs). Entities and asset
 * handles go through `opt->ctx` (each callback gets `ctx->user`; NULL = raw values). `opt` NULL
 * means all fields and raw values; `opt->format` is ignored.
 *
 * Writes are batched (at most 1 KiB per call, larger runs in one call). Allocates nothing except
 * thread scratch (de_scratch_begin) for the schema block's type table.
 *
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (NULL `r`, `obj` or `w`, a bad id, or required_flags
 *         above 0xFF); or the writer's error. After an error, `w` may hold part of the object.
 */
DE_API de_result de_serialize_binary(const de_type_registry *r, de_type_id type, const void *obj,
                                     const de_ser_options *opt, const de_ser_binary_options *bopt,
                                     de_writer *w);

/**
 * Reads one binary object of struct type `type` from `rd` into `obj`. The object is not
 * constructed first: pass an initialised object (de_type_construct); fields absent from the
 * data, or not selected by `opt->required_flags`, keep their values.
 *
 * - Same schema hash as stored: the fields are read with the current layout; a POD type whose
 *   packed encoding equals its memory image is read with one memcpy (de_ser_binary_stats).
 * - Different hash and a schema block: the stored object is decoded into an image of its old
 *   layout (old offsets and sizes), fields are matched by name (the same kind; STRING buffers,
 *   FIXED_ARRAY counts and nested structs may change), stored fields that match nothing are
 *   skipped with one warning per object, and when the stored version is older the type's
 *   `migrate(obj, from_version, old_image, old_fields, n)` hook runs (nested types first).
 *   old_fields describe the old image: name, kind, size, offset, flags, nested_type,
 *   array_count. Strings in it live in thread scratch and are valid only during the call.
 *   The image is untrusted input: check old_fields before reading it as an old struct, and
 *   read it with memcpy (a nested image is only as aligned as its stored offset).
 * - Different hash and no schema block: DE_ERR_VERSION_MISMATCH.
 *
 * Every length is checked against the payload and schema sizes: truncated or corrupt input gives
 * DE_ERR_PARSE and never reads or writes out of bounds.
 *
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (NULL arguments, bad id); DE_ERR_PARSE (not a binary
 *         object of this type, truncated, corrupt); DE_ERR_VERSION_MISMATCH (newer version or
 *         format, hash mismatch without a schema block, or `migrate` returned false); or the
 *         reader's error. After DE_OK and DE_ERR_VERSION_MISMATCH the reader stands right after
 *         the object; after other errors its position is unspecified. On DE_ERR_PARSE in the
 *         same-hash path `obj` may be partly overwritten; the schema path writes `obj` only
 *         after the whole object decoded.
 */
DE_API de_result de_deserialize_binary(const de_type_registry *r, de_type_id type, void *obj,
                                       const de_ser_options *opt, const de_ser_binary_options *bopt,
                                       de_reader *rd);

/**
 * A de_writer into memory: either a buffer that grows from an allocator (de_mem_writer_init) or
 * a fixed caller buffer (de_mem_writer_init_fixed; a write that does not fit fails with
 * DE_ERR_OUT_OF_MEMORY and writes nothing). Bytes written are data[0 .. size).
 */
typedef struct de_mem_writer {
    u8 *data;
    size_t size;
    size_t capacity;
    const de_allocator *a; /**< growth allocator; NULL for a fixed buffer */
} de_mem_writer;

/** A growing writer; `a` NULL = de_heap_allocator(). Free with de_mem_writer_free. */
DE_API void de_mem_writer_init(de_mem_writer *mw, const de_allocator *a);
/** A writer into `buf` (`capacity` bytes). Nothing to free. */
DE_API void de_mem_writer_init_fixed(de_mem_writer *mw, void *buf, size_t capacity);
/** Forgets the bytes written (size = 0) and keeps the buffer. */
DE_API void de_mem_writer_reset(de_mem_writer *mw);
/** Frees a growing writer's buffer and zeroes `mw`. A fixed writer is only zeroed. */
DE_API void de_mem_writer_free(de_mem_writer *mw);
/** The de_writer that appends to `mw`. `mw` must outlive it. */
DE_API de_writer de_writer_from_mem(de_mem_writer *mw);

/** A de_reader over `size` bytes at `data` (not copied); `pos` is the next byte to read. */
typedef struct de_mem_reader {
    const u8 *data;
    size_t size;
    size_t pos;
} de_mem_reader;

DE_API void de_mem_reader_init(de_mem_reader *mr, const void *data, size_t size);
/** The de_reader that reads `mr` from `pos`; at the end it reports 0 bytes read. */
DE_API de_reader de_reader_from_mem(de_mem_reader *mr);

DE_EXTERN_C_END
#endif
