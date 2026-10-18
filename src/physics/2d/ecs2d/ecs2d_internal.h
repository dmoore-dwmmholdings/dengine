#ifndef DE_ECS2D_INTERNAL_H
#define DE_ECS2D_INTERNAL_H
#include "physics/2d/p2d_internal.h"
#include "physics/2d/ecs2d/plane_map.h"
#include "physics/2d/manifold2d.h"
typedef struct de__ecs2d_entry {
    de_entity entity, grave;
    u32 grave_generation;
    de_body body;
    de_shape owned_shape;
    bool active, character, seen;
    de_transform prev, curr;
    de_vec2 character_size;
    DeRigidBody2D rb;
    DeCollider2D col;
} de__ecs2d_entry;
typedef struct de__ecs2d {
    de_physics2d_world *p;
    de_world *w;
    de_component rb, col, character, transform, global;
    de_event_type contact;
    de_resource_id resource;
    de_system step, interpolate;
    de__ecs2d_entry *entries;
    u32 capacity;
} de__ecs2d;
void de__ecs2d_step(de_system_ctx *ctx);
void de__ecs2d_interpolate(de_system_ctx *ctx);
#endif
