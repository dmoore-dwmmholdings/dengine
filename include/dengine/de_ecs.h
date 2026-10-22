/* de_ecs.h — archetype ECS: entities, components, queries, systems, schedules, events.
 * SPEC.md §7. */
#ifndef DE_ECS_H
#define DE_ECS_H
#include "de_base.h"
#include "de_math.h" /* de_transform (DENG-351 built-in components) */
#include "de_memory.h"
#include "de_reflect.h"
#include "de_task.h"
DE_EXTERN_C_BEGIN

/*
 * Storage model (SPEC §7.1, DENG-344).
 *
 * - An entity is a 64-bit id: the low 32 bits are an index into the world's entity records, the
 *   high 32 bits a generation. Generations start at 1, so an id with generation 0 is never a live
 *   entity (DE_ENTITY_NULL is all zeros). Each despawn bumps the index's generation; an index
 *   whose generation would wrap past 2^32 - 1 is retired and never handed out again.
 * - Freed indices are reused last-in, first-out. Ids depend only on the sequence of spawns and
 *   despawns, never on threads or timing.
 * - A component is a reflected type (de_reflect.h) registered with the world. Component ids are
 *   dense and given in registration order, so they are part of the determinism contract: register
 *   components in the same order on every machine.
 * - An archetype is a unique set of components. Its table stores the entities that have exactly
 *   that set, one column per component (SoA), in chunks of `chunk_bytes`. Archetypes are kept in
 *   creation order, and every iteration of the world uses archetype order, then chunk order, then
 *   row order (SPEC §5.7 rule 5).
 *
 * Threading: structural functions (spawn, despawn, add, remove, register) are single-threaded and
 * not allowed while a schedule runs (systems use commands; asserted in Debug and Dev). Read-only
 * functions (de_entity_alive, de_entity_has, de_entity_get_const, de_component_find) may run on
 * many threads at once while no structural change runs.
 */

typedef struct de_world de_world;
typedef struct de_entity {
    u64 bits;
} de_entity; /* low 32: index, high 32: generation */
#define DE_ENTITY_NULL ((de_entity){0})

/** The index part (low 32 bits) of an entity id. */
static inline u32 de_entity_index(de_entity e) {
    return (u32)(e.bits & 0xFFFFFFFFu);
}
/** The generation part (high 32 bits) of an entity id. 0 is never a live entity. */
static inline u32 de_entity_generation(de_entity e) {
    return (u32)(e.bits >> 32);
}

DE_HANDLE(de_component); /* component type id within a world */
DE_HANDLE(de_resource_id);
DE_HANDLE(de_event_type);
DE_HANDLE(de_system);
DE_HANDLE(de_query);

/** Default table chunk size of de_world_desc.chunk_bytes, in bytes (16 KiB). */
#define DE_ECS_DEFAULT_CHUNK_BYTES (16u * 1024u)
/** Smallest accepted de_world_desc.chunk_bytes. */
#define DE_ECS_MIN_CHUNK_BYTES 1024u
/** Largest component alignment the tables support (chunks are aligned to it). */
#define DE_ECS_MAX_COMPONENT_ALIGN 64u

/**
 * de_world_create() parameters.
 *
 * - `allocator`: every allocation of the world (records, archetypes, chunk pages). NULL means
 *   de_heap_allocator(). Must outlive the world.
 * - `types`: the type registry components are registered from. Required. Must outlive the world.
 * - `tasks`: the task system schedules run on (DENG-350). NULL is allowed for single-threaded use.
 * - `initial_entity_capacity`: entity records reserved up front (0 = none).
 * - `chunk_bytes`: bytes per table chunk. 0 = DE_ECS_DEFAULT_CHUNK_BYTES. Rounded up to a
 *   multiple of DE_ECS_MAX_COMPONENT_ALIGN; at least DE_ECS_MIN_CHUNK_BYTES.
 */
typedef struct de_world_desc {
    const de_allocator *allocator;
    de_type_registry *types;
    de_task_system *tasks;
    u32 initial_entity_capacity;
    u32 chunk_bytes;
} de_world_desc;

/**
 * Creates a world with the empty archetype (no components) in it.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (`desc`, `desc->types` or `out` NULL, or `chunk_bytes`
 *         too large) or DE_ERR_OUT_OF_MEMORY.
 */
DE_API de_result de_world_create(const de_world_desc *desc, de_world **out);
/**
 * Destructs every component of every entity (destruct hooks run in archetype, chunk and row
 * order) and frees all memory of the world. NULL is ignored.
 */
DE_API void de_world_destroy(de_world *w);
/** The world's tick counter: the simulation tick. 0 after create; each run of
 * DE_SCHEDULE_FIXED_UPDATE moves it on by one at its end (DENG-350). */
DE_API u64 de_world_tick(const de_world *w);
/** The type registry the world was created with (de_world_desc.types), or NULL for a NULL world.
 * Modules use it to register the struct types of the resources and events they insert (DENG-308).
 */
DE_API de_type_registry *de_world_types(const de_world *w);
/** Borrowed task system from de_world_desc.tasks; NULL for a serial or NULL world.
 * The binding is immutable for the world's lifetime. */
DE_API de_task_system *de_world_task_system(const de_world *w);

/* ---------- Components ---------- */
typedef enum de_component_flags {
    DE_COMP_SNAPSHOT = 1u << 0, /* included in world snapshots (rollback, replays) */
    DE_COMP_REPLICATE = 1u << 1,
    DE_COMP_SERIALIZE = 1u << 2,
    DE_COMP_TAG = 1u << 3 /* zero-size */
} de_component_flags;

/**
 * Registers struct type `type` of the world's type registry as a component and returns its id.
 * Ids are dense, in registration order (1, 2, 3, ...).
 *
 * The size, alignment and hooks are copied from the type's description. The type must be POD
 * (`is_pod`) or have both a copy and a destruct hook (SPEC §7.1). Component data is moved
 * between tables with memcpy, so every component must be trivially relocatable. With
 * DE_COMP_TAG the component has no data and no column, whatever the type's size.
 *
 * Registering a type again returns the same id when the flags are equal.
 *
 * On failure returns an invalid id and sets the error context: DE_ERR_INVALID_ARGUMENT (bad
 * world, unknown type, unknown flag bits, a type that is neither POD nor has copy and destruct
 * hooks), DE_ERR_ALREADY_EXISTS (registered before with other flags), DE_ERR_UNSUPPORTED
 * (alignment above DE_ECS_MAX_COMPONENT_ALIGN, or too large for one row of a chunk),
 * DE_ERR_OUT_OF_MEMORY. Not allowed while a schedule runs.
 */
DE_API de_component de_component_register(de_world *w, de_type_id type, u32 flags);
/** The component whose type is called `type_name`, or an invalid id. Sets no error. */
DE_API de_component de_component_find(const de_world *w, const char *type_name);

/* ---------- Entities (immediate; not allowed inside running systems — use commands) ---------- */
/**
 * Creates an entity with no components (in the empty archetype). Reuses the most recently freed
 * index, or takes the next new one. Returns DE_ENTITY_NULL (and sets the error context) when
 * memory runs out or every index is used.
 */
