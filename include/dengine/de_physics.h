/* de_physics.h — own deterministic 3D and 2D rigid-body physics. SPEC.md §13.
 * Runs in FixedUpdate. Positions are double (large worlds); solver is float, island-relative. */
#ifndef DE_PHYSICS_H
#define DE_PHYSICS_H
#include "de_base.h"
#include "de_math.h"
#include "de_ecs.h"
DE_EXTERN_C_BEGIN

typedef struct de_physics_world de_physics_world;
typedef struct de_physics2d_world de_physics2d_world;
DE_HANDLE(de_body);
DE_HANDLE(de_shape);
DE_HANDLE(de_joint);

typedef enum de_body_type { DE_BODY_STATIC, DE_BODY_KINEMATIC, DE_BODY_DYNAMIC } de_body_type;

/* ---------- Shapes ---------- */
typedef enum de_shape_kind {
    DE_SHAPE_SPHERE,
    DE_SHAPE_CAPSULE,
    DE_SHAPE_BOX,
    DE_SHAPE_CYLINDER,
    DE_SHAPE_CONVEX,
    DE_SHAPE_MESH,
    DE_SHAPE_HEIGHTFIELD,
    DE_SHAPE_COMPOUND
} de_shape_kind;

typedef struct de_shape_desc {
    de_shape_kind kind;
    de_vec3 half_extents;    /* box */
    f32 radius, half_height; /* sphere, capsule, cylinder */
    const de_vec3 *points;
    u32 point_count; /* convex */
    const de_vec3 *vertices;
    u32 vertex_count;
    const u32 *indices;
    u32 index_count; /* mesh (static only) */
    const f32 *heights;
    u32 rows, cols;
    de_vec3 scale; /* heightfield */
    const de_shape *children;
    const de_transform *child_xforms;
    u32 child_count; /* compound */
} de_shape_desc;

typedef struct de_material_phys {
    f32 friction, restitution, density;
} de_material_phys;

/* ---------- World ---------- */
typedef struct de_physics_desc {
    const de_allocator *allocator;
    de_task_system *tasks;
    de_vec3 gravity; /* default (0, 0, -9.81) */
    u32 substeps;    /* default 4 */
    u32 max_bodies;
    bool deterministic; /* default true */
} de_physics_desc;

DE_API de_result de_physics_create(const de_physics_desc *desc, de_physics_world **out);
DE_API void de_physics_destroy(de_physics_world *p);
/* Registers DeRigidBody/DeCollider/DeCharacter components and the FixedUpdate step system. */
DE_API void de_physics_attach(de_physics_world *p, de_world *w);
DE_API void de_physics_step(de_physics_world *p, f32 dt); /* manual stepping (tests, tools) */
DE_API u64 de_physics_checksum(const de_physics_world *p);

DE_API de_shape de_shape_create(de_physics_world *p, const de_shape_desc *desc);
DE_API void de_shape_destroy(de_physics_world *p, de_shape s);

/* ---------- Collision filtering ---------- */
typedef struct de_collision_filter {
    u32 layer;
    u32 mask;
    u32 group;
} de_collision_filter;

/* ---------- Queries ---------- */
typedef struct de_ray_hit {
    de_entity entity;
    de_body body;
    de_dvec3 point;
    de_vec3 normal;
    f32 fraction;
    u32 sub_shape;
} de_ray_hit;

DE_API bool de_physics_raycast(const de_physics_world *p, de_dvec3 origin, de_vec3 dir,
                               f32 max_dist, de_collision_filter filter, de_ray_hit *hit);
DE_API u32 de_physics_raycast_all(const de_physics_world *p, de_dvec3 origin, de_vec3 dir,
                                  f32 max_dist, de_collision_filter filter, de_ray_hit *hits,
                                  u32 max_hits);
DE_API bool de_physics_shape_cast(const de_physics_world *p, de_shape shape,
                                  const de_transform *from, de_vec3 dir, f32 max_dist,
                                  de_collision_filter filter, de_ray_hit *hit);
DE_API u32 de_physics_overlap(const de_physics_world *p, de_shape shape, const de_transform *at,
                              de_collision_filter filter, de_entity *out, u32 max);

