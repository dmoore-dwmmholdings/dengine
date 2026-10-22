#include "anim_fixture.h"
#include "anim/anim_math.h"
#include "de_alloc_guard.h"

static u32 bits(f32 value) {
    u32 result;
    memcpy(&result, &value, sizeof result);
    return result;
}

TEST(anim, shared_math_known_vectors_and_hemisphere) {
    de_fp_env_init_thread();
    const de_quat identity = {0, 0, 0, 1}, half_turn = {0, 0, 1, 0};
    de_quat n = de__anim_normalize((de_quat){0, 0, 3, 4}, true);
    EXPECT_EQ(bits(n.z), 0x3f19999au); /* 3/5, correctly rounded */
    EXPECT_EQ(bits(n.w), 0x3f4ccccdu); /* 4/5 */
    de_quat q = de__anim_nlerp(identity, half_turn, .5f, true);
    EXPECT_EQ(bits(q.z), 0x3f3504f3u); /* sqrt(1/2), correctly rounded */
    EXPECT_EQ(bits(q.w), 0x3f3504f3u);
    de_quat spherical = de__anim_slerp(identity, half_turn, .5f, true);
    EXPECT_EQ(bits(spherical.z), 0x3f3504f3u);
    EXPECT_EQ(bits(spherical.w), 0x3f3504f3u);
    for (u32 mode = 0; mode < 2; ++mode) {
        de_vec3 v = de__anim_lerp((de_vec3){1, 2, 3}, (de_vec3){5, 6, 7}, .25f, mode != 0);
        EXPECT_TRUE(v.x == 2 && v.y == 3 && v.z == 4);
        de_quat zero = de__anim_normalize((de_quat){0}, mode != 0);
        EXPECT_TRUE(zero.x == 0 && zero.y == 0 && zero.z == 0 && zero.w == 1);
        de_quat antipodal = de__anim_nlerp(identity, (de_quat){0, 0, 0, -1}, .37f, mode != 0);
        EXPECT_TRUE(antipodal.x == 0 && antipodal.y == 0 && antipodal.z == 0 && antipodal.w == 1);
        de_quat fast = de__anim_slerp(identity, half_turn, .5f, mode != 0);
        EXPECT_TRUE(fabsf(fast.z - .7071067811865476f) < 1e-6f);
        EXPECT_TRUE(fabsf(fast.w - .7071067811865476f) < 1e-6f);
        de_quat start = de__anim_slerp(identity, half_turn, 0, mode != 0);
        de_quat end = de__anim_slerp(identity, half_turn, 1, mode != 0);
        EXPECT_TRUE(memcmp(&start, &identity, sizeof start) == 0);
        EXPECT_TRUE(memcmp(&end, &half_turn, sizeof end) == 0);
    }
}

