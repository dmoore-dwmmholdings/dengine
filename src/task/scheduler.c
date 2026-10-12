/*
 * scheduler.c: task system lifetime, submit, wait, the main-thread queue and the record pools
 * (SPEC §8). The worker loops and the work search are in worker.c; the structures and the
 * design notes are in task_internal.h.
 */
#include "task/task_internal.h"

#include <dengine/de_math.h>

#include <stdio.h>
#include <string.h>

#include "core/de_compiler.h"

/* The calling thread's slot. One thread belongs to at most one live task system. */
static DE_THREAD_LOCAL de__task_slot *de__tls_slot;

/* "Cannot pin" is logged once per process, not once per create. */
static de_atomic_u32 de__pin_logged;

/** Upper bound on the worker counts a desc may ask for (sanity check, not a design limit). */
#define DE__TASK_MAX_THREADS 4096u

de__task_slot *de__task_current_slot(const de_task_system *ts) {
    de__task_slot *s = de__tls_slot;
    return (s != NULL && s->ts == ts) ? s : NULL;
}

void de__task_set_current_slot(de__task_slot *slot) {
    de__tls_slot = slot;
}

u32 de_task_worker_index(void) {
    const de__task_slot *s = de__tls_slot;
    return (s != NULL && s->kind == DE__SLOT_FRAME) ? s->index : DE_TASK_WORKER_NONE;
}

u32 de_task_worker_count(const de_task_system *ts) {
    return ts != NULL ? ts->frame_count : 0;
}

/* ---------- Record pools ---------- */

static bool de__slot_add_block(de_task_system *ts, de__task_slot *s) {
    de__task_block *b = DE_NEW(ts->allocator, de__task_block, ts->tag);
    if (b == NULL)
        return false;
    b->next = s->blocks;
    s->blocks = b;
    for (u32 i = DE__TASK_BLOCK_RECORDS; i-- > 0;) {
        de__task *t = &b->records[i];
        memset(t, 0, sizeof *t);
        t->owner = s->index;
        t->next = s->free_local;
        s->free_local = t;
    }
    const u32 total = de_atomic_load_u32(&s->records_total, DE_ATOMIC_RELAXED);
    de_atomic_store_u32(&s->records_total, total + DE__TASK_BLOCK_RECORDS, DE_ATOMIC_RELAXED);
    return true;
}

/* Owner: a free record from the local list, else the whole remote stack, else NULL. */
static de__task *de__slot_take_free(de__task_slot *s) {
    de__task *t = s->free_local;
    if (t == NULL) {
        t = (de__task *)de_atomic_exchange_ptr(&s->remote_free, NULL, DE_ATOMIC_ACQUIRE);
        if (t == NULL)
            return NULL;
    }
    s->free_local = t->next;
    return t;
}

/* Owner (the external slot: under ext_lock): a record, or NULL when memory runs out. */
static de__task *de__slot_alloc(de_task_system *ts, de__task_slot *s) {
    for (;;) {
        de__task *t = de__slot_take_free(s);
        if (t != NULL)
            return t;
        /* A frame slot at its soft limit runs queued work first: that recycles records (its own
         * directly, others' through their remote stacks) instead of growing without bound. */
        if (s->kind == DE__SLOT_FRAME && s->help_depth < DE__TASK_MAX_HELP_DEPTH &&
            de_atomic_load_u32(&s->records_total, DE_ATOMIC_RELAXED) >= DE__TASK_SLOT_SOFT_LIMIT) {
            de__task *w = de__task_find_work(ts, s, false);
            if (w != NULL) {
                ++s->help_depth;
                de__task_execute(ts, s, w);
                --s->help_depth;
                continue;
            }
        }
        if (!de__slot_add_block(ts, s))
            return NULL;
    }
}

/* Returns `t` to the slot it came from. `self` runs on the calling thread. */
static void de__task_recycle(de_task_system *ts, de__task_slot *self, de__task *t) {
    if (t->owner == self->index) {
        t->next = self->free_local;
        self->free_local = t;
        return;
    }
    de__task_slot *o = &ts->slots[t->owner];
    void *head = de_atomic_load_ptr(&o->remote_free, DE_ATOMIC_RELAXED);
    do {
        t->next = (de__task *)head;
    } while (
        !de_atomic_cas_weak_ptr(&o->remote_free, &head, t, DE_ATOMIC_RELEASE, DE_ATOMIC_RELAXED));
}

