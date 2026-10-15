/*
 * upload_ring.h: the uploader's internals, shared by the loaders and the tests (DENG-105).
 * Internal to de_render; include as "render/loaders/upload_ring.h". The public side is
 * render_assets.h.
 *
 * One UPLOAD for one asset: its GPU targets (a texture, or the buffers of a mesh) and its pieces.
 * A piece is one copy: a texture subresource (de_gpu_cmd_copy_buffer_to_texture) or a buffer range
 * of at most DE__UPLOAD_MAX_PIECE bytes (de_gpu_cmd_copy_buffer). The loader lays the pieces out
 * in a STAGED LAYOUT (de__upload_layout: each piece at an aligned offset), then de__upload_submit
 * copies them from the cooked blob either straight into the ring (one reservation for the whole
 * layout) or, when the ring has no room now, into a heap copy (a SPILL) that the flush moves into
 * the ring piece by piece. Uploads are queued in submit order.
 *
 * The ring is a FIFO over byte positions that only grow (head, tail); position p lives at offset
 * p mod size. A reservation never wraps inside itself: when it does not fit before the end it
 * starts at offset 0 and the skipped bytes belong to it. Each reservation is a RECORD in a fixed
 * array (DE__UPLOAD_MAX_RECORDS): [begin, end) of positions and the fence value that frees it,
 * set ("closed") once every copy reading it is recorded. The tail moves over closed records whose
 * fence value has completed, in order. An empty ring restarts at offset 0, so any piece up to the
 * ring size fits eventually. The flush reserves the space of spilled pieces in one record per
 * frame, closed with that frame's fence value.
 *
 * Threads: `lock` guards the ring, the queue, the release list and every upload's queue state.
 * Loaders reserve, then copy into the ring outside the lock (the record stays open until the
 * upload is done, so nothing reuses it). The flush holds the lock from its first reservation to
 * its submit, so an unload (which takes the lock to cancel its upload) can never destroy a
 * resource between the recording of a copy and its submit.
 */
#ifndef DE_RENDER_LOADERS_UPLOAD_RING_H
#define DE_RENDER_LOADERS_UPLOAD_RING_H

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_debug.h>
#include <dengine/de_gpu.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "platform/de_atomic.h"
#include "render/loaders/render_assets.h"

DE_EXTERN_C_BEGIN

/** Ring reservations in flight (fixed array, sized at create). */
#define DE__UPLOAD_MAX_RECORDS 1024u
/** Release barriers waiting for de_render_uploader_acquire (fixed array). A flush finishes no
 * more uploads than fit. */
#define DE__UPLOAD_MAX_RELEASES 4096u
/** Most GPU objects one upload writes (a mesh's buffers). */
#define DE__UPLOAD_MAX_TARGETS 5u
/** Barriers one barrier call of the flush or of an acquire holds (fixed scratch). */
#define DE__UPLOAD_SCRATCH_BARRIERS 64u
/** Largest buffer piece; longer ranges are cut. Also at most the ring size. */
#define DE__UPLOAD_MAX_PIECE ((u64)16 << 20)
/** Staged alignment of texture pieces (the D3D12 copy rule, DE_COOKED_TEXTURE_SUBRESOURCE_ALIGN)
 * and the ring's base alignment. */
#define DE__UPLOAD_TEXTURE_ALIGN 512u
/** Staged alignment of buffer pieces. */
#define DE__UPLOAD_BUFFER_ALIGN 16u
/** Default asset.upload_mb_per_frame. */
#define DE__UPLOAD_DEFAULT_MB_PER_FRAME 64

/** One GPU object an upload writes: exactly one of the two is valid. */
typedef struct de__upload_target {
    de_gpu_texture texture;
    de_gpu_buffer buffer;
} de__upload_target;

/** One copy. */
typedef struct de__upload_piece {
    u64 blob_off; /**< where its bytes are in the cooked blob (read by de__upload_submit) */
    u64 src;      /**< its offset in the staged layout (de__upload_layout) */
    u64 size;
    u32 target;    /**< index into de__upload.targets */
    u32 row_pitch; /**< texture: bytes per row (a multiple of 256); 0 for a buffer */
    u32 mip, layer;
    u64 dst_off; /**< buffer: byte offset in the target */
} de__upload_piece;

typedef enum de__upload_state {
    DE__UPLOAD_NEW = 0,  /**< built by the loader, not submitted */
    DE__UPLOAD_QUEUED,   /**< waiting for (more) copies */
    DE__UPLOAD_RECORDED, /**< every copy submitted; done when the fence reaches fence_value */
    DE__UPLOAD_DONE,     /**< the fence passed: the GPU has the data */
    DE__UPLOAD_FAILED,   /**< the submit of one of its copies failed */
    DE__UPLOAD_CANCELLED /**< released before it was done */
} de__upload_state;

