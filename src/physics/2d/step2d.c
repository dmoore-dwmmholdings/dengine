/*
 * step2d.c: de_physics2d_step (DENG-252 minimal). DENG-253 inserts the collision pipeline and
 * then owns this file; DENG-254 adds the solver after it.
 *
 * Allocation: none (pools only); tests/unit/physics2d/test_world2d.c checks it with a tracking
 * allocator.
 */
#include "physics/2d/p2d_internal.h"

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

    /* ---- 4. Solver, islands and integration (DENG-254) ---- */

    /* ---- 5. Advance the tick ---- */
    p->last_dt = dt;
    ++p->tick;
}