DE_API de_entity de_entity_spawn(de_world *w);
/**
 * Destroys entity `e`: runs the destruct hook of each of its components, removes its row (the
 * table's last row moves into the hole) and bumps the index's generation, so `e` and every copy
 * of it are no longer alive. The index is reused by a later spawn (last freed, first reused) with
 * the new generation. A dead or never-spawned `e` is ignored (a warning in Debug and Dev).
 * Immediate: not allowed inside a running system (use de_cmd_despawn).
 */
DE_API void de_entity_despawn(de_world *w, de_entity e);
/** true if `e` was spawned in this world and not despawned since (its generation matches). */
DE_API bool de_entity_alive(const de_world *w, de_entity e);
/**
 * Adds component `c` to `e` and returns its data: zeroed, or initialised by the type's construct
 * hook. Its added and changed ticks are the current change tick. If `e` already has `c`, nothing
 * moves and the existing data is returned and marked changed (not added). Otherwise `e`
 * moves to the archetype with `c` added (cached per archetype after the first move). For a tag
 * the result is a non-NULL placeholder that must not be written through. Like every pointer into
 * a table, the result is valid until the next structural change of the world.
 * Returns NULL (and sets the error context) if `e` is not alive, `c` is not registered, or memory
 * runs out; the world is then unchanged. Immediate: not allowed inside a running system (use
 * de_cmd_add).
 */
DE_API void *de_entity_add(de_world *w, de_entity e, de_component c); /* returns zeroed data */
/**
 * Removes component `c` from `e`, running its destruct hook, and moves `e` to the archetype
 * without `c`. Does nothing if `e` is not alive or has no `c`. Immediate: not allowed inside a
 * running system (use de_cmd_remove).
 */
DE_API void de_entity_remove(de_world *w, de_entity e, de_component c);
/**
 * Spawns an entity directly in the archetype of the components `comps[0..n)` (any order;
 * duplicates count once), with every component zeroed or constructed by its hook. Faster than a
 * spawn followed by n adds: one row, no intermediate archetypes. Fill the data with
 * de_entity_get(). `n` = 0 is a plain spawn.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (`w` or `out` NULL, `comps` NULL with n > 0, or a
 *         component that is not registered), DE_ERR_UNSUPPORTED (the row does not fit a chunk)
 *         or DE_ERR_OUT_OF_MEMORY. On failure `*out` is DE_ENTITY_NULL and the world unchanged.
 * Immediate: not allowed inside a running system.
 */
DE_API de_result de_entity_spawn_with(de_world *w, const de_component *comps, u32 n,
                                      de_entity *out);
/**
 * The data of component `c` of entity `e`, or NULL if `e` is not alive or has no `c`. For a tag
 * component the pointer is a non-NULL placeholder that must not be written through. The pointer
 * is valid until the next structural change of the world.
 * A mutable get marks the component changed (DE_CHANGED), written or not; read through
 * de_entity_get_const() to leave the change tick alone. Like a write, it must not run at the same
 * time as another access to the same component of the same entity.
 */
DE_API void *de_entity_get(de_world *w, de_entity e, de_component c); /* NULL if absent */
/** Like de_entity_get(), read-only. Does not mark the component changed. */
DE_API const void *de_entity_get_const(const de_world *w, de_entity e, de_component c);
/** true if `e` is alive and has component `c` (tags included). */
DE_API bool de_entity_has(const de_world *w, de_entity e, de_component c);
/**
 * Marks component `c` of `e` changed: its changed tick becomes the world's current change tick,
 * so DE_CHANGED filters report it once to every system on that system's next run (DENG-347).
 * Does nothing if `e` is not alive, has no `c`, or `c` is a tag (tags have no change ticks).
 * Allocates nothing. Thread rules: those of a write to that component.
 */
DE_API void de_entity_mark_changed(de_world *w, de_entity e, de_component c);

/* Hierarchy (ChildOf / Children) and transform propagation: see "Hierarchy and built-in
 * components" at the end of this header (DENG-351). */

/* ---------- Resources (typed singletons) ---------- */
/*
 * A resource is one value of a reflected struct type stored in the world (time, input state, the
 * asset system pointer, the physics world, ...; SPEC §7.1). There is at most one resource per
 * type. Each type gets its id when it is first inserted, and keeps it for the life of the world:
 * removing the value and inserting it again gives the same id, and an id never names another
 * type. Ids are dense, in first-insert order.
 *
 * Each value lives in its own block aligned to the type, so the pointer from de_resource_get()
 * stays valid until the resource is removed or the world is destroyed; replacing the value keeps
 * the same block.
 *
 * Threading: insert and remove are single-threaded; nothing else may touch resources while they
 * run (inside a schedule, only an exclusive system may call them). de_resource_get() and
 * de_resource_find() may run on many threads at once. Systems declare which resources they read
 * or write (de_system_desc.resources), and the scheduler keeps writers apart (DENG-350).
 */
/**
 * Stores a resource of struct type `type` (from the world's type registry) and returns its id.
 *
 * The value is a copy of `init` (the type's copy hook, or memcpy for a POD type), or, when `init`
 * is NULL, a new value (the construct hook, or zero). If the world already has a resource of
 * this type, the old value is destructed (destruct hook) and replaced in place, and the id stays
 * the same. Passing the resource's own current value as `init` changes nothing.
 *
 * On failure returns an invalid id and sets the error context: DE_ERR_INVALID_ARGUMENT (bad
 * world, unknown type, or `init` given for a type that is neither POD nor has a copy hook),
 * DE_ERR_UNSUPPORTED (alignment above DE_ECS_MAX_COMPONENT_ALIGN) or DE_ERR_OUT_OF_MEMORY. A
 * failed replace leaves the old value in place.
 */
DE_API de_resource_id de_resource_insert(de_world *w, de_type_id type, const void *init);
/** The value of resource `id`, or NULL if `id` is invalid or the resource was removed. */
DE_API void *de_resource_get(de_world *w, de_resource_id id);
/** The id of the resource of type `type`, or an invalid id if the world has none (never
 * inserted, or removed). Sets no error. */
DE_API de_resource_id de_resource_find(const de_world *w, de_type_id type);
/**
 * Destructs (destruct hook) and frees the value of resource `id`. de_resource_get() then returns
 * NULL until the type is inserted again, which gives back the same id. Ignores invalid or
 * already removed ids.
 */
DE_API void de_resource_remove(de_world *w, de_resource_id id);

