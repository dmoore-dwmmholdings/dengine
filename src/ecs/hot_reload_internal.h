/*
 * hot_reload_internal.h: what the app host needs from the ECS to hot-reload a game library
 * (DENG-93, SPEC §10.1). Internal to the engine; include as "ecs/hot_reload_internal.h". Not
 * public API: the functions use the `de__` prefix and are not exported from a shared engine.
 *
 * A reload replaces the code behind every system the game registered and behind the hooks of
 * every type it registered. The ECS copies hooks, sizes and alignments when a component, resource
 * or event type is registered, so after the game calls de_type_register again the ECS must take
 * the new description: the same layout only needs the new hook pointers, a changed layout needs
 * its data re-laid out. The host (src/host/hot_reload.c) drives it:
 *
 *   de__world_set_system_owner(w, GAME)  ... game registers systems ...  set_system_owner(w, 0)
 *   reload:  de__world_remove_systems_by_owner(w, GAME)
 *            game: register_types           (de_type_register keeps ids, see Trama "Decision:
 *                                            Type re-registration keeps the id and the previous
 *                                            layout")
 *            de__world_sync_types(w, &stats)
 *            game: register_systems         (under the owner tag again)
 *
 * All of it is main-thread work between frames: never while a schedule of the world runs.
 */
#ifndef DE_ECS_HOT_RELOAD_INTERNAL_H
#define DE_ECS_HOT_RELOAD_INTERNAL_H

#include <dengine/de_ecs.h>

DE_EXTERN_C_BEGIN

/** The owner tag of the engine's own systems. They are never removed by owner. */
#define DE__SYSTEM_OWNER_ENGINE 0u

/**
 * Sets the owner tag that de_system_add gives every system added from now on (0 =
 * DE__SYSTEM_OWNER_ENGINE, the default). The host sets the game's tag around the game's
 * callbacks and puts 0 back afterwards. NULL `w` is ignored.
 */
void de__world_set_system_owner(de_world *w, u32 owner);

/** The current owner tag (0 for NULL `w`). */
u32 de__world_system_owner(const de_world *w);

/**
 * Removes every system whose owner tag is `owner` (de_system_remove: queries destroyed, the
 * schedule rebuilt by its next run, handles go stale). `owner` 0 removes nothing: engine systems
 * are never removed this way. Not inside a schedule run.
 * @return the number of systems removed.
 */
u32 de__world_remove_systems_by_owner(de_world *w, u32 owner);

/** The number of live systems with owner tag `owner` (tests, logs). */
u32 de__world_system_count_by_owner(const de_world *w, u32 owner);

/**
 * Re-lays out component `c` for the type `new_type` (normally the component's own type, which a
 * re-registration keeps): every archetype with a column of `c` gets new chunks with the new
 * column size and alignment, and every row is moved into them in table order (entity, every
 * column's bytes, the added and changed ticks; records updated). The rows of `c` itself are
 * converted object by object:
 *   - zero the new object;
 *   - copy each field of the new description from the old field with the same name and kind
 *     (same size, and the same nested type name for STRUCT, ENUM, FLAGS, FIXED_ARRAY and
 *     ASSET; STRING buffers may grow or shrink and stay NUL-terminated; FIXED_ARRAY counts may
 *     change, the common prefix is copied). Types without fields copy the common byte prefix;
 *   - call the new type's `migrate` hook, if it has one, with the old version, the old object
 *     and the old fields. A `false` from it is counted (`*migrate_refused`) and the row keeps
 *     what the copy gave: a component cannot be dropped here.
 * Old objects are not destructed: their contents are moved, as when a row changes archetype.
 * The world's checksum cache and the command buffers' entity-offset cache of `c` are dropped.
 * A tag component only takes the new description. Nothing is allocated per frame afterwards.
 *
 * `migrate_refused` may be NULL. Not inside a schedule run.
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (bad `w`, `c` or `new_type`, or another type of that
 *         name is a component already); DE_ERR_UNSUPPORTED (the new type is too large for a
 *         chunk row, or aligned above DE_ECS_MAX_COMPONENT_ALIGN); DE_ERR_INVALID_ARGUMENT
 *         (neither POD nor copy and destruct hooks); DE_ERR_OUT_OF_MEMORY. On failure nothing
 *         changed.
 */
de_result de__world_relayout_component(de_world *w, de_component c, de_type_id new_type,
                                       u32 *migrate_refused);

/** What de__world_sync_types did. */
typedef struct de__world_sync_stats {
    u32 refreshed;       /**< components, resources and event types that only took new hooks */
    u32 relaid_out;      /**< components re-laid out (de__world_relayout_component) */
    u32 resources;       /**< resources re-laid out */
    u32 events;          /**< event types whose buffered payloads were re-laid out */
    u32 migrate_refused; /**< objects whose `migrate` hook returned false */
} de__world_sync_stats;

/**
 * Makes the world follow its type registry after types were registered again (a game reload).
 * For every component (id order), resource (slot order) and event type (id order) whose type's
 * current description is not the one it holds:
 *   - same schema hash: take the new description's hooks and `is_pod`; nothing else changes
 *     (column memory is untouched);
 *   - another schema hash: re-lay out the data (de__world_relayout_component for components;
 *     the resource value and the buffered event payloads are converted the same way).
 * Every changed description is checked first (size, alignment, hooks, a row that fits each
 * archetype's chunk); if one is refused, nothing changes and the error names the type. Only an
 * allocation failure after the checks leaves the world partly synced (the error says so).
 * `out` may be NULL. Not inside a schedule run.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT, DE_ERR_UNSUPPORTED or DE_ERR_OUT_OF_MEMORY.
 */
de_result de__world_sync_types(de_world *w, de__world_sync_stats *out);

DE_EXTERN_C_END

#endif /* DE_ECS_HOT_RELOAD_INTERNAL_H */
