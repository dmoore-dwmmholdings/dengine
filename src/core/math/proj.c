/* proj.c: view and projection matrices, scalar reference (SPEC §5.4, DENG-241).
 *
 * View space is right-handed and looks down -Z (+Y up, +X right). Projections are reverse-Z with
 * clip z in [0, 1]: depth 1 at the near plane, 0 at the far plane or at infinity. Clip-space
 * differences between graphics APIs (Vulkan's Y flip) are the RHI's business, not this file's. */
#include "core/math/math_internal.h"

/* s = normalize(f x up), the view +X axis; falls back to other up vectors when up is parallel
 * to the view direction f (unit length). */
static de_vec3 de__look_at_side(de_vec3 f, de_vec3 up) {
    const de_vec3 candidates[3] = {up, DE_AXIS_FORWARD, DE_AXIS_RIGHT};
    for (int i = 0; i < 3; ++i) {
        de_vec3 s = de_v3_cross(f, candidates[i]);
        f32 len_sq = de_v3_length_sq(s);
        /* |f x up| = |up| sin(angle): compare against |up| so the scale of up does not matter. */
        if (len_sq > 1e-10f * de_v3_length_sq(candidates[i]))
            return de_v3_scale(s, 1.0f / sqrtf(len_sq));
    }
    return DE_AXIS_RIGHT; /* unreachable for a finite unit f: it cannot be parallel to both */
}

de_mat4 de_m4_look_at(de_vec3 eye, de_vec3 target, de_vec3 up) {
    de_vec3 f = de_v3_normalize(de_v3_sub(target, eye)); /* view -Z */
    de_vec3 s = de__look_at_side(f, up);                 /* view +X */
    de_vec3 u = de_v3_cross(s, f);                       /* view +Y */
    /* Rows: s, u, -f; translation moves eye to the origin. */
    de_mat4 m;
    m.m[0] = s.x;
    m.m[4] = s.y;
    m.m[8] = s.z;
    m.m[12] = -de_v3_dot(s, eye);
    m.m[1] = u.x;
    m.m[5] = u.y;
    m.m[9] = u.z;
    m.m[13] = -de_v3_dot(u, eye);
    m.m[2] = -f.x;
    m.m[6] = -f.y;
    m.m[10] = -f.z;
    m.m[14] = de_v3_dot(f, eye);
    m.m[3] = 0.0f;
    m.m[7] = 0.0f;
    m.m[11] = 0.0f;
    m.m[15] = 1.0f;
    return m;
}

de_mat4 de_m4_perspective_rz(f32 fov_y, f32 aspect, f32 near_z) {
    /* clip = (x f / aspect, y f, near_z, -z), so NDC z = near_z / -z: 1 at z = -near_z, -> 0 as
     * z -> -inf. The infinite far plane leaves no depth range to lose to precision, and reverse-Z
     * puts float's dense range near 0 where 1/z flattens out. */
    f32 f = 1.0f / tanf(0.5f * fov_y);
    de_mat4 m = {{0}};
    m.m[0] = f / aspect;
    m.m[5] = f;
    m.m[11] = -1.0f;  /* row 3, column 2: w = -z */
    m.m[14] = near_z; /* row 2, column 3: z = near_z */
    return m;
}

de_mat4 de_m4_ortho_rz(f32 left, f32 right, f32 bottom, f32 top, f32 near_z, f32 far_z) {
    /* z_ndc = a z + b with a (-near) + b = 1 and a (-far) + b = 0. */
    f32 rl = 1.0f / (right - left);
    f32 tb = 1.0f / (top - bottom);
    f32 fn = 1.0f / (far_z - near_z);
    de_mat4 m = {{0}};
    m.m[0] = 2.0f * rl;
    m.m[5] = 2.0f * tb;
    m.m[10] = fn;
    m.m[12] = -(right + left) * rl;
    m.m[13] = -(top + bottom) * tb;
    m.m[14] = far_z * fn;
    m.m[15] = 1.0f;
    return m;
}
