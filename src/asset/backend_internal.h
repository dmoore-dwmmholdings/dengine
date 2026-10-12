/*
 * backend_internal.h: the seam between the load API and whatever does the loading (SPEC §11.3,
 * §11.4; DENG-141). Internal to de_asset; include as "asset/backend_internal.h".
 *
 * The load API (load.c, status.c) decides WHICH slots must be loaded, with which priority, and
 * when work is no longer wanted. A backend does the work: the real pipeline (I/O threads, zstd,
 * type loaders; DENG-142), the pack reader (S08) behind the same seam, or a fake in tests.
 *
 * Calls into the backend (main thread, from the load API):
 *   - submit(slot, priority): load the asset of `slot`. Each submit is answered by exactly one
 *     de__slot_complete, unless the slot is cancelled first.
 *   - cancel(slot): no request needs the slot any more. The backend drops the work if it can;
 *     after cancel returns it reports no further progress for that submission. A completion that
 *     races with the cancel (queued before it, or reported because the work was already ending)
 *     is handled: a success still makes the slot READY (its object is loaded), a failure is
 *     dropped, also from the queue, so it cannot end a later submission of the same slot.
 *   - reprioritize(slot, priority): the most urgent request that waits on the slot changed.
 * The same slot is never submitted twice without a completion or a cancel in between.
 *
 * Calls from the backend (any thread):
 *   - de__slot_progress: bytes read so far and the expected total, for byte-weighted progress.
 *     The first call also moves the slot from PENDING to LOADING (at the next update).
 *   - de__slot_complete: the final result. Completions are queued and applied on the main thread
 *     by de_asset_system_update (and de_asset_wait). DE_OK publishes the slot: it becomes READY,
 *     `bytes` becomes its resident size, `version` is bumped. Set the object (de__slot_set_object)
 *     and the asset's dependency refs BEFORE completing with DE_OK: a slot is READY only once its
 *     own dependencies are (SPEC §11.4; DENG-142 owns that order). A failure keeps the message of
 *     the calling thread's error context (de_error_last) when its code matches `result`.
 *
 * Priorities: de__load_priority, most urgent first (the order of DE_LOAD_PRIORITY_* flags and of
 * the manifest's bundle `priority`).
 */
#ifndef DE_ASSET_BACKEND_INTERNAL_H
#define DE_ASSET_BACKEND_INTERNAL_H

#include <dengine/de_asset.h>
#include <dengine/de_base.h>

DE_EXTERN_C_BEGIN

/** Load priority, most urgent first. Same values as de__manifest_priority. */
typedef enum de__load_priority {
    DE__LOAD_PRIORITY_CRITICAL = 0,
    DE__LOAD_PRIORITY_HIGH = 1,
    DE__LOAD_PRIORITY_NORMAL = 2, /**< no flag and no bundle priority */
    DE__LOAD_PRIORITY_LOW = 3,
    DE__LOAD_PRIORITY_BACKGROUND = 4,
    DE__LOAD_PRIORITY_COUNT = 5
} de__load_priority;

/** The backend interface. Every function pointer is required; `user` is passed back. Called on
 * the main thread only. `slot` is a slot handle id (slots_internal.h). */
typedef struct de__load_backend {
    void (*submit)(void *user, de_asset_system *s, u32 slot, de__load_priority priority);
    void (*cancel)(void *user, de_asset_system *s, u32 slot);
    void (*reprioritize)(void *user, de_asset_system *s, u32 slot, de__load_priority priority);
    void *user;
} de__load_backend;

/**
 * Installs the backend (copied), or removes it (NULL). Without a backend, every slot a request
 * submits fails at once with DE_ERR_UNSUPPORTED, so no request waits forever. Change it only
 * while no slot is in flight. Main thread.
 */
void de__asset_set_backend(de_asset_system *s, const de__load_backend *backend);
/** The installed backend, or NULL. */
const de__load_backend *de__asset_backend(const de_asset_system *s);

/**
 * Reports the final result of the submission of `slot` (see the top of the file). Any thread;
 * queued until the next de_asset_system_update or de_asset_wait. A stale or 0 handle is ignored.
 * The queue grows with the system allocator (amortised; no allocation once it is warm).
 */
void de__slot_complete(de_asset_system *s, u32 slot, de_result result, u64 bytes);

/**
 * Reports progress of an in-flight slot: `bytes_done` of `bytes_total` read (`bytes_total` may
 * grow; 0 = unknown so far). Any thread; lock-free. Values only grow (smaller ones are ignored).
 */
void de__slot_progress(de_asset_system *s, u32 slot, u64 bytes_done, u64 bytes_total);

DE_EXTERN_C_END

#endif /* DE_ASSET_BACKEND_INTERNAL_H */
