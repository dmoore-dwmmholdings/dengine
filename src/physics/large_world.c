/*
 * large_world.c: island solve origins and f32 precision helpers for large worlds (DENG-222,
 * SPEC §5.5, §5.7). See large_world.h for the model.
 */
#include "physics/large_world.h"

#include "physics/islands.h"
#include "physics/solver/solver_internal.h"

f64 de__phys_f32_spacing(f64 x) {
    const f32 f = (f32)de__absd(x);
    return (f64)de__f32_next_up(f) - (f64)f;
}

de_dvec3 de__phys_constraint_origin(const de__solver *s, const de__constraint *c) {
    const de_dvec3 zero = {0.0, 0.0, 0.0};
    const de__islands *is = s->islands;
    if (is == NULL)
        return zero;
    const u32 slot = (c->flags & DE__CONSTRAINT_WRITE_A) != 0u ? c->solver_a : c->solver_b;
    if (slot == DE__SOLVER_STATIC_SLOT || slot >= s->body_count)
        return zero;
    const u32 isl = is->island_of_slot[slot];
    if (isl == DE__ISLAND_NONE || isl >= is->island_count)
        return zero;
    return is->islands[isl].origin;
}

/* Scans the island lists of the last built step, so it also answers between steps (the body ->
 * slot map is cleared at finalize). For tests and debug views, not for per-constraint use. */
bool de__phys_body_origin(const de_physics_world *p, u32 body_index, de_dvec3 *out) {
    const de__islands *is = p != NULL ? de__islands_get(p) : NULL;
    if (is == NULL || out == NULL)
        return false;
    for (u32 i = 0; i < is->island_count; ++i) {
        const de__island *I = &is->islands[i];
        for (u32 k = 0; k < I->body_count; ++k) {
            if (is->island_bodies[I->first + k] == body_index) {
                *out = I->origin;
                return true;
            }
        }
    }
    return false;
}
