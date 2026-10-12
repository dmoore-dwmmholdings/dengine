/*
 * commands_internal.h: command buffers, pending ids and deterministic apply (DENG-349, SPEC §7.1).
 *
 * Include as "ecs/commands_internal.h" (or "commands_internal.h" inside src/ecs/). Module-private.
 * DENG-350 (systems and schedules) uses the acquire/apply pair below; DENG-351 appends hierarchy
 * ops to ecs_cmd_op and to the apply switch in commands.c.
 *
 * Layout:
 *   - A buffer (struct de_commands) is a chain of pages from the world allocator. Commands are
 *     appended to the current page and never straddle pages, so a payload pointer stays valid
 *     until the apply. An apply empties the pages and keeps them, so a warmed-up buffer never
 *     allocates. A command larger than a standard page gets a page of its own size, kept too.
 *   - Each command is an ecs_cmd header followed by its payload at the payload type's alignment
 *     (at most DE_ECS_MAX_COMPONENT_ALIGN). Records are 8-byte aligned.
 *   - The world's ecs_cmd_state owns every buffer, indexed by slot. Slot 0 is the main-thread
 *     buffer (de_world_commands). Other slots are handed out by ecs_cmd_buffer_acquire and go
 *     back on a LIFO free list after each apply; their pages stay with them.
 *
 * Pending ids: generation 0, index = (slot + 1) << ECS_CMD_LOCAL_BITS | local spawn index. They
 * never collide with a live id (generation 0 is never live, DENG-344) and never equal
 * DE_ENTITY_NULL (slot + 1 >= 1).
 */
#ifndef DE_ECS_COMMANDS_INTERNAL_H
#define DE_ECS_COMMANDS_INTERNAL_H

#include "ecs_internal.h"

#include <dengine/de_platform.h>

DE_EXTERN_C_BEGIN

/** Bits of a pending id's index that hold the spawn's index in its buffer. */
#define ECS_CMD_LOCAL_BITS 20u
_Static_assert(DE_CMD_MAX_SPAWNS == (1u << ECS_CMD_LOCAL_BITS), "DE_CMD_MAX_SPAWNS mismatch");
/** Most buffers a world can hold (slots 0 .. ECS_CMD_MAX_SLOTS - 1, slot 0 = main thread). */
#define ECS_CMD_MAX_SLOTS ((1u << (32u - ECS_CMD_LOCAL_BITS)) - 1u)
/** Bytes of a standard command page (header included). */
#define ECS_CMD_PAGE_BYTES (16u * 1024u)
/** A resolved slot whose spawn a despawn cancelled. Index 0xFFFFFFFF is never a live index. */
#define ECS_CMD_CANCELLED ((de_entity){0xFFFFFFFFFFFFFFFFull})

/** Op codes stored in ecs_cmd.op: the public de_command_op values. */
typedef u8 ecs_cmd_op;

/** One recorded command; its payload follows at `payload_offset` from the header. */
typedef struct ecs_cmd {
    ecs_cmd_op op;
    u8 pad_;
    u16 payload_offset; /**< bytes from this header to the payload */
    u32 size;           /**< bytes of the whole record, a multiple of 8 */
    u32 id;             /**< component id (add, remove) or event type id (send event) */
    u32 payload_size;   /**< 0: no payload */
    de_entity entity;   /**< target, or the pending id of a spawn */
} ecs_cmd;

/** A page of commands. The records start ECS_CMD_PAGE_HEADER bytes after the page start. */
typedef struct ecs_cmd_page {
    struct ecs_cmd_page *next;
    u32 capacity; /**< record bytes after the header */
    u32 used;     /**< record bytes in use */
} ecs_cmd_page;
#define ECS_CMD_PAGE_HEADER DE_ECS_MAX_COMPONENT_ALIGN
_Static_assert(sizeof(ecs_cmd_page) <= ECS_CMD_PAGE_HEADER, "page header too large");

/** The records of page `p`. 64-aligned (pages are allocated at that alignment). */
static inline u8 *ecs_cmd_page_data(const ecs_cmd_page *p) {
    return (u8 *)p + ECS_CMD_PAGE_HEADER;
}

