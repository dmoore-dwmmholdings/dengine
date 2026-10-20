/*
 * task_internal.h: the task system's internal structures, shared by scheduler.c, worker.c and
 * the later task-graph code (DENG-292, DENG-293). Internal to src/task/; include as
 * "task/task_internal.h". Keep internal structs here, not in .c files.
 *
 * Slots. Every thread that can own task records has a slot (de__task_slot), in one array:
 *   [0, frame_count)                       frame workers; slot 0 is the main thread
 *   [frame_count, frame_count + bg_count)  background workers
 *   [external_slot]                        shared by every other thread, behind ext_lock
 * A slot owns a record pool (blocks of DE__TASK_BLOCK_RECORDS records), a park semaphore, and,
 * for frame workers, a Chase-Lev deque.
 *
 * Records. A task record is taken from the submitting thread's slot and goes back to that slot
 * when the task has run: onto its local free list when the same thread ran it, otherwise onto
 * its `remote_free` stack (lock-free push by any thread; the owner takes the whole stack with
 * one exchange, so there is no ABA). Creation reserves one block for every slot,
 * including workers that do not submit during application warm-up. Larger workloads
 * grow their pools on demand; after warming those high-water marks, submits allocate nothing.
 *
 * Counters. de_task_counter's opaque bytes hold de__task_counter: one u64 with the pending
 * count in the low 32 bits and, in the high 32 bits, the waiter id of a thread parked on it
 * (0 = none). The task that takes the count to zero reads the waiter in the same atomic
 * operation and posts that waiter's park semaphore, and never touches the counter again, so the
 * waiter may free the counter as soon as it sees zero. Waiter ids: 1..slot_count is a slot
 * (index + 1); slot_count + 1 + k is external wait record k.
 *
 * External wait records (DENG-375). A thread that is not a worker of the system cannot run
 * tasks, so it must block in the OS while it waits: a thread that spins instead takes a core
 * from the threads that run the tasks it waits for. On Windows, which schedules strictly by
 * priority, a few such spinners at normal priority starved the background workers
 * (THREAD_PRIORITY_BELOW_NORMAL) for seconds, until the OS's anti-starvation boost ran them.
 * A non-worker waiter therefore takes one of DE__TASK_EXT_WAITERS records (a park semaphore,
 * claimed in the `ext_wait_free` bitmap) for the length of the wait, registers it in the counter
 * like a worker slot, and parks on it.
 *
 * Background boost (DENG-388). Windows runs a below-normal thread only when no thread of normal
 * or higher priority is ready, so while normal threads keep every core busy the background
 * workers (DE_THREAD_LOW) got no CPU until the OS's own anti-starvation boost, about 4 s later.
 * When the boost is on (de_task_bg_boost; Windows by default) a monitor thread looks at the
 * background workers every DE__TASK_BG_CHECK_NS while background tasks are pending:
 *   - Each background worker counts *events* (`bg_events`): a task starts or ends, its thread
 *     parks inside de_task_wait or returns from the park. A parked worker wakes on its park
 *     timeout (2 ms, at most a 15.6 ms tick on Windows) and counts again, so a worker that should
 *     run counts at least once per check unless it is not getting CPU time, or computes.
 *   - A worker is *stalled* when it counted nothing since the last check and either holds a task
 *     (`bg_holding`) or is idle while tasks are queued (`bg_queued`; its semaphore was posted).
 *   - The monitor raises each stalled worker to DE_THREAD_REALTIME (time-critical, priority 15,
 *     the level of Windows' own anti-starvation boost: above normal was not enough, because a
 *     thread woken from a wait runs one quantum one level higher) and sets `bg_boosted`. The
 *     worker lowers itself to DE_THREAD_LOW when it gives the core back (its task ends, or it
 *     parks), after its bookkeeping, because a lowered thread may not run again for a while.
 *     The monitor lowers it after DE__TASK_BG_SLICE_NS if it is still raised (a long task). So a
 *     starved short step runs to its end at once.
 *   - Startup: the monitor is created first and runs time-critical; on Windows the background
 *     workers start at normal priority and lower themselves once started
 *     (DE__TASK_BG_LOWERS_ITSELF). A thread that starts below normal can be starved while it
 *     holds the OS loader lock, which every other starting thread needs; on CI that delayed the
 *     monitor's first check by up to 5 s.
 *   - Raised time is charged to a budget: it refills at 1/DE__TASK_BG_SHARE_DIV of wall time, up
 *     to DE__TASK_BG_CREDIT_CAP_NS, and the monitor raises nobody while it is empty. That bounds
 *     what background work takes from fully busy frame workers: 1/8 of one core, all background
 *     workers together, after a burst of at most the cap.
 * The monitor blocks on `bg_monitor_sem` while nothing is pending; the submit that takes
 * `bg_pending` from 0 to 1 posts it when `bg_monitor_idle` is set (Dekker-style, seq_cst on both
 * sides, like the idle frame workers in worker.c).
 *
 * Quiescence (destroy). Each slot counts the tasks its thread submitted and the tasks it ran,
 * each counter written by one thread only. If the sum of `completed` read first equals the sum
 * of `submitted` read afterwards, no task was pending or running at the first read, and only a
 * running task could have submitted more.
 */
