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
 * hook. If `e` already has `c`, nothing moves and the existing data is returned. Otherwise `e`
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
    DE_CMD_SEND_EVENT = 5
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
 * replaced. Returns NULL and sets the error context if `c` is NULL, `comp` is not registered or
 * memory runs out; nothing is recorded then.
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
