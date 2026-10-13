/*
 * step.c: de_physics_step (DENG-132 minimal; DENG-135 wired the collision pipeline). S23 adds the
 * solver after the pipeline.
 *
 * Allocation: the first step creates the broadphase and the contacts state (if they do not exist
 * yet); after that the step allocates nothing (tests/unit/physics/test_collide_pipeline.c and
 * test_world_bodies.c check this with a counting allocator).
 */
#include "physics/broadphase/broadphase.h"
#include "physics/contact/manifold.h"
#include "physics/physics_internal.h"

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

    /* ---- 3. Collision pipeline (DENG-135) ---- */
    collide(p);

    /* ---- 4. Solver and integration (S23): reads de__contacts_manifolds ---- */

    /* ---- 5. Advance the tick ---- */
    p->last_dt = dt;
    ++p->tick;
}