/* ---------- Events (double-buffered; readable for two frames) ---------- */
/*
 * An event type is a queue of payloads of one reflected struct type (SPEC §7.1). Each type keeps
 * two buffers: events sent this frame and events sent last frame. de_world_update_events() runs
 * once per frame (at the start of DE_SCHEDULE_FIRST, DENG-350): it drops last frame's events and
 * starts a new frame. So an event can be read during the frame it was sent and the next one.
 *
 * Every event of a type has a sequence number (0, 1, 2, ... in send order). A reader is plain
 * data: the sequence number of the next event it reads, so it can live in a system's user data or
 * a snapshot. Each reader sees each event at most once, in send order (SPEC §5.7). A reader that
 * is not polled for two frames misses events; de_event_next() then skips to the oldest event
 * still alive and adds the dropped ones to `missed`.
 *
 * Payloads are copied into the buffer (the type's copy hook, or memcpy for a POD type) and moved
 * with memcpy when a buffer grows, so payload types are POD, or trivially relocatable with copy
 * and destruct hooks. The destruct hook runs when an event is dropped. Buffers keep their
 * capacity, so steady-state sending does not allocate.
 *
 * Threading: de_event_send() is not thread-safe for one event type: one sender at a time, and no
 * reader at the same time (the scheduler orders writers by access sets, DENG-350; systems that
 * run in parallel send through de_cmd_send_event()). Different event types may be sent from
 * different threads at once. Readers of a type may run at the same time as each other.
 * de_event_register() and de_world_update_events() are single-threaded.
 */
typedef struct de_event_reader {
    u64 cursor; /**< sequence number of the next event to read */
    de_event_type type;
    u32 missed; /**< events dropped before this reader read them (saturates) */
} de_event_reader;
/**
 * Registers struct type `payload` of the world's type registry as an event type. Ids are dense,
 * in registration order (1, 2, 3, ...). Registering a type again returns the same id.
 *
 * On failure returns an invalid id and sets the error context: DE_ERR_INVALID_ARGUMENT (bad
 * world, unknown type, or a type that is neither POD nor has copy and destruct hooks),
 * DE_ERR_UNSUPPORTED (alignment above DE_ECS_MAX_COMPONENT_ALIGN) or DE_ERR_OUT_OF_MEMORY.
 */
DE_API de_event_type de_event_register(de_world *w, de_type_id payload);
/**
 * Appends a copy of `*payload` (the event type's payload size) to this frame's events of `t`. If
 * the buffer cannot grow, the event is dropped and the error context is set
 * (DE_ERR_OUT_OF_MEMORY); an invalid `t` or NULL `payload` sets DE_ERR_INVALID_ARGUMENT.
 */
DE_API void de_event_send(de_world *w, de_event_type t, const void *payload);
/**
 * A reader of `t` that starts at the oldest event still alive (sent this frame or last frame).
 * For an invalid `t` the reader's type is invalid and de_event_next() returns NULL.
 */
DE_API de_event_reader de_event_reader_create(de_world *w, de_event_type t);
/**
 * The next unread event payload of `r->type`, or NULL when the reader has read every event. The
 * pointer is valid until the next de_event_send() of that type or de_world_update_events().
 * Events dropped before the reader got to them are skipped and counted in `r->missed`.
 */
DE_API const void *de_event_next(de_world *w, de_event_reader *r); /* NULL when drained */
/** Events `r` missed (dropped before it read them) since it was created. Saturates at
 * 0xFFFFFFFF. */
DE_API u32 de_event_reader_missed(const de_event_reader *r);
/**
 * Starts a new event frame for every event type, in registration order: drops the events sent
 * the frame before last (destruct hooks run) and makes this frame's events last frame's. Runs at
 * the start of DE_SCHEDULE_FIRST (DENG-350). Allocates nothing.
 */
DE_API void de_world_update_events(de_world *w);

/* ---------- Queries ---------- */
/*
 * Queries (SPEC §7.2, DENG-346).
 *
 * A query is declared with terms and filters:
 * - Terms are the components the callback gets data for: DE_READ / DE_WRITE (required), or
 *   DE_OPTIONAL_READ / DE_OPTIONAL_WRITE (the entity may lack it; its column is then NULL).
 * - Filters only select archetypes: DE_WITH (must have), DE_WITHOUT (must not have), DE_ADDED and
 *   DE_CHANGED (must have, plus a per-row test of the component's change ticks; DENG-347).
 * An archetype matches when it has every required term and every WITH / ADDED / CHANGED
 * component, and no WITHOUT component.
 *
 * Caching: de_query_create() matches the existing archetypes once. After that, each new archetype
 * is tested once against every live query when it is created, so keeping a query up to date
 * costs O(new archetypes), never a rescan.
 *
 * Iteration order (SPEC §5.7 rule 5): matched archetypes in archetype creation order, then each
 * table's chunks in order, then rows in order. Empty archetypes are skipped. The order depends
 * only on the sequence of world operations, never on threads, timing or hash maps.
 *
 * Chunks: the callback gets contiguous slices of one table chunk (de_query_chunk): `count` rows,
 * their entity ids, and per term a pointer to the first row's data of that column (SoA, aligned
 * to the component's alignment), so a loop over `count` elements is SIMD friendly. Writing
 * through a DE_WRITE column changes the component data seen by de_entity_get(). A tag term gets a
 * non-NULL placeholder that must not be read or written; an absent optional term gets NULL.
 * With ADDED / CHANGED filters a table chunk is delivered as its maximal runs of passing rows,
 * each still one contiguous slice.
 *
 * Change detection (SPEC §7.1, DENG-347): every row of every data column stores an `added` and a
 * `changed` tick (u32 counters, never time). Adding a component sets both to the world's current
 * change tick. The component is marked changed by de_entity_mark_changed(), a mutable
 * de_entity_get(), an add of a component the entity already has (also a deferred one), and by
 * iteration: every row a query hands out is marked changed in each DE_WRITE and
 * DE_OPTIONAL_WRITE column, whether the callback wrote it or not (conservative, as in Bevy).
 * Read terms, de_entity_get_const() and tags never mark. Inside a system, DE_ADDED / DE_CHANGED
 * pass the rows whose tick is newer than the system's last run and not newer than this run,
 * compared modulo 2^32 (the schedule sets the window, DENG-350); ticks older than 2^30 are
 * clamped, so a row untouched that long is never reported. Outside a system a query has no
 * window yet, and ADDED / CHANGED pass every row (as on a system's first run).
 *
 * Rules: the callback must not make structural changes (spawn, despawn, add, remove, register;
 * inside systems use commands) and must not destroy the query. Pointers in a chunk are valid
 * during the callback only.
 *
 * Threading: de_query_each() and de_query_count() only read the query and the world (apart from
 * the changed ticks of the rows they hand out to write terms), so they may run on many threads at
 * once while nothing changes the world's structure. de_query_par_each() marks each range's rows on
 * the thread that runs the range. de_query_par_each()
 * writes per-query scratch: at most one par_each per query at a time. create and destroy are
 * single-threaded, like structural changes.
 */
typedef enum de_access { DE_READ, DE_WRITE, DE_OPTIONAL_READ, DE_OPTIONAL_WRITE } de_access;
typedef enum de_filter { DE_WITH, DE_WITHOUT, DE_ADDED, DE_CHANGED } de_filter;

typedef struct de_query_term {
    de_component component;
    de_access access;
} de_query_term;
typedef struct de_query_filter {
    de_component component;
    de_filter filter;
} de_query_filter;