static void de__slot_free_blocks(de_task_system *ts, de__task_slot *s) {
    while (s->blocks != NULL) {
        de__task_block *next = s->blocks->next;
        DE_FREE(ts->allocator, s->blocks, sizeof(de__task_block));
        s->blocks = next;
    }
    s->free_local = NULL;
    de_atomic_store_ptr(&s->remote_free, NULL, DE_ATOMIC_RELAXED);
}

/* ---------- Counters ---------- */

void de__task_counter_finish(de_task_system *ts, de__task_counter *c) {
    const u64 old = de_atomic_fetch_sub_u64(&c->state, 1, DE_ATOMIC_ACQ_REL);
    DE_ASSERT(DE__COUNTER_PENDING(old) != 0);
    if (DE__COUNTER_PENDING(old) != 1)
        return;
    /* `c` may be gone from here on: the waiter can return as soon as it sees zero. */
    de_atomic_fetch_add_u64(&ts->counter_zero_events, 1, DE_ATOMIC_RELAXED);
    const u32 waiter = DE__COUNTER_WAITER(old);
    if (waiter != 0)
        de_sem_post(&ts->slots[waiter - 1].park, 1);
}

/* Registers slot id `id` (index + 1) as the counter's parked waiter. false when another
 * thread is registered (this one then parks with a short timeout and polls). */
static bool de__counter_register(de__task_counter *c, u32 id) {
    u64 s = de_atomic_load_u64(&c->state, DE_ATOMIC_ACQUIRE);
    for (;;) {
        if (DE__COUNTER_PENDING(s) == 0 || DE__COUNTER_WAITER(s) == id)
            return true;
        if (DE__COUNTER_WAITER(s) != 0)
            return false;
        if (de_atomic_cas_u64(&c->state, &s, s | ((u64)id << 32), DE_ATOMIC_ACQ_REL,
                              DE_ATOMIC_ACQUIRE))
            return true;
    }
}

static void de__counter_unregister(de__task_counter *c, u32 id) {
    u64 s = de_atomic_load_u64(&c->state, DE_ATOMIC_RELAXED);
    while (DE__COUNTER_WAITER(s) == id) {
        if (de_atomic_cas_u64(&c->state, &s, s & 0xFFFFFFFFull, DE_ATOMIC_RELAXED,
                              DE_ATOMIC_RELAXED))
            break;
    }
}

static u32 de__counter_pending(const de__task_counter *c) {
    return DE__COUNTER_PENDING(de_atomic_load_u64(&c->state, DE_ATOMIC_ACQUIRE));
}

bool de_task_done(const de_task_counter *counter) {
    DE_ASSERT(counter != NULL);
    return de__counter_pending(de__counter_c(counter)) == 0;
}

/* ---------- Running ---------- */

void de__task_execute(de_task_system *ts, de__task_slot *self, de__task *t) {
    const de_task_fn fn = t->fn;
    void *const user = t->user;
    de__task_counter *const c = t->counter;
    /* Recycle first: a task that submits finds the record already free. */
    de__task_recycle(ts, self, t);
    fn(user);
    const u64 done = de_atomic_load_u64(&self->completed, DE_ATOMIC_RELAXED);
    de_atomic_store_u64(&self->completed, done + 1, DE_ATOMIC_RELEASE);
    if (c != NULL)
        de__task_counter_finish(ts, c);
}

de__task *de__task_pop_main(de_task_system *ts) {
    if (de_atomic_load_u32(&ts->main_count, DE_ATOMIC_ACQUIRE) == 0)
        return NULL;
    de_mutex_lock(&ts->main_lock);
    de__task *t = ts->main_head;
    if (t != NULL) {
        ts->main_head = t->next;
        if (ts->main_head == NULL)
            ts->main_tail = NULL;
        de_atomic_fetch_sub_u32(&ts->main_count, 1, DE_ATOMIC_RELAXED);
    }
    de_mutex_unlock(&ts->main_lock);
    return t;
}

static void de__fifo_append(de__task **head, de__task **tail, de__task *t) {
    t->next = NULL;
    if (*tail != NULL)
        (*tail)->next = t;
    else
        *head = t;
    *tail = t;
}

/* ---------- Submit ---------- */

