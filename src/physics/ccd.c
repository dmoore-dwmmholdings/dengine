/*
 * ccd.c: continuous collision hooks of de_physics_step (DENG-217 stub; DENG-219 replaces this
 * file with per-body speculative margins and swept time-of-impact for bullets). The signatures
 * are final: step.c calls them and is not edited again. See solver_internal.h.
 */
#include "physics/solver/solver_internal.h"

/* Stub: the world-wide speculative margin of the contacts state applies to every body. */
void de__ccd_prepare(de_physics_world *p, f32 dt) {
    (void)p;
    (void)dt;
}

/* Stub: no time-of-impact clamps. */
void de__ccd_solve(de_physics_world *p, de__solver *s) {
    (void)p;
    (void)s;
}
