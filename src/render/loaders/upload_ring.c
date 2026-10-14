/*
 * upload_ring.c: the staging ring, the upload queue and the per-frame copy list of the render
 * asset types (SPEC §11.4, §12.1; DENG-105). upload_ring.h describes the ring, the records and
 * the threading; render_assets.h the public side.
 */

#include "render/loaders/upload_ring.h"

#include <dengine/de_base.h>
#include <dengine/de_debug.h>
#include <dengine/de_gpu.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include <stdint.h>
#include <string.h>

#define NO_RECORD 0xFFFFFFFFu

static u64 align_up(u64 v, u64 a) {
    return (v + a - 1u) & ~(a - 1u);
}

static u64 piece_align(const de__upload *up, const de__upload_piece *p) {
    return DE_HANDLE_VALID(up->targets[p->target].texture) ? DE__UPLOAD_TEXTURE_ALIGN
                                                           : DE__UPLOAD_BUFFER_ALIGN;
}

/* ---- Fence and retirement (lock held) -------------------------------------------------------- */

static void observe_fence(de_render_uploader *u) {
    u64 v = de_gpu_fence_value(u->dev, u->fence);
    if (v > u->test_fence_cap)
        v = u->test_fence_cap;
    if (v > u->completed)
        u->completed = v;
}

static de__ring_record *record_at(de_render_uploader *u, u32 abs) {
    return &u->records[abs % DE__UPLOAD_MAX_RECORDS];
}

static void record_close(de_render_uploader *u, u32 abs, u64 fence) {
    de__ring_record *r = record_at(u, abs);
    r->fence = fence;
    r->closed = true;
}

/* A closed record whose readers are done: the uploader's fence, or a stage's own (DENG-177). */
static bool record_done(de_render_uploader *u, const de__ring_record *r) {
    if (!r->closed)
        return false;
    if (DE_HANDLE_VALID(r->ext))
        return de_gpu_fence_value(u->dev, r->ext) >= r->fence;
    return r->fence <= u->completed;
}

static void ring_retire(de_render_uploader *u) {
    while (u->rec_count != 0) {
        const de__ring_record *r = record_at(u, u->rec_first);
        if (!record_done(u, r))
            break;
        u->tail = r->end;
        u->rec_first++;
        u->rec_count--;
    }
}

/* ---- Reservations (lock held) -----------------------------------------------------------------
 */

/* Where `size` bytes aligned to `align` go at the head: the offset and the new head. False when
 * the ring has no room now. */
static bool ring_fit(de_render_uploader *u, u64 size, u64 align, u64 *off, u64 *new_head) {
    if (size == 0 || size > u->ring_size)
        return false;
    if (u->rec_count == 0 && u->head == u->tail && u->head % u->ring_size != 0) {
        /* Empty: start again at offset 0, so any size up to the ring fits. */
        u->head += u->ring_size - u->head % u->ring_size;
        u->tail = u->head;
    }
    const u64 phys = u->head % u->ring_size;
    u64 start = align_up(phys, align), end;
    if (start + size > u->ring_size) {
        start = 0; /* never wrap inside a reservation: skip to the start */
        end = u->head + (u->ring_size - phys) + size;
    } else {
        end = u->head + (start - phys) + size;
    }
    if (end - u->tail > u->ring_size)
        return false;
    *off = start;
    *new_head = end;
    return true;
}

/* A new open record for `size` bytes. */
static bool ring_reserve(de_render_uploader *u, u64 size, u64 align, u64 *off, u32 *abs) {
    u64 end;
    if (u->rec_count == DE__UPLOAD_MAX_RECORDS || !ring_fit(u, size, align, off, &end))
        return false;
    *abs = u->rec_first + u->rec_count;
    de__ring_record *r = record_at(u, *abs);
    r->begin = u->head;
    r->end = end;
    r->fence = 0;
    r->closed = false;
    r->ext = (de_gpu_fence){0};
    u->rec_count++;
    u->head = end;
    return true;
}

/* Space for a spilled piece in this flush's record (`*frame_rec`, valid when `*have`). The
 * flush holds the lock, so its record is always the newest one and can grow. */
