#include "anim_fixture.h"
#include "de_alloc_guard.h"
#include "asset/hot_reload_internal.h"
#include "asset/slots_internal.h"

static bool pose_matches_reference(const de__anim_local_pose *pose,
                                   const de_cooked_joint_pose *reference) {
    for (u32 j = 0; j < pose->count; ++j) {
        de_cooked_joint_pose actual = de__anim_pose_joint(pose, j);
        if (memcmp(&actual, &reference[j], sizeof actual) != 0)
            return false;
    }
    return true;
}

static anim_blob dense_clip_blob(u32 joints) {
    const u32 keys = 65, indices = (keys * 2 + 3) & ~3u;
    const u32 tracks = 80, times = 128, data = aligned(times + keys * 4);
    const u32 rotation = indices + keys * 12, scale = rotation + indices + keys * 16;
    const u32 size = data + scale + 4;
    anim_blob blob = {calloc(1, size), size};
    u8 *p = blob.data;
    memcpy(p, "DCLP", 4);
    de_cooked_put_u32(p + 4, 1);
    de_cooked_put_u32(p + 12, joints);
    de_cooked_put_u32(p + 16, 3);
    de_cooked_put_u32(p + 20, keys);
    de_cooked_put_u32(p + 24, DE_COOKED_NONE);
    de_cooked_put_f32(p + 28, 64);
    de_cooked_put_f32(p + 32, 1);
    de_cooked_put_u32(p + 36, tracks);
    de_cooked_put_u32(p + 40, times);
    de_cooked_put_u32(p + 44, data);
    de_cooked_put_u32(p + 48, size - data);
    de_cooked_put_u32(p + 52, size);
    const u32 offsets[] = {0, rotation, scale};
    for (u32 i = 0; i < 3; ++i) {
        u8 *track = p + tracks + i * 16;
        track[2] = (u8)i;
        track[3] = i == 2 ? DE_COOKED_CLIP_ENC_CONST : DE_COOKED_CLIP_ENC_RAW;
        track[4] = (u8)(i == 0 ? 3 : i == 1 ? 4 : 1);
        if (i == 2)
            track[5] = DE_COOKED_CLIP_TRACK_UNIFORM;
        de_cooked_put_u32(track + 8, i == 2 ? 1 : keys);
        de_cooked_put_u32(track + 12, offsets[i]);
    }
    for (u32 i = 0; i < keys; ++i) {
        f32 time = (f32)(i * i) / 4096;
        de_cooked_put_f32(p + times + i * 4, time);
        put_u16(p + data + i * 2, (u16)i);
        put_u16(p + data + rotation + i * 2, (u16)i);
        for (u32 c = 0; c < 3; ++c)
            de_cooked_put_f32(p + data + indices + i * 12 + c * 4, (f32)(i * (c + 1)) / 64);
        f32 quat[] = {0, 0, (f32)sin(time), (f32)cos(time)};
        /* Include antipodal authored keys to exercise shortest-arc nlerp. */
        for (u32 c = 0; c < 4; ++c)
            de_cooked_put_f32(p + data + rotation + indices + i * 16 + c * 4,
                              i & 1 ? -quat[c] : quat[c]);
    }
    de_cooked_put_f32(p + data + scale, 1);
    return blob;
}

