/*
 * resolve_internal.h: asset IDs, globs on IDs, and the resolved manifest (SPEC §11.1, §11.2;
 * DENG-139). Internal to de_asset; include as "asset/resolve_internal.h".
 *
 * A parsed de__manifest (DENG-138) holds strings. de__manifest_resolve turns it into the data
 * the runtime needs:
 *   - the ID (de_asset_id) of every asset, and an ID -> asset table sorted by ID, which is also
 *     the reverse lookup that prints IDs as paths (two different paths with one ID are an error);
 *   - the members of every bundle: its explicit IDs and the IDs its globs match, sorted by ID,
 *     without duplicates;
 *   - the transitive `depends` closure of every bundle, in topological order (dependencies
 *     first; among bundles not ordered by a dependency, the one declared first comes first).
 * de__manifest_validate then checks what depends on the environment: asset types against a set
 * of known type names, and source files on disk.
 *
 * Issues: the resolved view has its own issue list. It starts with a copy of the manifest's
 * issues; resolution and validation append theirs, so tools print one list.
 *
 * Order: nothing here iterates a hash map. Assets and bundles are visited in manifest order,
 * members and the ID table are sorted by ID, issues are in the order they were found.
 *
 * Memory: a de__resolved lives in its own chunk arena on the resolve allocator and points into
 * the manifest it was built from (strings, bundles). Destroy it before that manifest.
 */
#ifndef DE_ASSET_RESOLVE_INTERNAL_H
#define DE_ASSET_RESOLVE_INTERNAL_H

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>

#include "asset/manifest_internal.h"

DE_EXTERN_C_BEGIN

/* ---- Asset IDs (asset_id.c) -------------------------------------------------------------- */

/** The ID of a hash: `h`, except that 0 (the invalid ID) is remapped to 1. */
static inline de_asset_id de__asset_id_make(u64 h) {
    de_asset_id id = {h != 0 ? h : 1u};
    return id;
}

/**
 * Checks the asset ID rules (see de_asset_id_from in de_asset.h) on `len` bytes of `path`.
 * Returns NULL when the path is a valid ID, else a short English reason ("empty segment") for
 * error messages. Allocation-free.
 */
const char *de__asset_id_check(const char *path, size_t len);

/* ---- Globs on asset IDs (glob.c) --------------------------------------------------------- */

/** True if `s` contains a glob wildcard (`*` or `?`), i.e. a bundle entry is a glob, not an ID. */
bool de__glob_is_pattern(const char *s);

/**
 * True if the asset ID `id` matches `glob`:
 *   - `*` matches any run of characters (also none) except `/`;
 *   - `**` (two or more stars in a row) matches any run of characters, `/` included;
 *   - `?` matches one character (one UTF-8 code point) except `/`;
 *   - every other byte matches itself (case-sensitive).
 * The whole ID must match. A sub-asset ID (one containing `#`) never matches: sub-assets come
 * from importers and are never declared. Allocation-free; time O(len(glob) * len(id)).
 */
bool de__glob_match(const char *glob, const char *id);

/* ---- Resolution (resolve.c) -------------------------------------------------------------- */

/** One row of the ID table: an ID and the index of its asset in `manifest->assets`. */
typedef struct de__resolved_entry {
    de_asset_id id;
    u32 asset;
} de__resolved_entry;

typedef struct de__resolved_bundle {
    const de__manifest_bundle *bundle; /**< the manifest bundle (name, file, priority, ...) */
    /** Explicit IDs and glob matches, sorted by `hash`, no duplicates. Only declared assets with
     * valid IDs; entries with errors are left out. */
    const de_asset_id *members;
    u32 member_count;
    /** The transitive `depends` closure, without the bundle itself: indices into
     * de__resolved.bundles, dependencies before the bundles that need them; ties go to the
     * bundle declared first. Unknown names and the edge that closes a cycle are left out. */
    const u32 *depends;
    u32 depends_count;
} de__resolved_bundle;

