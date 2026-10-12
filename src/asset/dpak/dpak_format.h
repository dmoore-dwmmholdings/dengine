/*
 * dpak_format.h: the .dpak pack archive format, version 1 (SPEC §11.7). Internal, header-only.
 *
 * Include as "asset/dpak/dpak_format.h". Shared by the writer (tools/pack/lib/dpak_writer.c,
 * DENG-211) and the runtime reader (src/asset/dpak/, DENG-212). The reader must never include
 * writer code. The byte-level description and the version history are in docs/formats/dpak.md;
 * this header and that file must agree.
 *
 * Every multi-byte field is little-endian. The structs below are the decoded form of each record,
 * with no implicit padding and sizes checked with _Static_assert, but code never copies them to or
 * from a file with memcpy: it encodes and decodes with the dpak_*_store / dpak_*_load helpers, so
 * the bytes are the same whatever the host's endianness and struct layout (packs cooked on macOS
 * and Linux must be byte-identical, DENG-216).
 *
 * File layout (offsets from the start of the file):
 *
 *   [header, DPAK_HEADER_SIZE bytes]
 *   [sections, in dpak_section order, each starting on an 8-byte boundary]
 *   [dictionary blobs, each starting on an 8-byte boundary]
 *   [asset data: each asset's chunks back to back, each asset starting on `alignment`]
 *
 * Padding bytes are zero. The file ends with the last byte of asset data (or of the last
 * dictionary or section when no asset has data). The header's content_hash is XXH3-64 (seed 0,
 * de_hash64) of every byte after the header.
 */
#ifndef DE_ASSET_DPAK_FORMAT_H
#define DE_ASSET_DPAK_FORMAT_H

#include <dengine/de_base.h>

DE_EXTERN_C_BEGIN

/* ---- Constants --------------------------------------------------------------------------- */

/** The first four bytes of every pack. */
#define DPAK_MAGIC_0 'D'
#define DPAK_MAGIC_1 'P'
#define DPAK_MAGIC_2 'A'
#define DPAK_MAGIC_3 'K'

/**
 * Format version. Bump it on any change to the layout, to the meaning of a field, to the asset
 * ID hash (de_hash64, seed 0, of the asset path; Trama "Decision: Names and asset IDs are XXH3-64
 * of the exact bytes") or to the type hash. Record the change in docs/formats/dpak.md.
 */
#define DPAK_VERSION 1u

/** Size of the header in bytes; stored in the header too. */
#define DPAK_HEADER_SIZE 256u

/** Largest (and default) raw size of one chunk. Every chunk but an asset's last is this size. */
#define DPAK_MAX_CHUNK_SIZE 65536u
/** Smallest chunk size a writer accepts. */
#define DPAK_MIN_CHUNK_SIZE 1024u

/** Default data alignment: each asset's first chunk starts on a multiple of it. */
#define DPAK_DEFAULT_ALIGNMENT 4096u
/** Alignment limits: a power of two in [DPAK_MIN_ALIGNMENT, DPAK_MAX_ALIGNMENT]. */
#define DPAK_MIN_ALIGNMENT 8u
#define DPAK_MAX_ALIGNMENT (1u << 20)

/** Sections and dictionary blobs start on multiples of this. */
#define DPAK_SECTION_ALIGNMENT 8u

/** dpak_chunk_entry.dict_index of a chunk compressed without a dictionary. */
#define DPAK_DICT_NONE 0xFFFFu
/** Most dictionaries one pack holds (dict_index is a u16 and DPAK_DICT_NONE is reserved). */
#define DPAK_MAX_DICTS 0xFFFEu

/**
 * A writer trains a per-type dictionary only for a type with at least this many chunks
 * (samples). zstd's trainer needs several samples to find shared content.
 */
#define DPAK_DICT_MIN_SAMPLES 8u
/** Largest dictionary a writer trains (zstd's recommended maximum, 110 KiB). */
#define DPAK_DICT_MAX_SIZE (110u * 1024u)

/** dpak_header.platform: the platform the pack was cooked for. */
typedef enum dpak_platform {
    DPAK_PLATFORM_ANY = 0, /**< platform-independent data */
    DPAK_PLATFORM_WINDOWS = 1,
    DPAK_PLATFORM_LINUX = 2,
    DPAK_PLATFORM_MACOS = 3,
    DPAK_PLATFORM_COUNT_
} dpak_platform;

/** dpak_header.flags. */
typedef enum dpak_header_flags {
    DPAK_HEADER_HAS_NAMES = 1u << 0, /**< the NAMES section holds a path for every TOC entry */
} dpak_header_flags;

