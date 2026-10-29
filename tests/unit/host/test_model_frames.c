#include "de_test.h"
#include "host/editor/editor_model_frames.h"
#include "render/formats/de_cooked_model.h"
#include <math.h>
#include <string.h>

TEST(model_frames, selects_unique_cooked_skin_and_preserves_failed_output) {
    de_asset_id skeletons[] = {{11}, {12}};
    de_mesh meshes[] = {{1}};
    de_render_model_node nodes[] = {{.mesh = {1}, .skin = 1}, {.mesh = {1}, .skin = 1}};
    de_render_model model = {.node_count = 2,
                             .mesh_count = 1,
                             .nodes = nodes,
                             .meshes = meshes,
                             .skeleton_count = 2,
                             .skeleton_ids = skeletons};
    u32 skin = 99;
    ASSERT_EQ(de__ed_model_skin(&model, 0, &skin), DE_OK);
    EXPECT_EQ(skin, 1u);
    nodes[1].skin = 0;
    skin = 99;
    EXPECT_EQ(de__ed_model_skin(&model, 0, &skin), DE_ERR_UNSUPPORTED);
    EXPECT_EQ(skin, 99u);
    nodes[0].skin = nodes[1].skin = DE_COOKED_NONE;
    EXPECT_EQ(de__ed_model_skin(&model, 0, &skin), DE_ERR_NOT_FOUND);
    nodes[0].skin = 2;
    EXPECT_EQ(de__ed_model_skin(&model, 0, &skin), DE_ERR_PARSE);
    EXPECT_EQ(de__ed_model_skin(&model, 1, &skin), DE_ERR_INVALID_ARGUMENT);
}
TEST(model_frames, transformed_cooked_box_and_bounded_failure) {
    de_transform pose = de_transform_identity();
    pose.position = (de_dvec3){1e9, 3, 4};
    pose.rotation = de_quat_axis_angle((de_vec3){0, 0, 1}, DE_PI * .5f);
    pose.scale = (de_vec3){2, 3, 1};
    const f32 min[] = {-1, -1, 0}, max[] = {1, 2, 3};
    de__ed_frame_line lines[12];
    u32 count = 0;
    ASSERT_EQ(de__ed_model_box(&pose, min, max, lines, 12, &count), DE_OK);
    EXPECT_EQ(count, 12u);
    EXPECT_TRUE(fabs(lines[0].a.x - (1e9 + 3)) < 1e-5);
    EXPECT_TRUE(fabs(lines[0].a.y - 1) < 1e-5);
    EXPECT_TRUE(fabs(lines[0].b.y - 5) < 1e-5);
    EXPECT_EQ(de__ed_model_box(&pose, min, max, lines, 12, &count), DE_ERR_UNSUPPORTED);
    EXPECT_EQ(count, 12u);
    count = 0;
    pose.position.x = NAN;
    EXPECT_EQ(de__ed_model_box(&pose, min, max, lines, 12, &count), DE_ERR_PARSE);
    EXPECT_EQ(count, 0u);
}
TEST(model_frames, bind_points_come_from_inverse_bind_not_rest_pose) {
    const u8 parents[] = {255, 255, 0, 0, 0, 0};
    u8 matrices[3 * 48];
    for (u32 i = 0; i < 3; ++i) {
        de_affine matrix = de_affine_identity();
        matrix.m[9] = i == 1 ? -1 : 0;
        matrix.m[10] = i == 2 ? -2 : 0;
        for (u32 k = 0; k < 12; ++k) {
            u32 bits;
            memcpy(&bits, &matrix.m[k], 4);
            de_cooked_put_u32(matrices + i * 48 + k * 4, bits);
        }
    }
    de_cooked_skeleton_view skeleton = {
        .joint_count = 3, .parents = parents, .inverse_bind = matrices};
    de_transform pose = de_transform_identity();
    pose.position = (de_dvec3){2, 3, 4};
    pose.rotation = de_quat_axis_angle((de_vec3){0, 0, 1}, DE_PI * .5f);
    pose.scale = (de_vec3){2, 1, 1};
    de_dvec3 joints[3];
    de__ed_frame_line lines[8];
    u32 count = 0;
    ASSERT_EQ(de__ed_model_bind(&skeleton, &pose, joints, 3, lines, 8, &count), DE_OK);
    EXPECT_EQ(count, 5u); // three joint markers, two actual parent links
    EXPECT_TRUE(fabs(joints[1].x - 2) < 1e-5 && fabs(joints[1].y - 5) < 1e-5);
    EXPECT_TRUE(fabs(joints[2].x) < 1e-5 && fabs(joints[2].y - 3) < 1e-5);
    count = 0;
    memset(matrices + 48, 0, 48);
    EXPECT_EQ(de__ed_model_bind(&skeleton, &pose, joints, 3, lines, 8, &count), DE_ERR_PARSE);
    EXPECT_EQ(count, 0u);
    EXPECT_EQ(de__ed_model_bind(&skeleton, &pose, joints, 2, lines, 8, &count), DE_ERR_UNSUPPORTED);
}
TEST(model_frames, clips_near_viewport_and_large_origin_without_out_of_bounds) {
    u8 storage[16 + 64 * 64 * 4 + 16];
    memset(storage, 0xa5, sizeof storage);
    u8 *rgba = storage + 16;
    memset(rgba, 0, 64 * 64 * 4);
    de_view_frame_info frame = {.render_origin = {1e9, 0, 0}, .view_projection = de_m4_identity()};
    de__ed_frame_line lines[] = {
        {.a = {1e9 - 2, 0, .5}, .b = {1e9 + 2, 0, .5}, .rgba = {255, 208, 64, 255}},
        {.a = {1e9, -2, -1}, .b = {1e9, 2, -1}, .rgba = {255, 0, 0, 255}},
        {.a = {1e9, 0, -2}, .b = {1e9, .5, .5}, .rgba = {64, 220, 255, 255}},
        {.a = {NAN, 0, 0}, .b = {1e9, 0, 0}, .rgba = {255, 0, 0, 255}}};
    de__ed_model_frame_draw(lines, 4, &frame, 64, rgba);
    u32 yellow = 0, cyan = 0, red = 0;
    for (u32 i = 0; i < 64 * 64; ++i) {
        yellow += rgba[4 * i] == 255 && rgba[4 * i + 1] == 208;
        cyan += rgba[4 * i] == 64 && rgba[4 * i + 1] == 220;
        red += rgba[4 * i] == 255 && rgba[4 * i + 1] == 0;
    }
    EXPECT_GT(yellow, 50u);
    EXPECT_GT(cyan, 3u);
    EXPECT_EQ(red, 0u);
    for (u32 i = 0; i < 16; ++i) {
        EXPECT_EQ(storage[i], 0xa5u);
        EXPECT_EQ(storage[sizeof storage - 1 - i], 0xa5u);
    }
}