/* Out of memory for a record: run the task now where that is allowed. */
static void de__task_run_inline(de_task_system *ts, const de__task_slot *self, de_task_pool pool,
                                de_task_fn fn, void *user) {
    if (pool == DE_TASK_POOL_MAIN_THREAD &&
        !(self != NULL && self->kind == DE__SLOT_FRAME && self->index == 0))
        DE_PANIC("de_task_submit: out of memory for a main-thread task record");
    DE_WARN(ts->log, "de_task_submit: out of memory for a task record; running it inline");
    fn(user);
}

void de_task_submit(de_task_system *ts, de_task_pool pool, de_task_fn fn, void *user,
                    de_task_counter *counter) {
    DE_ASSERT(ts != NULL && fn != NULL);
    DE_ASSERT(pool == DE_TASK_POOL_FRAME || pool == DE_TASK_POOL_BACKGROUND ||
              pool == DE_TASK_POOL_MAIN_THREAD);
    de__task_slot *self = de__task_current_slot(ts);
    de__task *t;
    if (self != NULL) {
        t = de__slot_alloc(ts, self);
    } else {
        de_mutex_lock(&ts->ext_lock);
        t = de__slot_alloc(ts, &ts->slots[ts->external_slot]);
        de_mutex_unlock(&ts->ext_lock);
    }
    if (t == NULL) {
        de__task_run_inline(ts, self, pool, fn, user);
        return;
    }
    t->fn = fn;
    t->user = user;
    t->pool = (u32)pool;
    t->counter = counter != NULL ? de__counter(counter) : NULL;
    if (counter != NULL) {
        const u64 old = de_atomic_fetch_add_u64(&t->counter->state, 1, DE_ATOMIC_RELAXED);
        DE_ASSERT(DE__COUNTER_PENDING(old) != 0xFFFFFFFFu);
        (void)old;
    }
    /* Counted before the task becomes visible, so destroy's quiescence check holds. */
    if (self != NULL) {
        const u64 n = de_atomic_load_u64(&self->submitted, DE_ATOMIC_RELAXED);
        de_atomic_store_u64(&self->submitted, n + 1, DE_ATOMIC_RELEASE);
    } else {
        de_atomic_fetch_add_u64(&ts->ext_submitted, 1, DE_ATOMIC_RELEASE);
    }

    switch (pool) {
    case DE_TASK_POOL_FRAME:
        if (self != NULL && self->kind == DE__SLOT_FRAME) {
            if (!de__deque_push(&self->deque, t)) {
                /* The deque could not grow: run it here rather than lose it. */
                DE_WARN(ts->log, "de_task_submit: deque growth failed; running the task inline");
                de__task_execute(ts, self, t);
                return;
            }
        } else {
            u32 spins = 0;
            while (!de_mpmc_try_push(&ts->inject, &t)) {
                /* Full: the frame workers are draining it. */
                if (++spins < DE__TASK_SPIN_ROUNDS)
                    de_cpu_pause();
                else
                    de_sleep_ns(DE__TASK_EXTERNAL_POLL_NS / 5);
            }
        }
        de__task_wake_one(ts);
        break;
    case DE_TASK_POOL_MAIN_THREAD:
        de_mutex_lock(&ts->main_lock);
        de__fifo_append(&ts->main_head, &ts->main_tail, t);
        de_atomic_fetch_add_u32(&ts->main_count, 1, DE_ATOMIC_SEQ_CST);
        de_mutex_unlock(&ts->main_lock);
        de__task_wake_main(ts);
        break;
    case DE_TASK_POOL_BACKGROUND:
        de_mutex_lock(&ts->bg_lock);
        de__fifo_append(&ts->bg_head, &ts->bg_tail, t);
        de_mutex_unlock(&ts->bg_lock);
        de_sem_post(&ts->bg_sem, 1);
        break;
    }
}

de__task *de__task_pop_background(de_task_system *ts) {
    de_mutex_lock(&ts->bg_lock);
    de__task *t = ts->bg_head;
    if (t != NULL) {
        ts->bg_head = t->next;
        if (ts->bg_head == NULL)
            ts->bg_tail = NULL;
    }
    de_mutex_unlock(&ts->bg_lock);
    return t;
}

/* ---------- Wait ---------- */

static void de__spin_round(void) {
    for (u32 i = 0; i < DE__TASK_PAUSES_PER_ROUND; ++i)
        de_cpu_pause();
}

