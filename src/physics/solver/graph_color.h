/*
 * graph_color.h: deterministic graph coloring of the constraint records and the colored,
 * parallel stage runner of the soft-step solver (DENG-218, SPEC §13.2, §5.7, §8). Internal:
 * include as "physics/solver/graph_color.h".
 *
 * Coloring (graph_color.c). Greedy, once per step after gather, over the records in record order
 * (classes ascending, each in its stable id order). Each color has a bitset over the solver slots;
 * a record takes the first color in which none of its *writable* sides (awake dynamic bodies) is
 * set yet, and sets them. Static, kinematic and sleeping sides (the static slot, or a kinematic
 * slot that no constraint writes) use no color. At most DE__GRAPH_MAX_COLORS colors; a record that
 * fits none goes to the overflow set. So the coloring depends only on the record order and the
 * slots, which depend only on body indices and constraint ids. In each color every writable body
 * appears at most once (de__graph_color_validate; asserted in Debug builds).
 *
 * Execution (solver_parallel.c). Every constraint stage (prepare, warm start, solve, relax,
 * restitution, store) runs the colors in ascending order, then the overflow set serially. A
 * color's records (in record order) are split into fixed chunks of `grain` records
 * (physics.solver.grain) run by de_task_parallel_for. The records of one color write disjoint
 * solver bodies and their own data only, so a color's result does not depend on how its chunks
 * are scheduled; with tasks == NULL the same colored order runs on the calling thread. Results are
 * bit-identical for any worker count. The per-body kernels (integration, finalize) are split into
 * fixed ranges of DE__SOLVER_BODY_GRAIN slots the same way.
 */
#ifndef DE_PHYSICS_SOLVER_GRAPH_COLOR_H
#define DE_PHYSICS_SOLVER_GRAPH_COLOR_H

#include "physics/solver/constraint.h"

DE_EXTERN_C_BEGIN

/** Colors before overflow (SPEC §13.2 graph-colored batches). */
#define DE__GRAPH_MAX_COLORS 24u
/** de__constraint.color of a record in the overflow set (solved serially after the colors). */
#define DE__GRAPH_OVERFLOW DE__GRAPH_MAX_COLORS

/** Records per de_task_parallel_for chunk (cvar physics.solver.grain). A constant per world: it
 *  must not depend on the machine (SPEC §5.7 rule 5). */
#define DE__SOLVER_DEFAULT_GRAIN 64u
#define DE__SOLVER_GRAIN_CVAR    "physics.solver.grain"
/** Solver bodies per chunk of the per-body kernels. */
#define DE__SOLVER_BODY_GRAIN 256u

/** The stages a constraint class runs (constraint.h). */
typedef enum de__solver_stage {
    DE__STAGE_PREPARE,
    DE__STAGE_WARM_START,
    DE__STAGE_SOLVE,
    DE__STAGE_RELAX,
    DE__STAGE_RESTITUTION,
    DE__STAGE_STORE
} de__solver_stage;

/** Counters of the last step. */
typedef struct de__coloring_stats {
    u32 colors;   /**< colors in use (without the overflow set) */
    u32 overflow; /**< records in the overflow set */
    u32 largest;  /**< records of the largest color */
    u32 chunks;   /**< parallel chunks of one pass over every stage's colors (grain-sized) */
} de__coloring_stats;

typedef struct de__coloring {
    u32 grain;          /**< records per chunk (physics.solver.grain at create; tests may set) */
    u32 body_grain;     /**< bodies (and islands) per chunk: DE__SOLVER_BODY_GRAIN; tests may set */
    u32 slot_capacity;  /**< solver slots covered by each bitset (max_bodies + 1) */
    u32 words;          /**< u64 words per color bitset */
    u64 *bits;          /**< [DE__GRAPH_MAX_COLORS * words] */
    u32 *stamp;         /**< [slot_capacity] scratch of de__graph_color_validate */
    u32 *order;         /**< [constraint capacity] record indices, grouped by color */
    u32 order_capacity; /**< entries of `order` (the solver's constraint capacity) */
    u32 start[DE__GRAPH_MAX_COLORS + 2u]; /**< color k: order[start[k], start[k + 1]); the
                                               overflow set is k = DE__GRAPH_OVERFLOW */
    u32 color_count;                      /**< colors in use */
    de__coloring_stats stats;
} de__coloring;

/** Bytes of the state and its bitsets for `slot_capacity` solver slots. */
size_t de__coloring_bytes(u32 slot_capacity);
/** Lays the state out in `mem` (zeroed, 16-byte aligned) and reads the grain cvar. `order` is
 *  set by the solver with the record array (de__coloring_bind_order). */
de__coloring *de__coloring_init(void *mem, u32 slot_capacity);
/** Points `order` at a buffer of `capacity` entries (the solver's record array grew). */
static inline void de__coloring_bind_order(de__coloring *c, u32 *order, u32 capacity) {
    c->order = order;
    c->order_capacity = capacity;
}

/**
 * Colors this step's records (sets de__constraint.color, fills order / start). The solver bodies
 * and records must be final (after gather).
 */
void de__graph_color(de__solver *s);
/**
 * Checks the coloring of this step: every record appears once in `order`, under its color, and
 * no writable solver body appears twice in one color. Returns true when it holds.
 */
bool de__graph_color_validate(const de__solver *s);

/**
 * solver_parallel.c: runs `stage` for every record: colors in ascending order, each split into
 * chunks of `grain` records on p->tasks (inline when tasks is NULL or the color fits one chunk),
 * then the overflow set serially.
 */
void de__solver_run_stage(de__solver *s, de__solver_stage stage);
/** solver_parallel.c: fn(s, begin, end) over solver slots [1, body_count) in fixed ranges of
 *  `body_grain` (DE__SOLVER_BODY_GRAIN) slots, on p->tasks when there is more than one range. */
void de__solver_for_bodies(de__solver *s, void (*fn)(de__solver *s, u32 begin, u32 end));

DE_EXTERN_C_END

#endif /* DE_PHYSICS_SOLVER_GRAPH_COLOR_H */
