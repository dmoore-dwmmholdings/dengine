/*
 * budget_internal.h: per-category budgets, residency accounting, LRU eviction of unreferenced
 * assets, resident bundles and the I/O rate (SPEC §11.8; DENG-144). Internal to de_asset; include
 * as "asset/budget_internal.h".
 *
 * Accounting. Every slot that becomes READY passes through de__budget_on_publish (load.c, where
 * a completion is applied). Its bytes count against the category of its type at that moment, and
 * the slot gets a budget record (de__budget_slot, in pages indexed by slot index, like the load
 * state). So the records list every slot that holds an object; de_asset_flush_unused scans them.
 *
 * Candidates. A slot is an eviction candidate while it is READY, accounted, not resident and has
 * no reference. Candidates sit on one intrusive doubly linked LRU list per category, ordered by
 * (de__slot.last_used_frame, asset ID): head = least recently used. New candidates (refcount
 * drained to zero, a publish without references, a typed access that touched a listed slot) go
 * to a pending batch first; de__budget_commit sorts the batch by ID (a merge sort of the linked
 * batch, no allocation) and merges it into the lists. Every key in a batch is the current frame,
 * so a merge only walks the tail entries of that frame. Nothing depends on thread timing or hash
 * map order.
 *
 * The lists are hints, like the slots' candidate list: a listed slot that gained a reference is
 * dropped from its list when eviction reaches it (it is queued again when its refcount crosses
 * zero again).
 *
 * Threads: main thread only, except de__budget_add_io_bytes (any thread; atomic).
 */
#ifndef DE_ASSET_BUDGET_INTERNAL_H
#define DE_ASSET_BUDGET_INTERNAL_H

#include <dengine/de_asset.h>
#include <dengine/de_base.h>

#include "asset/resolve_internal.h"
#include "asset/slots_internal.h"
#include "platform/de_atomic.h"

DE_EXTERN_C_BEGIN

/** Most evictions per de_asset_system_update (bounds the frame time). */
#define DE__BUDGET_EVICT_CAP 64u
/** I/O rate samples (one per >= 1/32 s): enough for the 1 s window. */
#define DE__BUDGET_IO_SAMPLES 40u
/** The I/O rate window and the over-budget warning interval, in ns. */
#define DE__BUDGET_WINDOW_NS ((u64)1000000000u)

/** Where a budget record is. */
typedef enum de__budget_list {
    DE__BUDGET_LIST_NONE = 0,
    DE__BUDGET_LIST_LRU = 1,    /**< on the LRU list of its category */
    DE__BUDGET_LIST_PENDING = 2 /**< in the pending batch (singly linked through `next`) */
} de__budget_list;

/** The budget record of one slot, indexed by slot index. `gen` names the occupant. */
typedef struct de__budget_slot {
    u32 gen;      /**< slot generation this record is for; 0 = unused */
    u32 prev;     /**< LRU list: previous record index + 1; 0 = none */
    u32 next;     /**< LRU list or pending batch: next record index + 1; 0 = none */
    u8 list;      /**< de__budget_list */
    u8 resident;  /**< pinned by a resident bundle; never a candidate */
    u8 accounted; /**< `bytes` count in the totals */
    u8 category;  /**< de_asset_category the bytes count under */
    u64 bytes;    /**< accounted bytes */
} de__budget_slot;

/** One I/O rate sample. */
typedef struct de__budget_io_sample {
    u64 t_ns;
    u64 bytes;
} de__budget_io_sample;

/** All budget state of a system (asset_internal.h: `s->budget`). */
typedef struct de__budget {
    de_asset_budgets budgets;
    u64 frame; /**< de_asset_system_update count: the LRU clock */
    /* Accounting. */
    u64 bytes[DE_ASSET_CAT_COUNT];
    u32 assets;
    /* Records: pages that never move (DE__SLOT_PAGE_SLOTS records each). */
    de__budget_slot *pages[DE__SLOT_MAX_PAGES];
    u32 index_end; /**< 1 + the highest record index ever prepared */
    /* LRU lists per category (record index + 1; 0 = empty) and the pending batch. */
    u32 lru_head[DE_ASSET_CAT_COUNT], lru_tail[DE_ASSET_CAT_COUNT];
    u32 pending_head;
    /* Over-budget warnings: when each category last warned (0 = never). */
    u64 warned_ns[DE_ASSET_CAT_COUNT];
    bool warned[DE_ASSET_CAT_COUNT];
    /* I/O rate. */
    de_atomic_u64 io_bytes; /**< bytes reported read, ever (de__budget_add_io_bytes) */
    de__budget_io_sample io_samples[DE__BUDGET_IO_SAMPLES];
    u32 io_sample_count, io_sample_next;
    u64 io_rate;           /**< bytes per second, as of the last update */
    u32 evictions_total;   /**< evictions since create (tests, debugging) */
    u32 over_budget_warns; /**< warnings logged since create (tests) */
} de__budget;

/** asset_system.c: create (budgets from `s->desc.budgets`). DE_ERR_OUT_OF_MEMORY. */
de_result de__budget_init(de_asset_system *s);
/** asset_system.c: destroy (frees the records; objects are the slots' business). */
void de__budget_shutdown(de_asset_system *s);

/**
 * de_asset_system_update, after the load update: takes the drained candidates, sorts them into
 * the lists, evicts while a category is over budget (at most DE__BUDGET_EVICT_CAP), warns about
 * categories over budget with no candidate, samples the I/O rate, then advances the frame.
 * `now_ns` is de_time_now_ns() (tests pass their own clock).
 */
void de__budget_update(de_asset_system *s, u64 now_ns);

/**
 * load.c: slot `slot` became READY with `bytes` resident (also a publish again): the bytes count
 * under its type's category (replacing what it counted before). A slot without references
 * becomes a candidate.
 */
void de__budget_on_publish(de_asset_system *s, u32 slot, u64 bytes);

/** typed_access.c: the slot was used now (its LRU frame becomes the current one). */
void de__budget_touch(de_asset_system *s, u32 slot);

/**
 * load.c (de_asset_load_bundle, after the request was created): pins the members of every
 * bundle with `resident = true` among `rb` and the bundles it depends on. Nothing unpins them.
 */
void de__budget_pin_bundle(de_asset_system *s, const de__resolved *res,
                           const de__resolved_bundle *rb);

/** load.c (de__slot_progress): `bytes` more were read. Any thread. */
void de__budget_add_io_bytes(de_asset_system *s, u64 bytes);

/**
 * DENG-212 (de_asset_load_bundle of a pack bundle, after the request was created): pins the
 * assets `ids` (the members of resident pack bundles) like de__budget_pin_bundle. Main thread.
 */
void de__budget_pin_ids(de_asset_system *s, const de_asset_id *ids, u32 count);
/** True when slot `slot` is pinned by a resident bundle. For tests. */
bool de__budget_is_resident(de_asset_system *s, u32 slot);

DE_EXTERN_C_END

#endif /* DE_ASSET_BUDGET_INTERNAL_H */
