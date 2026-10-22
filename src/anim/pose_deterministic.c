#include "anim_internal.h"

static de_affine compose(const de_affine *a, const de_affine *b) {
    de_affine result;
    for (u32 col = 0; col < 4; ++col)
        for (u32 row = 0; row < 3; ++row) {
            f32 x = a->m[row] * b->m[col * 3];
            f32 y = a->m[3 + row] * b->m[col * 3 + 1];
            f32 z = a->m[6 + row] * b->m[col * 3 + 2];
            result.m[col * 3 + row] = (x + y) + z;
            if (col == 3)
                result.m[col * 3 + row] += a->m[9 + row];
        }
    return result;
}
static de_affine local_matrix(de_vec3 t, de_quat q, de_vec3 s) {
    f32 xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    f32 xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    f32 wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    return (de_affine){{(1 - 2 * (yy + zz)) * s.x, 2 * (xy + wz) * s.x, 2 * (xz - wy) * s.x,
                        2 * (xy - wz) * s.y, (1 - 2 * (xx + zz)) * s.y, 2 * (yz + wx) * s.y,
                        2 * (xz + wy) * s.z, 2 * (yz - wx) * s.z, (1 - 2 * (xx + yy)) * s.z, t.x,
                        t.y, t.z}};
}
void de__anim_pose_deterministic(const de_cooked_skeleton_view *skel,
                                 const de__anim_local_pose *local, de_affine *model,
                                 de_affine *palette) {
    for (u32 j = 0; j < skel->joint_count; ++j) {
        de_affine matrix =
            local_matrix(local->translations[j], local->rotations[j], local->scales[j]);
        u16 parent = de_cooked_skeleton_parent(skel, j);
        model[j] = parent == DE_COOKED_SKEL_NO_PARENT ? matrix : compose(&model[parent], &matrix);
    }
    de_affine mesh;
    de_cooked_skeleton_mesh_from_model(skel, mesh.m);
    for (u32 i = 0; i < skel->mesh_joint_count; ++i) {
        u16 joint = de_cooked_skeleton_remap(skel, i);
        de_affine inverse;
        de_cooked_skeleton_inverse_bind(skel, joint, inverse.m);
        de_affine posed = compose(&model[joint], &inverse);
        palette[i] = compose(&mesh, &posed);
    }
}
