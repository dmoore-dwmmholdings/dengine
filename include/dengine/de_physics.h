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
/** A body of one physics world. Low 20 bits: index + 1; high 12 bits: generation (never 0). */
DE_HANDLE(de_body);
/** A collision shape of one physics world. Same packing as de_body. */
DE_HANDLE(de_shape);
DE_HANDLE(de_joint);

typedef enum de_body_type { DE_BODY_STATIC, DE_BODY_KINEMATIC, DE_BODY_DYNAMIC } de_body_type;

/*
 * 3D physics (DENG-132 and the rest of S22).
 *
 * Conventions (SPEC §5.5): Z up, right-handed, meters, kilograms, seconds, radians. Body positions
 * are de_dvec3 (f64, large worlds up to +-100 km); rotations, velocities and all shape data are
 * f32. A body's position is the position of its origin (the shape's local origin), not of its
 * center of mass.
 *
 * Determinism (SPEC §5.7, §13.2): the module is built without FP contraction, uses only de_dmath
 * and its own + - * / sqrt helpers, never iterates a hash map, and processes bodies in ascending
 * body index. Shape building (hulls, BVHs) is deterministic too: the same input in the same order
 * gives the same bytes on every platform. de_physics_checksum() hashes the simulation state.
 *
 * Threading: create/destroy of the world, bodies and shapes, the setters and de_physics_step()
 * are single-threaded. The getters, de_physics_checksum() and the queries may run on many threads
 * at once while nothing writes.
 */

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

/** Most vertices a convex hull keeps (de_shape_desc.points may have more). */
#define DE_SHAPE_MAX_HULL_VERTICES 256u
/** Most input points of a convex hull. */
#define DE_SHAPE_MAX_HULL_INPUT 65536u
/** Most children of a compound shape. */
#define DE_SHAPE_MAX_COMPOUND_CHILDREN 65536u
/** A heightfield sample with this value is a hole: every cell that touches it has no surface. */
#define DE_HEIGHTFIELD_HOLE (-3.402823466e+38f)
/** Heightfield cells per side of a midphase block (DE_HEIGHTFIELD_BLOCK^2 cells per block). */
#define DE_HEIGHTFIELD_BLOCK 8u

/**
 * de_shape_create() parameters. Fill the fields of `kind`; the others are ignored. All shapes are
 * in their own local frame (Z up); a body places that frame in the world.
 *
 * - DE_SHAPE_SPHERE: `radius` > 0, centered on the origin.
 * - DE_SHAPE_CAPSULE: a segment from (0, 0, -half_height) to (0, 0, +half_height) along local +Z,
 *   swept by `radius` > 0. `half_height` >= 0 (0 is a sphere). Total height 2 * (half_height +
 *   radius).
 * - DE_SHAPE_BOX: `half_extents` > 0 on every axis, centered on the origin.
 * - DE_SHAPE_CYLINDER: axis local +Z from -half_height to +half_height, `radius` > 0,
 *   `half_height` > 0.
 * - DE_SHAPE_CONVEX: the convex hull of `points[0..point_count)` (4 to DE_SHAPE_MAX_HULL_INPUT
 *   points). Points closer than `hull_tolerance` to the hull are dropped (welded); 0 picks the
 *   largest of 1e-5, 1e-4 times the longest side of the points' box, and 4e-7 times the largest
 *   |coordinate| (a few f32 steps). The hull keeps at most
 *   DE_SHAPE_MAX_HULL_VERTICES vertices: beyond that the farthest points win and the rest are
 *   left out (the hull then approximates the cloud). Coplanar triangles are merged into polygon
 *   faces. All points on one line or one plane (or all the same point) are rejected.
 * - DE_SHAPE_MESH: static triangle mesh: `vertices[0..vertex_count)` and `index_count` / 3
 *   triangles of `indices`, counter-clockwise when seen from the front (the front is the side
 *   the normal (b - a) x (c - a) points to). Static and kinematic bodies only. Zero-area
 *   triangles never collide; sub-shape ids are triangle indices.
 * - DE_SHAPE_HEIGHTFIELD: `rows` x `cols` samples (each >= 2), row-major: sample (col, row) is
 *   `heights[row * cols + col]` at local (col * scale.x, row * scale.y, height * scale.z). The
 *   origin is sample (0, 0); `scale` > 0 on every axis. Each cell (col, row)..(col+1, row+1) is
 *   two triangles split along the diagonal from (col, row) to (col+1, row+1). A sample equal to
 *   DE_HEIGHTFIELD_HOLE makes every cell around it a hole. Static and kinematic bodies only.
 * - DE_SHAPE_COMPOUND: `children[0..child_count)`, each placed by `child_xforms[i]` (position
 *   and rotation; scale is ignored; NULL `child_xforms` places every child at the origin).
 *   Children are any non-compound shapes of the same world (one nesting level); they are
 *   referenced, not copied, and cannot be destroyed while the compound exists. A compound with a
 *   mesh or heightfield child is static-only like them. Sub-shape ids are child indices.
 */
