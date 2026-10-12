/*
 * hierarchy_internal.h: built-in components, the ChildOf / Children hierarchy and transform
 * propagation (DENG-351, SPEC §7.1, §5.5, §10.2).
 *
 * Include as "ecs/hierarchy_internal.h" (or "hierarchy_internal.h" inside src/ecs/). Not public
 * API.
 *
 * Files:
 *   - builtin_components.c: the reflected types, lazy registration (ecs_builtins_ensure), the
 *     world state (de_world.hier) and de_builtin_components.
 *   - hierarchy.c: link / unlink in the intrusive child list, set_parent, the despawn hook,
 *     recursive despawn, child iteration.
 *   - transform_propagate.c: the built-in PostUpdate system.
 *
 * Commands (commands.c) apply DE_CMD_SET_PARENT, DE_CMD_DESPAWN_RECURSIVE and DE_CMD_DESPAWN
 * through ecs_hier_set_parent, ecs_hier_despawn_recursive and ecs_hier_on_despawn, like the
 * immediate API.
 */
#ifndef DE_ECS_HIERARCHY_INTERNAL_H
#define DE_ECS_HIERARCHY_INTERNAL_H

#include "ecs_internal.h"

#include "platform/de_atomic.h"

DE_EXTERN_C_BEGIN

/** Counters of transform propagation, summed over every run (tests measure the skip with them).
 */
typedef struct ecs_hier_stats {
    u64 runs;          /**< propagation system runs */
    u64 visited;       /**< entities with DeTransform walked */
    u64 recomputed;    /**< global transforms computed and written (or added) */
    u64 globals_added; /**< DeGlobalTransform adds recorded for entities that had none */
} ecs_hier_stats;

/** de_world.hier: created by the first successful ecs_builtins_ensure. */
typedef struct ecs_hier_state {
    de_builtin_component_ids ids;
    de_system propagate; /**< the built-in propagation system (may be removed by the user) */
    de_atomic_u64 runs, visited, recomputed, globals_added;
} ecs_hier_state;

/** true when the built-ins of `w` are registered. Thread-safe once they are. */
static inline bool ecs_builtins_ready(const de_world *w) {
    return w->hier != NULL;
}

/**
 * Registers the built-in types and components and adds the propagation system, once. Later calls
 * return DE_OK at once. Single-threaded; refuses (DE_ERR_BUSY) while a schedule runs.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT, DE_ERR_ALREADY_EXISTS, DE_ERR_BUSY or
 *         DE_ERR_OUT_OF_MEMORY (then nothing is registered that a retry would not reuse).
 */
de_result ecs_builtins_ensure(de_world *w);

/** Frees de_world.hier (de_world_destroy). Safe when it is NULL. */
void ecs_hier_destroy(de_world *w);

/** The propagation counters of `w` (zero before the built-ins exist). */
void ecs_hier_stats_get(const de_world *w, ecs_hier_stats *out);

/** Registers the propagation system (transform_propagate.c). Called by ecs_builtins_ensure. */
de_result ecs_transform_propagate_register(de_world *w, de_system *out);

/** The data of component `component` of alive entity `e` (`rec` is its record), or NULL when
 * it is absent or a tag. No access check and no change mark. */
static inline void *ecs_hier_data(const de_world *w, const ecs_record *rec, u32 component) {
    const ecs_archetype *a = ecs_archetype_at(w, rec->archetype);
    const u32 col = ecs_archetype_column(a, component);
    return col == ECS_INVALID_INDEX ? NULL : ecs_row_column(a, rec->chunk, rec->row, col);
}

/**
 * set_parent without the structural assert. With `from_command`, a cycle is dropped with a
 * warning (Debug and Dev); without, it is a DE_ASSERT failure (Debug and Dev). Both return
 * DE_ERR_INVALID_ARGUMENT for it and change nothing. The built-ins must be registered.
 */
de_result ecs_hier_set_parent(de_world *w, de_entity child, de_entity parent, bool from_command);

/** Called before alive entity `e` is despawned (not recursively): removes it from its parent's
 * list and detaches its children (they lose DeChildOf and become roots). No-op without built-ins.
 */
void ecs_hier_on_despawn(de_world *w, de_entity e);

/** Despawns alive entity `e` and its subtree (post-order). Without built-ins, a plain despawn. */
void ecs_hier_despawn_recursive(de_world *w, de_entity e);

DE_EXTERN_C_END

#endif /* DE_ECS_HIERARCHY_INTERNAL_H */
