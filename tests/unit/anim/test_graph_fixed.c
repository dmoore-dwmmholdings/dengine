#include "anim_fixture.h"
#include "anim/graph_internal.h"
#include "de_alloc_guard.h"

typedef struct fixed_graph_desc {
    de_anim_param_desc params[5];
    de_anim_node_desc nodes[6];
    de_anim_blend_point_desc points[3];
    de_anim_bone_mask_desc mask;
    de_anim_layer_desc layers[2];
    de_anim_graph_desc desc;
} fixed_graph_desc;
static void fixed_desc(anim_fixture *f, fixed_graph_desc *d) {
    memset(d, 0, sizeof *d);
    d->params[0] = (de_anim_param_desc){.name = de_name_from("x"), .float_default = .25f};
    d->params[1] = (de_anim_param_desc){.name = de_name_from("y"), .float_default = .25f};
    d->params[2] = (de_anim_param_desc){.name = de_name_from("add"), .float_default = .25f};
    d->params[3] = (de_anim_param_desc){
        .name = de_name_from("armed"), .kind = DE_ANIM_PARAM_BOOL, .bool_default = true};
    d->params[4] =
        (de_anim_param_desc){.name = de_name_from("fire"), .kind = DE_ANIM_PARAM_TRIGGER};
    const f32 speeds[] = {1, 2, .5f};
    for (u32 i = 0; i < 3; ++i) {
        d->nodes[i] =
            (de_anim_node_desc){.kind = DE_ANIM_NODE_CLIP,
                                .data.clip = {.clip = f->clip, .speed = speeds[i], .loop = true}};
        d->points[i] = (de_anim_blend_point_desc){{i == 1 ? 1.f : 0.f, i == 2 ? 1.f : 0.f}, i};
    }
    d->nodes[3] = (de_anim_node_desc){.kind = DE_ANIM_NODE_BLEND2D,
                                      .data.blend = {.x_param = d->params[0].name,
                                                     .y_param = d->params[1].name,
                                                     .points = d->points,
                                                     .point_count = 3}};
    d->nodes[4] = (de_anim_node_desc){
        .kind = DE_ANIM_NODE_ADDITIVE,
        .data.additive = {.base = 3, .input = 1, .weight_param = d->params[2].name}};
    d->mask = (de_anim_bone_mask_desc){de_name_from("joint1"), 1, true};
    d->layers[0] = (de_anim_layer_desc){
        .node = 2, .weight = .5f, .root_weight = 1, .mask = &d->mask, .mask_count = 1};
    d->layers[1] = (de_anim_layer_desc){
        .node = 1, .mode = DE_ANIM_LAYER_ADDITIVE, .weight = .1f, .root_weight = 1};
    d->nodes[5] = (de_anim_node_desc){.kind = DE_ANIM_NODE_LAYER,
                                      .data.layer = {.base = 4, .layer_count = 2}};
    d->desc = (de_anim_graph_desc){.params = d->params,
                                   .param_count = 5,
                                   .nodes = d->nodes,
                                   .node_count = 6,
                                   .output_node = 5,
                                   .layers = d->layers,
                                   .layer_count = 2};
}
static de_anim_graph create_fixed_graph(anim_fixture *f) {
    fixed_graph_desc d;
    fixed_desc(f, &d);
    de_anim_graph g = {0};
    ASSERT_EQ(de_anim_graph_create(f->anim, &d.desc, &g), DE_OK);
    return g;
}
static de_entity fixed_actor(anim_fixture *f, de_anim_graph g, bool bind) {
    de_entity entity = spawn_animator(f);
    if (bind)
        ASSERT_EQ(de_anim_graph_bind(f->world, entity, g), DE_OK);
    DeAnimator *a = de_entity_get(f->world, entity, f->anim->component);
    a->graph = g;
    a->deterministic = a->playing = true;
    a->speed = 1;
    return entity;
}
static void graph_tick(anim_fixture *f, f32 dt) {
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_FIXED_UPDATE, dt, 0), DE_OK);
}
TEST(anim_graph_fixed, unbounded_clock_precision_pause_reverse_seek_and_invalid_wrap) {
    de_fp_env_init_thread();
    de__anim_clock c = {0};
    DeAnimator a = {.playing = true, .speed = 1};
    f32 dt = 1.f / 60.f;
    for (u32 i = 1; i <= 10000; ++i) {
        de__anim_graph_clock_step(&c, &a, dt, 77, false);
        EXPECT_EQ(c.ticks, i);
        EXPECT_TRUE(c.exact_time == (f64)i * (f64)dt);
    }
    f64 paused = c.exact_time;
    a.playing = false;
    de__anim_graph_clock_step(&c, &a, dt, 77, false);
    EXPECT_TRUE(c.exact_time == paused);
    a.playing = true;
    a.speed = -2;
    de__anim_graph_clock_step(&c, &a, .125f, 77, false);
    EXPECT_TRUE(c.exact_time == paused - .25);
    a.time = -.5f;
    de__anim_graph_clock_step(&c, &a, .125f, 77, true);
    EXPECT_TRUE(c.exact_time == -.75);
    EXPECT_FLOAT_EQ(de__anim_wrap_time(c.exact_time, 1), .25, 0);
    c = (de__anim_clock){.initialized = true,
                         .duration = -1,
                         .clip_key = 77,
                         .dt = .125f,
                         .speed = 1,
                         .segment_ticks = UINT64_C(1) << 36};
    a = (DeAnimator){.playing = true, .speed = 1};
    de__anim_graph_clock_step(&c, &a, .125f, 77, false);
    EXPECT_FLOAT_EQ(de__anim_wrap_time(c.exact_time, 1), .125, 0);
    EXPECT_TRUE((f64)a.time != c.exact_time); /* Must wrap before narrowing to f32. */
    EXPECT_FLOAT_EQ(de__anim_wrap_time(1, 0), 0, 0);
    EXPECT_FLOAT_EQ(de__anim_wrap_time(1, -1), 0, 0);
    EXPECT_FLOAT_EQ(de__anim_wrap_time(INFINITY, 1), 0, 0);
    EXPECT_FLOAT_EQ(de__anim_wrap_time(1, NAN), 0, 0);
}
TEST_F(anim_graph_fixed, first_fixed_pose_direct_defaults_pause_and_render_cadence, anim_fixture) {
    load_assets(f, 3);
    de_anim_graph g = create_fixed_graph(f);
    de_entity entity = fixed_actor(f, g, false);
    graph_tick(f, .125f);
    de__anim_entry *e = &f->anim->entries[de_entity_index(entity)];
    ASSERT_TRUE(e->ready && e->finalized);
    EXPECT_EQ(e->graph_samples, 3u);
    EXPECT_FLOAT_EQ(e->local.translations[0].x, .45625f, 1e-6f);
    de_transform bone;
    ASSERT_TRUE(de_anim_bone_transform(f->world, entity, de_name_from("joint0"), &bone));
    EXPECT_FLOAT_EQ(bone.position.x, .45625, 1e-6);
    f32 x = 0;
    ASSERT_TRUE(de_anim_get_float(f->world, entity, de_name_from("x"), &x));
    EXPECT_FLOAT_EQ(x, .25f, 0);
    u64 hash = de_anim_checksum(f->world);
    for (u32 i = 0; i < 5; ++i)
        anim_frame(f, .37f);
    EXPECT_EQ(de_anim_checksum(f->world), hash);
    DeAnimator *a = de_entity_get(f->world, entity, f->anim->component);
    a->playing = false;
    u64 pose = de__anim_pose_checksum(&e->local);
    graph_tick(f, .125f);
    EXPECT_EQ(de__anim_pose_checksum(&e->local), pose);
    a = de_entity_get(f->world, entity, f->anim->component);
    a->playing = true;
    a->speed = -1;
    graph_tick(f, .0625f);
    EXPECT_FLOAT_EQ(a->time, .0625f, 0);
}
static void graph_inputs(anim_fixture *f, de_entity e, u32 tick) {
    de_anim_set_float(f->world, e, de_name_from("x"), (f32)(tick % 4) * .125f);
    de_anim_set_float(f->world, e, de_name_from("y"), (f32)(tick % 3) * .125f);
    de_anim_set_bool(f->world, e, de_name_from("armed"), (tick & 1) != 0);
    if (tick % 4 == 0)
        de_anim_trigger(f->world, e, de_name_from("fire"));
    else
        (void)de_anim_consume_trigger(f->world, e, de_name_from("fire"));
}
TEST_F(anim_graph_fixed, rollback_replays_full_world_and_animation_checksums_across_visual_frames,
       anim_fixture) {
    load_assets(f, 3);
    de_entity entity = fixed_actor(f, create_fixed_graph(f), true);
    graph_tick(f, .125f);
    u64 initial = de_anim_checksum(f->world);
    u64 world = de_world_checksum(f->world, DE_COMP_SNAPSHOT);
    de_world_snapshot snap = {0};
    ASSERT_EQ(de_world_snapshot_take(f->world, &f->allocator, &snap), DE_OK);
    u64 anims[24], worlds[24];
    for (u32 i = 0; i < 24; ++i) {
        graph_inputs(f, entity, i);
        graph_tick(f, .03125f);
        anims[i] = de_anim_checksum(f->world);
        worlds[i] = de_world_checksum(f->world, DE_COMP_SNAPSHOT);
    }
    ASSERT_EQ(de_world_snapshot_restore(f->world, &snap), DE_OK);
    EXPECT_EQ(de_anim_checksum(f->world), initial);
    EXPECT_EQ(de_world_checksum(f->world, DE_COMP_SNAPSHOT), world);
    de_transform bone;
    EXPECT_FALSE(de_anim_bone_transform(f->world, entity, de_name_from("joint0"), &bone));
    for (u32 i = 0; i < 24; ++i) {
        graph_inputs(f, entity, i);
        DeAnimator *a = de_entity_get(f->world, entity, f->anim->component);
        a->deterministic = false;
        anim_frame(f, .017f * (f32)(i % 3 + 1));
        a = de_entity_get(f->world, entity, f->anim->component);
        a->deterministic = true;
        graph_tick(f, .03125f);
        EXPECT_EQ(de_anim_checksum(f->world), anims[i]);
        EXPECT_EQ(de_world_checksum(f->world, DE_COMP_SNAPSHOT), worlds[i]);
    }
    de_world_snapshot_free(&f->allocator, &snap);
}
TEST_F(anim_graph_fixed, checksum_covers_pending_parameters_and_logical_graph_not_handle_ids,
       anim_fixture) {
    load_assets(f, 3);
    de_anim_graph g = create_fixed_graph(f), clone = create_fixed_graph(f);
    ASSERT_NE(g.id, clone.id);
    EXPECT_EQ(de__anim_graph_get(f->anim, g)->definition_hash,
              de__anim_graph_get(f->anim, clone)->definition_hash);
    de_entity entity = fixed_actor(f, g, true);
    graph_tick(f, .125f);
    u64 base = de_anim_checksum(f->world);
    de_anim_set_bool(f->world, entity, de_name_from("armed"), false);
    EXPECT_NE(de_anim_checksum(f->world), base);
    de_anim_set_bool(f->world, entity, de_name_from("armed"), true);
    EXPECT_EQ(de_anim_checksum(f->world), base);
    de_anim_trigger(f->world, entity, de_name_from("fire"));
    EXPECT_NE(de_anim_checksum(f->world), base);
    EXPECT_TRUE(de_anim_consume_trigger(f->world, entity, de_name_from("fire")));
    EXPECT_EQ(de_anim_checksum(f->world), base);
    de_anim_set_float(f->world, entity, de_name_from("x"), .5f);
    EXPECT_NE(de_anim_checksum(f->world), base);
    de_anim_set_float(f->world, entity, de_name_from("x"), .25f);
    EXPECT_EQ(de_anim_checksum(f->world), base);
    ASSERT_EQ(de_anim_graph_bind(f->world, entity, clone), DE_OK);
    EXPECT_EQ(de_anim_checksum(f->world), base);
    fixed_graph_desc changed;
    fixed_desc(f, &changed);
    changed.nodes[0].data.clip.speed = 1.5f;
    de_anim_graph other = {0};
    ASSERT_EQ(de_anim_graph_create(f->anim, &changed.desc, &other), DE_OK);
    ASSERT_EQ(de_anim_graph_bind(f->world, entity, other), DE_OK);
    EXPECT_NE(de_anim_checksum(f->world), base);
}
static void worker_run(u32 workers, u64 hashes[32]) {
    anim_fixture f = {0};
    anim_fixture_setup_workers(&f, false, workers);
    EXPECT_EQ(f.tasks ? de_task_worker_count(f.tasks) : 1, workers ? workers + 1 : 1);
    load_assets(&f, 3);
    de_anim_graph g = create_fixed_graph(&f);
    de_entity actors[96];
    for (u32 i = 0; i < 96; ++i) {
        actors[i] = fixed_actor(&f, g, true);
        DeAnimator *a = de_entity_get(f.world, actors[i], f.anim->component);
        a->time = (f32)i * .00390625f;
        a->speed = i & 1 ? -.5f : 1.25f;
        graph_inputs(&f, actors[i], i);
    }
    graph_tick(&f, .015625f);
    graph_tick(&f, .015625f);
    (void)de_anim_checksum(f.world);
    de_alloc_guard guard = {0};
    de_alloc_guard_begin(&guard);
    for (u32 tick = 0; tick < 32; ++tick) {
        for (u32 i = 0; i < 96; ++i)
            graph_inputs(&f, actors[i], tick + i);
        graph_tick(&f, .015625f);
        hashes[tick] = de_anim_checksum(f.world);
    }
    de_alloc_guard_end(&guard);
    DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
    anim_fixture_teardown(&f);
}
TEST(anim_graph_fixed, serial_four_eight_workers_match_each_tick_without_warmed_allocations) {
    u64 serial[32], four[32], eight[32];
    worker_run(0, serial);
    worker_run(3, four);
    worker_run(7, eight);
    EXPECT_MEMEQ(serial, four, sizeof serial);
    EXPECT_MEMEQ(serial, eight, sizeof serial);
}
TEST_F(anim_graph_fixed, long_uptime_leaf_wraps_double_clock_before_float_conversion,
       anim_fixture) {
    load_assets(f, 3);
    de_anim_node_desc node = {.kind = DE_ANIM_NODE_CLIP,
                              .data.clip = {.clip = f->clip, .speed = 1, .loop = true}};
    de_anim_graph g = {0};
    ASSERT_EQ(
        de_anim_graph_create(f->anim, &(de_anim_graph_desc){.nodes = &node, .node_count = 1}, &g),
        DE_OK);
    de_entity entity = fixed_actor(f, g, true);
    graph_tick(f, .125f);
    de__anim_clock *c = de_entity_get(f->world, entity, f->anim->clock_component);
    DeAnimator *a = de_entity_get(f->world, entity, f->anim->component);
    c->phase = 0;
    c->segment_ticks = UINT64_C(1) << 36;
    c->exact_time = (f64)c->segment_ticks * .125;
    c->output_time = a->time = (f32)c->exact_time;
    graph_tick(f, .125f);
    de__anim_entry *e = &f->anim->entries[de_entity_index(entity)];
    ASSERT_TRUE(e->ready && e->finalized);
    EXPECT_FLOAT_EQ(e->local.translations[0].x, .25f, 1e-6f);
    EXPECT_EQ(e->graph_samples, 1u);
    EXPECT_FLOAT_EQ(de__anim_wrap_time(c->exact_time, 1), .125, 0);
}
TEST_F(anim_graph_fixed, snapshot_before_first_clock_rejects_stale_graph_pose, anim_fixture) {
    load_assets(f, 3);
    de_entity entity = fixed_actor(f, create_fixed_graph(f), true);
    de_world_snapshot snapshot = {0};
    ASSERT_EQ(de_world_snapshot_take(f->world, &f->allocator, &snapshot), DE_OK);
    graph_tick(f, .125f);
    de_transform bone;
    ASSERT_TRUE(de_anim_bone_transform(f->world, entity, de_name_from("joint0"), &bone));
    ASSERT_EQ(de_world_snapshot_restore(f->world, &snapshot), DE_OK);
    EXPECT_FALSE(de_anim_bone_transform(f->world, entity, de_name_from("joint0"), &bone));
    graph_tick(f, .125f);
    ASSERT_TRUE(de_anim_bone_transform(f->world, entity, de_name_from("joint0"), &bone));
    EXPECT_FLOAT_EQ(bone.position.x, .45625, 1e-6);
    de_world_snapshot_free(&f->allocator, &snapshot);
}
