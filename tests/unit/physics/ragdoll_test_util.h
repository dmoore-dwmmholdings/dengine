/* ragdoll_test_util.h: a 15-bone humanoid ragdoll (DENG-224), shared by the ragdoll unit tests
 * (test_ragdoll.c) and the ragdoll_stairs determinism scenario. Test-only.
 *
 * Model space: Z up, the feet on z = 0, facing -Y, left side +X, T-pose (arms along +-X) as the
 * bind pose; about 1.78 m and 65 kg. Every bone's frame has the identity rotation in the bind
 * pose; joint frames orient the joint axes:
 *   - spine and neck: cone-twist, X up;
 *   - shoulders and wrists: cone-twist, X along the arm;
 *   - elbows: hinges about the vertical, flexing forward (-Y), [0, 2.4] rad;
 *   - hips and ankles: cone-twist, X down (swing about Z of the joint frame swings the leg
 *     forward and back, about Y sideways);
 *   - knees: hinges about X, flexing backward (+Y), [0, 2.4] rad.
 * SPEC §5.7: de_dmath only (also built into the deterministic scenario executable). */
#ifndef DE_RAGDOLL_TEST_UTIL_H
#define DE_RAGDOLL_TEST_UTIL_H

#include <dengine/de_physics.h>

#include <string.h>

#define RT_BONES 15u
#define RT_PI    3.14159265358979f

enum {
    RT_PELVIS,
    RT_CHEST,
    RT_HEAD,
    RT_UARM_L,
    RT_FARM_L,
    RT_HAND_L,
    RT_UARM_R,
    RT_FARM_R,
    RT_HAND_R,
    RT_THIGH_L,
    RT_SHIN_L,
    RT_FOOT_L,
    RT_THIGH_R,
    RT_SHIN_R,
    RT_FOOT_R
};

static inline de_quat rt_axis_angle(f32 x, f32 y, f32 z, f32 angle) {
    f32 s, c;
    de_dsincos(0.5f * angle, &s, &c);
    de_quat q;
    q.x = x * s;
    q.y = y * s;
    q.z = z * s;
    q.w = c;
    return q;
}

static inline de_quat rt_identity(void) {
    return rt_axis_angle(0.0f, 0.0f, 1.0f, 0.0f);
}

static inline de_transform rt_xform(f64 x, f64 y, f64 z, de_quat q) {
    de_transform t;
    memset(&t, 0, sizeof t);
    t.position.x = x;
    t.position.y = y;
    t.position.z = z;
    t.rotation = q;
    t.scale.x = 1.0f;
    t.scale.y = 1.0f;
    t.scale.z = 1.0f;
    return t;
}

/* One bone: parent, joint position (model space), mass. */
static inline de_ragdoll_bone rt_bone(u32 parent, f64 x, f64 y, f64 z, f32 mass) {
    de_ragdoll_bone b;
    memset(&b, 0, sizeof b);
    b.parent = parent;
    b.bind = rt_xform(x, y, z, rt_identity());
    b.shape_offset = rt_xform(0, 0, 0, rt_identity());
    b.mass = mass;
    b.joint_rotation = rt_identity();
    return b;
}

static inline void rt_capsule(de_ragdoll_bone *b, f32 r, f32 hh, f64 ox, f64 oy, f64 oz,
                              bool along_x) {
    b->shape = DE_RAGDOLL_SHAPE_CAPSULE;
    b->radius = r;
    b->half_height = hh;
    b->shape_offset = rt_xform(
        ox, oy, oz, along_x ? rt_axis_angle(0.0f, 1.0f, 0.0f, 0.5f * RT_PI) : rt_identity());
}

static inline void rt_box(de_ragdoll_bone *b, f32 hx, f32 hy, f32 hz, f64 ox, f64 oy, f64 oz) {
    b->shape = DE_RAGDOLL_SHAPE_BOX;
    b->half_extents.x = hx;
    b->half_extents.y = hy;
    b->half_extents.z = hz;
    b->shape_offset = rt_xform(ox, oy, oz, rt_identity());
}

static inline void rt_cone(de_ragdoll_bone *b, de_quat frame, f32 twist, f32 swing_y, f32 swing_z) {
    b->joint = DE_RAGDOLL_JOINT_CONE_TWIST;
    b->joint_rotation = frame;
    b->limit_min = -twist;
    b->limit_max = twist;
    b->swing_y = swing_y;
    b->swing_z = swing_z;
}

static inline void rt_hinge(de_ragdoll_bone *b, de_quat frame, f32 lo, f32 hi) {
    b->joint = DE_RAGDOLL_JOINT_HINGE;
    b->joint_rotation = frame;
    b->limit_min = lo;
    b->limit_max = hi;
}

