#ifndef DE_PLANE_MAP_H
#define DE_PLANE_MAP_H
#include <dengine/de_physics.h>
/* Position mapping preserves the caller's depth axis. Rotations ignore out-of-plane tilt. */
de_transform2d de__plane2d_from_world(de_physics2d_plane plane, const de_transform *t);
de_transform de__plane2d_to_world(de_physics2d_plane plane, de_transform2d t, de_transform keep);
de_vec3 de__plane2d_vector(de_physics2d_plane plane, de_vec2 v);
#endif
