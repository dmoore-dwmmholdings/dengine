/*
 * slots_internal.h: asset slots, typed handles, refcounts and the asset type registry (SPEC §11.1,
 * §11.2, §11.8; DENG-140). Internal to de_asset; include as "asset/slots_internal.h".
 *
 * A typed handle (de_texture, de_mesh, ... de_blob) names a SLOT, not memory, so hot reload (S08)
 * and streaming (S20) can swap the data behind it. Every slot holds one asset ID.
 *
 * Handle encoding (u32): low 20 bits slot index, high 12 bits generation. A generation is 1..4095
 * and never 0, so a handle id is never 0 (0 = invalid handle). Freeing a slot bumps its
 * generation, so every handle to the old occupant becomes stale: de__slot_get returns NULL for it.
 * At most 2^20 slots exist at once.
 *
 * Storage: slots live in pages of DE__SLOT_PAGE_SLOTS, taken from a de_pool on the system
 * allocator. Pages never move and are never freed before the system is destroyed, so a de__slot
 * pointer stays valid as long as the slot is not freed. An ID -> slot index de_hashmap finds the
 * slot of an ID (lookup only, never iterated).
 *
 * Threads:
 *   - Main thread only: creating and freeing slots, de__slot_find, de__asset_handle_of and the
 *     typed accessors, the plain (non-atomic) slot fields, the eviction drain, registering types.
 *   - Any thread: de__slot_get, de__asset_object, the refcount functions (atomic), type lookups.
 *     A worker must hold a reference (or otherwise know the slot cannot be freed) while it uses
 *     a slot pointer.
 *
 * Refcounts (§11.8): a slot is referenced by load handles and by other assets (asset -> asset
 * dependency edges). Both counts live in one atomic u64 (handle refs in the low 32 bits, asset
 * refs in the high 32 bits), so exactly one decrement sees the total reach zero. That decrement
 * puts the slot on the eviction-candidate list, at most once until the list is drained
 * (de__slot_eviction_drain, consumed by DENG-144). A candidate is only a candidate: whoever
 * drains the list re-checks the refcount before evicting.
 */
#ifndef DE_ASSET_SLOTS_INTERNAL_H
#define DE_ASSET_SLOTS_INTERNAL_H

#include <dengine/de_asset.h>
#include <dengine/de_base.h>

#include "platform/de_atomic.h"

DE_EXTERN_C_BEGIN

/* ---- Handles ---------------------------------------------------------------------------- */

#define DE__SLOT_INDEX_BITS 20u
#define DE__SLOT_INDEX_MASK ((1u << DE__SLOT_INDEX_BITS) - 1u)
#define DE__SLOT_GEN_BITS   12u
#define DE__SLOT_GEN_MAX    ((1u << DE__SLOT_GEN_BITS) - 1u) /**< 4095; generations are 1..4095 */
/** Most slots at once: indices 0 .. 2^20 - 1. */
#define DE__SLOT_MAX (1u << DE__SLOT_INDEX_BITS)
/** Slots per page (a power of two). */
#define DE__SLOT_PAGE_SHIFT 8u
#define DE__SLOT_PAGE_SLOTS (1u << DE__SLOT_PAGE_SHIFT)
#define DE__SLOT_MAX_PAGES  (DE__SLOT_MAX / DE__SLOT_PAGE_SLOTS)

DE_STATIC_ASSERT(DE__SLOT_INDEX_BITS + DE__SLOT_GEN_BITS == 32, "a handle id is 32 bits");

static inline u32 de__slot_handle_make(u32 index, u32 generation) {
    return (generation << DE__SLOT_INDEX_BITS) | (index & DE__SLOT_INDEX_MASK);
}
static inline u32 de__slot_handle_index(u32 handle_id) {
    return handle_id & DE__SLOT_INDEX_MASK;
}
static inline u32 de__slot_handle_generation(u32 handle_id) {
    return handle_id >> DE__SLOT_INDEX_BITS;
}

/* ---- Slots ------------------------------------------------------------------------------ */

/** Handle refs are counted in the low 32 bits of de__slot.refs, asset refs in the high 32. */
#define DE__SLOT_REF_HANDLE ((u64)1)
#define DE__SLOT_REF_ASSET  ((u64)1 << 32)

/**
 * One asset slot. Fields marked (main) are written on the main thread only; DENG-141 and later
 * tasks own their meaning (states, sizes, dependencies), this task only stores and resets them.
 */
