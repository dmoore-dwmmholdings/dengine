/*
 * test_ragdoll.c: ragdolls (DENG-224; SPEC §13.4): building a 15-bone humanoid (bodies and joints
 * in bone order, poses, handles, destroy and rebuild), refused descs, a drop from 2 m that settles
 * within 3 s with joints together and limits held, a throw with limits held, the self-collision
 * rule, a powered ragdoll holding a T-pose and new motor targets, and no allocation per step.
 * DENG-404: two ragdolls side by side, arms end to end, stay finite.
 */
#include "joint_test_util.h"
#include "ragdoll_test_util.h"

#include "physics/ragdoll_internal.h"

#define RD_DEG (RT_PI / 180.0f)

static de_physics_world *rd_world(u32 max_bodies, const de_allocator *a) {
    de_physics_world *p = st_world(max_bodies, NULL, a);
    if (p != NULL) {
        const de_shape g = st_box(p, 20.0f, 20.0f, 0.5f);
        (void)st_body(p, DE_BODY_STATIC, g, st_xform(0, 0, -0.5, st_quat_identity()), 0.8f, 0.0f);
    }
    return p;
}

static f32 rd_angle(de_quat a, de_quat b) {
    return jt_rot_error(a, b);
}

/* Separation of joint j's anchors (m). */
static f64 rd_separation(const de_physics_world *p, de_joint h) {
    const de__joint *j = de__joint_resolve(p, h);
    if (j == NULL)
        return 0.0;
    const de_dvec3 la = {j->local_a.x, j->local_a.y, j->local_a.z};
    const de_dvec3 lb = {j->local_b.x, j->local_b.y, j->local_b.z};
    const de_dvec3 wa = jt_world_point(p, de__body_handle(p, j->body_a), la);
    const de_dvec3 wb = jt_world_point(p, de__body_handle(p, j->body_b), lb);
    return jt_dist(wa, wb);
}

/* How far (rad) joint j is past its limits; 0 inside them. Cone: the swing past the ellipse in
 * the swing's direction, and the twist; hinge: the angle and the axis misalignment. */
static f32 rd_limit_excess(const de_physics_world *p, de_joint h) {
    const de__joint *j = de__joint_resolve(p, h);
    if (j == NULL)
        return 0.0f;
    f32 twist, sy, sz;
    jt_angles(p->bodies.rotation[j->body_a], j->frame_qa, p->bodies.rotation[j->body_b],
              j->frame_qb, &twist, &sy, &sz);
    f32 ex = 0.0f;
    const f32 swing = de_dsqrt(sy * sy + sz * sz);
    if (j->kind == (u8)DE_JOINT_HINGE) {
        ex = de__maxf(ex, de__maxf(j->lo[0] - twist, twist - j->hi[0]));
        ex = de__maxf(ex, swing); /* the hinge axes stay aligned */
    } else {
        ex = de__maxf(ex, de__maxf(j->lo[3] - twist, twist - j->hi[3]));
        if (swing > 1.0e-5f) {
            const f32 ey = (sy / swing) / de__maxf(j->hi[4], 1.0e-3f);
            const f32 ez = (sz / swing) / de__maxf(j->hi[5], 1.0e-3f);
            ex = de__maxf(ex, swing - 1.0f / de_dsqrt(ey * ey + ez * ez));
        }
    }
    return ex;
}

typedef struct rd_stats {
    f64 max_sep;    /* largest joint separation over the run (m) */
    f32 max_excess; /* largest limit excess over the run (rad) */
    f64 last_sep;   /* the same over the last second */
    f32 last_excess;
} rd_stats;

/* Joint separation and limit excess after a step; `last` marks the last second of the run. */
static void rd_measure(const de_physics_world *p, de_ragdoll r, rd_stats *s, bool last) {
    for (u32 i = 1; i < RT_BONES; ++i) {
        const de_joint j = de_ragdoll_bone_joint(p, r, i);
        const f64 sep = rd_separation(p, j);
        const f32 ex = rd_limit_excess(p, j);
        s->max_sep = sep > s->max_sep ? sep : s->max_sep;
        s->max_excess = ex > s->max_excess ? ex : s->max_excess;
        if (last) {
            s->last_sep = sep > s->last_sep ? sep : s->last_sep;
            s->last_excess = ex > s->last_excess ? ex : s->last_excess;
        }
    }
}

