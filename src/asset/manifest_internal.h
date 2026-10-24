/*
 * manifest_internal.h: the parsed asset manifest (assets.toml, SPEC §11.2). Internal to de_asset;
 * include as "asset/manifest_internal.h".
 *
 * de__manifest_parse reads the root manifest and every file it includes (through a reader
 * callback, so tests, the fuzzer and tools can parse from memory) and builds one in-memory
 * de__manifest. Every later S07 piece reads it: ID resolution and bundle closure (DENG-139),
 * slots (DENG-140), loading (DENG-141) and `de-cook --check` (DENG-145).
 *
 * What the parser does NOT do: hash asset IDs, expand bundle globs, close bundle dependencies, or
 * check types and sources against what is registered or on disk (all DENG-139).
 *
 * Memory: everything a manifest points to (the struct, the lists, every string) lives in one
 * chunk arena per manifest, taken from the parse allocator and released by de__manifest_destroy.
 * A manifest is immutable after parsing and may be read from any thread.
 *
 * Order: files are visited root first, then includes depth-first in the order they are listed.
 * Assets, bundles and issues are in that file order, then in key order inside a file. Nothing is
 * iterated out of a hash map.
 */
#ifndef DE_ASSET_MANIFEST_INTERNAL_H
#define DE_ASSET_MANIFEST_INTERNAL_H

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>

DE_EXTERN_C_BEGIN

/** Most files one manifest may consist of (root + includes). More is an error. */
#define DE__MANIFEST_MAX_FILES 1024u
/** Deepest include chain (root = depth 0). Deeper is an error. */
#define DE__MANIFEST_MAX_INCLUDE_DEPTH 32u

/** An issue found while parsing; the same type tools see through de_asset_manifest_issues. */
typedef de_asset_manifest_issue de__manifest_issue;

/** The type of a settings value. TOML types are kept, so S08 can hash settings exactly. */
typedef de_asset_setting_type de__manifest_value_type;
typedef de_asset_setting_value de__manifest_value;
#define DE__MANIFEST_BOOL         DE_ASSET_SETTING_BOOL
#define DE__MANIFEST_INT          DE_ASSET_SETTING_INT
#define DE__MANIFEST_FLOAT        DE_ASSET_SETTING_FLOAT
#define DE__MANIFEST_STRING       DE_ASSET_SETTING_STRING
#define DE__MANIFEST_STRING_ARRAY DE_ASSET_SETTING_STRING_ARRAY
#define DE__MANIFEST_ARRAY        DE_ASSET_SETTING_ARRAY
#define DE__MANIFEST_TABLE        DE_ASSET_SETTING_TABLE

/** One importer setting. */
typedef struct de__manifest_kv {
    const char *key;
    de__manifest_value value;
} de__manifest_kv;

/** `[defaults.<type>]`: settings every asset of `type` starts from. Keys sorted by byte order. */
typedef struct de__manifest_defaults {
    const char *type;
    const de__manifest_kv *settings;
    u32 setting_count;
} de__manifest_defaults;

/** One entry of `[assets."<id>".locale]`: `locale = "source"`. */
typedef struct de__manifest_locale {
    const char *locale;
    const char *source;
} de__manifest_locale;

typedef struct de__manifest_asset {
    const char *id;     /**< the asset ID string, exactly as written (not hashed here) */
    const char *type;   /**< `type`: never empty in a manifest without errors */
    const char *source; /**< `source`, relative to the project's asset_root */
    const char *file;   /**< normalised vpath of the manifest file that declared it */
    u32 line;           /**< line of the declaration (0 if unknown) */
    /** Effective settings: `defaults.<type>` overlaid by the asset's own keys (the asset wins).
     * Every key except `type`, `source`, `locale` and `variants`. Sorted by key (byte order), so
     * the list does not depend on where in the file a key was written. */
    const de__manifest_kv *settings;
    u32 setting_count;
    const de__manifest_locale *locales; /**< in file order */
    u32 locale_count;
} de__manifest_asset;

/** Bundle load priority (`priority = "..."`); maps onto the DE_LOAD_PRIORITY_* flags. */
typedef enum de__manifest_priority {
    DE__MANIFEST_PRIORITY_CRITICAL,
    DE__MANIFEST_PRIORITY_HIGH,
    DE__MANIFEST_PRIORITY_NORMAL, /**< the default */
    DE__MANIFEST_PRIORITY_LOW,
    DE__MANIFEST_PRIORITY_BACKGROUND
} de__manifest_priority;

