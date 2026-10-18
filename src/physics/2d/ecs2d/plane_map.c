#include "physics/2d/ecs2d/plane_map.h"
#include "physics/physics_internal.h"
#include <dengine/de_debug.h>

de_transform2d de__plane2d_from_world(de_physics2d_plane plane, const de_transform *t) {
    de_transform2d r;
    r.x = t->position.x;
    r.y = plane == DE_PHYSICS2D_PLANE_SIDE ? t->position.z : t->position.y;
    de_quat q = t->rotation;
    if (plane == DE_PHYSICS2D_PLANE_SIDE) {
        r.rotation = de_rot2d_from_angle(2 * de_datan2(-q.y, q.w));
        if (de__absf(q.x) + de__absf(q.z) > 1e-4f)
            DE_WARN(de__physics_log(), "2D side transform ignores out-of-plane tilt");
    } else {
        r.rotation = de_rot2d_from_angle(2 * de_datan2(q.z, q.w));
        if (de__absf(q.x) + de__absf(q.y) > 1e-4f)
            DE_WARN(de__physics_log(), "2D top transform ignores out-of-plane tilt");
    }
    return r;
}
de_transform de__plane2d_to_world(de_physics2d_plane plane, de_transform2d t, de_transform keep) {
    keep.position.x = t.x;
    f32 angle = de_datan2(t.rotation.s, t.rotation.c);
    f32 s, c;
    de_dsincos(angle * 0.5f, &s, &c);
    keep.rotation = (de_quat){0, 0, 0, c};
    if (plane == DE_PHYSICS2D_PLANE_SIDE) {
        keep.position.z = t.y;
        keep.rotation.y = -s;
    } else {
        keep.position.y = t.y;
        keep.rotation.z = s;
    }
    return keep;
}
de_vec3 de__plane2d_vector(de_physics2d_plane plane, de_vec2 v) {
    return plane == DE_PHYSICS2D_PLANE_SIDE ? (de_vec3){v.x, 0, v.y} : (de_vec3){v.x, v.y, 0};
}
