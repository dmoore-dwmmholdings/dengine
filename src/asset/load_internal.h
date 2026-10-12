/*
 * load_internal.h: load requests and per-slot load state (SPEC §11.1, §11.3, §11.4; DENG-141).
 * Internal to de_asset; include as "asset/load_internal.h".
 *
 * A load request (de_asset_load / _load_many / _load_bundle) is a list of asset entries, each
 * holding one load-handle reference on its slot (slots_internal.h) until de_asset_release. The
 * request's state, counts, bytes, progress and first failure are cached on the request: they are
 * computed when the request is created and refreshed by de__load_update, so de_asset_status is
 * O(1) and never allocates.
 *
 * Per-slot load state lives next to the slot table, indexed by slot index (de__load_slot): which
 * non-final requests wait on the slot (an interest count per priority), whether a submission is
 * in flight, the backend's progress counters and the last failure message. The slot's own
 * `state`, `error`, `bytes` and `version` fields (slots_internal.h) are written here.
 *
 * Slot states (de__slot.state):
 *   INVALID   never requested
 *   PENDING   submitted to the backend, no progress reported yet
 *   LOADING   the backend reported progress
 *   READY     completed with DE_OK (stays READY when every request has released it)
 *   FAILED    completed with an error
 *   CANCELLED no request waited on it any more, so the backend's work was cancelled
 * A request submits the slots that are INVALID or CANCELLED, and the FAILED ones that no other
 * waiting request still counts as failed (a retry). READY slots count at once.
 *
 * Threads: everything here is main-thread only, except de__slot_progress / de__slot_complete
 * (backend_internal.h), which write the progress atomics and the completion queue.
 */
#ifndef DE_ASSET_LOAD_INTERNAL_H
#define DE_ASSET_LOAD_INTERNAL_H

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_platform.h>

#include "asset/backend_internal.h"
#include "asset/slots_internal.h"
#include "platform/de_atomic.h"

DE_EXTERN_C_BEGIN

/** Most load requests at once (the 20-bit index of a load handle). */
#define DE__LOAD_MAX_REQUESTS (1u << 20)
/** Longest failure message kept per slot and per request (de_load_info.first_error_msg). */
#define DE__LOAD_MSG_MAX 256u
/** Entry index meaning "none" (no failure yet). */
#define DE__LOAD_NONE 0xFFFFFFFFu

/** Per-slot load state, indexed by slot index. `gen` names the slot occupant it describes. */
typedef struct de__load_slot {
    u32 gen;                               /**< slot generation this entry is for; 0 = unused */
    u32 interest[DE__LOAD_PRIORITY_COUNT]; /**< waiting (non-final) requests, per priority */
    u32 interest_total;
    u8 priority;               /**< the priority the backend last got (de__load_priority) */
    u8 in_flight;              /**< submitted; no completion or cancel yet */
    de_atomic_u32 started;     /**< 1 once the backend reported progress (any thread) */
    de_atomic_u64 bytes_done;  /**< backend progress (any thread; only grows) */
    de_atomic_u64 bytes_total; /**< expected bytes; 0 = unknown (any thread; only grows) */
    char *error_msg;           /**< the last failure's message (system allocator), or NULL */
    u32 error_msg_size;
} de__load_slot;

/** One asset of a request. */
typedef struct de__load_entry {
    de_asset_id id;
    u32 slot;        /**< slot handle id (holds one handle ref), or 0 when the asset is unknown */
    de_result error; /**< why `slot` is 0 (DE_ERR_NOT_FOUND, ...); DE_OK otherwise */
} de__load_entry;

