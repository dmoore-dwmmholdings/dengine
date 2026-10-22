#include "anim_fixture.h"
#include "anim/graph_internal.h"
#include "de_alloc_guard.h"
#include "asset/slots_internal.h"

static de__anim_entry *actor_entry(anim_fixture *f, de_entity e) {
    u32 i = de_entity_index(e);
    ASSERT_LT(i, f->anim->capacity);
    return &f->anim->entries[i];
}
static de_anim_graph build_graph(anim_fixture *f, de_anim_node_desc *nodes, u32 count,
                                 const de_anim_param_desc *params, u32 param_count,
                                 const de_anim_layer_desc *layers, u32 layer_count) {
    de_anim_graph g = {0};
    ASSERT_EQ(de_anim_graph_create(f->anim,
                                   &(de_anim_graph_desc){.nodes = nodes,
                                                         .node_count = count,
                                                         .output_node = count - 1,
                                                         .params = params,
                                                         .param_count = param_count,
                                                         .layers = layers,
                                                         .layer_count = layer_count},
                                   &g),
              DE_OK);
    return g;
}
static void clips(anim_fixture *f, de_anim_node_desc *nodes, u32 count) {
    for (u32 i = 0; i < count; ++i)
        nodes[i] =
            (de_anim_node_desc){.kind = DE_ANIM_NODE_CLIP,
                                .data.clip = {.clip = f->clip, .speed = (f32)i + 1, .loop = true}};
}
static void bind_at(anim_fixture *f, de_entity e, de_anim_graph g, f32 time) {
    ASSERT_EQ(de_anim_graph_bind(f->world, e, g), DE_OK);
    DeAnimator *a = de_entity_get(f->world, e, de_anim_component(f->anim));
    a->time = time;
    a->playing = false;
}
TEST_F(anim_graph_runtime, visual_1d_weights_endpoints_threshold_and_bone_publication,
       anim_fixture) {
    load_assets(f, 3);
    de_anim_param_desc param = {.name = de_name_from("speed"), .float_default = 2};
    de_anim_node_desc nodes[3];
    clips(f, nodes, 2);
    de_anim_blend_point_desc points[] = {{{0, 0}, 0}, {{4, 0}, 1}};
    nodes[2] = (de_anim_node_desc){
        .kind = DE_ANIM_NODE_BLEND1D,
        .data.blend = {.x_param = param.name, .points = points, .point_count = 2}};
    de_anim_graph g = build_graph(f, nodes, 3, &param, 1, NULL, 0);
    de_entity e = spawn_animator(f);
    bind_at(f, e, g, .25f);
    anim_frame(f, 0);
    de__anim_entry *entry = actor_entry(f, e);
    ASSERT_TRUE(entry->ready && entry->finalized);
    EXPECT_EQ(entry->graph_samples, 2u);
    EXPECT_FLOAT_EQ(entry->local.translations[0].x, .75f, 1e-6f);
    de_transform bone;
    ASSERT_TRUE(de_anim_bone_transform(f->world, e, de_name_from("joint0"), &bone));
    EXPECT_FLOAT_EQ(bone.position.x, .75, 1e-6);
    de_anim_set_float(f->world, e, param.name, -1);
    anim_frame(f, 0);
    EXPECT_EQ(entry->graph_samples, 1u);
    EXPECT_FLOAT_EQ(entry->local.translations[0].x, .5f, 1e-6f);
    de_anim_set_float(f->world, e, param.name, 10);
    anim_frame(f, 0);
    EXPECT_EQ(entry->graph_samples, 1u);
    EXPECT_FLOAT_EQ(entry->local.translations[0].x, 1, 1e-6f);
    de_anim_set_float(f->world, e, param.name, .0002f); /* run contribution=5e-5 */
    anim_frame(f, 0);
    EXPECT_EQ(entry->graph_samples, 1u);
    EXPECT_FLOAT_EQ(entry->local.translations[0].x, .5f, 1e-6f);
}
TEST_F(anim_graph_runtime, visual_2d_barycentric_and_outside_hull, anim_fixture) {
    load_assets(f, 2);
    de_anim_param_desc params[] = {{.name = de_name_from("x"), .float_default = .25f},
                                   {.name = de_name_from("y"), .float_default = .25f}};
    de_anim_node_desc nodes[4];
    clips(f, nodes, 3);
    de_anim_blend_point_desc points[] = {{{0, 0}, 0}, {{1, 0}, 1}, {{0, 1}, 2}};
    nodes[3] = (de_anim_node_desc){.kind = DE_ANIM_NODE_BLEND2D,
                                   .data.blend = {.x_param = params[0].name,
                                                  .y_param = params[1].name,
                                                  .points = points,
                                                  .point_count = 3}};
    de_anim_graph g = build_graph(f, nodes, 4, params, 2, NULL, 0);
    de_entity e = spawn_animator(f);
    bind_at(f, e, g, .2f);
    anim_frame(f, 0);
    de__anim_entry *entry = actor_entry(f, e);
    ASSERT_TRUE(entry->ready);
    EXPECT_EQ(entry->graph_samples, 3u);
    EXPECT_FLOAT_EQ(entry->local.translations[0].x, .7f, 1e-6f);
    de_anim_set_float(f->world, e, params[0].name, 2);
    de_anim_set_float(f->world, e, params[1].name, 0);
    anim_frame(f, 0);
    EXPECT_EQ(entry->graph_samples, 1u);
    EXPECT_FLOAT_EQ(entry->local.translations[0].x, .8f, 1e-6f);
}
TEST_F(anim_graph_runtime, visual_additive_and_ordered_masked_layer_stack, anim_fixture) {
    load_assets(f, 3);
    de_anim_param_desc param = {.name = de_name_from("add"), .float_default = .5f};
    de_anim_node_desc nodes[4];
    clips(f, nodes, 2);
    nodes[2] =
        (de_anim_node_desc){.kind = DE_ANIM_NODE_ADDITIVE,
                            .data.additive = {.base = 0, .input = 1, .weight_param = param.name}};
    de_anim_bone_mask_desc mask = {de_name_from("joint0"), .5f, false};
    de_anim_layer_desc layers[] = {
        {.node = 1, .weight = .5f, .root_weight = 1, .mask = &mask, .mask_count = 1},
        {.node = 1, .weight = .25f, .root_weight = 1, .mode = DE_ANIM_LAYER_ADDITIVE}};
    nodes[3] = (de_anim_node_desc){.kind = DE_ANIM_NODE_LAYER,
                                   .data.layer = {.base = 2, .layer_count = 2}};
    de_anim_graph g = build_graph(f, nodes, 4, &param, 1, layers, 2);
    de_entity e = spawn_animator(f);
    bind_at(f, e, g, .25f);
    anim_frame(f, 0);
    de__anim_entry *entry = actor_entry(f, e);
    ASSERT_TRUE(entry->ready);
    EXPECT_EQ(entry->graph_samples, 2u); /* Shared input sampled once. */
    EXPECT_FLOAT_EQ(entry->local.translations[0].x, 1.25f, 1e-6f);
    de_cooked_joint_pose rest = de_cooked_skeleton_rest(&entry->skeleton, 1);
    EXPECT_MEMEQ(&entry->local.translations[1], rest.t, sizeof(de_vec3));
    /* Remove the layers via another graph and require exact zero-additive identity. */
    g = build_graph(f, nodes, 3, &param, 1, NULL, 0);
    bind_at(f, e, g, .25f);
    de_anim_set_float(f->world, e, param.name, 0);
    anim_frame(f, 0);
    entry = actor_entry(f, e);
    EXPECT_EQ(entry->graph_samples, 1u);
    de_cooked_joint_pose expected[3];
    const de__anim_asset *clip = de__anim_asset_get(f->anim, f->clip.id, "anim_clip");
    ASSERT_TRUE(de__anim_sample(&clip->clip, &entry->skeleton, .25f, expected));
    EXPECT_MEMEQ(&entry->local.translations[0], expected[0].t, sizeof(de_vec3));
    EXPECT_MEMEQ(&entry->local.rotations[0], expected[0].r, sizeof(de_quat));
}
TEST_F(anim_graph_runtime, direct_assignment_defaults_warmed_no_alloc_rebind_and_destroy,
       anim_fixture) {
    load_assets(f, 2);
    de_anim_param_desc param = {.name = de_name_from("speed"), .float_default = 2};
    de_anim_node_desc nodes[3];
    clips(f, nodes, 2);
    de_anim_blend_point_desc points[] = {{{0, 0}, 0}, {{4, 0}, 1}};
    nodes[2] = (de_anim_node_desc){
        .kind = DE_ANIM_NODE_BLEND1D,
        .data.blend = {.x_param = param.name, .points = points, .point_count = 2}};
    de_anim_graph g = build_graph(f, nodes, 3, &param, 1, NULL, 0);
    de_entity e = spawn_animator(f);
    DeAnimator *a = de_entity_get(f->world, e, de_anim_component(f->anim));
    a->graph = g;
    a->time = .25f;
    a->playing = false;
    anim_frame(f, 0);
    f32 value = 0;
    ASSERT_TRUE(de_anim_get_float(f->world, e, param.name, &value));
    EXPECT_FLOAT_EQ(value, 2, 0);
    EXPECT_FLOAT_EQ(actor_entry(f, e)->local.translations[0].x, .75f, 1e-6f);
    anim_frame(f, 0);
    de_alloc_guard guard = {0};
    de_alloc_guard_begin(&guard);
    for (u32 i = 0; i < 128; ++i) {
        de_anim_set_float(f->world, e, param.name, (f32)(i % 5));
        anim_frame(f, 0);
    }
    de_alloc_guard_end(&guard);
    DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
    de_anim_graph_destroy(f->anim, g);
    EXPECT_TRUE(actor_entry(f, e)->graph_runtime == NULL);
    anim_frame(f, 0);
    EXPECT_FALSE(actor_entry(f, e)->ready);
    EXPECT_EQ(actor_entry(f, e)->graph_samples, 0u);
    ASSERT_EQ(de_anim_graph_bind(f->world, e, (de_anim_graph){0}), DE_OK);
    anim_frame(f, 0);
    EXPECT_TRUE(actor_entry(f, e)->ready);
    EXPECT_TRUE(actor_entry(f, e)->graph_runtime == NULL);
    de_entity_despawn(f->world, e);
    anim_frame(f, 0);
    EXPECT_EQ(actor_entry(f, e)->entity.bits, 0u);
}