/** de_query_create() parameters. Both arrays are copied. */
typedef struct de_query_desc {
    const de_query_term *terms; /**< at most DE_QUERY_MAX_TERMS; columns[i] matches terms[i] */
    u32 term_count;
    const de_query_filter *filters; /**< at most DE_QUERY_MAX_FILTERS */
    u32 filter_count;
} de_query_desc;

/** Most terms of one query. */
#define DE_QUERY_MAX_TERMS 32u
/** Most filters of one query. */
#define DE_QUERY_MAX_FILTERS 32u
/** de_query_par_each() rows per range when `grain_rows` is 0. A fixed number on every machine. */
#define DE_QUERY_DEFAULT_GRAIN_ROWS 1024u

/**
 * A contiguous run of rows in one archetype table chunk. `columns[i]` matches the query's
 * terms[i]: the data of row 0 of the slice, NULL for an absent optional term, a placeholder for a
 * tag term.
 *
 * `first_row` is the position of the slice's first row in the query's row order: the rows of the
 * matched archetypes, concatenated in iteration order, numbered from 0 (rows skipped by ADDED /
 * CHANGED filters keep their numbers). `range` is the de_query_par_each() range the slice belongs
 * to (first_row / grain_rows), and 0 in de_query_each(). Per-range partial results indexed by
 * `range` and reduced in range order are the same for every worker count.
 */
typedef struct de_query_chunk {
    u32 count;
    const de_entity *entities;
    void **columns;
    u32 range;
    u32 first_row;
    /** (DENG-350) Inside a system: the command buffer for this slice. In de_query_par_each each
     * range has its own buffer (key (system order, range + 1)), so ranges record in parallel and
     * still apply in a fixed order; in de_query_each it is the system's own buffer
     * (de_system_ctx.commands). Only the thread running the slice records into it. NULL outside
     * systems (and if memory ran out). */
    struct de_commands *commands;
} de_query_chunk;

typedef void (*de_chunk_fn)(const de_query_chunk *chunk, void *user);

/**
 * Creates a query: copies the terms and filters, checks them and matches the existing
 * archetypes. Returns an invalid handle and sets the error context on failure:
 * DE_ERR_INVALID_ARGUMENT (`w` or `desc` NULL, a NULL array with a non-zero count, more than
 * DE_QUERY_MAX_TERMS terms or DE_QUERY_MAX_FILTERS filters, an unregistered component, an
 * unknown access or filter value, a component in two terms, or a DE_WITHOUT component that is
 * also a term or another filter's component), DE_ERR_UNSUPPORTED (DE_ADDED / DE_CHANGED on a tag:
 * tags have no change ticks) or DE_ERR_OUT_OF_MEMORY. A query with no terms and no filters
 * matches every archetype. Not allowed while a schedule runs (systems get their queries from
 * their desc, DENG-350).
 */
DE_API de_query de_query_create(de_world *w, const de_query_desc *desc);
/**
 * Destroys a query. Invalid or stale handles are ignored. Every query is destroyed with its
 * world. Not allowed during an iteration of that query.
 */
DE_API void de_query_destroy(de_world *w, de_query q);
/**
 * Calls `fn(chunk, user)` for every non-empty slice of the query, in iteration order (above), on
 * the calling thread. An invalid handle does nothing. Allocates nothing.
 */
DE_API void de_query_each(de_world *w, de_query q, de_chunk_fn fn, void *user);
/**
 * Parallel de_query_each() with a deterministic split. The query's rows, numbered in iteration
 * order (de_query_chunk.first_row), are cut into ranges [k * grain_rows, (k + 1) * grain_rows);
 * `grain_rows` 0 means DE_QUERY_DEFAULT_GRAIN_ROWS. Each range runs as one de_task_parallel_for
 * chunk on the world's task system (serially on the calling thread, in range order, when the
 * world has none) and is delivered as one or more slices, with `chunk->range` = k. Which rows
 * form a range depends only on `grain_rows` and the world's data, never on the worker count.
 * Returns when every range is done. Allocates nothing.
 */
DE_API void de_query_par_each(de_world *w, de_query q, u32 grain_rows, de_chunk_fn fn, void *user);
/**
 * The number of rows (entities) in the query's matched archetypes. ADDED / CHANGED filters are
 * not applied: the count is the size of the row space that de_query_par_each() splits. 0 for an
 * invalid handle.
 */
DE_API u32 de_query_count(de_world *w, de_query q);

/* ---------- Commands (deferred structural changes) ---------- */
/*
 * Commands (SPEC §7.1, DENG-349). Code that may not change the world's structure right away (a
 * running system, a parallel range, a hook) records spawn, despawn, add, remove and send-event
 * commands into a command buffer. The buffers are applied at a sync point.
 *
 * Apply order is deterministic (SPEC §5.7 rule 5): first the main-thread buffer
 * (de_world_commands), then every other buffer sorted by (system_order, range_index), the key it
 * was acquired with (DENG-350 gives systems and query ranges their buffers). Inside a buffer,
 * commands apply in record order. Thread ids, timing and the order in which buffers were acquired
 * or filled never matter. Each command applies through the same code as the immediate API, so a
 * sequence of commands gives the same world as the same immediate calls in apply order.
 *
 * Pending entities. de_cmd_spawn() cannot hand out a real id: real ids depend on the order of
 * spawns and despawns, which is only known at apply. It returns a *pending* id: generation 0 (never
 * a live entity) with the buffer slot and the spawn's index in that buffer in the low 32 bits.
 * A pending id may be the target of later commands in any buffer of the same apply, and may be
 * stored in a DE_FK_ENTITY field of a command payload (also inside nested structs and fixed
 * arrays): apply allocates the real entity in apply order and replaces the pending id with it. A
 * pending id is a placeholder: its bit pattern may differ between runs and must not be used for
 * anything but these references. Immediate functions do not accept pending ids (asserted in Debug
 * and Dev; de_entity_alive() returns false). After the apply, de_commands_resolve() gives the real
 * id, until the next apply.
 *
 * Dropped commands. A command whose target is not alive when it applies (despawned earlier, or a
 * pending id that is not spawned yet in apply order) is dropped, with a warning in Debug and Dev
 * (log category `ecs`). A de_cmd_despawn() of a pending id cancels that spawn: the entity is never
 * created, every command on it is dropped without a warning, and payload fields that refer to it
 * become DE_ENTITY_NULL.
 *
 * Payloads. de_cmd_add() returns storage for the component inside the buffer; de_cmd_send_event()
 * copies the event. Apply moves component payloads into the table (memcpy; components are
 * trivially relocatable) and copies event payloads with de_event_send(). Payloads of commands
 * that are dropped or never applied are destructed (destruct hook).
 *
 * Memory: a buffer keeps its pages, and the world its bookkeeping, after an apply, so recording
 * and applying the same amount of commands again allocates nothing (steady state).
 *
 * Threading: one thread at a time records into one buffer. Different buffers may record on
 * different threads at once, while nothing changes the world's structure. Apply is
 * single-threaded and nothing may record while it runs.
 */