static f32 rd_max_speed(const de_physics_world *p, de_ragdoll r) {
    f32 m = 0.0f;
    for (u32 i = 0; i < RT_BONES; ++i) {
        const de_body b = de_ragdoll_bone_body(p, r, i);
        m = de__maxf(m, st_lenf(st_vel(p, b)));
    }
    return m;
}

/* Bone i's rotation relative to its parent, from a pose. */
static de_quat rd_local(const de_transform *pose, const de_ragdoll_bone *bones, u32 i) {
    return jt_qmul(de__jq_conj(pose[bones[i].parent].rotation), pose[i].rotation);
}

/* ================================ Build ================================================= */

TEST(physics_ragdoll, build_humanoid) {
    de_physics_world *p = st_world(64, NULL, NULL);
    ASSERT_TRUE(p != NULL);
    de_ragdoll_bone bones[RT_BONES];
    rt_humanoid(bones, 0.0f);
    const de_quat yaw = st_quat_axis_angle((de_vec3){0, 0, 1}, 0.7f);
    de_ragdoll_desc d = rt_desc(bones, st_xform(3.0, -2.0, 0.5, yaw));
    d.owner.bits = 0x500000003ull;
    const de_ragdoll r = de_ragdoll_create(p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(r));
    EXPECT_TRUE(de_ragdoll_valid(p, r));
    EXPECT_EQ(de_ragdoll_bone_count(p, r), RT_BONES);
    EXPECT_EQ(de_body_count(p), RT_BONES);
    EXPECT_FALSE(DE_HANDLE_VALID(de_ragdoll_bone_joint(p, r, 0)));
    EXPECT_FALSE(DE_HANDLE_VALID(de_ragdoll_bone_body(p, r, RT_BONES)));

    /* Bodies and joints in bone order; one group with the automatic bit; the owner. */
    f32 mass = 0.0f;
    const de__ragdoll *rr = de__ragdoll_resolve(p, r);
    ASSERT_TRUE(rr != NULL);
    u32 last_joint = 0;
    for (u32 i = 0; i < RT_BONES; ++i) {
        const de_body b = de_ragdoll_bone_body(p, r, i);
        ASSERT_TRUE(de_body_valid(p, b));
        EXPECT_EQ(de__handle_index(b.id), i);
        const u32 bi = de__handle_index(b.id);
        EXPECT_EQ(p->bodies.type[bi], (u8)DE_BODY_DYNAMIC);
        EXPECT_EQ(p->bodies.filter[bi].group, rr->group);
        EXPECT_EQ(p->bodies.owner[bi].bits, d.owner.bits);
        EXPECT_TRUE((p->bodies.flags[bi] & (u32)DE_BODY_FLAG_ALLOW_SLEEP) != 0u);
        mass += p->bodies.mass[bi];
        if (i > 0u) {
            const de_joint j = de_ragdoll_bone_joint(p, r, i);
            ASSERT_TRUE(de_joint_valid(p, j));
            EXPECT_TRUE(i == 1u || de__handle_index(j.id) > last_joint);
            last_joint = de__handle_index(j.id);
            ST_LT(rd_separation(p, j), 1.0e-6);
            ST_LT(rd_limit_excess(p, j), 1.0e-5f);
        }
    }
    EXPECT_TRUE((rr->group & DE_RAGDOLL_AUTO_GROUP_BIT) != 0u);
    ST_LT(de__absf(mass - 65.0f), 1.0e-3f);

    /* The pose is the bind pose placed by the transform. */
    de_transform pose[RT_BONES];
    EXPECT_FALSE(de_ragdoll_get_pose(p, r, pose, RT_BONES - 1u));
    ASSERT_TRUE(de_ragdoll_get_pose(p, r, pose, RT_BONES));
    for (u32 i = 0; i < RT_BONES; ++i) {
        const de_vec3 bp = {(f32)bones[i].bind.position.x, (f32)bones[i].bind.position.y,
                            (f32)bones[i].bind.position.z};
        const de_vec3 w = de__quat_rotate(yaw, bp);
        const de_dvec3 expect = {3.0 + w.x, -2.0 + w.y, 0.5 + w.z};
        ST_LT(jt_dist(pose[i].position, expect), 1.0e-5);
        ST_LT(rd_angle(pose[i].rotation, yaw), 1.0e-5f);
    }

    /* set_pose / get_pose round trip; set_velocity is a rigid motion. */
    pose[RT_HEAD].position.z += 0.25;
    pose[RT_HEAD].rotation = st_quat_axis_angle((de_vec3){1, 0, 0}, 0.3f);
    ASSERT_TRUE(de_ragdoll_set_pose(p, r, pose, RT_BONES));
    de_transform back[RT_BONES];
    ASSERT_TRUE(de_ragdoll_get_pose(p, r, back, RT_BONES));
    ST_LT(jt_dist(back[RT_HEAD].position, pose[RT_HEAD].position), 1.0e-6);
    ST_LT(rd_angle(back[RT_HEAD].rotation, pose[RT_HEAD].rotation), 1.0e-5f);
    de_transform bad[RT_BONES];
    memcpy(bad, pose, sizeof bad);
    volatile f64 zero = 0.0;
    bad[3].position.x = 1.0 / zero;
    EXPECT_FALSE(de_ragdoll_set_pose(p, r, bad, RT_BONES));
    ASSERT_TRUE(de_ragdoll_set_velocity(p, r, (de_vec3){1, 0, 0}, (de_vec3){0, 0, 2}));
    ST_LT(st_lenf(
              de__v3_sub(st_angvel(p, de_ragdoll_bone_body(p, r, RT_FOOT_L)), (de_vec3){0, 0, 2})),
          1.0e-6f);
    ST_LT(st_lenf(de__v3_sub(st_vel(p, de_ragdoll_bone_body(p, r, RT_PELVIS)), (de_vec3){1, 0, 0})),
          1.0e-5f);

    /* A second ragdoll has another automatic group. */
    const de_ragdoll r2 = de_ragdoll_create(p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(r2));
    EXPECT_NE(de__ragdoll_resolve(p, r2)->group, de__ragdoll_resolve(p, r)->group);

    /* Destroy: everything goes; a rebuild gets the same indices; stale handles are ignored. */
    u32 body_idx[RT_BONES], joint_idx[RT_BONES];
    for (u32 i = 0; i < RT_BONES; ++i) {
        body_idx[i] = de__handle_index(de_ragdoll_bone_body(p, r2, i).id);
        joint_idx[i] = i == 0u ? 0u : de__handle_index(de_ragdoll_bone_joint(p, r2, i).id);
    }
    const u32 shapes_before = p->shape_live_count;
    de_ragdoll_destroy(p, r2);
    EXPECT_FALSE(de_ragdoll_valid(p, r2));
    EXPECT_EQ(de_body_count(p), RT_BONES);
    EXPECT_EQ(p->shape_live_count, shapes_before - RT_BONES);
    de_ragdoll_destroy(p, r2);
    EXPECT_EQ(de_ragdoll_bone_count(p, r2), 0u);
    const de_ragdoll r3 = de_ragdoll_create(p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(r3));
    EXPECT_NE(r3.id, r2.id);
    for (u32 i = 0; i < RT_BONES; ++i) {
        EXPECT_EQ(de__handle_index(de_ragdoll_bone_body(p, r3, i).id), body_idx[i]);
        if (i > 0u)
            EXPECT_EQ(de__handle_index(de_ragdoll_bone_joint(p, r3, i).id), joint_idx[i]);
    }
    de_ragdoll_destroy(p, r3);
    de_ragdoll_destroy(p, r);
    EXPECT_EQ(de_body_count(p), 0u);
    EXPECT_EQ(p->shape_live_count, 0u);
    de_physics_destroy(p);
}

