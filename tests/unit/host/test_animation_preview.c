#include "de_test.h"
#include "de_alloc_guard.h"
#include "host/editor/editor_animation_pose.h"
#include "../anim/anim_test_assets.h"
#include <math.h>

static void parse(anim_blob *skel, anim_blob *clip, de_cooked_skeleton_view *s,
                  de_cooked_clip_view *c) {
    *skel = skeleton_blob(3);
    *clip = clip_blob(3);
    ASSERT_TRUE(de_cooked_skeleton_parse(skel->data, skel->bytes, s, NULL));
    ASSERT_TRUE(de_cooked_clip_parse(clip->data, clip->bytes, c, NULL));
}
TEST(animation_preview, actual_cooked_motion_hierarchy_rest_and_version_elision) {
    anim_blob sb, cb;
    de_cooked_skeleton_view s;
    de_cooked_clip_view c;
    parse(&sb, &cb, &s, &c);
    u8 memory[16384];
    de_ep_animation_preview_params q = {.time_seconds = .5f};
    de_ep_arena a = {memory, sizeof memory, 0};
    de_ep_animation_preview r;
    ASSERT_EQ(de__ed_animation_sample(&c, &s, &q, 4, 5, de_ep_cstr("rig#skin"), &a, &r), DE_OK);
    ASSERT_EQ(r.pose.count, 3u);
    ASSERT_TRUE(r.has_metadata);
    EXPECT_EQ(r.metadata.joints.items[0].has_parent, false);
    EXPECT_EQ(r.metadata.joints.items[2].parent, 1u);
    EXPECT_TRUE(de_ep_str_eq(r.metadata.joints.items[2].name, "Joint 2"));
    EXPECT_FLOAT_EQ(r.metadata.rest_pose.items[2].matrix.items[10], 2, .0001);
    /* Actual compressed rotation and scale on the root transform descendants too. */
    EXPECT_FLOAT_EQ(r.pose.items[0].matrix.items[9], 1, .0001);
    EXPECT_FLOAT_EQ(r.pose.items[2].matrix.items[9], 1 - 3 * .70710678, .001);
    EXPECT_FLOAT_EQ(r.pose.items[2].matrix.items[10], 3 * .70710678, .001);
    size_t initial_bytes = a.used;
    q.has_clip_version = q.has_skeleton_version = true;
    q.clip_version = 4;
    q.skeleton_version = 5;
    q.time_seconds = 99;
    a.used = 0;
    ASSERT_EQ(de__ed_animation_sample(&c, &s, &q, 4, 5, de_ep_cstr("rig#skin"), &a, &r), DE_OK);
    EXPECT_FALSE(r.has_metadata);
    EXPECT_TRUE(a.used < initial_bytes);
    EXPECT_FLOAT_EQ(r.time_seconds, 1, .0001);
    EXPECT_FLOAT_EQ(r.pose.items[0].matrix.items[9], 2, .0001);
    a.used = 0;
    q.time_seconds = -1;
    ASSERT_EQ(de__ed_animation_sample(&c, &s, &q, 4, 6, de_ep_cstr("rig#skin"), &a, &r), DE_OK);
    EXPECT_TRUE(r.has_metadata);
    EXPECT_FLOAT_EQ(r.time_seconds, 0, .0001);
    c.duration = 0;
    a.used = 0;
    ASSERT_EQ(de__ed_animation_sample(&c, &s, &q, 4, 6, de_ep_cstr("rig#skin"), &a, &r), DE_OK);
    EXPECT_FLOAT_EQ(r.duration_seconds, 0, .0001);
    free(sb.data);
    free(cb.data);
}
TEST(animation_preview, invalid_data_and_short_arena_are_atomic) {
    anim_blob sb, cb;
    de_cooked_skeleton_view s;
    de_cooked_clip_view c;
    parse(&sb, &cb, &s, &c);
    u8 memory[4096];
    memset(memory, 0xcd, sizeof memory);
    de_ep_arena a = {memory, 31, 0};
    de_ep_animation_preview r, sentinel;
    memset(&sentinel, 0xa4, sizeof sentinel);
    r = sentinel;
    de_ep_animation_preview_params q = {0};
    EXPECT_EQ(de__ed_animation_sample(&c, &s, &q, 1, 1, de_ep_cstr("s"), &a, &r),
              DE_ERR_OUT_OF_MEMORY);
    EXPECT_EQ(a.used, 0u);
    ASSERT_MEMEQ(&r, &sentinel, sizeof r);
    for (u32 i = 31; i < sizeof memory; ++i)
        EXPECT_EQ(memory[i], 0xcdu);
    a.size = sizeof memory;
    q.time_seconds = NAN;
    EXPECT_EQ(de__ed_animation_sample(&c, &s, &q, 1, 1, de_ep_cstr("s"), &a, &r),
              DE_ERR_INVALID_ARGUMENT);
    q.time_seconds = 0;
    q.has_clip_version = true;
    EXPECT_EQ(de__ed_animation_sample(&c, &s, &q, 1, 1, de_ep_cstr("s"), &a, &r),
              DE_ERR_INVALID_ARGUMENT);
    q.has_clip_version = false;
    s.joint_count = 4097;
    EXPECT_EQ(de__ed_animation_sample(&c, &s, &q, 1, 1, de_ep_cstr("s"), &a, &r),
              DE_ERR_UNSUPPORTED);
    s.joint_count = 2;
    EXPECT_EQ(de__ed_animation_sample(&c, &s, &q, 1, 1, de_ep_cstr("s"), &a, &r), DE_ERR_PARSE);
    s.joint_count = 3;
    de_cooked_put_f32((u8 *)s.rest, NAN);
    EXPECT_EQ(de__ed_animation_sample(&c, &s, &q, 1, 1, de_ep_cstr("s"), &a, &r), DE_ERR_PARSE);
    EXPECT_EQ(a.used, 0u);
    ASSERT_MEMEQ(&r, &sentinel, sizeof r);
    free(sb.data);
    free(cb.data);
}
TEST(animation_preview, repeated_sampling_uses_only_bounded_request_storage) {
    anim_blob sb, cb;
    de_cooked_skeleton_view s;
    de_cooked_clip_view c;
    parse(&sb, &cb, &s, &c);
    u8 memory[16384];
    de_ep_animation_preview_params q = {0};
    de_ep_animation_preview r;
    de_alloc_guard guard = {0};
    de_alloc_guard_begin(&guard);
    bool ok = true;
    for (u32 i = 0; i < 1000; ++i) {
        de_ep_arena a = {memory, sizeof memory, 0};
        q.time_seconds = (f32)(i % 100) / 100;
        ok &= de__ed_animation_sample(&c, &s, &q, 1, 2, de_ep_cstr("s"), &a, &r) == DE_OK;
    }
    de_alloc_guard_end(&guard);
    EXPECT_TRUE(ok);
    EXPECT_EQ(de_alloc_guard_offences(&guard), 0u);
    free(sb.data);
    free(cb.data);
}
