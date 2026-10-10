/* de_task.h — work-stealing scheduler and task graphs. SPEC.md §8. */
#ifndef DE_TASK_H
#define DE_TASK_H
#include "de_base.h"
#include "de_memory.h"
DE_EXTERN_C_BEGIN

typedef struct de_task_system de_task_system;
typedef struct de_task_graph  de_task_graph;
DE_HANDLE(de_task_node);
typedef struct de_task_counter { DE_ALIGN(8) u8 opaque[16]; } de_task_counter;  /* waitable */

typedef enum de_task_pool { DE_TASK_POOL_FRAME, DE_TASK_POOL_BACKGROUND, DE_TASK_POOL_MAIN_THREAD } de_task_pool;

typedef void (*de_task_fn)(void *user);
typedef void (*de_range_fn)(u32 begin, u32 end, void *user);

typedef struct de_task_system_desc {
    u32 worker_count;            /* 0 = logical cores - 1 */
    u32 background_worker_count; /* 0 = max(1, cores / 4) */
    const de_allocator *allocator;
} de_task_system_desc;

DE_API de_result de_task_system_create(const de_task_system_desc *desc, de_task_system **out);
DE_API void      de_task_system_destroy(de_task_system *ts);
DE_API u32       de_task_worker_count(const de_task_system *ts);
DE_API u32       de_task_worker_index(void);   /* 0 = main thread */

/* One-shot tasks. The counter is decremented as each task finishes. */
DE_API void de_task_submit(de_task_system *ts, de_task_pool pool, de_task_fn fn, void *user,
                           de_task_counter *counter);
DE_API void de_task_wait(de_task_system *ts, de_task_counter *counter);   /* helps run work while waiting */
DE_API bool de_task_done(const de_task_counter *counter);

/* Fixed-chunk parallel for. Chunk boundaries depend only on (count, grain), so results
 * are deterministic when fn is deterministic. Blocks until complete. */
DE_API void de_task_parallel_for(de_task_system *ts, u32 count, u32 grain, de_range_fn fn, void *user);

/* ---------- Task graphs (built once, run many times) ---------- */
typedef struct de_task_node_desc {
    const char *name;            /* for Tracy and the stats overlay */
    de_task_fn  fn;
    void       *user;
    de_task_pool pool;
} de_task_node_desc;

DE_API de_result     de_task_graph_create(de_task_system *ts, de_task_graph **out);
DE_API void          de_task_graph_destroy(de_task_graph *g);
DE_API de_task_node  de_task_graph_add(de_task_graph *g, const de_task_node_desc *desc);
DE_API void          de_task_graph_depend(de_task_graph *g, de_task_node before, de_task_node after);
DE_API de_result     de_task_graph_compile(de_task_graph *g);  /* detects cycles */
DE_API void          de_task_graph_run(de_task_graph *g, de_task_counter *done);

DE_EXTERN_C_END
#endif