TEST(physics_ragdoll, create_errors) {
    de_tracking_allocator t;
    de_allocator al = de_tracking_allocator_init(&t, NULL, DE_MEM_PHYSICS);
    de_physics_world *p = st_world(64, NULL, &al);
    ASSERT_TRUE(p != NULL);
    de_ragdoll_bone bones[RT_BONES];
    de_ragdoll_desc d;

    EXPECT_FALSE(DE_HANDLE_VALID(de_ragdoll_create(p, NULL)));
    rt_humanoid(bones, 0.0f);
    d = rt_desc(bones, st_xform(0, 0, 0, st_quat_identity()));
    EXPECT_FALSE(DE_HANDLE_VALID(de_ragdoll_create(NULL, &d)));
    d.bone_count = 0;
    EXPECT_FALSE(DE_HANDLE_VALID(de_ragdoll_create(p, &d)));
    d.bone_count = DE_RAGDOLL_MAX_BONES + 1u;
    EXPECT_FALSE(DE_HANDLE_VALID(de_ragdoll_create(p, &d)));
    d = rt_desc(NULL, st_xform(0, 0, 0, st_quat_identity()));
    EXPECT_FALSE(DE_HANDLE_VALID(de_ragdoll_create(p, &d)));

    /* One broken field at a time. */
    for (u32 k = 0; k < 10u; ++k) {
        rt_humanoid(bones, 0.0f);
        d = rt_desc(bones, st_xform(0, 0, 0, st_quat_identity()));
        switch (k) {
        case 0:
            bones[0].parent = 0;
            break; /* the root has a parent */
        case 1:
            bones[4].parent = 4;
            break; /* not a lower index */
        case 2:
            bones[5].mass = -1.0f;
            break;
        case 3:
            bones[2].radius = 0.0f;
            break;
        case 4:
            bones[6].limit_min = 1.0f;
            break; /* min > max */
        case 5:
            bones[9].swing_y = -0.1f;
            break;
        case 6:
            bones[3].shape = (de_ragdoll_shape)7;
            break;
        case 7:
            bones[3].joint = (de_ragdoll_joint)9;
            break;
        case 8:
            bones[7].motor_max_force = -2.0f;
            break;
        default:
            d.linear_damping = -1.0f;
            break;
        }
        EXPECT_FALSE(DE_HANDLE_VALID(de_ragdoll_create(p, &d)));
        EXPECT_EQ(de_body_count(p), 0u);
    }
    EXPECT_EQ(p->shape_live_count, 0u);

    /* A full body pool: nothing is left behind. */
    rt_humanoid(bones, 0.0f);
    d = rt_desc(bones, st_xform(0, 0, 0, st_quat_identity()));
    const de_shape filler = st_box(p, 0.1f, 0.1f, 0.1f);
    for (u32 i = 0; i < 55u; ++i)
        (void)st_body(p, DE_BODY_STATIC, filler, st_xform(10.0 * i, 0, 0, st_quat_identity()), 0,
                      0);
    EXPECT_FALSE(DE_HANDLE_VALID(de_ragdoll_create(p, &d)));
    EXPECT_EQ(de_error_last()->result, DE_ERR_OUT_OF_MEMORY);
    EXPECT_EQ(de_body_count(p), 55u);
    EXPECT_EQ(p->shape_live_count, 1u);
    de_physics_destroy(p);
    EXPECT_EQ(de_tracking_allocator_stats(&t).bytes_live, 0u);
}