static bool frame_reserve(de_render_uploader *u, u64 size, u64 align, u64 *off, u32 *frame_rec,
                          bool *have) {
    if (!*have) /* a flag, not a sentinel: record numbers wrap through every u32 */
        return *have = ring_reserve(u, size, align, off, frame_rec);
    DE_ASSERT(*frame_rec == u->rec_first + u->rec_count - 1u);
    u64 end;
    if (!ring_fit(u, size, align, off, &end))
        return false;
    record_at(u, *frame_rec)->end = end;
    u->head = end;
    return true;
}

/* ---- Queue (lock held) ------------------------------------------------------------------------
 */

static void q_push(de_render_uploader *u, de__upload *up) {
    up->q_prev = u->q_tail;
    up->q_next = NULL;
    if (u->q_tail != NULL)
        u->q_tail->q_next = up;
    else
        u->q_head = up;
    u->q_tail = up;
    u->queued++;
    if (!up->in_ring)
        u->spills_queued++;
}

static void q_unlink(de_render_uploader *u, de__upload *up) {
    if (up->q_prev != NULL)
        up->q_prev->q_next = up->q_next;
    else
        u->q_head = up->q_next;
    if (up->q_next != NULL)
        up->q_next->q_prev = up->q_prev;
    else
        u->q_tail = up->q_prev;
    up->q_prev = up->q_next = NULL;
    DE_ASSERT(u->queued != 0);
    u->queued--;
    if (!up->in_ring) {
        DE_ASSERT(u->spills_queued != 0);
        u->spills_queued--;
    }
}

/* Drops the pending acquire barriers that name a target of `up`. */
static void releases_drop(de_render_uploader *u, const de__upload *up) {
    u32 w = 0;
    for (u32 i = 0; i < u->release_count; ++i) {
        const de__upload_handoff *r = &u->releases[i];
        bool mine = false;
        for (u32 t = 0; t < up->target_count && !mine; ++t)
            mine =
                (up->targets[t].texture.id != 0 &&
                 r->barrier.texture.id == up->targets[t].texture.id) ||
                (up->targets[t].buffer.id != 0 && r->barrier.buffer.id == up->targets[t].buffer.id);
        if (!mine)
            u->releases[w++] = *r;
    }
    u->release_count = w;
}

/* A failed submit (or a lost device): every queued upload and every upload of `done` fails; the
 * records that waited for the failed value retire at once (nothing reads them any more). */
static void fail_all(de_render_uploader *u, de_result r, de__upload *done, u64 failed_value) {
    u->submit_error = r;
    for (de__upload *up = done; up != NULL;) {
        de__upload *next = up->q_next;
        up->q_prev = up->q_next = NULL;
        up->state = DE__UPLOAD_FAILED;
        up = next;
    }
    while (u->q_head != NULL) {
        de__upload *up = u->q_head;
        q_unlink(u, up);
        if (up->in_ring)
            record_close(u, up->record, u->submitted);
        up->state = DE__UPLOAD_FAILED;
    }
    u32 w = 0;
    for (u32 i = 0; i < u->release_count; ++i)
        if (u->releases[i].value != failed_value)
            u->releases[w++] = u->releases[i];
    u->release_count = w;
    for (u32 i = 0; i < u->rec_count; ++i) {
        de__ring_record *rec = record_at(u, u->rec_first + i);
        if (rec->closed && !DE_HANDLE_VALID(rec->ext) && rec->fence == failed_value)
            rec->fence = u->submitted;
    }
}

/* ---- Lifetime ---------------------------------------------------------------------------------
 */

