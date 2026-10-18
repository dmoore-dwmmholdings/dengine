#include "ecs2d_test_util.h"
TEST(physics2d_plane, side_roundtrip_rotation_and_depth) {
    de_transform t = de_transform_identity();
    t.position = (de_dvec3){123456.125, 7, -23.5};
    de_transform2d p = {t.position.x, t.position.z, {0, 1}};
    de_transform mapped = de__plane2d_to_world(DE_PHYSICS2D_PLANE_SIDE, p, t);
    EXPECT_FLOAT_EQ(mapped.rotation.y, -de_dsqrt(0.5f), 1e-6);
    EXPECT_FLOAT_EQ(mapped.rotation.x, 0, 1e-7);
    EXPECT_TRUE(mapped.position.y == 7);
    de_transform2d back = de__plane2d_from_world(DE_PHYSICS2D_PLANE_SIDE, &mapped);
    EXPECT_TRUE(back.x == p.x && back.y == p.y);
    EXPECT_FLOAT_EQ(back.rotation.s, 1, 1e-6);
}
TEST(physics2d_plane, top_roundtrip_and_axis) {
    de_transform t = de_transform_identity();
    t.position = (de_dvec3){12.25, -98.5, 44};
    de_transform2d p = {t.position.x, t.position.y, {0, 1}};
    de_transform mapped = de__plane2d_to_world(DE_PHYSICS2D_PLANE_TOP_DOWN, p, t);
    EXPECT_FLOAT_EQ(mapped.rotation.z, de_dsqrt(0.5f), 1e-6);
    EXPECT_TRUE(mapped.position.z == 44);
    de_transform2d back = de__plane2d_from_world(DE_PHYSICS2D_PLANE_TOP_DOWN, &mapped);
    EXPECT_TRUE(back.x == p.x && back.y == p.y);
}
TEST(physics2d_plane, gravity_defaults) {
    e2_env side = e2_create(DE_PHYSICS2D_PLANE_SIDE, NULL, NULL);
    EXPECT_FLOAT_EQ(side.p->gravity.y, -9.81, 1e-5);
    e2_destroy(&side);
    e2_env top = e2_create(DE_PHYSICS2D_PLANE_TOP_DOWN, NULL, NULL);
    EXPECT_FLOAT_EQ(top.p->gravity.y, 0, 0);
    e2_destroy(&top);
}
