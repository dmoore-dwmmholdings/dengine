/*
 * dpak_reader.h: the runtime reader of .dpak pack archives (SPEC §11.7; DENG-212). Internal to
 * de_asset; include as "asset/dpak/dpak_reader.h". Format: dpak_format.h, docs/formats/dpak.md.
 *
 *     dpak_reader *r;
 *     DE_TRY(dpak_open("game:/base.dpak", &(dpak_open_desc){.platform = dpak_platform_host()},
 *                      &r));
 *     u32 i = dpak_find(r, de_asset_id_from("player/mesh").hash);
 *     ...
 *     dpak_close(r);
 *
 * A pack is untrusted input. dpak_open and dpak_open_memory check every offset, count, size and
 * range of the file before they return (the list is in docs/formats/dpak.md, "Reader checks"),
 * so the accessors below need no checks of their own and never read outside the file. Only the
 * content hash is optional (`verify`): it reads the whole file.
 *
 * Format versions 1 and 2 are read. A version 2 pack has a hash of every chunk's stored bytes and
 * of every dictionary (DENG-392): dpak_check_chunk / dpak_check_dict check one, and
 * `check_chunks` makes dpak_decode_chunk check every chunk before it decodes it. Version 1 packs
 * have no hashes; for them the checks always pass.
 *
 * Memory: a file reader reads the metadata block (header, sections, dictionaries) once at open
 * and keeps the file open for the reads of asset data (de_file reads take an offset, so several
 * threads may read at once). A memory reader keeps a pointer to the caller's bytes, which must
 * outlive it. Each dictionary becomes one ZSTD_DDict at open. Nothing allocates per chunk.
 *
 * Threads: open and close on one thread; every other function is read-only and safe on any
 * thread at once.
 */
#ifndef DE_ASSET_DPAK_READER_H
#define DE_ASSET_DPAK_READER_H

#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "asset/dpak/dpak_format.h"

DE_EXTERN_C_BEGIN

typedef struct dpak_reader dpak_reader;

/** dpak_find / dpak_bundle_find: no such entry. */
#define DPAK_NOT_FOUND 0xFFFFFFFFu

/** Settings of dpak_open. `(dpak_open_desc){0}` is valid. */
typedef struct dpak_open_desc {
    /** The reader's memory (metadata copy, dictionaries); NULL = de_heap_allocator(). */
    const de_allocator *allocator;
    /** The platform this process runs on (dpak_platform_host()). A pack cooked for another
     * platform fails with DE_ERR_UNSUPPORTED. DPAK_PLATFORM_ANY (0) accepts every pack; a pack
     * marked DPAK_PLATFORM_ANY is accepted everywhere. */
    u32 platform;
    /** Check the header's content_hash. Reads every byte of the file once. */
    bool verify;
    /** Version 2 packs: check each dictionary's hash at open, and each chunk's hash in
     * dpak_decode_chunk and dpak_read_entry before it is decoded, so a damaged chunk fails with
     * DE_ERR_PARSE instead of decoding to wrong bytes. Costs one XXH3 pass over the stored bytes
     * (much faster than zstd). The mount stack sets it in Debug and Dev builds. No effect on a
     * version 1 pack. */
    bool check_chunks;
} dpak_open_desc;

/** The dpak_platform value of this build's target (Windows, Linux or macOS). */
u32 dpak_platform_host(void);

/**
 * Opens the pack at the virtual path `vpath`, checks it and reads its metadata.
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (NULL argument); DE_ERR_NOT_FOUND / DE_ERR_IO (the file
 *         cannot be opened or read); DE_ERR_VERSION_MISMATCH (another format version or header
 *         size); DE_ERR_UNSUPPORTED (cooked for another platform); DE_ERR_PARSE (not a pack, a
 *         truncated pack, or any value out of range, a content hash mismatch included);
 *         DE_ERR_OUT_OF_MEMORY. Failures set the error context, naming the path.
 */
de_result dpak_open(const char *vpath, const dpak_open_desc *desc, dpak_reader **out);

/**
 * Opens a pack held in memory (tests, the DENG-216 fuzz target, tools). `data` must stay valid
 * and unchanged until dpak_close. Same checks and results as dpak_open.
 */
de_result dpak_open_memory(const void *data, u64 size, const dpak_open_desc *desc,
                           dpak_reader **out);

/** Frees the reader and closes its file. NULL is ignored. */
void dpak_close(dpak_reader *r);

/** The decoded header. */
const dpak_header *dpak_get_header(const dpak_reader *r);
/** The pack file (for de_io reads of asset data), or NULL for a memory reader. */
de_file *dpak_file(const dpak_reader *r);
/** The pack bytes of a memory reader, or NULL for a file reader. */
const u8 *dpak_memory(const dpak_reader *r);

/* ---- Entries ------------------------------------------------------------------------------ */