void de_task_wait(de_task_system *ts, de_task_counter *counter) {
    DE_ASSERT(ts != NULL && counter != NULL);
    de__task_counter *c = de__counter(counter);
    if (de__counter_pending(c) == 0)
        return;
    de__task_slot *self = de__task_current_slot(ts);
    if (self == NULL) {
        /* Not a worker of `ts`: it cannot run tasks, so it polls. */
        u32 spins = 0;
        while (de__counter_pending(c) != 0) {
            if (++spins < DE__TASK_SPIN_ROUNDS)
                de__spin_round();
            else
                de_sleep_ns(DE__TASK_EXTERNAL_POLL_NS);
        }
        return;
    }
    const bool frame = self->kind == DE__SLOT_FRAME;
    const bool is_main = frame && self->index == 0;
    const u32 id = self->index + 1;
    bool registered = false;
    u32 spins = 0;
    while (de__counter_pending(c) != 0) {
        if (frame) {
            de__task *t = de__task_find_work(ts, self, is_main);
            if (t != NULL) {
                de__task_execute(ts, self, t);
                spins = 0;
                continue;
            }
        }
        if (spins < DE__TASK_SPIN_ROUNDS) {
            ++spins;
            de__spin_round();
            continue;
        }
        /* Park. The task that finishes the counter posts our semaphore; on the main thread a
         * main-thread submit does too (parked flag, seq_cst on both sides). */
        if (!registered)
            registered = de__counter_register(c, id);
        if (is_main)
            de_atomic_store_u32(&self->parked, 1, DE_ATOMIC_SEQ_CST);
        if (de__counter_pending(c) != 0 && !(frame && de__task_work_visible(ts, self, is_main)))
            de_sem_wait_timeout(&self->park, registered ? DE__TASK_PARK_REGISTERED_NS
                                                        : DE__TASK_PARK_UNREGISTERED_NS);
        if (is_main)
            de_atomic_store_u32(&self->parked, 0, DE_ATOMIC_RELAXED);
        spins = DE__TASK_SPIN_ROUNDS / 2;
    }
    if (registered)
        de__counter_unregister(c, id);
}

u32 de_task_pump_main(de_task_system *ts, u32 max) {
    DE_ASSERT(ts != NULL);
    de__task_slot *self = de__task_current_slot(ts);
    DE_ASSERT(self != NULL && self->kind == DE__SLOT_FRAME && self->index == 0);
    if (self == NULL || self->kind != DE__SLOT_FRAME || self->index != 0)
        return 0;
    if (max == 0)
        max = de_atomic_load_u32(&ts->main_count, DE_ATOMIC_ACQUIRE);
    u32 n = 0;
    while (n < max) {
        de__task *t = de__task_pop_main(ts);
        if (t == NULL)
            break;
        de__task_execute(ts, self, t);
        ++n;
    }
    return n;
}

/* ---------- Statistics and quiescence ---------- */

de__task_stats de__task_get_stats(const de_task_system *ts) {
    de__task_stats st = {0};
    for (u32 i = 0; i < ts->slot_count; ++i) {
        const de__task_slot *s = &ts->slots[i];
        st.completed += de_atomic_load_u64(&s->completed, DE_ATOMIC_ACQUIRE);
        st.record_blocks +=
            de_atomic_load_u32(&s->records_total, DE_ATOMIC_RELAXED) / DE__TASK_BLOCK_RECORDS;
    }
    for (u32 i = 0; i < ts->slot_count; ++i)
        st.submitted += de_atomic_load_u64(&ts->slots[i].submitted, DE_ATOMIC_ACQUIRE);
    st.submitted += de_atomic_load_u64(&ts->ext_submitted, DE_ATOMIC_ACQUIRE);
    st.counter_zero_events = de_atomic_load_u64(&ts->counter_zero_events, DE_ATOMIC_RELAXED);
    return st;
}

/* No task pending or running (completed is read before submitted; see task_internal.h). */
static bool de__task_quiescent(const de_task_system *ts) {
    const de__task_stats st = de__task_get_stats(ts);
    return st.completed == st.submitted;
}

/* ---------- Create and destroy ---------- */

typedef struct de__task_init_state {
    u32 park_sems; /* slots whose park semaphore is initialised */
    u32 deques;    /* frame slots whose deque is initialised */
    bool idle_sem;
    bool bg_sem;
    bool locks;
    bool inject;
} de__task_init_state;

