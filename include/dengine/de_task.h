/* de_task.h — work-stealing scheduler and task graphs. SPEC.md §8.
 *
 * Threads. A task system has three kinds of threads:
 *   - Frame workers: the creating thread (the main thread, worker index 0) plus `worker_count`
 *     threads named `de-worker-<n>` (indices 1..worker_count). Each owns a work-stealing deque.
 *     Frame-pool tasks run on any frame worker; the main thread runs them only while it is inside
 *     de_task_wait() (or de_task_system_destroy()).
 *   - Background workers: `background_worker_count` threads named `de-bg-<n>` at DE_THREAD_LOW.
 *     They run DE_TASK_POOL_BACKGROUND tasks only, and frame workers never run those, so a long
 *     task (cooking, shader compiles, navmesh builds) never stalls a frame. On Windows, which
 *     runs a below-normal thread only when no normal one is ready, a monitor thread gives them a
 *     bounded share while the frame workers keep every core busy (de_task_bg_boost).
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

/**
 * Anti-starvation boost of the background workers (Trama "Decision: Background workers get a
 * bounded share, never none"). While background tasks are pending, a monitor thread
 * (`de-bg-boost`, DE_THREAD_REALTIME, blocked while nothing is pending) checks every 10 ms for a
 * background worker that wants the CPU (it holds a task, or tasks are queued) but did nothing
 * since the last check: no task started or ended, no park in de_task_wait. It raises that worker
 * to DE_THREAD_REALTIME until the worker gives the core back (its task ends or it parks), for at
 * most 2 ms (on Windows up to the next 15.6 ms timer tick). Raised time comes from a budget of
 * 1/8 of wall time (bursts up to 32 ms), so all background workers together take at most about
 * 1/8 of one core from frame workers that keep every core busy. A starved background worker
 * waits one or two checks (on Windows about 16 to 40 ms) while the budget lasts, instead of a
 * frame worker's quantum or the OS's own boost (about 190 ms to 5 s).
 */
typedef enum de_task_bg_boost {
    DE_TASK_BG_BOOST_AUTO, /**< ON on Windows (strict priority scheduling), OFF elsewhere */
    DE_TASK_BG_BOOST_OFF,  /**< never: background workers may starve while cores are busy */
    /** Run the monitor on every OS. Where de_thread_set_priority is unsupported (macOS, Linux)
     * it changes nothing; this is for tests of the policy. */
    DE_TASK_BG_BOOST_ON
} de_task_bg_boost;

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
    /** Anti-starvation boost of the background workers; see de_task_bg_boost. */
    de_task_bg_boost background_boost;
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
/** True only on a background worker belonging to `ts`; false for frame/external
 * threads, workers of another system and NULL. Unlike de_task_worker_index(), this tests pool
 * identity. */
DE_API bool de_task_is_background_worker(const de_task_system *ts);

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
 * Waits may nest (a task that waits, a nested de_task_parallel_for, a graph run from a task) and
 * never deadlock fork-join work. The first 32 nested waits on a thread help with any task; deeper
 * ones stop stealing from other workers and run only the thread's own queued tasks, the
 * injection queue and (main thread) main-thread tasks, which bounds the stack.
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

/**
 * Fixed-chunk parallel for (SPEC §8, §5.7 rule 5). Calls `fn(begin, end, user)` once per chunk:
 * chunk k covers `[k * grain, min((k + 1) * grain, count))`, so `begin / grain` is the chunk
 * index. The boundaries depend only on (count, grain): with a deterministic `fn` that writes
 * per-chunk results, reduced afterwards in chunk order, the result is the same on every machine
 * and worker count. Which thread runs a chunk, and in which order chunks run, is not
 * deterministic.
 *
 * The calling thread runs chunks too and returns when every chunk has finished (their writes
 * are visible). Any thread may call it, including a task (nested loops wait and help, see
 * de_task_wait). Allocates nothing. `count == 0` returns at once; a single chunk runs inline.
 *
 * @param grain  Indices per chunk. **0 picks max(1, count / (de_task_worker_count(ts) * 4)),
 *               which depends on the machine's worker count: grain 0 is NOT deterministic
 *               across machines and must not be used by deterministic code.** Deterministic
 *               callers pass an explicit grain (the ECS de_query_par_each uses 1024 rows).
 */
DE_API void de_task_parallel_for(de_task_system *ts, u32 count, u32 grain, de_range_fn fn,
                                 void *user);

/* ---------- Task graphs (built once, run many times) ----------
 * A graph is built on one thread (add, depend), compiled, then run any number of times; the
 * S05 ECS compiles each schedule into one. Building and compiling are not thread-safe and must
 * not overlap a run. Changing a compiled graph (add or depend) marks it dirty: compile it again
 * before the next run. Runs allocate nothing once the task system has warmed up. */

