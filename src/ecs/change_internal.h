/*
 * change_internal.h: change detection: the world change tick, added / changed row ticks, the
 * DE_ADDED / DE_CHANGED row test, write marking and tick-wrap clamping (DENG-347, SPEC §7.1,
 * §7.2).
 *
 * Include as "ecs/change_internal.h" (or "change_internal.h" inside src/ecs/). Not public API.
 *
 * Ticks are plain u32 counters, never time, so change detection is deterministic and replays
 * identically (SPEC §5.7).
 *
 *   - World change tick (`de_world.change_tick`): the tick that the next system run gets. A
 *     system run takes it with ecs_change_tick_advance (DENG-350 calls it once per system run, in
 *     a fixed order), which returns it and moves the counter on by one. Changes made outside a
 *     system are stamped with the counter, so they are newer than every run handed out so far
 *     and every system sees them on its next run.
 *   - Row ticks: every data column row holds `added` and `changed`. Adding a component (spawn,
 *     spawn_with, add, a deferred add) sets both to the executing system's run tick, or the
 *     world change tick outside a system (including deferred adds at the sync point).
 *     Moves between archetypes keep them (ecs_row_init_ticks, table.c).
 *   - Marking changed: de_entity_mark_changed, a mutable de_entity_get and an add of a
 *     component the entity already has (immediate or deferred: the value is replaced) set
 *     `changed` to the current change tick. A query iteration marks every yielded row of each
 *     DE_WRITE / DE_OPTIONAL_WRITE data column with the query's `this_run_tick` (the current
 *     change tick if the query has no window).
 *   - Window: a query compares against (last_run_tick, this_run_tick], set with
 *     ecs_query_set_change_window (DENG-350 sets it before each system run). A tick passes when
 *     it is newer than last_run and not newer than this_run, compared modulo 2^32
 *     (ecs_tick_in_window). A query whose window was never set passes every row, like a system's
 *     first run.
 *   - Wrap: comparisons are valid while ages stay below 2^31. ecs_check_change_ticks clamps
 *     every stored tick older than ECS_MAX_CHANGE_AGE (2^30) to `now - ECS_MAX_CHANGE_AGE`;
 *     ecs_change_tick_advance runs it every ECS_CHANGE_CHECK_PERIOD (2^29) advances, so no tick
 *     is ever older than 2^30 + 2^29 and the test stays valid forever. A clamped tick is treated
 *     as exactly ECS_MAX_CHANGE_AGE old: it is never newer than any window's last_run.
 *
 * Threading: marking writes only the ticks of the rows it is given. Query write marking runs on
 * the thread that iterated the rows, after the callback, and the rows of one par_each range never
 * overlap another range's, so it needs no atomics. ecs_change_tick_advance,
 * ecs_check_change_ticks and ecs_debug_set_tick are single-threaded (no iteration and no other
 * marking may run at the same time).
 */
#ifndef DE_ECS_CHANGE_INTERNAL_H
#define DE_ECS_CHANGE_INTERNAL_H

#include "query_internal.h"

DE_EXTERN_C_BEGIN

/** Ticks older than this (relative to the current change tick) are clamped to it (Bevy's
 * MAX_CHANGE_AGE). A tick of exactly this age is never reported as added or changed. */
#define ECS_MAX_CHANGE_AGE (1u << 30)
/** ecs_change_tick_advance runs ecs_check_change_ticks once this many ticks have passed since the
 * last check. ECS_MAX_CHANGE_AGE + ECS_CHANGE_CHECK_PERIOD < 2^31 keeps comparisons valid. */
#define ECS_CHANGE_CHECK_PERIOD (1u << 29)

_Static_assert(ECS_MAX_CHANGE_AGE + ECS_CHANGE_CHECK_PERIOD < 0x80000000u,
               "change ticks must stay comparable modulo 2^32");

/** Age of `tick` at `now` (modulo 2^32), saturated at ECS_MAX_CHANGE_AGE. */
static inline u32 ecs_tick_age(u32 tick, u32 now) {
    const u32 age = now - tick;
    return age < ECS_MAX_CHANGE_AGE ? age : ECS_MAX_CHANGE_AGE;
}