de_result de__uploader_init(de_render_uploader *u, u64 ring_bytes, u64 frame_cap_bytes) {
    u->ring_size =
        align_up(ring_bytes != 0 ? ring_bytes : (u64)128 << 20, DE__UPLOAD_TEXTURE_ALIGN);
    u->frame_cap = frame_cap_bytes;
    u->test_fence_cap = UINT64_MAX;
    u->test_frame_cap = UINT64_MAX;
    de_atomic_init_u64(&u->retire_value, 0);
    de_mutex_init(&u->lock);
    u->records =
        (de__ring_record *)DE_ALLOC(u->alloc, sizeof(de__ring_record) * DE__UPLOAD_MAX_RECORDS,
                                    DE_ALIGNOF(de__ring_record), DE_MEM_RENDER);
    u->releases = (de__upload_handoff *)DE_ALLOC(
        u->alloc, sizeof(de__upload_handoff) * DE__UPLOAD_MAX_RELEASES,
        DE_ALIGNOF(de__upload_handoff), DE_MEM_RENDER);
    if (u->records == NULL || u->releases == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "render assets: no memory for the upload queue");
    u->ring = de_gpu_buffer_create(u->dev, &(de_gpu_buffer_desc){.size = u->ring_size,
                                                                 .usage = DE_USAGE_COPY_SRC,
                                                                 .memory = DE_MEM_UPLOAD,
                                                                 .name = "asset upload ring"});
    if (!DE_HANDLE_VALID(u->ring)) {
        const de_error *e = de_error_last();
        return DE_FAIL_WRAP(e != NULL && e->result != DE_OK ? e->result : DE_ERR_INTERNAL,
                            "render assets: cannot create the %llu-byte upload ring",
                            (unsigned long long)u->ring_size);
    }
    u->ring_ptr = (u8 *)de_gpu_buffer_map(u->dev, u->ring);
    u->fence = de_gpu_fence_create(u->dev, 0);
    if (u->ring_ptr == NULL || !DE_HANDLE_VALID(u->fence)) {
        const de_error *e = de_error_last();
        return DE_FAIL_WRAP(e != NULL && e->result != DE_OK ? e->result : DE_ERR_INTERNAL,
                            "render assets: cannot map the upload ring or create its fence");
    }
    de_cvar_desc cd = {.name = "asset.upload_mb_per_frame",
                       .help = "MiB the asset uploader copies per frame at most (one piece "
                               "always goes)",
                       .type = DE_CVAR_INT,
                       .default_number = (f64)DE__UPLOAD_DEFAULT_MB_PER_FRAME,
                       .min = 1.0,
                       .max = 65536.0};
    u->cap_cvar = de_cvar_register(&cd);
    u->log = de_log_register_category("render", DE_LOG_INFO);
    return DE_OK;
}

void de__uploader_fini(de_render_uploader *u) {
    DE_ASSERT(u->q_head == NULL);
    if (u->dev != NULL) {
        if (DE_HANDLE_VALID(u->fence))
            de_gpu_fence_destroy(u->dev, u->fence);
        if (DE_HANDLE_VALID(u->ring))
            de_gpu_buffer_destroy(u->dev, u->ring);
    }
    if (u->records != NULL)
        DE_FREE(u->alloc, u->records, sizeof(de__ring_record) * DE__UPLOAD_MAX_RECORDS);
    if (u->releases != NULL)
        DE_FREE(u->alloc, u->releases, sizeof(de__upload_handoff) * DE__UPLOAD_MAX_RELEASES);
    u->records = NULL;
    u->releases = NULL;
    de_mutex_destroy(&u->lock);
}

/* ---- Uploads (loader side) --------------------------------------------------------------------
 */

de__upload *de__upload_new(de_render_uploader *u, u32 piece_count) {
    const size_t head = (size_t)align_up(sizeof(de__upload), 16u);
    const size_t block = head + sizeof(de__upload_piece) * (size_t)piece_count;
    de__upload *up = (de__upload *)DE_ALLOC(u->alloc, block, 16, DE_MEM_RENDER);
    if (up == NULL)
        return NULL;
    memset(up, 0, block);
    up->block_size = block;
    up->piece_count = piece_count;
    up->pieces = (de__upload_piece *)((u8 *)up + head);
    up->state = DE__UPLOAD_NEW;
    up->record = NO_RECORD;
    return up;
}

