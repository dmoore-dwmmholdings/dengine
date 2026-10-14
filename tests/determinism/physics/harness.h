/*
 * harness.h: the physics per-tick checksum harness (DENG-136; SPEC §5.7, §13.2, §19.4, risk R6).
 * Test-only: not part of the public API, never installed.
 *
 * A thin layer over the ECS determinism harness (../de_det_harness.h): every run is a
 * de_det_session (type registry, ECS world, optional task system, deterministic FP mode), and
 * this file adds the physics world and the physics checksum. One tick of a run:
 *
 *   1. de_det tick callback (outside the schedule): scenario->script(ctx, tick), then the
 *      test-only inject hook of the run options (de_phys_det_options.inject).
 *   2. de_world_run_schedule(FIXED_UPDATE): the system "physics.step" that de_physics_attach adds
 *      (DENG-223) syncs the ECS entities with physics bodies (scenarios may spawn them), moves
 *      the ECS characters, and calls de_physics_step(physics, dt). Until DENG-223 the harness had
 *      its own exclusive "physics.det_step"; for scenarios without physics components the two
 *      give the same state.
 *   3. The tick's checksum: XXH3-64 over de_physics_checksum(physics), scenario->probe(ctx, tick)
 *      (0 when there is no probe) and de_world_checksum(world, DE_COMP_SNAPSHOT), in that order,
 *      as little-endian u64 values. Also, for diagnosis only, one hash per field group
 *      (de_phys_det_field): which part of the state differs at a divergent tick.
 *
 * The physics world gets the session's task system (de_physics_desc.tasks), so worker counts
 * reach physics as well as the ECS schedule. de_physics_attach() registers the physics components
 * and inserts the DePhysicsWorld resource before setup runs.
 *
 * Writing a scenario (tests/determinism/physics/scenarios_<topic>.c):
 *   - Define `const de_phys_det_scenario de_phys_det_scenario_<name>` and put
 *     DE_PHYS_DET_REGISTER(<name>) in the same file. That defines the de_test cases
 *     `<name>.workers_0`, `.workers_1` and `.workers_4`.
 *   - Add the file and the name to a de_phys_det_add_scenarios() call in CMakeLists.txt, which
 *     registers the CTest entries `determinism_physics_<name>.workers_<n>`. harness.c needs no
 *     edit.
 *   - Generate golden/<name>.checksums once (see "Golden files" below) and check it in.
 *   - SPEC §5.7: setup and script use de_dmath / de_dvec3 / de_rng only (no <math.h>, no fast
 *     de_v3_* / de_quat_* math), draw random numbers only from de_rng streams seeded from
 *     ctx->seed, one draw per statement (argument evaluation order differs between compilers),
 *     and visit bodies in a fixed order. The sources build with DE_DETERMINISTIC_FLAGS.
 *   - State kept outside the physics world goes in ctx->state (state_size bytes, zeroed, owned by
 *     the harness). It is not rolled back by an ECS snapshot restore.
 *
 * Sanitizer builds (DENG-412). A build with DE_SANITIZE set compiles this harness with
 * DE_PHYS_DET_SANITIZED=1, and de_phys_det_check then does less work, so the slow scenarios stay
 * well inside their CTest timeout under TSan:
 *   - a scenario with `sanitizer_ticks` runs only that many ticks, and the check compares that
 *     prefix of the full golden file (which must still hold `ticks` values). Set it on a scenario
 *     whose sanitizer run comes near its timeout, and keep every scripted event inside the prefix;
 *   - a workers 1/4 case runs the workers 0 reference only when its run does not match the golden
 *     file. A match is enough: the workers_0 case checks the same golden file.
 * Update mode always runs the full scenario. Other builds run the full scenario and the
 * reference.
 *
 * Golden files and outputs. golden/<name>.checksums (de_det_write_checksums format: a `#` header,
 * then one 16-digit hex value per tick) is a cross-platform contract: every platform, compiler and
 * worker count must reproduce it. The `workers_0` test also writes what it computed to
 * <out>/physics_<name>.checksums and <out>/physics_<name>.fields (one line per tick: the tick,
 * then the field-group hashes; <out> is DE_DET_OUT_DIR), which the Determinism CI workflow
 * uploads and compares across platforms. To regenerate the goldens after a deliberate change
 * (Debug and Dev builds only; never to make a platform pass):
 *     build/dev/tests/determinism/physics/test_det_physics --update-golden
 * (or DE_DET_UPDATE_GOLDEN=1 with a --filter on `*.workers_0`). Regenerate on one platform; CI
 * then proves that the others reproduce it.
 */
#ifndef DE_PHYS_DET_HARNESS_H
#define DE_PHYS_DET_HARNESS_H

