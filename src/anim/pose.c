#include "anim_internal.h"

/* anim is deterministic (SPEC §5.7): plain f32 arithmetic, compiled without FP contraction,
 * instead of the SIMD de_affine_* functions. */

/* [R(q) S | t], column-major (de_affine). */
static de_affine trs(const de_cooked_joint_pose *p) {
    const f32 x = p->r[0], y = p->r[1], z = p->r[2], w = p->r[3];
    const f32 xx = x * x, yy = y * y, zz = z * z;
    const f32 xy = x * y, xz = x * z, yz = y * z, wx = w * x, wy = w * y, wz = w * z;
    de_affine a;
    a.m[0] = (1.0f - 2.0f * (yy + zz)) * p->s[0];
    a.m[1] = 2.0f * (xy + wz) * p->s[0];
    a.m[2] = 2.0f * (xz - wy) * p->s[0];
    a.m[3] = 2.0f * (xy - wz) * p->s[1];
    a.m[4] = (1.0f - 2.0f * (xx + zz)) * p->s[1];
    a.m[5] = 2.0f * (yz + wx) * p->s[1];
    a.m[6] = 2.0f * (xz + wy) * p->s[2];
    a.m[7] = 2.0f * (yz - wx) * p->s[2];
    a.m[8] = (1.0f - 2.0f * (xx + yy)) * p->s[2];
    a.m[9] = p->t[0];
    a.m[10] = p->t[1];
    a.m[11] = p->t[2];
    return a;
}

/* a * b: b first, then a. */
static de_affine mul(const de_affine *a, const de_affine *b) {
    de_affine o;
    for (u32 c = 0; c < 4; ++c)
        for (u32 r = 0; r < 3; ++r) {
            f32 v = a->m[r] * b->m[c * 3] + a->m[3 + r] * b->m[c * 3 + 1] + a->m[6 + r] * b->m[c * 3 + 2];
            if (c == 3)
                v += a->m[9 + r];
            o.m[c * 3 + r] = v;
        }
    return o;
}

void de__anim_pose(const de_cooked_skeleton_view *skeleton, const de_cooked_joint_pose *local,
                   de_affine *model, de_affine *palette) {
    for (u32 j = 0; j < skeleton->joint_count; ++j) {
        const de_affine matrix = trs(&local[j]);
        u16 parent = de_cooked_skeleton_parent(skeleton, j);
        model[j] = parent == DE_COOKED_SKEL_NO_PARENT ? matrix : mul(&model[parent], &matrix);
    }
    de_affine mesh_from_model;
    de_cooked_skeleton_mesh_from_model(skeleton, mesh_from_model.m);
    for (u32 i = 0; i < skeleton->mesh_joint_count; ++i) {
        u16 joint = de_cooked_skeleton_remap(skeleton, i);
        de_affine inverse;
        de_cooked_skeleton_inverse_bind(skeleton, joint, inverse.m);
        const de_affine posed = mul(&model[joint], &inverse);
        palette[i] = mul(&mesh_from_model, &posed);
    }
}
