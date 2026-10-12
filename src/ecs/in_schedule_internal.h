/*
 * in_schedule_internal.h: "is the calling thread running an ECS system?" for modules above the
 * ECS (DENG-141). Internal to the engine; include as "ecs/in_schedule_internal.h".
 *
 * The ECS keeps a per-thread "current system" scope (system.c, DENG-350): de_world_run_schedule
 * enters it around every system and every par_each range of a system, on whichever thread runs
 * it. This query reads that scope without naming a world, so code that must never run inside a
 * frame-loop system (blocking waits such as de_asset_wait) can assert on it.
 */
#ifndef DE_ECS_IN_SCHEDULE_INTERNAL_H
#define DE_ECS_IN_SCHEDULE_INTERNAL_H

#include <dengine/de_base.h>

DE_EXTERN_C_BEGIN

/**
 * True while the calling thread runs a system (or a par_each range of one) of any world inside
 * de_world_run_schedule; false elsewhere, including on a thread that only waits for a schedule.
 * Exclusive systems count as inside. Any thread; no allocation.
 */
bool de__ecs_in_schedule(void);

/* ---- DENG-84 ---- */
/**
 * The de_schedule (as an int) of the system the calling thread runs, or -1 outside every system.
 * Like de__ecs_in_schedule: any world, any thread, no allocation. Input queries use it to read
 * the per-tick action view inside FixedUpdate systems.
 */
int de__ecs_current_schedule(void);
/* ---- end DENG-84 ---- */

DE_EXTERN_C_END

#endif /* DE_ECS_IN_SCHEDULE_INTERNAL_H */
