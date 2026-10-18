/* command_queue.c: commands to the audio thread, voice slots, and the pending list (DENG-308).
 *
 * Callers (any thread) push fixed-size de__audio_cmd values into `cmd_queue`, a bounded lock-free
 * MPMC queue (core/de_mpmc.h) used as MPSC: the audio thread is its only consumer. A full queue
 * drops the command and counts it; a push never waits. Voice handles are made on the caller's
 * side, from `free_slots` and the slot's atomic generation, so de_audio_play returns the handle at
 * once.
 *
 * The audio thread drains the whole queue at the start of each block. A command whose sample is
 * in the future goes into `pending`, an array sorted by (at_sample, seq) with room for
 * `command_capacity` entries, allocated at create. `seq` is the order in which the audio thread
 * popped the commands, so commands for the same sample apply in arrival order (FIFO).
 */
#include "audio/audio_internal.h"

#include <string.h>

/* ---- Caller side ----------------------------------------------------------------------------- */

bool de__audio_cmd_push(de_audio *a, const de__audio_cmd *cmd) {
    if (de_mpmc_try_push(&a->cmd_queue, cmd))
        return true;
    u32 dropped = de_atomic_fetch_add_u32(&a->cmds_dropped, 1, DE_ATOMIC_RELAXED) + 1;
#if defined(DE_DEBUG) || defined(DE_DEV)
    /* Rate-limited: the 1st, 2nd, 4th, 8th, ... drop. */
    if ((dropped & (dropped - 1)) == 0)
        DE_WARN(a->log, "audio: command queue full (%u commands); %u commands dropped so far",
                a->command_capacity, dropped);
#else
    (void)dropped;
#endif
    return false;
}

bool de__audio_post_fn(de_audio *a, de__audio_cmd_fn fn, void *user, u64 arg, u64 at_sample) {
    if (a == NULL || fn == NULL)
        return false;
    de__audio_cmd c;
    memset(&c, 0, sizeof c);
    c.op = DE__ACMD_FN;
    c.at_sample = at_sample;
    c.u.fn.fn = fn;
    c.u.fn.user = user;
    c.u.fn.arg = arg;
    return de__audio_cmd_push(a, &c);
}

bool de__audio_slot_alloc(de_audio *a, u32 *slot, u32 *gen) {
    u32 s;
    if (!de_mpmc_try_pop(&a->free_slots, &s)) {
        /* Ended voices may wait for the control thread; collect them here instead. */
        de__audio_collect(a);
        if (!de_mpmc_try_pop(&a->free_slots, &s))
            return false;
    }
    *slot = s;
    /* Acquire: pairs with the release that freed the slot. */
    *gen = de_atomic_load_u32(&a->voice_gen[s], DE_ATOMIC_ACQUIRE);
    return true;
}

void de__audio_slot_release(de_audio *a, u32 slot) {
    u32 g = de_atomic_load_u32(&a->voice_gen[slot], DE_ATOMIC_RELAXED);
    de_atomic_store_u32(&a->voice_gen[slot], de__voice_next_gen(g), DE_ATOMIC_RELEASE);
    bool ok = de_mpmc_try_push(&a->free_slots, &slot);
    DE_ASSERT(ok); /* capacity >= slot count */
    (void)ok;
}

u32 de__audio_collect(de_audio *a) {
    u32 n = de__voice_collect_tails(a);
    u32 slot;
    while (de_mpmc_try_pop(&a->retired, &slot)) {
        /* The audio thread retired the slot (generation already bumped) and no longer touches
         * it; the source pointer it left there is ours to close. */
        de__voice *v = &a->voices[slot];
        de__audio_source *src = v->src;
        v->src = NULL;
        /* Free the slot first: whoever sees the source closed can reuse the slot at once. */
        bool ok = de_mpmc_try_push(&a->free_slots, &slot);
        DE_ASSERT(ok);
        (void)ok;
        if (src != NULL && src->vt != NULL && src->vt->close != NULL)
            src->vt->close(src);
        ++n;
    }
    return n;
}

/* ---- Audio thread ---------------------------------------------------------------------------- */

