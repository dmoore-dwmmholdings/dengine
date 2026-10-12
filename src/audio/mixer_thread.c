/* mixer_thread.c: the output ring and the mixer thread (DENG-308).
 *
 * The ring holds `audio.buffer_blocks` whole mixer blocks. The audio thread renders straight into
 * the next free block and publishes it with the mixer sample of its first frame; the device thread
 * copies any number of frames out (de__audio_ring_read) and frees a block once it has read all of
 * it. One producer, one consumer, no locks (an SPSC ring over whole blocks; core/de_spsc.h needs a
 * power-of-two capacity and copies elements, so the ring is written here).
 *
 * Pacing: the mixer renders while the ring has space and then sleeps on `mixer_wake` (an OS
 * semaphore, as Trama "Decision: A waiting thread blocks in the OS" asks). The consumer posts it
 * only when the mixer says it is waiting (`mixer_waiting`), so the semaphore count stays small.
 * The handshake is the classic one and needs seq_cst on both sides: the mixer stores waiting = 1
 * and then re-reads read_pos; the consumer stores read_pos and then exchanges waiting. At least
 * one of them sees the other's store, so the mixer cannot sleep on a ring that has space.
 */
#include "audio/audio_internal.h"

#include <string.h>

de_result de__audio_ring_init(de_audio *a, u32 blocks) {
    de__audio_ring *r = &a->ring;
    memset(r, 0, sizeof *r);
    r->blocks = blocks;
    r->block_samples = a->block_frames * a->channels;
    size_t data_bytes = (size_t)blocks * r->block_samples * sizeof(f32);
    r->data = (f32 *)DE_ALLOC(&a->alloc, data_bytes, 64, DE_MEM_AUDIO);
    r->block_start =
        (u64 *)DE_ALLOC(&a->alloc, sizeof(u64) * blocks, DE_ALIGNOF(u64), DE_MEM_AUDIO);
    if (r->data == NULL || r->block_start == NULL) {
        de__audio_ring_free(a);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "audio: cannot allocate the output ring");
    }
    memset(r->data, 0, data_bytes);
    memset(r->block_start, 0, sizeof(u64) * blocks);
    de_atomic_init_u64(&r->write_pos, 0);
    de_atomic_init_u64(&r->read_pos, 0);
    return DE_OK;
}

void de__audio_ring_free(de_audio *a) {
    de__audio_ring *r = &a->ring;
    if (r->data != NULL)
        DE_FREE(&a->alloc, r->data, (size_t)r->blocks * r->block_samples * sizeof(f32));
    if (r->block_start != NULL)
        DE_FREE(&a->alloc, r->block_start, sizeof(u64) * r->blocks);
    r->data = NULL;
    r->block_start = NULL;
}

u32 de__audio_ring_space(de_audio *a) {
    de__audio_ring *r = &a->ring;
    u64 w = de_atomic_load_u64(&r->write_pos, DE_ATOMIC_RELAXED);
    u64 rd = de_atomic_load_u64(&r->read_pos, DE_ATOMIC_SEQ_CST);
    return r->blocks - (u32)(w - rd);
}

f32 *de__audio_ring_write_ptr(de_audio *a) {
    de__audio_ring *r = &a->ring;
    u64 w = de_atomic_load_u64(&r->write_pos, DE_ATOMIC_RELAXED);
    return r->data + (size_t)(w % r->blocks) * r->block_samples;
}

void de__audio_ring_publish(de_audio *a, u64 start) {
    de__audio_ring *r = &a->ring;
    u64 w = de_atomic_load_u64(&r->write_pos, DE_ATOMIC_RELAXED);
    r->block_start[w % r->blocks] = start;
    /* Release: the block's samples and start are visible before the consumer sees the block. */
    de_atomic_store_u64(&r->write_pos, w + 1, DE_ATOMIC_RELEASE);
}

/* Consumer: frees block `rd` and wakes the mixer if it sleeps on a full ring. */
static void ring_pop(de_audio *a, u64 rd) {
    de_atomic_store_u64(&a->ring.read_pos, rd + 1, DE_ATOMIC_SEQ_CST);
    if (de_atomic_exchange_u32(&a->mixer_waiting, 0, DE_ATOMIC_SEQ_CST) != 0)
        de_sem_post(&a->mixer_wake, 1);
}