typedef struct de_commands de_commands;

/** The kind of a recorded command (de_commands_iterate). DENG-351 appends hierarchy ops. */
typedef enum de_command_op {
    DE_CMD_SPAWN = 1,
    DE_CMD_DESPAWN = 2,
    DE_CMD_ADD = 3,
    DE_CMD_REMOVE = 4,
    DE_CMD_SEND_EVENT = 5,
    /* DENG-351: hierarchy ops. */
    DE_CMD_SET_PARENT = 6, /**< payload: the new parent (de_entity; DE_ENTITY_NULL detaches) */
    DE_CMD_DESPAWN_RECURSIVE = 7 /**< no payload */
} de_command_op;

/** true if `e` is a pending id from de_cmd_spawn() (generation 0 and not DE_ENTITY_NULL). */
static inline bool de_entity_is_pending(de_entity e) {
    return (e.bits >> 32) == 0 && e.bits != 0;
}

/**
 * Records a spawn of an entity with no components and returns its **pending** id (see above):
 * the real id is allocated at apply, in apply order. Use the pending id as the target of later
 * commands, or in DE_FK_ENTITY payload fields; de_commands_resolve() gives the real id after the
 * apply. Returns DE_ENTITY_NULL (and sets the error context) if `c` is NULL, the buffer already
 * has DE_CMD_MAX_SPAWNS spawns since its last apply, or memory runs out.
 */
DE_API de_entity de_cmd_spawn(de_commands *c); /* pending id; resolved at apply */
/** Records a despawn of `e` (real or pending). A pending `e` cancels its spawn (see above). */
DE_API void de_cmd_despawn(de_commands *c, de_entity e);
/**
 * Records an add of component `comp` to `e` (real or pending) and returns the component's
 * payload storage inside the buffer: zeroed, or initialised by the type's construct hook (for a
 * tag, a non-NULL placeholder that must not be written through, as de_entity_add() gives). Fill
 * it before the apply; the pointer stays valid until the buffer is applied. At apply the payload
 * is moved into the entity's row; if `e` already has `comp`, its old value is destructed and
 * replaced (and marked changed, not added). Returns NULL and sets the error context if `c` is NULL,
 * `comp` is not registered or memory runs out; nothing is recorded then.
 */
DE_API void *de_cmd_add(de_commands *c, de_entity e, de_component comp);
/** Records a remove of component `comp` from `e` (real or pending). */
DE_API void de_cmd_remove(de_commands *c, de_entity e, de_component comp);
/**
 * Records a send of event `t` with a copy of `*payload` (the type's copy hook, or memcpy). At
 * apply the event is sent with de_event_send(), so readers see deferred events in apply order.
 * An invalid `t`, a NULL `payload` or out of memory records nothing and sets the error context.
 */
DE_API void de_cmd_send_event(de_commands *c, de_event_type t, const void *payload);

/** Largest number of de_cmd_spawn() calls on one buffer between two applies (2^20). */
#define DE_CMD_MAX_SPAWNS (1u << 20)

/**
 * The world's main-thread command buffer, for code outside systems. It is applied first by every
 * apply (de_world_apply_commands, and the sync points of schedules, DENG-350). Created on first
 * use; NULL (error context set) if `w` is NULL or memory runs out. Only the main thread records
 * into it.
 */
DE_API de_commands *de_world_commands(de_world *w);
/**
 * Applies every recorded buffer of `w` (the main-thread buffer, then all others by
 * (system_order, range_index)) and empties them. Single-threaded; not allowed while a schedule
 * runs (asserted in Debug and Dev; schedules apply at their own sync points).
 */
DE_API void de_world_apply_commands(de_world *w);
/**
 * The real entity that pending id `pending` became at the last apply, or DE_ENTITY_NULL if it
 * was cancelled, not applied (yet), or comes from an earlier apply. Valid until the next apply
 * starts. A non-pending `pending` is returned unchanged.
 */
DE_API de_entity de_commands_resolve(const de_world *w, de_entity pending);

/**
 * One recorded command, as de_commands_iterate() shows it. `entity` is the target (the pending id
 * for DE_CMD_SPAWN, DE_ENTITY_NULL for DE_CMD_SEND_EVENT). `payload` points into the buffer
 * (`payload_size` bytes; NULL and 0 for commands without one) and is valid until the apply.
 */
typedef struct de_command {
    de_command_op op;
    u32 payload_size;
    de_entity entity;
    de_component component; /**< DE_CMD_ADD, DE_CMD_REMOVE */
    de_event_type event;    /**< DE_CMD_SEND_EVENT */
    const void *payload;
} de_command;

/** Called once per command, in record order. Return false to stop. */
typedef bool (*de_command_visit_fn)(const de_command *cmd, void *user);
/**
 * Calls `fn` for each command recorded in `c` since its last apply, in record order (the order
 * they will apply in). Read-only: the editor's undo/redo (S38) inspects buffers through it.
 */
DE_API void de_commands_iterate(const de_commands *c, de_command_visit_fn fn, void *user);
/** Commands recorded in `c` since its last apply (0 for NULL). */
DE_API u32 de_commands_count(const de_commands *c);

/* ---------- Systems and schedules ---------- */
/*
 * Systems and schedules (SPEC §7.3, §8, DENG-350).
 *
 * A system is a function plus its declared access: the components of its queries (DE_READ /
 * DE_OPTIONAL_READ and DE_ADDED / DE_CHANGED filters read, DE_WRITE / DE_OPTIONAL_WRITE write;
 * DE_WITH / DE_WITHOUT are not access), the resources it reads or writes, and the event types it
 * reads or sends. A DE_SYSTEM_EXCLUSIVE system has full access to the world and conflicts with
 * every other system.
 *
 * Schedules run in the fixed order of de_schedule, driven by the host's main loop (SPEC §10.2):
 * de_world_run_schedule() runs one. Inside a schedule, two systems conflict when both access the
 * same component, resource or event type and at least one writes it. The schedule's order is a
 * DAG: first the explicit `before` / `after` edges (by system name), then, for every conflicting
 * pair that those do not already order, an edge from the system registered first to the other.
 * Non-conflicting systems run in parallel on the world's task system (one compiled de_task_graph
 * per schedule, reused until a system of that schedule is added or removed). Conflicting systems
 * never overlap, and the order does not depend on the worker count.
 *
 * A run of a schedule:
 * 1. DE_SCHEDULE_FIRST only: de_world_update_events() (the event frame step).
 * 2. For every enabled system, single-threaded, in the schedule's order (a topological order of
 *    the DAG, ties broken by registration order): it takes a change tick, its queries get the
 *    change window (its last run, this run], and it gets a command buffer.
 * 3. The systems run (in parallel where the DAG allows; serially in order when the world has no
 *    task system). DE_SYSTEM_MAIN_THREAD systems run on the main thread only.
 * 4. Sync point: every command buffer is applied (de_world_apply_commands order: the main-thread
 *    buffer, then each system's buffers in schedule order, each system's own buffer before its
 *    par_each range buffers in range order). Entities spawned by commands get the same ids for
 *    every worker count.
 * 5. DE_SCHEDULE_FIXED_UPDATE only: the world tick (de_world_tick) moves on by one.
 *
 * Inside a system: no immediate structural changes (spawn, despawn, add, remove, register,
 * de_query_create): record commands into ctx->commands, or chunk->commands inside a query
 * callback. Exclusive systems are the exception: nothing else runs beside them, so they may use
 * the immediate API. A mutable de_entity_get() or de_entity_mark_changed() inside a system stamps
 * the system's own run tick, like writes through its query terms, so a system never sees its own
 * changes on its next run.
 *
 * Debug and Dev builds check every system (a DE_PANIC naming the system on a violation):
 * de_entity_get() / de_entity_get_const(), query iteration, de_resource_get() and the event
 * functions must stay inside the declared access (a write declaration allows reads too).
 * DE_SYSTEM_DETERMINISTIC systems must run with the deterministic FP environment
 * (de_fp_env_check()) and must not iterate a hash map (SPEC §5.7 rules 4 and 5); registering one
 * outside DE_SCHEDULE_FIXED_UPDATE or DE_SCHEDULE_STARTUP logs a warning.
 *
 * Threading: de_system_add, de_system_remove and de_world_run_schedule are single-threaded and
 * not allowed while a schedule runs. Run schedules that have main-thread systems from the task
 * system's main thread (frame worker 0), which drains main-thread work while it waits.
 */