typedef struct de_shape_desc {
    de_shape_kind kind;
    de_vec3 half_extents;    /* box */
    f32 radius, half_height; /* sphere, capsule, cylinder */
    const de_vec3 *points;
    u32 point_count;    /* convex */
    f32 hull_tolerance; /* convex: weld / coplanarity tolerance in meters; 0 = automatic */
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

/**
 * Surface and bulk material. `density` in kg/m^3; 0 means DE_PHYSICS_DEFAULT_DENSITY. Friction
 * and restitution are used by the solver (S23).
 */
typedef struct de_material_phys {
    f32 friction, restitution, density;
} de_material_phys;

/** Density used when de_material_phys.density is 0 (water, kg/m^3). */
#define DE_PHYSICS_DEFAULT_DENSITY 1000.0f

/**
 * Mass properties: `mass` in kg, `center` (center of mass) in the shape's or body's local frame,
 * `inertia` the inertia tensor about the center of mass in local axes (kg m^2, symmetric).
 */
typedef struct de_mass_props {
    f32 mass;
    de_vec3 center;
    de_mat3 inertia;
} de_mass_props;

/* ---------- World ---------- */
/** Default de_physics_desc.max_bodies. */
#define DE_PHYSICS_DEFAULT_MAX_BODIES 65536u
/** Largest de_physics_desc.max_bodies (the handle index has 20 bits). */
#define DE_PHYSICS_MAX_BODIES_LIMIT 1048574u

/**
 * de_physics_create() parameters. `(de_physics_desc){0}` (or a NULL desc) gives the defaults.
 *
 * - `allocator`: everything the world allocates (body pool, shapes). NULL = de_heap_allocator().
 *   Must outlive the world.
 * - `tasks`: the task system the step may use (DENG-135); NULL = single-threaded. Results never
 *   depend on it.
 * - `gravity`: m/s^2. The zero vector means the default (0, 0, -9.81); for zero gravity call
 *   de_physics_set_gravity() after create.
 * - `substeps`: solver substeps per step (S23); 0 = 4.
 * - `max_bodies`: size of the preallocated body pool; 0 = DE_PHYSICS_DEFAULT_MAX_BODIES, at most
 *   DE_PHYSICS_MAX_BODIES_LIMIT. Creating more bodies fails.
 * - `deterministic`: S22 physics is always deterministic; false is treated as true (reserved for
 *   a later faster mode).
 */
typedef struct de_physics_desc {
    const de_allocator *allocator;
    de_task_system *tasks;
    de_vec3 gravity; /* default (0, 0, -9.81) */
    u32 substeps;    /* default 4 */
    u32 max_bodies;
    bool deterministic; /* default true */
} de_physics_desc;

/**
 * Creates a physics world and preallocates its body pool.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (`out` NULL, `max_bodies` above the limit, or a
 *         non-finite gravity) or DE_ERR_OUT_OF_MEMORY.
 */
DE_API de_result de_physics_create(const de_physics_desc *desc, de_physics_world **out);
/** Destroys every body and shape and frees all memory of the world. NULL is ignored. */
DE_API void de_physics_destroy(de_physics_world *p);
/**
 * ECS integration (SPEC §13.1): registers the DeRigidBody, DeCollider and DeCharacter components
 * (with DE_COMP_SNAPSHOT) and their field types in `types`, and inserts a DePhysicsWorld resource
 * that points to `p` into `w`. `types` must be the type registry `w` was created with. Calling it
 * again for the same world is harmless. The FixedUpdate systems that sync components and bodies
 * come in S23. This is the only function of the module that touches the ECS.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT, or a registration error (see de_component_register).
 */
DE_API de_result de_physics_attach(de_physics_world *p, de_world *w, de_type_registry *types);
/**
 * Advances the world by `dt` seconds (> 0, finite): one tick. Manual stepping for tests and tools;
 * the S23 FixedUpdate system calls it. Allocates nothing after create (pools only). An invalid
 * `dt` sets DE_ERR_INVALID_ARGUMENT and does nothing.
 */
DE_API void de_physics_step(de_physics_world *p, f32 dt); /* manual stepping (tests, tools) */
/**
 * XXH3-64 of the simulation state: the tick, the gravity and substeps, and every live body in
 * ascending index order, field by field (never raw struct bytes). Equal states give equal values
 * on every platform. 0 for NULL.
 */
DE_API u64 de_physics_checksum(const de_physics_world *p);
/** Ticks stepped since create. */
DE_API u64 de_physics_tick(const de_physics_world *p);
/** Sets the gravity (m/s^2; any finite vector, zero included). */
DE_API void de_physics_set_gravity(de_physics_world *p, de_vec3 gravity);
DE_API de_vec3 de_physics_get_gravity(const de_physics_world *p);

/**
 * Builds a shape (see de_shape_desc). Shapes are immutable and shared: any number of bodies (and
 * compounds) may use one. May allocate. On failure returns an invalid handle and sets the error
 * context: DE_ERR_INVALID_ARGUMENT (bad parameters, degenerate hull input, unknown or stale child
 * shape, nested compound) or DE_ERR_OUT_OF_MEMORY.
 */
DE_API de_shape de_shape_create(de_physics_world *p, const de_shape_desc *desc);
/**
 * Destroys a shape. A shape still used by a body or a compound is not destroyed: the call returns
 * DE_ERR_BUSY, logs a warning and changes nothing. A stale or invalid handle gives
 * DE_ERR_INVALID_ARGUMENT.
 */
DE_API de_result de_shape_destroy(de_physics_world *p, de_shape s);
/** true if `s` is a live shape of `p`. */
DE_API bool de_shape_valid(const de_physics_world *p, de_shape s);
/** The kind of a live shape, or false for a stale handle. */
DE_API bool de_shape_get_kind(const de_physics_world *p, de_shape s, de_shape_kind *out);
/** The shape's bounding box in its local frame, or false for a stale handle. */
DE_API bool de_shape_get_local_aabb(const de_physics_world *p, de_shape s, de_aabb *out);
/**
 * Mass properties of the shape filled with `density` (kg/m^3; 0 = DE_PHYSICS_DEFAULT_DENSITY):
 * analytic for primitives, tetrahedra for hulls, parallel axis for compounds. Meshes and
 * heightfields (and compounds that hold one) have no volume: mass 0.
 * @return DE_OK or DE_ERR_INVALID_ARGUMENT (stale handle, NULL `out`, negative density).
 */
DE_API de_result de_shape_get_mass_props(const de_physics_world *p, de_shape s, f32 density,
                                         de_mass_props *out);

/* ---------- Collision filtering ---------- */
typedef struct de_collision_filter {
    u32 layer;
    u32 mask;
    u32 group;
} de_collision_filter;

/** The filter a body gets when its de_body_desc.filter is all zero: layer 1, every mask bit. */
#define DE_COLLISION_FILTER_DEFAULT_LAYER 1u
#define DE_COLLISION_FILTER_DEFAULT_MASK  0xFFFFFFFFu

/* ---------- Bodies (direct API: tests, tools, and the S23 ECS systems) ---------- */
/** de_body_desc.flags. */
typedef enum de_body_flags {
    DE_BODY_FLAG_CCD = 1u << 0,
    DE_BODY_FLAG_ALLOW_SLEEP = 1u << 1,
    DE_BODY_FLAG_LOCK_ROT_X = 1u << 2,
    DE_BODY_FLAG_LOCK_ROT_Y = 1u << 3,
    DE_BODY_FLAG_LOCK_ROT_Z = 1u << 4,
    DE_BODY_FLAG_TRIGGER = 1u << 5
} de_body_flags;

/**
 * de_body_create() parameters. `(de_body_desc){0}` is a static body at the origin with no shape.
 *
 * - `transform`: position (f64) and rotation of the body origin. The rotation is normalized; a
 *   zero quaternion is the identity. `scale` is ignored (shapes carry their size).
 * - `shape`: a live shape of the same world, or an invalid handle for no shape. A dynamic body
 *   needs a shape with volume (not a mesh, heightfield, or compound holding one).
 * - `mass`: kg; 0 = from the shape volume and `material.density`. A positive mass rescales the
 *   shape's inertia to it.
 * - `material`: density 0 means DE_PHYSICS_DEFAULT_DENSITY.
 * - `filter`: all zero means {DE_COLLISION_FILTER_DEFAULT_LAYER, DE_COLLISION_FILTER_DEFAULT_MASK,
 *   0}.
 * - `flags`: de_body_flags. `owner`: the ECS entity the body belongs to (DE_ENTITY_NULL for none).
 */
typedef struct de_body_desc {
    de_body_type type;
    de_transform transform;
    de_vec3 linear_velocity, angular_velocity;
    de_shape shape;
    f32 mass;
    de_material_phys material;
    de_collision_filter filter;
    f32 linear_damping, angular_damping, gravity_scale;
    u32 flags;
    de_entity owner;
} de_body_desc;

/**
 * Creates a body in the preallocated pool: the most recently freed index first (LIFO), else the
 * next unused one. Allocates nothing. On failure returns an invalid handle and sets the error
 * context: DE_ERR_INVALID_ARGUMENT (bad type, stale shape, non-finite transform or velocity,
 * negative mass, a dynamic body without a volume shape) or DE_ERR_OUT_OF_MEMORY (pool full).
 */
DE_API de_body de_body_create(de_physics_world *p, const de_body_desc *desc);
/** Destroys a body; its handle and every copy of it become stale. Stale handles are ignored. */
DE_API void de_body_destroy(de_physics_world *p, de_body b);
/** true if `b` is a live body of `p`. */
DE_API bool de_body_valid(const de_physics_world *p, de_body b);
/** Live bodies. */
DE_API u32 de_body_count(const de_physics_world *p);
/** Position and rotation of a live body (scale is 1); false for a stale handle. */
DE_API bool de_body_get_transform(const de_physics_world *p, de_body b, de_transform *out);
/** Teleports a body (scale ignored, rotation normalized). false for a stale handle or a
 *  non-finite transform. */
DE_API bool de_body_set_transform(de_physics_world *p, de_body b, const de_transform *t);
/** Linear (m/s) and angular (rad/s, world axes) velocity; either output may be NULL. false for a
 *  stale handle. */
DE_API bool de_body_get_velocity(const de_physics_world *p, de_body b, de_vec3 *linear,
                                 de_vec3 *angular);
/** Sets both velocities. false for a stale handle or a non-finite value. Static bodies keep zero
 *  velocity. */
DE_API bool de_body_set_velocity(de_physics_world *p, de_body b, de_vec3 linear, de_vec3 angular);
/** Mass properties of a live body in its local frame (mass 0 for static and kinematic bodies,
 *  which have infinite mass); false for a stale handle. */
DE_API bool de_body_get_mass_props(const de_physics_world *p, de_body b, de_mass_props *out);

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
/* DeCollider.shape is a runtime handle: snapshotted, not serialized (assets bind it, S23). */

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

/** The ECS resource de_physics_attach() inserts: the physics world of an ECS world. Not
 *  serialized or snapshotted (a pointer). */
typedef struct DePhysicsWorld {
    de_physics_world *world;
} DePhysicsWorld;

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
