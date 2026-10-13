/*
 * scene_format.h: the cooked scene and prefab format, version 1 (SPEC §9.2, §11.6; DENG-215).
 * Internal; include as "asset/scene/scene_format.h".
 *
 * The `scene` and `prefab` importers (scene_type.c) write it from TOML; the loader (scene_type.c)
 * checks it and keeps it; de_prefab_instantiate (scene_instantiate.c) spawns it into a world.
 * A prefab is a scene with exactly one root entity.
 *
 * Every multi-byte field is little-endian and is encoded and decoded with the helpers below, never
 * with a struct memcpy, so the bytes are the same on every host (cooks are byte-identical). There
 * is no padding between sections. Layout:
 *
 *   header      DE__SCENE_HEADER_SIZE (48) bytes:
 *                 0  u8[4] magic "DSCN"
 *                 4  u32   format version (DE__SCENE_VERSION)
 *                 8  u32   kind (de__scene_kind)
 *                12  u32   type count T
 *                16  u32   entity count E
 *                20  u32   component count C
 *                24  u32   asset reference count A
 *                28  u32   string bytes S
 *                32  u64   data bytes D
 *                40  u32   root count (entities without a parent)
 *                44  u32   reserved, 0
 *   types       T x 24: u32 name offset (into strings), u32 name length, u32 type version,
 *               u32 reserved 0, u64 schema hash (de_type_schema_hash when cooked). Sorted by name
 *               (bytewise), names unique.
 *   entities    E x 24: u64 scene ID (never 0), u32 parent (entity index, DE__SCENE_NO_PARENT for
 *               a root), u32 first component, u32 component count, u32 reserved 0. Sorted by scene
 *               ID, IDs unique. Components of entity i are [first, first + count); `first` is the
 *               sum of the counts before it.
 *   components  C x 16: u32 type index, u32 data size, u64 data offset (into data). Within an
 *               entity sorted by type index (so by type name), each type once. Offsets run
 *               contiguously through the data section in record order.
 *   asset refs  A x 8: u64 asset IDs the components reference, sorted, unique, never 0. The
 *               loader declares them as load dependencies.
 *   strings     S bytes: type names, each followed by a NUL.
 *   data        D bytes: one binary object per component (de_serialize_binary, self-describing,
 *               fields with DE_FIELD_SERIALIZE; ENTITY fields hold scene IDs, ASSET fields asset
 *               IDs).
 *
 * The file is exactly the sum of these sizes. Bump DE__SCENE_VERSION on any change.
 */
#ifndef DE_ASSET_SCENE_FORMAT_H
#define DE_ASSET_SCENE_FORMAT_H

#include <dengine/de_asset.h>
#include <dengine/de_base.h>

#include <string.h>

DE_EXTERN_C_BEGIN

#define DE__SCENE_MAGIC       "DSCN"
#define DE__SCENE_VERSION     1u
#define DE__SCENE_HEADER_SIZE 48u
#define DE__SCENE_TYPE_SIZE   24u
#define DE__SCENE_ENTITY_SIZE 24u
#define DE__SCENE_COMP_SIZE   16u
#define DE__SCENE_ASSET_SIZE  8u
#define DE__SCENE_NO_PARENT   0xFFFFFFFFu
/** Largest count of each table (types, entities, components, asset refs) a reader accepts. */
#define DE__SCENE_MAX_COUNT (1u << 24)
/** Longest type name (the reflection registry has no limit; the format caps it). */
#define DE__SCENE_MAX_NAME 255u
/** The importer and loader type names, and the importer version (bump it when the output of the
 * importer changes, so `.decache` entries cooked by an older importer are not used). */
#define DE__SCENE_TYPE_NAME    "scene"
#define DE__PREFAB_TYPE_NAME   "prefab"
#define DE__SCENE_IMPORTER_VER 1u

typedef enum de__scene_kind { DE__SCENE_KIND_SCENE = 0, DE__SCENE_KIND_PREFAB = 1 } de__scene_kind;

/* ---- Little-endian helpers ---------------------------------------------------------------- */

static inline void de__scene_put_u32(u8 *p, u32 v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}
static inline void de__scene_put_u64(u8 *p, u64 v) {
    de__scene_put_u32(p, (u32)v);
    de__scene_put_u32(p + 4, (u32)(v >> 32));
}
static inline u32 de__scene_get_u32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static inline u64 de__scene_get_u64(const u8 *p) {
    return (u64)de__scene_get_u32(p) | ((u64)de__scene_get_u32(p + 4) << 32);
}

