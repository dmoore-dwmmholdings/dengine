#include "anim_internal.h"
void de__anim_pose(const de_cooked_skeleton_view *skeleton, const de_cooked_joint_pose *local,
                   de_affine *model, de_affine *palette) {
    for (u32 j = 0; j < skeleton->joint_count; ++j) {
        const de_cooked_joint_pose *p = &local[j];
        de_affine matrix = de_affine_from_trs((de_vec3){p->t[0], p->t[1], p->t[2]},
                                              (de_quat){p->r[0], p->r[1], p->r[2], p->r[3]},
                                              (de_vec3){p->s[0], p->s[1], p->s[2]});
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