/* Inserts after every entry with at_sample <= c->at_sample (FIFO among equal samples). */
static void pending_insert(de_audio *a, const de__audio_cmd *c) {
    if (a->pending_count == a->command_capacity) {
        /* Cannot happen while the queue and the list have the same capacity and the list is
         * drained every block, unless commands are scheduled far ahead faster than they come due.
         */
        de_atomic_fetch_add_u32(&a->cmds_dropped, 1, DE_ATOMIC_RELAXED);
        if (c->op == DE__ACMD_PLAY) {
            /* The voice was marked scheduled: end it, so its source is closed. */
            u32 slot = de__voice_slot((de_voice){c->voice});
            if (slot < a->voice_count && a->voices[slot].gen == de__voice_gen((de_voice){c->voice}))
                de__voice_retire(a, slot);
        }
        return;
    }
    u32 lo = 0, hi = a->pending_count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        if (a->pending[mid].at_sample <= c->at_sample)
            lo = mid + 1;
        else
            hi = mid;
    }
    memmove(&a->pending[lo + 1], &a->pending[lo], sizeof(de__audio_cmd) * (a->pending_count - lo));
    a->pending[lo] = *c;
    ++a->pending_count;
}

void de__audio_cmd_drain(de_audio *a, u64 block_start) {
    de__audio_cmd c;
    /* At most one queue's worth per block, so producers that never stop cannot hold the block. */
    for (u32 i = 0; i < a->command_capacity && de_mpmc_try_pop(&a->cmd_queue, &c); ++i) {
        c.seq = a->next_seq++;
        if (c.op == DE__ACMD_PLAY)
            de__voice_on_play_queued(a, &c);
        if (c.at_sample > block_start) {
            pending_insert(a, &c);
        } else {
            if (c.at_sample != 0 && c.at_sample < block_start)
                de_atomic_fetch_add_u32(&a->cmds_late, 1, DE_ATOMIC_RELAXED);
            de__audio_cmd_apply(a, &c, block_start);
        }
    }
}

u64 de__audio_pending_next(const de_audio *a) {
    return a->pending_count > 0 ? a->pending[0].at_sample : UINT64_MAX;
}

void de__audio_pending_apply(de_audio *a, u64 sample) {
    u32 n = 0;
    while (n < a->pending_count && a->pending[n].at_sample <= sample)
        ++n;
    if (n == 0)
        return;
    for (u32 i = 0; i < n; ++i)
        de__audio_cmd_apply(a, &a->pending[i], sample);
    a->pending_count -= n;
    memmove(&a->pending[0], &a->pending[n], sizeof(de__audio_cmd) * a->pending_count);
}

void de__audio_cmd_apply(de_audio *a, const de__audio_cmd *cmd, u64 sample) {
    switch (cmd->op) {
    case DE__ACMD_PLAY:
        de__voice_on_play(a, cmd, sample);
        break;
    case DE__ACMD_STOP:
        de__voice_on_stop(a, cmd);
        break;
    case DE__ACMD_FN:
        cmd->u.fn.fn(a, cmd->u.fn.user, cmd->u.fn.arg, sample);
        break;
    case DE__ACMD_VOLUME:
    case DE__ACMD_PITCH:
    case DE__ACMD_POSITION:
        de__voice_on_control(a, cmd);
        break;
    case DE__ACMD_BUS_CREATE:
    case DE__ACMD_BUS_VOLUME:
    case DE__ACMD_BUS_MUTE:
        de__bus_on_control(a, cmd);
        break;
    default:
        break;
    }
}

/* ---- Destroy --------------------------------------------------------------------------------- */

static void close_cmd_source(const de__audio_cmd *c) {
    if (c->op == DE__ACMD_PLAY && c->u.play.src != NULL && c->u.play.src->vt != NULL &&
        c->u.play.src->vt->close != NULL)
        c->u.play.src->vt->close(c->u.play.src);
}

void de__audio_cmd_discard_all(de_audio *a) {
    /* No audio thread runs any more. Queued play commands still own their sources; pending
     * ones were marked scheduled, so their sources are in the voice slots and closed with them
     * (the pending copies are not closed twice). */
    de__audio_cmd c;
    while (de_mpmc_try_pop(&a->cmd_queue, &c))
        close_cmd_source(&c);
    a->pending_count = 0;
}
