/*
 * ecs_internal.h: the private structures and internal API of the ECS module (SPEC §7).
 *
 * Include as "ecs/ecs_internal.h" (or "ecs_internal.h" inside src/ecs/). Not part of the public
 * API. This header is the contract between the S05 tasks: DENG-344 (storage) defines the world,
 * entity records, components, archetypes and tables; later tasks append their own fields and
 * functions, each in a block marked with its key, so merges stay trivial:
 *   - DENG-345 (structural changes): edge-cache fields at the end of ecs_archetype.
 *   - DENG-348 (resources, events): world fields at the end of de_world.
 *   - DENG-349 (commands), DENG-347 (change ticks), DENG-350 (systems), DENG-352 (snapshots):
 *     their own blocks at the end of de_world and of this file.
 *
 * Internal functions use the prefix `ecs_` (module-private; never exported, never in a public
 * header).
 *
 * Storage layout (DENG-344):
 *   - Entity records: one dense array indexed by the entity index, {archetype, chunk, row,
 *     generation}. A free or retired index has archetype ECS_INVALID_INDEX. Freed indices go on a
 *     LIFO free list; its capacity is kept >= the record count, so freeing never allocates.
 *   - Components: a dense array in registration order; component id = index + 1.
 *   - Archetypes: an array of pointers in creation order (archetype index = position; index 0 is
 *     the empty archetype). That array is the only iteration order. A hash map from the sorted
 *     component set to the index serves lookups only and is never iterated (SPEC §5.7 rule 5).
 *   - Tables: per archetype, a dense list of chunks. Every chunk but the last is full. A chunk is
 *     one `chunk_bytes` block from the world's de_pool, aligned to DE_ECS_MAX_COMPONENT_ALIGN:
 *       [de_entity entities[rows]] then, per data column in component-id order,
 *       [T data[rows]] at the column's alignment, then per column [u32 added[rows]]
 *       [u32 changed[rows]]. Tags have no column. The chunk's row count lives in ecs_chunk, so
 *       the whole block is row data.
 *
 * Threading: everything that changes storage is single-threaded (see de_ecs.h). Reads may run
 * concurrently while nothing changes it.
 */
#ifndef DE_ECS_INTERNAL_H
#define DE_ECS_INTERNAL_H

#include <dengine/de_ecs.h>

#include "core/de_array.h"
#include "core/de_hashmap.h"

#include <string.h>

DE_EXTERN_C_BEGIN

/** "No archetype / no column / no index". */
#define ECS_INVALID_INDEX 0xFFFFFFFFu
/** Index of the empty archetype (no components) in de_world.archetypes. */
#define ECS_EMPTY_ARCHETYPE 0u
/** Largest de_world_desc.chunk_bytes (16 MiB). */
#define ECS_MAX_CHUNK_BYTES (16u * 1024u * 1024u)
/** Largest entity index + 1 (ECS_INVALID_INDEX itself is never an index). */
#define ECS_MAX_ENTITIES 0xFFFFFFFFu

/* Asserts (Debug, Dev) that no schedule is running: immediate structural calls are not allowed
 * inside systems, which must use commands (DENG-349). DENG-350 sets `in_schedule`. */
#define ECS_ASSERT_STRUCTURAL(w) DE_ASSERT(!(w)->in_schedule)

/* ---- Entities (DENG-344) -------------------------------------------------------------------- */

/** Where an entity lives. `archetype` is ECS_INVALID_INDEX while the index is free or retired. */
typedef struct ecs_record {
    u32 archetype;  /**< index into de_world.archetypes */
    u32 chunk;      /**< chunk index in that archetype's table */
    u32 row;        /**< row in that chunk */
    u32 generation; /**< current generation of the index (0: retired) */
} ecs_record;

static inline de_entity ecs_entity_make(u32 index, u32 generation) {
    de_entity e;
    e.bits = ((u64)generation << 32) | (u64)index;
    return e;
}

/* ---- Components (DENG-344) ------------------------------------------------------------------ */

/** A registered component. Copied from the type's reflection data at registration. */
typedef struct ecs_component_info {
    de_type_id type;
    const char *name; /**< the type's name; owned by the type registry */
    u32 flags;        /**< de_component_flags */
    u32 size;         /**< 0 for tags */
    u32 align;        /**< 1 for tags */
    bool is_pod;
    de_type_hooks hooks;
} ecs_component_info;