de_result de__upload_layout(de_render_uploader *u, de__upload *up) {
    u64 at = 0;
    for (u32 i = 0; i < up->piece_count; ++i) {
        de__upload_piece *p = &up->pieces[i];
        if (p->size > u->ring_size)
            return DE_FAIL(DE_ERR_UNSUPPORTED,
                           "a %llu-byte copy does not fit in the %llu-byte upload ring (raise "
                           "de_asset_system_desc.staging_ring_bytes)",
                           (unsigned long long)p->size, (unsigned long long)u->ring_size);
        p->src = align_up(at, piece_align(up, p));
        at = p->src + p->size;
    }
    up->staged_size = at;
    return DE_OK;
}

de_result de__upload_submit(de_render_uploader *u, de__upload *up, const u8 *blob) {
    if (up->piece_count == 0) {
        up->state = DE__UPLOAD_DONE;
        return DE_OK;
    }
    u64 off = 0;
    u32 rec = NO_RECORD;
    de_mutex_lock(&u->lock);
    /* Behind a queued spill a new upload spills too: the flush serves the queue in order, and a
     * ring reservation taken now would only hold space the spill is waiting for. */
    const bool in_ring = u->spills_queued == 0 &&
                         ring_reserve(u, up->staged_size, DE__UPLOAD_TEXTURE_ALIGN, &off, &rec);
    de_mutex_unlock(&u->lock);
    u8 *dst;
    if (in_ring) {
        dst = u->ring_ptr + off;
    } else {
        up->spill = (u8 *)DE_ALLOC(u->alloc, (size_t)up->staged_size, 16, DE_MEM_RENDER);
        if (up->spill == NULL)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "no memory to stage %llu bytes for the GPU",
                           (unsigned long long)up->staged_size);
        dst = up->spill;
    }
    for (u32 i = 0; i < up->piece_count; ++i) {
        const de__upload_piece *p = &up->pieces[i];
        memcpy(dst + p->src, blob + p->blob_off, (size_t)p->size);
    }
    de_mutex_lock(&u->lock);
    up->in_ring = in_ring;
    up->ring_off = off;
    up->record = rec;
    up->state = DE__UPLOAD_QUEUED;
    if (!in_ring)
        u->st_spilled++;
    q_push(u, up);
    de_mutex_unlock(&u->lock);
    return DE_OK;
}

de_result de__upload_poll(de_render_uploader *u, de__upload *up) {
    if (up == NULL)
        return DE_OK;
    de_result r = DE_ERR_BUSY;
    de_mutex_lock(&u->lock);
    observe_fence(u);
    switch ((de__upload_state)up->state) {
    case DE__UPLOAD_RECORDED:
        if (u->completed >= up->fence_value) {
            up->state = DE__UPLOAD_DONE;
            r = DE_OK;
        } else if (de_gpu_device_is_lost(u->dev)) {
            /* The fence will never get there. */
            up->state = DE__UPLOAD_FAILED;
            u->submit_error = DE_ERR_DEVICE_LOST;
            r = DE_ERR_DEVICE_LOST;
        }
        break;
    case DE__UPLOAD_DONE:
        r = DE_OK;
        break;
    case DE__UPLOAD_FAILED:
        r = u->submit_error != DE_OK ? u->submit_error : DE_ERR_DEVICE_LOST;
        break;
    case DE__UPLOAD_QUEUED:
        break;
    case DE__UPLOAD_NEW:
    case DE__UPLOAD_CANCELLED:
    default:
        r = DE_ERR_INTERNAL;
        break;
    }
    /* Once every piece is in the ring, nothing reads the spill any more. */
    u8 *spill = up->state != DE__UPLOAD_QUEUED ? up->spill : NULL;
    if (spill != NULL)
        up->spill = NULL;
    de_mutex_unlock(&u->lock);
    if (spill != NULL)
        DE_FREE(u->alloc, spill, (size_t)up->staged_size);
    if (r != DE_OK && r != DE_ERR_BUSY)
        return DE_FAIL(r, "the GPU upload failed (%s)", de_result_str(r));
    return r;
}