#include "de_det_harness.h"
#include "de_test.h"

#include <dengine/de_physics.h>

/** Default seed of a scenario (de_phys_det_scenario.seed = 0). */
#define DE_PHYS_DET_DEFAULT_SEED 0x5EED0136ull
/** Default body pool size of a scenario's physics world (de_phys_det_scenario.max_bodies = 0). */
#define DE_PHYS_DET_DEFAULT_MAX_BODIES 4096u
/** Most ticks a scenario may run. */
#define DE_PHYS_DET_MAX_TICKS 100000u

/** What scenario callbacks get. Valid for the run; `state` is zeroed before setup. */
typedef struct de_phys_det_ctx {
    de_physics_world *physics;
    de_world *world; /**< the ECS world; holds the DePhysicsWorld resource */
    void *state;     /**< scenario->state_size bytes; NULL when 0 */
    u64 seed;
    u32 worker_count;
    f32 dt;
} de_phys_det_ctx;

typedef struct de_phys_det_scenario {
    const char *name; /**< equals the suite name of DE_PHYS_DET_REGISTER; file-name safe */
    /** Builds the start state (shapes, bodies). Return DE_OK, or an error to stop the run. */
    de_result (*setup)(de_phys_det_ctx *c);
    /** Optional. Per-tick input before the step (move kinematic bodies, add or remove bodies).
     *  `tick` counts from 0. */
    void (*script)(de_phys_det_ctx *c, u64 tick);
    /** Optional. Extra state hashed into the tick's checksum, after the step: what the physics
     *  checksum does not cover yet (contacts computed by the scenario, built shapes). Called once
     *  per tick; it must not change the simulation. */
    u64 (*probe)(de_phys_det_ctx *c, u64 tick);
    /** Optional. Called before the physics world is destroyed. */
    void (*teardown)(de_phys_det_ctx *c);
    u32 ticks;      /**< ticks of a full run, 1..DE_PHYS_DET_MAX_TICKS */
    u32 state_size; /**< bytes of zeroed scenario state */
    u32 max_bodies; /**< physics body pool; 0 = DE_PHYS_DET_DEFAULT_MAX_BODIES */
    f32 dt;         /**< fixed step; 0 = DE_DET_DEFAULT_DT (1/60) */
    u64 seed;       /**< 0 = DE_PHYS_DET_DEFAULT_SEED */
    /** Ticks that de_phys_det_check runs in a sanitizer build (see the file comment), 1..ticks;
     *  0 = ticks. */
    u32 sanitizer_ticks;
} de_phys_det_scenario;

/** Field groups: one diagnostic hash each per tick. Names: de_phys_det_field_name(). */
typedef enum de_phys_det_field {
    DE_PHYS_DET_FIELD_WORLD,    /**< tick, gravity, substeps, live body count */
    DE_PHYS_DET_FIELD_POSITION, /**< body positions (f64) */
    DE_PHYS_DET_FIELD_ROTATION, /**< body rotations */
    DE_PHYS_DET_FIELD_VELOCITY, /**< linear and angular velocities */
    DE_PHYS_DET_FIELD_MASS,     /**< mass, inverse mass, center of mass, inertia and inverse */
    DE_PHYS_DET_FIELD_AABB,     /**< world AABBs */
    DE_PHYS_DET_FIELD_BODY,     /**< index, generation, type, flags, shape, filter, material,
                                     damping, gravity scale, owner */
    DE_PHYS_DET_FIELD_PROBE,    /**< scenario->probe */
    DE_PHYS_DET_FIELD_ECS,      /**< de_world_checksum */
    DE_PHYS_DET_FIELD_COUNT
} de_phys_det_field;

/** Run options. `(de_phys_det_options){0}` is a plain run without a task system. */
typedef struct de_phys_det_options {
    u32 worker_count; /**< frame workers besides the calling thread; 0 = no task system */
    /** Test only: called after scenario->script, before the step (a deliberate perturbation). */
    void (*inject)(de_phys_det_ctx *c, u64 tick, void *user);
    void *inject_user;
} de_phys_det_options;

/** One run in progress. Fields are read-only for callers; the struct must not move between
 *  begin and end (the de_det session points into it). */
typedef struct de_phys_det_session {
    de_det_session det;
    de_det_scenario det_scenario; /**< the de_det wrapper of `scenario` */
    const de_phys_det_scenario *scenario;
    de_phys_det_options options;
    de_phys_det_ctx ctx;
    void *state;
    u64 tick;       /**< ticks stepped so far */
    u64 last_probe; /**< probe value of the last tick */
    u64 last_ecs;   /**< de_world_checksum after the last tick */
} de_phys_det_session;