/* ================================ Dynamics ============================================== */

/*
 * A drop from 2 m (feet 2 m up, leaning forward, a little spin). At rest after 3 s: slower than
 * 5 cm/s, joints together within 1 cm and limits within 5 degrees over the last second, every bone
 * on the ground near the drop point. The impact itself stretches the soft joints for a few ticks
 * (DENG-220: rigid joints are 60 Hz springs at 4 substeps, so a joint that stops a limb hitting
 * the ground at v stretches about v / (2 pi 60 Hz)): about 2 cm and 10 degrees at the peak, which
 * the bounds below allow, while an explosion would not pass them.
 */
TEST(physics_ragdoll, settles_from_2m) {
    de_physics_world *p = rd_world(64, NULL);
    ASSERT_TRUE(p != NULL);
    de_ragdoll_bone bones[RT_BONES];
    rt_humanoid(bones, 0.0f);
    const de_quat lean = st_quat_axis_angle((de_vec3){1, 0.3f, 0}, 0.35f);
    de_ragdoll_desc d = rt_desc(bones, st_xform(0, 0, 2.0, lean));
    d.angular_velocity = (de_vec3){0.5f, 0.0f, 0.8f};
    const de_ragdoll r = de_ragdoll_create(p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(r));
    rd_stats s = {0};
    for (u32 tick = 0; tick < 180u; ++tick) {
        de_physics_step(p, ST_DT);
        rd_measure(p, r, &s, tick >= 120u);
    }
    const f32 speed = rd_max_speed(p, r);
    fprintf(stderr,
            "  drop 2 m: peak separation %.4f m, peak limit excess %.2f deg; last second %.4f m, "
            "%.2f deg; speed at 3 s %.4f m/s\n",
            s.max_sep, (double)(s.max_excess / RD_DEG), s.last_sep,
            (double)(s.last_excess / RD_DEG), (double)speed);
    ST_LT(speed, 0.05f);
    ST_LT(s.last_sep, 0.01);
    ST_LT(s.last_excess, 5.0f * RD_DEG);
    ST_LT(s.max_sep, 0.04);
    ST_LT(s.max_excess, 15.0f * RD_DEG);
    de_transform pose[RT_BONES];
    ASSERT_TRUE(de_ragdoll_get_pose(p, r, pose, RT_BONES));
    for (u32 i = 0; i < RT_BONES; ++i) {
        ST_LT(pose[i].position.x * pose[i].position.x + pose[i].position.y * pose[i].position.y,
              2.5 * 2.5);
        ST_GT(pose[i].position.z, -0.05);
        ST_LT(pose[i].position.z, 0.6);
    }
    de_physics_destroy(p);
}