/**
 * true when `tick` is in the window (last_run, this_run]: newer than `last_run` and not newer
 * than `this_run`. Wrap-safe: (u32)(this_run - tick) < (u32)(this_run - last_run), with both
 * ages saturated at ECS_MAX_CHANGE_AGE (a tick from after `this_run` has a huge age and fails).
 */
static inline bool ecs_tick_in_window(u32 tick, u32 last_run, u32 this_run) {
    return ecs_tick_age(tick, this_run) < ecs_tick_age(last_run, this_run);
}

/** `tick`, or `now - ECS_MAX_CHANGE_AGE` if it is older than that. */
static inline u32 ecs_tick_clamp(u32 tick, u32 now) {
    return (u32)(now - tick) > ECS_MAX_CHANGE_AGE ? now - ECS_MAX_CHANGE_AGE : tick;
}

/** The current change tick: what immediate changes are stamped with. */
static inline u32 ecs_change_tick(const de_world *w) {
    return w->change_tick;
}

/**
 * Takes the tick of one system run: returns the current change tick and moves the counter on by
 * one, so later changes outside the system are newer than this run. Every
 * ECS_CHANGE_CHECK_PERIOD advances it also runs ecs_check_change_ticks. DENG-350 calls it once
 * per system run and passes the result as `this_run_tick` to ecs_query_set_change_window.
 * Single-threaded.
 */
u32 ecs_change_tick_advance(de_world *w);

/**
 * Clamps every stored tick older than ECS_MAX_CHANGE_AGE to `now - ECS_MAX_CHANGE_AGE`, where
 * `now` is the current change tick: the added and changed ticks of every row (archetype creation
 * order, chunk order, column order) and the windows of every query that has one. O(rows *
 * columns); never allocates. Single-threaded. DENG-350 also clamps each system's last_run here.
 */
void ecs_check_change_ticks(de_world *w);

/**
 * Test hook: sets the change tick to `t` (for example near the u32 wrap), as if a check had just
 * run. Stored ticks are not changed. Single-threaded.
 */
void ecs_debug_set_tick(de_world *w, u32 t);

/** Sets the `changed` tick of `column` at (`chunk`, `row`) of `a` to `tick`. */
static inline void ecs_mark_changed_row(ecs_archetype *a, u32 chunk, u32 row, u32 column,
                                        u32 tick) {
    ecs_chunk_changed(a, ecs_table_chunk(a, chunk), column)[row] = tick;
}

/** The tick that write marking of query `q` stamps: its window's this_run, or the current change
 * tick if it has no window. */
static inline u32 ecs_query_write_tick(const de_world *w, const ecs_query *q) {
    return q->change_window_set ? q->this_run_tick : w->change_tick;
}

/**
 * The DE_ADDED / DE_CHANGED row test (the ecs_change_filter_fn of every query; called through
 * ecs_change_filter_default). true when row `row` of chunk `ch` passes every change filter of `q`
 * in its window, or when `q` has no window. Read-only.
 */
bool ecs_change_filter_ticks(const ecs_query *q, const ecs_archetype *a, const ecs_chunk *ch,
                             const u32 *filter_cols, u32 row);

/**
 * Write marking of one yielded slice: sets the changed tick of rows [row, row + count) of chunk
 * `ch` to ecs_query_write_tick, in every DE_WRITE / DE_OPTIONAL_WRITE term's data column
 * (`cols[i]`; absent optionals and tags have none). O(write terms + rows); no allocation.
 */
void ecs_change_mark_slice(const de_world *w, const ecs_query *q, const ecs_archetype *a,
                           const ecs_chunk *ch, const u32 *cols, u32 row, u32 count);

/**
 * Marks `component` of alive entity `e` changed (current change tick) if it has a data column.
 * A dead entity, a missing component or a tag does nothing.
 */
void ecs_entity_mark_changed(de_world *w, de_entity e, u32 component);

DE_EXTERN_C_END

#endif /* DE_ECS_CHANGE_INTERNAL_H */
