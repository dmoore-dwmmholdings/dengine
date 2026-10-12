# src/ecs: module `ecs` (target de_ecs), layer runtime. SPEC §7. Deterministic (SPEC §5.7).
# Shared file (append-only within S05): later S05 tasks add their sources with target_sources()
# below, one block per task, so merges keep both sides.
de_add_module(
    NAME ecs
    LAYER runtime
    SOURCES
        ecs_internal.h
        world.c
        entity.c
        component.c
        archetype.c
        table.c
    DEPS core task reflect
    DETERMINISTIC
)

# DENG-348: resources and events.
target_sources(de_ecs PRIVATE resource.c event.c)

# DENG-345: structural changes (add, remove, despawn, spawn_with) and the archetype edge cache.
target_sources(de_ecs PRIVATE structural.c archetype_graph.c)

# DENG-349: command buffers, pending entity ids and deterministic apply.
target_sources(de_ecs PRIVATE commands_internal.h commands.c)

# DENG-346: queries (cached archetype matching, chunk iteration, par_each).
target_sources(de_ecs PRIVATE query_internal.h query.c query_cache.c query_iter.c)

# DENG-347: change detection (change ticks, ADDED / CHANGED row test, write marking, wrap clamp).
target_sources(de_ecs PRIVATE change_internal.h change.c)

# DENG-350: systems, access sets, schedules (conflict DAG on the task graph) and debug checks.
target_sources(de_ecs PRIVATE system_internal.h system.c access.c schedule.c debug_checks.c)
