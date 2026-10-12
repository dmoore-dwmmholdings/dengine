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

# DENG-345: structural changes (add, remove, despawn, spawn_with) and the archetype edge cache.
target_sources(de_ecs PRIVATE structural.c archetype_graph.c)
