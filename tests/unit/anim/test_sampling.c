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
    EXPECT_TRUE(de_world_task_system(f->world) == f->tasks);
    EXPECT_FALSE(de_world_task_system(NULL));
    /* A serial world with an explicit animation pool exercises the S19 binding
     * fallback without violating one-task-system-per-creating-thread ownership. */
    anim_fixture separate = {.allocator = f->allocator,
                             .tasks = f->tasks,
                             .types = f->types,
                             .assets = f->assets,
                             .skeleton = f->skeleton,
                             .clip = f->clip};
    ASSERT_EQ(de_world_create(&(de_world_desc){.allocator = &f->allocator, .types = f->types},
                              &separate.world),
              DE_OK);
    ASSERT_EQ(de_anim_create(&f->allocator, f->assets, &separate.anim), DE_OK);
    de_anim_set_task_system(separate.anim, f->tasks);
    de_anim_attach(separate.anim, separate.world);
    ASSERT_TRUE(de_anim_component(separate.anim).id);
    EXPECT_FALSE(de_world_task_system(separate.world));
    de_entity entities[2][1000];
    const size_t local_bytes = 60 * sizeof(de_cooked_joint_pose);
    const size_t affine_bytes = 60 * sizeof(de_affine), stride = local_bytes + 2 * affine_bytes;
    u8 *reference = malloc(1000 * stride);
    ASSERT_TRUE(reference);
    for (u32 i = 0; i < 1000; ++i) {
        entities[0][i] = spawn_animator(f);
        entities[1][i] = spawn_animator(&separate);
    }
    for (u32 pass = 0; pass < 3; ++pass) {
        anim_fixture *active = pass == 2 ? &separate : f;
        u32 world_index = pass == 2 ? 1 : 0;
        de_anim_set_task_system(active->anim, pass == 1 ? NULL : f->tasks);
        for (u32 i = 0; i < 1000; ++i) {
            DeAnimator *a = de_entity_get(active->world, entities[world_index][i],
                                          de_anim_component(active->anim));
            a->time = (f32)(i % 97) / 97;
            a->speed = i & 1 ? -2.3f : 3.1f;
        }
        anim_frame(active, .17f);
        for (u32 i = 0; i < 1000; ++i) {
            const de__anim_entry *e =
                &active->anim->entries[de_entity_index(entities[world_index][i])];
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
    de_anim_set_task_system(f->anim, f->tasks);
    de_anim_destroy(separate.anim);
    de_world_destroy(separate.world);
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

/* Compare against the independent compressed reference, including exact cursor
 * accounting. One-joint clips cover complete tracks; three joints retain rest. */
TEST_F(anim, prepared_keys_match_compressed_samples_and_cursor, anim_fixture) {
    for (u32 variant = 0; variant < 6; ++variant) {
        u32 joints = variant & 1 ? 3 : 1;
        anim_blob skeleton = skeleton_blob(joints);
        anim_blob blob = variant < 2 ? clip_blob(joints) : dense_clip_blob(joints);
        de_cooked_skeleton_view skel;
        de_cooked_clip_view clip;
        ASSERT_TRUE(de_cooked_skeleton_parse(skeleton.data, skeleton.bytes, &skel, NULL));
        ASSERT_TRUE(de_cooked_clip_parse(blob.data, blob.bytes, &clip, NULL));
        if (variant == 4) {
            /* Constant T/R/S, including rotation, preserve authored key bits. */
            for (u32 i = 0; i < clip.track_count; ++i) {
                de_cooked_clip_track track = de_cooked_clip_get_track(&clip, i);
                u8 *record = blob.data + 80 + i * 16;
                record[3] = DE_COOKED_CLIP_ENC_CONST;
                de_cooked_put_u32(record + 8, 1);
                de_cooked_put_u32(record + 12, (u32)(track.values - clip.data));
            }
            ASSERT_TRUE(de_cooked_clip_parse(blob.data, blob.bytes, &clip, NULL));
        } else if (variant == 5) {
            /* Nonuniform scale covers all three output components. */
            u32 old_bytes = blob.bytes;
            blob.bytes += 8;
            blob.data = realloc(blob.data, blob.bytes);
            ASSERT_TRUE(blob.data);
            de_cooked_put_u32(blob.data + 48, (u32)clip.data_size + 8);
            de_cooked_put_u32(blob.data + 52, blob.bytes);
            blob.data[80 + 32 + 4] = 3;
            blob.data[80 + 32 + 5] = 0;
            de_cooked_put_f32(blob.data + old_bytes, 2);
            de_cooked_put_f32(blob.data + old_bytes + 4, 3);
            ASSERT_TRUE(de_cooked_clip_parse(blob.data, blob.bytes, &clip, NULL));
        }
        de__anim_prepared_clip *prepared = NULL;
        ASSERT_TRUE(de__anim_prepare_clip(&f->allocator, &clip, &prepared));
        ASSERT_TRUE(prepared);
        EXPECT_TRUE(prepared->base == clip.base);
        u32 keys[3] = {0}, reference_keys[3] = {0};
        de__anim_clip_cursor cursor = {.keys = keys, .capacity = 3};
        de__anim_clip_cursor reference_cursor = {.keys = reference_keys, .capacity = 3};
        de_quat rotations[3];
        de_vec3 translations[3], scales[3];
        de__anim_local_pose pose = {.count = joints,
                                    .rotations = rotations,
                                    .translations = translations,
                                    .scales = scales};
        de_cooked_joint_pose reference[3];
        const f32 seeks[] = {.8f, .2f, .95f, .95f, .001f, 1, 0, -1, NAN, INFINITY};
        de_alloc_guard guard = {0};
        de_alloc_guard_begin(&guard);
        for (u32 i = 0; i < 4107; ++i) {
            f32 time = i <= 4096 ? (f32)i / 4096 : seeks[i - 4097];
            memset(rotations, 0xa5, sizeof rotations);
            memset(translations, 0xa5, sizeof translations);
            memset(scales, 0xa5, sizeof scales);
            ASSERT_TRUE(de__anim_sample_prepared(&clip, prepared, &skel, time, &pose, &cursor));
            ASSERT_TRUE(de__anim_sample_cached(&clip, &skel, time, reference, &reference_cursor));
            EXPECT_TRUE(pose_matches_reference(&pose, reference));
            ASSERT_TRUE(de__anim_clip_sample(&clip, &skel, time, reference));
            EXPECT_TRUE(pose_matches_reference(&pose, reference));
            EXPECT_TRUE(memcmp(keys, reference_keys, sizeof keys) == 0);
            EXPECT_EQ(cursor.binary_searches, reference_cursor.binary_searches);
            EXPECT_EQ(cursor.forward_steps, reference_cursor.forward_steps);
            EXPECT_TRUE(cursor.clip_base == reference_cursor.clip_base);
            EXPECT_TRUE(cursor.time == reference_cursor.time && cursor.valid);
            EXPECT_TRUE(pose.root_translation.x == 0 && pose.root_translation.y == 0 &&
                        pose.root_translation.z == 0 && pose.root_rotation.x == 0 &&
                        pose.root_rotation.y == 0 && pose.root_rotation.z == 0 &&
                        pose.root_rotation.w == 1);
        }
        /* NULL cache exercises the production oversize fallback; foreign cache is rejected. */
        ASSERT_TRUE(de__anim_sample_prepared(&clip, NULL, &skel, .3f, &pose, &cursor));
        ASSERT_TRUE(de__anim_clip_sample(&clip, &skel, .3f, reference));
        EXPECT_TRUE(pose_matches_reference(&pose, reference));
        de_cooked_clip_view foreign = clip;
        foreign.base = NULL;
        EXPECT_FALSE(de__anim_sample_prepared(&foreign, prepared, &skel, .3f, &pose, &cursor));
        de_alloc_guard_end(&guard);
        DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
        DE_FREE(&f->allocator, prepared, prepared->bytes);
        free(blob.data);
        free(skeleton.data);
    }
}

TEST_F(anim, prepared_cache_failure_and_large_shared_ranges_are_bounded, anim_fixture) {
    anim_blob seed = dense_clip_blob(62000);
    de_cooked_clip_view clip;
    ASSERT_TRUE(de_cooked_clip_parse(seed.data, seed.bytes, &clip, NULL));
    de__anim_prepared_clip *prepared = NULL;
    u64 before = de_tracking_allocator_stats(&f->tracking).bytes_live;
    de_tracking_allocator_fail_after(&f->tracking, 0);
    EXPECT_FALSE(de__anim_prepare_clip(&f->allocator, &clip, &prepared));
    de_tracking_allocator_fail_after(&f->tracking, -1);
    EXPECT_FALSE(prepared);
    EXPECT_EQ(de_tracking_allocator_stats(&f->tracking).bytes_live, before);
    /* Valid tracks may alias a raw key range: small source, >64MiB expansion. */
    const u32 tracks = 62000, times = 80 + tracks * 16;
    u32 data = aligned(times + clip.time_count * 4), size = data + (u32)clip.data_size;
    anim_blob large = {calloc(1, size), size};
    ASSERT_TRUE(large.data);
    memcpy(large.data, seed.data, 80);
    de_cooked_put_u32(large.data + 16, tracks);
    de_cooked_put_u32(large.data + 40, times);
    de_cooked_put_u32(large.data + 44, data);
    de_cooked_put_u32(large.data + 52, size);
    for (u32 i = 0; i < tracks; ++i) {
        memcpy(large.data + 80 + i * 16, seed.data + 80, 16);
        put_u16(large.data + 80 + i * 16, (u16)i);
    }
    memcpy(large.data + times, clip.times, clip.time_count * 4);
    memcpy(large.data + data, clip.data, (size_t)clip.data_size);
    ASSERT_TRUE(de_cooked_clip_parse(large.data, large.bytes, &clip, NULL));
    de_alloc_guard guard = {0};
    de_alloc_guard_begin(&guard);
    EXPECT_TRUE(de__anim_prepare_clip(&f->allocator, &clip, &prepared));
    EXPECT_FALSE(prepared);
    de_alloc_guard_end(&guard);
    DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
    free(large.data);
    free(seed.data);
}

TEST_F(anim, prepared_asset_residency_counts_shared_cache_once, anim_fixture) {
    load_assets(f, 3);
    const de__anim_asset *asset = de__anim_asset_get(f->anim, f->clip.id, "anim_clip");
    ASSERT_TRUE(asset && asset->prepared);
    const de__slot *slot = de__slot_get(f->assets, f->clip.id);
    EXPECT_EQ(slot->bytes, asset->bytes + asset->prepared->bytes);
    const de__anim_prepared_clip *cache = asset->prepared;
    for (u32 i = 0; i < 32; ++i)
        spawn_animator(f);
    anim_frame(f, .1f);
    EXPECT_TRUE(de__anim_asset_get(f->anim, f->clip.id, "anim_clip")->prepared == cache);
    EXPECT_EQ(slot->bytes, asset->bytes + cache->bytes);
}
