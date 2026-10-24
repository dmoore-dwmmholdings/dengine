#include "anim_fixture.h"
#include "anim/graph_internal.h"
#include "anim/sync_groups.h"
#include "de_alloc_guard.h"
#include "asset/slots_internal.h"
static de_anim_graph sync_graph(anim_fixture *f, u32 groups, de_anim_clip second) {
    de_anim_param_desc param = {.name = de_name_from("blend"), .float_default = .5f};
    de_anim_node_desc nodes[3] = {
        {.kind = DE_ANIM_NODE_CLIP, .data.clip = {.clip = f->clip, .speed = 1, .loop = true}},
        {.kind = DE_ANIM_NODE_CLIP,
         .data.clip = {
             .clip = second.id ? second : f->clip, .speed = second.id ? 1 : 2, .loop = true}}};
    if (groups) {
        nodes[0].data.clip.sync_group = de_name_from("feet");
        nodes[1].data.clip.sync_group = de_name_from(groups == 2 ? "hands" : "feet");
    }
    de_anim_blend_point_desc points[] = {{{0, 0}, 0}, {{1, 0}, 1}};
    nodes[2] = (de_anim_node_desc){
        .kind = DE_ANIM_NODE_BLEND1D,
        .data.blend = {.x_param = param.name, .points = points, .point_count = 2}};
    de_anim_graph graph = {0};
    ASSERT_EQ(de_anim_graph_create(f->anim,
                                   &(de_anim_graph_desc){.params = &param,
                                                         .param_count = 1,
                                                         .nodes = nodes,
                                                         .node_count = 3,
                                                         .output_node = 2},
                                   &graph),
              DE_OK);
    return graph;
}
static de_entity sync_actor(anim_fixture *f, de_anim_graph graph, bool fixed, f32 time) {
    de_entity entity = spawn_animator(f);
    ASSERT_EQ(de_anim_graph_bind(f->world, entity, graph), DE_OK);
    DeAnimator *a = de_entity_get(f->world, entity, f->anim->component);
    a->deterministic = fixed;
    a->time = time;
    a->playing = false;
    a->speed = 1;
    return entity;
}
static de__anim_sync_state *sync_state(anim_fixture *f, de_entity entity, de_anim_graph graph) {
    de__anim_graph *g = de__anim_graph_get(f->anim, graph);
    ASSERT_TRUE(g && g->sync_component.id);
    de__anim_sync_state *state = de_entity_get(f->world, entity, g->sync_component);
    ASSERT_TRUE(state);
    return state;
}
static void sync_tick(anim_fixture *f, f32 dt) {
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_FIXED_UPDATE, dt, 0), DE_OK);
}
static f32 bone_x(anim_fixture *f, de_entity entity) {
    de_transform bone;
    ASSERT_TRUE(de_anim_bone_transform(f->world, entity, de_name_from("joint0"), &bone));
    return bone.position.x;
}
TEST_F(anim_graph_sync, visual_leaf_sampling_and_continuous_leader_changes, anim_fixture) {
    load_assets(f, 2);
    de_anim_graph graph = sync_graph(f, 1, (de_anim_clip){0});
    de_entity entity = sync_actor(f, graph, false, .25f);
    anim_frame(f, 0);
    EXPECT_FLOAT_EQ(bone_x(f, entity), .5f, 1e-6f);
    de__anim_sync_state *state = sync_state(f, entity, graph);
    EXPECT_EQ(state->clocks[1].leader, 0u);
    de_anim_set_float(f->world, entity, de_name_from("blend"), .75f);
    anim_frame(f, 0);
    EXPECT_FLOAT_EQ(bone_x(f, entity), .5f, 1e-6f);
    EXPECT_EQ(sync_state(f, entity, graph)->clocks[1].leader, 1u);
    DeAnimator *a = de_entity_get(f->world, entity, f->anim->component);
    a->playing = true;
    anim_frame(f, .125f);
    EXPECT_FLOAT_EQ(bone_x(f, entity), 1, 1e-6f);
    a = de_entity_get(f->world, entity, f->anim->component);
    a->playing = false;
    anim_frame(f, .125f);
    EXPECT_FLOAT_EQ(bone_x(f, entity), 1, 1e-6f);
    a = de_entity_get(f->world, entity, f->anim->component);
    a->time = .125f; /* Explicit seek resets normalized phase at current leader rate. */
    anim_frame(f, 0);
    EXPECT_FLOAT_EQ(bone_x(f, entity), .5f, 1e-6f);
}
TEST_F(anim_graph_sync, different_duration_clips_share_actual_pose_phase, anim_fixture) {
    load_assets(f, 2);
    anim_blob blob = clip_blob(2);
    de_cooked_put_f32(blob.data + 32, .6f);
    de_cooked_put_f32(blob.data + 128 + 4, .6f);
    ASSERT_EQ(de_file_write_all_atomic("animtest:/run.dclp", blob.data, blob.bytes), DE_OK);
    free(blob.data);
    const char *manifest = "[assets.\"test/skeleton\"]\ntype='skeleton'\nsource='skeleton.dskl'\n"
                           "[assets.\"test/clip\"]\ntype='anim_clip'\nsource='clip.dclp'\n"
                           "[assets.\"test/run\"]\ntype='anim_clip'\nsource='run.dclp'\n";
    ASSERT_EQ(de_file_write_all_atomic("animtest:/assets.toml", manifest, strlen(manifest)), DE_OK);
    ASSERT_EQ(de_asset_manifest_load(f->assets, "animtest:/assets.toml"), DE_OK);
    de_load_handle load = de_asset_load(f->assets, de_asset_id_from("test/run"), 0);
    ASSERT_EQ(de_asset_wait(f->assets, load, 10000000000ull), DE_LOAD_READY);
    de_anim_clip run = de_anim_clip_asset(f->anim, de_asset_id_from("test/run"));
    ASSERT_TRUE(run.id);
    de_anim_graph graph = sync_graph(f, 1, run);
    de_entity entity = sync_actor(f, graph, false, .25f);
    anim_frame(f, 0);
    EXPECT_FLOAT_EQ(bone_x(f, entity), .5f, 1e-6f);
    de_anim_set_float(f->world, entity, de_name_from("blend"), 1);
    anim_frame(f, 0);
    EXPECT_FLOAT_EQ(bone_x(f, entity), .5f, 1e-6f);
    DeAnimator *a = de_entity_get(f->world, entity, f->anim->component);
    a->playing = true;
    anim_frame(f, .125f);
    EXPECT_FLOAT_EQ(bone_x(f, entity), 2 * (.25 + .125 / (f64).6f), 1e-6);
    de_asset_release(f->assets, load);
}
TEST_F(anim_graph_sync, groups_and_actors_are_independent_and_rebinding_cleans_old_storage,
       anim_fixture) {
    load_assets(f, 2);
    de_anim_graph two = sync_graph(f, 2, (de_anim_clip){0});
    de_anim_graph one = sync_graph(f, 1, (de_anim_clip){0});
    de_entity first = sync_actor(f, two, false, .25f);
    de_entity second = sync_actor(f, two, false, .125f);
    anim_frame(f, 0);
    EXPECT_FLOAT_EQ(bone_x(f, first), .75f, 1e-6f);
    EXPECT_FLOAT_EQ(bone_x(f, second), .375f, 1e-6f);
    de_component old = de__anim_graph_get(f->anim, two)->sync_component;
    ASSERT_EQ(de_anim_graph_bind(f->world, first, one), DE_OK);
    anim_frame(f, 0);
    EXPECT_FALSE(de_entity_has(f->world, first, old));
    EXPECT_TRUE(de_entity_has(f->world, second, old));
    EXPECT_FLOAT_EQ(bone_x(f, first), .5f, 1e-6f);
    de_component active = de__anim_graph_get(f->anim, one)->sync_component;
    ASSERT_EQ(de_anim_graph_bind(f->world, first, (de_anim_graph){0}), DE_OK);
    anim_frame(f, 0);
    EXPECT_FALSE(de_entity_has(f->world, first, active));
    EXPECT_FALSE(de_entity_has(f->world, first, f->anim->sync_binding));
    de_anim_graph_destroy(f->anim, two);
    /* A freshly attached system discovers existing world-owned sync schemas. */
    de_component binding = f->anim->sync_binding;
    f->anim->sync_binding = (de_component){0};
    anim_frame(f, 0);
    EXPECT_FALSE(de_entity_has(f->world, second, old));
    EXPECT_FALSE(de_entity_has(f->world, second, binding));
}
static void sync_inputs(anim_fixture *f, de_entity entity, u32 tick) {
    de_anim_set_float(f->world, entity, de_name_from("blend"), tick % 3 ? .75f : .25f);
}
TEST_F(anim_graph_sync,
       fixed_snapshot_replay_ignores_intervening_visual_phase_and_hashes_sync_state, anim_fixture) {
    load_assets(f, 2);
    de_anim_graph graph = sync_graph(f, 1, (de_anim_clip){0});
    de_entity entity = sync_actor(f, graph, true, 0);
    DeAnimator *a = de_entity_get(f->world, entity, f->anim->component);
    a->playing = true;
    sync_tick(f, .125f);
    EXPECT_FLOAT_EQ(bone_x(f, entity), .25f, 1e-6f);
    u64 initial = de_anim_checksum(f->world), world = de_world_checksum(f->world, DE_COMP_SNAPSHOT);
    de_world_snapshot snapshot = {0};
    ASSERT_EQ(de_world_snapshot_take(f->world, &f->allocator, &snapshot), DE_OK);
    u64 animations[24], worlds[24];
    f32 poses[24];
    for (u32 i = 0; i < 24; ++i) {
        sync_inputs(f, entity, i);
        sync_tick(f, .03125f);
        animations[i] = de_anim_checksum(f->world);
        worlds[i] = de_world_checksum(f->world, DE_COMP_SNAPSHOT);
        poses[i] = bone_x(f, entity);
    }
    ASSERT_EQ(de_world_snapshot_restore(f->world, &snapshot), DE_OK);
    EXPECT_EQ(de_anim_checksum(f->world), initial);
    EXPECT_EQ(de_world_checksum(f->world, DE_COMP_SNAPSHOT), world);
    for (u32 i = 0; i < 24; ++i) {
        sync_inputs(f, entity, i);
        a = de_entity_get(f->world, entity, f->anim->component);
        a->deterministic = false;
        anim_frame(f, .017f * (f32)(i % 3 + 1));
        a = de_entity_get(f->world, entity, f->anim->component);
        a->deterministic = true;
        sync_tick(f, .03125f);
        EXPECT_EQ(de_anim_checksum(f->world), animations[i]);
        EXPECT_EQ(de_world_checksum(f->world, DE_COMP_SNAPSHOT), worlds[i]);
        EXPECT_FLOAT_EQ(bone_x(f, entity), poses[i], 0);
    }
    u64 hash = de_anim_checksum(f->world);
    de__anim_sync_state *state = sync_state(f, entity, graph);
    state->clocks[0].phase += .125;
    EXPECT_NE(de_anim_checksum(f->world), hash);
    de_world_snapshot_free(&f->allocator, &snapshot);
}
TEST_F(anim_graph_sync, restore_before_first_sync_component_and_visual_snapshot_replays,
       anim_fixture) {
    load_assets(f, 2);
    de_anim_graph graph = sync_graph(f, 1, (de_anim_clip){0});
    de_entity entity = sync_actor(f, graph, false, .25f);
    de_world_snapshot snapshot = {0};
    ASSERT_EQ(de_world_snapshot_take(f->world, &f->allocator, &snapshot), DE_OK);
    anim_frame(f, 0);
    de_anim_set_float(f->world, entity, de_name_from("blend"), .75f);
    anim_frame(f, 0);
    ASSERT_EQ(de_world_snapshot_restore(f->world, &snapshot), DE_OK);
    EXPECT_FALSE(
        de_entity_has(f->world, entity, de__anim_graph_get(f->anim, graph)->sync_component));
    anim_frame(f, 0);
    EXPECT_FLOAT_EQ(bone_x(f, entity), .5f, 1e-6f);
    de_world_snapshot_free(&f->allocator, &snapshot);
    ASSERT_EQ(de_world_snapshot_take(f->world, &f->allocator, &snapshot), DE_OK);
    de_anim_set_float(f->world, entity, de_name_from("blend"), .75f);
    DeAnimator *a = de_entity_get(f->world, entity, f->anim->component);
    a->playing = true;
    anim_frame(f, .125f);
    f32 expected = bone_x(f, entity);
    u64 checksum = de_world_checksum(f->world, DE_COMP_SNAPSHOT);
    ASSERT_EQ(de_world_snapshot_restore(f->world, &snapshot), DE_OK);
    de_anim_set_float(f->world, entity, de_name_from("blend"), .75f);
    a = de_entity_get(f->world, entity, f->anim->component);
    a->playing = true;
    anim_frame(f, .125f);
    EXPECT_FLOAT_EQ(bone_x(f, entity), expected, 0);
    EXPECT_EQ(de_world_checksum(f->world, DE_COMP_SNAPSHOT), checksum);
    de_world_snapshot_free(&f->allocator, &snapshot);
}
TEST_F(anim_graph_sync, visual_to_fixed_handoff_then_pause_reverse_seek_preserves_phase,
       anim_fixture) {
    load_assets(f, 2);
    de_anim_graph graph = sync_graph(f, 1, (de_anim_clip){0});
    de_entity entity = sync_actor(f, graph, false, .25f);
    anim_frame(f, 0);
    de_anim_set_float(f->world, entity, de_name_from("blend"), .75f);
    anim_frame(f, 0);
    DeAnimator *a = de_entity_get(f->world, entity, f->anim->component);
    a->playing = true;
    anim_frame(f, .125f);
    EXPECT_FLOAT_EQ(bone_x(f, entity), 1, 1e-6f);
    a = de_entity_get(f->world, entity, f->anim->component);
    a->deterministic = true;
    sync_tick(f, .125f);
    EXPECT_FLOAT_EQ(bone_x(f, entity), 1.5f, 1e-6f);
    a = de_entity_get(f->world, entity, f->anim->component);
    a->deterministic = false;
    anim_frame(f, .0625f);
    EXPECT_FLOAT_EQ(bone_x(f, entity), 1.75f, 1e-6f);
    a = de_entity_get(f->world, entity, f->anim->component);
    a->deterministic = true;
    sync_tick(f, .03125f);
    EXPECT_FLOAT_EQ(bone_x(f, entity), 1.625f, 1e-6f);
    de_anim_set_float(f->world, entity, de_name_from("blend"), .25f);
    a = de_entity_get(f->world, entity, f->anim->component);
    a->playing = false;
    sync_tick(f, .125f);
    EXPECT_FLOAT_EQ(bone_x(f, entity), 1.625f, 1e-6f);
    a = de_entity_get(f->world, entity, f->anim->component);
    a->playing = true;
    a->speed = -1;
    sync_tick(f, .0625f);
    EXPECT_FLOAT_EQ(bone_x(f, entity), 1.5f, 1e-6f);
    a = de_entity_get(f->world, entity, f->anim->component);
    a->time = .25f;
    sync_tick(f, 0);
    EXPECT_FLOAT_EQ(bone_x(f, entity), .5f, 1e-6f);
}
TEST_F(anim_graph_sync, fixed_restore_before_initial_state_recreates_phase_and_pose, anim_fixture) {
    load_assets(f, 2);
    de_anim_graph graph = sync_graph(f, 1, (de_anim_clip){0});
    de_entity entity = sync_actor(f, graph, true, .25f);
    de_world_snapshot snapshot = {0};
    ASSERT_EQ(de_world_snapshot_take(f->world, &f->allocator, &snapshot), DE_OK);
    sync_tick(f, 0);
    u64 expected = de_anim_checksum(f->world);
    EXPECT_FLOAT_EQ(bone_x(f, entity), .5f, 1e-6f);
    de_anim_set_float(f->world, entity, de_name_from("blend"), .75f);
    sync_tick(f, .125f);
    ASSERT_EQ(de_world_snapshot_restore(f->world, &snapshot), DE_OK);
    sync_tick(f, 0);
    EXPECT_EQ(de_anim_checksum(f->world), expected);
    EXPECT_FLOAT_EQ(bone_x(f, entity), .5f, 1e-6f);
    de_world_snapshot_free(&f->allocator, &snapshot);
}
TEST(anim_graph_sync, schema_and_actor_preflight_allocation_failures_recover_and_drain) {
    bool completed = false;
    for (i64 failure = 0; failure < 128; ++failure) {
        anim_fixture f = {0};
        anim_fixture_setup(&f);
        load_assets(&f, 2);
        de_anim_graph graph = sync_graph(&f, 1, (de_anim_clip){0});
        de_entity entity = sync_actor(&f, graph, false, .25f);
        de_tracking_allocator_fail_after(&f.tracking, failure);
        bool ok = de__anim_graph_sync_prepare(f.anim, entity);
        de_tracking_allocator_fail_after(&f.tracking, -1);
        de_error_clear();
        ASSERT_TRUE(de__anim_graph_sync_prepare(f.anim, entity));
        anim_frame(&f, 0);
        EXPECT_FLOAT_EQ(bone_x(&f, entity), .5f, 1e-6f);
        const DeAnimator *a = de_entity_get_const(f.world, entity, f.anim->component);
        EXPECT_EQ(a->graph.id, graph.id);
        EXPECT_FLOAT_EQ(a->time, .25f, 0);
        anim_fixture_teardown(&f);
        if (ok) {
            completed = true;
            EXPECT_GT(failure, 0);
            break;
        }
    }
    EXPECT_TRUE(completed);
}
TEST(anim_graph_sync,
     synchronized_fixed_actors_match_serial_and_parallel_without_warmed_allocations) {
    u64 reference[12] = {0};
    const u32 workers[] = {0, 3, 7};
    for (u32 mode = 0; mode < 3; ++mode) {
        anim_fixture f = {0};
        anim_fixture_setup_workers(&f, false, workers[mode]);
        load_assets(&f, 2);
        de_anim_graph graph = sync_graph(&f, 1, (de_anim_clip){0});
        de_entity actors[96];
        for (u32 i = 0; i < 96; ++i) {
            actors[i] = sync_actor(&f, graph, true, (f32)(i % 5) * .03125f);
            DeAnimator *a = de_entity_get(f.world, actors[i], f.anim->component);
            a->playing = true;
        }
        for (u32 i = 0; i < 3; ++i)
            sync_tick(&f, .03125f);
        /* Warm the checksum's independent row scratch too. */
        (void)de_anim_checksum(f.world);
        de_alloc_guard guard = {0};
        de_alloc_guard_begin(&guard);
        for (u32 tick = 0; tick < 12; ++tick) {
            for (u32 i = 0; i < 96; ++i)
                sync_inputs(&f, actors[i], tick + i);
            sync_tick(&f, .03125f);
            u64 checksum = de_anim_checksum(f.world);
            if (!mode)
                reference[tick] = checksum;
            else
                EXPECT_EQ(checksum, reference[tick]);
        }
        de_alloc_guard_end(&guard);
        DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
        anim_fixture_teardown(&f);
    }
}
TEST(anim_graph_sync, conflicting_sync_component_layout_is_rejected_before_sampling) {
    const char *names[] = {"DeAnimSyncState_1", "DeAnimSyncBinding"};
    for (u32 i = 0; i < 2; ++i) {
        anim_fixture f = {0};
        anim_fixture_setup(&f);
        load_assets(&f, 2);
        de_type_id type = de_type_register(
            f.types,
            &(de_type_desc){.name = names[i], .size = 8, .align = 8, .version = 1, .is_pod = true});
        ASSERT_TRUE(type.id);
        ASSERT_TRUE(de_component_register(f.world, type, DE_COMP_SNAPSHOT).id);
        de_anim_graph graph = sync_graph(&f, 1, (de_anim_clip){0});
        de_entity entity = sync_actor(&f, graph, false, .25f);
        EXPECT_FALSE(de__anim_graph_sync_prepare(f.anim, entity));
        de_error_clear();
        anim_frame(&f, 0);
        de__anim_entry *entry = &f.anim->entries[de_entity_index(entity)];
        EXPECT_FALSE(entry->ready);
        EXPECT_EQ(entry->graph_samples, 0u);
        anim_fixture_teardown(&f);
    }
}

