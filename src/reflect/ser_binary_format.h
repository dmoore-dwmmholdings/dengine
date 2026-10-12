/*
 * ser_binary_format.h: the byte layout of one binary-serialized object (DENG-296, SPEC §9.2).
 * Private to src/reflect/; the layout is a cross-platform file-format contract (cooked scenes and
 * prefabs, save games, snapshot tooling), checked byte for byte by the golden test
 * (tests/unit/reflect/data/bin_golden_transform.bin).
 *
 * All integers are little-endian. str(s) = u32 byte length followed by the bytes (no NUL).
 *
 *   Header (DE__BIN_HEADER_SIZE = 32 bytes):
 *     off  0  u8[4]  magic "DEBO"
 *     off  4  u16    format version (DE__BIN_FORMAT_VERSION)
 *     off  6  u16    flags: bit 0 DE__BIN_FLAG_SELF_DESCRIBING; bits 8..15 the required_flags the
 *                    writer filtered fields with (so a reader knows which fields are present);
 *                    every other bit is 0
 *     off  8  u64    type name hash: de_hash64_str(type name)
 *     off 16  u32    type version
 *     off 20  u64    schema hash: de_type_schema_hash() of the type when written
 *     off 28  u32    payload size: bytes of field data (the schema block is not counted)
 *
 *   Schema block (only with DE__BIN_FLAG_SELF_DESCRIBING), directly after the header:
 *     u32 block size (bytes that follow, up to DE__BIN_MAX_SCHEMA_BYTES)
 *     u32 type count (>= 1)
 *     per type, in topological order (the object's type first; a type comes before every type
 *     its fields nest, so a reference always points forward and the table has no cycles):
 *       u64 schema hash, u64 name hash, u32 version, u32 size, u32 field count, str(name)
 *       per present field, in declaration order:
 *         u64 name hash (de_hash64_str(name))
 *         u64 nested hash: as in the schema hash (Decision page "Schema hash is XXH3-64 over a
 *             versioned little-endian encoding"): the schema hash of the struct or enum that
 *             STRUCT / ENUM / FLAGS / FIXED_ARRAY name, de_hash64_str of a built-in element name
 *             or of an ASSET's asset type, else 0. A STRUCT field (or FIXED_ARRAY of structs)
 *             names a later table entry by this hash.
 *         u32 size, u32 offset, u32 flags (all de_field_flags bits), u32 array count
 *         u8 kind (de_field_kind), u8 element kind (FIXED_ARRAY; else 0)
 *         str(name), str(nested type name, or empty)
 *
 *   Payload (payload size bytes): the present fields in declaration order, each encoded as:
 *     BOOL u8 0/1 · I8 U8 1 byte · I16 U16 2 · I32 U32 F32 4 · I64 U64 F64 NAME 8
 *     VEC2/VEC3/VEC4/QUAT/COLOR 2/3/4/4/4 f32 · DVEC3 3 f64 · MAT4 16 f32 (column-major)
 *     ENUM / FLAGS: an integer of the field size
 *     STRING: str (byte length < field size, the bytes before the first NUL)
 *     ENTITY: u64 (the scene ID from de_ser_context.entity_to_scene_id, or the raw entity bits)
 *     ASSET: u64 (the asset ID from de_ser_context.asset_to_id, or the raw handle)
 *     STRUCT: the nested type's present fields, recursively
 *     FIXED_ARRAY: array count elements, each encoded as its element kind
 *   A field is present when (flags & required_flags) == required_flags; the rule applies to the
 *   fields of nested structs too. Padding is never written.
 *
 * When the type is POD, every field is present and the packed encoding equals the object's memory
 * image (fields contiguous in declaration order, no BOOL / STRING / ASSET, no entity mapping), the
 * payload is the object's bytes and both directions are one memcpy (the fast path).
 */
#ifndef DE_REFLECT_SER_BINARY_FORMAT_H
#define DE_REFLECT_SER_BINARY_FORMAT_H

#include <dengine/de_base.h>

/* Every engine target is little-endian (SPEC §3: x86-64, arm64). The payload of numbers is
 * copied with memcpy, so a big-endian host would write a different format. */
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
DE_STATIC_ASSERT(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
                 "the binary serializer requires a little-endian host");
#elif !defined(_WIN32)
#error "the binary serializer requires a little-endian host; add a check for this compiler"
#endif

#define DE__BIN_MAGIC0 'D'
#define DE__BIN_MAGIC1 'E'
#define DE__BIN_MAGIC2 'B'
#define DE__BIN_MAGIC3 'O'

/** Version of this layout. Bump it with any change, together with "de.type.v1" if the schema
 * hash encoding changes. */
#define DE__BIN_FORMAT_VERSION 1u

#define DE__BIN_HEADER_SIZE 32u

#define DE__BIN_FLAG_SELF_DESCRIBING 0x0001u
/** Header flag bits that format version 1 defines (the required_flags byte included). */
#define DE__BIN_FLAGS_KNOWN     0xFF01u
#define DE__BIN_FLAGS_REQ_SHIFT 8u

/** Largest schema block and largest stored type size a reader accepts (corrupt-input guard). */
#define DE__BIN_MAX_SCHEMA_BYTES (16u << 20)
#define DE__BIN_MAX_TYPE_SIZE    (16u << 20)

/** Fixed bytes of a type record and of a field record (strings excluded). */
#define DE__BIN_TYPE_RECORD_FIXED  28u
#define DE__BIN_FIELD_RECORD_FIXED 34u

#endif /* DE_REFLECT_SER_BINARY_FORMAT_H */
