/*
 * step.c: de_physics_step (DENG-132, minimal). Ownership passes to DENG-135, which inserts the
 * collision pipeline into the marked section; S23 adds the solver after it.
 *
 * Allocates nothing: everything it touches was allocated at create (the zero-alloc test in
 * tests/unit/physics/test_world_bodies.c checks this with a counting allocator).
 */
#include "physics/physics_internal.h"

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

    /* ---- 2. Refresh world AABBs (ascending body index) ---- */
    de__bodies_refresh_aabbs(p);

    /* ---- 3. Collision pipeline (DENG-135): broadphase update + sorted pairs, narrowphase per
     *         pair in fixed ranges, serial merge into manifolds, end-touch ---- */

    /* ---- 4. Solver and integration (S23) ---- */

    /* ---- 5. Advance the tick ---- */
    p->last_dt = dt;
    ++p->tick;
}