static void write_marked_clip(const char *path, f32 duration, f32 right, const char *extra) {
    anim_blob blob = clip_blob(2);
    u32 offset = blob.bytes, count = extra ? 4 : 3;
    blob.bytes += count * DE_COOKED_CLIP_MARKER_SIZE;
    u8 *bytes = realloc(blob.data, blob.bytes);
    ASSERT_TRUE(bytes);
    memset(bytes + offset, 0, blob.bytes - offset);
    de_cooked_put_u32(bytes + 4, DE_COOKED_CLIP_VERSION_MARKERS);
    de_cooked_put_f32(bytes + 32, duration);
    de_cooked_put_f32(bytes + 132, duration);
    de_cooked_put_u32(bytes + 52, blob.bytes);
    de_cooked_put_u32(bytes + 64, offset);
    de_cooked_put_u32(bytes + 68, count);
    de_cooked_clip_marker records[4] = {{de_name_from("L"), 0},
                                        {de_name_from("R"), right},
                                        {de_name_from("L"), duration},
                                        {de_name_from(extra ? extra : "unused"), 0}};
    for (u32 i = 1; i < count; ++i) {
        de_cooked_clip_marker marker = records[i];
        u32 j = i;
        while (j && (records[j - 1].time > marker.time ||
                     (records[j - 1].time == marker.time &&
                      records[j - 1].name.hash > marker.name.hash))) {
            records[j] = records[j - 1];
            --j;
        }
        records[j] = marker;
    }
    for (u32 i = 0; i < count; ++i) {
        de_cooked_put_u64(bytes + offset + i * 16, records[i].name.hash);
        de_cooked_put_f32(bytes + offset + i * 16 + 8, records[i].time);
    }
    ASSERT_EQ(de_file_write_all_atomic(path, bytes, blob.bytes), DE_OK);
    free(bytes);
}
static de_anim_graph marked_graph_variant(anim_fixture *f, bool mixed) {
    load_assets(f, 2);
    write_marked_clip("animtest:/marked-walk.dclp", 1, .5f, mixed ? "walk-extra" : NULL);
    write_marked_clip("animtest:/marked-run.dclp", .6f, .15f, mixed ? "run-extra" : NULL);
    const char *manifest =
        "[assets.\"test/skeleton\"]\ntype='skeleton'\nsource='skeleton.dskl'\n"
        "[assets.\"test/clip\"]\ntype='anim_clip'\nsource='clip.dclp'\n"
        "[assets.\"test/marked-walk\"]\ntype='anim_clip'\nsource='marked-walk.dclp'\n"
        "[assets.\"test/marked-run\"]\ntype='anim_clip'\nsource='marked-run.dclp'\n";
    ASSERT_EQ(de_file_write_all_atomic("animtest:/assets.toml", manifest, strlen(manifest)), DE_OK);
    ASSERT_EQ(de_asset_manifest_load(f->assets, "animtest:/assets.toml"), DE_OK);
    de_load_handle a = de_asset_load(f->assets, de_asset_id_from("test/marked-walk"), 0);
    de_load_handle b = de_asset_load(f->assets, de_asset_id_from("test/marked-run"), 0);
    ASSERT_EQ(de_asset_wait(f->assets, a, 10000000000ull), DE_LOAD_READY);
    ASSERT_EQ(de_asset_wait(f->assets, b, 10000000000ull), DE_LOAD_READY);
    f->clip = de_anim_clip_asset(f->anim, de_asset_id_from("test/marked-walk"));
    de_anim_clip run = de_anim_clip_asset(f->anim, de_asset_id_from("test/marked-run"));
    ASSERT_TRUE(f->clip.id && run.id);
    de_anim_graph graph = sync_graph(f, 1, run);
    de_asset_release(f->assets, a);
    de_asset_release(f->assets, b);
    return graph;
}
static de_anim_graph marked_graph(anim_fixture *f) {
    return marked_graph_variant(f, false);
}
TEST_F(anim_graph_sync, actual_marker_poses_align_different_foot_times_and_keep_leader_phase,
       anim_fixture) {
    de_anim_graph graph = marked_graph(f);
    de_entity entity = sync_actor(f, graph, false, .5f);
    anim_frame(f, 0);
    /* At walk's R=.5, run must sample its R=.15, not normalized time .3. */
    EXPECT_FLOAT_EQ(bone_x(f, entity), .75f, 2e-6f);
    f64 phase = sync_state(f, entity, graph)->clocks[1].phase;
    de_anim_set_float(f->world, entity, de_name_from("blend"), 1);
    anim_frame(f, 0);
    EXPECT_FLOAT_EQ(sync_state(f, entity, graph)->clocks[1].phase, phase, 0);
    EXPECT_FLOAT_EQ(bone_x(f, entity), .5f, 2e-6f);
    DeAnimator *actor = de_entity_get(f->world, entity, f->anim->component);
    actor->playing = true;
    anim_frame(f, .06f);
    EXPECT_FLOAT_EQ(bone_x(f, entity), .7f, 3e-6f);
    actor = de_entity_get(f->world, entity, f->anim->component);
    actor->speed = -1;
    anim_frame(f, .06f);
    EXPECT_FLOAT_EQ(bone_x(f, entity), .5f, 3e-6f);
}
TEST(anim_graph_sync, marked_snapshot_and_parallel_replay_preserve_full_phase_without_allocations) {
    const u32 workers[] = {0, 3, 7};
    u64 reference[16] = {0};
    for (u32 mode = 0; mode < 6; ++mode) {
        anim_fixture f = {0};
        anim_fixture_setup_workers(&f, false, workers[mode % 3]);
        de_anim_graph graph = marked_graph_variant(&f, mode >= 3);
        de_entity actors[96];
        for (u32 i = 0; i < 96; ++i) {
            actors[i] = sync_actor(&f, graph, true, (f32)(i % 7) / 16);
            DeAnimator *actor = de_entity_get(f.world, actors[i], f.anim->component);
            actor->playing = true;
        }
        /* Warm both schedules, transform propagation and query/task scratch. */
        anim_frame(&f, 0);
        sync_tick(&f, .03125f);
        anim_frame(&f, 0);
        (void)de_anim_checksum(f.world);
        de_world_snapshot snapshot = {0};
        ASSERT_EQ(de_world_snapshot_take(f.world, &f.allocator, &snapshot), DE_OK);
        u64 worlds[16];
        for (u32 replay = 0; replay < 2; ++replay) {
            if (replay)
                ASSERT_EQ(de_world_snapshot_restore(f.world, &snapshot), DE_OK);
            de_alloc_guard guard = {0};
            for (u32 tick = 0; tick < 16; ++tick) {
                de_alloc_guard_begin(&guard);
                for (u32 i = 0; i < 96; ++i) {
                    sync_inputs(&f, actors[i], tick + i);
                    DeAnimator *actor = de_entity_get(f.world, actors[i], f.anim->component);
                    actor->speed = tick % 5 == 0 ? -1 : 1;
                    if (replay)
                        actor->deterministic = false;
                }
                if (replay) {
                    anim_frame(&f, .02f);
                    for (u32 i = 0; i < 96; ++i) {
                        DeAnimator *actor = de_entity_get(f.world, actors[i], f.anim->component);
                        actor->deterministic = true;
                    }
                }
                sync_tick(&f, .03125f);
                u64 checksum = de_anim_checksum(f.world);
                if (!mode && !replay)
                    reference[tick] = checksum;
                else
                    EXPECT_EQ(checksum, reference[tick]);
                de_alloc_guard_end(&guard);
                DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
                /* The generic ECS checksum allocates its own verification scratch. */
                u64 world = de_world_checksum(f.world, DE_COMP_SNAPSHOT);
                if (!replay)
                    worlds[tick] = world;
                else
                    EXPECT_EQ(world, worlds[tick]);
            }
        }
        de_world_snapshot_free(&f.allocator, &snapshot);
        anim_fixture_teardown(&f);
    }
}