void de__upload_release(de_render_uploader *u, de__upload *up) {
    if (up == NULL)
        return;
    de_mutex_lock(&u->lock);
    if (up->state == DE__UPLOAD_QUEUED) {
        q_unlink(u, up);
        /* Its copies submitted so far read the reservation until `submitted` completes. */
        if (up->in_ring)
            record_close(u, up->record, u->submitted);
    }
    releases_drop(u, up);
    up->state = DE__UPLOAD_CANCELLED;
    u8 *spill = up->spill;
    up->spill = NULL;
    de_mutex_unlock(&u->lock);
    if (spill != NULL)
        DE_FREE(u->alloc, spill, (size_t)up->staged_size);
    DE_FREE(u->alloc, up, up->block_size);
}

/* ---- Flush (main thread) ----------------------------------------------------------------------
 */

static u64 frame_cap(const de_render_uploader *u) {
    if (u->frame_cap != 0)
        return u->frame_cap;
    i64 mb = DE_HANDLE_VALID(u->cap_cvar) ? de_cvar_int(u->cap_cvar)
                                          : (i64)DE__UPLOAD_DEFAULT_MB_PER_FRAME;
    return (u64)(mb < 1 ? 1 : mb) << 20;
}

static de_gpu_barrier target_barrier(const de__upload_target *t, u32 before, u32 after) {
    de_gpu_barrier b;
    memset(&b, 0, sizeof b);
    b.texture = t->texture;
    b.buffer = t->buffer;
    b.before = before;
    b.after = after;
    return b;
}

static void record_piece(de_gpu_cmd *cmd, const de__upload *up, const de__upload_piece *p,
                         de_gpu_buffer ring, u64 src) {
    const de__upload_target *t = &up->targets[p->target];
    if (DE_HANDLE_VALID(t->texture))
        de_gpu_cmd_copy_buffer_to_texture(cmd, t->texture, p->mip, p->layer, ring, src,
                                          p->row_pitch);
    else
        de_gpu_cmd_copy_buffer(cmd, t->buffer, p->dst_off, ring, src, p->size);
}