/**
 * Starts a run: de_det_session_begin (FP mode, task system, registry, ECS world), then the
 * physics world (with the session's task system), de_physics_attach (which adds physics.step),
 * and scenario->setup. On failure everything is torn down again.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (bad scenario) or the error of a failed step.
 */
de_result de_phys_det_begin(de_phys_det_session *s, const de_phys_det_scenario *scenario,
                            const de_phys_det_options *options);
/**
 * One tick (see the file comment). Stores the tick's checksum in `*out_checksum` and, when
 * `out_fields` is not NULL, the DE_PHYS_DET_FIELD_COUNT field-group hashes in out_fields[].
 */
de_result de_phys_det_step(de_phys_det_session *s, u64 *out_checksum, u64 *out_fields);
/** Calls scenario->teardown, destroys the physics world, then ends the de_det session. A zeroed
 *  session is ignored. */
void de_phys_det_end(de_phys_det_session *s);

/**
 * A full run of scenario->ticks ticks. `out_checksums` holds `ticks` values; `out_fields` is NULL
 * or holds ticks * DE_PHYS_DET_FIELD_COUNT values (tick t at [t * DE_PHYS_DET_FIELD_COUNT]).
 */
de_result de_phys_det_run(const de_phys_det_scenario *scenario, const de_phys_det_options *options,
                          u64 *out_checksums, u64 *out_fields);

/** The first field of a field-level diff (de_phys_det_diff_at). */
typedef struct de_phys_det_field_diff {
    u32 differing;  /**< fields that differ in total (0: the states are equal) */
    u32 body;       /**< body index of the first difference; 0xFFFFFFFF for a world field */
    char field[48]; /**< name of the first differing field ("position.x", "probe", ...) */
} de_phys_det_field_diff;

/**
 * Field-level diff: runs the scenario with options `a`, then with options `b`, each up to and
 * including tick `tick`, and compares the physics state after that tick field by field (every
 * body pool field, the world settings, the probe value and the ECS checksum). Prints the first
 * differences to stderr (body index, field, both values as hex and decimal) under the heading
 * `what`, fills `*out` (may be NULL) and returns the number of differing fields.
 */
u32 de_phys_det_diff_at(const char *what, const de_phys_det_scenario *scenario,
                        const de_phys_det_options *a, const de_phys_det_options *b, u64 tick,
                        de_phys_det_field_diff *out);

/** "world", "position", ... ; "?" out of range. */
const char *de_phys_det_field_name(u32 field);

/**
 * The check behind every DE_PHYS_DET_REGISTER case (de_test.h checks; call it from a TEST only):
 *   - runs the scenario with `workers` for de_phys_det_check_ticks() ticks; the run's checksums
 *     must change on most ticks;
 *   - workers 0: writes <out>/physics_<name>.checksums and .fields; in update mode also rewrites
 *     golden/<name>.checksums (Debug and Dev builds only);
 *   - workers > 0: runs workers 0 too and compares; on a divergence reports the first divergent
 *     tick and a field-level diff (de_phys_det_diff_at), and fails. A sanitizer build does this
 *     only when the golden comparison below fails or is skipped (update mode);
 *   - compares with golden/<name>.checksums (all `ticks` values must be there; the ticks run are
 *     compared); on a divergence reports the first divergent tick, the field groups of this run
 *     at that tick, and fails.
 */
void de_phys_det_check(const de_phys_det_scenario *scenario, u32 workers);

/** Ticks that de_phys_det_check runs: scenario->sanitizer_ticks (when not 0) in a sanitizer
 *  build outside update mode, else scenario->ticks. */
u32 de_phys_det_check_ticks(const de_phys_det_scenario *scenario);

/** true in a sanitizer build (DE_PHYS_DET_SANITIZED, set when DE_SANITIZE is not empty). */
bool de_phys_det_sanitized(void);

/** true when golden files are rewritten (--update-golden or DE_DET_UPDATE_GOLDEN=1). */
bool de_phys_det_update_mode(void);

/** The worker counts every scenario is checked with (CMakeLists.txt registers the same list). */
#define DE_PHYS_DET_REGISTER(name)                                                                 \
    TEST(name, workers_0) {                                                                        \
        de_phys_det_check(&de_phys_det_scenario_##name, 0u);                                       \
    }                                                                                              \
    TEST(name, workers_1) {                                                                        \
        de_phys_det_check(&de_phys_det_scenario_##name, 1u);                                       \
    }                                                                                              \
    TEST(name, workers_4) {                                                                        \
        de_phys_det_check(&de_phys_det_scenario_##name, 4u);                                       \
    }

#endif /* DE_PHYS_DET_HARNESS_H */
