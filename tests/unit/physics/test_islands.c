/* test_islands.c: islands of the 3D solver (DENG-218, SPEC §13.1, §13.2): union-find over the
 * constraint records, island id = lowest body index, bodies listed in ascending index, static and
 * kinematic bodies never link islands, joints (a fake constraint class through constraint.h) do,
 * and the result does not depend on the order in which links are made. */
#include "island_test_util.h"

/* The islands of the last step, as (id, sorted body list) for checks. */
static const de__islands *islands_of(de_physics_world *p) {
    const de__islands *is = de__islands_get(p);
    ASSERT_TRUE(is != NULL);
    return is;
}

static const de__island *island_with(const de__islands *is, u32 id) {
    for (u32 k = 0; k < is->island_count; ++k)
        if (is->islands[k].id == id)
            return &is->islands[k];
    return NULL;
}

static void expect_members(const de__islands *is, const de__island *I, const u32 *bodies, u32 n) {
    ASSERT_TRUE(I != NULL);
    ASSERT_EQ(I->body_count, n);
    for (u32 k = 0; k < n; ++k)
        EXPECT_EQ(is->island_bodies[I->first + k], bodies[k]);
}

TEST(physics_islands, stacks_static_ground_and_free_bodies) {
    de_physics_world *p = st_world(64, NULL, NULL);
    ASSERT_TRUE(p != NULL);
    const de_shape g = st_box(p, 20.0f, 20.0f, 0.5f);
    const de_shape c = st_box(p, 0.5f, 0.5f, 0.5f);
    /* Index 0: free box in the air. 1: static ground. 2..4: stack at x=-5 (bottom first);
     * 5..7: stack at x=+5 created top first; 8: a box resting alone on the ground. */
    const de_body air =
        st_body(p, DE_BODY_DYNAMIC, c, st_xform(0, 0, 30, st_quat_identity()), 0, 0);
    (void)st_body(p, DE_BODY_STATIC, g, st_xform(0, 0, -0.5, st_quat_identity()), 0.5f, 0);
    for (u32 k = 0; k < 3; ++k)
        (void)st_body(p, DE_BODY_DYNAMIC, c, st_xform(-5, 0, 0.5 + 1.001 * k, st_quat_identity()),
                      0.5f, 0);
    for (u32 k = 0; k < 3; ++k)
        (void)st_body(p, DE_BODY_DYNAMIC, c,
                      st_xform(5, 0, 0.5 + 1.001 * (2 - k), st_quat_identity()), 0.5f, 0);
    (void)st_body(p, DE_BODY_DYNAMIC, c, st_xform(0, 8, 0.5, st_quat_identity()), 0.5f, 0);
    EXPECT_EQ(it_index(air), 0u);
    for (u32 t = 0; t < 3; ++t)
        de_physics_step(p, ST_DT);
    const de__islands *is = islands_of(p);
    /* The ground links nothing: four islands, numbered by their lowest body index. */
    ASSERT_EQ(is->island_count, 4u);
    EXPECT_EQ(is->islands[0].id, 0u);
    EXPECT_EQ(is->islands[1].id, 2u);
    EXPECT_EQ(is->islands[2].id, 5u);
    EXPECT_EQ(is->islands[3].id, 8u);
    const u32 a[] = {0}, s1[] = {2, 3, 4}, s2[] = {5, 6, 7}, lone[] = {8};
    expect_members(is, island_with(is, 0), a, 1);
    expect_members(is, island_with(is, 2), s1, 3);
    expect_members(is, island_with(is, 5), s2, 3);
    expect_members(is, island_with(is, 8), lone, 1);
    /* Constraints: each stack has 2 box-box and 1 ground contact; the lone box 1; the air 0. */
    EXPECT_EQ(island_with(is, 0)->constraint_count, 0u);
    EXPECT_EQ(island_with(is, 2)->constraint_count, 3u);
    EXPECT_EQ(island_with(is, 5)->constraint_count, 3u);
    EXPECT_EQ(island_with(is, 8)->constraint_count, 1u);
    de__islands_stats st;
    de__islands_stats_get(p, &st);
    EXPECT_EQ(st.islands, 4u);
    EXPECT_EQ(st.largest, 3u);
    de_physics_destroy(p);
}

TEST(physics_islands, kinematic_bodies_do_not_link) {
    de_physics_world *p = st_world(64, NULL, NULL);
    ASSERT_TRUE(p != NULL);
    const de_shape plate = st_box(p, 4.0f, 4.0f, 0.25f);
    const de_shape c = st_box(p, 0.5f, 0.5f, 0.5f);
    const de_body k =
        st_body(p, DE_BODY_KINEMATIC, plate, st_xform(0, 0, 0, st_quat_identity()), 0.5f, 0);
    (void)st_body(p, DE_BODY_DYNAMIC, c, st_xform(-2, 0, 0.751, st_quat_identity()), 0.5f, 0);
    (void)st_body(p, DE_BODY_DYNAMIC, c, st_xform(2, 0, 0.751, st_quat_identity()), 0.5f, 0);
    de_physics_step(p, ST_DT);
    const de__islands *is = islands_of(p);
    ASSERT_EQ(is->island_count, 2u);
    EXPECT_EQ(is->islands[0].id, 1u);
    EXPECT_EQ(is->islands[1].id, 2u);
    EXPECT_EQ(is->islands[0].flags & DE__ISLAND_KEEP_AWAKE, 0u); /* still plate */
    /* A moving kinematic body keeps both islands awake but still does not link them. */
    ASSERT_TRUE(de_body_set_velocity(p, k, (de_vec3){0.0f, 0.1f, 0.0f}, (de_vec3){0, 0, 0}));
    de_physics_step(p, ST_DT);
    ASSERT_EQ(is->island_count, 2u);
    EXPECT_NE(is->islands[0].flags & DE__ISLAND_KEEP_AWAKE, 0u);
    EXPECT_NE(is->islands[1].flags & DE__ISLAND_KEEP_AWAKE, 0u);
    de_physics_destroy(p);
}