typedef struct de__slot {
    de_asset_id id;       /**< the asset in this slot; {0} when free */
    u64 type_hash;        /**< de_hash64_str of the declared type name (e.g. "texture") */
    de_atomic_u64 refs;   /**< handle refs (low 32) | asset refs (high 32); see the file comment */
    de_atomic_ptr object; /**< the live object (type-specific); NULL until loaded */
    u64 bytes;            /**< (main) resident bytes, for budgets */
    u64 last_used_frame;  /**< (main) for LRU eviction (DENG-144) */
    de_asset_id *deps;    /**< (main) dependency IDs; owned, from the system allocator */
    de_asset_type type;   /**< registered type when the slot was created; {0} if none was */
    de_atomic_u32 live_gen;   /**< the generation while live; 0 while free */
    u32 gen;                  /**< (main) the last generation handed out for this index */
    de_load_state state;      /**< (main) DE_LOAD_INVALID until DENG-141 requests a load */
    u32 version;              /**< (main) bumped by each (re)load that publishes an object */
    u32 dep_count;            /**< (main) entries in `deps` */
    de_result error;          /**< (main) the last load error, DE_OK if none */
    de_atomic_u32 queued;     /**< 1 while on the eviction-candidate list */
    de_atomic_u32 evict_next; /**< next slot index + 1 on the eviction-candidate list; 0 = end */
    u32 free_next;            /**< (main) next slot index + 1 on the free list; 0 = end */
} de__slot;

/**
 * The live slot of `handle_id`, or NULL when the handle is 0, out of range, or stale (its slot
 * was freed, or freed and reused). Any thread. Debug and Dev builds log the first stale lookup
 * of each system once (warning, category "asset").
 */
de__slot *de__slot_get(de_asset_system *s, u32 handle_id);

/**
 * The handle id of the slot holding `id`, or 0 when no slot holds it. Main thread. Does not set
 * the error context.
 */
u32 de__slot_find(de_asset_system *s, de_asset_id id);

/**
 * Creates the slot of `id` (no type check, no manifest lookup): for de__asset_handle_of and for
 * loaders that create slots for sub-assets (`player/model#Body`, S08+). `type_hash` is
 * de_hash64_str of the type name; `type` is resolved from the registry when that type is
 * registered. The slot starts with zero refs, state DE_LOAD_INVALID, no object, and is NOT on the
 * eviction-candidate list. Main thread.
 * @return DE_OK (`*out_handle` set), DE_ERR_INVALID_ARGUMENT (NULL argument, `id` 0),
 *         DE_ERR_ALREADY_EXISTS (a slot holds `id`; `*out_handle` gets it), DE_ERR_OUT_OF_MEMORY
 *         (2^20 slots are live, or no memory). Failures set the error context.
 */
de_result de__slot_create(de_asset_system *s, de_asset_id id, u64 type_hash, u32 *out_handle);

/**
 * Frees the slot of `handle_id`: unloads its object through its type's `unload` (if both
 * exist), frees `deps`, forgets the ID and bumps the generation so every handle to it becomes
 * stale. It does not drop the asset refs this asset holds on its dependencies (the pipeline,
 * DENG-142, does that first). Main thread.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (stale handle), DE_ERR_BUSY (refs are not zero).
 */
de_result de__slot_free(de_asset_system *s, u32 handle_id);

/** Live slots. Main thread. */
u32 de__slot_live_count(const de_asset_system *s);

/* ---- Refcounts (any thread) --------------------------------------------------------------- */

/** Adds a load-handle reference. false (nothing changed) if the handle is stale. */
bool de__slot_ref_handle(de_asset_system *s, u32 handle_id);
/**
 * Drops a load-handle reference. true if this call dropped the slot's LAST reference (handle and
 * asset refs together); the slot is then on the eviction-candidate list. false otherwise, and
 * also (with a Debug/Dev assert) if the handle is stale or has no handle reference.
 */
bool de__slot_unref_handle(de_asset_system *s, u32 handle_id);
/** Adds an asset -> asset reference (the dependent holds one per dependency). */
bool de__slot_ref_asset(de_asset_system *s, u32 handle_id);
/** Drops an asset -> asset reference; same rules as de__slot_unref_handle. */
bool de__slot_unref_asset(de_asset_system *s, u32 handle_id);

/** Reads both counts at once. */
static inline void de__slot_refs(const de__slot *slot, u32 *handle_refs, u32 *asset_refs) {
    u64 r = de_atomic_load_u64(&slot->refs, DE_ATOMIC_ACQUIRE);
    if (handle_refs != NULL)
        *handle_refs = (u32)r;
    if (asset_refs != NULL)
        *asset_refs = (u32)(r >> 32);
}