typedef struct de__upload {
    /* Filled by the loader before de__upload_submit, read-only afterwards. */
    de__upload_target targets[DE__UPLOAD_MAX_TARGETS];
    u32 target_count;
    u32 piece_count;
    de__upload_piece *pieces; /**< in the same block as the upload */
    u64 staged_size;          /**< bytes of the staged layout */
    size_t block_size;

    /* Under the uploader's lock. */
    u32 state;      /**< de__upload_state */
    u32 next_piece; /**< the first piece not recorded yet */
    bool began;     /**< the targets were moved to COPY_DST (first piece recorded) */
    bool in_ring;   /**< the staged layout is in the ring at ring_off (record `record`) */
    u64 ring_off;
    u32 record; /**< absolute record number of its reservation (in_ring) */
    u8 *spill;  /**< heap copy of the staged layout (!in_ring); freed once recorded */
    u64 fence_value;
    struct de__upload *q_prev, *q_next;
} de__upload;

/** One ring reservation. */
typedef struct de__ring_record {
    u64 begin, end; /**< positions */
    u64 fence;      /**< the fence value after which nothing reads it (valid when closed) */
    bool closed;
    /** A stage record (de__uploader_stage, DENG-177): `fence` is a value of this fence, not of the
     * uploader's. Invalid for the uploader's own records. */
    de_gpu_fence ext;
} de__ring_record;

/** A release whose acquire half the graphics queue still has to record. */
typedef struct de__upload_handoff {
    de_gpu_barrier barrier;
    u64 value; /**< fence value of the copy list that recorded the release */
} de__upload_handoff;

/* The four types' shared state lives in the uploader (loaders.c fills it). */
struct de_render_uploader {
    const de_allocator *alloc;
    de_gpu_device *dev;
    de_asset_system *assets;
    bool broken; /**< registration failed half way: loaders refuse */

    de_mutex lock;
    /* Ring. */
    de_gpu_buffer ring;
    u8 *ring_ptr;
    u64 ring_size;
    u64 head, tail;
    de__ring_record *records; /**< DE__UPLOAD_MAX_RECORDS, a FIFO */
    u32 rec_first;            /**< absolute number of the oldest record */
    u32 rec_count;
    /* Queue. */
    de__upload *q_head, *q_tail;
    u32 queued;
    u32 spills_queued; /**< queued uploads that are spills: new uploads spill too (order) */
    /* Release barriers for the graphics queue. */
    de__upload_handoff *releases; /**< DE__UPLOAD_MAX_RELEASES */
    u32 release_count;
    bool warned_releases_full; /**< logged once until the next acquire */
    de_log_category log;
    /* Fence. */
    de_gpu_fence fence;
    u64 submitted;
    u64 completed;
    de_result submit_error; /**< the last failed submit (its uploads fail) */
    /* Per-frame cap. */
    u64 frame_cap; /**< bytes; 0 = the cvar */
    de_cvar cap_cvar;
    /* Barrier scratch of one flush (fixed). */
    de_gpu_barrier scratch[DE__UPLOAD_SCRATCH_BARRIERS];
    /* Stats. */
    u64 st_frames, st_bytes, st_last_bytes, st_spilled;
    u32 st_last_pieces;
    /* Tests: the completed value the uploader sees is at most this (UINT64_MAX = off). */
    u64 test_fence_cap;
    /* Deferred unloads (DENG-422, render_assets.h). Under `lock`, except retire_value. */
    de_gpu_fence frame_fence;   /**< the renderer's; invalid: unloads free at once */
    de_atomic_u64 retire_value; /**< fence value an object unloaded now waits for (0: none) */
    struct de__render_retire *retire_head, *retire_tail; /**< FIFO, values ascending */
    u32 retire_pending;
    u64 retire_freed;
    u64 test_frame_cap; /**< tests: completed frame-fence value seen is at most this */
    /* DENG-302: bumped whenever the GPU material blocks may need a repack (a material published,
     * reloaded or unloaded; a texture reloaded or unloaded). The GPU scene re-checks every
     * referenced material in the frame after a change (render/materials/material_instances.h).
     * Any thread. */
    de_atomic_u64 material_epoch;
};

/** DENG-302: a material or texture changed (de_render_uploader.material_epoch). Any thread. */
static inline void de__uploader_materials_changed(de_render_uploader *u) {
    if (u != NULL)
        (void)de_atomic_fetch_add_u64(&u->material_epoch, 1u, DE_ATOMIC_RELEASE);
}