/* Thrown at 7 m/s with 8 rad/s of spin onto the ground: the limits and joints recover once it
 * lies still (last second), and the impacts stay bounded. */
TEST(physics_ragdoll, limits_hold_when_thrown) {
    de_physics_world *p = rd_world(64, NULL);
    ASSERT_TRUE(p != NULL);
    de_ragdoll_bone bones[RT_BONES];
    rt_humanoid(bones, 0.0f);
    de_ragdoll_desc d = rt_desc(bones, st_xform(-3.0, 0, 1.0, st_quat_identity()));
    d.linear_velocity = (de_vec3){6.0f, 1.0f, 3.0f};
    d.angular_velocity = (de_vec3){4.0f, -6.0f, 3.0f};
    const de_ragdoll r = de_ragdoll_create(p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(r));
    rd_stats s = {0};
    for (u32 tick = 0; tick < 300u; ++tick) {
        de_physics_step(p, ST_DT);
        rd_measure(p, r, &s, tick >= 240u);
    }
    fprintf(stderr,
            "  thrown: peak separation %.4f m, peak limit excess %.2f deg; last second %.4f m, "
            "%.2f deg; speed %.4f m/s\n",
            s.max_sep, (double)(s.max_excess / RD_DEG), s.last_sep,
            (double)(s.last_excess / RD_DEG), (double)rd_max_speed(p, r));
    ST_LT(s.last_sep, 0.01);
    ST_LT(s.last_excess, 5.0f * RD_DEG);
    ST_LT(s.max_sep, 0.06);
    ST_LT(s.max_excess, 30.0f * RD_DEG);
    de_physics_destroy(p);
}

/* ================================ Self-collision ======================================== */

static bool rd_adjacent(const de_ragdoll_bone *bones, u32 a, u32 b) {
    return bones[a].parent == b || bones[b].parent == a;
}

/* 1 when body index `bi` is a bone of a, 2 for b, else 0. */
static u32 rd_owner_of(const de_physics_world *p, de_ragdoll a, de_ragdoll b, u32 bi) {
    for (u32 i = 0; i < RT_BONES; ++i) {
        if (de__handle_index(de_ragdoll_bone_body(p, a, i).id) == bi)
            return 1u;
        if (de__handle_index(de_ragdoll_bone_body(p, b, i).id) == bi)
            return 2u;
    }
    return 0u;
}