typedef enum de_schedule {
    DE_SCHEDULE_STARTUP,
    DE_SCHEDULE_FIRST,
    DE_SCHEDULE_PRE_UPDATE,
    DE_SCHEDULE_FIXED_UPDATE,
    DE_SCHEDULE_UPDATE,
    DE_SCHEDULE_POST_UPDATE,
    DE_SCHEDULE_EXTRACT,
    DE_SCHEDULE_LAST,
    DE_SCHEDULE_COUNT_
} de_schedule;

/** What a system function gets. Valid during the call only. */
typedef struct de_system_ctx {
    de_world *world;
    /** The system's own command buffer, applied at the end of the schedule (NULL if memory ran
     * out). Only the system's own thread records into it; query ranges use chunk->commands. */
    de_commands *commands;
    const de_query *queries; /**< created from de_system_desc.queries, in declared order */
    f32 dt;                  /**< de_world_run_schedule's `dt` (fixed dt in FIXED_UPDATE) */
    f32 alpha;               /**< de_world_run_schedule's `alpha` (fixed-step interpolation) */
    u64 tick;                /**< de_world_tick() when the schedule started */
    void *user;              /**< de_system_desc.user */
} de_system_ctx;

typedef void (*de_system_fn)(de_system_ctx *ctx);

typedef enum de_system_flags {
    DE_SYSTEM_MAIN_THREAD = 1u << 0,
    DE_SYSTEM_DETERMINISTIC = 1u << 1, /* checked against SPEC §5.7 rules in Debug */
    DE_SYSTEM_EXCLUSIVE = 1u << 2      /* full world access; runs alone */
} de_system_flags;

typedef struct de_resource_access {
    de_resource_id id;
    de_access access;
} de_resource_access;

/**
 * de_system_add() parameters. Everything is copied; the pointers need not outlive the call.
 *
 * - `name`: unique in the world (NULL: an unnamed system, which before / after cannot name).
 * - `fn` (required), `user`, `flags` (de_system_flags), `schedule`.
 * - `queries`: created with de_query_create(); ctx->queries[i] is queries[i].
 * - `resources`: resource ids with DE_READ or DE_WRITE (the optional forms count as the same).
 * - `reads_events` / `writes_events`: event types the system reads (de_event_next) or sends
 *   (de_event_send). de_cmd_send_event() needs no declaration: it applies at the sync point.
 * - `before` / `after`: names of systems of the same schedule this one runs before / after. A
 *   name no system of the schedule has is ignored, with a warning when the schedule is built.
 */
typedef struct de_system_desc {
    const char *name;
    de_schedule schedule;
    de_system_fn fn;
    void *user;
    u32 flags;
    const de_query_desc *queries;
    u32 query_count;
    const de_resource_access *resources;
    u32 resource_count;
    const de_event_type *reads_events;
    u32 reads_event_count;
    const de_event_type *writes_events;
    u32 writes_event_count;
    const char *const *before;
    u32 before_count; /* system names */
    const char *const *after;
    u32 after_count;
} de_system_desc;

/**
 * Adds a system to its schedule, after every system added before it (registration order).
 * Creates its queries and computes its access set; the schedule's graph is rebuilt on its next
 * run. Returns an invalid handle and sets the error context on failure: DE_ERR_INVALID_ARGUMENT
 * (`w` or `desc` NULL, no `fn`, an unknown schedule or flag, a NULL array with a non-zero count,
 * an invalid resource id, event type or access value, a NULL before / after name, a query desc
 * de_query_create() refuses, or before / after edges that form a cycle: the message names the
 * systems, `"a -> b -> a"`), DE_ERR_ALREADY_EXISTS (the name is taken) or DE_ERR_OUT_OF_MEMORY.
 * Not allowed while a schedule runs.
 */
DE_API de_system de_system_add(de_world *w, const de_system_desc *desc);
/** Removes a system and destroys its queries. Invalid or stale handles are ignored. Not allowed
 * while a schedule runs. */
DE_API void de_system_remove(de_world *w, de_system s);
/**
 * Enables or disables a system (enabled after add). A disabled system is skipped: it takes no
 * change tick, so on its next enabled run its change filters report everything since its last
 * run. The schedule's graph does not change. Takes effect at the next run of its schedule.
 */
DE_API void de_system_set_enabled(de_world *w, de_system s, bool enabled);
/** true if `s` is a live system of `w` and enabled. */
DE_API bool de_system_enabled(const de_world *w, de_system s);
/**
 * Runs schedule `s` once (see "A run of a schedule" above): builds its task graph if a system
 * was added or removed since the last run, otherwise reuses it, runs the systems, then applies
 * every command buffer. Allocates nothing once the graph is built and the command buffers have
 * warmed up.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (`w` NULL or an unknown schedule), DE_ERR_BUSY (called
 *         while a schedule of `w` runs, for example from a system) or DE_ERR_OUT_OF_MEMORY
 *         (building the graph; nothing ran).
 */
DE_API de_result de_world_run_schedule(de_world *w, de_schedule s, f32 dt, f32 alpha);
/**
 * Timings of system `s` (SPEC §18.4 inspector), from the schedule's task graph node
 * (de_task_graph_node_stats) or, without a task system, measured the same way. Reset when the
 * schedule's graph is rebuilt.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (`w` or `out` NULL, `s` not a live system of `w`) or
 *         DE_ERR_UNSUPPORTED (Release and Shipping builds, which do not measure; `*out` zeroed).
 */
DE_API de_result de_system_get_stats(const de_world *w, de_system s, de_task_node_stats *out);