#ifndef DE_TASK_INTERNAL_H
#define DE_TASK_INTERNAL_H

#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_task.h>

#include "core/de_mpmc.h"
#include "platform/de_atomic.h"
#include "task/deque.h"

/* ---------- Tuning ---------- */

/** Records per pool block. */
#define DE__TASK_BLOCK_RECORDS 256u
/** Records a frame slot may own before a submit that finds no free record first runs queued
 * work to recycle one (bounds the memory of a long submit loop). */
#define DE__TASK_SLOT_SOFT_LIMIT (64u * 1024u)
/** Nested "run work to free a record" levels inside submit. */
#define DE__TASK_MAX_HELP_DEPTH 2u
/** Global injection queue capacity (non-worker frame submits). */
#define DE__TASK_INJECT_CAPACITY 4096u
/** Initial deque capacity per frame worker. */
#define DE__TASK_DEQUE_CAPACITY 1024u
/** Rounds of "look for work" a thread spins (with de_cpu_pause) before it parks. */
#define DE__TASK_SPIN_ROUNDS 64u
/** de_cpu_pause() calls per spin round. */
#define DE__TASK_PAUSES_PER_ROUND 32u
/** Nested de_task_wait levels on one thread that help with any task. A wait nested deeper only
 * runs its own deque, the injection queue and (main thread) main-thread tasks, and never steals,
 * so a thread's stack stays bounded (DENG-292; design note in graph.c). */
#define DE__TASK_MAX_WAIT_DEPTH 32u
/** Park timeout of a waiting thread registered on its counter, and of one that is not
 * (another thread is registered). The timeouts are a safety net, not the wake-up path. */
#define DE__TASK_PARK_REGISTERED_NS   (2ull * 1000 * 1000)
#define DE__TASK_PARK_UNREGISTERED_NS (100ull * 1000)
/** Wait records for threads that are not workers (see the top of this file). A non-worker
 * waiter that finds none free (more than this many wait at once) polls, parking on `ext_nap`. */
#define DE__TASK_EXT_WAITERS 32u
/** Pause of a non-worker submitter that finds the injection queue full. */
#define DE__TASK_INJECT_FULL_PAUSE_NS (10ull * 1000)
/** Background boost (see the top of this file). The monitor's check interval and the longest
 * time a stalled worker stays raised (both timed waits: on Windows they end on a timer tick, up
 * to 15.6 ms later). */