TEST(anim_graph_sync, reference_selection_skips_orphan_and_prefers_richer_shared_foot_cycle) {
    for (u32 partial = 0; partial < 2; ++partial) {
        anim_fixture f = {0};
        anim_fixture_setup(&f);
        de_anim_graph old = marked_graph_variant(&f, true);
        anim_blob blob = clip_blob(2);
        u32 offset = blob.bytes;
        blob.bytes += 16;
        u8 *bytes = realloc(blob.data, blob.bytes);
        ASSERT_TRUE(bytes);
        memset(bytes + offset, 0, 16);
        de_cooked_put_u32(bytes + 4, DE_COOKED_CLIP_VERSION_MARKERS);
        de_cooked_put_u32(bytes + 52, blob.bytes);
        de_cooked_put_u32(bytes + 64, offset);
        de_cooked_put_u32(bytes + 68, 1);
        de_cooked_put_u64(bytes + offset, de_name_from(partial ? "L" : "solo").hash);
        de_cooked_put_f32(bytes + offset + 8, .25f);
        ASSERT_EQ(de_file_write_all_atomic("animtest:/orphan.dclp", bytes, blob.bytes), DE_OK);
        free(bytes);
        const char *manifest =
            "[assets.\"test/skeleton\"]\ntype='skeleton'\nsource='skeleton.dskl'\n"
            "[assets.\"test/clip\"]\ntype='anim_clip'\nsource='clip.dclp'\n"
            "[assets.\"test/marked-walk\"]\ntype='anim_clip'\nsource='marked-walk.dclp'\n"
            "[assets.\"test/marked-run\"]\ntype='anim_clip'\nsource='marked-run.dclp'\n"
            "[assets.\"test/orphan\"]\ntype='anim_clip'\nsource='orphan.dclp'\n";
        ASSERT_EQ(de_file_write_all_atomic("animtest:/assets.toml", manifest, strlen(manifest)),
                  DE_OK);
        ASSERT_EQ(de_asset_manifest_load(f.assets, "animtest:/assets.toml"), DE_OK);
        de_load_handle load = de_asset_load(f.assets, de_asset_id_from("test/orphan"), 0);
        ASSERT_EQ(de_asset_wait(f.assets, load, 10000000000ull), DE_LOAD_READY);
        de_anim_clip clips[] = {de_anim_clip_asset(f.anim, de_asset_id_from("test/orphan")), f.clip,
                                de_anim_clip_asset(f.anim, de_asset_id_from("test/marked-run"))};
        de_name group = de_name_from("feet");
        de_anim_node_desc nodes[4] = {0};
        for (u32 i = 0; i < 3; ++i)
            nodes[i] = (de_anim_node_desc){
                .kind = DE_ANIM_NODE_CLIP,
                .data.clip = {.clip = clips[i], .speed = 1, .loop = true, .sync_group = group}};
        de_anim_param_desc params[] = {{.name = de_name_from("x"), .float_default = .5f},
                                       {.name = de_name_from("y"), .float_default = .5f}};
        de_anim_blend_point_desc points[] = {{{0, 0}, 0}, {{1, 0}, 1}, {{0, 1}, 2}};
        nodes[3] = (de_anim_node_desc){.kind = DE_ANIM_NODE_BLEND2D,
                                       .data.blend = {.x_param = params[0].name,
                                                      .y_param = params[1].name,
                                                      .points = points,
                                                      .point_count = 3}};
        de_anim_graph graph = {0};
        ASSERT_EQ(de_anim_graph_create(f.anim,
                                       &(de_anim_graph_desc){.params = params,
                                                             .param_count = 2,
                                                             .nodes = nodes,
                                                             .node_count = 4,
                                                             .output_node = 3},
                                       &graph),
                  DE_OK);
        de_asset_release(f.assets, load);
        de_anim_graph_destroy(f.anim, old);
        de_entity entity = sync_actor(&f, graph, false, .5f);
        anim_frame(&f, 0);
        EXPECT_FLOAT_EQ(bone_x(&f, entity), .75f, 3e-6f);
        EXPECT_FLOAT_EQ(sync_state(&f, entity, graph)->clocks[1].phase, .5, 0);
        EXPECT_EQ(f.anim->entries[de_entity_index(entity)].graph_samples, 2u);
        anim_fixture_teardown(&f);
    }
}
TEST_F(anim_graph_sync, common_marker_storage_failure_preserves_previous_runtime_and_recovers,
       anim_fixture) {
    de_anim_graph graph = marked_graph_variant(f, true);
    de_entity entity = sync_actor(f, graph, false, .5f);
    anim_frame(f, 0);
    de__anim_entry *entry = &f->anim->entries[de_entity_index(entity)];
    struct de__anim_graph_runtime *old = entry->graph_runtime;
    ++de__slot_get(f->assets, f->clip.id)->version;
    bool completed = false;
    for (i64 failure = 0; failure < 64; ++failure) {
        de_tracking_allocator_fail_after(&f->tracking, failure);
        bool ok = de__anim_graph_prepare(f->anim, entry);
        de_tracking_allocator_fail_after(&f->tracking, -1);
        de_error_clear();
        if (ok) {
            completed = true;
            EXPECT_GE(failure, 2);
            break;
        }
        EXPECT_TRUE(entry->graph_runtime == old);
        EXPECT_FLOAT_EQ(bone_x(f, entity), .75f, 3e-6f);
    }
    EXPECT_TRUE(completed);
    anim_frame(f, 0);
    EXPECT_FLOAT_EQ(bone_x(f, entity), .75f, 3e-6f);
}