TEST(anim_graph_runtime, failed_cold_binding_unwinds_every_owned_allocation) {
    bool completed = false;
    for (i64 failure = 0; failure < 32; ++failure) {
        anim_fixture f = {0};
        anim_fixture_setup(&f);
        load_assets(&f, 3);
        de_anim_node_desc nodes[4];
        clips(&f, nodes, 2);
        nodes[2] = (de_anim_node_desc){.kind = DE_ANIM_NODE_ADDITIVE,
                                       .data.additive = {.base = 0, .input = 1, .weight = .5f}};
        de_anim_bone_mask_desc mask = {de_name_from("joint1"), 1, true};
        de_anim_layer_desc layer = {.node = 1,
                                    .mode = DE_ANIM_LAYER_ADDITIVE,
                                    .weight = .5f,
                                    .mask = &mask,
                                    .mask_count = 1};
        nodes[3] = (de_anim_node_desc){.kind = DE_ANIM_NODE_LAYER,
                                       .data.layer = {.base = 2, .layer_count = 1}};
        de_anim_graph g = build_graph(&f, nodes, 4, NULL, 0, &layer, 1);
        de_entity entity = spawn_animator(&f);
        bind_at(&f, entity, g, .25f);
        const de__anim_asset *skel = de__anim_asset_get(f.anim, f.skeleton.id, "skeleton");
        de__anim_entry e = {.skeleton_handle = f.skeleton.id,
                            .entity = entity,
                            .joints = 3,
                            .skeleton = skel->skeleton,
                            .animator = de_entity_get(f.world, entity, de_anim_component(f.anim))};
        de_tracking_allocator_fail_after(&f.tracking, failure);
        bool ok = de__anim_graph_prepare(f.anim, &e);
        de_tracking_allocator_fail_after(&f.tracking, -1);
        if (!ok)
            EXPECT_TRUE(e.graph_runtime == NULL);
        de__anim_graph_runtime_release(f.anim, &e);
        anim_fixture_teardown(&f); /* Every pool, temporary mask rule and graph must drain. */
        if (ok) {
            completed = true;
            EXPECT_GT(failure, 0);
            break;
        }
    }
    EXPECT_TRUE(completed);
}
TEST_F(anim_graph_runtime, shared_graph_serial_parallel_pose_parity_and_zero_alloc, anim_fixture) {
    load_assets(f, 4);
    de_anim_param_desc param = {.name = de_name_from("speed"), .float_default = 2};
    de_anim_node_desc nodes[3];
    clips(f, nodes, 2);
    de_anim_blend_point_desc points[] = {{{0, 0}, 0}, {{4, 0}, 1}};
    nodes[2] = (de_anim_node_desc){
        .kind = DE_ANIM_NODE_BLEND1D,
        .data.blend = {.x_param = param.name, .points = points, .point_count = 2}};
    de_anim_graph g = build_graph(f, nodes, 3, &param, 1, NULL, 0);
    enum { COUNT = 96 };
    de_entity entities[COUNT];
    u64 hashes[COUNT];
    for (u32 i = 0; i < COUNT; ++i) {
        entities[i] = spawn_animator(f);
        bind_at(f, entities[i], g, .1f + .003f * (f32)i);
        de_anim_set_float(f->world, entities[i], param.name, (f32)(i % 5));
    }
    de_anim_set_task_system(f->anim, NULL);
    anim_frame(f, 0);
    for (u32 i = 0; i < COUNT; ++i) {
        de__anim_entry *e = actor_entry(f, entities[i]);
        ASSERT_TRUE(e->ready && e->finalized);
        hashes[i] = de__anim_pose_checksum(&e->local);
    }
    de_anim_set_task_system(f->anim, f->tasks);
    anim_frame(f, 0);
    anim_frame(f, 0);
    de_alloc_guard guard = {0};
    de_alloc_guard_begin(&guard);
    for (u32 frame = 0; frame < 20; ++frame) {
        de_anim_set_task_system(f->anim, frame & 1 ? NULL : f->tasks);
        anim_frame(f, 0);
        for (u32 i = 0; i < COUNT; ++i) {
            de__anim_entry *e = actor_entry(f, entities[i]);
            EXPECT_TRUE(e->ready && e->finalized);
            EXPECT_EQ(de__anim_pose_checksum(&e->local), hashes[i]);
        }
    }
    de_alloc_guard_end(&guard);
    DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
}