TEST_F(anim, cursor_matches_reference_forward_reverse_and_seeks, anim_fixture) {
    f->skel_blob = skeleton_blob(3);
    f->clip_blob = dense_clip_blob(3);
    de_cooked_skeleton_view skel;
    de_cooked_clip_view clip;
    const char *why = NULL;
    ASSERT_TRUE(de_cooked_skeleton_parse(f->skel_blob.data, f->skel_blob.bytes, &skel, &why));
    ASSERT_TRUE(de_cooked_clip_parse(f->clip_blob.data, f->clip_blob.bytes, &clip, &why));
    u32 keys[3] = {0};
    u32 pose_keys[3] = {0};
    de__anim_clip_cursor cursor = {.keys = keys, .capacity = 3};
    de__anim_clip_cursor pose_cursor = {.keys = pose_keys, .capacity = 3};
    de_quat rotations[3];
    de_vec3 translations[3], scales[3];
    de__anim_local_pose pose = {
        .count = 3, .rotations = rotations, .translations = translations, .scales = scales};
    de_cooked_joint_pose actual[3], reference[3];
    de_alloc_guard guard = {0};
    de_alloc_guard_begin(&guard);
    for (u32 i = 0; i <= 4096; ++i) {
        f32 time = (f32)i / 4096;
        ASSERT_TRUE(de__anim_sample_cached(&clip, &skel, time, actual, &cursor));
        ASSERT_TRUE(de__anim_clip_sample(&clip, &skel, time, reference));
        ASSERT_TRUE(memcmp(actual, reference, sizeof actual) == 0);
        ASSERT_TRUE(de__anim_sample_pose_cached(&clip, &skel, time, &pose, &pose_cursor));
        ASSERT_TRUE(pose_matches_reference(&pose, reference));
        EXPECT_TRUE(pose.root_translation.x == 0 && pose.root_rotation.w == 1);
    }
    EXPECT_EQ(cursor.binary_searches, 0u);
    EXPECT_EQ(pose_cursor.binary_searches, 0u);
    EXPECT_TRUE(cursor.forward_steps > 0);
    u64 searches = cursor.binary_searches;
    u64 forward_steps = cursor.forward_steps;
    /* Reversed times, forward jumps, wraps, endpoints and invalid-time clamps. */
    const f32 seek_times[] = {.8f, .2f, .95f, .95f, .001f, 1, 0, -1, NAN, INFINITY};
    for (u32 i = 0; i < sizeof seek_times / sizeof seek_times[0]; ++i) {
        ASSERT_TRUE(de__anim_sample_cached(&clip, &skel, seek_times[i], actual, &cursor));
        ASSERT_TRUE(de__anim_clip_sample(&clip, &skel, seek_times[i], reference));
        ASSERT_TRUE(memcmp(actual, reference, sizeof actual) == 0);
        ASSERT_TRUE(de__anim_sample_pose_cached(&clip, &skel, seek_times[i], &pose, &pose_cursor));
        ASSERT_TRUE(pose_matches_reference(&pose, reference));
    }
    EXPECT_TRUE(cursor.binary_searches > searches);
    de_alloc_guard_end(&guard);
    DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
    printf("4097 forward samples: %llu searches/%llu key advances; seek samples: %llu searches\n",
           (unsigned long long)searches, (unsigned long long)forward_steps,
           (unsigned long long)(cursor.binary_searches - searches));
    /* A fresh blob with another key count must not inherit the old key indices. */
    anim_blob other = clip_blob(3);
    de_cooked_clip_view replacement;
    ASSERT_TRUE(de_cooked_clip_parse(other.data, other.bytes, &replacement, &why));
    ASSERT_TRUE(de__anim_sample_cached(&replacement, &skel, .37f, actual, &cursor));
    ASSERT_TRUE(de__anim_clip_sample(&replacement, &skel, .37f, reference));
    EXPECT_TRUE(memcmp(actual, reference, sizeof actual) == 0);
    free(other.data);
}

TEST_F(anim, pooled_pose_blocks_are_reused_after_entity_churn, anim_fixture) {
    load_assets(f, 60);
    de_entity entities[32];
    void *old_blocks[32];
    for (u32 i = 0; i < 32; ++i)
        entities[i] = spawn_animator(f);
    anim_frame(f, .1f);
    de_pool *pool = f->anim->entries[de_entity_index(entities[0])].pose_pool;
    ASSERT_TRUE(pool);
    de_pool_info before = de_pool_get_info(pool);
    EXPECT_EQ(before.blocks_live, 32u);
    for (u32 i = 0; i < 32; ++i) {
        de__anim_entry *e = &f->anim->entries[de_entity_index(entities[i])];
        EXPECT_TRUE(e->pose_pool == pool);
        old_blocks[i] = e->block;
        e->local.root_translation.x = 7;
        e->local.root_rotation.w = 0;
        de_entity_despawn(f->world, entities[i]);
    }
    anim_frame(f, .1f);
    EXPECT_EQ(de_pool_get_info(pool).blocks_live, 0u);
    for (u32 i = 0; i < 32; ++i)
        entities[i] = spawn_animator(f);
    anim_frame(f, .1f);
    for (u32 i = 0; i < 32; ++i) {
        const de__anim_entry *e = &f->anim->entries[de_entity_index(entities[i])];
        ASSERT_TRUE(e->ready && e->finalized);
        bool reused = false;
        for (u32 j = 0; j < 32; ++j)
            reused |= e->block == old_blocks[j];
        EXPECT_TRUE(reused);
        EXPECT_TRUE(e->local.root_translation.x == 0 && e->local.root_rotation.w == 1);
        EXPECT_TRUE(((uintptr_t)e->local.rotations & 15u) == 0);
        EXPECT_TRUE(((uintptr_t)e->local.translations & 15u) == 0);
        EXPECT_TRUE(((uintptr_t)e->local.scales & 15u) == 0);
    }
    EXPECT_EQ(de_pool_get_info(pool).page_count, before.page_count);
    EXPECT_EQ(de_pool_get_info(pool).blocks_live, 32u);
}

