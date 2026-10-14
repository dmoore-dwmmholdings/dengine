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
#include "task/profile.h"

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
    de_sem_post(&self->ts->start_sem, 1); /* DENG-416: started; create may return */
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
            DE__TASK_PROF_PLOT_PARKED(&ts->sleepers);
            DE_PROFILE_ZONE_BEGIN(park_zone, "task.park");
            de_sem_wait(&ts->idle_sem);
            DE_PROFILE_ZONE_END(park_zone);
            DE__TASK_PROF_PLOT_PARKED(&ts->sleepers);
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
#if DE__TASK_BG_LOWERS_ITSELF
    /* Started at normal priority (task_internal.h). The creator posts the park semaphore once
     * `self->thread` is stored. */
    de_sem_wait(&self->park);
    if (de_thread_set_priority(self->thread, DE_THREAD_LOW) != DE_OK)
        de_error_clear();
#endif
    for (;;) {
        /* One post per queued task, plus one per thread at shutdown (after the queue is empty). */
        de_sem_wait(&ts->bg_sem);
        de__task *t = de__task_pop_background(ts);
        if (t != NULL) {
            if (ts->bg_boost) {
                de_atomic_store_u32(&self->bg_holding, 1, DE_ATOMIC_RELAXED);
                de_atomic_fetch_sub_u32(&ts->bg_queued, 1, DE_ATOMIC_RELAXED);
                de__bg_event(ts, self, false);
            }
            de__task_execute(ts, self, t);
            if (ts->bg_boost) {
                /* The bookkeeping before the event: the event may lower this thread, and a
                 * lowered thread may not run again for a while. */
                de_atomic_store_u32(&self->bg_holding, 0, DE_ATOMIC_RELAXED);
                de_atomic_fetch_sub_u32(&ts->bg_pending, 1, DE_ATOMIC_RELEASE);
                de__bg_event(ts, self, true);
            }
            continue;
        }
        if (de_atomic_load_u32(&ts->shutdown, DE_ATOMIC_ACQUIRE) != 0)
            break;
    }
    de__task_set_current_slot(NULL);
    return 0;
}

/* ---------- Background boost (DENG-388; design at the top of task_internal.h) ---------- */

void de__bg_note_submit(de_task_system *ts) {
    if (!ts->bg_boost)
        return;
    de_atomic_fetch_add_u32(&ts->bg_queued, 1, DE_ATOMIC_RELAXED);
    /* Pairs with the monitor's seq_cst store of bg_monitor_idle before it rechecks bg_pending:
     * either the monitor sees the pending task, or this submit sees the monitor idle. */
    if (de_atomic_fetch_add_u32(&ts->bg_pending, 1, DE_ATOMIC_SEQ_CST) == 0 &&
        de_atomic_exchange_u32(&ts->bg_monitor_idle, 0, DE_ATOMIC_SEQ_CST) != 0)
        de_sem_post(&ts->bg_monitor_sem, 1);
}

/* Sets a background worker's priority. A failure other than "unsupported" is counted and logged
 * once; "unsupported" (macOS, Linux with DE_TASK_BG_BOOST_ON) stops further attempts, and the
 * monitor still runs the policy. */
static void de__bg_set_priority(de_task_system *ts, de__task_slot *w, de_thread_priority prio) {
    if (de_atomic_load_u32(&ts->bg_can_boost, DE_ATOMIC_RELAXED) == 0)
        return;
    const de_result r = de_thread_set_priority(w->thread, prio);
    if (r == DE_OK)
        return;
    if (r == DE_ERR_UNSUPPORTED) {
        de_atomic_store_u32(&ts->bg_can_boost, 0, DE_ATOMIC_RELAXED);
    } else if (de_atomic_fetch_add_u32(&ts->bg_prio_failures, 1, DE_ATOMIC_RELAXED) == 0) {
        const de_error *e = de_error_last();
        DE_WARN(ts->log, "background boost: cannot change the priority of de-bg-%u: %s",
                w->index - ts->frame_count, e != NULL ? e->message : de_result_str(r));
    }
    de_error_clear();
}

/* Lowers `w` again if it is still raised (by the worker itself when it gives the core back, or
 * by the monitor at the end of the slice) and books the raised time. Lowering comes last: a
 * worker that lowers itself is preempted at once when the cores are busy, and anything after it
 * waits for the next raise (CI booked 30-150 ms raises that way, which emptied the budget). */
static void de__bg_lower(de_task_system *ts, de__task_slot *w) {
    if (de_atomic_exchange_u32(&w->bg_boosted, 0, DE_ATOMIC_ACQ_REL) == 0)
        return;
    const u64 from = de_atomic_load_u64(&w->bg_boost_from, DE_ATOMIC_RELAXED);
    const u64 now = de_time_now_ns();
    const u64 ns = now > from ? now - from : 0;
    de_atomic_fetch_add_u64(&ts->bg_boost_used, ns, DE_ATOMIC_RELAXED);
    de_atomic_fetch_add_u64(&ts->bg_boost_ns, ns, DE_ATOMIC_RELAXED);
    de__bg_set_priority(ts, w, DE_THREAD_LOW);
}

