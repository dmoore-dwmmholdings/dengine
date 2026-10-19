#ifndef DE_JOINT2D_INTERNAL_H
#define DE_JOINT2D_INTERNAL_H
#include "physics/2d/constraint2d.h"
/* The broadphase asks this before retaining each pair. */
bool de__joints2d_collide(const de_physics2d_world *p, u32 a, u32 b);
u64 de__joints2d_checksum(const de_physics2d_world *p, u64 seed);
#endif