TEST_F(anim, pose_pool_creation_and_page_failure_recover_without_partial_pose, anim_fixture) {
    load_assets(f, 60);
    de_entity entity = spawn_animator(f);
    DeAnimator *a = de_entity_get(f->world, entity, de_anim_component(f->anim));
    a->clip = (de_anim_clip){0};
    /* Warm ECS storage/schedules without ever allocating a pose pool. */
    anim_frame(f, 0);
    a = de_entity_get(f->world, entity, de_anim_component(f->anim));
    a->clip = f->clip;
    for (i64 allocations = 0; allocations < 2; ++allocations) {
        de_tracking_allocator_fail_after(&f->tracking, allocations);
        anim_frame(f, 0);
        de_tracking_allocator_fail_after(&f->tracking, -1);
        const de__anim_entry *e = &f->anim->entries[de_entity_index(entity)];
        EXPECT_FALSE(e->ready);
        EXPECT_FALSE(e->block);
        for (u32 i = 0; i < DE__ANIM_POSE_POOL_CLASSES; ++i)
            if (f->anim->pose_pools[i]) {
                EXPECT_EQ(de_pool_get_info(f->anim->pose_pools[i]).page_count, 0u);
                EXPECT_EQ(de_pool_get_info(f->anim->pose_pools[i]).blocks_live, 0u);
            }
    }
    anim_frame(f, 0);
    const de__anim_entry *e = &f->anim->entries[de_entity_index(entity)];
    EXPECT_TRUE(e->ready && e->finalized);
    EXPECT_EQ(de_pool_get_info(e->pose_pool).blocks_live, 1u);
    EXPECT_TRUE(e->local.root_rotation.w == 1);
}

TEST_F(anim, playback_uses_cursor_and_resets_after_clip_replacement, anim_fixture) {
    load_assets(f, 3);
    de_entity entity = spawn_animator(f);
    for (u32 i = 0; i < 8; ++i)
        anim_frame(f, .03f);
    de__anim_entry *entry = &f->anim->entries[de_entity_index(entity)];
    EXPECT_TRUE(entry->cursor.valid);
    EXPECT_EQ(entry->cursor.capacity, entry->clip.track_count);
    EXPECT_TRUE(entry->cursor.clip_base == entry->clip.base);
    EXPECT_EQ(entry->cursor.binary_searches, 0u);
    de_cooked_joint_pose reference[3];
    ASSERT_TRUE(
        de__anim_clip_sample(&entry->clip, &entry->skeleton, entry->animator->time, reference));
    EXPECT_TRUE(pose_matches_reference(&entry->local, reference));
    /* Asset replacement removes and re-adds the clip through the real gather path. */
    DeAnimator *animator = de_entity_get(f->world, entity, de_anim_component(f->anim));
    animator->clip = (de_anim_clip){0};
    anim_frame(f, .03f);
    EXPECT_FALSE(f->anim->entries[de_entity_index(entity)].cursor.valid);
    animator = de_entity_get(f->world, entity, de_anim_component(f->anim));
    animator->clip = f->clip;
    animator->time = .4f;
    anim_frame(f, 0);
    entry = &f->anim->entries[de_entity_index(entity)];
    EXPECT_TRUE(entry->cursor.valid);
    ASSERT_TRUE(de__anim_clip_sample(&entry->clip, &entry->skeleton, .4f, reference));
    EXPECT_TRUE(pose_matches_reference(&entry->local, reference));
}