/* ---- Archetypes and tables (DENG-344) ------------------------------------------------------- */

/** One data column (a non-tag component) of an archetype. Offsets are bytes into a chunk. */
typedef struct ecs_column {
    u32 component;      /**< component id */
    u32 size;           /**< bytes per row */
    u32 data_offset;    /**< T data[rows_per_chunk], aligned to the component's alignment */
    u32 added_offset;   /**< u32 added[rows_per_chunk] (tick semantics: DENG-347) */
    u32 changed_offset; /**< u32 changed[rows_per_chunk] */
} ecs_column;

/** One chunk of a table: a pool block and the rows used in it. */
typedef struct ecs_chunk {
    u8 *data;  /**< chunk_bytes, aligned to DE_ECS_MAX_COMPONENT_ALIGN */
    u32 count; /**< rows in use (all but the last chunk are full) */
    u32 pad_;
} ecs_chunk;

/** An archetype: a unique, sorted component set and its table. */
typedef struct ecs_archetype {
    /* -- DENG-344: identity and layout -- */
    u32 index;           /**< position in de_world.archetypes (creation order) */
    u32 component_count; /**< components in the set (tags included) */
    u32 *components;     /**< sorted ascending component ids */
    u32 *column_of;      /**< per set entry: column index, or ECS_INVALID_INDEX for a tag */
    u32 column_count;    /**< data columns (non-tag components) */
    ecs_column *columns; /**< in component-id order */
    u32 rows_per_chunk;  /**< >= 1 */
    u32 chunk_used;      /**< bytes of a full chunk that the layout uses (<= chunk_bytes) */
    u64 set_hash;        /**< de_hash64 of `components` */
    size_t alloc_size;   /**< bytes of the block holding this struct and its arrays */
    /* -- DENG-344: table -- */
    u32 count;                  /**< rows in the table */
    u32 pad_;                   /* explicit padding */
    DE_ARRAY(ecs_chunk) chunks; /**< dense; every chunk but the last is full */
    /* -- DENG-345: archetype edge cache (append below) -- */
    /* Lazily built edges to the neighbouring archetypes (archetype_graph.c): `add_edges` maps a
     * component this archetype lacks to the archetype with it added, `remove_edges` a component
     * it has to the archetype without it. Each array is sorted by component id (binary search)
     * and owned by the world allocator. A cache only: nothing iterates it to decide a result. */
    struct ecs_edge *add_edges;
    struct ecs_edge *remove_edges;
    u32 add_edge_count, add_edge_capacity;
    u32 remove_edge_count, remove_edge_capacity;
    u64 edge_hits;   /**< add/remove target lookups from this archetype answered by an edge */
    u64 edge_misses; /**< lookups that went to ecs_archetype_get_or_create (set hash + map) */
} ecs_archetype;

/** Key of de_world.archetype_by_set: a view of a sorted component-id array, compared by
 * contents. The stored key points at the archetype's own `components` array. */
typedef struct ecs_set_key {
    const u32 *ids;
    u32 count;
    u32 pad_; /* zeroed */
} ecs_set_key;

/** Called after an archetype is created, in subscription order (DENG-346 queries). */
typedef void (*ecs_archetype_created_fn)(de_world *w, ecs_archetype *a, void *user);

typedef struct ecs_archetype_listener {
    ecs_archetype_created_fn fn;
    void *user;
} ecs_archetype_listener;

/* ---- Resources and events (DENG-348) -------------------------------------------------------- */

/**
 * One resource slot. A slot is bound to its type for the life of the world; `data` is NULL while
 * the resource is removed. The size, alignment and hooks are copied from reflection when the slot
 * is created.
 */
typedef struct ecs_resource {
    de_type_id type;
    u32 size;
    u32 align;
    bool is_pod;
    de_type_hooks hooks;
    void *data; /**< `size` bytes aligned to `align` from the world allocator, or NULL */
} ecs_resource;

/**
 * The two buffers of one event type. `buf[older]` holds last frame's events, with sequence
 * numbers [start[older], start[older] + count); `buf[older ^ 1]` holds this frame's, starting at
 * start[older] + buf[older].count. A buffer is a de_array of payloads (elem_size = payload size).
 */