/** One node of a task graph. `(de_task_node_desc){0}` is not valid: `fn` is required. */
typedef struct de_task_node_desc {
    /** For error messages, Tracy and the stats overlay; may be NULL. Not copied: it must stay
     * valid while the graph exists (a string literal). */
    const char *name;
    de_task_fn fn; /**< required */
    void *user;    /**< passed to fn */
    /** Where the node runs. DE_TASK_POOL_MAIN_THREAD nodes run on the main thread, inside
     * de_task_wait (a graph waited on from the main thread drains them) or de_task_pump_main. */
    de_task_pool pool;
} de_task_node_desc;

/**
 * Creates an empty graph on `ts`; its memory comes from the task system's allocator.
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (`ts` or `out` NULL); DE_ERR_OUT_OF_MEMORY.
 */
DE_API de_result de_task_graph_create(de_task_system *ts, de_task_graph **out);
/** Frees the graph. It must not be running (wait for the last run's counter first). NULL does
 * nothing. */
DE_API void de_task_graph_destroy(de_task_graph *g);
/**
 * Adds a node. Returns its handle, or an invalid handle (id 0) when `desc` is NULL, `fn` is NULL,
 * the pool is unknown, the graph has 2^24 - 1 nodes or memory runs out; the failure is also
 * reported by the next de_task_graph_compile, so a build sequence needs only one check, at
 * compile. Marks a compiled graph dirty.
 */
DE_API de_task_node de_task_graph_add(de_task_graph *g, const de_task_node_desc *desc);
/**
 * `after` starts only once `before` has finished (and sees its writes). Duplicate edges are
 * allowed and count once. A handle that is not a node of `g` (or out of memory) is reported by
 * the next compile. Marks a compiled graph dirty.
 */
DE_API void de_task_graph_depend(de_task_graph *g, de_task_node before, de_task_node after);
/**
 * Sorts the graph topologically and lays out its dependency counts and successor lists in one
 * allocation. Call it after the last change and before the next run.
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (a cycle: the message names its nodes in edge order,
 *         `"a -> b -> a"`; or an earlier add/depend got bad arguments); DE_ERR_OUT_OF_MEMORY
 *         (now or in an earlier add/depend). A failed compile leaves the graph not runnable.
 */
DE_API de_result de_task_graph_compile(de_task_graph *g); /* detects cycles */
/**
 * Starts one run of a compiled graph and returns at once: the roots are submitted, and each
 * finishing node submits the successors it made ready (onto the current worker's deque; main-
 * thread nodes onto the main-thread queue). `done` (required) is incremented by the node count
 * and reaches zero when every node has finished: wait with de_task_wait(ts, done). Any thread
 * may run a graph, including a task (its wait helps). One run at a time per graph. Running a graph
 * changed since its last compile asserts in Debug and Dev builds (Release compiles it first).
 * Overhead per node: one atomic decrement per successor edge, one submit, one decrement of
 * `done`; no allocation.
 */
DE_API void de_task_graph_run(de_task_graph *g, de_task_counter *done);

/* ---------- Node statistics (DENG-293; SPEC §18.4 system timings) ----------
 * Debug and Dev builds time every node run with de_time_now_ns() and keep the numbers per
 * compiled node, for the stats overlay and the ECS inspector. Release and Shipping builds do not
 * measure (de_task_graph_node_stats returns DE_ERR_UNSUPPORTED). In profile builds (DE_PROFILE)
 * each node also runs inside a profiler zone named after the node. */

/** Timings of one graph node since the graph was last compiled. */
typedef struct de_task_node_stats {
    u64 last_ns;     /**< duration of the most recent run of the node's fn */
    u64 avg_ns;      /**< mean duration over `runs` (0 when it never ran) */
    u64 runs;        /**< runs since the last compile */
    u32 last_worker; /**< de_task_worker_index() of the last run (DE_TASK_WORKER_NONE: a
                          background worker); DE_TASK_WORKER_NONE when it never ran */
} de_task_node_stats;

/**
 * Reads the statistics of `node`. Any thread may call it at any time; the fields are exact when
 * the graph is not running, and may mix two runs while it is.
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (`g` or `out` NULL, `node` is not a node of `g`, or the
 *         graph has not been compiled since the node was added); DE_ERR_UNSUPPORTED (Release and
 *         Shipping builds: statistics are compiled out, `*out` is zeroed).
 */
DE_API de_result de_task_graph_node_stats(const de_task_graph *g, de_task_node node,
                                          de_task_node_stats *out);

DE_EXTERN_C_END
#endif