#define DE__TASK_BG_CHECK_NS (10ull * 1000 * 1000)
#define DE__TASK_BG_SLICE_NS (2ull * 1000 * 1000)
/** Windows: background workers start at normal priority and lower themselves to DE_THREAD_LOW
 * once started. A thread that starts below normal can be starved while it holds the loader lock,
 * and every thread started after it then waits too (DENG-388). Elsewhere the priority cannot be
 * changed afterwards, and lower priorities still get a share of the CPU. */
#if defined(_WIN32)
#define DE__TASK_BG_LOWERS_ITSELF 1
#else
#define DE__TASK_BG_LOWERS_ITSELF 0
#endif
/** The raised-time budget refills at 1/DE__TASK_BG_SHARE_DIV of wall time, up to the cap. */
#define DE__TASK_BG_SHARE_DIV     8u
#define DE__TASK_BG_CREDIT_CAP_NS (32ull * 1000 * 1000)

/* ---------- Records and counters ---------- */

typedef struct de__task_counter {
    de_atomic_u64 state; /* low 32: pending tasks; high 32: parked waiter slot + 1 */
    u64 reserved;        /* zero; free for DENG-292/293 */
} de__task_counter;

_Static_assert(sizeof(de__task_counter) <= sizeof(de_task_counter),
               "de__task_counter must fit in de_task_counter");
_Static_assert(DE_ALIGNOF(de__task_counter) <= DE_ALIGNOF(de_task_counter),
               "de__task_counter alignment exceeds de_task_counter's");

#define DE__COUNTER_PENDING(s) ((u32)((s) & 0xFFFFFFFFull))
#define DE__COUNTER_WAITER(s)  ((u32)((s) >> 32))

static inline de__task_counter *de__counter(de_task_counter *c) {
    return (de__task_counter *)(void *)c->opaque;
}
static inline const de__task_counter *de__counter_c(const de_task_counter *c) {
    return (const de__task_counter *)(const void *)c->opaque;
}

typedef struct de__task {
    struct de__task *next; /* free lists, the main-thread and background FIFOs */
    de_task_fn fn;
    void *user;
    de__task_counter *counter; /* NULL: nobody waits */
    u32 owner;                 /* slot index of the pool the record came from */
    u32 pool;                  /* de_task_pool */
} de__task;

typedef struct de__task_block {
    struct de__task_block *next;
    de__task records[DE__TASK_BLOCK_RECORDS];
} de__task_block;

/* ---------- Slots ---------- */

typedef enum de__slot_kind { DE__SLOT_FRAME, DE__SLOT_BACKGROUND, DE__SLOT_EXTERNAL } de__slot_kind;

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push)
#pragma warning(disable : 4324) /* structure was padded due to alignment specifier: intended */
#endif

typedef struct de__task_slot {
    de__deque deque; /* frame slots only (initialised for every slot; others stay empty) */

    /* Owner-only state (the external slot: under ext_lock). */
    DE_ALIGN(DE_CACHE_LINE) de__task *free_local;
    de__task_block *blocks;
    de_atomic_u32 records_total; /* written by the owner only */
    u32 help_depth;
    u32 wait_depth; /* de_task_wait calls active on this thread (DENG-292) */
    u64 rng;        /* xorshift64 state for victim selection; affects scheduling only */

    /* Written by the owner only, read by destroy. */
    de_atomic_u64 submitted;
    de_atomic_u64 completed;

    /* Written by other threads. */
    DE_ALIGN(DE_CACHE_LINE) de_atomic_ptr remote_free; /* de__task * Treiber stack */
    de_atomic_u32 parked; /* slot 0: 1 while parked in a wait (main-thread queue wake-up) */
    de_sem park;

    /* Background boost (background slots; DENG-388, see the top of this file). */
    de_atomic_u32 bg_events;     /* written by the worker */
    de_atomic_u32 bg_holding;    /* 1 while the worker runs a task (written by the worker) */
    de_atomic_u32 bg_boosted;    /* 1 while raised: set by the monitor, taken by either */
    de_atomic_u64 bg_boost_from; /* when the monitor raised it */
    u32 bg_seen;                 /* monitor only: bg_events at the last check */

    /* Read-only after create. */
    struct de_task_system *ts;
    de_thread *thread; /* NULL for slot 0 and the external slot */
    u32 index;
    de__slot_kind kind;
} de__task_slot;

