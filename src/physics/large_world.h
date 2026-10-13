/*
 * large_world.h: large-world precision rules of the 3D physics solver (DENG-222, SPEC §5.5,
 * §13.1, §5.7; DENG-2 Q10: worlds up to +-100 km without rebasing). Internal: include as
 * "physics/large_world.h".
 *
 * The model:
 *   - Body positions are f64 (de_dvec3). At 100 km an f32 has a spacing of 2^-7 m (7.8 mm), so
 *     no world position is ever rounded to f32 in the solver.
 *   - The solver integrates f32 displacements dp / dq from zero each step and adds them to the
 *     f64 positions once at the end (DENG-217, integrate.c).
 *   - Each island has a solve origin (de__island.origin, islands.c): the f64 position of its
 *     lowest-index body, set every step by de__islands_build. Constraint setup that needs world
 *     points forms them relative to that origin in f64, so every f64 term stays small, and rounds
 *     only the final offset (anchor minus center of mass) to f32 (de__phys_relative).
 *   - Offsets are measured from a center of mass, so they are about the size of the body. A
 *     static body whose contacts lie kilometres from its center (one huge terrain or level mesh)
 *     still gives km-sized f32 offsets, in the narrowphase (A's frame) and here: keep static
 *     bodies within about 1 km of their own origin (streaming cells are 256 m, DENG-2 Q10).
 *   - Broadphase and world AABBs stay f32 in world coordinates, rounded outward from f64
 *     (DENG-132, DENG-133). At 100 km they are loose by at most one f32 spacing (7.8 mm) per
 *     side, which the fat margin (physics.broadphase.margin, 0.05 m = 6.4 spacings) and the pair
 *     distance absorb: no pair is lost, at most a few extra candidates reach the narrowphase,
 *     which works in A's frame (f64 relative pose, rounded once).
 *
 * Determinism (SPEC §5.7): the helpers are f64 + - and one f32 cast per component, no FMA (the
 * module is built with -ffp-contract=off).
 */
#ifndef DE_PHYSICS_LARGE_WORLD_H
#define DE_PHYSICS_LARGE_WORLD_H

#include "physics/physics_internal.h"

DE_EXTERN_C_BEGIN

typedef struct de__solver de__solver;
typedef struct de__constraint de__constraint;

/** Largest coordinate of a supported world (m): DENG-2 Q10, +-100 km. */
#define DE__PHYS_WORLD_LIMIT 100000.0
/**
 * Largest offset (m) that solver setup may round to f32. Every f32 vector the solver forms from
 * world points is an offset from a center of mass or an island origin, so it is about the size
 * of a body or a joint. A raw world position 100 km out is far above this, so the Debug check in
 * de__phys_relative catches a path that rounds world coordinates to f32. f32 spacing at this
 * size: about 2 mm.
 */
#define DE__PHYS_MAX_RELATIVE 16384.0

/** true when `d` (an offset formed in f64) is small enough to be rounded to f32 by the solver. */
static inline bool de__phys_relative_ok(de_dvec3 d) {
    const f64 m = DE__PHYS_MAX_RELATIVE;
    return d.x >= -m && d.x <= m && d.y >= -m && d.y <= m && d.z >= -m && d.z <= m;
}

/**
 * `world - origin`, subtracted in f64 and then rounded to f32 once per component: the only way
 * the solver turns f64 world points into f32 (same arithmetic as de_dv3_relative in de_math.h).
 * Debug and Dev builds assert that the offset is below DE__PHYS_MAX_RELATIVE.
 */
static inline de_vec3 de__phys_relative(de_dvec3 world, de_dvec3 origin) {
    const de_dvec3 d = de__d3_sub(world, origin);
    DE_ASSERT(de__phys_relative_ok(d));
    return de__v3((f32)d.x, (f32)d.y, (f32)d.z);
}

/**
 * The f32 spacing (ulp) at coordinate `x`: the most an outward-rounded f32 bound at `x` lies
 * outside the f64 value. 2^-7 m (7.8 mm) for 65,536 m <= |x| < 131,072 m, so at 100 km.
 */
f64 de__phys_f32_spacing(f64 x);

/**
 * The solve origin of constraint record `c` this step: the origin of the island of its writable
 * side (an awake dynamic body). Valid from de__islands_build until the end of the step, so in
 * prepare and the later stages; zero when the record has no island (it is not solved).
 */
de_dvec3 de__phys_constraint_origin(const de__solver *s, const de__constraint *c);

/**
 * The solve origin of the island of body `body_index` this step, or false when the body has no
 * island this step (static, kinematic, asleep, or the solver has not prepared the step).
 */
bool de__phys_body_origin(const de_physics_world *p, u32 body_index, de_dvec3 *out);

DE_EXTERN_C_END

#endif /* DE_PHYSICS_LARGE_WORLD_H */
