#include "anim_fixture.h"
#include "anim/graph_internal.h"
#include "de_alloc_guard.h"

static de_anim_graph parameter_graph(anim_fixture *f, f32 speed) {
    const de_anim_param_desc params[] = {
        {.name = de_name_from("speed"), .kind = DE_ANIM_PARAM_FLOAT, .float_default = speed},
        {.name = de_name_from("armed"), .kind = DE_ANIM_PARAM_BOOL, .bool_default = true},
        {.name = de_name_from("fire"), .kind = DE_ANIM_PARAM_TRIGGER}};
    const de_anim_node_desc node = {.kind = DE_ANIM_NODE_CLIP,
                                    .data.clip = {.clip = f->clip, .speed = 1, .loop = true}};
    de_anim_graph graph = {0};
    ASSERT_EQ(de_anim_graph_create(
                  f->anim,
                  &(de_anim_graph_desc){
                      .params = params, .param_count = 3, .nodes = &node, .node_count = 1},
                  &graph),
              DE_OK);
    return graph;
}
TEST_F(anim_params, defaults_typed_access_trigger_latch_rebind_and_unbind, anim_fixture) {
    load_assets(f, 2);
    de_anim_graph graph = parameter_graph(f, 2), other = parameter_graph(f, 4);
    de_entity entity = spawn_animator(f);
    ASSERT_EQ(de_anim_graph_bind(f->world, entity, graph), DE_OK);
    f32 speed = 99;
    bool armed = false;
    ASSERT_TRUE(de_anim_get_float(f->world, entity, de_name_from("speed"), &speed));
    ASSERT_TRUE(de_anim_get_bool(f->world, entity, de_name_from("armed"), &armed));
    EXPECT_TRUE(speed == 2 && armed);
    de_anim_set_float(f->world, entity, de_name_from("speed"), 3);
    de_anim_set_bool(f->world, entity, de_name_from("armed"), false);
    de_anim_trigger(f->world, entity, de_name_from("fire"));
    de_anim_trigger(f->world, entity, de_name_from("fire"));
    EXPECT_TRUE(de_anim_consume_trigger(f->world, entity, de_name_from("fire")));
    EXPECT_FALSE(de_anim_consume_trigger(f->world, entity, de_name_from("fire")));
    ASSERT_EQ(de_anim_graph_bind(f->world, entity, graph), DE_OK);
    ASSERT_TRUE(de_anim_get_float(f->world, entity, de_name_from("speed"), &speed));
    EXPECT_TRUE(speed == 3); /* Same binding preserves caller values. */
    de_anim_set_float(f->world, entity, de_name_from("speed"), NAN);
    ASSERT_TRUE(de_anim_get_float(f->world, entity, de_name_from("speed"), &speed));
    EXPECT_TRUE(speed == 3);
    EXPECT_FALSE(de_anim_get_float(f->world, entity, de_name_from("armed"), &speed));
    EXPECT_TRUE(speed == 3);
    ASSERT_EQ(de_anim_graph_bind(f->world, entity, other), DE_OK);
    ASSERT_TRUE(de_anim_get_float(f->world, entity, de_name_from("speed"), &speed));
    ASSERT_TRUE(de_anim_get_bool(f->world, entity, de_name_from("armed"), &armed));
    EXPECT_TRUE(speed == 4 && armed);
    ASSERT_EQ(de_anim_graph_bind(f->world, entity, (de_anim_graph){0}), DE_OK);
    EXPECT_FALSE(de_anim_get_float(f->world, entity, de_name_from("speed"), &speed));
    EXPECT_TRUE(speed == 4);
    const DeAnimator *a = de_entity_get_const(f->world, entity, de_anim_component(f->anim));
    EXPECT_EQ(a->graph.id, 0u);
}
TEST_F(anim_params, parameter_snapshots_restore_values_and_unconsumed_triggers, anim_fixture) {
    load_assets(f, 2);
    de_anim_graph graph = parameter_graph(f, 2);
    de_entity entity = spawn_animator(f);
    ASSERT_EQ(de_anim_graph_bind(f->world, entity, graph), DE_OK);
    de_anim_set_float(f->world, entity, de_name_from("speed"), 7);
    de_anim_trigger(f->world, entity, de_name_from("fire"));
    de_world_snapshot snapshot = {0};
    ASSERT_EQ(de_world_snapshot_take(f->world, &f->allocator, &snapshot), DE_OK);
    EXPECT_TRUE(de_anim_consume_trigger(f->world, entity, de_name_from("fire")));
    de_anim_set_float(f->world, entity, de_name_from("speed"), 9);
    de_anim_set_bool(f->world, entity, de_name_from("armed"), false);
    ASSERT_EQ(de_world_snapshot_restore(f->world, &snapshot), DE_OK);
    f32 speed = 0;
    bool armed = false;
    ASSERT_TRUE(de_anim_get_float(f->world, entity, de_name_from("speed"), &speed));
    ASSERT_TRUE(de_anim_get_bool(f->world, entity, de_name_from("armed"), &armed));
    EXPECT_TRUE(speed == 7 && armed);
    EXPECT_TRUE(de_anim_consume_trigger(f->world, entity, de_name_from("fire")));
    de_world_snapshot_free(&f->allocator, &snapshot);
}
TEST_F(anim_params, invalid_and_failed_binding_preserves_existing_actor_state, anim_fixture) {
    load_assets(f, 2);
    de_anim_graph graph = parameter_graph(f, 2);
    de_entity entity = spawn_animator(f);
    EXPECT_EQ(de_anim_graph_bind(f->world, entity, (de_anim_graph){f->clip.id}),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(de_entity_get_const(f->world, entity, de_anim_params_component(f->anim)) == NULL);
    de_tracking_allocator_fail_after(&f->tracking, 0);
    de_result failed = de_anim_graph_bind(f->world, entity, graph);
    de_tracking_allocator_fail_after(&f->tracking, -1);
    EXPECT_EQ(failed, DE_ERR_OUT_OF_MEMORY);
    const DeAnimator *a = de_entity_get_const(f->world, entity, de_anim_component(f->anim));
    ASSERT_TRUE(a != NULL);
    EXPECT_EQ(a->graph.id, 0u);
    ASSERT_EQ(de_anim_graph_bind(f->world, entity, graph), DE_OK);
    de_anim_set_float(f->world, entity, de_name_from("speed"), 5);
    EXPECT_EQ(de_anim_graph_bind(f->world, entity, (de_anim_graph){f->clip.id}),
              DE_ERR_INVALID_ARGUMENT);
    f32 value = 0;
    ASSERT_TRUE(de_anim_get_float(f->world, entity, de_name_from("speed"), &value));
    EXPECT_TRUE(value == 5);
}
typedef struct parameter_system {
    de_entity entity;
    de_name speed, armed, fire;
    bool read;
} parameter_system;
static void write_parameters(de_system_ctx *ctx) {
    parameter_system *p = ctx->user;
    de_anim_set_float(ctx->world, p->entity, p->speed, 6);
    de_anim_set_bool(ctx->world, p->entity, p->armed, false);
    de_anim_trigger(ctx->world, p->entity, p->fire);
}
static void read_parameters(de_system_ctx *ctx) {
    parameter_system *p = ctx->user;
    f32 speed = 0;
    bool armed = true;
    p->read = de_anim_get_float(ctx->world, p->entity, p->speed, &speed) && speed == 6 &&
              de_anim_get_bool(ctx->world, p->entity, p->armed, &armed) && !armed;
}
TEST_F(anim_params, declared_component_access_works_inside_systems_without_structural_changes,
       anim_fixture) {
    load_assets(f, 2);
    de_anim_graph graph = parameter_graph(f, 2);
    parameter_system p = {spawn_animator(f), de_name_from("speed"), de_name_from("armed"),
                          de_name_from("fire"), false};
    ASSERT_EQ(de_anim_graph_bind(f->world, p.entity, graph), DE_OK);
    de_query_term write = {de_anim_params_component(f->anim), DE_WRITE};
    de_query_desc wq = {.terms = &write, .term_count = 1};
    de_system writer = de_system_add(f->world, &(de_system_desc){.name = "test.anim_params.write",
                                                                 .schedule = DE_SCHEDULE_UPDATE,
                                                                 .fn = write_parameters,
                                                                 .user = &p,
                                                                 .queries = &wq,
                                                                 .query_count = 1});
    ASSERT_TRUE(writer.id != 0);
    de_query_term read = {de_anim_params_component(f->anim), DE_READ};
    de_query_desc rq = {.terms = &read, .term_count = 1};
    const char *after[] = {"test.anim_params.write"};
    de_system reader = de_system_add(f->world, &(de_system_desc){.name = "test.anim_params.read",
                                                                 .schedule = DE_SCHEDULE_UPDATE,
                                                                 .fn = read_parameters,
                                                                 .user = &p,
                                                                 .queries = &rq,
                                                                 .query_count = 1,
                                                                 .after = after,
                                                                 .after_count = 1});
    ASSERT_TRUE(reader.id != 0);
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_UPDATE, .016f, 0), DE_OK);
    EXPECT_TRUE(p.read);
    EXPECT_TRUE(de_anim_consume_trigger(f->world, p.entity, p.fire));
    de_system_remove(f->world, reader);
    de_system_remove(f->world, writer);
}
static void warning_sink(const de_log_record *record, void *user) {
    if (record->level == DE_LOG_WARN &&
        strstr(record->message.ptr, "unknown or wrong-type animation parameter"))
        ++*(u32 *)user;
}
TEST_F(anim_params, unknown_setter_warns_once_and_warmed_parameter_edits_allocate_nothing,
       anim_fixture) {
    load_assets(f, 2);
    ASSERT_EQ(de_log_init(&(de_log_desc){.no_console_sink = true}), DE_OK);
    u32 warnings = 0;
    de_log_add_sink(warning_sink, &warnings);
    de_anim_graph graph = parameter_graph(f, 2);
    de_entity entity = spawn_animator(f);
    ASSERT_EQ(de_anim_graph_bind(f->world, entity, graph), DE_OK);
    const de_name unknown = de_name_from("unknown"), speed = de_name_from("speed"),
                  armed = de_name_from("armed"), fire = de_name_from("fire");
    for (u32 i = 0; i < 10; ++i) {
        de_anim_set_float(f->world, entity, unknown, 1);
        de_anim_set_bool(f->world, entity, speed, false);
    }
    de_log_flush();
    EXPECT_EQ(warnings, 1u);
    de_alloc_guard guard = {0};
    de_alloc_guard_begin(&guard);
    for (u32 i = 0; i < 1000; ++i) {
        de_anim_set_float(f->world, entity, speed, 4);
        de_anim_set_bool(f->world, entity, armed, false);
        de_anim_trigger(f->world, entity, fire);
        ASSERT_TRUE(de_anim_consume_trigger(f->world, entity, fire));
    }
    de_alloc_guard_end(&guard);
    DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
    de_log_remove_sink(warning_sink, &warnings);
    de_log_shutdown();
}
TEST_F(anim_params, all_32_slots_and_recycled_entities_do_not_share_values, anim_fixture) {
    load_assets(f, 2);
    de_anim_param_desc params[DE_ANIM_MAX_PARAMS] = {0};
    for (u32 i = 0; i < DE_ANIM_MAX_PARAMS; ++i) {
        char name[16];
        snprintf(name, sizeof name, "p%u", i);
        params[i].name = de_name_from(name);
        params[i].float_default = (f32)i;
    }
    params[30].kind = DE_ANIM_PARAM_BOOL;
    params[31].kind = DE_ANIM_PARAM_TRIGGER;
    const de_anim_node_desc node = {.kind = DE_ANIM_NODE_CLIP,
                                    .data.clip = {.clip = f->clip, .speed = 1}};
    de_anim_graph graph = {0};
    ASSERT_EQ(de_anim_graph_create(f->anim,
                                   &(de_anim_graph_desc){.params = params,
                                                         .param_count = DE_ANIM_MAX_PARAMS,
                                                         .nodes = &node,
                                                         .node_count = 1},
                                   &graph),
              DE_OK);
    de_entity entity = spawn_animator(f);
    ASSERT_EQ(de_anim_graph_bind(f->world, entity, graph), DE_OK);
    de_anim_set_float(f->world, entity, params[29].name, 100);
    de_anim_set_bool(f->world, entity, params[30].name, true);
    de_anim_trigger(f->world, entity, params[31].name);
    EXPECT_TRUE(de_anim_consume_trigger(f->world, entity, params[31].name));
    bool b = false;
    ASSERT_TRUE(de_anim_get_bool(f->world, entity, params[30].name, &b));
    EXPECT_TRUE(b);
    de_entity_despawn(f->world, entity);
    de_entity replacement = spawn_animator(f);
    ASSERT_EQ(de_anim_graph_bind(f->world, replacement, graph), DE_OK);
    f32 value = 0;
    ASSERT_TRUE(de_anim_get_float(f->world, replacement, params[29].name, &value));
    EXPECT_TRUE(value == 29);
    EXPECT_FALSE(de_anim_consume_trigger(f->world, replacement, params[31].name));
}
