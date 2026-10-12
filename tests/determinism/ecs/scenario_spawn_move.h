/*
 * scenario_spawn_move.h: the ECS determinism scenario `ecs_spawn_move` (DENG-353).
 *
 * Five deterministic FixedUpdate systems over five snapshot components:
 *   spawn  (DSpawner)        de_rng-driven spawns through the system's command buffer
 *   move   (DPos, DVel)      par_each: pos += vel * dt, bounces off the walls and the floor
 *   phase  (DPhase)          par_each: phase advance with de_dfmod; a kick (de_cmd_add of DKick)
 *                            when de_dsin changes sign
 *   kick   (DVel, DKick)     applies and removes kicks (after move: both write DVel)
 *   age    (DLife)           par_each: counts down, despawns at 0, spawns a child at 30
 *                            (registered last, so its despawns apply after the kick commands)
 * spawn, move, phase and age have disjoint access, so they run in parallel; their par_each
 * ranges record commands into per-range buffers. Every 64 ticks the scenario's tick callback
 * (outside the schedule) applies a "wind" to every DVel in query order.
 *
 * Test-only injection (never compiled into src/): det_ecs_inject makes the wind visit the
 * entities in hash-map iteration order from tick `from_tick` on (SPEC §5.7 rule 5 broken on
 * purpose), so det.harness_detects_divergence can check that the harness reports that tick.
 */
#ifndef DE_DET_SCENARIO_SPAWN_MOVE_H
#define DE_DET_SCENARIO_SPAWN_MOVE_H

#include "de_det_harness.h"

/** Seed of the checked-in golden file `ecs_spawn_move.checksums`. */
#define DET_ECS_SPAWN_MOVE_SEED 0xD353ull
/** Ticks between two wind steps of the tick callback. */
#define DET_ECS_WIND_PERIOD 64u

typedef enum det_inject_kind {
    DET_INJECT_NONE = 0,
    /** The wind ranks entities in de_hashmap iteration order instead of query order. */
    DET_INJECT_HASHMAP_ORDER = 1
} det_inject_kind;

typedef struct det_inject {
    det_inject_kind kind;
    u64 from_tick;
} det_inject;

/** The scenario (1000 ticks). */
extern const de_det_scenario det_ecs_spawn_move;
/** Test-only nondeterminism switch; DET_INJECT_NONE except inside det.harness_detects_divergence.
 * Read by the tick callback on the thread that runs the session. */
extern det_inject det_ecs_inject;

#endif /* DE_DET_SCENARIO_SPAWN_MOVE_H */