typedef struct ecs_event_queue {
    de_type_id payload;
    u32 size;
    u32 align;
    bool is_pod;
    u32 older; /**< 0 or 1: index of last frame's buffer */
    de_type_hooks hooks;
    de_array buf[2];
    u64 start[2]; /**< sequence number of each buffer's first event */
} ecs_event_queue;

/* ---- The world ------------------------------------------------------------------------------ */

struct de_world {
    /* -- DENG-344: storage -- */
    const de_allocator *a;
    de_type_registry *types;
    de_task_system *tasks;
    u64 tick;         /**< de_world_tick() */
    u32 change_tick;  /**< current change tick written by ecs_row_init_ticks (DENG-347) */
    u32 chunk_bytes;  /**< bytes per chunk */
    bool in_schedule; /**< set while a schedule runs (DENG-350) */
    de_pool *chunk_pool;
    DE_ARRAY(ecs_record) records;            /**< indexed by entity index */
    DE_ARRAY(u32) free_indices;              /**< LIFO; capacity >= records.count */
    DE_ARRAY(ecs_component_info) components; /**< id - 1 */
    de_hashmap component_by_name;            /**< de_hash64_str(name) -> u32 id; lookup only */
    de_hashmap component_by_type;            /**< de_type_id.id -> u32 id; lookup only */
    DE_ARRAY(ecs_archetype *) archetypes;    /**< creation order; [0] = empty archetype */
    de_hashmap archetype_by_set;             /**< ecs_set_key -> u32 archetype index; lookup only */
    DE_ARRAY(u32) set_scratch;               /**< canonicalisation buffer of get_or_create */
    DE_ARRAY(ecs_archetype_listener) archetype_listeners;
    /** Returned by de_entity_get for tag components: non-NULL, never read or written. */
    u64 tag_placeholder[2];
    /* -- DENG-348: resources and events (append below) -- */
    DE_ARRAY(ecs_resource) resources; /**< resource id - 1; first-insert order */
    de_hashmap resource_by_type;      /**< de_type_id.id -> u32 resource id; lookup only */
    DE_ARRAY(ecs_event_queue) events; /**< event type id - 1; registration order */
    de_hashmap event_by_type;         /**< de_type_id.id -> u32 event type id; lookup only */
    /* -- DENG-349: command buffers (append below) -- */
    struct ecs_cmd_state *cmd; /**< commands.c; created by ecs_commands_init */
    /* -- DENG-346: queries (append below) -- */
    struct ecs_query_registry *queries; /**< query slots (query_internal.h); NULL until the
                                             first de_query_create */
    /* -- DENG-347: change detection (append below; the rest is in change_internal.h) -- */
    u32 change_check_tick; /**< change_tick at the last ecs_check_change_ticks */
    /* -- DENG-352: snapshots and the world checksum (append below) -- */
    struct ecs_snap_state *snap; /**< snapshot.c: restore scratch and counters; NULL until the
                                      first restore */
    struct ecs_ck_cache *ck;     /**< checksum.c: per-component field spans; NULL until the first
                                      de_world_checksum */
};

/* ---- Internal API: entities (entity.c, DENG-344) -------------------------------------------- */

/** The record of `e` if it is alive, else NULL. */
ecs_record *ecs_entity_record(const de_world *w, de_entity e);

/**
 * Creates an entity in archetype `a`: takes an index (LIFO free list, else a new one), appends a
 * row, constructs every column (construct hook, or zero) and initialises its ticks
 * (ecs_row_init_ticks). On failure nothing changes. Used by de_entity_spawn, DENG-345's
 * spawn_with and DENG-349's apply step.
 */
de_result ecs_entity_create(de_world *w, ecs_archetype *a, de_entity *out);

/**
 * Frees index `index` after its row was removed: bumps the generation and pushes the index on
 * the free list, or retires it when the generation would wrap past 2^32 - 1 (generation 0, never
 * reused). Never allocates. DENG-345's despawn calls it after destructing and removing the row.
 */
void ecs_entity_free(de_world *w, u32 index);

/* ---- Internal API: components (component.c, DENG-344) --------------------------------------- */