/** dpak_toc_entry.flags. */
typedef enum dpak_entry_flags {
    DPAK_ENTRY_SUB_ASSET = 1u << 0,    /**< the asset path has a `#` part ("model#Body") */
    DPAK_ENTRY_UNCOMPRESSED = 1u << 1, /**< every chunk is stored raw (DPAK_CHUNK_RAW) */
} dpak_entry_flags;

/** dpak_chunk_entry.flags. */
typedef enum dpak_chunk_flags {
    DPAK_CHUNK_RAW = 1u << 0, /**< stored as is: stored_size == raw_size, no zstd frame */
} dpak_chunk_flags;

/** dpak_bundle_entry.flags. */
typedef enum dpak_bundle_flags {
    DPAK_BUNDLE_RESIDENT = 1u << 0, /**< never evicted (assets.toml `resident = true`) */
} dpak_bundle_flags;

/** dpak_bundle_entry.priority: SPEC §11.3 load priorities, most urgent first. */
typedef enum dpak_priority {
    DPAK_PRIORITY_CRITICAL = 0,
    DPAK_PRIORITY_HIGH = 1,
    DPAK_PRIORITY_NORMAL = 2,
    DPAK_PRIORITY_LOW = 3,
    DPAK_PRIORITY_BACKGROUND = 4,
    DPAK_PRIORITY_COUNT_
} dpak_priority;

/** The sections, in file order. dpak_header.sections[] is indexed by this. */
typedef enum dpak_section {
    DPAK_SECTION_TOC = 0,    /**< dpak_toc_entry[count], sorted by id, ids unique */
    DPAK_SECTION_CHUNKS,     /**< dpak_chunk_entry[count], in TOC order */
    DPAK_SECTION_DEPS,       /**< u64[count]: asset ids; each entry's deps sorted, unique */
    DPAK_SECTION_BUNDLES,    /**< dpak_bundle_entry[count], sorted by name_hash, unique */
    DPAK_SECTION_BUNDLE_IDS, /**< u64[count]: bundle members (asset ids), depends (name hashes) */
    DPAK_SECTION_DICTS,      /**< dpak_dict_entry[count], sorted by type_hash */
    DPAK_SECTION_NAMES,      /**< dpak_name_entry[count]: count == TOC count, or 0 (no names) */
    DPAK_SECTION_STRINGS,    /**< bytes: names and bundle names, each followed by a NUL */
    DPAK_SECTION_COUNT
} dpak_section;

/* ---- Records (decoded form) ---------------------------------------------------------------- */

/** Where a section is. An empty section has offset, size and count 0. 24 bytes. */
typedef struct dpak_section_desc {
    u64 offset;   /**< from the start of the file; a multiple of DPAK_SECTION_ALIGNMENT */
    u64 size;     /**< bytes */
    u32 count;    /**< records (bytes for STRINGS) */
    u32 reserved; /**< 0 */
} dpak_section_desc;

/** The header at offset 0. DPAK_HEADER_SIZE bytes. */
typedef struct dpak_header {
    u8 magic[4];      /**<   0: "DPAK" */
    u32 version;      /**<   4: DPAK_VERSION */
    u32 header_size;  /**<   8: DPAK_HEADER_SIZE */
    u32 platform;     /**<  12: dpak_platform */
    u8 build_id[16];  /**<  16: opaque build identifier, chosen by the cooker */
    u64 content_hash; /**<  32: de_hash64(file + header_size, file_size - header_size, 0) */
    u64 file_size;    /**<  40: total bytes */
    u32 flags;        /**<  48: dpak_header_flags */
    u32 chunk_size;   /**<  52: raw bytes per full chunk, <= DPAK_MAX_CHUNK_SIZE */
    u32 alignment;    /**<  56: data alignment, a power of two */
    u32 reserved;     /**<  60: 0 */
    dpak_section_desc sections[DPAK_SECTION_COUNT]; /**< 64: indexed by dpak_section */
} dpak_header;

/** One asset. 64 bytes. */
typedef struct dpak_toc_entry {
    u64 id;          /**<  0: asset id: de_hash64 of the asset path, seed 0 */
    u64 type_hash;   /**<  8: de_hash64 of the type name ("texture"), seed 0 */
    u64 data_offset; /**< 16: file offset of the first chunk; a multiple of the alignment */
    u64 raw_size;    /**< 24: bytes after decompression */
    u64 stored_size; /**< 32: bytes in the file (sum of the chunks' stored sizes) */
    u32 first_chunk; /**< 40: index into CHUNKS */
    u32 chunk_count; /**< 44: ceil(raw_size / chunk_size); 0 for an empty asset */
    u32 dep_first;   /**< 48: index into DEPS */
    u32 dep_count;   /**< 52 */
    u32 flags;       /**< 56: dpak_entry_flags */
    u32 reserved;    /**< 60: 0 */
} dpak_toc_entry;

