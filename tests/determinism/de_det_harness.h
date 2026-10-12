/*
 * de_det_harness.h: the determinism harness (SPEC §19.4, §5.7 rule 7; DENG-353). Test-only: not
 * part of the public API, never installed.
 *
 * A scenario builds a world and runs it for a fixed number of simulation ticks. After each tick
 * the harness records the world checksum (de_world_checksum, DE_COMP_SNAPSHOT components by
 * default). Two runs are deterministic when their per-tick checksums are equal: across repeated
 * runs, across worker counts, and across platforms and architectures (the checked-in golden file
 * and the `determinism` CI workflow).
 *
 * One tick of a run:
 *   1. scenario->tick(world, tick_index, state), if set: input from outside the simulation
 *      (scripted spawns, impulses). tick_index is de_world_tick() before the tick.
 *   2. de_world_run_schedule(world, DE_SCHEDULE_FIXED_UPDATE, dt, 0), which applies the commands
 *      of the tick and moves the world tick on by one.
 *   3. checksum[tick_index] = de_world_checksum(world, mask).
 *
 * Each run gets a fresh type registry, a fresh world and, when worker_count > 0, its own task
 * system with that many frame workers besides the calling thread (worker_count 0 runs every
 * system serially on the calling thread, with no task system). Only one run may be active per
 * thread at a time (a thread can own one task system).
 *
 * Writing a scenario (tests/determinism/<module>/):
 *   - setup() registers the components (in a fixed order: component ids are part of the
 *     checksum), adds the systems and spawns the start state. Draw random numbers only from
 *     de_rng streams seeded from `seed` (SPEC §5.7 rule 6).
 *   - Simulation code follows SPEC §5.7: de_dmath only, no <math.h>, no hash-map iteration,
 *     stable iteration order. Build scenario sources with DE_DETERMINISTIC_FLAGS.
 *   - Components that carry simulation state need DE_COMP_SNAPSHOT, or they are not checksummed.
 *   - State the scenario keeps outside the world goes in `state` (state_size bytes, zeroed, owned
 *     by the harness, freed when the run ends).
 */
#ifndef DE_DET_HARNESS_H
#define DE_DET_HARNESS_H

#include <dengine/de_ecs.h>

/** No divergence: returned by de_det_compare() when the sequences are equal. */
#define DE_DET_NO_DIVERGENCE 0xFFFFFFFFu
/** Default fixed time step of a scenario (de_det_scenario.dt = 0). */
#define DE_DET_DEFAULT_DT (1.0f / 60.0f)

typedef struct de_det_scenario {
    const char *name; /**< file-name safe: the checksum file is `<name>.checksums` */
    /** Builds the start state on an empty world. Return DE_OK, or an error to stop the run. */
    de_result (*setup)(de_world *w, u64 seed, void *state);
    /** Optional. Called before each tick's FIXED_UPDATE run, outside the schedule. */
    void (*tick)(de_world *w, u64 tick_index, void *state);
    /** Optional. Called before the world is destroyed (free what setup allocated in `state`). */
    void (*teardown)(de_world *w, void *state);
    u32 ticks;         /**< ticks of a full run (de_det_run) */
    u32 state_size;    /**< bytes of zeroed scenario state; 0 passes NULL */
    f32 dt;            /**< fixed time step; 0 means DE_DET_DEFAULT_DT */
    u32 checksum_mask; /**< de_world_checksum mask; 0 means DE_COMP_SNAPSHOT */
} de_det_scenario;

/** One run in progress. Fields are read-only for callers. */
typedef struct de_det_session {
    const de_det_scenario *scenario;
    de_task_system *tasks; /**< NULL when worker_count is 0 */
    de_type_registry *types;
    de_world *world;
    void *state;
    u64 seed;
    u32 worker_count;
    f32 dt;
    u32 mask;
} de_det_session;

/**
 * Starts a run: sets the calling thread's deterministic FP mode, creates the task system (when
 * worker_count > 0), a type registry and a world, then calls scenario->setup.
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (NULL arguments, no setup, no name); the error of a
 *         failed create or setup (everything is torn down again; `*s` is zeroed).
 */
de_result de_det_session_begin(de_det_session *s, const de_det_scenario *scenario, u64 seed,
                               u32 worker_count);
/**
 * Runs one tick (see the file comment) and stores the checksum after it in `*out_checksum`
 * (may be NULL).
 * @return DE_OK or the error of de_world_run_schedule.
 */
de_result de_det_session_step(de_det_session *s, u64 *out_checksum);
/** Calls scenario->teardown, destroys the world, the registry and the task system. Zeroes `*s`.
 * A zeroed session is ignored. */
void de_det_session_end(de_det_session *s);

/**
 * A full run: begin, scenario->ticks steps (out_checksums[t] is the checksum after tick t), end.
 * `out_checksums` holds scenario->ticks values.
 */
de_result de_det_run(const de_det_scenario *scenario, u64 seed, u32 worker_count,
                     u64 *out_checksums);

/**
 * The first tick whose checksums differ, or DE_DET_NO_DIVERGENCE when a[0..count) equals
 * b[0..count).
 */
u32 de_det_compare(const u64 *a, const u64 *b, u32 count);
/**
 * de_det_compare, and on a divergence prints the first divergent tick, both checksums of the
 * ticks around it and how many ticks differ in total, to stderr, with `what` naming the two runs
 * ("workers 0 vs 8"). Returns the first divergent tick or DE_DET_NO_DIVERGENCE.
 */
u32 de_det_report(const char *what, const u64 *a, const u64 *b, u32 count);

/**
 * Writes `count` checksums to `path`: a `#` header line naming the scenario, the seed and the
 * tick count, then one 16-digit lowercase hex value per line (LF line ends).
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT or DE_ERR_IO.
 */
de_result de_det_write_checksums(const char *path, const char *scenario_name, u64 seed,
                                 const u64 *checksums, u32 count);
/**
 * Reads a checksum file written by de_det_write_checksums (lines starting with `#` and empty
 * lines are skipped; CR LF is accepted). Stores at most `capacity` values in `out` and the number
 * of values in the file in `*out_count`.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (NULL arguments), DE_ERR_NOT_FOUND (cannot open) or
 *         DE_ERR_PARSE (a line that is not one hex value, or more than `capacity` values).
 */
de_result de_det_read_checksums(const char *path, u64 *out, u32 capacity, u32 *out_count);

#endif /* DE_DET_HARNESS_H */
