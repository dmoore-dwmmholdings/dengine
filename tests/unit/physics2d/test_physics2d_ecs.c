#include "ecs2d_test_util.h"
TEST(physics2d_ecs, spawn_fall_despawn_same_tick) {
    e2_env e = e2_create(DE_PHYSICS2D_PLANE_SIDE, NULL, NULL);
    de_entity entity = e2_body(&e, DE_BODY_DYNAMIC, 0, 5, 0.5f);
    e2_tick(&e, 1);
    const DeRigidBody2D *rb = de_entity_get_const(e.w, entity, e.rb);
    EXPECT_TRUE(rb->body.id != 0);
    const DeTransform *t = de_entity_get_const(e.w, entity, e.builtins.transform);
    EXPECT_TRUE(t->position.z < 5);
    EXPECT_TRUE(t->position.y == 7);
    de_entity_despawn(e.w, entity);
    e2_tick(&e, 1);
    EXPECT_EQ(e.p->bodies.live_count, 0u);
    e2_destroy(&e);
}
TEST(physics2d_ecs, game_pose_velocity_and_shape_changes) {
    e2_env e = e2_create(DE_PHYSICS2D_PLANE_TOP_DOWN, NULL, NULL);
    de_entity entity = e2_body(&e, DE_BODY_DYNAMIC, 0, 0, 0.5f);
    e2_tick(&e, 1);
    DeTransform *t = de_entity_get(e.w, entity, e.builtins.transform);
    t->position.x = 10;
    DeRigidBody2D *rb = de_entity_get(e.w, entity, e.rb);
    rb->linear_velocity.x = 3;
    e2_tick(&e, 1);
    t = de_entity_get(e.w, entity, e.builtins.transform);
    EXPECT_TRUE(t->position.x > 10);
    DeCollider2D *c = de_entity_get(e.w, entity, e.col);
    c->radius = 0.7f;
    e2_tick(&e, 1);
    EXPECT_EQ(e.p->bodies.live_count, 1u);
    e2_destroy(&e);
}
TEST(physics2d_ecs, contacts_once_per_transition_and_world_mapping) {
    e2_env e = e2_create(DE_PHYSICS2D_PLANE_SIDE, NULL, NULL);
    (void)e2_body(&e, DE_BODY_STATIC, 0, 0, 1);
    de_entity b = e2_body(&e, DE_BODY_DYNAMIC, 0, 1.5, 1);
    de_type_id type = de_type_find(e.types, "de_contact_event");
    de_event_type event = de_event_register(e.w, type);
    de_event_reader reader = de_event_reader_create(e.w, event);
    e2_tick(&e, 1);
    const de_contact_event *ev = de_event_next(e.w, &reader);
    ASSERT_TRUE(ev != NULL);
    EXPECT_EQ(ev->kind, DE_CONTACT_BEGIN);
    EXPECT_FLOAT_EQ(ev->normal.y, 0, 0);
    EXPECT_TRUE(ev->normal.z > 0);
    EXPECT_TRUE(de_event_next(e.w, &reader) == NULL);
    e2_tick(&e, 1);
    EXPECT_TRUE(de_event_next(e.w, &reader) == NULL);
    de_entity_despawn(e.w, b);
    e2_tick(&e, 1);
    ev = de_event_next(e.w, &reader);
    ASSERT_TRUE(ev != NULL);
    EXPECT_EQ(ev->kind, DE_CONTACT_END);
    EXPECT_TRUE(ev->a.bits || ev->b.bits);
    EXPECT_TRUE(de_event_next(e.w, &reader) == NULL);
    e2_destroy(&e);
}
TEST(physics2d_ecs, character_moves_via_ecs) {
    e2_env e = e2_create(DE_PHYSICS2D_PLANE_TOP_DOWN, NULL, NULL);
    de_entity entity = de_entity_spawn(e.w);
    DeTransform *t = de_entity_add(e.w, entity, e.builtins.transform);
    *t = de_transform_identity();
    DeCharacter2D *ch = de_entity_add(e.w, entity, e.ch);
    ch->size = (de_vec2){0.5f, 1};
    ch->desired_velocity.x = 2;
    e2_tick(&e, 60);
    t = de_entity_get(e.w, entity, e.builtins.transform);
    EXPECT_FLOAT_EQ(t->position.x, 2, 0.01);
    ch = de_entity_get(e.w, entity, e.ch);
    EXPECT_FLOAT_EQ(ch->velocity.x, 2, 0.01);
    e2_destroy(&e);
}
TEST(physics2d_ecs, interpolation_preserves_simulation_checksum) {
    e2_env e = e2_create(DE_PHYSICS2D_PLANE_SIDE, NULL, NULL);
    de_entity entity = e2_body(&e, DE_BODY_DYNAMIC, 0, 5, 0.5f);
    e2_tick(&e, 1);
    ASSERT_EQ(de_world_run_schedule(e.w, DE_SCHEDULE_POST_UPDATE, SV_DT, 1), DE_OK);
    e2_tick(&e, 1);
    u64 world = de_world_checksum(e.w, DE_COMP_SNAPSHOT), physics = de_physics2d_checksum(e.p);
    const DeTransform *sim = de_entity_get_const(e.w, entity, e.builtins.transform);
    f64 z = sim->position.z;
    ASSERT_EQ(de_world_run_schedule(e.w, DE_SCHEDULE_POST_UPDATE, SV_DT, 0.25f), DE_OK);
    const DeGlobalTransform *global = de_entity_get_const(e.w, entity, e.builtins.global_transform);
    ASSERT_TRUE(global != NULL);
    EXPECT_TRUE(global->position.z > z);
    EXPECT_EQ(de_world_checksum(e.w, DE_COMP_SNAPSHOT), world);
    EXPECT_EQ(de_physics2d_checksum(e.p), physics);
    e2_destroy(&e);
}
TEST(physics2d_ecs, no_allocation_steady_state) {
    de_tracking_allocator tr;
    de_allocator a = de_tracking_allocator_init(&tr, NULL, DE_MEM_PHYSICS);
    e2_env e = e2_create(DE_PHYSICS2D_PLANE_SIDE, &a, NULL);
    (void)e2_body(&e, DE_BODY_DYNAMIC, 0, 5, 0.5f);
    e2_tick(&e, 3);
    de_tracking_stats before = de_tracking_allocator_stats(&tr);
    e2_tick(&e, 60);
    de_tracking_stats after = de_tracking_allocator_stats(&tr);
    EXPECT_EQ(before.alloc_count, after.alloc_count);
    EXPECT_EQ(before.realloc_count, after.realloc_count);
    e2_destroy(&e);
    EXPECT_EQ(de_tracking_allocator_stats(&tr).bytes_live, 0u);
}
TEST(physics2d_ecs, entity_joint_lookup_and_checksum_config_sensitivity) {
    e2_env e = e2_create(DE_PHYSICS2D_PLANE_TOP_DOWN, NULL, NULL);
    de_entity entity = e2_body(&e, DE_BODY_DYNAMIC, 0, 0, 0.5f);
    e2_tick(&e, 1);
    de_joint2d_desc d = {0};
    d.entity_b = entity;
    de_joint j = {0};
    ASSERT_EQ(de_joint2d_create(e.p, &d, &j), DE_OK);
    u64 before = de_physics2d_checksum(e.p);
    ASSERT_EQ(de_joint2d_set_motor(e.p, j, 2, 10), DE_OK);
    EXPECT_NE(de_physics2d_checksum(e.p), before); /* DENG-471: retained sensitivity regression. */
    e2_destroy(&e);
}
TEST(physics2d_ecs, externally_destroyed_body_is_recreated) {
    e2_env e = e2_create(DE_PHYSICS2D_PLANE_TOP_DOWN, NULL, NULL);
    de_entity entity = e2_body(&e, DE_BODY_DYNAMIC, 0, 0, 0.5f);
    e2_tick(&e, 1);
    const DeRigidBody2D *rb = de_entity_get_const(e.w, entity, e.rb);
    de_body old = rb->body;
    de_body2d_destroy(e.p, old);
    e2_tick(&e, 1);
    rb = de_entity_get_const(e.w, entity, e.rb);
    EXPECT_NE(old.id, rb->body.id);
    EXPECT_EQ(e.p->bodies.live_count, 1u);
    e2_destroy(&e);
}
