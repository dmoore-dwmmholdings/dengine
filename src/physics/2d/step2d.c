/*
 * step2d.c: de_physics2d_step (DENG-252 minimal; DENG-253 wired the collision pipeline). The
 * pipeline (SPEC §13.3, the 2D mirror of 3D's step.c):
 *
 *   1. validate dt
 *   2. refresh world AABBs (ascending body index)
 *   3. collision: broadphase pairs within the speculative margin, chain midphase into sub-pairs,
 *      narrowphase per sub-pair on `tasks` (fixed ranges, per-sub-pair slots), serial merge into
 *      the sorted manifolds with begin / persist / end (manifold2d.h)
 *   4. [DENG-254] solver, islands and integration: reads and writes the manifolds of step 3
 *      (de__contacts2d_manifolds_mut) and moves the bodies
 *   5. end-touch: the ids of the manifolds that ended in step 3 become free
 *   6. advance the tick
 *
 * Works with tasks == NULL (everything on the calling thread) and gives the same state with any
 * worker count. Allocation: the first step creates the broadphase and the contacts state (if they
 * do not exist yet); after that the step allocates nothing (tests/unit/physics2d/
 * test_manifold2d.c and test_world2d.c check it with a tracking allocator).
 */
#include "physics/2d/broadphase2d.h"
#include "physics/2d/manifold2d.h"
#include "physics/2d/p2d_internal.h"

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

    /* ---- 4. Solver, islands and integration (DENG-254) ---- */

    /* ---- 5. End-touch (DENG-253) ---- */
    de__contacts2d_end_touch(p);

    /* ---- 6. Advance the tick ---- */
    p->last_dt = dt;
    ++p->tick;
}
