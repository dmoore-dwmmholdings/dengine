/*
 * step.c: de_physics_step (DENG-132 minimal; DENG-135 wired the collision pipeline; DENG-217 the
 * solver and the hooks of the wave-2 tasks). The pipeline is final: islands (DENG-218) and CCD
 * (DENG-219) replace islands.c and ccd.c, graph coloring (DENG-218) the stage loops of
 * solver/solver.c; none of them edits this file.
 *
 *   1. validate dt
 *   2. refresh world AABBs (ascending body index)
 *   3. de__ccd_prepare                          (ccd.c)
 *   4. collision pipeline: broadphase, narrowphase on `tasks`, manifolds (DENG-135)
 *   5. solver (solver_internal.h): islands, prepare, substeps
 *      (integrate velocities, warm start, solve, integrate positions, relax), restitution,
 *      store impulses, de__ccd_solve, finalize (f64 positions), de__islands_finalize
 *   6. advance the tick
 *
 * Allocation: the first step creates the broadphase, the contacts and the solver state (if they do
 * not exist yet); after that the step allocates nothing
 * (tests/unit/physics/test_collide_pipeline.c, test_world_bodies.c and test_solver_stack.c check
 * this with a counting allocator).
 */
#include "physics/broadphase/broadphase.h"
#include "physics/contact/manifold.h"
#include "physics/physics_internal.h"
#include "physics/solver/solver_internal.h"

/* Broadphase update + sorted pairs, narrowphase per pair in fixed ranges, serial merge into
 * manifolds with begin / end (SPEC §13.1, §13.2). Failures leave the error context set; an
 * overflow still keeps the lowest keys, so the step goes on. */
static void collide(de_physics_world *p) {
    de__contacts *c = NULL;
    if (de__contacts_ensure(p, &c) != DE_OK)
        return;
    /* Pairs within the speculative margin, not only overlapping ones (manifold.h). */
    de__broadphase_set_pair_distance(p, c->speculative);
    (void)de__broadphase_update(p);
    const de__bp_pair *pairs = NULL;
    u32 count = 0;
    de__broadphase_pairs(p, &pairs, &count);
    de__contacts_collide(p, pairs, count);
    (void)de__contacts_merge(p, pairs, count);
}

/* Sub-stepped soft-step solver (SPEC §13.1): see solver_internal.h. Without a solver state (no
 * memory) bodies do not move this step; the error context says why. */
static void solve(de_physics_world *p, f32 dt) {
    de__solver *s = de__solver_begin(p, dt);
    if (s == NULL) {
        de__ccd_solve(p, NULL);
        return;
    }
    de__islands_update(p, s);
    de__solver_prepare(s);
    for (u32 i = 0; i < s->ctx.substeps; ++i) {
        de__solver_integrate_velocities(s);
        de__solver_warm_start(s);
        de__solver_solve(s);
        de__solver_integrate_positions(s);
        de__solver_relax(s);
    }
    de__solver_restitution(s);
    de__solver_store(s);
    de__ccd_solve(p, s);
    de__solver_finalize(s);
    de__islands_finalize(p, s);
}

void de_physics_step(de_physics_world *p, f32 dt) {
    /* ---- 1. Validate ---- */
    if (p == NULL) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_physics_step: world is NULL");
        return;
    }
    if (!de__f32_finite(dt) || !(dt > 0.0f)) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_physics_step: dt must be finite and > 0");
        return;
    }

    /* ---- 2. Refresh world AABBs (ascending body index; catches direct pool writes) ---- */
    de__bodies_refresh_aabbs(p);

    /* ---- 3. CCD preparation (DENG-219) ---- */
    de__ccd_prepare(p, dt);

    /* ---- 4. Collision pipeline (DENG-135) ---- */
    collide(p);

    /* ---- 5. Solver and integration (DENG-217) ---- */
    solve(p, dt);

    /* ---- 6. Advance the tick ---- */
    p->last_dt = dt;
    ++p->tick;
}
