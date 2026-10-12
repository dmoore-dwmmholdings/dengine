# src/task: module `task` (target de_task), layer runtime. SPEC §8.
# Append-only within S04: DENG-292 and DENG-293 add their sources to the SOURCES list, one per
# line.
de_add_module(
    NAME task
    LAYER runtime
    SOURCES
        task_internal.h
        deque.h
        deque.c
        scheduler.c
        worker.c
        graph.c
        parallel_for.c
        profile.h
        profile.c
    DEPS core platform
)