/** Fills bones[RT_BONES]; every bone except the root gets `motor_force` (N m; 0 = unpowered). */
static inline void rt_humanoid(de_ragdoll_bone *bones, f32 motor_force) {
    const de_quat up = rt_axis_angle(0.0f, 1.0f, 0.0f, -0.5f * RT_PI);  /* X -> +Z */
    const de_quat down = rt_axis_angle(0.0f, 1.0f, 0.0f, 0.5f * RT_PI); /* X -> -Z */
    const de_quat left = rt_identity();                                 /* X -> +X */
    const de_quat right = rt_axis_angle(0.0f, 0.0f, 1.0f, RT_PI);       /* X -> -X */
    de_ragdoll_bone *b = bones;

    b[RT_PELVIS] = rt_bone(DE_RAGDOLL_NO_PARENT, 0, 0, 1.0, 12.0f);
    rt_box(&b[RT_PELVIS], 0.16f, 0.1f, 0.1f, 0, 0, 0);

    b[RT_CHEST] = rt_bone(RT_PELVIS, 0, 0, 1.1, 15.0f);
    rt_box(&b[RT_CHEST], 0.17f, 0.11f, 0.18f, 0, 0, 0.2);
    rt_cone(&b[RT_CHEST], up, 0.4f, 0.5f, 0.4f);

    b[RT_HEAD] = rt_bone(RT_CHEST, 0, 0, 1.55, 5.0f);
    b[RT_HEAD].shape = DE_RAGDOLL_SHAPE_SPHERE;
    b[RT_HEAD].radius = 0.11f;
    b[RT_HEAD].shape_offset = rt_xform(0, 0, 0.12, rt_identity());
    rt_cone(&b[RT_HEAD], up, 0.6f, 0.6f, 0.5f);

    for (u32 side = 0; side < 2u; ++side) {
        const f64 s = side == 0u ? 1.0 : -1.0;
        const u32 ua = side == 0u ? RT_UARM_L : RT_UARM_R;
        const u32 fa = side == 0u ? RT_FARM_L : RT_FARM_R;
        const u32 ha = side == 0u ? RT_HAND_L : RT_HAND_R;
        b[ua] = rt_bone(RT_CHEST, 0.2 * s, 0, 1.45, 2.5f);
        rt_capsule(&b[ua], 0.05f, 0.1f, 0.15 * s, 0, 0, true);
        rt_cone(&b[ua], side == 0u ? left : right, 0.8f, 1.2f, 1.2f);
        b[fa] = rt_bone(ua, 0.5 * s, 0, 1.45, 1.5f);
        rt_capsule(&b[fa], 0.045f, 0.1f, 0.14 * s, 0, 0, true);
        /* Flexing forward (-Y): about -Z on the left (+X) arm, +Z on the right. */
        rt_hinge(&b[fa], side == 0u ? down : up, 0.0f, 2.4f);
        b[ha] = rt_bone(fa, 0.78 * s, 0, 1.45, 0.5f);
        rt_box(&b[ha], 0.07f, 0.03f, 0.045f, 0.07 * s, 0, 0);
        rt_cone(&b[ha], side == 0u ? left : right, 0.5f, 0.6f, 0.4f);

        const u32 th = side == 0u ? RT_THIGH_L : RT_THIGH_R;
        const u32 sh = side == 0u ? RT_SHIN_L : RT_SHIN_R;
        const u32 ft = side == 0u ? RT_FOOT_L : RT_FOOT_R;
        b[th] = rt_bone(RT_PELVIS, 0.1 * s, 0, 0.95, 7.0f);
        rt_capsule(&b[th], 0.07f, 0.15f, 0, 0, -0.22, false);
        rt_cone(&b[th], down, 0.5f, 0.6f, 1.2f);
        b[sh] = rt_bone(th, 0.1 * s, 0, 0.5, 4.0f);
        rt_capsule(&b[sh], 0.055f, 0.15f, 0, 0, -0.22, false);
        rt_hinge(&b[sh], left, 0.0f, 2.4f);
        b[ft] = rt_bone(sh, 0.1 * s, 0, 0.08, 1.0f);
        rt_box(&b[ft], 0.05f, 0.1f, 0.04f, 0, -0.05, -0.03);
        rt_cone(&b[ft], down, 0.3f, 0.3f, 0.6f);
    }
    for (u32 i = 1; i < RT_BONES; ++i)
        b[i].motor_max_force = motor_force;
}

/** A desc for `bones` placed at `at`, friction 0.6, sleeping allowed. */
static inline de_ragdoll_desc rt_desc(const de_ragdoll_bone *bones, de_transform at) {
    de_ragdoll_desc d;
    memset(&d, 0, sizeof d);
    d.bones = bones;
    d.bone_count = RT_BONES;
    d.transform = at;
    d.material.friction = 0.6f;
    d.flags = (u32)DE_BODY_FLAG_ALLOW_SLEEP;
    d.owner = DE_ENTITY_NULL;
    return d;
}

#endif /* DE_RAGDOLL_TEST_UTIL_H */
