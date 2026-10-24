#include "de_test.h"
#include "de_alloc_guard.h"
#include "anim/blend.h"
#include "anim/anim_math.h"
#include <math.h>
#include <string.h>

typedef struct blend_pose {
    de_quat r[4];
    de_vec3 t[4], s[4];
    de__anim_local_pose pose;
} blend_pose;
static void init_pose(blend_pose *p) {
    memset(p, 0, sizeof *p);
    for (u32 j = 0; j < 4; ++j) {
        p->r[j] = (de_quat){0, 0, 0, 1};
        p->s[j] = (de_vec3){1, 1, 1};
    }
    p->pose = (de__anim_local_pose){.count = 4,
                                    .rotations = p->r,
                                    .translations = p->t,
                                    .scales = p->s,
                                    .root_rotation = {0, 0, 0, 1}};
}
static void same_bone(const blend_pose *a, const blend_pose *b, u32 j) {
    EXPECT_MEMEQ(&a->r[j], &b->r[j], sizeof a->r[j]);
    EXPECT_MEMEQ(&a->t[j], &b->t[j], sizeof a->t[j]);
    EXPECT_MEMEQ(&a->s[j], &b->s[j], sizeof a->s[j]);
}
TEST(anim_blend, zero_weight_and_mask_preserve_base_bits_and_root_slot) {
    de_fp_env_init_thread();
    for (u32 mode = 0; mode < 2; ++mode) {
        blend_pose base, layer, out;
        init_pose(&base);
        init_pose(&layer);
        init_pose(&out);
        base.t[0].x = -0.f;
        base.r[0].x = -0.f;
        base.pose.root_translation.x = -0.f;
        layer.t[0] = (de_vec3){4, 5, 6};
        layer.r[0] = (de_quat){0, 0, 1, 0};
        layer.pose.root_translation = (de_vec3){7, 8, 9};
        for (u32 additive = 0; additive < 2; ++additive) {
            ASSERT_TRUE(de__anim_pose_layer(&out.pose, &base.pose, &layer.pose, 0, NULL, 1,
                                            additive != 0, mode != 0));
            for (u32 j = 0; j < 4; ++j)
                same_bone(&out, &base, j);
            EXPECT_MEMEQ(&out.pose.root_translation, &base.pose.root_translation, sizeof(de_vec3));
            EXPECT_MEMEQ(&out.pose.root_rotation, &base.pose.root_rotation, sizeof(de_quat));
            f32 mask[] = {0, 1, 1, 1};
            ASSERT_TRUE(de__anim_pose_layer(&out.pose, &base.pose, &layer.pose, 1, mask, 0,
                                            additive != 0, mode != 0));
            same_bone(&out, &base, 0);
            EXPECT_MEMEQ(&out.pose.root_translation, &base.pose.root_translation, sizeof(de_vec3));
        }
    }
}
TEST(anim_blend, additive_reference_rotation_order_fractional_weight_and_aliasing) {
    de_fp_env_init_thread();
    for (u32 mode = 0; mode < 2; ++mode) {
        blend_pose reference, sample, delta, base, out;
        init_pose(&reference);
        init_pose(&sample);
        init_pose(&delta);
        init_pose(&base);
        init_pose(&out);
        /* Reference Rx90, sample Rx90*Rz90, base Ry90. Expected base*delta
         * is Ry90*Rz90 = (.5,.5,.5,.5); reversing multiplication flips x. */
        const f32 h = .7071067811865476f;
        reference.r[1] = (de_quat){h, 0, 0, h};
        sample.r[1] = (de_quat){.5f, -.5f, .5f, .5f};
        base.r[1] = (de_quat){0, h, 0, h};
        reference.t[1] = (de_vec3){1, 2, 3};
        sample.t[1] = (de_vec3){5, 8, 11};
        sample.s[1] = (de_vec3){3, 5, 7};
        base.t[1] = (de_vec3){10, 20, 30};
        reference.pose.root_translation = (de_vec3){1, 0, 0};
        sample.pose.root_translation = (de_vec3){5, 0, 0};
        base.pose.root_translation = (de_vec3){10, 0, 0};
        ASSERT_TRUE(de__anim_pose_delta(&delta.pose, &sample.pose, &reference.pose, mode != 0));
        f32 mask[] = {0, 1, 0, 0};
        ASSERT_TRUE(
            de__anim_pose_layer(&out.pose, &base.pose, &delta.pose, 1, mask, .5f, true, mode != 0));
        EXPECT_TRUE(fabsf(out.r[1].x - .5f) < 1e-6f);
        EXPECT_TRUE(fabsf(out.r[1].y - .5f) < 1e-6f);
        EXPECT_TRUE(fabsf(out.r[1].z - .5f) < 1e-6f);
        EXPECT_TRUE(fabsf(out.r[1].w - .5f) < 1e-6f);
        EXPECT_TRUE(out.t[1].x == 14 && out.t[1].y == 26 && out.t[1].z == 38);
        EXPECT_TRUE(out.s[1].x == 3 && out.s[1].y == 5 && out.s[1].z == 7);
        EXPECT_TRUE(out.pose.root_translation.x == 12);
        for (u32 j = 0; j < 4; ++j)
            if (j != 1)
                same_bone(&out, &base, j);
        /* In-place delta construction and output==delta are both supported. */
        ASSERT_TRUE(de__anim_pose_delta(&sample.pose, &sample.pose, &reference.pose, mode != 0));
        EXPECT_MEMEQ(sample.r, delta.r, sizeof sample.r);
        ASSERT_TRUE(de__anim_pose_layer(&sample.pose, &base.pose, &sample.pose, 1, mask, .5f, true,
                                        mode != 0));
        EXPECT_MEMEQ(sample.r, out.r, sizeof sample.r);
        EXPECT_MEMEQ(sample.t, out.t, sizeof sample.t);
        ASSERT_TRUE(de__anim_pose_layer(&base.pose, &base.pose, &delta.pose, .5f, mask, 1, true,
                                        mode != 0));
        EXPECT_TRUE(base.t[1].x == 12 && base.s[1].z == 4);
        /* Ry90*Rz45 has x/z = sin45*sin22.5 and y/w = sin45*cos22.5. */
        EXPECT_TRUE(fabsf(base.r[1].x - .2705980501f) < 1e-6f);
        EXPECT_TRUE(fabsf(base.r[1].y - .6532814824f) < 1e-6f);
        EXPECT_TRUE(fabsf(base.r[1].z - .2705980501f) < 1e-6f);
        EXPECT_TRUE(fabsf(base.r[1].w - .6532814824f) < 1e-6f);
    }
}
TEST(anim_blend, override_combines_mask_weight_and_preserves_exact_full_endpoint) {
    de_fp_env_init_thread();
    for (u32 mode = 0; mode < 2; ++mode) {
        blend_pose base, layer, out;
        init_pose(&base);
        init_pose(&layer);
        init_pose(&out);
        base.t[2] = (de_vec3){2, 2, 2};
        base.s[2] = (de_vec3){2, 2, 2};
        layer.t[2] = (de_vec3){10, 10, 10};
        layer.s[2] = (de_vec3){6, 6, 6};
        layer.r[2] = (de_quat){0, 0, 1, 0};
        base.pose.root_translation.x = 2;
        layer.pose.root_translation.x = 10;
        layer.pose.root_rotation = layer.r[2];
        f32 mask[] = {0, 0, .5f, 0};
        ASSERT_TRUE(de__anim_pose_layer(&out.pose, &base.pose, &layer.pose, .5f, mask, .5f, false,
                                        mode != 0));
        EXPECT_TRUE(out.t[2].x == 4 && out.s[2].x == 3);
        EXPECT_TRUE(fabsf(out.r[2].z - .3162277660f) < 1e-6f);
        EXPECT_TRUE(fabsf(out.r[2].w - .9486832981f) < 1e-6f);
        EXPECT_TRUE(out.pose.root_translation.x == 4);
        EXPECT_TRUE(fabsf(out.pose.root_rotation.z - .3162277660f) < 1e-6f);
        for (u32 j = 0; j < 4; ++j)
            if (j != 2)
                same_bone(&out, &base, j);
        layer.t[2].x = -0.f;
        ASSERT_TRUE(
            de__anim_pose_layer(&base.pose, &base.pose, &layer.pose, 1, NULL, 1, false, mode != 0));
        same_bone(&base, &layer, 2);
        EXPECT_MEMEQ(&base.pose.root_rotation, &layer.pose.root_rotation, sizeof(de_quat));
        ASSERT_TRUE(
            de__anim_pose_layer(&layer.pose, &out.pose, &layer.pose, 1, NULL, 1, false, mode != 0));
        same_bone(&base, &layer, 2);
    }
}
TEST(anim_blend, named_masks_include_descendants_but_exclude_other_branches) {
    u8 parents[10] = {255, 255, 0, 0, 1, 0, 0, 0, 3, 0}, names[40];
    const char *labels[] = {"root", "spine_01", "head", "leg", "foot"};
    for (u32 j = 0; j < 5; ++j) {
        u64 hash = de_name_from(labels[j]).hash;
        for (u32 k = 0; k < 8; ++k)
            names[j * 8 + k] = (u8)(hash >> (k * 8));
    }
    de_cooked_skeleton_view skeleton = {.joint_count = 5, .parents = parents, .name_hashes = names};
    de__anim_bone_mask rules[] = {{de_name_from("spine_01"), .75f, true},
                                  {de_name_from("foot"), .25f, false}};
    f32 mask[5];
    ASSERT_TRUE(de__anim_mask_resolve(&skeleton, rules, 2, mask));
    const f32 expected[] = {0, .75f, .75f, 0, .25f};
    EXPECT_MEMEQ(mask, expected, sizeof mask);
    rules[0].include_children = false;
    ASSERT_TRUE(de__anim_mask_resolve(&skeleton, rules, 1, mask));
    EXPECT_TRUE(mask[1] == .75f && mask[2] == 0 && mask[4] == 0);
    rules[0].bone = de_name_from("missing");
    f32 before[5];
    memcpy(before, mask, sizeof mask);
    EXPECT_FALSE(de__anim_mask_resolve(&skeleton, rules, 1, mask));
    EXPECT_MEMEQ(before, mask, sizeof mask);
    ASSERT_TRUE(de__anim_mask_resolve(&skeleton, NULL, 0, mask));
    for (u32 j = 0; j < 5; ++j)
        EXPECT_TRUE(mask[j] == 1);
}
TEST(anim_blend, invalid_arguments_write_nothing_and_warmed_layers_allocate_nothing) {
    blend_pose base, delta, out;
    init_pose(&base);
    init_pose(&delta);
    init_pose(&out);
    f32 mask[] = {1, 1, 1, NAN};
    blend_pose before = out;
    EXPECT_FALSE(de__anim_pose_layer(&out.pose, &base.pose, &delta.pose, 1, mask, 1, true, true));
    EXPECT_MEMEQ(out.r, before.r, sizeof out.r);
    EXPECT_MEMEQ(out.t, before.t, sizeof out.t);
    EXPECT_FALSE(de__anim_pose_layer(&out.pose, &base.pose, &delta.pose, NAN, NULL, 1, true, true));
    EXPECT_FALSE(de__anim_pose_layer(&out.pose, &base.pose, &delta.pose, 1, NULL, -1, true, true));
    delta.pose.count = 3;
    EXPECT_FALSE(de__anim_pose_delta(&out.pose, &base.pose, &delta.pose, true));
    EXPECT_FALSE(de__anim_pose_layer(&out.pose, &base.pose, &delta.pose, 1, NULL, 1, true, true));
    EXPECT_MEMEQ(out.s, before.s, sizeof out.s);
    delta.pose.count = 4;
    de_fp_env_init_thread();
    for (u32 mode = 0; mode < 2; ++mode)
        ASSERT_TRUE(
            de__anim_pose_layer(&out.pose, &base.pose, &delta.pose, .5f, NULL, 1, true, mode != 0));
    de_alloc_guard guard = {0};
    de_alloc_guard_begin(&guard);
    for (u32 i = 0; i < 2000; ++i)
        ASSERT_TRUE(de__anim_pose_layer(&out.pose, &base.pose, &delta.pose, .5f, NULL, 1, true,
                                        i % 2 != 0));
    de_alloc_guard_end(&guard);
    DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
}