/* ---------- Snapshots ---------- */
/*
 * World snapshots and the world checksum (SPEC §7.4, §5.7 rule 7, DENG-352).
 *
 * A snapshot holds the world tick, the entity allocator (every index's generation, which indices
 * are alive, the LIFO free list) and, per non-empty archetype with at least one DE_COMP_SNAPSHOT
 * component, its entity ids and one contiguous block per snapshot column (rows in table order).
 * It is one block from the caller's allocator. It holds nothing else: no component without
 * DE_COMP_SNAPSHOT, no resources, no events, no event reader state, no change ticks, no queries
 * or commands. Rollback netcode (S33), replays, play-in-editor (S38) and the determinism tests
 * use it.
 *
 * Snapshot components are copied as bytes, so they must be POD (`is_pod`; SPEC §7.1): take
 * refuses a snapshot component whose type is not.
 *
 * A snapshot is an in-memory format for this process: restore it into the world it was taken
 * from, or into a world with the same component registration (same ids, flags and sizes). It is
 * not a file format (save games are S37).
 *
 * Threading: take, size and checksum only read the world (no other thread may change it while
 * they run). Restore is a structural change: single-threaded and not allowed while a schedule
 * runs.
 */
typedef struct de_world_snapshot {
    void *data; /**< the snapshot block (`size` bytes), owned by the allocator given to take */
    u64 size;
    u64 tick; /**< de_world_tick() at take */
} de_world_snapshot;
/**
 * The size in bytes that de_world_snapshot_take() would allocate for the world as it is now, so
 * callers can size a buffer pool or a snapshot ring up front. 0 for a NULL world.
 */
DE_API u64 de_world_snapshot_size(const de_world *w);
/**
 * Copies the world's entity allocator and every DE_COMP_SNAPSHOT component column into one block
 * allocated from `a` (NULL means de_heap_allocator()). Free it with de_world_snapshot_free() and
 * the same allocator. A world with no snapshot components gives a small, valid snapshot (the
 * header and the entity allocator only).
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (`w` or `out` NULL), DE_ERR_UNSUPPORTED (a snapshot
 *         component in use whose type is not POD) or DE_ERR_OUT_OF_MEMORY. On failure `*out` is
 *         zeroed.
 */
DE_API de_result de_world_snapshot_take(de_world *w, const de_allocator *a, de_world_snapshot *out);
/**
 * Puts the world back in the state of `snap`:
 *
 * - The entity set, every index's generation and the free list become exactly the snapshot's, so
 *   entity handles from take time are valid again and later spawns get the same ids.
 * - Entities alive now but not in the snapshot (or alive with another generation) are despawned,
 *   in ascending index order (destruct hooks run).
 * - Entities in the snapshot but not alive now are re-created with their snapshot components only.
 * - Entities alive in both keep their components without DE_COMP_SNAPSHOT; their snapshot
 *   components become exactly the snapshot's set (added or removed as needed).
 * - Snapshot component data is restored byte for byte. Rows a restore writes get `changed` = the
 *   world's current change tick (re-created or added columns also get `added`); ticks are not part
 *   of a snapshot.
 * - Each archetype's rows are put back in the snapshot's row order when the entities are in the
 *   same archetypes as at take time (always true when every component has DE_COMP_SNAPSHOT), so
 *   iteration order after the restore is the take-time order.
 * - The world tick becomes `snap->tick`.
 *
 * When an archetype still holds the snapshot's entities in the snapshot's order (only component
 * data changed since take), its data is restored with one memcpy per column and chunk.
 * Archetypes created since take stay (empty if every component has DE_COMP_SNAPSHOT). Resources,
 * events, queries and recorded commands are not touched: apply or discard commands first.
 *
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (`w` or `snap` NULL, or a snapshot that is corrupt or
 *         does not fit the world's components; the world is unchanged) or DE_ERR_OUT_OF_MEMORY.
 *         Out of memory after the restore started leaves a valid world that is only partly
 *         restored (some snapshot entities may be missing); restore again.
 * Not allowed while a schedule runs (asserted in Debug and Dev).
 */
DE_API de_result de_world_snapshot_restore(de_world *w, const de_world_snapshot *snap);
/** Frees the block of `snap` (allocated from `a`; NULL means de_heap_allocator()) and zeroes
 * `*snap`. A NULL `snap` or an empty snapshot is ignored. */
DE_API void de_world_snapshot_free(const de_allocator *a, de_world_snapshot *snap);
/**
 * XXH3-64 checksum of the world's logical content (SPEC §5.7 rule 7), for determinism tests and
 * desync detection. It covers every alive entity in ascending index order (its index and
 * generation) and, for each of its components in ascending component id whose flags intersect
 * `component_flags_mask`, the component id and the bytes of each reflected field (de_type_fields;
 * nested structs and arrays field by field, strings up to their terminator). Bytes no field
 * covers (struct padding) never enter it, floats count by bit pattern, and tag components count
 * by id. It does not depend on archetype creation order, chunk placement or row order. The world
 * tick, change ticks, resources and events are not part of it.
 *
 * The value is the same on every supported platform (all are little-endian) for the same logical
 * content and the same component registration order. Mask 0 covers the entities only. Allocates
 * only the first time it meets a component (its field layout is cached). 0 for a NULL world.
 */
DE_API u64 de_world_checksum(de_world *w, u32 component_flags_mask);