/** One chunk of an asset. 24 bytes. */
typedef struct dpak_chunk_entry {
    u64 file_offset; /**<  0 */
    u32 stored_size; /**<  8: bytes in the file */
    u32 raw_size;    /**< 12: bytes after decompression, <= chunk_size */
    u16 dict_index;  /**< 16: index into DICTS, or DPAK_DICT_NONE */
    u16 flags;       /**< 18: dpak_chunk_flags */
    u32 reserved;    /**< 20: 0 */
} dpak_chunk_entry;

/** One bundle (SPEC §11.1), so a packed game needs no assets.toml. 40 bytes. */
typedef struct dpak_bundle_entry {
    u64 name_hash;    /**<  0: de_hash64 of the bundle name, seed 0 */
    u32 name_offset;  /**<  8: into STRINGS */
    u32 name_len;     /**< 12: bytes, without the NUL */
    u32 member_first; /**< 16: index into BUNDLE_IDS: member asset ids, sorted, unique */
    u32 member_count; /**< 20 */
    u32 depend_first; /**< 24: index into BUNDLE_IDS: name hashes of the bundles it depends on */
    u32 depend_count; /**< 28: sorted, unique */
    u32 flags;        /**< 32: dpak_bundle_flags */
    u32 priority;     /**< 36: dpak_priority */
} dpak_bundle_entry;

/** One zstd dictionary, trained for one asset type. 24 bytes. */
typedef struct dpak_dict_entry {
    u64 type_hash;   /**<  0: the type whose chunks may use it */
    u64 file_offset; /**<  8: the dictionary bytes (zstd dictionary format) */
    u32 size;        /**< 16: bytes */
    u32 zstd_id;     /**< 20: ZDICT_getDictID() of the dictionary (frames do not store it) */
} dpak_dict_entry;

/** The path of the TOC entry with the same index. 8 bytes. */
typedef struct dpak_name_entry {
    u32 offset; /**< 0: into STRINGS */
    u32 len;    /**< 4: bytes, without the NUL; 0 when the writer had no name for the asset */
} dpak_name_entry;

/** Encoded size of each record. The structs have no implicit padding, so these equal sizeof. */
#define DPAK_SECTION_DESC_SIZE 24u
#define DPAK_TOC_ENTRY_SIZE    64u
#define DPAK_CHUNK_ENTRY_SIZE  24u
#define DPAK_BUNDLE_ENTRY_SIZE 40u
#define DPAK_DICT_ENTRY_SIZE   24u
#define DPAK_NAME_ENTRY_SIZE   8u
#define DPAK_ID_SIZE           8u

DE_STATIC_ASSERT(sizeof(dpak_section_desc) == DPAK_SECTION_DESC_SIZE, "dpak_section_desc size");
DE_STATIC_ASSERT(sizeof(dpak_header) == DPAK_HEADER_SIZE, "dpak_header size");
DE_STATIC_ASSERT(sizeof(dpak_toc_entry) == DPAK_TOC_ENTRY_SIZE, "dpak_toc_entry size");
DE_STATIC_ASSERT(sizeof(dpak_chunk_entry) == DPAK_CHUNK_ENTRY_SIZE, "dpak_chunk_entry size");
DE_STATIC_ASSERT(sizeof(dpak_bundle_entry) == DPAK_BUNDLE_ENTRY_SIZE, "dpak_bundle_entry size");
DE_STATIC_ASSERT(sizeof(dpak_dict_entry) == DPAK_DICT_ENTRY_SIZE, "dpak_dict_entry size");
DE_STATIC_ASSERT(sizeof(dpak_name_entry) == DPAK_NAME_ENTRY_SIZE, "dpak_name_entry size");
DE_STATIC_ASSERT(DPAK_HEADER_SIZE == 64u + DPAK_SECTION_COUNT * DPAK_SECTION_DESC_SIZE,
                 "dpak header: 64 fixed bytes + the section table");