struct de_task_system {
    de_mpmc inject; /* de__task * from non-worker frame submits */

    DE_ALIGN(DE_CACHE_LINE) de_atomic_u32 sleepers; /* idle frame workers in idle_sem */
    de_atomic_u32 shutdown;
    de_sem idle_sem;

    /* Main-thread FIFO (drained by slot 0 only). */
    DE_ALIGN(DE_CACHE_LINE) de_atomic_u32 main_count;
    de_mutex main_lock;
    de__task *main_head;
    de__task *main_tail;

    /* Background FIFO; bg_sem counts queued tasks plus shutdown posts. */
    DE_ALIGN(DE_CACHE_LINE) de_mutex bg_lock;
    de__task *bg_head;
    de__task *bg_tail;
    de_sem bg_sem;

    /* The external slot's pool and its submitted count (any thread). */
    de_mutex ext_lock;
    de_atomic_u64 ext_submitted;

    /* External wait records (DENG-375): bit k set = ext_park[k] is free. `ext_nap` is never
     * posted; a timed wait on it is an OS sleep. */
    DE_ALIGN(DE_CACHE_LINE) de_atomic_u32 ext_wait_free;
    de_sem ext_park[DE__TASK_EXT_WAITERS];
    de_sem ext_nap;

    /* Background boost (DENG-388; see the top of this file). */
    DE_ALIGN(DE_CACHE_LINE) de_atomic_u32 bg_pending; /* submitted, not finished */
    de_atomic_u32 bg_queued;                          /* submitted, not started */
    de_atomic_u32 bg_can_boost;     /* 0 once the OS said priority changes are unsupported */
    de_atomic_u32 bg_prio_failures; /* other failed priority changes (logged once) */
    de_atomic_u64 bg_boost_used;    /* raised time (ns) not yet charged to the budget */
    de_atomic_u32 bg_monitor_idle;  /* 1 while the monitor blocks with nothing pending */
    de_sem bg_monitor_sem;          /* posted by a 0 -> 1 submit while idle, and at shutdown */
    de_thread *bg_monitor;          /* NULL when the boost is off */
    bool bg_boost;

    /* Statistics (tests, DENG-293). */
    DE_ALIGN(DE_CACHE_LINE) de_atomic_u64 counter_zero_events;
    de_atomic_u64 bg_boosts;   /* times the monitor raised a worker (DENG-388) */
    de_atomic_u64 bg_boost_ns; /* total raised time */
    de_atomic_u64 bg_checks;   /* monitor checks made */

    /* Read-only after create. */
    DE_ALIGN(DE_CACHE_LINE) const de_allocator *allocator;
    de__task_slot *slots;
    u32 slot_count;
    u32 frame_count; /* worker_count + 1 */
    u32 bg_count;
    u32 external_slot;
    u32 tag;
    de_log_category log;
    bool pin_workers;

    /* Start latch (DENG-416): every thread that create starts posts it once after its thread init
     * (FP mode, crash alt-stack, thread-local slot: the first touch of thread-local storage, which
     * makes dyld allocate the thread's TLS block on macOS). de_task_system_create waits for all of
     * them, so no thread's first-use allocations land after create returns (inside a caller's
     * no-heap region, DENG-415). */
    de_sem start_sem;
};

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif

/* ---------- Shared internal functions ---------- */

/** The calling thread's slot in `ts`, or NULL when it has none (it then uses the external
 * slot). */
de__task_slot *de__task_current_slot(const de_task_system *ts);
/** Sets the calling thread's slot (worker start, create) or clears it (NULL). */
void de__task_set_current_slot(de__task_slot *slot);

