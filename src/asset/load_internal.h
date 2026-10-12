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
    /** DENG-143: the waiting requests made without DE_LOAD_NO_EVENTS (part of
     * `interest_total`); per-asset events are sent only while it is not 0. */
    u32 interest_events;
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
    u32 free_next;  /**< next free index + 1 (FIFO); 0 = end */
    bool releasing; /**< DENG-143: de_asset_release ends it (no REQUEST_DONE event) */
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

/* ---- DENG-387: waking a blocked waiter (status.c) -------------------------------------------
 *
 * de_asset_wait and the pipeline shutdown wait for loads that other threads finish. They block
 * in the OS on the asset system's wake object (`s->wake`; Trama "Decision: A waiting thread
 * blocks in the OS; it never spins without a bound"). A producer sets `pending` and signals
 * whenever the main thread has something to apply: a queued completion (de__slot_complete), a
 * finished pipeline job (de__pipeline_finish) or a main-thread load (de__load_stage_begin). The
 * waiter clears `pending` BEFORE it looks for progress (de__load_update) and blocks only while
 * `pending` is still clear, so a wake-up between the look and the block is not lost. A timed
 * wait is only a safety net (main-thread tasks queued while it blocks).
 *
 * Lifetime: the asset system creates the object and destroys it last, after the pipeline
 * shutdown. de__pipeline_finish publishes a job INSIDE de__asset_wake_begin/_end, so once the
 * main thread has taken every job, no poster can still be inside the lock: the shutdown's
 * lock/unlock is a barrier, and the object is freed after it.
 */

/** Wake-up counters for tests (main thread). */
typedef struct de__asset_wait_stats {
    u64 waits;     /**< de_asset_wait calls */
    u64 rounds;    /**< de_asset_wait loop rounds (update + check) */
    u64 blocks;    /**< de__asset_wake_block calls (de_asset_wait, the pipeline shutdown) */
    u64 signalled; /**< blocks a producer ended, or that found `pending` already set */
    u64 timeouts;  /**< blocks that ran out (the safety net, or the caller's deadline) */
} de__asset_wait_stats;

/** The wake object. Fields are guarded by `lock`. */
typedef struct de__asset_wake {
    de_mutex lock;
    de_cond cond;
    bool pending; /**< a producer reported progress since the waiter last cleared it */
    u32 waiters;  /**< threads blocked in de__asset_wake_block */
    de__asset_wait_stats stats;
} de__asset_wake;

/** de_asset_system_create: allocates the wake object. DE_ERR_OUT_OF_MEMORY. */
de_result de__asset_wake_init(de_asset_system *s);
/** de_asset_system_destroy, last (after the pipeline shutdown): barrier, then frees it. */
void de__asset_wake_shutdown(de_asset_system *s);
/** A producer: takes the wake lock. Publish the progress, then call de__asset_wake_end. Any
 * thread. A producer may hold the pipeline's `lock` or the load state's `lock` while it calls
 * this; the waiter takes neither of them while it holds the wake lock. */
void de__asset_wake_begin(de_asset_system *s);
/** A producer: sets `pending`, wakes a blocked waiter, releases the wake lock. */
void de__asset_wake_end(de_asset_system *s);
/** A producer whose progress is already published (and outlives the call): begin + end. */
void de__asset_wake_signal(de_asset_system *s);
/** The waiter, BEFORE it looks for progress: clears `pending`. */
void de__asset_wake_reset(de_asset_system *s);
/**
 * The waiter: blocks until a producer sets `pending` or `max_ns` passed (a timed wait in the OS;
 * on Windows it lasts at least one timer tick). Returns at once if `pending` is set. True when a
 * producer ended the wait.
 */
bool de__asset_wake_block(de_asset_system *s, u64 max_ns);
/** The counters (zeroes without a wake object). Main thread. */
void de__asset_wait_get_stats(const de_asset_system *s, de__asset_wait_stats *out);

DE_EXTERN_C_END

#endif /* DE_ASSET_LOAD_INTERNAL_H */