void de__audio_ring_read(void *audio, f32 *out, u32 frames) {
    de_audio *a = (de_audio *)audio;
    de__audio_ring *r = &a->ring;
    const u32 ch = a->channels;
    de__rt_guard_enter();
    while (frames > 0) {
        u64 rd = de_atomic_load_u64(&r->read_pos, DE_ATOMIC_RELAXED);
        /* Acquire: pairs with publish, so the block's samples and start are visible. */
        u64 w = de_atomic_load_u64(&r->write_pos, DE_ATOMIC_ACQUIRE);
        if (rd == w) {
            memset(out, 0, (size_t)frames * ch * sizeof(f32));
            de_atomic_fetch_add_u32(&a->underruns, 1, DE_ATOMIC_RELAXED);
            break;
        }
        const u32 slot = (u32)(rd % r->blocks);
        const u32 avail = a->block_frames - r->read_offset;
        const u32 n = frames < avail ? frames : avail;
        memcpy(out, r->data + (size_t)slot * r->block_samples + (size_t)r->read_offset * ch,
               (size_t)n * ch * sizeof(f32));
        out += (size_t)n * ch;
        frames -= n;
        r->read_offset += n;
        de_atomic_store_u64(&a->play_pos, r->block_start[slot] + r->read_offset, DE_ATOMIC_RELAXED);
        if (r->read_offset == a->block_frames) {
            r->read_offset = 0;
            ring_pop(a, rd);
        }
    }
    de__rt_guard_leave();
}

void de__audio_ring_flush(de_audio *a) {
    de__audio_ring *r = &a->ring;
    u64 rd = de_atomic_load_u64(&r->read_pos, DE_ATOMIC_RELAXED);
    u64 w = de_atomic_load_u64(&r->write_pos, DE_ATOMIC_ACQUIRE);
    if (w == rd)
        return;
    /* Everything up to the end of the newest block is dropped: the next frame the device can
     * play is the first frame of the next block the mixer renders. The clock only moves forward,
     * and the mixer timeline (scheduled commands) is untouched. */
    u64 end = r->block_start[(w - 1) % r->blocks] + a->block_frames;
    if (end > de_atomic_load_u64(&a->play_pos, DE_ATOMIC_RELAXED))
        de_atomic_store_u64(&a->play_pos, end, DE_ATOMIC_RELAXED);
    r->read_offset = 0;
    ring_pop(a, w - 1);
}

/* ---- The mixer thread ------------------------------------------------------------------------ */

static i32 mixer_main(void *user) {
    de_audio *a = (de_audio *)user;
    de_fp_env_init_thread(); /* FTZ/DAZ: denormals in filters and fades cost nothing */
    de__rt_guard_enter();
    while (de_atomic_load_u32(&a->mixer_quit, DE_ATOMIC_ACQUIRE) == 0) {
        if (de_atomic_load_u32(&a->test_stall, DE_ATOMIC_RELAXED) != 0) {
            /* Tests only: stand still (underrun on purpose) without spinning. */
            de_sem_wait_timeout(&a->mixer_wake, 1000000u);
            continue;
        }
        if (de__audio_ring_space(a) == 0) {
            de_atomic_store_u32(&a->mixer_waiting, 1, DE_ATOMIC_SEQ_CST);
            if (de__audio_ring_space(a) == 0 &&
                de_atomic_load_u32(&a->mixer_quit, DE_ATOMIC_SEQ_CST) == 0)
                de_sem_wait(&a->mixer_wake);
            de_atomic_store_u32(&a->mixer_waiting, 0, DE_ATOMIC_RELAXED);
            continue;
        }
        de__mixer_render_block(a);
    }
    de__rt_guard_leave();
    return 0;
}

de_result de__audio_mixer_start(de_audio *a) {
    de_atomic_store_u32(&a->mixer_quit, 0, DE_ATOMIC_RELAXED);
    de_result r = de_thread_create(mixer_main, a, "de-audio-mix", DE_THREAD_REALTIME, 0, &a->mixer);
    if (r != DE_OK)
        return DE_FAIL_WRAP(r, "audio: cannot start the mixer thread");
    return DE_OK;
}

void de__audio_mixer_stop(de_audio *a) {
    if (a->mixer == NULL)
        return;
    de_atomic_store_u32(&a->mixer_quit, 1, DE_ATOMIC_SEQ_CST);
    de_sem_post(&a->mixer_wake, 1);
    de_thread_join(a->mixer);
    a->mixer = NULL;
}
