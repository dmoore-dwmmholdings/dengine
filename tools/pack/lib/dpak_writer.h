/*
 * dpak_writer.h: writes .dpak pack archives (SPEC §11.7). Tools only (target de_pack_lib);
 * the runtime never links it. Format: src/asset/dpak/dpak_format.h, docs/formats/dpak.md.
 *
 *     dpak_writer *w;
 *     DE_TRY(dpak_writer_create(&(dpak_writer_desc){.platform = DPAK_PLATFORM_MACOS}, &w));
 *     dpak_writer_add(w, de_hash64_str("player/mesh"), "mesh", data, size, deps, 2,
 *                     "player/mesh");
 *     const u64 members[] = {de_hash64_str("player/mesh")};
 *     dpak_writer_add_bundle(w, "level_01", members, 1, NULL, 0, false, DPAK_PRIORITY_HIGH);
 *     de_result r = dpak_writer_finish(w, "out:/game.dpak");
 *     dpak_writer_destroy(w);
 *
 * Determinism: the bytes written depend only on the set of entries and bundles and on the desc,
 * never on the order of the add calls, the host, the time or the thread count. Entries are
 * sorted by ID, each entry's dependencies and each bundle's members and depends are sorted and
 * deduplicated, zstd runs single-threaded at a fixed level with fixed frame parameters, and
 * every padding byte is zero.
 *
 * The writer copies each entry's data on add and keeps everything in memory until finish, so a
 * pack needs about its raw size plus its stored size in memory. Not thread-safe.
 */
#ifndef DE_TOOLS_PACK_DPAK_WRITER_H
#define DE_TOOLS_PACK_DPAK_WRITER_H

#include <dengine/de_base.h>
#include <dengine/de_memory.h>

DE_EXTERN_C_BEGIN

/** zstd level used when dpak_writer_desc.zstd_level is 0. */
#define DPAK_WRITER_DEFAULT_ZSTD_LEVEL 19

typedef struct dpak_writer dpak_writer;

/** Settings of one pack. `(dpak_writer_desc){0}` is valid. */
typedef struct dpak_writer_desc {
    /** For the writer's own memory and the copies of the data; NULL = de_heap_allocator(). */
    const de_allocator *allocator;
    /** dpak_platform value stored in the header. */
    u32 platform;
    /** Opaque build identifier stored in the header (all zero is allowed). */
    u8 build_id[16];
    /** Raw bytes per chunk, in [DPAK_MIN_CHUNK_SIZE, DPAK_MAX_CHUNK_SIZE]; 0 = 65536. */
    u32 chunk_size;
    /** zstd level, ZSTD_minCLevel() to ZSTD_maxCLevel(); 0 = DPAK_WRITER_DEFAULT_ZSTD_LEVEL. */
    i32 zstd_level;
    /**
     * Train one zstd dictionary per asset type that has at least DPAK_DICT_MIN_SAMPLES chunks,
     * from that type's chunks. A dictionary is kept only when it makes the type's chunks smaller
     * by more than its own size, and each chunk uses it only when that makes the chunk smaller.
     */
    bool use_dictionaries;
    /** Store every entry's name (asset path) in the NAMES section, for `de-pack list`. Off for
     * stripped shipping packs. Bundle names are always stored. */
    bool include_names;
    /** Each asset's data starts on a multiple of this: a power of two in
     * [DPAK_MIN_ALIGNMENT, DPAK_MAX_ALIGNMENT]; 0 = DPAK_DEFAULT_ALIGNMENT (4096). */
    u32 alignment;
    /** Format version to write, in [DPAK_VERSION_MIN, DPAK_VERSION]; 0 = DPAK_VERSION. Version 2
     * stores a hash of every chunk and dictionary. Version 1 (no hashes) exists so tests and the
     * fuzz corpus can check that readers still load old packs; it writes the bytes the version 1
     * writer wrote. */
    u32 format_version;
} dpak_writer_desc;

/**
 * Creates an empty writer. `desc` may be NULL (same as a zeroed desc).
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (NULL `out`, chunk size, alignment, level or platform
 *         out of range); DE_ERR_OUT_OF_MEMORY.
 */
de_result dpak_writer_create(const dpak_writer_desc *desc, dpak_writer **out);

/** Frees the writer and every copy it holds. NULL is ignored. */
void dpak_writer_destroy(dpak_writer *w);

/**
 * Adds one asset. The data, the dependencies and the strings are copied.
 *
 * @param id         asset ID: de_hash64 of the asset path, seed 0 (S07 de_asset_id_from).
 *                   IDs must be unique in the pack; finish fails on a duplicate.
 * @param type_name  asset type ("texture"); not NULL or empty. Stored as de_hash64_str().
 * @param data       `size` bytes; may be NULL when `size` is 0.
 * @param deps       IDs of assets this one depends on (they may live in another pack); NULL
 *                   when `dep_count` is 0. Sorted and deduplicated; `id` itself is an error.
 * @param name       the asset path, or NULL. Stored only with include_names. A `#` in it marks
 *                   the entry DPAK_ENTRY_SUB_ASSET; it also names the asset in error messages.
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (a bad argument, or the pack would exceed a format
 *         limit); DE_ERR_OUT_OF_MEMORY. On failure the writer is unchanged.
 */
de_result dpak_writer_add(dpak_writer *w, u64 id, const char *type_name, const void *data, u64 size,
                          const u64 *deps, u32 dep_count, const char *name);

/**
 * Adds a bundle (assets.toml `[bundles.<name>]`), so a packed game needs no manifest.
 *
 * @param name          bundle name; not NULL or empty. Unique in the pack (finish fails on a
 *                      duplicate). Stored as text and as de_hash64_str().
 * @param ids           member asset IDs (NULL when `count` is 0); sorted and deduplicated.
 *                      Members are not checked against the pack's entries: a patch pack's
 *                      bundle may name assets of another pack.
 * @param depends       names of the bundles this one depends on (NULL when `depend_count` is
 *                      0); stored as name hashes, sorted and deduplicated. `name` itself is an
 *                      error.
 * @param resident      never evicted.
 * @param priority      a dpak_priority value.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT, DE_ERR_OUT_OF_MEMORY. On failure the writer is
 *         unchanged.
 */
de_result dpak_writer_add_bundle(dpak_writer *w, const char *name, const u64 *ids, u32 count,
                                 const char *const *depends, u32 depend_count, bool resident,
                                 u32 priority);

/**
 * Builds the pack in memory. The block comes from `a` (NULL = de_heap_allocator()); free it with
 * `DE_FREE(a, *out_data, *out_size)`. The writer keeps its entries, so finishing twice gives
 * the same bytes, and more entries may be added afterwards.
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (NULL outputs); DE_ERR_ALREADY_EXISTS (two entries with
 *         one ID, or two bundles with one name hash); DE_ERR_INTERNAL (zstd failed);
 *         DE_ERR_OUT_OF_MEMORY.
 */
de_result dpak_writer_finish_memory(dpak_writer *w, const de_allocator *a, void **out_data,
                                    u64 *out_size);

/**
 * Builds the pack and writes it to the virtual path `vpath` (a writable mount, see de_fs_mount)
 * with de_file_write_all_atomic(), so a reader never sees a partly written pack.
 * @return the errors of dpak_writer_finish_memory() and of de_file_write_all_atomic().
 */
de_result dpak_writer_finish(dpak_writer *w, const char *vpath);

DE_EXTERN_C_END

#endif /* DE_TOOLS_PACK_DPAK_WRITER_H */