TEST(anim, pose_blend_includes_root_delta_aliases_and_exact_endpoints) {
    de_fp_env_init_thread();
    for (u32 mode = 0; mode < 2; ++mode) {
        de_quat ar[2] = {{0, 0, 0, 1}, {0, 0, 0, 1}};
        de_quat br[2] = {{0, 0, 1, 0}, {0, 0, 0, -1}}, result_r[2];
        de_vec3 at[2] = {{-0.f, 0, 0}, {0, 0, 0}}, bt[2] = {{2, 4, 6}, {2, 4, 6}};
        de_vec3 as[2] = {{1, 1, 1}, {1, 1, 1}}, bs[2] = {{3, 3, 3}, {3, 3, 3}};
        de_vec3 result_t[2], result_s[2];
        de__anim_local_pose a = {2, ar, at, as, {10, 20, 30}, {0, 0, 0, 1}};
        de__anim_local_pose b = {2, br, bt, bs, {14, 24, 34}, {0, 0, 1, 0}};
        de__anim_local_pose out = {
            .count = 2, .rotations = result_r, .translations = result_t, .scales = result_s};
        ASSERT_TRUE(de__anim_pose_blend(&out, &a, &b, 0, mode != 0));
        EXPECT_TRUE(memcmp(at, result_t, sizeof at) == 0);
        ASSERT_TRUE(de__anim_pose_blend(&out, &a, &b, 1, mode != 0));
        EXPECT_TRUE(memcmp(br, result_r, sizeof br) == 0);
        de_alloc_guard guard = {0};
        de_alloc_guard_begin(&guard);
        for (u32 repeat = 0; repeat < 2000; ++repeat)
            ASSERT_TRUE(de__anim_pose_blend(&out, &a, &b, .5f, mode != 0));
        de_alloc_guard_end(&guard);
        DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
        EXPECT_TRUE(result_t[0].x == 1 && result_t[0].y == 2 && result_t[0].z == 3);
        EXPECT_TRUE(result_s[0].x == 2 && result_s[0].y == 2 && result_s[0].z == 2);
        EXPECT_TRUE(fabsf(result_r[0].z - .7071067811865476f) < 1e-6f);
        EXPECT_TRUE(result_r[1].w == 1);
        EXPECT_TRUE(out.root_translation.x == 12 && out.root_translation.y == 22);
        EXPECT_TRUE(fabsf(out.root_rotation.z - .7071067811865476f) < 1e-6f);
        de_quat saved_ar[2];
        de_vec3 saved_at[2], saved_as[2];
        memcpy(saved_ar, ar, sizeof ar);
        memcpy(saved_at, at, sizeof at);
        memcpy(saved_as, as, sizeof as);
        ASSERT_TRUE(de__anim_pose_blend(&a, &a, &b, .5f, mode != 0));
        EXPECT_TRUE(memcmp(ar, result_r, sizeof ar) == 0);
        EXPECT_TRUE(memcmp(at, result_t, sizeof at) == 0);
        EXPECT_TRUE(memcmp(&a.root_translation, &out.root_translation, sizeof(de_vec3)) == 0);
        memcpy(ar, saved_ar, sizeof ar);
        memcpy(at, saved_at, sizeof at);
        memcpy(as, saved_as, sizeof as);
        a.root_translation = (de_vec3){10, 20, 30};
        a.root_rotation = (de_quat){0, 0, 0, 1};
        ASSERT_TRUE(de__anim_pose_blend(&b, &a, &b, .5f, mode != 0));
        EXPECT_TRUE(memcmp(br, result_r, sizeof br) == 0);
        EXPECT_TRUE(memcmp(bt, result_t, sizeof bt) == 0);
        EXPECT_TRUE(memcmp(&b.root_rotation, &out.root_rotation, sizeof(de_quat)) == 0);
        b.count = 1;
        EXPECT_FALSE(de__anim_pose_blend(&out, &a, &b, .5f, mode != 0));
        b.count = 2;
        EXPECT_FALSE(de__anim_pose_blend(&out, &a, &b, NAN, mode != 0));
        EXPECT_FALSE(de__anim_pose_blend(&out, &a, &b, 1.5f, mode != 0));
        EXPECT_TRUE(result_t[0].x == 1 && out.root_translation.x == 12);
    }
}

static void three_bone_pose(de_anim_system *s, void *user) {
    (void)user;
    ASSERT_EQ(s->job_count, 1u);
    de__anim_local_pose *p = &s->entries[s->jobs[0]].local;
    ASSERT_EQ(p->count, 3u);
    p->rotations[0] = (de_quat){0, 0, .7071067811865476f, .7071067811865476f};
    p->rotations[1] = p->rotations[2] = (de_quat){0, 0, 0, 1};
    p->translations[0] = (de_vec3){1, 2, 0};
    p->translations[1] = (de_vec3){0, 1, 0};
    p->translations[2] = (de_vec3){0, 2, 0};
    for (u32 j = 0; j < 3; ++j)
        p->scales[j] = (de_vec3){1, 1, 1};
}
TEST_F(anim, three_bone_rotated_chain_has_analytic_model_palette_and_world_socket, anim_fixture) {
    load_assets(f, 3);
    de_entity entity = spawn_animator(f);
    DeAnimator *a = de_entity_get(f->world, entity, de_anim_component(f->anim));
    a->playing = false;
    ASSERT_EQ(de_anim__register_stage(f->anim, DE_ANIM_STAGE_BLEND, three_bone_pose, NULL), DE_OK);
    /* Retain the large-distance case and the exact S27 acceptance anchor. */
    const de_dvec3 origins[] = {{1000000000.25, 0, 0}, {100000, 100000, 0}};
    for (u32 i = 0; i < sizeof origins / sizeof origins[0]; ++i) {
        DeTransform *transform = de_entity_get(f->world, entity, f->anim->builtins.transform);
        transform->position = origins[i];
        anim_frame(f, 0);
        const de__anim_entry *entry = &f->anim->entries[de_entity_index(entity)];
        EXPECT_TRUE(fabsf(entry->model[1].m[9]) < 1e-6f);
        EXPECT_TRUE(fabsf(entry->model[2].m[9] + 2) < 1e-6f);
        EXPECT_TRUE(fabsf(entry->model[2].m[10] - 2) < 1e-6f);
        EXPECT_TRUE(fabsf(entry->palette[2].m[9]) < 1e-6f);
        EXPECT_TRUE(fabsf(entry->palette[2].m[10] - 2) < 1e-6f);
        de_transform socket;
        ASSERT_TRUE(de_anim_bone_transform(f->world, entity, de_name_from("joint2"), &socket));
        EXPECT_TRUE(fabs(socket.position.x - (origins[i].x - 2)) < 1e-6);
        EXPECT_TRUE(fabs(socket.position.y - (origins[i].y + 2)) < 1e-6);
        EXPECT_TRUE(fabs(socket.position.z - origins[i].z) < 1e-6);
    }
}