TEST(physics_ragdoll, self_collision) {
    de_physics_world *p = rd_world(128, NULL);
    ASSERT_TRUE(p != NULL);
    de_ragdoll_bone bones[RT_BONES];
    rt_humanoid(bones, 0.0f);
    /* Default: one group, no pair inside the ragdoll; the bones of two ragdolls collide. */
    de_ragdoll_desc d = rt_desc(bones, st_xform(0, 0, 0.0, st_quat_identity()));
    const de_ragdoll a = de_ragdoll_create(p, &d);
    d.transform = st_xform(0.0, 0.35, 0.0, st_quat_axis_angle((de_vec3){0, 0, 1}, 0.5f * RT_PI));
    const de_ragdoll b = de_ragdoll_create(p, &d);
    /* self_collision: jointed pairs only are excluded. The hands get layer 2 and mask 1, so they
     * miss each other but hit every other bone. */
    rt_humanoid(bones, 0.0f);
    bones[RT_HAND_L].filter = (de_collision_filter){2u, 1u, 0u};
    bones[RT_HAND_R].filter = (de_collision_filter){2u, 1u, 0u};
    d = rt_desc(bones, st_xform(0, 5.0, 0.0, st_quat_identity()));
    d.self_collision = true;
    const de_ragdoll c = de_ragdoll_create(p, &d);
    /* An explicit group is used as given. */
    d.self_collision = false;
    d.filter.group = 77u;
    d.transform.position.y = 10.0;
    const de_ragdoll e = de_ragdoll_create(p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(a) && DE_HANDLE_VALID(b) && DE_HANDLE_VALID(c) &&
                DE_HANDLE_VALID(e));
    EXPECT_EQ(de__ragdoll_resolve(p, c)->group, 0u);
    EXPECT_EQ(de__ragdoll_resolve(p, e)->group, 77u);

    u32 c_pairs = 0;
    for (u32 i = 0; i < RT_BONES; ++i) {
        for (u32 k = 0; k < RT_BONES; ++k) {
            const u32 ai = de__handle_index(de_ragdoll_bone_body(p, a, i).id);
            const u32 ak = de__handle_index(de_ragdoll_bone_body(p, a, k).id);
            const u32 bk = de__handle_index(de_ragdoll_bone_body(p, b, k).id);
            const u32 ci = de__handle_index(de_ragdoll_bone_body(p, c, i).id);
            const u32 ck = de__handle_index(de_ragdoll_bone_body(p, c, k).id);
            const u32 ei = de__handle_index(de_ragdoll_bone_body(p, e, i).id);
            const u32 ek = de__handle_index(de_ragdoll_bone_body(p, e, k).id);
            if (i != k) {
                EXPECT_FALSE(de__bp_should_collide(p, ai, ak));
                EXPECT_FALSE(de__bp_should_collide(p, ei, ek));
            }
            EXPECT_TRUE(de__bp_should_collide(p, ai, bk));
            if (i < k) {
                const bool hands = (i == RT_HAND_L && k == RT_HAND_R);
                const bool expect = !rd_adjacent(bones, i, k) && !hands;
                EXPECT_EQ(de__bp_should_collide(p, ci, ck), expect);
                c_pairs += expect ? 1u : 0u;
            }
        }
    }
    EXPECT_EQ(c_pairs, (RT_BONES * (RT_BONES - 1u)) / 2u - (RT_BONES - 1u) - 1u);

    /* Dropped in a heap, the default ragdolls never make a contact between their own bones. */
    for (u32 tick = 0; tick < 90u; ++tick)
        de_physics_step(p, ST_DT);
    const de__manifold *m = NULL;
    u32 mc = 0;
    de__contacts_manifolds(p, &m, &mc);
    u32 between = 0;
    for (u32 i = 0; i < mc; ++i) {
        const u32 ra = rd_owner_of(p, a, b, m[i].key.a), rb = rd_owner_of(p, a, b, m[i].key.b);
        EXPECT_TRUE(ra == 0u || ra != rb); /* never two bones of one ragdoll */
        between += (ra != 0u && rb != 0u && ra != rb) ? 1u : 0u;
    }
    EXPECT_GT(between, 0u); /* the two default ragdolls do touch each other */
    de_physics_destroy(p);
}

/* The DENG-224 repro of DENG-404: two humanoids side by side, the second 0.6 m along +X, so
 * the first's right upper arm and the second's right forearm lie end to end on one axis, 1.5 cm
 * apart. The narrowphase made a -9.5 cm point there, and the friction clamp NaN velocities in
 * the first step. Every bone stays finite, and the two arms never push each other. */