/** Unload of a render asset object (the four types' `unload`): frees it with `free_fn` now, or
 * puts it on the retire list (render_assets.h, DENG-422). Any thread. */
void de__uploader_retire(de_render_uploader *u, struct de__render_retire *node, void *object,
                         void (*free_fn)(de_render_uploader *u, void *object));
/** Frees every retired object now (the uploader's destroy; the GPU must be idle). */
void de__uploader_retire_all(de_render_uploader *u);

/**
 * Creates the ring and the fence of `u` (its `alloc` and `dev` set): a `ring_bytes` UPLOAD buffer
 * (rounded up to 512) and a timeline fence. DE_ERR_* from the RHI, DE_ERR_OUT_OF_MEMORY.
 */
de_result de__uploader_init(de_render_uploader *u, u64 ring_bytes, u64 frame_cap_bytes);
/** Frees what de__uploader_init made. Every upload must be released. */
void de__uploader_fini(de_render_uploader *u);

/**
 * A new upload with room for `piece_count` pieces, state NEW (loader thread). Fill targets and
 * pieces (blob_off, size, target and the copy fields), then call de__upload_layout. NULL when
 * out of memory.
 */
de__upload *de__upload_new(de_render_uploader *u, u32 piece_count);
/**
 * Assigns each piece its staged offset (textures DE__UPLOAD_TEXTURE_ALIGN, buffers
 * DE__UPLOAD_BUFFER_ALIGN) and sets staged_size. DE_ERR_UNSUPPORTED (with a message) when a
 * piece is larger than the ring.
 */
de_result de__upload_layout(de_render_uploader *u, de__upload *up);
/**
 * Copies each piece from `blob` (blob + blob_off, size bytes) into the ring, or into a heap spill
 * when the ring has no room now (or spills are queued), and queues the upload. Loader thread.
 * DE_ERR_OUT_OF_MEMORY (no spill).
 */
de_result de__upload_submit(de_render_uploader *u, de__upload *up, const u8 *blob);
/**
 * Where the upload is: DE_OK once the GPU has every byte (the fence passed its last copy),
 * DE_ERR_BUSY while copies are queued or in flight, or the submit error. Frees the spill once it
 * is no longer read. Main thread (the `ready` hooks).
 */
de_result de__upload_poll(de_render_uploader *u, de__upload *up);
/**
 * Releases the upload: takes it off the queue if it is there (its ring reservation retires
 * after the copies already submitted), drops its pending acquire barriers, frees it. Call it
 * before destroying the targets. Any thread. NULL is ignored.
 */
void de__upload_release(de_render_uploader *u, de__upload *up);

/** Tests: the uploader sees the fence's completed value as at most `cap` (UINT64_MAX = off), as
 * if the GPU were late. */
void de__uploader_test_hold_fence(de_render_uploader *u, u64 cap);

/* ---- Stages: ring space for a caller that records its own copies (DENG-177) ------------------
 *
 * The GPU scene (src/render/scene/) stages its per-frame table updates in this ring instead of a
 * staging buffer of its own, and copies them in a render-graph pass on the graphics queue. A
 * stage is a ring record like any other (the tail moves over records in order), except that it
 * retires on a fence of the caller's: the value the frame that reads it signals when it is done.
 * The per-frame cap (asset.upload_mb_per_frame) does not apply to stages.
 */

/** One stage: where its bytes go. */
typedef struct de__upload_stage {
    u8 *ptr;            /**< the reservation in the mapped ring (write the bytes here) */
    u64 offset;         /**< its offset in `ring` (the copies' source offset) */
    de_gpu_buffer ring; /**< the ring buffer (DE_MEM_UPLOAD, COPY_SRC) */
    u32 record;         /**< for de__uploader_stage_close */
} de__upload_stage;

/**
 * Reserves `size` bytes aligned to `align` (a power of two, at most 512) now, in a record of its
 * own, after retiring what completed. Any thread (it takes the lock).
 * @return DE_OK; DE_ERR_BUSY when the ring has no room now (retry after earlier frames
 *         complete); DE_ERR_UNSUPPORTED (with a message) when `size` is 0 or larger than the ring.
 */
de_result de__uploader_stage(de_render_uploader *u, u64 size, u64 align, de__upload_stage *out);
/** Closes stage `record`: it retires once `fence` reaches `value`. Call it once per stage, after
 * the copies that read it are submitted (or, when they never will be, with an invalid fence:
 * it retires at once). Any thread. */
void de__uploader_stage_close(de_render_uploader *u, u32 record, de_gpu_fence fence, u64 value);

DE_EXTERN_C_END

#endif /* DE_RENDER_LOADERS_UPLOAD_RING_H */