/** One load request. */
typedef struct de__request {
    u32 gen; /**< current generation of this index; live while `live` */
    bool live;
    de_load_state state;     /**< cached aggregate; final once >= DE_LOAD_READY */
    u32 flags;               /**< de_load_flags as given (DE_LOAD_NO_EVENTS kept for DENG-143) */
    u8 priority;             /**< de__load_priority of the request */
    de__load_entry *entries; /**< unique assets, in request order */
    u32 count;
    u32 entry_capacity; /**< entries allocated (the IDs given, before duplicates dropped) */
    /* Cached aggregate (status.c). */
    u32 ready, failed;
    u64 bytes_total, bytes_done;
    f32 progress;          /**< monotonic; 1.0 once final */
    u32 first_failed;      /**< entry index of the first failure in request order, or NONE */
    de_result first_error; /**< DE_OK, or the first failure's code (also an unknown bundle) */
    char first_error_msg[DE__LOAD_MSG_MAX];
    u32 free_next; /**< next free index + 1 (FIFO); 0 = end */
} de__request;

/** Completion queued by de__slot_complete. The message lives in the queue's text buffer. */
typedef struct de__load_completion {
    u32 slot;
    de_result result;
    u64 bytes;
    u32 msg_offset, msg_len; /**< into the text buffer; msg_len 0 = no message */
} de__load_completion;

/** A completion queue buffer (two of them, swapped by de__load_update). */
typedef struct de__load_queue {
    de__load_completion *items;
    u32 count, capacity;
    char *text;
    u32 text_len, text_capacity;
} de__load_queue;

/** All load state of a system (asset_internal.h: `s->load`). */
typedef struct de__load_state {
    de__load_backend backend;
    bool has_backend;
    /* Requests: an array grown on the main thread (handles never hold pointers into it). */
    de__request *requests;
    u32 request_count, request_capacity; /* indices handed out / allocated */
    u32 live_requests;
    u32 free_head, free_tail, free_count; /* FIFO free list, index + 1 */
    /* Per-slot load state: pages that never move (workers read the progress atomics). */
    de_atomic_ptr slot_pages[DE__SLOT_MAX_PAGES]; /* de__load_slot[DE__SLOT_PAGE_SLOTS] */
    /* Completion queue: producers append to `queues[incoming]` under `lock`. */
    de_mutex lock;
    de__load_queue queues[2];
    u32 incoming;
    bool applying; /* de__load_update is running (reentrancy guard) */
} de__load_state;

/** The load state, created on first use (NULL only when out of memory). Main thread. */
de__load_state *de__load_get(de_asset_system *s, bool create);

/** The per-slot load state of slot handle `slot` if it exists for this occupant, else NULL. */
de__load_slot *de__load_slot_find(de_asset_system *s, u32 slot);

/** The live request of `h`, or NULL (0, out of range, released). Main thread. */
de__request *de__request_get(de_asset_system *s, de_load_handle h);

/**
 * Recomputes the cached aggregate of a non-final request from its slots (status.c), and when it
 * reaches a final state, records it and drops its interest in its slots (cancelling the backend
 * work of slots nobody else waits on). Final requests are left alone. Main thread.
 */
void de__request_refresh(de_asset_system *s, de__request *r);

/** Drops one waiting request of priority `priority` from slot `slot`. When nobody waits any more,
 * an in-flight submission is cancelled (backend `cancel`, slot state CANCELLED); otherwise the
 * backend gets the most urgent remaining priority if it changed. Main thread. */
void de__load_slot_unwant(de_asset_system *s, u32 slot, de__load_priority priority);

/** Makes a non-final request final with `state` (READY, PARTIAL, FAILED or CANCELLED): progress
 * 1.0, interests dropped. Main thread. */
void de__request_finish(de_asset_system *s, de__request *r, de_load_state state);

/**
 * Applies the queued completions, moves started slots to LOADING and refreshes every non-final
 * request. Called by de_asset_system_update and de_asset_wait. Main thread.
 */
void de__load_update(de_asset_system *s);

/** asset_system.c calls this from de_asset_system_destroy, before de__slots_shutdown: releases
 * every request (and its refs) and frees the load state. */
void de__load_shutdown(de_asset_system *s);

DE_EXTERN_C_END

#endif /* DE_ASSET_LOAD_INTERNAL_H */
