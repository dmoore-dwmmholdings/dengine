/*
 * reflect_internal.h: the type registry's private structures (DENG-294). Shared by registry.c,
 * validate.c, schema_hash.c and path.c; not part of the public API.
 */
#ifndef DE_REFLECT_INTERNAL_H
#define DE_REFLECT_INTERNAL_H

#include <dengine/de_reflect.h>

#include "core/de_array.h"
#include "core/de_hashmap.h"

DE_EXTERN_C_BEGIN

/* de_type_id: low DE__TYPE_INDEX_BITS hold index + 1, the bits above the generation. */
#define DE__TYPE_INDEX_BITS 20u
#define DE__TYPE_INDEX_MASK ((1u << DE__TYPE_INDEX_BITS) - 1u)
#define DE__TYPE_GEN_MASK   ((1u << (32u - DE__TYPE_INDEX_BITS)) - 1u)
/** Most entries a registry holds (index + 1 must fit the index bits). */
#define DE__TYPE_MAX_ENTRIES (DE__TYPE_INDEX_MASK - 1u)

/** Nesting depth limit for schema hashing and path resolution (guards against cycles). */
#define DE__TYPE_MAX_DEPTH 64u

/** Per-field data the registry derives at registration: array elements and vector components. */
typedef struct de__field_extra {
    de_field_info element;  /* FIXED_ARRAY: the element (see de_type_array_element) */
    de_field_info comps[4]; /* components of a vector field, or of a vector array's elements */
    u32 comp_count;
} de__field_extra;

/** One stored (deep-copied) description of a struct type. */
typedef struct de__type_layout {
    de_type_desc desc;      /* public view; desc.fields == fields */
    de_field_info *fields;  /* desc.field_count entries */
    de__field_extra *extra; /* desc.field_count entries */
} de__type_layout;

typedef enum de__entry_kind { DE__ENTRY_STRUCT = 1, DE__ENTRY_ENUM = 2 } de__entry_kind;

/** One registry slot: a struct type or an enum. */
typedef struct de__type_entry {
    de__entry_kind kind;
    u32 generation;
    u64 name_hash;
    const char *name;        /* registry-owned */
    de__type_layout *cur;    /* STRUCT: current description */
    de__type_layout *prev;   /* STRUCT: before the last layout change, or NULL */
    de_enum_info *enum_info; /* ENUM: current values */
    u64 hash;                /* current schema hash (nested hashes as registered now) */
    u64 registered_hash;     /* schema hash computed when this entry was last registered */
    u32 bin_summary;         /* STRUCT: de__ser_binary_summary(cur), refreshed with `hash` */
} de__type_entry;

/** A bump block of registry memory. */
typedef struct de__reg_block {
    struct de__reg_block *next;
    size_t size; /* bytes of the whole block, header included */
    size_t used;
} de__reg_block;

struct de_type_registry {
    const de_allocator *a;
    de_array entries;   /* de__type_entry, dense, never shrinks */
    de_hashmap by_name; /* key: u64 name hash, value: u32 entry index */
    de__reg_block *blocks;
    u64 revision; /* Successful publications, including metadata/hook-only replacements. */
};

/* ---- registry.c ---------------------------------------------------------------------------- */

/** The entry of a valid id, or NULL. */
const de__type_entry *de__reflect_entry(const de_type_registry *r, de_type_id id);
/** The entry called `name`, or NULL. */
const de__type_entry *de__reflect_find_entry(const de_type_registry *r, const char *name);
/** The current layout of struct type `name`, or NULL. */
const de__type_layout *de__reflect_find_layout(const de_type_registry *r, const char *name);

/* ---- validate.c ---------------------------------------------------------------------------- */

/** The fixed byte size of a kind, or 0 for kinds whose size depends on the field. */
u32 de__reflect_kind_fixed_size(de_field_kind kind);
/** Component count of a vector kind (0 for other kinds), and the size of one component. */
u32 de__reflect_kind_components(de_field_kind kind, u32 *comp_size);
/** The kind called `name` ("f32", "vec3", ...), or false. */
bool de__reflect_builtin_kind(const char *name, de_field_kind *out_kind);
/** true if `s` is a C identifier ([A-Za-z_][A-Za-z0-9_]*). */
bool de__reflect_is_identifier(const char *s);

/**
 * Checks `desc` against the registry (see de_type_register_ex), including nesting that would
 * reach desc->name itself (a cycle through a re-registration). Sets the error context.
 */
de_result de__reflect_validate_type(const de_type_registry *r, const de_type_desc *desc);
/** Checks an enum registration. Sets the error context. */
de_result de__reflect_validate_enum(const char *name, const de_enum_value *values, u32 count);

/**
 * Element description of a FIXED_ARRAY field (validated): kind, size and nested name.
 * `*out_elem_nested` gets the struct/enum name, or NULL for built-in kinds.
 */
de_result de__reflect_array_element(const de_type_registry *r, const char *type_name,
                                    const de_field_info *f, de_field_kind *out_kind, u32 *out_size,
                                    const char **out_elem_nested);

/* ---- schema_hash.c ------------------------------------------------------------------------- */

/**
 * Schema hash of a struct description whose nested types are resolved in `r` (their current
 * hashes). `depth` guards against cycles.
 */
u64 de__reflect_hash_type(const de_type_registry *r, const de_type_desc *desc, u32 depth);
/** Schema hash of an enum. */
u64 de__reflect_hash_enum(const de_enum_info *e);
/** Recomputes every entry's current hash (after a layout change of a nested type). */
void de__reflect_rehash_all(de_type_registry *r);

/* ---- ser_binary.c (DENG-296) --------------------------------------------------------------- */

/* de__type_entry.bin_summary: what the binary serializer's memcpy fast path needs to know about a
 * type, computed once per (re)registration so the per-object check is O(1). It depends only on
 * data the schema hash covers, so it is refreshed exactly when the hash is. */
#define DE__BIN_SUM_FLAGS_AND  0xFFu     /* AND of (flags & DE_FIELD_SCHEMA_FLAGS), recursive */
#define DE__BIN_SUM_PACKED     (1u << 8) /* packed encoding == memory image (raw references) */
#define DE__BIN_SUM_HAS_ENTITY (1u << 9) /* an ENTITY field, at any depth */

/** The summary of a stored layout whose nested types are resolved in `r` (current layouts). */
u32 de__ser_binary_summary(const de_type_registry *r, const de__type_layout *l, u32 depth);

DE_EXTERN_C_END

#endif /* DE_REFLECT_INTERNAL_H */
