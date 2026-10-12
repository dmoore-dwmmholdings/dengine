/* change.c: change detection: the world change tick, the DE_ADDED / DE_CHANGED row test, write
 * marking, de_entity_mark_changed and tick-wrap clamping (DENG-347, SPEC §7.1, §7.2).
 *
 * The rules are in change_internal.h. Row ticks of a new column are set by ecs_row_init_ticks
 * (table.c: added = changed = the current change tick); everything else about ticks is here.
 * Integer-only and iterated in creation order (SPEC §5.7). */
#include "change_internal.h"

#include "system_internal.h" /* DENG-350: systems' last runs, per-run stamp tick */

/* ---- World change tick ----------------------------------------------------------------------- */

u32 ecs_change_tick_advance(de_world *w) {
    const u32 this_run = w->change_tick;
    w->change_tick = this_run + 1u;
    if ((u32)(w->change_tick - w->change_check_tick) >= ECS_CHANGE_CHECK_PERIOD)
        ecs_check_change_ticks(w);
    return this_run;
}

void ecs_debug_set_tick(de_world *w, u32 t) {
    w->change_tick = t;
    w->change_check_tick = t;
}

/* Clamps every tick of the rows [0, count) of one tick array. */
static void clamp_ticks(u32 *ticks, u32 count, u32 now) {
    for (u32 i = 0; i < count; ++i)
        ticks[i] = ecs_tick_clamp(ticks[i], now);
}

void ecs_check_change_ticks(de_world *w) {
    const u32 now = w->change_tick;
    /* Rows: creation order, chunk order, column order (any order gives the same result: each
     * tick is clamped on its own). */
    for (u32 ai = 0; ai < ecs_archetype_count(w); ++ai) {
        ecs_archetype *a = ecs_archetype_at(w, ai);
        for (u32 c = 0; c < a->chunks.count; ++c) {
            const ecs_chunk *ch = ecs_table_chunk(a, c);
            for (u32 col = 0; col < a->column_count; ++col) {
                clamp_ticks(ecs_chunk_added(a, ch, col), ch->count, now);
                clamp_ticks(ecs_chunk_changed(a, ch, col), ch->count, now);
            }
        }
    }
    /* Query windows (slot order). */
    if (w->queries != NULL) {
        for (u32 s = 0; s < w->queries->slots.count; ++s) {
            ecs_query *q = DE_ARRAY_AT(&w->queries->slots, ecs_query_slot, s).q;
            if (q == NULL || !q->change_window_set)
                continue;
            q->last_run_tick = ecs_tick_clamp(q->last_run_tick, now);
            q->this_run_tick = ecs_tick_clamp(q->this_run_tick, now);
        }
    }
    /* DENG-350: each system's last run. */
    ecs_systems_clamp_ticks(w, now);
    w->change_check_tick = now;
}

/* ---- Row test and marking -------------------------------------------------------------------- */

bool ecs_change_filter_ticks(const ecs_query *q, const ecs_archetype *a, const ecs_chunk *ch,
                             const u32 *filter_cols, u32 row) {
    if (!q->change_window_set)
        return true; /* no window yet: every row is new, as on a system's first run */
    const u32 last = q->last_run_tick, now = q->this_run_tick;
    for (u32 j = 0; j < q->change_count; ++j) {
        const u32 col = filter_cols[j]; /* a data column: ADDED / CHANGED on tags is refused */
        const u32 *ticks = q->filters[q->change_filters[j]].filter == DE_ADDED
                               ? ecs_chunk_added(a, ch, col)
                               : ecs_chunk_changed(a, ch, col);
        if (!ecs_tick_in_window(ticks[row], last, now))
            return false;
    }
    return true;
}

void ecs_change_mark_slice(const de_world *w, const ecs_query *q, const ecs_archetype *a,
                           const ecs_chunk *ch, const u32 *cols, u32 row, u32 count) {
    const u32 tick = ecs_query_write_tick(w, q);
    for (u32 i = 0; i < q->term_count; ++i) {
        const de_access access = q->terms[i].access;
        if (access != DE_WRITE && access != DE_OPTIONAL_WRITE)
            continue;
        const u32 col = cols[i];
        if (col == ECS_QUERY_COL_ABSENT || col == ECS_QUERY_COL_TAG)
            continue;
        u32 *changed = ecs_chunk_changed(a, ch, col) + row;
        for (u32 k = 0; k < count; ++k)
            changed[k] = tick;
    }
}

void ecs_entity_mark_changed(de_world *w, de_entity e, u32 component) {
    const ecs_record *rec = ecs_entity_record(w, e);
    if (rec == NULL)
        return;
    ecs_archetype *a = ecs_archetype_at(w, rec->archetype);
    const u32 col = ecs_archetype_column(a, component);
    if (col == ECS_INVALID_INDEX)
        return; /* absent, or a tag (no ticks) */
    /* DENG-350: inside a system, the system's run tick (like its query writes), so the system
     * does not see its own change on its next run; else the current change tick. */
    ecs_mark_changed_row(a, rec->chunk, rec->row, col, ecs_system_stamp_tick(w));
}

/* ---- Public API ------------------------------------------------------------------------------ */

void de_entity_mark_changed(de_world *w, de_entity e, de_component c) {
    if (w == NULL)
        return;
    ECS_ASSERT_NOT_PENDING(e); /* DENG-349: pending ids are for commands only */
    ecs_entity_mark_changed(w, e, c.id);
}