typedef struct de__manifest_bundle {
    const char *name;
    const char *file; /**< normalised vpath of the manifest file that declared it */
    u32 line;
    const char *const *assets; /**< asset IDs or globs on IDs, unresolved (DENG-139) */
    u32 asset_count;
    const char *const *depends; /**< bundle names, unresolved */
    u32 depends_count;
    bool resident;
    de__manifest_priority priority;
} de__manifest_bundle;

typedef struct de__manifest {
    const char *root;       /**< normalised vpath of the root manifest */
    const char *asset_root; /**< `[project] asset_root`; "" when not set */
    i64 version;            /**< `[project] version`; 0 when not set */
    bool has_version;
    const char *const *files; /**< every file read, root first, in visit order */
    u32 file_count;
    const de__manifest_defaults *defaults; /**< one per type, in order of first appearance */
    u32 defaults_count;
    const de__manifest_asset *assets;
    u32 asset_count;
    const de__manifest_bundle *bundles;
    u32 bundle_count;
    const de__manifest_issue *issues; /**< file order, then key order */
    u32 issue_count;
    u32 error_count, warning_count, info_count;
    void *arena_; /* private: the chunk arena that owns all of the above */
} de__manifest;

/**
 * Reads one manifest file. On DE_OK, `*data` is a block of `*size + 1` bytes from `a` (the last
 * byte may be anything; the parser does not need a NUL), which the parser frees with
 * `DE_FREE(a, *data, *size + 1)`. This is exactly the de_file_read_all contract, so the default
 * reader is de_file_read_all. Any other result is reported as an issue on the including key
 * (or the root file). Called from the parsing thread only.
 */
typedef de_result (*de__manifest_read_fn)(void *user, const char *vpath, const de_allocator *a,
                                          void **data, u64 *size);

typedef struct de__manifest_parse_desc {
    /** Root manifest, a virtual path (`dev:/assets.toml`). Required. Includes resolve relative to
     * the directory of the including file and must stay inside its root. */
    const char *path;
    /** For the manifest arena, temporaries and tomlc17 (NULL = de_heap_allocator()). */
    const de_allocator *allocator;
    de__manifest_read_fn read; /**< NULL = de_file_read_all */
    void *read_user;
} de__manifest_parse_desc;

/**
 * Parses the manifest at `d->path` and its includes. Errors in the files do not stop the parse:
 * each one becomes an issue, and the parse goes on with the next key (the rule is "report
 * everything, never stop at the first"). So DE_OK means "`*out` holds a manifest"; check
 * `(*out)->error_count` to know whether it is usable. A root file that cannot be read is an
 * error issue with that read's result as its `code`.
 *
 * @return DE_OK (`*out` set), DE_ERR_INVALID_ARGUMENT (no desc, no out, or `path` is not a
 *         valid virtual path), DE_ERR_OUT_OF_MEMORY (nothing is leaked; `*out` is NULL).
 */
de_result de__manifest_parse(const de__manifest_parse_desc *d, de__manifest **out);

/** Releases a manifest and everything it points to. NULL is ignored. */
void de__manifest_destroy(de__manifest *m);

/** The asset with this ID string, or NULL. Linear; for tests and tools (DENG-139 hashes IDs). */
const de__manifest_asset *de__manifest_find_asset(const de__manifest *m, const char *id);
/** The bundle with this name, or NULL. Linear. */
const de__manifest_bundle *de__manifest_find_bundle(const de__manifest *m, const char *name);
/** The defaults of `type`, or NULL. Linear. */
const de__manifest_defaults *de__manifest_find_defaults(const de__manifest *m, const char *type);
/** The setting `key` in a sorted settings list, or NULL. */
const de__manifest_kv *de__manifest_find_setting(const de__manifest_kv *settings, u32 count,
                                                 const char *key);

/**
 * The source of `asset` for `locale`: the `[assets."<id>".locale]` entry that matches the locale
 * exactly (ASCII case-insensitive, `_` = `-`), else the closest parent tag (`pt-BR` falls back to
 * `pt`;
 * `-` and `_` both separate subtags), else the asset's own `source`. A NULL or empty `locale`
 * gives `source`. `m` is unused today and kept for a later manifest-wide fallback chain.
 */
const char *de__manifest_source_for(const de__manifest *m, const de__manifest_asset *asset,
                                    const char *locale);

/** "critical", "high", ... */
const char *de__manifest_priority_name(de__manifest_priority p);

DE_EXTERN_C_END

#endif /* DE_ASSET_MANIFEST_INTERNAL_H */