/** Size in bytes of one record of section `s` (1 for STRINGS). */
static inline u32 dpak_section_record_size(dpak_section s) {
    switch (s) {
    case DPAK_SECTION_TOC:
        return DPAK_TOC_ENTRY_SIZE;
    case DPAK_SECTION_CHUNKS:
        return DPAK_CHUNK_ENTRY_SIZE;
    case DPAK_SECTION_DEPS:
    case DPAK_SECTION_BUNDLE_IDS:
        return DPAK_ID_SIZE;
    case DPAK_SECTION_BUNDLES:
        return DPAK_BUNDLE_ENTRY_SIZE;
    case DPAK_SECTION_DICTS:
        return DPAK_DICT_ENTRY_SIZE;
    case DPAK_SECTION_NAMES:
        return DPAK_NAME_ENTRY_SIZE;
    case DPAK_SECTION_STRINGS:
    case DPAK_SECTION_COUNT:
    default:
        return 1u;
    }
}

/* ---- Little-endian load/store -------------------------------------------------------------- */

static inline void dpak_store_u16(u8 *p, u16 v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static inline void dpak_store_u32(u8 *p, u32 v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

static inline void dpak_store_u64(u8 *p, u64 v) {
    dpak_store_u32(p, (u32)v);
    dpak_store_u32(p + 4, (u32)(v >> 32));
}

static inline u16 dpak_load_u16(const u8 *p) {
    return (u16)(p[0] | (p[1] << 8));
}

static inline u32 dpak_load_u32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static inline u64 dpak_load_u64(const u8 *p) {
    return (u64)dpak_load_u32(p) | ((u64)dpak_load_u32(p + 4) << 32);
}

/* ---- Record encode/decode -------------------------------------------------------------------
 * Each encoder writes exactly DPAK_<RECORD>_SIZE bytes at `p`; each decoder reads that many.
 * Decoders do no validation: the reader checks the values (counts, offsets, sizes) itself. */

static inline void dpak_section_desc_store(u8 *p, const dpak_section_desc *s) {
    dpak_store_u64(p + 0, s->offset);
    dpak_store_u64(p + 8, s->size);
    dpak_store_u32(p + 16, s->count);
    dpak_store_u32(p + 20, s->reserved);
}

static inline void dpak_section_desc_load(const u8 *p, dpak_section_desc *s) {
    s->offset = dpak_load_u64(p + 0);
    s->size = dpak_load_u64(p + 8);
    s->count = dpak_load_u32(p + 16);
    s->reserved = dpak_load_u32(p + 20);
}

static inline void dpak_header_store(u8 *p, const dpak_header *h) {
    for (u32 i = 0; i < 4u; ++i)
        p[i] = h->magic[i];
    dpak_store_u32(p + 4, h->version);
    dpak_store_u32(p + 8, h->header_size);
    dpak_store_u32(p + 12, h->platform);
    for (u32 i = 0; i < 16u; ++i)
        p[16 + i] = h->build_id[i];
    dpak_store_u64(p + 32, h->content_hash);
    dpak_store_u64(p + 40, h->file_size);
    dpak_store_u32(p + 48, h->flags);
    dpak_store_u32(p + 52, h->chunk_size);
    dpak_store_u32(p + 56, h->alignment);
    dpak_store_u32(p + 60, h->reserved);
    for (u32 i = 0; i < (u32)DPAK_SECTION_COUNT; ++i)
        dpak_section_desc_store(p + 64 + i * DPAK_SECTION_DESC_SIZE, &h->sections[i]);
}

static inline void dpak_header_load(const u8 *p, dpak_header *h) {
    for (u32 i = 0; i < 4u; ++i)
        h->magic[i] = p[i];
    h->version = dpak_load_u32(p + 4);
    h->header_size = dpak_load_u32(p + 8);
    h->platform = dpak_load_u32(p + 12);
    for (u32 i = 0; i < 16u; ++i)
        h->build_id[i] = p[16 + i];
    h->content_hash = dpak_load_u64(p + 32);
    h->file_size = dpak_load_u64(p + 40);
    h->flags = dpak_load_u32(p + 48);
    h->chunk_size = dpak_load_u32(p + 52);
    h->alignment = dpak_load_u32(p + 56);
    h->reserved = dpak_load_u32(p + 60);
    for (u32 i = 0; i < (u32)DPAK_SECTION_COUNT; ++i)
        dpak_section_desc_load(p + 64 + i * DPAK_SECTION_DESC_SIZE, &h->sections[i]);
}

/** True if the first four bytes at `p` are the magic. */
static inline bool dpak_magic_ok(const u8 *p) {
    return p[0] == (u8)DPAK_MAGIC_0 && p[1] == (u8)DPAK_MAGIC_1 && p[2] == (u8)DPAK_MAGIC_2 &&
           p[3] == (u8)DPAK_MAGIC_3;
}

static inline void dpak_toc_entry_store(u8 *p, const dpak_toc_entry *e) {
    dpak_store_u64(p + 0, e->id);
    dpak_store_u64(p + 8, e->type_hash);
    dpak_store_u64(p + 16, e->data_offset);
    dpak_store_u64(p + 24, e->raw_size);
    dpak_store_u64(p + 32, e->stored_size);
    dpak_store_u32(p + 40, e->first_chunk);
    dpak_store_u32(p + 44, e->chunk_count);
    dpak_store_u32(p + 48, e->dep_first);
    dpak_store_u32(p + 52, e->dep_count);
    dpak_store_u32(p + 56, e->flags);
    dpak_store_u32(p + 60, e->reserved);
}

static inline void dpak_toc_entry_load(const u8 *p, dpak_toc_entry *e) {
    e->id = dpak_load_u64(p + 0);
    e->type_hash = dpak_load_u64(p + 8);
    e->data_offset = dpak_load_u64(p + 16);
    e->raw_size = dpak_load_u64(p + 24);
    e->stored_size = dpak_load_u64(p + 32);
    e->first_chunk = dpak_load_u32(p + 40);
    e->chunk_count = dpak_load_u32(p + 44);
    e->dep_first = dpak_load_u32(p + 48);
    e->dep_count = dpak_load_u32(p + 52);
    e->flags = dpak_load_u32(p + 56);
    e->reserved = dpak_load_u32(p + 60);
}

static inline void dpak_chunk_entry_store(u8 *p, const dpak_chunk_entry *c) {
    dpak_store_u64(p + 0, c->file_offset);
    dpak_store_u32(p + 8, c->stored_size);
    dpak_store_u32(p + 12, c->raw_size);
    dpak_store_u16(p + 16, c->dict_index);
    dpak_store_u16(p + 18, c->flags);
    dpak_store_u32(p + 20, c->reserved);
}

static inline void dpak_chunk_entry_load(const u8 *p, dpak_chunk_entry *c) {
    c->file_offset = dpak_load_u64(p + 0);
    c->stored_size = dpak_load_u32(p + 8);
    c->raw_size = dpak_load_u32(p + 12);
    c->dict_index = dpak_load_u16(p + 16);
    c->flags = dpak_load_u16(p + 18);
    c->reserved = dpak_load_u32(p + 20);
}

static inline void dpak_bundle_entry_store(u8 *p, const dpak_bundle_entry *b) {
    dpak_store_u64(p + 0, b->name_hash);
    dpak_store_u32(p + 8, b->name_offset);
    dpak_store_u32(p + 12, b->name_len);
    dpak_store_u32(p + 16, b->member_first);
    dpak_store_u32(p + 20, b->member_count);
    dpak_store_u32(p + 24, b->depend_first);
    dpak_store_u32(p + 28, b->depend_count);
    dpak_store_u32(p + 32, b->flags);
    dpak_store_u32(p + 36, b->priority);
}

static inline void dpak_bundle_entry_load(const u8 *p, dpak_bundle_entry *b) {
    b->name_hash = dpak_load_u64(p + 0);
    b->name_offset = dpak_load_u32(p + 8);
    b->name_len = dpak_load_u32(p + 12);
    b->member_first = dpak_load_u32(p + 16);
    b->member_count = dpak_load_u32(p + 20);
    b->depend_first = dpak_load_u32(p + 24);
    b->depend_count = dpak_load_u32(p + 28);
    b->flags = dpak_load_u32(p + 32);
    b->priority = dpak_load_u32(p + 36);
}

static inline void dpak_dict_entry_store(u8 *p, const dpak_dict_entry *d) {
    dpak_store_u64(p + 0, d->type_hash);
    dpak_store_u64(p + 8, d->file_offset);
    dpak_store_u32(p + 16, d->size);
    dpak_store_u32(p + 20, d->zstd_id);
}

static inline void dpak_dict_entry_load(const u8 *p, dpak_dict_entry *d) {
    d->type_hash = dpak_load_u64(p + 0);
    d->file_offset = dpak_load_u64(p + 8);
    d->size = dpak_load_u32(p + 16);
    d->zstd_id = dpak_load_u32(p + 20);
}

static inline void dpak_name_entry_store(u8 *p, const dpak_name_entry *n) {
    dpak_store_u32(p + 0, n->offset);
    dpak_store_u32(p + 4, n->len);
}

static inline void dpak_name_entry_load(const u8 *p, dpak_name_entry *n) {
    n->offset = dpak_load_u32(p + 0);
    n->len = dpak_load_u32(p + 4);
}

DE_EXTERN_C_END

#endif /* DE_ASSET_DPAK_FORMAT_H */