/* ---------- Joints ---------- */
typedef enum de_joint_kind {
    DE_JOINT_FIXED,
    DE_JOINT_HINGE,
    DE_JOINT_BALL,
    DE_JOINT_SLIDER,
    DE_JOINT_DISTANCE,
    DE_JOINT_CONE_TWIST,
    DE_JOINT_6DOF
} de_joint_kind;
typedef struct de_joint_desc {
    de_joint_kind kind;
    de_entity a, b;
    de_transform frame_a, frame_b;    /* local anchor frames */
    f32 limits_min[6], limits_max[6]; /* per-axis for 6DOF; [0] used by 1-DOF joints */
    f32 motor_target[6], motor_max_force[6];
    f32 break_force; /* 0 = unbreakable */
} de_joint_desc;
DE_API de_joint de_joint_create(de_physics_world *p, const de_joint_desc *desc);
DE_API void de_joint_destroy(de_physics_world *p, de_joint j);

/* ---------- ECS components ---------- */
typedef struct DeRigidBody {
    de_body_type type;
    f32 mass; /* 0 = from shape density */
    de_vec3 linear_velocity, angular_velocity;
    f32 linear_damping, angular_damping;
    f32 gravity_scale;
    bool ccd, allow_sleep, lock_rot_x, lock_rot_y, lock_rot_z;
    de_body body; /* internal, set by the physics system */
} DeRigidBody;

typedef struct DeCollider {
    de_shape shape;
    de_transform offset;
    de_material_phys material;
    de_collision_filter filter;
    bool is_trigger;
} DeCollider;

typedef struct DeCharacter {
    f32 radius, height, step_height, max_slope_rad, skin_width;
    de_vec3 desired_velocity; /* input; written by game */
    de_vec3 velocity;         /* output */
    bool grounded;            /* output */
    de_vec3 ground_normal;    /* output */
    de_entity ground_entity;  /* output; moving platforms */
} DeCharacter;

/* Contact events (sent each fixed step). */
typedef enum de_contact_kind {
    DE_CONTACT_BEGIN,
    DE_CONTACT_END,
    DE_TRIGGER_ENTER,
    DE_TRIGGER_EXIT
} de_contact_kind;
typedef struct de_contact_event {
    de_contact_kind kind;
    de_entity a, b;
    de_dvec3 point;
    de_vec3 normal;
    f32 impulse;
} de_contact_event;

/* ---------- 2D (same design, operates in the configured 2D plane) ---------- */
typedef enum de_shape2d_kind {
    DE_SHAPE2D_CIRCLE,
    DE_SHAPE2D_CAPSULE,
    DE_SHAPE2D_BOX,
    DE_SHAPE2D_POLYGON,
    DE_SHAPE2D_CHAIN
} de_shape2d_kind;
typedef struct DeRigidBody2D {
    de_body_type type;
    f32 mass;
    de_vec2 linear_velocity;
    f32 angular_velocity;
    f32 linear_damping, angular_damping, gravity_scale;
    bool fixed_rotation, bullet;
    de_body body;
} DeRigidBody2D;
typedef struct DeCollider2D {
    de_shape2d_kind kind;
    de_vec2 half_extents;
    f32 radius;
    de_vec2 points[8];
    u32 point_count;
    de_material_phys material;
    de_collision_filter filter;
    bool is_trigger;
} DeCollider2D;
typedef struct DeCharacter2D {
    de_vec2 size;
    f32 max_slope_rad, step_height;
    de_vec2 desired_velocity;
    de_vec2 velocity;
    bool grounded;
} DeCharacter2D;

DE_API de_result de_physics2d_create(const de_physics_desc *desc, de_physics2d_world **out);
DE_API void de_physics2d_destroy(de_physics2d_world *p);
DE_API void de_physics2d_attach(de_physics2d_world *p, de_world *w);
DE_API bool de_physics2d_raycast(const de_physics2d_world *p, de_vec2 origin, de_vec2 dir,
                                 f32 max_dist, de_collision_filter filter, de_entity *hit_entity,
                                 de_vec2 *point, de_vec2 *normal);
DE_API u64 de_physics2d_checksum(const de_physics2d_world *p);

DE_EXTERN_C_END
#endif