TEST_F(anim_graph_runtime, clip_skeleton_revisions_rebind_atomically_and_visual_snapshot_restores,
       anim_fixture) {
    load_assets(f, 2);
    de_anim_node_desc node;
    clips(f, &node, 1);
    de_anim_graph g = build_graph(f, &node, 1, NULL, 0, NULL, 0);
    de_entity entity = spawn_animator(f);
    bind_at(f, entity, g, .25f);
    anim_frame(f, 0);
    de__anim_entry *e = actor_entry(f, entity);
    ASSERT_TRUE(e->ready);
    struct de__anim_graph_runtime *old = e->graph_runtime;
    ++de__slot_get(f->assets, f->clip.id)->version;
    de_tracking_allocator_fail_after(&f->tracking, 0);
    bool ok = de__anim_graph_prepare(f->anim, e);
    de_tracking_allocator_fail_after(&f->tracking, -1);
    EXPECT_FALSE(ok);
    EXPECT_TRUE(e->graph_runtime == old);
    ASSERT_TRUE(de__anim_graph_prepare(f->anim, e));
    EXPECT_TRUE(e->graph_runtime != old);
    old = e->graph_runtime;
    ++de__slot_get(f->assets, f->skeleton.id)->version;
    ASSERT_TRUE(de__anim_graph_prepare(f->anim, e));
    EXPECT_TRUE(e->graph_runtime != old);
    anim_frame(f, 0);
    u64 hash = de__anim_pose_checksum(&e->local);
    de_world_snapshot snapshot = {0};
    ASSERT_EQ(de_world_snapshot_take(f->world, &f->allocator, &snapshot), DE_OK);
    DeAnimator *a = de_entity_get(f->world, entity, de_anim_component(f->anim));
    a->playing = true;
    a->speed = 1;
    anim_frame(f, .125f);
    EXPECT_TRUE(de__anim_pose_checksum(&e->local) != hash);
    ASSERT_EQ(de_world_snapshot_restore(f->world, &snapshot), DE_OK);
    anim_frame(f, 0);
    EXPECT_EQ(de__anim_pose_checksum(&actor_entry(f, entity)->local), hash);
    de_world_snapshot_free(&f->allocator, &snapshot);
}

