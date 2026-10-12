/*
 * decache.h: the cook cache `.decache` (SPEC §11.5, DENG-210). Tools-internal, like de_cooker.h:
 * include it as "cook/lib/decache.h".
 *
 * A cooker made with `de_cooker_desc.cache_dir` (a virtual directory: `cache:/decache` for the
 * runtime, `.decache/` next to the manifest for `de-cook`) looks every cook up in the cache
 * before it reads the source, and stores what an import produced after it. A hit returns the same
 * de_cook_result an import would have, without running the importer.
 *
 * Key (content-addressed): XXH3-128 (seed 0) over a little-endian encoding of
 *
 *   "DECK" u32 cache format version, u32 zstd version (ZSTD_VERSION_NUMBER)
 *   u32 target platform, u32 importer version, u64 settings hash (de_cook_settings_hash)
 *   str type name, str asset ID, str source path (relative to the asset root)
 *   16 bytes: XXH3-128 of the source bytes
 *   u32 extra source count, then per extra source in the order the importer read them:
 *     str path (relative to the asset root), u8 present (0 = the file is missing),
 *     16 bytes: XXH3-128 of its bytes (zero when missing)
 *
 * where `str` is u32 length + bytes. The extra sources are known only after an import, so a
 * lookup first reads the entry of the *base key* (the same encoding with tag "DEBK" and no extra
 * sources). For an asset whose importer read no extra file that is the entry itself. Otherwise
 * it is a redirect that lists the extra source paths; the lookup hashes those files and reads the
 * entry of the full key. A deterministic importer reads the same files for the same source,
 * settings and earlier file contents, so the redirect never goes stale.
 *
 * Files: `<cache_dir>/<first 2 hex of the key>/<32 hex>.dc` and `<cache_dir>/index.bin`. The
 * entry format is in decache.c. Entries hold no time, process ID or host path: identical inputs
 * give byte-identical entries on every host. Writes are atomic (a temporary file in the same
 * directory, renamed over the target), so a reader never sees a partial entry and two processes
 * may share one cache directory. A damaged entry is a miss: it is deleted and logged at Warn.
 *
 * Warm path: `index.bin` maps a virtual path to (size, mtime, XXH3-128 of the bytes), so a lookup
 * whose files have the recorded size and mtime reads no source byte. The index is a hint only:
 * every key is computed from content hashes, and a file whose mtime is less than the racy window
 * (2 s) older than the moment it was hashed is not recorded (it could change again within one
 * file-system timestamp tick). It is loaded once per cooker and written atomically every
 * DE__DECACHE_INDEX_WRITE_EVERY stores and when the cooker is destroyed.
 *
 * Extra sources are hashed when the cook is stored, after the importer read them. If one of them
 * changed less than the racy window before the cook began, it may have changed while the importer
 * ran, so the store is left out (`store_deferred`); the next cook of the asset stores it.
 *
 * Not handled: a size cap or garbage collection of old entries, and a shared or remote cache
 * (DENG-2 Q20).
 */
#ifndef DE_DECACHE_H
#define DE_DECACHE_H

#include "cook/lib/de_cooker.h"

#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "core/de_hashmap.h"

DE_EXTERN_C_BEGIN

/* ---- Statistics (tools API) ------------------------------------------------------------------ */

/** What the cook cache of one cooker did since it was created. */
typedef struct de_cooker_cache_stats {
    u64 hits;           /**< cooks answered from the cache (no import ran) */
    u64 misses;         /**< lookups that found no valid entry */
    u64 stores;         /**< entries (and redirects) written */
    u64 store_skipped;  /**< stores that found the entry already there (another cook wrote it) */
    u64 store_failures; /**< stores that could not write (logged at Warn) */
    /** stores left out because an extra source changed less than the racy window before the
     * cook began (it may have changed while the importer ran); the next cook stores it */
    u64 store_deferred;
    u64 corrupt;           /**< damaged entries found, deleted and treated as misses */
    u64 bytes_read;        /**< bytes of cache files read (entries, redirects) */
    u64 bytes_written;     /**< bytes of cache files written */
    u64 source_bytes_read; /**< bytes of source and extra source files read to hash them */
    u64 files_hashed;      /**< source and extra source files hashed (index misses) */
    u64 index_writes;      /**< times `index.bin` was written */
    u64 lookup_ns;         /**< time spent in lookups, hits and misses */
    u64 store_ns;          /**< time spent storing */
} de_cooker_cache_stats;

/** Writes the cache statistics of `c` into `*out`; all zero when `c` has no cache. Any thread. */
void de_cooker_get_cache_stats(const de_cooker *c, de_cooker_cache_stats *out);

/**
 * Writes `index.bin` now if it changed since it was last written (the cooker also does this
 * every DE__DECACHE_INDEX_WRITE_EVERY stores and in de_cooker_destroy). Any thread.
 * @return DE_OK (also without a cache, or with nothing to write), DE_ERR_IO, DE_ERR_OUT_OF_MEMORY.
 */
de_result de_cooker_flush_cache(de_cooker *c);

/* ---- Internal (cooker.c, decache*.c, tests) -------------------------------------------------- */

/** Version of the key encoding and the entry format; bump it with any change to either. */
#define DE__DECACHE_FORMAT 1u
/** `index.bin` is written after this many stores (and on destroy). */
#define DE__DECACHE_INDEX_WRITE_EVERY 64u
/** A file whose mtime is less than this much older than its hashing is not put in the index. */
#define DE__DECACHE_RACY_NS ((i64)2000000000)