void de_render_uploader_flush(de_render_uploader *u) {
    if (u == NULL)
        return;
    (void)de__render_uploader_retire_poll(u); /* DENG-422: unloads whose frames completed */
    de_mutex_lock(&u->lock);
    observe_fence(u);
    ring_retire(u);
    u->st_last_bytes = 0;
    u->st_last_pieces = 0;
    if (u->q_head == NULL) {
        de_mutex_unlock(&u->lock);
        return;
    }
    if (de_gpu_device_is_lost(u->dev)) {
        fail_all(u, DE_ERR_DEVICE_LOST, NULL, 0);
        de_mutex_unlock(&u->lock);
        return;
    }
    const u64 cap = frame_cap(u);
    const u64 value = u->submitted + 1u;
    de_gpu_cmd *cmd = NULL;
    u64 bytes = 0;
    u32 pieces = 0, frame_rec = 0;
    bool have_frame_rec = false;
    bool spill_blocked = false, stop = false;
    de__upload *done = NULL, *done_tail = NULL;

    for (de__upload *up = u->q_head, *next = NULL; up != NULL && !stop; up = next) {
        next = up->q_next;
        if (u->release_count + up->target_count > DE__UPLOAD_MAX_RELEASES) {
            /* No room for its release barriers: the next acquire makes room. */
            if (!u->warned_releases_full) {
                u->warned_releases_full = true;
                DE_WARN(u->log,
                        "asset uploads wait: %u release barriers are pending; call "
                        "de_render_uploader_acquire (or _acquire_submit) every frame",
                        u->release_count);
            }
            break;
        }
        while (up->next_piece < up->piece_count) {
            const de__upload_piece *p = &up->pieces[up->next_piece];
            if (bytes != 0 && bytes + p->size > cap) {
                stop = true; /* the frame's budget is spent; one piece always goes */
                break;
            }
            if (!up->in_ring && spill_blocked)
                break;
            u64 src = 0;
            if (up->in_ring) {
                src = up->ring_off + p->src;
            } else {
                if (!frame_reserve(u, p->size, piece_align(up, p), &src, &frame_rec,
                                   &have_frame_rec)) {
                    spill_blocked = true; /* staged uploads behind it still go */
                    break;
                }
                memcpy(u->ring_ptr + src, up->spill + p->src, (size_t)p->size);
            }
            if (cmd == NULL) {
                cmd = de_gpu_cmd_begin(u->dev, DE_QUEUE_COPY, "asset uploads");
                if (cmd == NULL) {
                    stop = true; /* the next flush copies this piece again */
                    break;
                }
            }
            if (!up->began) {
                for (u32 t = 0; t < up->target_count; ++t)
                    u->scratch[t] =
                        target_barrier(&up->targets[t], DE_ACCESS_NONE, DE_ACCESS_COPY_DST);
                de_gpu_cmd_barriers(cmd, u->scratch, up->target_count);
                up->began = true;
            }
            record_piece(cmd, up, p, u->ring, src);
            bytes += p->size;
            pieces++;
            up->next_piece++;
        }
        if (up->next_piece < up->piece_count)
            continue;
        /* Every copy recorded: hand the targets to the graphics queue. */
        for (u32 t = 0; t < up->target_count; ++t) {
            de_gpu_barrier b =
                target_barrier(&up->targets[t], DE_ACCESS_COPY_DST, DE_ACCESS_SHADER_READ);
            b.src_queue = DE_QUEUE_COPY;
            b.dst_queue = DE_QUEUE_GRAPHICS;
            u->scratch[t] = b;
            u->releases[u->release_count].barrier = b;
            u->releases[u->release_count].value = value;
            u->release_count++;
        }
        if (cmd != NULL) /* NULL only for an upload whose pieces were all recorded before */
            de_gpu_cmd_barriers(cmd, u->scratch, up->target_count);
        q_unlink(u, up);
        if (up->in_ring)
            record_close(u, up->record, value);
        up->fence_value = value;
        up->q_next = NULL;
        if (done_tail != NULL)
            done_tail->q_next = up;
        else
            done = up;
        done_tail = up;
    }
    if (have_frame_rec) /* without a list nothing reads it: free at the last value */
        record_close(u, frame_rec, cmd != NULL ? value : u->submitted);

    if (cmd != NULL) {
        de_gpu_submit sub = {
            .cmds = &cmd, .cmd_count = 1, .signal = u->fence, .signal_value = value};
        de_result r = de_gpu_queue_submit(u->dev, DE_QUEUE_COPY, &sub);
        if (r == DE_OK) {
            u->submitted = value;
            for (de__upload *up = done; up != NULL;) {
                de__upload *next = up->q_next;
                up->q_next = NULL;
                up->state = DE__UPLOAD_RECORDED;
                up = next;
            }
            u->st_frames++;
            u->st_bytes += bytes;
            u->st_last_bytes = bytes;
            u->st_last_pieces = pieces;
        } else {
            fail_all(u, r, done, value);
        }
    } else {
        /* Nothing was recorded (no room, or no command list): finished uploads cannot exist
         * then, as an upload finishes only with its last copy recorded in this list. */
        DE_ASSERT(done == NULL);
    }
    de_mutex_unlock(&u->lock);
}

u32 de_render_uploader_acquire(de_render_uploader *u, de_gpu_cmd *cmd, de_gpu_wait *wait) {
    if (wait != NULL)
        memset(wait, 0, sizeof *wait);
    if (u == NULL || cmd == NULL)
        return 0;
    de_mutex_lock(&u->lock);
    const u32 n = u->release_count;
    u64 value = 0;
    /* In batches through the fixed scratch: the RHI records each call as one barrier command. */
    for (u32 i = 0, k = 0; i < n; i += k) {
        k = n - i < DE__UPLOAD_SCRATCH_BARRIERS ? n - i : DE__UPLOAD_SCRATCH_BARRIERS;
        for (u32 j = 0; j < k; ++j) {
            u->scratch[j] = u->releases[i + j].barrier;
            if (u->releases[i + j].value > value)
                value = u->releases[i + j].value;
        }
        de_gpu_cmd_barriers(cmd, u->scratch, k);
    }
    u->release_count = 0;
    u->warned_releases_full = false;
    de_mutex_unlock(&u->lock);
    if (wait != NULL && n != 0) {
        wait->fence = u->fence;
        wait->value = value;
    }
    return n;
}