/* Stops the threads that were started and frees what was initialised. */
static void de__task_teardown(de_task_system *ts, const de__task_init_state *st) {
    de_atomic_store_u32(&ts->shutdown, 1, DE_ATOMIC_SEQ_CST);
    if (st->idle_sem)
        de_sem_post(&ts->idle_sem, ts->frame_count);
    if (st->bg_sem)
        de_sem_post(&ts->bg_sem, ts->bg_count);
    for (u32 i = 0; i < ts->slot_count; ++i) {
        if (ts->slots[i].thread != NULL) {
            de_thread_join(ts->slots[i].thread);
            ts->slots[i].thread = NULL;
        }
    }
    for (u32 i = 0; i < st->deques; ++i)
        de__deque_destroy(&ts->slots[i].deque);
    for (u32 i = 0; i < ts->slot_count; ++i)
        de__slot_free_blocks(ts, &ts->slots[i]);
    for (u32 i = 0; i < st->park_sems; ++i)
        de_sem_destroy(&ts->slots[i].park);
    if (st->inject)
        de_mpmc_destroy(&ts->inject);
    if (st->idle_sem)
        de_sem_destroy(&ts->idle_sem);
    if (st->bg_sem)
        de_sem_destroy(&ts->bg_sem);
    if (st->locks) {
        de_mutex_destroy(&ts->main_lock);
        de_mutex_destroy(&ts->bg_lock);
        de_mutex_destroy(&ts->ext_lock);
    }
    if (de__tls_slot != NULL && de__tls_slot->ts == ts)
        de__tls_slot = NULL;
    const de_allocator *a = ts->allocator;
    DE_FREE(a, ts->slots, sizeof(de__task_slot) * ts->slot_count);
    DE_FREE(a, ts, sizeof *ts);
}

static void de__task_pin(de_task_system *ts) {
    const u32 cores = de_cpu_logical_cores();
    const u32 mask_cores = cores < 64 ? cores : 64;
    for (u32 i = 1; i < ts->frame_count; ++i) {
        const de_result r = de_thread_set_affinity(ts->slots[i].thread, (u64)1 << (i % mask_cores));
        if (r == DE_OK)
            continue;
        if (r == DE_ERR_UNSUPPORTED) {
            if (de_atomic_exchange_u32(&de__pin_logged, 1, DE_ATOMIC_RELAXED) == 0)
                DE_INFO(ts->log, "pin_workers: this OS cannot pin threads; workers run unpinned");
        } else {
            DE_WARN(ts->log, "pin_workers: pinning worker %u failed (%s); workers run unpinned", i,
                    de_result_str(r));
        }
        de_error_clear();
        return;
    }
}