/** Total references (handles + assets). */
static inline u64 de__slot_ref_total(const de__slot *slot) {
    u64 r = de_atomic_load_u64(&slot->refs, DE_ATOMIC_ACQUIRE);
    return (r & 0xFFFFFFFFu) + (r >> 32);
}

/** The slot's object (acquire), for any thread. */
static inline void *de__slot_object(const de__slot *slot) {
    return de_atomic_load_ptr(&slot->object, DE_ATOMIC_ACQUIRE);
}

/** Publishes `object` into the slot (release), so a thread that reads it sees it complete. Main
 * thread (state publishing); the previous object is not unloaded here. */
static inline void de__slot_set_object(de__slot *slot, void *object) {
    de_atomic_store_ptr(&slot->object, object, DE_ATOMIC_RELEASE);
}

/* ---- Eviction candidates (DENG-144 consumes them) ------------------------------------------ */

/** Called once per candidate by de__slot_eviction_drain. */
typedef void (*de__slot_evict_fn)(de_asset_system *s, u32 handle_id, void *user);

/**
 * Takes every slot off the eviction-candidate list and calls `fn` for each one that is still
 * live, most recently queued first. The callback may free the slot. A slot whose refcount drops
 * to zero again during the drain is queued again for the next drain. Main thread.
 * @return The number of `fn` calls.
 */
u32 de__slot_eviction_drain(de_asset_system *s, de__slot_evict_fn fn, void *user);

/* ---- Typed access ------------------------------------------------------------------------- */

/**
 * The handle id of the slot of `id`, created on first use, checked against `type_name`
 * ("texture", "test_bytes", ...; NULL = any type). Without an existing slot, `id` must be an
 * asset of the active resolved manifest (`s->resolved`, DENG-139), and its declared `type` must
 * be `type_name`. The handle is valid
 * before the asset is loaded (§11.3); no reference is taken. Main thread.
 * @return The handle id, or 0 with the error context set: DE_ERR_INVALID_ARGUMENT (NULL system,
 *         id 0, or the asset has another type), DE_ERR_NOT_FOUND (no slot and no manifest
 *         asset), DE_ERR_OUT_OF_MEMORY.
 */
u32 de__asset_handle_of(de_asset_system *s, de_asset_id id, const char *type_name);

/** The object in the slot of `handle_id`, or NULL (stale handle, or not loaded). Any thread. */
void *de__asset_object(de_asset_system *s, u32 handle_id);

/** asset_system.c calls this from de_asset_system_destroy, before de__asset_types_shutdown:
 * unloads every live object and frees the slot table. */
void de__slots_shutdown(de_asset_system *s);

/* ---- Asset type registry (asset_types.c) --------------------------------------------------- */

/** Most registered asset types. */
#define DE__ASSET_MAX_TYPES 256u
/** Longest type name, without the NUL. */
#define DE__ASSET_TYPE_NAME_MAX 63u

/** The registered desc of `type` (name and extensions are owned copies; extensions are stored
 * lowercase without the leading dot), or NULL for an invalid handle. Any thread. */
const de_asset_type_desc *de__asset_type_get(const de_asset_system *s, de_asset_type type);
/** The type named `name` (exact bytes), or {0}. Any thread. */
de_asset_type de__asset_type_find(const de_asset_system *s, const char *name);
/** The type whose name hashes to `name_hash` (de_hash64_str), or {0}. Any thread. */
de_asset_type de__asset_type_find_hash(const de_asset_system *s, u64 name_hash);
/**
 * The type that claims the file extension `ext` (`".png"`, `"png"` or `"PNG"`; ASCII case is
 * ignored), or {0}. When two types claim one extension, the first registered wins. Any thread.
 */
de_asset_type de__asset_type_find_ext(const de_asset_system *s, const char *ext);
/**
 * Every registered type name, in registration order: the known-type set for the manifest
 * validator (DENG-139). `*count` gets their number. The array and strings stay valid until the
 * system is destroyed; registering a type appends to it. Any thread.
 */
const char *const *de__asset_type_names(const de_asset_system *s, u32 *count);

/** The registered type of a slot: its `type`, or (if the type was registered after the slot was
 * created) the type registered under its type name since; {0} if none. */
de_asset_type de__slot_type(const de_asset_system *s, const de__slot *slot);

/** asset_system.c calls this from de_asset_system_destroy, after de__slots_shutdown. */
void de__asset_types_shutdown(de_asset_system *s);

DE_EXTERN_C_END

#endif /* DE_ASSET_SLOTS_INTERNAL_H */
