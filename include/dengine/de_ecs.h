/* de_ecs.h — archetype ECS: entities, components, queries, systems, schedules, events.
 * SPEC.md §7. */
#ifndef DE_ECS_H
#define DE_ECS_H
#include "de_base.h"
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
/** The world's tick counter. 0 after create; schedules advance it (DENG-350). */
DE_API u64 de_world_tick(const de_world *w);

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
DE_API void de_entity_despawn(de_world *w, de_entity e);
/** true if `e` was spawned in this world and not despawned since (its generation matches). */
DE_API bool de_entity_alive(const de_world *w, de_entity e);
DE_API void *de_entity_add(de_world *w, de_entity e, de_component c); /* returns zeroed data */
DE_API void de_entity_remove(de_world *w, de_entity e, de_component c);
/**
 * The data of component `c` of entity `e`, or NULL if `e` is not alive or has no `c`. For a tag
 * component the pointer is a non-NULL placeholder that must not be written through. The pointer
 * is valid until the next structural change of the world.
 */
DE_API void *de_entity_get(de_world *w, de_entity e, de_component c); /* NULL if absent */
/** Like de_entity_get(), read-only. */
DE_API const void *de_entity_get_const(const de_world *w, de_entity e, de_component c);
/** true if `e` is alive and has component `c` (tags included). */
DE_API bool de_entity_has(const de_world *w, de_entity e, de_component c);
DE_API void de_entity_mark_changed(de_world *w, de_entity e, de_component c);

/* Hierarchy (ChildOf / Children). Transform propagation is a built-in PostUpdate system. */
DE_API void de_entity_set_parent(de_world *w, de_entity child, de_entity parent);
DE_API de_entity de_entity_parent(const de_world *w, de_entity e);

/* ---------- Resources (typed singletons) ---------- */
DE_API de_resource_id de_resource_insert(de_world *w, de_type_id type, const void *init);
DE_API void *de_resource_get(de_world *w, de_resource_id id);

/* ---------- Events (double-buffered; readable for two frames) ---------- */
typedef struct de_event_reader {
    u64 cursor;
    de_event_type type;
} de_event_reader;
DE_API de_event_type de_event_register(de_world *w, de_type_id payload);
DE_API void de_event_send(de_world *w, de_event_type t, const void *payload);
DE_API de_event_reader de_event_reader_create(de_world *w, de_event_type t);
DE_API const void *de_event_next(de_world *w, de_event_reader *r); /* NULL when drained */

/* ---------- Queries ---------- */
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

typedef struct de_query_desc {
    const de_query_term *terms;
    u32 term_count;
    const de_query_filter *filters;
    u32 filter_count;
} de_query_desc;

/* A chunk is a contiguous run of rows in one archetype table. columns[i] matches terms[i]
 * (NULL for an absent optional term). */
typedef struct de_query_chunk {
    u32 count;
    const de_entity *entities;
    void **columns;
} de_query_chunk;

typedef void (*de_chunk_fn)(const de_query_chunk *chunk, void *user);

DE_API de_query de_query_create(de_world *w, const de_query_desc *desc);
DE_API void de_query_each(de_world *w, de_query q, de_chunk_fn fn, void *user);
/* Parallel over fixed row ranges; deterministic split. */
DE_API void de_query_par_each(de_world *w, de_query q, u32 grain_rows, de_chunk_fn fn, void *user);
DE_API u32 de_query_count(de_world *w, de_query q);

/* ---------- Commands (deferred structural changes) ---------- */
typedef struct de_commands de_commands;
DE_API de_entity de_cmd_spawn(de_commands *c); /* reserved id, valid after apply */
DE_API void de_cmd_despawn(de_commands *c, de_entity e);
DE_API void *de_cmd_add(de_commands *c, de_entity e, de_component comp);
DE_API void de_cmd_remove(de_commands *c, de_entity e, de_component comp);
DE_API void de_cmd_send_event(de_commands *c, de_event_type t, const void *payload);

/* ---------- Systems and schedules ---------- */
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

typedef struct de_system_ctx {
    de_world *world;
    de_commands *commands;   /* per-thread buffer, applied at the next sync point */
    const de_query *queries; /* in the order declared in the system desc */
    f32 dt;                  /* fixed dt in FIXED_UPDATE, frame dt otherwise */
    f32 alpha;               /* interpolation alpha (fixed-step mode) */
    u64 tick;
    void *user;
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

DE_API de_system de_system_add(de_world *w, const de_system_desc *desc);
DE_API void de_system_remove(de_world *w, de_system s);
DE_API void de_system_set_enabled(de_world *w, de_system s, bool enabled);
/* Builds (or reuses) the task graph for the schedule and runs it, then applies commands. */
DE_API void de_world_run_schedule(de_world *w, de_schedule s, f32 dt, f32 alpha);

/* ---------- Snapshots ---------- */
typedef struct de_world_snapshot {
    void *data;
    u64 size;
    u64 tick;
} de_world_snapshot;
DE_API de_result de_world_snapshot_take(de_world *w, const de_allocator *a, de_world_snapshot *out);
DE_API de_result de_world_snapshot_restore(de_world *w, const de_world_snapshot *snap);
DE_API void de_world_snapshot_free(const de_allocator *a, de_world_snapshot *snap);
DE_API u64 de_world_checksum(de_world *w, u32 component_flags_mask);

DE_EXTERN_C_END
#endif