void de__bg_event(de_task_system *ts, de__task_slot *self, bool yields) {
    de_atomic_fetch_add_u32(&self->bg_events, 1, DE_ATOMIC_RELAXED);
    if (yields && de_atomic_load_u32(&self->bg_boosted, DE_ATOMIC_RELAXED) != 0)
        de__bg_lower(ts, self);
}

/* Raises `w` (the monitor). The priority is in place before the flag is published, so the
 * worker's own lowering always comes after it. */
static void de__bg_raise(de_task_system *ts, de__task_slot *w, u64 now) {
    de__bg_set_priority(ts, w, DE_THREAD_REALTIME);
    de_atomic_store_u64(&w->bg_boost_from, now, DE_ATOMIC_RELAXED);
    de_atomic_store_u32(&w->bg_boosted, 1, DE_ATOMIC_RELEASE);
    de_atomic_fetch_add_u64(&ts->bg_boosts, 1, DE_ATOMIC_RELAXED);
}

static bool de__bg_stopping(const de_task_system *ts) {
    return de_atomic_load_u32(&ts->shutdown, DE_ATOMIC_ACQUIRE) != 0;
}

static void de__bg_snapshot(de_task_system *ts) {
    for (u32 i = 0; i < ts->bg_count; ++i) {
        de__task_slot *w = &ts->slots[ts->frame_count + i];
        w->bg_seen = de_atomic_load_u32(&w->bg_events, DE_ATOMIC_RELAXED);
    }
}

i32 de__task_bg_monitor_main(void *p) {
    de_task_system *ts = (de_task_system *)p;
    de_crash_thread_init();
    de__task_set_current_slot(
        NULL); /* DENG-416: touch thread-local storage before create returns */
    de_sem_post(&ts->start_sem, 1);
    i64 credit = (i64)DE__TASK_BG_CREDIT_CAP_NS; /* raised time the budget allows (ns) */
    u64 credit_at = de_time_now_ns();
    bool watching = false; /* bg_seen holds the counts of the previous check */
    while (!de__bg_stopping(ts)) {
        if (de_atomic_load_u32(&ts->bg_pending, DE_ATOMIC_ACQUIRE) == 0) {
            /* Nothing pending: block until a submit takes bg_pending from 0 to 1. */
            de_atomic_store_u32(&ts->bg_monitor_idle, 1, DE_ATOMIC_SEQ_CST);
            if (de_atomic_load_u32(&ts->bg_pending, DE_ATOMIC_SEQ_CST) == 0 && !de__bg_stopping(ts))
                de_sem_wait(&ts->bg_monitor_sem);
            else if (de_atomic_exchange_u32(&ts->bg_monitor_idle, 0, DE_ATOMIC_SEQ_CST) == 0)
                de_sem_wait(&ts->bg_monitor_sem); /* a submit took the flag: its post is coming */
            de_atomic_store_u32(&ts->bg_monitor_idle, 0, DE_ATOMIC_RELAXED);
            watching = false;
            continue;
        }
        if (!watching) {
            de__bg_snapshot(ts);
            watching = true;
        }
        /* Only shutdown posts the semaphore while the monitor is not idle (a late post of an
         * earlier idle period only makes one check early). */
        (void)de_sem_wait_timeout(&ts->bg_monitor_sem, DE__TASK_BG_CHECK_NS);
        if (de__bg_stopping(ts))
            break;

        de_atomic_fetch_add_u64(&ts->bg_checks, 1, DE_ATOMIC_RELAXED);
        /* The budget: refill by elapsed time, charge the raised time booked since. */
        u64 now = de_time_now_ns();
        credit += (i64)((now - credit_at) / DE__TASK_BG_SHARE_DIV);
        credit_at = now;
        credit -= (i64)de_atomic_exchange_u64(&ts->bg_boost_used, 0, DE_ATOMIC_RELAXED);
        if (credit > (i64)DE__TASK_BG_CREDIT_CAP_NS)
            credit = (i64)DE__TASK_BG_CREDIT_CAP_NS;

        const bool queued = de_atomic_load_u32(&ts->bg_queued, DE_ATOMIC_RELAXED) != 0;
        bool raised = false;
        for (u32 i = 0; i < ts->bg_count && credit > 0; ++i) {
            de__task_slot *w = &ts->slots[ts->frame_count + i];
            const bool quiet = de_atomic_load_u32(&w->bg_events, DE_ATOMIC_RELAXED) == w->bg_seen;
            const bool wants_cpu =
                de_atomic_load_u32(&w->bg_holding, DE_ATOMIC_RELAXED) != 0 || queued;
            if (quiet && wants_cpu && de_atomic_load_u32(&w->bg_boosted, DE_ATOMIC_RELAXED) == 0) {
                de__bg_raise(ts, w, now);
                raised = true;
            }
        }
        if (raised) {
            /* The slice: a raised worker usually lowers itself sooner, at its next event. */
            (void)de_sem_wait_timeout(&ts->bg_monitor_sem, DE__TASK_BG_SLICE_NS);
            for (u32 i = 0; i < ts->bg_count; ++i)
                de__bg_lower(ts, &ts->slots[ts->frame_count + i]);
        }
        de__bg_snapshot(ts);
    }
    for (u32 i = 0; i < ts->bg_count; ++i)
        de__bg_lower(ts, &ts->slots[ts->frame_count + i]);
    return 0;
}