TEST_F(anim_graph_runtime, visual_graph_handoff_does_not_advance_existing_fixed_clip_clock,
       anim_fixture) {
    load_assets(f, 2);
    de_anim_node_desc node;
    clips(f, &node, 1);
    de_anim_graph g = build_graph(f, &node, 1, NULL, 0, NULL, 0);
    de_entity entity = spawn_animator(f);
    DeAnimator *a = de_entity_get(f->world, entity, de_anim_component(f->anim));
    a->deterministic = true;
    a->playing = true;
    a->speed = 1;
    a->loop = true;
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_FIXED_UPDATE, .125f, 0), DE_OK);
    ASSERT_EQ(de_anim_graph_bind(f->world, entity, g), DE_OK);
    a = de_entity_get(f->world, entity, de_anim_component(f->anim));
    a->deterministic = false;
    anim_frame(f, .25f);
    a = de_entity_get(f->world, entity, de_anim_component(f->anim));
    EXPECT_FLOAT_EQ(a->time, .375f, 0);
    ASSERT_EQ(de_anim_graph_bind(f->world, entity, (de_anim_graph){0}), DE_OK);
    a = de_entity_get(f->world, entity, de_anim_component(f->anim));
    a->deterministic = true;
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_FIXED_UPDATE, .125f, 0), DE_OK);
    a = de_entity_get(f->world, entity, de_anim_component(f->anim));
    EXPECT_FLOAT_EQ(a->time, .25f, 0); /* Two fixed ticks; intervening graph frame is visual. */
}