/* Free bodies in zero gravity, linked only by fake joints. */
static de_physics_world *joint_world(it_joints *j, u32 n) {
    de_physics_world *p = st_world(64, NULL, NULL);
    if (p == NULL)
        return NULL;
    memset(j, 0, sizeof *j);
    it_joints_register(p, j);
    const de_shape c = st_sphere(p, 0.25f);
    for (u32 i = 0; i < n; ++i) {
        de_body_desc d;
        memset(&d, 0, sizeof d);
        d.type = DE_BODY_DYNAMIC;
        d.shape = c;
        d.transform = st_xform(3.0 * (f64)i, 0, 0, st_quat_identity());
        (void)de_body_create(p, &d);
    }
    return p;
}

TEST(physics_islands, joints_link_and_world_joints_do_not) {
    static it_joints j;
    de_physics_world *p = joint_world(&j, 8);
    ASSERT_TRUE(p != NULL);
    (void)it_joints_add(&j, 6, 3);
    (void)it_joints_add(&j, 3, 5);
    (void)it_joints_add(&j, 1, DE__CONSTRAINT_NO_BODY); /* to the world */
    (void)it_joints_add(&j, 7, 2);
    de_physics_step(p, ST_DT);
    const de__islands *is = islands_of(p);
    /* {0} {1} {2,7} {3,5,6} {4} */
    ASSERT_EQ(is->island_count, 5u);
    const u32 i0[] = {0}, i1[] = {1}, i2[] = {2, 7}, i3[] = {3, 5, 6}, i4[] = {4};
    expect_members(is, island_with(is, 0), i0, 1);
    expect_members(is, island_with(is, 1), i1, 1);
    expect_members(is, island_with(is, 2), i2, 2);
    expect_members(is, island_with(is, 3), i3, 3);
    expect_members(is, island_with(is, 4), i4, 1);
    EXPECT_EQ(island_with(is, 1)->constraint_count, 1u);
    EXPECT_EQ(island_with(is, 3)->constraint_count, 2u);
    for (u32 k = 0; k < j.count; ++k)
        EXPECT_GT(j.solves[k], 0u);
    de_physics_destroy(p);
}

/* The islands do not depend on the order of the links (record order) nor on which side is A. */
TEST(physics_islands, independent_of_link_order) {
    const u32 links[][2] = {{9, 4}, {4, 2}, {11, 12}, {0, 9}, {6, 5}, {12, 13}, {5, 7}};
    const u32 n = (u32)(sizeof links / sizeof links[0]);
    u32 ids[2][16] = {{0}}, members[2][16] = {{0}}, count[2] = {0, 0};
    for (u32 run = 0; run < 2; ++run) {
        static it_joints j;
        de_physics_world *p = joint_world(&j, 14);
        ASSERT_TRUE(p != NULL);
        for (u32 k = 0; k < n; ++k) {
            const u32 *l = links[run == 0 ? k : n - 1u - k];
            if (run == 0)
                (void)it_joints_add(&j, l[0], l[1]);
            else
                (void)it_joints_add(&j, l[1], l[0]);
        }
        de_physics_step(p, ST_DT);
        const de__islands *is = islands_of(p);
        count[run] = is->island_count;
        ASSERT_TRUE(count[run] <= 16u);
        for (u32 k = 0; k < is->island_count; ++k)
            ids[run][k] = is->islands[k].id;
        for (u32 k = 0; k < 14; ++k)
            members[run][k] = is->island_bodies[k];
        de_physics_destroy(p);
    }
    /* {0,2,4,9} {1} {3} {5,6,7} {8} {10} {11,12,13} */
    ASSERT_EQ(count[0], 7u);
    ASSERT_EQ(count[1], count[0]);
    const u32 want_ids[7] = {0, 1, 3, 5, 8, 10, 11};
    const u32 want_members[14] = {0, 2, 4, 9, 1, 3, 5, 6, 7, 8, 10, 11, 12, 13};
    for (u32 k = 0; k < 7; ++k) {
        EXPECT_EQ(ids[0][k], want_ids[k]);
        EXPECT_EQ(ids[1][k], want_ids[k]);
    }
    for (u32 k = 0; k < 14; ++k) {
        EXPECT_EQ(members[0][k], want_members[k]);
        EXPECT_EQ(members[1][k], want_members[k]);
    }
}