/** Number of TOC entries. */
u32 dpak_entry_count(const dpak_reader *r);
/** Decodes TOC entry `index` (< dpak_entry_count). */
void dpak_entry(const dpak_reader *r, u32 index, dpak_toc_entry *out);
/** The TOC index of asset `id` (binary search), or DPAK_NOT_FOUND. */
u32 dpak_find(const dpak_reader *r, u64 id);
/** Decodes chunk `index` of the CHUNKS section (an entry's chunks are first_chunk + k). In a
 * version 1 pack `out->hash` is 0. */
void dpak_chunk(const dpak_reader *r, u32 index, dpak_chunk_entry *out);
/** Number of dictionaries (DICTS entries). */
u32 dpak_dict_count(const dpak_reader *r);
/** Decodes dictionary entry `index` (< dpak_dict_count). In a version 1 pack `out->hash` is 0. */
void dpak_dict(const dpak_reader *r, u32 index, dpak_dict_entry *out);
/** Dependency `index` of the DEPS section (an entry's are dep_first + k): an asset ID. */
u64 dpak_dep(const dpak_reader *r, u32 index);
/**
 * The asset path of TOC entry `index`, NUL-terminated, with its length in `*len` (optional), or
 * NULL when the pack has no names (stripped) or the writer had none for that entry.
 */
const char *dpak_name(const dpak_reader *r, u32 index, u32 *len);

/* ---- Bundles ------------------------------------------------------------------------------- */

/** Number of bundles. */
u32 dpak_bundle_count(const dpak_reader *r);
/** Decodes bundle `index` (< dpak_bundle_count). */
void dpak_bundle(const dpak_reader *r, u32 index, dpak_bundle_entry *out);
/** The index of the bundle whose name hashes to `name_hash`, or DPAK_NOT_FOUND. */
u32 dpak_bundle_find(const dpak_reader *r, u64 name_hash);
/** The bundle's name, NUL-terminated (`b` from dpak_bundle). */
const char *dpak_bundle_name(const dpak_reader *r, const dpak_bundle_entry *b);
/** Entry `index` of the BUNDLE_IDS section: a member asset ID or a depend name hash. */
u64 dpak_bundle_id(const dpak_reader *r, u32 index);

/* ---- Hashes (format version 2) ------------------------------------------------------------- */

/** True if the pack stores chunk and dictionary hashes (format version 2 and later). */
bool dpak_has_hashes(const dpak_reader *r);

/** True if the reader checks chunk hashes before decoding (`check_chunks` on a version 2 pack).
 * A caller that uses stored bytes without dpak_decode_chunk (the raw read path of the load
 * pipeline) calls dpak_check_chunk itself when this is true. */
bool dpak_checks_chunks(const dpak_reader *r);

/**
 * Checks one chunk's stored bytes against its hash: `src` holds its chunk->stored_size bytes
 * (from the file or from memory). Any thread.
 * @return DE_OK (the hash matches, or the pack has no hashes); DE_ERR_PARSE (damaged: the
 *         message names the pack, the chunk's file offset and both hashes).
 */
de_result dpak_check_chunk(const dpak_reader *r, const dpak_chunk_entry *chunk, const void *src);

/**
 * Checks dictionary `index` against its hash (the bytes are in the reader's metadata). Any thread.
 * @return DE_OK (the hash matches, the pack has no hashes, or `index` is out of range);
 *         DE_ERR_PARSE (damaged).
 */
de_result dpak_check_dict(const dpak_reader *r, u32 index);

/* ---- Data ---------------------------------------------------------------------------------- */

/**
 * Decodes one chunk: `src` holds its chunk->stored_size stored bytes, `dst` gets its
 * chunk->raw_size bytes. A RAW chunk is copied; a zstd chunk is decompressed with `dctx` (a
 * ZSTD_DCtx *), through its dictionary's ZSTD_DDict when it names one. With `check_chunks` the
 * chunk's hash is checked first (dpak_check_chunk). Any thread.
 * @return DE_OK; DE_ERR_PARSE (a corrupt frame, one whose size differs from raw_size, or a hash
 *         mismatch). Sets the error context on failure.
 */
de_result dpak_decode_chunk(const dpak_reader *r, void *dctx, const dpak_chunk_entry *chunk,
                            const void *src, void *dst);

/**
 * Reads and decodes the whole of entry `index` into `dst` (`dst_size` >= its raw_size), one chunk
 * at a time, synchronously: from the file (de_file_read) or from memory. `dctx` is a ZSTD_DCtx *,
 * or NULL to use a temporary one. For tools, tests and the fuzz target; the load pipeline reads
 * and decodes chunks itself. Allocates one chunk-sized buffer for a file reader.
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (index or size); DE_ERR_IO; DE_ERR_PARSE;
 *         DE_ERR_OUT_OF_MEMORY.
 */
de_result dpak_read_entry(const dpak_reader *r, u32 index, void *dctx, void *dst, u64 dst_size);

DE_EXTERN_C_END

#endif /* DE_ASSET_DPAK_READER_H */