/** The info of component `id`, or NULL if it is not registered. */
static inline const ecs_component_info *ecs_component_get(const de_world *w, u32 id) {
    if (id == 0 || id > w->components.count)
        return NULL;
    return (const ecs_component_info *)de_array_at(&w->components, id - 1u);
}

/* ---- Internal API: archetypes (archetype.c, DENG-344) --------------------------------------- */

static inline u32 ecs_archetype_count(const de_world *w) {
    return w->archetypes.count;
}

static inline ecs_archetype *ecs_archetype_at(const de_world *w, u32 index) {
    return ((ecs_archetype *const *)w->archetypes.data)[index];
}

/**
 * The archetype with exactly the components `ids[0..count)` (any order, duplicates allowed),
 * creating it if needed. A new archetype is appended to the creation-order array, then every
 * ecs_on_archetype_created listener runs. `ids` must not point into w->set_scratch.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (an id that is not registered), DE_ERR_UNSUPPORTED (no
 *         row fits a chunk) or DE_ERR_OUT_OF_MEMORY. On failure nothing changes.
 */
de_result ecs_archetype_get_or_create(de_world *w, const u32 *ids, u32 count, ecs_archetype **out);

/** The archetype with exactly the sorted, unique set `ids[0..count)`, or NULL. Lookup only. */
ecs_archetype *ecs_archetype_find(const de_world *w, const u32 *sorted_ids, u32 count);

/** Position of component `component` in `a->components`, or ECS_INVALID_INDEX. Binary search. */
u32 ecs_archetype_find_component(const ecs_archetype *a, u32 component);

/** Column index of component `component` in `a`, or ECS_INVALID_INDEX (absent, or a tag). */
static inline u32 ecs_archetype_column(const ecs_archetype *a, u32 component) {
    const u32 i = ecs_archetype_find_component(a, component);
    return i == ECS_INVALID_INDEX ? ECS_INVALID_INDEX : a->column_of[i];
}

/** true if `a` contains `component` (tags included). */
static inline bool ecs_archetype_has(const ecs_archetype *a, u32 component) {
    return ecs_archetype_find_component(a, component) != ECS_INVALID_INDEX;
}

/**
 * Subscribes `fn(w, archetype, user)` to archetype creation. Listeners run in subscription order,
 * after the archetype is in the array and the lookup map. Existing archetypes are not replayed:
 * a subscriber matches them itself (in creation order). @return DE_OK or DE_ERR_OUT_OF_MEMORY.
 */
de_result ecs_on_archetype_created(de_world *w, ecs_archetype_created_fn fn, void *user);

/** Removes the first subscription with this (fn, user); keeps the order of the others. */
void ecs_off_archetype_created(de_world *w, ecs_archetype_created_fn fn, void *user);

/* ---- Internal API: archetype graph and structural changes (DENG-345) ------------------------ */

/** One cached edge of the archetype graph. */
typedef struct ecs_edge {
    u32 component; /**< the component added or removed */
    u32 target;    /**< index of the archetype it leads to */
} ecs_edge;

/**
 * The archetype `a` plus `component` (a itself if it has it already). The first lookup per
 * (archetype, component) goes through ecs_archetype_get_or_create (which may create the target,
 * in operation order) and caches the edge, plus the reverse remove edge on the target; later
 * lookups are a binary search. A failure to cache (out of memory) is ignored: the edge is a
 * cache. @return DE_OK, DE_ERR_INVALID_ARGUMENT (`component` not registered),
 * DE_ERR_UNSUPPORTED or DE_ERR_OUT_OF_MEMORY (no target; nothing changed).
 */
de_result ecs_archetype_add_target(de_world *w, ecs_archetype *a, u32 component,
                                   ecs_archetype **out);

/** The archetype `a` minus `component` (a itself if it lacks it). Same caching and results as
 * ecs_archetype_add_target; caches the reverse add edge on the target. */
de_result ecs_archetype_remove_target(de_world *w, ecs_archetype *a, u32 component,
                                      ecs_archetype **out);

/** Frees `a`'s edge arrays (de_world_destroy). */
void ecs_archetype_edges_free(de_world *w, ecs_archetype *a);