typedef struct de__resolved {
    const de__manifest *manifest;
    /** One per manifest asset (same index): its ID, or {0} when the ID string is invalid. */
    const de_asset_id *asset_ids;
    /** Every valid asset ID, sorted by `id.hash`. Two different paths with one ID (an error)
     * both appear, next to each other. */
    const de__resolved_entry *ids;
    u32 id_count;
    const de__resolved_bundle *bundles; /**< one per manifest bundle, same index and order */
    u32 bundle_count;
    /** The manifest's issues, then resolution's, then validation's. Strings live in the manifest
     * or in this object. */
    const de__manifest_issue *issues;
    u32 issue_count;
    u32 error_count, warning_count, info_count;
    void *impl_; /* private: arena and the growable issue list */
} de__resolved;

typedef struct de__resolve_desc {
    /** For the resolved view and temporaries (NULL = de_heap_allocator()). */
    const de_allocator *allocator;
    /** Tests only: replaces de_hash64 of the ID bytes (to force collisions and the 0 -> 1
     * remap). NULL in real use. */
    u64 (*hash_override)(const char *path, size_t len);
} de__resolve_desc;

/**
 * Resolves `m` (see the top of this file). Works on any manifest, also one with parse errors
 * (it resolves what was parsed). Problems are issues, never fatal:
 *   - error: an asset ID that breaks the ID rules; two asset paths with the same ID;
 *   - error: a bundle entry that is an invalid ID, or a valid ID no asset declares;
 *   - warning: a glob that matches no asset (the message names the bundle);
 *   - error: `depends` names an unknown bundle;
 *   - error: a dependency cycle, reported once, on the bundle whose `depends` closes it, with the
 *     path (`a -> b -> c -> a`).
 * `d` may be NULL.
 * @return DE_OK (`*out` set; check `(*out)->error_count`), DE_ERR_INVALID_ARGUMENT (`m` or `out`
 *         NULL), DE_ERR_OUT_OF_MEMORY (`*out` NULL, nothing leaked).
 */
de_result de__manifest_resolve(const de__manifest *m, const de__resolve_desc *d,
                               de__resolved **out);

/** Releases a resolved view. NULL is ignored. Call before destroying its manifest. */
void de__resolved_destroy(de__resolved *r);

typedef struct de__validate_desc {
    /** Known asset type names. NULL = do not check types (the zero desc). With `known_types`
     * set, an asset whose `type` is not in the list is an error. */
    const char *const *known_types;
    u32 known_type_count;
    /** true = do not check that source files exist. */
    bool skip_sources;
    /** Virtual directory the sources are relative to. NULL = `asset_root` next to the root
     * manifest (`dev:/assets.toml` with `asset_root = "assets"` -> `dev:/assets`). */
    const char *source_dir;
    /** Existence test for a normalised virtual path. NULL = de_path_exists. */
    bool (*exists)(void *user, const char *vpath);
    void *exists_user;
} de__validate_desc;

/**
 * Appends the environment checks to `r`'s issues, asset by asset in manifest order:
 *   - error: `type` is not one of `known_types` (when given);
 *   - error: the `source`, or a `locale` override's source, does not exist (unless
 *     `skip_sources`), or joined to the source directory is not a valid virtual path (e.g. it
 *     climbs out of the root with `..`).
 * `d` may be NULL (= the zero desc: sources checked, types not).
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (`r` NULL), DE_ERR_OUT_OF_MEMORY (the issues appended
 *         so far stay).
 */
de_result de__manifest_validate(de__resolved *r, const de__validate_desc *d);

/** The ID table row of `id`, or NULL. When two paths share the ID (an error), the first row.
 * Binary search; no allocation. */
const de__resolved_entry *de__resolved_find(const de__resolved *r, de_asset_id id);
/** The manifest asset of `id`, or NULL. */
const de__manifest_asset *de__resolved_asset(const de__resolved *r, de_asset_id id);
/** The path (asset ID string) of `id`, or NULL when no asset of the manifest has it. */
const char *de__resolved_path(const de__resolved *r, de_asset_id id);
/** The resolved bundle named `name`, or NULL. Binary search; no allocation. */
const de__resolved_bundle *de__resolved_find_bundle(const de__resolved *r, const char *name);

DE_EXTERN_C_END

#endif /* DE_ASSET_RESOLVE_INTERNAL_H */