struct de_commands {
    de_world *w;
    u32 slot;
    u32 system_order;     /**< sort key, high part (acquired buffers) */
    u32 range_index;      /**< sort key, low part */
    u32 cmd_count;        /**< commands since the last apply */
    u32 spawn_count;      /**< pending ids handed out since the last apply */
    u32 pending_despawns; /**< despawns of pending ids since the last apply (cancel pre-pass) */
    bool active;          /**< acquired (or, for slot 0, used) since the last apply */
    ecs_cmd_page *head;   /**< first page, in record order */
    ecs_cmd_page *cur;    /**< page being written; pages after it are empty */
    /** Per local spawn index of the last apply this buffer took part in: the real entity,
     * DE_ENTITY_NULL (not spawned) or ECS_CMD_CANCELLED. Capacity is kept. */
    DE_ARRAY(de_entity) resolved;
    u64 resolved_epoch; /**< ecs_cmd_state.epoch of that apply */
};

/** An acquired buffer in the apply order: key = system_order << 32 | range_index. */
typedef struct ecs_cmd_active {
    u64 key;
    u32 slot;
    u32 pad_;
} ecs_cmd_active;

/** The DE_FK_ENTITY byte offsets of one reflected type (flattened through structs and arrays):
 * `count` entries of ecs_cmd_state.entity_offsets starting at `first`. */
typedef struct ecs_cmd_type_refs {
    u32 first;
    u32 count;
    bool built;
} ecs_cmd_type_refs;

typedef struct ecs_cmd_state {
    de_mutex lock;                              /**< guards buffers, free_slots, active (acquire) */
    DE_ARRAY(de_commands *) buffers;            /**< by slot; [0] = the main-thread buffer */
    de_commands *main;                          /**< buffers[0], readable without the lock */
    DE_ARRAY(u32) free_slots;                   /**< LIFO */
    DE_ARRAY(ecs_cmd_active) active;            /**< acquired since the last apply, sorted by key */
    DE_ARRAY(ecs_cmd_type_refs) component_refs; /**< by component id - 1 (built lazily) */
    DE_ARRAY(ecs_cmd_type_refs) event_refs;     /**< by event type id - 1 */
    DE_ARRAY(u32) entity_offsets;               /**< storage of the two above */
    u64 epoch;                                  /**< applies so far */
    bool applying;
} ecs_cmd_state;

/**
 * The buffer for key (`system_order`, `range_index`), for DENG-350's systems and query ranges.
 * Thread-safe: any number of threads may acquire at once. Acquiring a key again before the next
 * apply returns the same buffer (one thread at a time may record into it). The key, not the
 * buffer's slot or the time of the acquire, decides where the buffer applies.
 * Returns NULL (error context set) on out of memory or when ECS_CMD_MAX_SLOTS buffers are in use.
 */
de_commands *ecs_cmd_buffer_acquire(de_world *w, u32 system_order, u32 range_index);

/**
 * Applies the main-thread buffer, then every acquired buffer by (system_order, range_index), each
 * in record order, and releases the acquired buffers. Single-threaded: no thread may record or
 * acquire while it runs. Does not check w->in_schedule (schedules call it at sync points).
 */
void ecs_cmd_apply_all(de_world *w);

/** Empties `c` without applying: destructs its payloads and resets its pages. */
void ecs_cmd_buffer_discard(de_commands *c);

/** The pending id of spawn `local` of buffer `slot`. */
static inline de_entity ecs_cmd_pending_make(u32 slot, u32 local) {
    de_entity e;
    e.bits = (u64)(((slot + 1u) << ECS_CMD_LOCAL_BITS) | local);
    return e;
}
static inline u32 ecs_cmd_pending_slot(de_entity e) {
    return (de_entity_index(e) >> ECS_CMD_LOCAL_BITS) - 1u;
}
static inline u32 ecs_cmd_pending_local(de_entity e) {
    return de_entity_index(e) & ((1u << ECS_CMD_LOCAL_BITS) - 1u);
}

DE_EXTERN_C_END

#endif /* DE_ECS_COMMANDS_INTERNAL_H */