TEST(physics_ragdoll, side_by_side_arms_stay_finite) {
    de_physics_world *p = rd_world(64, NULL);
    ASSERT_TRUE(p != NULL);
    de_ragdoll_bone bones[RT_BONES];
    rt_humanoid(bones, 0.0f);
    de_ragdoll r[2];
    for (u32 k = 0; k < 2u; ++k) {
        const de_ragdoll_desc d = rt_desc(bones, st_xform(0.6 * k, 0, 0.5, st_quat_identity()));
        r[k] = de_ragdoll_create(p, &d);
        ASSERT_TRUE(DE_HANDLE_VALID(r[k]));
    }
    const de_body arm0 = de_ragdoll_bone_body(p, r[0], RT_UARM_R);
    const de_body arm1 = de_ragdoll_bone_body(p, r[1], RT_FARM_R);
    de_physics_step(p, ST_DT);
    /* The arm pair's contact is the true 1.5 cm gap, not -9.5 cm. */
    const de__manifold *m = NULL;
    u32 mc = 0;
    de__contacts_manifolds(p, &m, &mc);
    const u32 i0 = de__handle_index(arm0.id), i1 = de__handle_index(arm1.id);
    u32 arm_points = 0;
    for (u32 i = 0; i < mc; ++i) {
        if (!((m[i].key.a == i0 && m[i].key.b == i1) || (m[i].key.a == i1 && m[i].key.b == i0)))
            continue;
        for (u32 k = 0; k < m[i].point_count; ++k) {
            ST_GT(m[i].points[k].separation, 0.014f);
            ++arm_points;
        }
    }
    EXPECT_EQ(arm_points, 1u);
    bool finite = true;
    for (u32 tick = 1; tick < 120u; ++tick) {
        de_physics_step(p, ST_DT);
        for (u32 k = 0; k < 2u; ++k)
            for (u32 i = 0; i < RT_BONES; ++i) {
                const de_body b = de_ragdoll_bone_body(p, r[k], i);
                const de_vec3 v = st_vel(p, b), w = st_angvel(p, b);
                finite = finite && v.x - v.x == 0.0f && v.y - v.y == 0.0f && v.z - v.z == 0.0f &&
                         w.x - w.x == 0.0f && w.y - w.y == 0.0f && w.z - w.z == 0.0f;
            }
    }
    EXPECT_TRUE(finite);
    ST_LT(rd_max_speed(p, r[0]), 0.5f);
    ST_LT(rd_max_speed(p, r[1]), 0.5f);
    de_physics_destroy(p);
}

/* ================================ Powered =============================================== */

/* Largest angle between a bone's rotation relative to its parent and target[i]. */
static f32 rd_pose_error(const de_physics_world *p, de_ragdoll r, const de_ragdoll_bone *bones,
                         const de_quat *target, u32 *worst) {
    de_transform pose[RT_BONES];
    if (!de_ragdoll_get_pose(p, r, pose, RT_BONES))
        return 1.0e9f;
    f32 m = 0.0f;
    for (u32 i = 1; i < RT_BONES; ++i) {
        const f32 e = rd_angle(rd_local(pose, bones, i), target[i]);
        if (e > m) {
            m = e;
            if (worst != NULL)
                *worst = i;
        }
    }
    return m;
}