typedef struct de__decache de__decache;

/** Creates the cache of `c` for `c->cache_dir` (nothing touches the disk until the first use).
 * DE_OK or DE_ERR_OUT_OF_MEMORY. */
de_result de__decache_create(de_cooker *c);
/** Writes the index if it changed and frees the cache of `c`. */
void de__decache_destroy(de_cooker *c);

/** One cook's view of the cache: what the lookup learnt, for the store that follows a miss. */
typedef struct de__decache_probe {
    bool active; /**< the cooker has a cache and the lookup ran */
    u8 base_key[16];
    /** The source bytes, when the lookup had to read them to hash them (de_file_read_all layout:
     * `source_size + 1` bytes from the cooker's allocator). The cooker takes them over. */
    void *source;
    u64 source_size;
    /* Inputs, borrowed from de_cooker_cook for the store. */
    const char *root; /**< normalised asset root */
    const char *rel;  /**< source path relative to the root */
    const char *id;
    const char *type;
    u32 importer_version;
    u64 settings_hash;
    i64 start_utc_ns; /**< when the lookup ran (de__decache_now_utc_ns) */
} de__decache_probe;

/**
 * Hook 1 of de_cooker_cook: looks the asset up before its source is read. On a hit `*out` holds
 * the cooked result (as de_cooker_cook would return it) and DE_OK is returned. Otherwise
 * DE_ERR_NOT_FOUND, `*out` stays zeroed, and `*p` keeps what the store needs (and maybe the source
 * bytes). Without a cache it returns DE_ERR_NOT_FOUND at once. Never sets the error context.
 */
de_result de__decache_lookup(de_cooker *c, de__decache_probe *p, const char *root, const char *rel,
                             const char *id, const char *type, u32 importer_version,
                             u64 settings_hash, de_cook_result *out);

/**
 * Hook 2 of de_cooker_cook: stores a successful cook. `source` is the bytes that were imported.
 * Failures are logged at Warn and counted, never returned: the cook itself succeeded.
 */
void de__decache_store(de_cooker *c, const de__decache_probe *p, const void *source,
                       u64 source_size, const de_cook_result *r);

/** Overrides DE__DECACHE_RACY_NS for the cache of `c` (tests). */
void de__decache_set_racy_window(de_cooker *c, i64 ns);

/** Size and mtime (nanoseconds since 1970, UTC) of the file `vpath`, through the mount table
 * (the mount de_file_open would read). DE_OK, DE_ERR_NOT_FOUND, DE_ERR_INVALID_ARGUMENT. */
de_result de__decache_file_stat(const char *vpath, u64 *size, i64 *mtime_ns);

/** The path of the entry for `key` (`<dir>/<2 hex>/<32 hex>.dc`) in `buf`. False if too long. */
bool de__decache_entry_path(const de_cooker *c, const u8 key[16], char *buf, size_t cap);

/* ---- The cache object (decache.c) and its index (decache_index.c) ---------------------------- */

/** One `index.bin` record: what a file's bytes hashed to when it had this size and mtime. */
typedef struct de__decache_file {
    char *path; /**< normalised virtual path; NULL = removed */
    size_t path_size;
    u64 size;
    i64 mtime_ns;
    u8 hash[16];
} de__decache_file;

struct de__decache {
    const de_allocator *alloc;
    char dir[DE_FS_PATH_MAX]; /**< normalised cache directory */
    bool dir_ok;              /**< `dir` normalised */
    de_mutex lock;            /**< guards everything below */
    bool opened;              /**< the directory was created and the index loaded */
    bool disabled;            /**< the directory cannot be created: every lookup misses */
    de_log_category log;
    i64 racy_ns;
    u64 shards_made[4]; /**< bit per `<2 hex>` directory created by this cooker */

    /* The index: path hash -> record. */
    de_hashmap map; /**< u64 de_hash64 of the path -> u32 index into `files` */
    de__decache_file *files;
    u32 file_count, file_cap;
    bool index_dirty;
    u32 stores_since_write;

    de_cooker_cache_stats stats;
};

/** Loads `index.bin` (missing: empty; damaged: empty, logged at Warn). Under `lock`. */
void de__decache_index_load(de__decache *d);
/** Frees the index records. */
void de__decache_index_free(de__decache *d);
/** Writes `index.bin` if it changed (takes `lock` itself). */
de_result de__decache_index_write(de__decache *d);
/**
 * The XXH3-128 of the bytes of file `vpath`. `*present` false (and a zero hash) when the file is
 * missing. Uses the index when the size and mtime match; otherwise reads and hashes the file
 * and records it. With `keep` the bytes read are handed to the caller (`*keep` NULL when the
 * index answered); free them with DE_FREE(d->alloc, *keep, *keep_size + 1).
 * @return DE_OK, or the read error (DE_ERR_IO, DE_ERR_OUT_OF_MEMORY).
 */
de_result de__decache_file_hash(de__decache *d, const char *vpath, u8 hash[16], bool *present,
                                void **keep, u64 *keep_size);
/** Wall-clock now in the mtime timebase (ns since 1970 UTC); 0 without a clock. */
i64 de__decache_now_utc_ns(void);
/** XXH3-128 of `n` bytes, canonical (big-endian) byte order. */
void de__decache_hash128(const void *data, size_t n, u8 out[16]);

DE_EXTERN_C_END

#endif /* DE_DECACHE_H */
