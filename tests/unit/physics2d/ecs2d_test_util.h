#ifndef DE_ECS2D_TEST_UTIL_H
#define DE_ECS2D_TEST_UTIL_H
#include "solver2d_test_util.h"
#include "physics/2d/ecs2d/ecs2d_internal.h"
typedef struct e2_env {
    de_type_registry *types;
    de_world *w;
    de_physics2d_world *p;
    de_builtin_component_ids builtins;
    de_component rb, col, ch;
} e2_env;
static inline e2_env e2_create(de_physics2d_plane plane, const de_allocator *alloc,
                               de_task_system *tasks) {
    de_fp_env_init_thread();
    e2_env e = {0};
    ASSERT_EQ(de_type_registry_create(alloc, &e.types), DE_OK);
    de_world_desc wd = {0};
    wd.types = e.types;
    wd.allocator = alloc;
    wd.tasks = tasks;
    wd.initial_entity_capacity = 512;
    ASSERT_EQ(de_world_create(&wd, &e.w), DE_OK);
    e.builtins = de_builtin_components(e.w);
    de_physics2d_desc pd = {0};
    pd.allocator = alloc;
    pd.tasks = tasks;
    pd.max_bodies = 512;
    pd.plane = plane;
    ASSERT_EQ(de_physics2d_create(&pd, &e.p), DE_OK);
    de_physics2d_attach(e.p, e.w);
    e.rb = de_component_find(e.w, "DeRigidBody2D");
    e.col = de_component_find(e.w, "DeCollider2D");
    e.ch = de_component_find(e.w, "DeCharacter2D");
    ASSERT_TRUE(e.rb.id && e.col.id && e.ch.id);
    return e;
}
static inline void e2_destroy(e2_env *e) {
    de_physics2d_destroy(e->p);
    de_world_destroy(e->w);
    de_type_registry_destroy(e->types);
}
static inline de_entity e2_body(e2_env *e, de_body_type type, f64 x, f64 y, f32 radius) {
    de_entity entity = de_entity_spawn(e->w);
    DeTransform *t = de_entity_add(e->w, entity, e->builtins.transform);
    *t = de_transform_identity();
    if (e->p->plane == DE_PHYSICS2D_PLANE_SIDE) {
        t->position.x = x;
        t->position.z = y;
        t->position.y = 7;
    } else {
        t->position.x = x;
        t->position.y = y;
        t->position.z = 7;
    }
    DeRigidBody2D *rb = de_entity_add(e->w, entity, e->rb);
    rb->type = type;
    DeCollider2D *c = de_entity_add(e->w, entity, e->col);
    c->kind = DE_SHAPE2D_CIRCLE;
    c->radius = radius;
    return entity;
}
static inline void e2_tick(e2_env *e, u32 ticks) {
    for (u32 i = 0; i < ticks; ++i)
        ASSERT_EQ(de_world_run_schedule(e->w, DE_SCHEDULE_FIXED_UPDATE, SV_DT, 0), DE_OK);
}
#endif
