/*
 * worker.c: the frame and background worker loops, the work search (own deque, injection
 * queue, random-victim stealing) and idle parking (SPEC §8).
 *
 * Idle parking. A frame worker that found nothing for DE__TASK_SPIN_ROUNDS rounds parks on
 * the shared idle semaphore. The lost-wake-up race is closed Dekker-style with seq_cst fences on
 * both sides (C11 7.17.4: two seq_cst fences order the operations around them):
 *   worker:    sleepers += 1 (seq_cst RMW); fence; look at the queues; sem_wait
 *   submitter: push the task;                fence; read sleepers; if > 0, take one and post
 * Either the worker sees the task, or the submitter sees the sleeper. A worker that saw work
 * after announcing itself takes its announcement back; if a submitter already took it, a post is
 * on its way, and the worker consumes it with sem_wait (it returns at once).
 */
#include "task/task_internal.h"

#include <dengine/de_debug.h>
#include <dengine/de_math.h>

static u32 de__xorshift(de__task_slot *s) {
    u64 x = s->rng;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    s->rng = x;
    return (u32)(x >> 32);
}

de__task *de__task_find_work(de_task_system *ts, de__task_slot *self, bool with_main) {
    DE_ASSERT(self->kind == DE__SLOT_FRAME);
    if (with_main && self->index == 0) {
        de__task *t = de__task_pop_main(ts);
        if (t != NULL)
            return t;
    }
    de__task *t = (de__task *)de__deque_pop(&self->deque);
    if (t != NULL)
        return t;
    if (de_mpmc_try_pop(&ts->inject, &t))
        return t;
    const u32 n = ts->frame_count;
    if (n < 2)
        return NULL;
    /* Random first victim, then every other worker once. The randomness spreads thieves over
     * victims; it never changes results (SPEC §5.7 rule 5). */
    const u32 start = de__xorshift(self) % n;
    for (u32 i = 0; i < n; ++i) {
        const u32 v = (start + i) % n;
        if (v == self->index)
            continue;
        for (u32 attempt = 0; attempt < 4; ++attempt) {
            void *item = NULL;
            const de__steal_result r = de__deque_steal(&ts->slots[v].deque, &item);
            if (r == DE__STEAL_OK)
                return (de__task *)item;
            if (r == DE__STEAL_EMPTY)
                break;
            de_cpu_pause(); /* lost a race: the victim still has work, try again */
        }
    }
    return NULL;
}

bool de__task_work_visible(de_task_system *ts, const de__task_slot *self, bool with_main) {
    if (with_main && self->index == 0 &&
        de_atomic_load_u32(&ts->main_count, DE_ATOMIC_SEQ_CST) != 0)
        return true;
    if (de_mpmc_size_approx(&ts->inject) != 0)
        return true;
    for (u32 i = 0; i < ts->frame_count; ++i) {
        if (de__deque_size_approx(&ts->slots[i].deque) != 0)
            return true;
    }
    return false;
}

void de__task_wake_one(de_task_system *ts) {
    de_atomic_thread_fence(DE_ATOMIC_SEQ_CST);
    u32 s = de_atomic_load_u32(&ts->sleepers, DE_ATOMIC_RELAXED);
    while (s != 0) {
        if (de_atomic_cas_weak_u32(&ts->sleepers, &s, s - 1, DE_ATOMIC_ACQ_REL,
                                   DE_ATOMIC_RELAXED)) {
            de_sem_post(&ts->idle_sem, 1);
            return;
        }
    }
}

void de__task_wake_main(de_task_system *ts) {
    /* Pairs with the main thread's seq_cst store of `parked` before it rechecks main_count. */
    if (de_atomic_exchange_u32(&ts->slots[0].parked, 0, DE_ATOMIC_SEQ_CST) != 0)
        de_sem_post(&ts->slots[0].park, 1);
}

/* Takes back this worker's sleeper announcement. false when a submitter took it first (a post
 * for it is coming). */
static bool de__cancel_sleep(de_task_system *ts) {
    u32 s = de_atomic_load_u32(&ts->sleepers, DE_ATOMIC_RELAXED);
    while (s != 0) {
        if (de_atomic_cas_weak_u32(&ts->sleepers, &s, s - 1, DE_ATOMIC_ACQ_REL, DE_ATOMIC_RELAXED))
            return true;
    }
    return false;
}

/* Every engine-created worker thread: deterministic FP mode (SPEC §5.7 rule 4) and a crash
 * alt-stack (DENG-251) before the first task. */
static void de__worker_thread_init(de__task_slot *self) {
    de_fp_env_init_thread();
    de_crash_thread_init();
    de__task_set_current_slot(self);
}

i32 de__task_frame_worker_main(void *p) {
    de__task_slot *self = (de__task_slot *)p;
    de_task_system *ts = self->ts;
    de__worker_thread_init(self);
    u32 spins = 0;
    for (;;) {
        de__task *t = de__task_find_work(ts, self, false);
        if (t != NULL) {
            de__task_execute(ts, self, t);
            spins = 0;
            continue;
        }
        if (de_atomic_load_u32(&ts->shutdown, DE_ATOMIC_ACQUIRE) != 0)
            break;
        if (spins < DE__TASK_SPIN_ROUNDS) {
            ++spins;
            for (u32 i = 0; i < DE__TASK_PAUSES_PER_ROUND; ++i)
                de_cpu_pause();
            continue;
        }
        de_atomic_fetch_add_u32(&ts->sleepers, 1, DE_ATOMIC_SEQ_CST);
        de_atomic_thread_fence(DE_ATOMIC_SEQ_CST);
        if (de_atomic_load_u32(&ts->shutdown, DE_ATOMIC_ACQUIRE) != 0 ||
            de__task_work_visible(ts, self, false)) {
            if (!de__cancel_sleep(ts))
                de_sem_wait(&ts->idle_sem);
        } else {
            de_sem_wait(&ts->idle_sem);
        }
        spins = 0;
    }
    de__task_set_current_slot(NULL);
    return 0;
}

i32 de__task_background_worker_main(void *p) {
    de__task_slot *self = (de__task_slot *)p;
    de_task_system *ts = self->ts;
    de__worker_thread_init(self);
    for (;;) {
        /* One post per queued task, plus one per thread at shutdown (after the queue is empty). */
        de_sem_wait(&ts->bg_sem);
        de__task *t = de__task_pop_background(ts);
        if (t != NULL) {
            de__task_execute(ts, self, t);
            continue;
        }
        if (de_atomic_load_u32(&ts->shutdown, DE_ATOMIC_ACQUIRE) != 0)
            break;
    }
    de__task_set_current_slot(NULL);
    return 0;
}