de_result de_task_system_create(const de_task_system_desc *desc, de_task_system **out) {
    if (out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_task_system_create: out is NULL");
    *out = NULL;
    const de_task_system_desc d = desc != NULL ? *desc : (de_task_system_desc){0};
    if (de__tls_slot != NULL)
        return DE_FAIL(DE_ERR_BUSY,
                       "de_task_system_create: this thread already belongs to a task system");
    if (d.worker_count > DE__TASK_MAX_THREADS || d.background_worker_count > DE__TASK_MAX_THREADS)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_task_system_create: %u workers / %u background workers is too many",
                       d.worker_count, d.background_worker_count);
    /* SPEC §5.7 rule 4: the main thread runs tasks too, so it must be in the deterministic FP
     * mode like the workers. */
    DE_ASSERT(de_fp_env_check());

    const de_allocator *a = d.allocator != NULL ? d.allocator : de_heap_allocator();
    const u32 tag = de_mem_register_tag("task");
    const u32 cores = de_cpu_logical_cores();
    const u32 workers = d.worker_count != 0 ? d.worker_count : (cores > 1 ? cores - 1 : 1);
    const u32 bg = d.background_worker_count != 0 ? d.background_worker_count
                                                  : (cores / 4 > 1 ? cores / 4 : 1);

    de_task_system *ts = DE_NEW(a, de_task_system, tag);
    if (ts == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_task_system_create: system");
    memset(ts, 0, sizeof *ts);
    ts->allocator = a;
    ts->tag = tag;
    ts->frame_count = workers + 1;
    ts->bg_count = bg;
    ts->external_slot = ts->frame_count + bg;
    ts->slot_count = ts->external_slot + 1;
    ts->pin_workers = d.pin_workers;
    ts->log = de_log_register_category("task", DE_LOG_INFO);
    ts->slots = (de__task_slot *)DE_ALLOC(a, sizeof(de__task_slot) * ts->slot_count,
                                          DE_ALIGNOF(de__task_slot), tag);
    if (ts->slots == NULL) {
        DE_FREE(a, ts, sizeof *ts);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_task_system_create: %u slots", ts->slot_count);
    }
    memset(ts->slots, 0, sizeof(de__task_slot) * ts->slot_count);

    de__task_init_state st = {0};
    de_result r = DE_OK;
    de_mutex_init(&ts->main_lock);
    de_mutex_init(&ts->bg_lock);
    de_mutex_init(&ts->ext_lock);
    st.locks = true;
    for (u32 i = 0; i < ts->slot_count; ++i) {
        de__task_slot *s = &ts->slots[i];
        s->ts = ts;
        s->index = i;
        s->kind = i < ts->frame_count     ? DE__SLOT_FRAME
                  : i < ts->external_slot ? DE__SLOT_BACKGROUND
                                          : DE__SLOT_EXTERNAL;
        /* splitmix64 of the index: distinct non-zero xorshift seeds. Scheduling only. */
        u64 z = (u64)(i + 1) * 0x9E3779B97F4A7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        s->rng = (z ^ (z >> 31)) | 1;
        if ((r = de_sem_init(&s->park, 0)) != DE_OK)
            goto fail;
        st.park_sems = i + 1;
    }
    for (u32 i = 0; i < ts->frame_count; ++i) {
        if ((r = de__deque_init(&ts->slots[i].deque, DE__TASK_DEQUE_CAPACITY, a, tag)) != DE_OK)
            goto fail;
        st.deques = i + 1;
    }
    if ((r = de_sem_init(&ts->idle_sem, 0)) != DE_OK)
        goto fail;
    st.idle_sem = true;
    if ((r = de_sem_init(&ts->bg_sem, 0)) != DE_OK)
        goto fail;
    st.bg_sem = true;
    const de_mpmc_desc qd = {.capacity = DE__TASK_INJECT_CAPACITY,
                             .elem_size = (u32)sizeof(de__task *),
                             .tag = tag,
                             .allocator = a};
    if ((r = de_mpmc_init(&ts->inject, &qd)) != DE_OK)
        goto fail;
    st.inject = true;

    /* The creating thread is frame worker 0. */
    de__tls_slot = &ts->slots[0];

    char name[32];
    for (u32 i = 1; i < ts->frame_count; ++i) {
        snprintf(name, sizeof name, "de-worker-%u", i);
        r = de_thread_create(de__task_frame_worker_main, &ts->slots[i], name, DE_THREAD_NORMAL, 0,
                             &ts->slots[i].thread);
        if (r != DE_OK)
            goto fail;
    }
    for (u32 i = 0; i < bg; ++i) {
        de__task_slot *s = &ts->slots[ts->frame_count + i];
        snprintf(name, sizeof name, "de-bg-%u", i);
        r = de_thread_create(de__task_background_worker_main, s, name, DE_THREAD_LOW, 0,
                             &s->thread);
        if (r != DE_OK)
            goto fail;
    }
    if (ts->pin_workers)
        de__task_pin(ts);
    *out = ts;
    return DE_OK;

fail:
    de__task_teardown(ts, &st);
    return DE_FAIL_WRAP(r, "de_task_system_create failed");
}

void de_task_system_destroy(de_task_system *ts) {
    if (ts == NULL)
        return;
    de__task_slot *self = &ts->slots[0];
    DE_ASSERT(de__tls_slot == self);
    /* Drain: run frame and main-thread work here until nothing is pending or running anywhere
     * (background tasks run on their own threads; this thread sleeps while they do). */
    u32 spins = 0;
    for (;;) {
        de__task *t = de__task_find_work(ts, self, true);
        if (t != NULL) {
            de__task_execute(ts, self, t);
            spins = 0;
            continue;
        }
        if (de__task_quiescent(ts))
            break;
        if (spins < DE__TASK_SPIN_ROUNDS) {
            ++spins;
            de__spin_round();
            continue;
        }
        de_atomic_store_u32(&self->parked, 1, DE_ATOMIC_SEQ_CST);
        if (!de__task_work_visible(ts, self, true))
            de_sem_wait_timeout(&self->park, DE__TASK_PARK_UNREGISTERED_NS * 10);
        de_atomic_store_u32(&self->parked, 0, DE_ATOMIC_RELAXED);
    }
    const de__task_init_state all = {
        .park_sems = ts->slot_count,
        .deques = ts->frame_count,
        .idle_sem = true,
        .bg_sem = true,
        .locks = true,
        .inject = true,
    };
    de__task_teardown(ts, &all);
}