/** Archetype-graph counters, summed over every archetype in creation order. */
typedef struct ecs_graph_stats {
    u64 edge_hits;   /**< target lookups answered by a cached edge */
    u64 edge_misses; /**< target lookups that hashed the set (ecs_archetype_get_or_create) */
    u32 add_edges;   /**< cached add edges */
    u32 remove_edges;
} ecs_graph_stats;

void ecs_graph_stats_get(const de_world *w, ecs_graph_stats *out);

/**
 * Adds `component` to alive entity `e` (structural.c). If `e` has it, nothing moves. Otherwise
 * `e` moves to the add target; the new column is constructed (hook, or zero) and its ticks set
 * (ecs_row_construct -> ecs_row_init_ticks). `*out_data` (may be NULL) gets the component's data,
 * or the tag placeholder for a tag. DENG-349 applies deferred adds through this.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (`e` not alive, `component` not registered),
 *         DE_ERR_UNSUPPORTED or DE_ERR_OUT_OF_MEMORY (nothing changed).
 */
de_result ecs_entity_add(de_world *w, de_entity e, u32 component, void **out_data);

/**
 * Removes `component` from alive entity `e` (its destruct hook runs). Absent: DE_OK, nothing
 * changes. @return DE_OK, DE_ERR_INVALID_ARGUMENT (`e` not alive) or DE_ERR_OUT_OF_MEMORY
 * (nothing changed).
 */
de_result ecs_entity_remove(de_world *w, de_entity e, u32 component);

/**
 * Despawns alive entity `e`: destructs every column (column order), swap-removes the row
 * (updating the record of the row that moves in) and frees the index (generation + 1, LIFO free
 * list). Never allocates. @return DE_OK or DE_ERR_INVALID_ARGUMENT (`e` not alive; no change).
 */
de_result ecs_entity_despawn(de_world *w, de_entity e);

/* ---- Internal API: tables (table.c, DENG-344) ----------------------------------------------- */

static inline ecs_chunk *ecs_table_chunk(const ecs_archetype *a, u32 chunk) {
    return &((ecs_chunk *)a->chunks.data)[chunk];
}

/** The entity array of a chunk (row i holds the entity of row i). */
static inline de_entity *ecs_chunk_entities(const ecs_chunk *ch) {
    return (de_entity *)(void *)ch->data;
}

/** The data array of column `column` in chunk `ch` of archetype `a`. */
static inline void *ecs_chunk_column(const ecs_archetype *a, const ecs_chunk *ch, u32 column) {
    return ch->data + a->columns[column].data_offset;
}

/** The `added` tick array of column `column`. */
static inline u32 *ecs_chunk_added(const ecs_archetype *a, const ecs_chunk *ch, u32 column) {
    return (u32 *)(void *)(ch->data + a->columns[column].added_offset);
}

/** The `changed` tick array of column `column`. */
static inline u32 *ecs_chunk_changed(const ecs_archetype *a, const ecs_chunk *ch, u32 column) {
    return (u32 *)(void *)(ch->data + a->columns[column].changed_offset);
}

/** Pointer to the data of `column` at (`chunk`, `row`). */
static inline void *ecs_row_column(const ecs_archetype *a, u32 chunk, u32 row, u32 column) {
    return (u8 *)ecs_chunk_column(a, ecs_table_chunk(a, chunk), column) +
           (size_t)row * a->columns[column].size;
}

/**
 * Appends a row for `e` to `a`'s table and writes `e` into the entity array. The component data
 * and ticks of the row are uninitialised, and no record is changed. Allocates a chunk from the
 * pool when the last one is full. @return DE_OK or DE_ERR_OUT_OF_MEMORY (nothing changed).
 */
de_result ecs_table_push_row(de_world *w, ecs_archetype *a, de_entity e, u32 *out_chunk,
                             u32 *out_row);

/**
 * Removes row (`chunk`, `row`) of `a` by moving the table's last row into it (entity, data and
 * ticks, raw bytes; no hooks), and updates the record of the entity that moved. The removed
 * row's own record is the caller's. Gives an emptied last chunk back to the pool.
 */
void ecs_table_swap_remove(de_world *w, ecs_archetype *a, u32 chunk, u32 row);

/** Initialises `column` at (`chunk`, `row`): the construct hook, or zero; then its ticks. */
void ecs_row_construct(de_world *w, ecs_archetype *a, u32 chunk, u32 row, u32 column);