de_render_upload_stats de_render_uploader_stats(const de_render_uploader *cu) {
    de_render_upload_stats st;
    memset(&st, 0, sizeof st);
    if (cu == NULL)
        return st;
    de_render_uploader *u = (de_render_uploader *)cu; /* the lock only */
    de_mutex_lock(&u->lock);
    observe_fence(u);
    st.frames = u->st_frames;
    st.bytes = u->st_bytes;
    st.last_frame_bytes = u->st_last_bytes;
    st.last_frame_pieces = u->st_last_pieces;
    st.queued = u->queued;
    st.spilled = u->st_spilled;
    st.ring_bytes = u->ring_size;
    st.ring_used = u->head - u->tail;
    st.ring_records = u->rec_count;
    st.pending_acquires = u->release_count;
    st.submitted = u->submitted;
    st.completed = u->completed;
    de_mutex_unlock(&u->lock);
    return st;
}

de_gpu_fence de_render_uploader_fence(const de_render_uploader *u) {
    return u != NULL ? u->fence : (de_gpu_fence){0};
}

void de__uploader_test_hold_fence(de_render_uploader *u, u64 cap) {
    de_mutex_lock(&u->lock);
    u->test_fence_cap = cap;
    de_mutex_unlock(&u->lock);
}

u32 de_render_uploader_acquire_submit(de_render_uploader *u) {
    if (u == NULL)
        return 0;
    de_mutex_lock(&u->lock);
    const bool any = u->release_count != 0;
    de_mutex_unlock(&u->lock);
    if (!any) /* the main thread is the only one that adds releases (flush) */
        return 0;
    de_gpu_cmd *cmd = de_gpu_cmd_begin(u->dev, DE_QUEUE_GRAPHICS, "asset upload acquire");
    if (cmd == NULL)
        return 0;
    de_gpu_wait wait;
    const u32 n = de_render_uploader_acquire(u, cmd, &wait);
    de_gpu_submit sub = {.cmds = &cmd,
                         .cmd_count = 1,
                         .waits = n != 0 ? &wait : NULL,
                         .wait_count = n != 0 ? 1u : 0u};
    (void)de_gpu_queue_submit(u->dev, DE_QUEUE_GRAPHICS, &sub);
    return n;
}

/* ---- Stages (DENG-177) ------------------------------------------------------------------------
 */

de_result de__uploader_stage(de_render_uploader *u, u64 size, u64 align, de__upload_stage *out) {
    memset(out, 0, sizeof *out);
    if (size == 0 || size > u->ring_size || align == 0 || align > DE__UPLOAD_TEXTURE_ALIGN ||
        (align & (align - 1u)) != 0)
        return DE_FAIL(DE_ERR_UNSUPPORTED,
                       "a %llu-byte stage (alignment %llu) does not fit the %llu-byte upload ring",
                       (unsigned long long)size, (unsigned long long)align,
                       (unsigned long long)u->ring_size);
    de_mutex_lock(&u->lock);
    observe_fence(u);
    ring_retire(u);
    u64 off = 0;
    u32 rec = 0;
    const bool ok = ring_reserve(u, size, align, &off, &rec);
    de_mutex_unlock(&u->lock);
    if (!ok)
        return DE_ERR_BUSY;
    out->ptr = u->ring_ptr + off;
    out->offset = off;
    out->ring = u->ring;
    out->record = rec;
    return DE_OK;
}

void de__uploader_stage_close(de_render_uploader *u, u32 record, de_gpu_fence fence, u64 value) {
    de_mutex_lock(&u->lock);
    de__ring_record *r = record_at(u, record);
    r->ext = fence;
    r->fence = DE_HANDLE_VALID(fence) ? value : 0;
    r->closed = true;
    ring_retire(u);
    de_mutex_unlock(&u->lock);
}

/* ---- Deferred unloads (DENG-422, render_assets.h) ---------------------------------------------
 */