/** de_task_submit with a counter the caller has already charged for this task (task graphs:
 * de_task_graph_run charges `done` with the node count). The task takes one off `charged` when
 * it has run, after its completion is counted, like any task with a counter. */
void de__task_submit_charged(de_task_system *ts, de_task_pool pool, de_task_fn fn, void *user,
                             de__task_counter *charged);

/** Runs `t` on the calling thread, which owns `self`: calls the function, recycles the record,
 * counts the completion and decrements the counter (waking its waiter). */
void de__task_execute(de_task_system *ts, de__task_slot *self, de__task *t);
/** Decrements `c` by one finished task; wakes the parked waiter on the transition to zero. */
void de__task_counter_finish(de_task_system *ts, de__task_counter *c);
/** The park semaphore of waiter id `id` (a slot or an external wait record; see the top). */
de_sem *de__task_waiter_sem(de_task_system *ts, u32 id);

/**
 * Frame slot `self`: the next task it may run, or NULL. Order: the main-thread queue (only when
 * `self` is slot 0 and `with_main`), its own deque, the injection queue, then the other frame
 * workers' deques from a random first victim.
 */
de__task *de__task_find_work(de_task_system *ts, de__task_slot *self, bool with_main);
/** Like de__task_find_work, but never steals: the main-thread queue (slot 0 and `with_main`), its
 * own deque, then the injection queue. Used by deeply nested waits (DE__TASK_MAX_WAIT_DEPTH). */
de__task *de__task_find_own_work(de_task_system *ts, de__task_slot *self, bool with_main);
/** Pops one main-thread task (slot 0 only), or NULL. */
de__task *de__task_pop_main(de_task_system *ts);
/** Pops one background task, or NULL. */
de__task *de__task_pop_background(de_task_system *ts);
/** true when some frame work is visible to `self` (cheap approximate check before parking). */
bool de__task_work_visible(de_task_system *ts, const de__task_slot *self, bool with_main);
/** After queueing frame work: wakes one idle frame worker if any is parked. */
void de__task_wake_one(de_task_system *ts);
/** After queueing a main-thread task: wakes slot 0 if it is parked in a wait. */
void de__task_wake_main(de_task_system *ts);

/** Before a background task becomes visible: counts it as pending and queued, and wakes an
 * idle monitor (background boost). */
void de__bg_note_submit(de_task_system *ts);
/** A background worker's event (background boost): counts it and, when the worker `yields` the
 * core (a task ends, the thread parks), lowers it if the monitor raised it. Call only when
 * `ts->bg_boost`. */
void de__bg_event(de_task_system *ts, de__task_slot *self, bool yields);

/** Thread entry points (worker.c). */
i32 de__task_frame_worker_main(void *slot);
i32 de__task_background_worker_main(void *slot);
/** The background boost monitor (worker.c); `p` is the task system. */
i32 de__task_bg_monitor_main(void *p);

/** The grain de_task_parallel_for uses for `grain == 0`: max(1, count / (frame workers * 4)).
 * Depends on the worker count, so it is not deterministic across machines (parallel_for.c). */
u32 de__task_parallel_for_auto_grain(const de_task_system *ts, u32 count);

/** Statistics for tests and DENG-293 profiling. */
typedef struct de__task_stats {
    u64 submitted;           /* tasks submitted, every pool */
    u64 completed;           /* tasks run, every pool */
    u64 counter_zero_events; /* counter transitions to zero */
    u64 record_blocks;       /* record blocks allocated, every slot */
    u64 bg_boosts;           /* background workers raised (DENG-388) */
    u64 bg_boost_ns;         /* total time background workers were raised */
    u32 bg_prio_failures;    /* failed priority changes (other than unsupported) */
    u64 bg_checks;           /* boost monitor checks made */
} de__task_stats;
/** Sums the per-slot counts. Exact when no task runs. */
de__task_stats de__task_get_stats(const de_task_system *ts);

#endif /* DE_TASK_INTERNAL_H */