/* ---------- Hierarchy and built-in components ---------- */
/*
 * Hierarchy and transforms (SPEC §7.1 Relationships, §5.5 large worlds, §10.2 step 4; DENG-351).
 *
 * Built-in components. Four reflected types, registered in the world's type registry under these
 * names and then as components of the world, in this order:
 *   - DeTransform ("DeTransform"): the local transform, relative to the parent (or to the world
 *     for a root). Flags DE_COMP_SNAPSHOT | DE_COMP_SERIALIZE. Constructed as the identity.
 *   - DeGlobalTransform ("DeGlobalTransform"): the world transform, written by transform
 *     propagation. Derived data: no component flags (not in snapshots or saves). Constructed as
 *     the identity.
 *   - DeChildOf ("DeChildOf"): the parent, and the links of the parent's intrusive child list.
 *     DE_COMP_SNAPSHOT | DE_COMP_SERIALIZE.
 *   - DeChildren ("DeChildren"): the first and last child and the child count.
 *     DE_COMP_SNAPSHOT | DE_COMP_SERIALIZE.
 * The child list is intrusive (doubly linked through DeChildOf), so every component stays POD and
 * the child order is the insertion order: deterministic, and the same after a snapshot restore.
 *
 * Registration is lazy: the first de_builtin_components() or de_entity_set_parent() call
 * registers the four
 * components and adds the transform propagation system. Their component ids are the next free ids
 * at that point, so they are part of the determinism contract like every other component id: make
 * that first call at the same point of the world's setup on every machine (normally right after
 * de_world_create). It is single-threaded and not allowed while a schedule runs. Read-only calls
 * (de_entity_parent, child iteration) on a world without built-ins return DE_ENTITY_NULL, and
 * de_cmd_set_parent() needs the built-ins registered.
 *
 * Hierarchy rules:
 *   - Change DeChildOf and DeChildren only through these functions (and the commands); never add,
 *     remove or write them directly.
 *   - An entity has DeChildOf exactly when it has a parent, and DeChildren exactly when it has at
 *     least one child (it is removed with the last child). Any entity may be in a hierarchy; only
 *     entities with DeTransform take part in transform propagation.
 *   - de_entity_despawn() of a parent detaches its children first: they lose DeChildOf and become
 *     roots (their DeTransform then counts as world-relative). Use de_entity_despawn_recursive()
 *     to despawn a whole subtree. Snapshot restores do not detach: they restore the hierarchy
 *     components as they were at take time.
 *   - A change of parent marks the child's DeTransform and DeChildOf changed, and the DeChildren
 *     of each parent whose list changed; links of siblings are rewritten without a mark.
 *
 * Transform propagation (the built-in system DE_TRANSFORM_PROPAGATE_SYSTEM in
 * DE_SCHEDULE_POST_UPDATE): every root (an entity with DeTransform and without DeChildOf) gets
 * global = local; then each root's subtree is walked depth first in child order and every
 * descendant with DeTransform gets global = de_transform_mul(parent global, local). Translations
 * compose in f64 (de_dvec3) and are never rounded to f32, so a 1 mm offset 100 km out keeps its
 * precision; rotation and scale are f32 (SPEC §5.5; see de_transform_mul for the non-uniform scale
 * rule). An entity without DeTransform stops the walk (as a descendant; at the top it is no
 * root): neither it nor its subtree is propagated.
 * Roots are split into fixed de_query_par_each ranges; a subtree is always handled by the range of
 * its root, so the results are bit-identical for every worker count. Only + - * / and sqrt are
 * used (no transcendental), so propagation obeys SPEC §5.7.
 *   - Skip: an entity is recomputed only when its DeTransform or DeChildOf changed since the
 *     system's last run, an ancestor was recomputed, or it has no DeGlobalTransform. Unchanged
 *     subtrees are walked (one tick test per entity) but not recomputed or written.
 *   - Missing DeGlobalTransform: an entity with DeTransform and no DeGlobalTransform gets one,
 *     holding the computed world transform, through the range's command buffer, so it is there
 *     after the PostUpdate sync point of the same run. Adding both components is not required.
 *   - Systems that write DeTransform in PostUpdate should run before the propagation system
 *     (de_system_desc.before = DE_TRANSFORM_PROPAGATE_SYSTEM); later writes are propagated on the
 *     next run.
 */

/** The local transform of an entity (relative to its parent; to the world for a root). */
typedef de_transform DeTransform;
/** The world transform of an entity, written by transform propagation. */
typedef de_transform DeGlobalTransform;
/** The parent of an entity and its links in the parent's child list (DE_ENTITY_NULL at the ends).
 */
typedef struct DeChildOf {
    de_entity parent;
    de_entity next_sibling;
    de_entity prev_sibling;
} DeChildOf;
/** The child list of an entity: first and last child (insertion order) and the count (>= 1). */
typedef struct DeChildren {
    de_entity first;
    de_entity last;
    u32 count;
} DeChildren;

/** Name of the built-in transform propagation system (DE_SCHEDULE_POST_UPDATE). Use it in
 * de_system_desc.before / after. */
#define DE_TRANSFORM_PROPAGATE_SYSTEM "de.transform_propagate"

/** The component ids of the built-in components of a world. */
typedef struct de_builtin_component_ids {
    de_component transform;        /**< DeTransform */
    de_component global_transform; /**< DeGlobalTransform */
    de_component child_of;         /**< DeChildOf */
    de_component children;         /**< DeChildren */
} de_builtin_component_ids;

/**
 * The built-in component ids of `w`, registering the built-in components and adding the
 * propagation system on the first call (see above). Later calls only read, and may run on any
 * thread. On failure (NULL world, out of memory, a first call while a schedule runs, a type name
 * or the system name taken with another layout) every id is invalid and the error context is set.
 */
DE_API de_builtin_component_ids de_builtin_components(de_world *w);

/**
 * Makes `parent` the parent of `child`: `child` is appended to the end of `parent`'s child list
 * (DeChildOf added to the child, DeChildren to the parent as needed). DE_ENTITY_NULL detaches
 * `child` (it loses DeChildOf and becomes a root). Setting the current parent again does nothing
 * and keeps the child's place in the list. A parent that is `child` itself or one of its
 * descendants would make a cycle: the call is rejected and changes nothing (a DE_ASSERT failure in
 * Debug and Dev builds). Registers the built-ins on first use.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (`w` NULL, `child` not alive, `parent` neither
 *         DE_ENTITY_NULL nor alive, or a cycle) or DE_ERR_OUT_OF_MEMORY (nothing changed).
 * Immediate: not allowed inside a running system (use de_cmd_set_parent).
 */
DE_API de_result de_entity_set_parent(de_world *w, de_entity child, de_entity parent);
/** The parent of `e`, or DE_ENTITY_NULL (a root, not alive, or no built-ins). */
DE_API de_entity de_entity_parent(const de_world *w, de_entity e);
/** The first child of `parent` (insertion order), or DE_ENTITY_NULL. */
DE_API de_entity de_entity_children_first(const de_world *w, de_entity parent);
/** The next sibling of `child` in its parent's list, or DE_ENTITY_NULL after the last child.
 * Iterate: for (c = de_entity_children_first(w, p); c.bits; c = de_entity_children_next(w, c)). */
DE_API de_entity de_entity_children_next(const de_world *w, de_entity child);
/** The number of children of `parent` (0 for none, not alive, or no built-ins). */
DE_API u32 de_entity_children_count(const de_world *w, de_entity parent);
/**
 * Despawns `e` and all its descendants (post-order: the leftmost leaf first, each parent after
 * its children) and removes `e` from its parent's list. A dead `e` is ignored. Without
 * built-ins it is a plain despawn. Immediate: not allowed inside a running system (use
 * de_cmd_despawn_recursive).
 */
DE_API void de_entity_despawn_recursive(de_world *w, de_entity e);
/**
 * Records de_entity_set_parent(child, parent) (DE_CMD_SET_PARENT). Both may be pending ids. At
 * apply, a cycle, or a `child` or non-null `parent` that is not alive, drops the command (a
 * warning in Debug and Dev; no assert: parallel systems may record conflicting changes). The
 * built-ins must be registered (de_builtin_components); otherwise nothing is recorded and the
 * error context is set.
 */
DE_API void de_cmd_set_parent(de_commands *c, de_entity child, de_entity parent);
/** Records de_entity_despawn_recursive(e) (DE_CMD_DESPAWN_RECURSIVE). `e` may be pending (it then
 * does not cancel the spawn, unlike de_cmd_despawn: the entity is spawned, then despawned). */
DE_API void de_cmd_despawn_recursive(de_commands *c, de_entity e);

DE_EXTERN_C_END
#endif
