/*
 * islands.c: island hooks of de_physics_step (DENG-217 stub; DENG-218 replaces this file with
 * islands, sleeping and wake-up). The signatures are final: step.c calls them and is not edited
 * again. See solver_internal.h.
 */
#include "physics/solver/solver_internal.h"

/* Stub: every live dynamic and kinematic body is awake (no sleeping), ascending body index. */
void de__islands_update(de_physics_world *p, de__solver *s) {
    const de__body_pool *b = &p->bodies;
    for (u32 i = 0; i < b->high_water; ++i)
        if (b->alive[i] && b->type[i] != (u8)DE_BODY_STATIC)
            (void)de__solver_add_awake(s, i);
}

/* Stub: no sleep timers yet. */
void de__islands_finalize(de_physics_world *p, de__solver *s) {
    (void)p;
    (void)s;
}
