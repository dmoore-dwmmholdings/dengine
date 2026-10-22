#include "anim_internal.h"
void de__anim_pose(const de_cooked_skeleton_view *skeleton, const de__anim_local_pose *local,
                   de_affine *model, de_affine *palette) {
    for (u32 j = 0; j < skeleton->joint_count; ++j) {
        de_affine matrix =
            de_affine_from_trs(local->translations[j], local->rotations[j], local->scales[j]);
        u16 parent = de_cooked_skeleton_parent(skeleton, j);
        model[j] =
            parent == DE_COOKED_SKEL_NO_PARENT ? matrix : de_affine_mul(&model[parent], &matrix);
    }
    de_affine mesh_from_model;
    de_cooked_skeleton_mesh_from_model(skeleton, mesh_from_model.m);
    for (u32 i = 0; i < skeleton->mesh_joint_count; ++i) {
        u16 joint = de_cooked_skeleton_remap(skeleton, i);
        de_affine inverse;
        de_cooked_skeleton_inverse_bind(skeleton, joint, inverse.m);
        de_affine posed = de_affine_mul(&model[joint], &inverse);
        palette[i] = de_affine_mul(&mesh_from_model, &posed);
    }
}