void de__render_uploader_set_frame_fence(de_render_uploader *u, de_gpu_fence fence) {
    if (u == NULL)
        return;
    de_mutex_lock(&u->lock);
    u->frame_fence = fence;
    de_mutex_unlock(&u->lock);
}

void de__render_uploader_frame_prepared(de_render_uploader *u, u64 frame_index) {
    if (u != NULL)
        de_atomic_store_u64(&u->retire_value, frame_index + 1u, DE_ATOMIC_RELEASE);
}

void de__uploader_retire(de_render_uploader *u, de__render_retire *node, void *object,
                         void (*free_fn)(de_render_uploader *u, void *object)) {
    de_mutex_lock(&u->lock);
    /* Read under the lock, so the list stays in ascending order. */
    const u64 value = de_atomic_load_u64(&u->retire_value, DE_ATOMIC_ACQUIRE);
    /* No frame fence (no renderer), or no frame prepared yet: nothing can name the object. */
    const bool now = !DE_HANDLE_VALID(u->frame_fence) || value == 0;
    if (!now) {
        node->next = NULL;
        node->value = value;
        node->object = object;
        node->free_fn = free_fn;
        if (u->retire_tail != NULL)
            u->retire_tail->next = node;
        else
            u->retire_head = node;
        u->retire_tail = node;
        u->retire_pending++;
    }
    de_mutex_unlock(&u->lock);
    if (now)
        free_fn(u, object);
}

/* Takes the retired objects whose value `completed` has reached (all with UINT64_MAX) off the
 * list; returns the chain. Lock held. */
static de__render_retire *retire_take(de_render_uploader *u, u64 completed) {
    de__render_retire *first = u->retire_head, *last = NULL;
    de__render_retire *n = u->retire_head;
    /* Values are ascending (frames are prepared in order), so stop at the first one not done. */
    while (n != NULL && n->value <= completed) {
        last = n;
        n = n->next;
        u->retire_pending--;
    }
    if (last == NULL)
        return NULL;
    u->retire_head = n;
    if (n == NULL)
        u->retire_tail = NULL;
    last->next = NULL;
    return first;
}

static u32 retire_free(de_render_uploader *u, de__render_retire *chain) {
    u32 freed = 0;
    while (chain != NULL) {
        de__render_retire *next = chain->next;
        chain->free_fn(u, chain->object); /* frees the record holding `chain` */
        chain = next;
        ++freed;
    }
    return freed;
}

u32 de__render_uploader_retire_poll(de_render_uploader *u) {
    if (u == NULL)
        return 0;
    de_mutex_lock(&u->lock);
    de__render_retire *chain = NULL;
    if (u->retire_head != NULL && DE_HANDLE_VALID(u->frame_fence)) {
        u64 completed = de_gpu_fence_value(u->dev, u->frame_fence);
        if (completed > u->test_frame_cap)
            completed = u->test_frame_cap;
        chain = retire_take(u, completed);
    }
    de_mutex_unlock(&u->lock);
    /* Outside the lock: the free functions release uploads, which take it. */
    const u32 freed = retire_free(u, chain);
    if (freed != 0) {
        de_mutex_lock(&u->lock);
        u->retire_freed += freed;
        de_mutex_unlock(&u->lock);
    }
    return freed;
}

void de__uploader_retire_all(de_render_uploader *u) {
    de_mutex_lock(&u->lock);
    de__render_retire *chain = retire_take(u, UINT64_MAX);
    de_mutex_unlock(&u->lock);
    const u32 freed = retire_free(u, chain);
    de_mutex_lock(&u->lock);
    u->retire_freed += freed;
    de_mutex_unlock(&u->lock);
}

void de__render_uploader_retire_stats(de_render_uploader *u, u32 *pending, u64 *freed) {
    de_mutex_lock(&u->lock);
    if (pending != NULL)
        *pending = u->retire_pending;
    if (freed != NULL)
        *freed = u->retire_freed;
    de_mutex_unlock(&u->lock);
}

void de__render_uploader_test_hold_frames(de_render_uploader *u, u64 cap) {
    de_mutex_lock(&u->lock);
    u->test_frame_cap = cap;
    de_mutex_unlock(&u->lock);
}