/** Runs the destruct hook (if any) of `column` at (`chunk`, `row`). */
void ecs_row_destruct(de_world *w, ecs_archetype *a, u32 chunk, u32 row, u32 column);

/**
 * Moves alive entity `e` to archetype `dst`: appends a row to `dst`, moves the bytes and ticks of
 * every column both archetypes have (memcpy: components are trivially relocatable), constructs
 * the columns only `dst` has, destructs the columns only the source has, swap-removes the source
 * row and updates both records. A move to the entity's own archetype does nothing.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (`e` not alive) or DE_ERR_OUT_OF_MEMORY (nothing
 *         changed).
 */
de_result ecs_entity_move(de_world *w, de_entity e, ecs_archetype *dst);

/**
 * Initialises the `added` and `changed` ticks of `column` at (`chunk`, `row`) for a newly added
 * component. Default (DENG-344): both = w->change_tick. DENG-347 owns the semantics and may
 * replace this definition (it lives in table.c in its own block).
 */
void ecs_row_init_ticks(de_world *w, ecs_archetype *a, u32 chunk, u32 row, u32 column);

/* ---- Internal API: resources and events (resource.c, event.c, DENG-348) -------------------- */

/** Initialises the resource and event fields of a zeroed world. Allocates nothing. */
de_result ecs_resources_init(de_world *w);
de_result ecs_events_init(de_world *w);

/**
 * Destructs and frees every resource, in reverse first-insert order (later resources may refer to
 * earlier ones). Safe on a world whose resource fields are zeroed or initialised.
 */
void ecs_resources_destroy(de_world *w);

/** Destructs every alive event and frees every event buffer, in registration order. Safe on a
 * world whose event fields are zeroed or initialised. */
void ecs_events_destroy(de_world *w);

/* ---- Internal API: command buffers (commands.c, DENG-349) ---------------------------------- */
/* The full command-buffer API (acquire, apply) is in commands_internal.h. */

/** Creates the world's command state (w->cmd). @return DE_OK or DE_ERR_OUT_OF_MEMORY. */
de_result ecs_commands_init(de_world *w);

/** Destructs the payloads of every unapplied command and frees all command memory. Safe when
 * w->cmd is NULL. */
void ecs_commands_destroy(de_world *w);

/** Asserts (Debug, Dev) that `e` is not a pending id: immediate functions take real ids only. */
#define ECS_ASSERT_NOT_PENDING(e) DE_ASSERT(!de_entity_is_pending(e))

/* ---- Internal API: queries (query.c, DENG-346; the rest is in query_internal.h) ----------- */

/** Destroys every query of the world and the query registry (de_world_destroy). */
void ecs_queries_destroy(de_world *w);

/* ---- Internal API: snapshots and checksum (snapshot.c, checksum.c, DENG-352) --------------- */

/** Counters of de_world_snapshot_restore, summed over every restore of the world. Kept in every
 * build type (like the edge-cache counters). */
typedef struct ecs_snapshot_stats {
    u64 restores;         /**< successful restores */
    u64 fast_sections;    /**< sections whose table already held their rows in order */
    u64 ordered_sections; /**< other sections whose rows were put back in snapshot order */
    u64 loose_sections;   /**< sections whose entities ended up in several archetypes */
    u64 block_copies;     /**< memcpy calls of one column of one chunk */
    u64 row_copies;       /**< memcpy calls of one column of one row */
    u64 despawned;        /**< entities despawned because the snapshot does not have them */
    u64 recreated;        /**< entities re-created from the snapshot */
    u64 moved;            /**< surviving entities moved to another archetype */
    u64 row_swaps;        /**< row swaps that put a table back in snapshot order */
} ecs_snapshot_stats;

/** The world's restore counters (all zero before the first restore). */
void ecs_snapshot_stats_get(const de_world *w, ecs_snapshot_stats *out);

/** Frees the restore scratch (de_world_destroy). Safe when w->snap is NULL. */
void ecs_snapshot_destroy(de_world *w);

/** Frees the checksum's field-span cache (de_world_destroy). Safe when w->ck is NULL. */
void ecs_checksum_destroy(de_world *w);

DE_EXTERN_C_END

#endif /* DE_ECS_INTERNAL_H */