TEST(physics_ragdoll, powered_holds_t_pose) {
    de_physics_world *p = st_world(64, NULL, NULL);
    ASSERT_TRUE(p != NULL);
    de_ragdoll_bone powered[RT_BONES], limp[RT_BONES];
    rt_humanoid(powered, 400.0f);
    rt_humanoid(limp, 0.0f);
    de_ragdoll r[2];
    for (u32 k = 0; k < 2u; ++k) {
        const de_ragdoll_desc d =
            rt_desc(k == 0u ? powered : limp, st_xform(4.0 * k, 0, 2.0, st_quat_identity()));
        r[k] = de_ragdoll_create(p, &d);
        ASSERT_TRUE(DE_HANDLE_VALID(r[k]));
        /* The pelvis is held in place by a fixed joint to the world (no ground). */
        de_joint_desc fj;
        memset(&fj, 0, sizeof fj);
        fj.kind = DE_JOINT_FIXED;
        fj.body_b = de_ragdoll_bone_body(p, r[k], RT_PELVIS);
        fj.frame_a = st_xform(4.0 * k, 0, 3.0, st_quat_identity());
        fj.frame_b = st_xform(0, 0, 0, st_quat_identity());
        ASSERT_TRUE(DE_HANDLE_VALID(de_joint_create(p, &fj)));
    }
    de_quat target[RT_BONES];
    for (u32 i = 0; i < RT_BONES; ++i)
        target[i] = st_quat_identity(); /* every bind rotation is the identity */
    for (u32 tick = 0; tick < 180u; ++tick)
        de_physics_step(p, ST_DT);
    u32 worst = 0;
    const f32 held = rd_pose_error(p, r[0], powered, target, &worst);
    const f32 sagged = rd_pose_error(p, r[1], limp, target, NULL);
    fprintf(stderr, "  T-pose after 3 s: powered %.2f deg (bone %u), limp %.1f deg\n",
            (double)(held / RD_DEG), worst, (double)(sagged / RD_DEG));
    ST_LT(held, 3.0f * RD_DEG);
    ST_GT(sagged, 30.0f * RD_DEG);

    /* New targets: elbows at 90 degrees, arms lowered 40 degrees, head turned. */
    target[RT_FARM_L] = st_quat_axis_angle((de_vec3){0, 0, -1}, 0.5f * RT_PI);
    target[RT_FARM_R] = st_quat_axis_angle((de_vec3){0, 0, 1}, 0.5f * RT_PI);
    target[RT_UARM_L] = st_quat_axis_angle((de_vec3){0, 1, 0}, 0.7f);
    target[RT_UARM_R] = st_quat_axis_angle((de_vec3){0, 1, 0}, -0.7f);
    target[RT_HEAD] = st_quat_axis_angle((de_vec3){0, 0, 1}, 0.4f);
    EXPECT_FALSE(de_ragdoll_set_motor_targets(p, r[0], target, RT_BONES - 1u));
    ASSERT_TRUE(de_ragdoll_set_motor_targets(p, r[0], target, RT_BONES));
    for (u32 tick = 0; tick < 120u; ++tick)
        de_physics_step(p, ST_DT);
    const f32 moved = rd_pose_error(p, r[0], powered, target, &worst);
    fprintf(stderr, "  new targets after 2 s: %.2f deg (bone %u)\n", (double)(moved / RD_DEG),
            worst);
    ST_LT(moved, 3.0f * RD_DEG);
    de_physics_destroy(p);
}

/* ================================ Memory ================================================ */

TEST(physics_ragdoll, no_alloc_per_step) {
    de_tracking_allocator t;
    de_allocator al = de_tracking_allocator_init(&t, NULL, DE_MEM_PHYSICS);
    de_physics_world *p = rd_world(64, &al);
    ASSERT_TRUE(p != NULL);
    de_ragdoll_bone bones[RT_BONES];
    rt_humanoid(bones, 200.0f);
    const de_ragdoll_desc d = rt_desc(bones, st_xform(0, 0, 0.5, st_quat_identity()));
    const de_ragdoll r = de_ragdoll_create(p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(r));
    de_physics_step(p, ST_DT); /* warm-up */
    const de_tracking_stats before = de_tracking_allocator_stats(&t);
    de_transform pose[RT_BONES];
    de_quat target[RT_BONES];
    for (u32 i = 0; i < RT_BONES; ++i)
        target[i] = st_quat_axis_angle((de_vec3){0, 1, 0}, 0.1f);
    for (u32 tick = 0; tick < 60u; ++tick) {
        if (tick == 20u)
            EXPECT_TRUE(de_ragdoll_set_motor_targets(p, r, target, RT_BONES));
        EXPECT_TRUE(de_ragdoll_get_pose(p, r, pose, RT_BONES));
        if (tick == 40u)
            EXPECT_TRUE(de_ragdoll_set_pose(p, r, pose, RT_BONES));
        de_physics_step(p, ST_DT);
    }
    const de_tracking_stats after = de_tracking_allocator_stats(&t);
    EXPECT_EQ(after.alloc_count, before.alloc_count);
    EXPECT_EQ(after.free_count, before.free_count);
    de_ragdoll_destroy(p, r);
    de_physics_destroy(p);
    EXPECT_EQ(de_tracking_allocator_stats(&t).bytes_live, 0u);
}
