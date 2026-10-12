/* de_task.h — work-stealing scheduler and task graphs. SPEC.md §8.
 *
 * Threads. A task system has three kinds of threads:
 *   - Frame workers: the creating thread (the main thread, worker index 0) plus `worker_count`
 *     threads named `de-worker-<n>` (indices 1..worker_count). Each owns a work-stealing deque.
 *     Frame-pool tasks run on any frame worker; the main thread runs them only while it is inside
 *     de_task_wait() (or de_task_system_destroy()).
 *   - Background workers: `background_worker_count` threads named `de-bg-<n>` at DE_THREAD_LOW.
 *     They run DE_TASK_POOL_BACKGROUND tasks only, and frame workers never run those, so a long
 *     task (cooking, shader compiles, navmesh builds) never stalls a frame.
 *   - Any other thread may submit and wait. Its frame-pool submits go through a global injection
 *     queue; it never runs tasks.
 * Every engine-created worker calls de_fp_env_init_thread() and de_crash_thread_init() before it
 * runs a task (SPEC §5.7 rule 4). The creating thread must have called de_fp_env_init_thread()
 * itself (asserted in Debug and Dev builds).
 *
 * Determinism. Which worker runs a task, and in which order, is not deterministic (stealing
 * picks random victims). Deterministic results come from fixed chunking and fixed-order
 * reduction (de_task_parallel_for), never from execution order (SPEC §5.7 rule 5).
 *
 * Errors. The error context is per thread (de_error_last). A task that fails must store its
 * de_error, or a copy of what it needs, in its own data for the waiting thread to read.
 */
#ifndef DE_TASK_H
#define DE_TASK_H
#include "de_base.h"
#include "de_memory.h"
DE_EXTERN_C_BEGIN

typedef struct de_task_system de_task_system;
typedef struct de_task_graph de_task_graph;
DE_HANDLE(de_task_node);

/**
 * A completion counter: the number of tasks submitted with it that have not finished yet.
 * Zero-initialize it (`de_task_counter c = {0};`) before its first submit. Several tasks may
 * share one counter, and a counter may be reused once it is done. It must stay valid (and must
 * not move) until every task submitted with it has finished. At most 2^32 - 1 tasks may be
 * pending on one counter at a time.
 */
typedef struct de_task_counter {
    DE_ALIGN(8) u8 opaque[16];
} de_task_counter; /* waitable */

typedef enum de_task_pool {
    DE_TASK_POOL_FRAME,      /**< frame workers (and the main thread inside de_task_wait) */
    DE_TASK_POOL_BACKGROUND, /**< background workers only; for long-running work */
    DE_TASK_POOL_MAIN_THREAD /**< the main thread only, inside de_task_wait/de_task_pump_main */
} de_task_pool;

typedef void (*de_task_fn)(void *user);
typedef void (*de_range_fn)(u32 begin, u32 end, void *user);

/** de_task_worker_index() on a thread that is not a frame worker of a live task system. */
#define DE_TASK_WORKER_NONE 0xFFFFFFFFu

/** Parameters of de_task_system_create(). `(de_task_system_desc){0}` is valid. */
typedef struct de_task_system_desc {
    /** Frame worker threads besides the main thread. 0 = de_cpu_logical_cores() - 1, at least
     * 1. */
    u32 worker_count;
    /** Background worker threads. 0 = max(1, de_cpu_logical_cores() / 4). */
    u32 background_worker_count;
    /** Every allocation of the system (records, deques, queues). NULL = de_heap_allocator(). */
    const de_allocator *allocator;
    /** Pin frame worker n to logical core n (SPEC §8: only on request). The main thread is not
     * pinned. Where the OS cannot pin (macOS), a message is logged once and the workers run
     * unpinned. */
    bool pin_workers;
} de_task_system_desc;

/**
 * Creates a task system and starts its threads. The calling thread becomes frame worker 0 (the
 * "main thread" of the system): only it may call de_task_pump_main() and
 * de_task_system_destroy(), and only it runs DE_TASK_POOL_MAIN_THREAD tasks.
 *
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (`out` NULL); DE_ERR_BUSY (the calling thread is
 *         already the main thread or a worker of another live task system);
 *         DE_ERR_OUT_OF_MEMORY; DE_ERR_INTERNAL (a thread or semaphore cannot be created).
 */
DE_API de_result de_task_system_create(const de_task_system_desc *desc, de_task_system **out);
/**
 * Runs every pending task (frame, main-thread and background, including tasks those submit),
 * then stops and joins the threads and frees everything. Call it on the creating thread, when
 * no other thread submits any more. NULL does nothing.
 */
DE_API void de_task_system_destroy(de_task_system *ts);
/** Number of frame workers including the main thread: indices 0..count-1. */
DE_API u32 de_task_worker_count(const de_task_system *ts);
/** The calling thread's frame worker index (0 = main thread), or DE_TASK_WORKER_NONE on any
 * other thread (background workers included). */
DE_API u32 de_task_worker_index(void);

/* One-shot tasks. The counter is decremented as each task finishes. */

/**
 * Queues `fn(user)` on `pool`. `counter` may be NULL (nobody waits). Thread-safe; callable
 * from any thread, including from inside a task. Never fails: task records come from per-thread
 * free lists, and if memory runs out a frame or background task runs inline instead.
 * No heap allocation once the record pools and deques have grown to the workload (warm-up).
 */
DE_API void de_task_submit(de_task_system *ts, de_task_pool pool, de_task_fn fn, void *user,
                           de_task_counter *counter);
/**
 * Returns when `counter` is done (every task submitted with it has finished; their writes are
 * visible). A frame worker helps meanwhile: it runs tasks from its own deque, the injection
 * queue and other workers' deques, and on the main thread also main-thread tasks. Other threads
 * sleep. Background workers never help with frame tasks.
 */
DE_API void de_task_wait(de_task_system *ts,
                         de_task_counter *counter); /* helps run work while waiting */
/** true when every task submitted with `counter` has finished (acquire). */
DE_API bool de_task_done(const de_task_counter *counter);
/**
 * Runs queued DE_TASK_POOL_MAIN_THREAD tasks on the main thread. Main thread only.
 * @param max  The most tasks to run. 0 = the tasks queued when the call starts (tasks they
 *             submit to the main-thread queue wait for the next pump or wait).
 * @return The number of tasks run.
 */
DE_API u32 de_task_pump_main(de_task_system *ts, u32 max);

/* Fixed-chunk parallel for. Chunk boundaries depend only on (count, grain), so results
 * are deterministic when fn is deterministic. Blocks until complete. */
DE_API void de_task_parallel_for(de_task_system *ts, u32 count, u32 grain, de_range_fn fn,
                                 void *user);

/* ---------- Task graphs (built once, run many times) ---------- */
typedef struct de_task_node_desc {
    const char *name; /* for Tracy and the stats overlay */
    de_task_fn fn;
    void *user;
    de_task_pool pool;
} de_task_node_desc;

DE_API de_result de_task_graph_create(de_task_system *ts, de_task_graph **out);
DE_API void de_task_graph_destroy(de_task_graph *g);
DE_API de_task_node de_task_graph_add(de_task_graph *g, const de_task_node_desc *desc);
DE_API void de_task_graph_depend(de_task_graph *g, de_task_node before, de_task_node after);
DE_API de_result de_task_graph_compile(de_task_graph *g); /* detects cycles */
DE_API void de_task_graph_run(de_task_graph *g, de_task_counter *done);

DE_EXTERN_C_END
#endif