/* ---- Decoded view -------------------------------------------------------------------------- */

/** A checked cooked scene: counts and pointers to each section of the bytes (not copied). */
typedef struct de__scene_view {
    const u8 *bytes;
    u64 size;
    de__scene_kind kind;
    u32 type_count, entity_count, component_count, asset_count, root_count;
    const u8 *types, *entities, *components, *assets, *strings, *data;
    u32 string_bytes;
    u64 data_bytes;
} de__scene_view;

typedef struct de__scene_type_rec {
    const char *name; /**< NUL-terminated, in the strings section */
    u32 name_len;
    u32 version;
    u64 schema_hash;
} de__scene_type_rec;

typedef struct de__scene_entity_rec {
    u64 scene_id;
    u32 parent; /**< DE__SCENE_NO_PARENT for a root */
    u32 first_component, component_count;
} de__scene_entity_rec;

typedef struct de__scene_comp_rec {
    u32 type_index;
    u32 size;
    const u8 *data; /**< `size` bytes in the data section */
} de__scene_comp_rec;

/**
 * Checks `size` bytes as a cooked scene (every count, offset, order and parent link; the binary
 * objects themselves are checked when instantiated) and fills `*out`. `kind_name` names the asset
 * type in messages. Allocation-free; any thread.
 * @return DE_OK, or DE_ERR_PARSE (DE_ERR_VERSION_MISMATCH for another format version) with a
 *         message in `err` (`err_cap` bytes; may be NULL).
 */
de_result de__scene_view_parse(const void *data, u64 size, de__scene_view *out, char *err,
                               size_t err_cap);

static inline de__scene_type_rec de__scene_type_at(const de__scene_view *v, u32 i) {
    const u8 *p = v->types + (size_t)i * DE__SCENE_TYPE_SIZE;
    de__scene_type_rec r;
    r.name = (const char *)v->strings + de__scene_get_u32(p);
    r.name_len = de__scene_get_u32(p + 4);
    r.version = de__scene_get_u32(p + 8);
    r.schema_hash = de__scene_get_u64(p + 16);
    return r;
}
static inline de__scene_entity_rec de__scene_entity_at(const de__scene_view *v, u32 i) {
    const u8 *p = v->entities + (size_t)i * DE__SCENE_ENTITY_SIZE;
    de__scene_entity_rec r;
    r.scene_id = de__scene_get_u64(p);
    r.parent = de__scene_get_u32(p + 8);
    r.first_component = de__scene_get_u32(p + 12);
    r.component_count = de__scene_get_u32(p + 16);
    return r;
}
static inline de__scene_comp_rec de__scene_comp_at(const de__scene_view *v, u32 i) {
    const u8 *p = v->components + (size_t)i * DE__SCENE_COMP_SIZE;
    de__scene_comp_rec r;
    r.type_index = de__scene_get_u32(p);
    r.size = de__scene_get_u32(p + 4);
    r.data = v->data + de__scene_get_u64(p + 8);
    return r;
}
static inline u64 de__scene_asset_at(const de__scene_view *v, u32 i) {
    return de__scene_get_u64(v->assets + (size_t)i * DE__SCENE_ASSET_SIZE);
}
/** The index of the entity with `scene_id` (binary search), or DE__SCENE_NO_PARENT. */
static inline u32 de__scene_find_entity(const de__scene_view *v, u64 scene_id) {
    u32 lo = 0, hi = v->entity_count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2u;
        u64 id = de__scene_get_u64(v->entities + (size_t)mid * DE__SCENE_ENTITY_SIZE);
        if (id == scene_id)
            return mid;
        if (id < scene_id)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return DE__SCENE_NO_PARENT;
}

/* ---- The loaded object (scene_type.c) ------------------------------------------------------ */

/** The object of a READY scene or prefab slot: the cooked bytes (owned) and their view. */
typedef struct de__scene_object {
    de__scene_view view;
    const de_allocator *alloc;
    size_t block_size; /**< this struct and the bytes after it, one allocation */
} de__scene_object;

/* ---- Built-in types (blob_type.c, scene_type.c) -------------------------------------------- */

/** Registers the `blob` type (blob_type.c). Main thread. */
de_result de__blob_type_register(de_asset_system *s);
/** Registers the `scene` and `prefab` types (scene_type.c). Main thread. */
de_result de__scene_types_register(de_asset_system *s);

DE_EXTERN_C_END

#endif /* DE_ASSET_SCENE_FORMAT_H */