TEST_F(anim, parallel_and_serial_sampling_match_all_pose_bytes, anim_fixture) {
    load_assets(f, 60);
    de_entity entities[1000];
    const size_t local_bytes = 60 * sizeof(de_cooked_joint_pose);
    const size_t affine_bytes = 60 * sizeof(de_affine), stride = local_bytes + 2 * affine_bytes;
    u8 *reference = malloc(1000 * stride);
    ASSERT_TRUE(reference);
    for (u32 i = 0; i < 1000; ++i)
        entities[i] = spawn_animator(f);
    for (u32 pass = 0; pass < 2; ++pass) {
        de_anim_set_task_system(f->anim, pass == 0 ? f->tasks : NULL);
        for (u32 i = 0; i < 1000; ++i) {
            DeAnimator *a = de_entity_get(f->world, entities[i], de_anim_component(f->anim));
            a->time = (f32)(i % 97) / 97;
            a->speed = i & 1 ? -2.3f : 3.1f;
        }
        anim_frame(f, .17f);
        for (u32 i = 0; i < 1000; ++i) {
            const de__anim_entry *e = &f->anim->entries[de_entity_index(entities[i])];
            ASSERT_TRUE(e->ready && e->finalized);
            u8 *p = reference + (size_t)i * stride;
            if (pass == 0) {
                for (u32 j = 0; j < e->local.count; ++j) {
                    de_cooked_joint_pose joint = de__anim_pose_joint(&e->local, j);
                    memcpy(p + (size_t)j * sizeof joint, &joint, sizeof joint);
                }
                memcpy(p + local_bytes, e->model, affine_bytes);
                memcpy(p + local_bytes + affine_bytes, e->palette, affine_bytes);
            } else {
                EXPECT_TRUE(pose_matches_reference(&e->local, (const de_cooked_joint_pose *)p));
                EXPECT_TRUE(memcmp(p + local_bytes, e->model, affine_bytes) == 0);
                EXPECT_TRUE(memcmp(p + local_bytes + affine_bytes, e->palette, affine_bytes) == 0);
            }
        }
    }
    free(reference);
}

#if DE__ASSET_RELOAD_ENABLED
typedef anim_fixture anim_reload_fixture;
static void anim_reload_fixture_setup(anim_reload_fixture *f) {
    anim_fixture_setup_options(f, true);
}
static void anim_reload_fixture_teardown(anim_reload_fixture *f) {
    anim_fixture_teardown(f);
}
TEST_F(anim, published_clip_reload_invalidates_key_intervals, anim_reload_fixture) {
    load_assets(f, 3);
    de_entity entity = spawn_animator(f);
    anim_frame(f, .4f);
    anim_blob dense = dense_clip_blob(3);
    for (u32 pass = 0; pass < 2; ++pass) {
        const de__slot *slot = de__slot_get(f->assets, f->clip.id);
        u32 version = slot->version;
        const anim_blob *replacement = pass == 0 ? &dense : &f->clip_blob;
        ASSERT_EQ(
            de_file_write_all_atomic("animtest:/clip.dclp", replacement->data, replacement->bytes),
            DE_OK);
        /* Inject only the file notification; loading/publication use the real pipeline. */
        de__asset_reload_on_change(f->assets, "animtest:/clip.dclp", DE_WATCH_MODIFIED, false);
        u64 deadline = de_time_now_ns() + 5000000000ull;
        while (slot->version == version && de_time_now_ns() < deadline) {
            de_asset_system_update(f->assets, NULL);
            de_sleep_ns(1000000);
        }
        ASSERT_TRUE(slot->version != version);
        anim_frame(f, 0);
        const de__anim_entry *entry = &f->anim->entries[de_entity_index(entity)];
        ASSERT_TRUE(entry->ready && entry->cursor.valid);
        EXPECT_EQ(entry->clip_version, slot->version);
        de_cooked_joint_pose reference[3];
        ASSERT_TRUE(de__anim_clip_sample(&entry->clip, &entry->skeleton, .4f, reference));
        EXPECT_TRUE(pose_matches_reference(&entry->local, reference));
        EXPECT_EQ(de_cooked_clip_get_track(&entry->clip, 0).key_count, pass == 0 ? 65u : 2u);
    }
    free(dense.data);
}
#endif
