/*
 * step2d.c: de_physics2d_step (DENG-252 minimal; DENG-253 wired the collision pipeline; DENG-254
 * the solver). The pipeline (SPEC §13.3, the 2D mirror of 3D's step.c):
 *
 *   1. validate dt
 *   2. refresh world AABBs (ascending body index)
 *   3. collision: broadphase pairs within the speculative margin, chain midphase into sub-pairs,
 *      narrowphase per sub-pair on `tasks` (fixed ranges, per-sub-pair slots), serial merge into
 *      the sorted manifolds with begin / persist / end (manifold2d.h)
 *   4. solver (DENG-254, solver2d_internal.h): wake requests and the awake set, gather (again
 *      when a constraint woke an island), islands, coloring, prepare; substeps (integrate
 *      velocities, warm start, solve, integrate positions, relax); restitution; store impulses
 *      into the manifolds; bullets (time of impact); finalize (f64 positions, AABBs, broadphase
 *      sync); sleep
 *   5. end-touch: the ids of the manifolds that ended in step 3 become free
 *   6. advance the tick
 *
 * Works with tasks == NULL (everything on the calling thread) and gives the same state with any
 * worker count. Allocation: the first step creates the broadphase, the contacts and the solver
 * state (if they do not exist yet); after that the step allocates nothing (tests/unit/physics2d/
 * test_manifold2d.c, test_world2d.c and test_solver2d_parallel.c check it with a tracking
 * allocator).
 */
#include "physics/2d/broadphase2d.h"
#include "physics/2d/manifold2d.h"
#include "physics/2d/p2d_internal.h"
#include "physics/2d/solver2d_internal.h"

/* Broadphase update, midphase, narrowphase in fixed ranges, serial merge. Failures leave the
 * error context set; an overflow keeps the lowest keys, so the step goes on. */
static void collide(de_physics2d_world *p) {
    de__contacts2d *c = NULL;
    if (de__contacts2d_ensure(p, &c) != DE_OK)
        return;
    /* Pairs within the speculative margin, not only overlapping ones. */
    de__bp2d_set_pair_distance(p, de__contacts2d_speculative(p));
    (void)de__bp2d_update(p);
    de__contacts2d_collide(p);
    (void)de__contacts2d_merge(p);
}

/* Sub-stepped soft-step solver (SPEC §13.3, §13.1). Without a solver state (no memory) bodies do
 * not move this step; the error context says why. */
static void solve(de_physics2d_world *p, f32 dt) {
    de__solver2d *s = de__solver2d_begin(p, dt);
    if (s == NULL)
        return;
    de__islands2d_update(p, s);
    de__solver2d_prepare(s);
    for (u32 i = 0; i < s->ctx.substeps; ++i) {
        de__solver2d_integrate_velocities(s);
        de__solver2d_warm_start(s);
        de__solver2d_solve(s);
        de__solver2d_integrate_positions(s);
        de__solver2d_relax(s);
    }
    de__solver2d_restitution(s);
    de__solver2d_store(s);
    de__bullets2d_solve(p, s);
    de__solver2d_finalize(s);
    de__islands2d_finalize(p, s);
}

void de_physics2d_step(de_physics2d_world *p, f32 dt) {
    /* ---- 1. Validate ---- */
    if (p == NULL) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_physics2d_step: world is NULL");
        return;
    }
    if (!de__f32_finite(dt) || !(dt > 0.0f)) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_physics2d_step: dt must be finite and > 0");
        return;
    }

    /* ---- 2. Refresh world AABBs (ascending body index) ---- */
    de__bodies2d_refresh_aabbs(p);

    /* ---- 3. Collision pipeline (DENG-253) ---- */
    collide(p);

    /* ---- 4. Solver, islands, sleeping and bullets (DENG-254) ---- */
    solve(p, dt);

    /* ---- 5. End-touch (DENG-253) ---- */
    de__contacts2d_end_touch(p);

    /* ---- 6. Advance the tick ---- */
    p->last_dt = dt;
    ++p->tick;
}
