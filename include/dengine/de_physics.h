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
 * Surface and bulk material. `density` in kg/m^3; 0 means DE_PHYSICS_DEFAULT_DENSITY.
 * `friction` (Coulomb coefficient, >= 0) and `restitution` (bounce, >= 0; 1 keeps the approach
 * speed) are used by the solver (DENG-217): a contact uses sqrt(friction_a * friction_b) and
 * max(restitution_a, restitution_b). A zeroed material is frictionless and does not bounce.
 * Contacts closing slower than the restitution threshold (cvar
 * `physics.solver.restitution_threshold`, 1 m/s) do not bounce.
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
 * - `substeps`: solver substeps per step (DENG-217); 0 = 4. More substeps make stacks and mass
 *   ratios stiffer at a proportional solver cost.
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
 * again for the same world is harmless. Since DENG-223 it also registers the contact and joint
 * break event types and adds the systems DE_PHYSICS_STEP_SYSTEM and DE_PHYSICS_INTERPOLATE_SYSTEM
 * (see "ECS integration" below). One physics world serves one ECS world. This is the only
 * function of the module that touches the ECS.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT, DE_ERR_ALREADY_EXISTS (`p` is attached to another
 *         ECS world), DE_ERR_OUT_OF_MEMORY, or a registration error (see de_component_register).
 */
DE_API de_result de_physics_attach(de_physics_world *p, de_world *w, de_type_registry *types);
/**
 * Advances the world by `dt` seconds (> 0, finite): one tick. Manual stepping for tests and tools;
 * the S23 FixedUpdate system calls it. Collision (broadphase, narrowphase, persistent manifolds),
 * then the sub-stepped soft-step solver (DENG-217, SPEC §13.1): `substeps` substeps of dt /
 * substeps, each integrating velocities (gravity * gravity_scale, damping), warm starting,
 * solving contacts with soft position correction, integrating positions and relaxing; then
 * restitution. Dynamic bodies move by forces and contacts, kinematic bodies by their velocity
 * (infinite mass), static bodies never. The first step allocates the collision and solver state;
 * later steps allocate nothing. An invalid `dt` sets DE_ERR_INVALID_ARGUMENT and does nothing.
 */
DE_API void de_physics_step(de_physics_world *p, f32 dt); /* manual stepping (tests, tools) */
/**
 * XXH3-64 of the simulation state: the tick, the gravity and substeps, every live body in
 * ascending index order, field by field (never raw struct bytes), then the contact manifolds
 * (with the solver's accumulated impulses) in key order. Equal states give equal values on every
 * platform. 0 for NULL.
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
/**
 * Which bodies may touch. Two bodies a and b collide only if
 * `(a.layer & b.mask) != 0 && (b.layer & a.mask) != 0`, and never when they share a non-zero
 * group (`a.group == b.group && a.group != 0`, e.g. the bones of one ragdoll). Scene queries use
 * the same rule with the query's filter as one side. Independently of the filter, the broadphase
 * never pairs two bodies that are both static or kinematic.
 */
typedef struct de_collision_filter {
    u32 layer;
    u32 mask;
    u32 group;
} de_collision_filter;

/** The filter a body gets when its de_body_desc.filter is all zero: layer 1, every mask bit. */
#define DE_COLLISION_FILTER_DEFAULT_LAYER 1u
#define DE_COLLISION_FILTER_DEFAULT_MASK  0xFFFFFFFFu

/* ---------- Bodies (direct API: tests, tools, and the S23 ECS systems) ---------- */
/** de_body_desc.flags. LOCK_ROT_X/Y/Z: a dynamic body never rotates about that *world* axis
 *  (the solver zeroes those rows and columns of its world inverse inertia and that component of
 *  its angular velocity). */
typedef enum de_body_flags {
    DE_BODY_FLAG_CCD = 1u << 0,
    DE_BODY_FLAG_ALLOW_SLEEP = 1u << 1,
    DE_BODY_FLAG_LOCK_ROT_X = 1u << 2,
    DE_BODY_FLAG_LOCK_ROT_Y = 1u << 3,
    DE_BODY_FLAG_LOCK_ROT_Z = 1u << 4,
    DE_BODY_FLAG_TRIGGER = 1u << 5
} de_body_flags;

/*
 * Continuous collision (DENG-219, SPEC §13.1).
 *
 * - Speculative contacts (every body). Each step the broadphase grows a moving body's box by its
 *   motion over the step (velocity * dt, plus its spin), so fast pairs are found before they
 *   touch, and a pair that closes fast keeps contact points up to
 *   clamp(approach speed * dt, physics.contact.speculative (2 cm), physics.ccd.speculative_max
 *   (1 m)) apart; the solver lets such a gap close but no more. A far point is only kept when the
 *   two bodies can really meet during the step, so a body passing close by a corner is not
 *   stopped by it. Bodies faster than speculative_max per step can still pass through thin
 *   geometry: give them DE_BODY_FLAG_CCD.
 * - DE_BODY_FLAG_CCD (bullets; DeRigidBody.ccd). After the solver, an awake dynamic bullet is
 *   swept from its start pose to its end pose (rotation included) against static and kinematic
 *   bodies and dynamic bodies without the flag. If it ends inside or beyond one (or still closing
 *   on it fast), it is put back at its time of impact, 5 mm short of the surface, keeping its
 *   velocity; the next step's contact stops it. Bullets are not swept against each other. Cvar
 *   physics.ccd.enabled (on) switches the sweep off.
 * - All of it is deterministic (fixed body order, bounded iterations) and allocates nothing per
 *   step.
 */

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
 * - `linear_velocity`: of the center of mass (m/s); `angular_velocity`: rad/s about world axes.
 * - `linear_damping`, `angular_damping` (1/s, >= 0 used): each substep h scales the velocity by
 *   1 / (1 + h * damping).
 * - `gravity_scale`: the body feels gravity * gravity_scale. Taken as given: 0 (a zeroed desc)
 *   means no gravity, so set 1 for a normal falling body. Dynamic bodies only.
 * - `flags`: de_body_flags. `owner`: the ECS entity the body belongs to (DE_ENTITY_NULL for none).
 * - `friction_axis`, `friction_secondary` (DENG-217): anisotropic friction. A non-zero
 *   `friction_axis` is a body-local direction (normalized at create) along which
 *   `material.friction` applies; `friction_secondary` (>= 0) applies across it in the contact
 *   plane. Zero (the default) is isotropic. In a contact, the first body with an axis (A, then B)
 *   sets the directions, and each direction mixes the two bodies' coefficients as sqrt(a * b).
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
    de_vec3 friction_axis;  /* DENG-217: body-local; zero = isotropic friction */
    f32 friction_secondary; /* DENG-217: friction across friction_axis */
} de_body_desc;

/**
 * Creates a body in the preallocated pool: the most recently freed index first (LIFO), else the
 * next unused one. Allocates nothing. On failure returns an invalid handle and sets the error
 * context: DE_ERR_INVALID_ARGUMENT (bad type, stale shape, non-finite transform or velocity,
 * negative mass, negative or non-finite friction, restitution or friction_secondary, non-finite
 * friction axis, a dynamic body without a volume shape) or DE_ERR_OUT_OF_MEMORY (pool full).
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
/** Linear velocity of the center of mass (m/s) and angular velocity (rad/s, world axes); either
 *  output may be NULL. false for a stale handle. */
DE_API bool de_body_get_velocity(const de_physics_world *p, de_body b, de_vec3 *linear,
                                 de_vec3 *angular);
/** Sets both velocities. false for a stale handle or a non-finite value. Static bodies keep zero
 *  velocity. */
DE_API bool de_body_set_velocity(de_physics_world *p, de_body b, de_vec3 linear, de_vec3 angular);
/** Mass properties of a live body in its local frame (mass 0 for static and kinematic bodies,
 *  which have infinite mass); false for a stale handle. */
DE_API bool de_body_get_mass_props(const de_physics_world *p, de_body b, de_mass_props *out);

/* ---------- Sleeping (DENG-218) ----------
 * Bodies touching each other through contacts or joints form islands (static and kinematic bodies
 * never join one). An island falls asleep when every body has DE_BODY_FLAG_ALLOW_SLEEP and has
 * moved slower than 0.05 m/s and 0.035 rad/s (cvars physics.sleep.linear_threshold,
 * physics.sleep.angular_threshold) for 0.5 s (physics.sleep.time), and no moving kinematic body
 * touches it. Sleeping bodies keep their pose, have zero velocity and cost no solver work. The
 * whole island wakes at the next step when an awake body or a moving kinematic body starts
 * touching it (or a joint links it), when the user gives one of its bodies a velocity or another
 * transform (de_body_set_velocity, de_body_set_transform), when a body of the island or a body
 * touching it is destroyed, or with de_body_wake. Without DE_BODY_FLAG_ALLOW_SLEEP a body never
 * sleeps, and neither does any island it belongs to. */

/** Wakes a sleeping body and its whole island (they move again from the next step). No effect on
 *  an awake body or a stale handle. */
DE_API void de_body_wake(de_physics_world *p, de_body b);
/** true when the body is asleep and no wake is pending for it; false for a stale handle. */
DE_API bool de_body_is_sleeping(const de_physics_world *p, de_body b);

/* ---------- Queries ---------- */
/*
 * Scene queries (DENG-137, SPEC §13.1): rays, shape casts, overlaps and closest points against
 * the bodies of a world, filtered like collisions.
 *
 * - Filter: a body is a candidate when it is alive, has a shape and passes the collision rule of
 *   de_collision_filter with the query's filter as the other side. An all-zero query filter means
 *   the body default {DE_COLLISION_FILTER_DEFAULT_LAYER, DE_COLLISION_FILTER_DEFAULT_MASK, 0}.
 *   Trigger bodies (DE_BODY_FLAG_TRIGGER) are skipped unless
 *   DE_PHYSICS_QUERY_INCLUDE_TRIGGERS is set. The body-type rule of the broadphase does not apply:
 *   queries see static bodies too.
 * - Rays and casts: `dir` need not be unit length (it is normalized in f64); `max_dist` > 0.
 *   `fraction` is in [0, 1] of `max_dist`. `normal` is unit and faces the ray (against the
 *   motion of a cast). A ray that starts inside a solid shape (not a mesh or heightfield) hits at
 *   fraction 0 with normal -dir and point = origin; a cast that starts overlapping a body hits it
 *   at fraction 0 with normal -dir and point = the cast's start position. Mesh and heightfield
 *   triangles are two-sided unless DE_PHYSICS_QUERY_IGNORE_BACK_FACES is set (their front side is
 *   the counter-clockwise side; a heightfield's front is +Z).
 * - Ordering: the closest hit wins, ties to the lower body index; raycast_all keeps one hit per
 *   body (its nearest) sorted by fraction, then body index; overlap results are sorted by body
 *   index. When an output array is full, the first entries of that order are kept. Within one
 *   body, ties go to the lower sub-shape.
 * - `sub_shape`: mesh triangle index; heightfield triangle 2 * (row * (cols - 1) + col) + k;
 *   compound child index; DE_SUB_SHAPE_NONE for the other kinds.
 * - Large worlds: the query origin is f64, and shapes are tested in their own frame with the
 *   offset formed in f64 first, so a query 100 km out behaves like one at the origin.
 * - Determinism (SPEC §5.7): de_dmath only, traversal and tie rules fixed, results independent
 *   of the broadphase tree shape. Queries never allocate (caller arrays, fixed stacks).
 * - Threading: queries only read the world; any number may run at once, but not during
 *   de_physics_step or any other write. Once the broadphase exists, queries walk its trees, so a
 *   body moved by de_body_set_transform is found at its new place only after the broadphase has
 *   followed it (its next update); until then the tree still holds its old box.
 */

/** de_ray_hit.sub_shape of a shape without sub-shapes (sphere, capsule, box, cylinder, hull). */
#define DE_SUB_SHAPE_NONE 0xFFFFFFFFu

/** de_physics_query_filter.flags. */
typedef enum de_physics_query_flags {
    /** Also report trigger bodies. */
    DE_PHYSICS_QUERY_INCLUDE_TRIGGERS = 1u << 0,
    /** Rays and casts pass through the back faces of mesh and heightfield triangles. */
    DE_PHYSICS_QUERY_IGNORE_BACK_FACES = 1u << 1
} de_physics_query_flags;

/**
 * Full query filter for the *_ex functions. `(de_physics_query_filter){0}` is the default filter,
 * no flags and no ignored body. A NULL de_physics_query_filter pointer is the same.
 */
typedef struct de_physics_query_filter {
    de_collision_filter filter; /**< all zero = the body default (see above) */
    u32 flags;                  /**< de_physics_query_flags */
    de_body ignore_body;        /**< never reported (e.g. the caster itself); invalid = none */
} de_physics_query_filter;

/**
 * One hit. `point` is in world space: the hit point on the body's surface (ray, cast), or the
 * closest point (closest_point). `fraction` is the distance along the ray or cast, or the
 * closest-point distance, divided by `max_dist`. `entity` is the body's owner (DE_ENTITY_NULL for
 * bodies outside the ECS).
 */
typedef struct de_ray_hit {
    de_entity entity;
    de_body body;
    de_dvec3 point;
    de_vec3 normal;
    f32 fraction;
    u32 sub_shape;
} de_ray_hit;

/**
 * Closest hit of the segment origin .. origin + normalize(dir) * max_dist. Writes *hit and
 * returns true on a hit; returns false (and leaves *hit unchanged) on a miss or for invalid
 * arguments (NULL world or hit, non-finite input, zero dir, max_dist <= 0).
 */
DE_API bool de_physics_raycast(const de_physics_world *p, de_dvec3 origin, de_vec3 dir,
                               f32 max_dist, de_collision_filter filter, de_ray_hit *hit);
/**
 * Every body the segment hits, one hit per body (its nearest), sorted by fraction then body
 * index. Writes at most `max_hits` (the first ones of that order) and returns how many it wrote.
 */
DE_API u32 de_physics_raycast_all(const de_physics_world *p, de_dvec3 origin, de_vec3 dir,
                                  f32 max_dist, de_collision_filter filter, de_ray_hit *hits,
                                  u32 max_hits);
/**
 * Sweeps `shape` (convex, or a compound of convex children; not a mesh or heightfield) from
 * `from` (position and rotation; scale ignored) along normalize(dir) * max_dist without
 * rotating, and reports the first body it touches: fraction = time of impact, point on the
 * body's surface, normal facing the cast. The shape need not belong to a body. Time of impact
 * is found to about 1e-5 m (conservative advancement, so never past the contact).
 */
DE_API bool de_physics_shape_cast(const de_physics_world *p, de_shape shape,
                                  const de_transform *from, de_vec3 dir, f32 max_dist,
                                  de_collision_filter filter, de_ray_hit *hit);
/**
 * Bodies whose shape overlaps (or touches) `shape` placed at `at` (convex or a compound of
 * convex children). Writes the owner entities of at most `max` bodies, in ascending body index
 * (the lowest indices when there are more), and returns how many it wrote.
 */
DE_API u32 de_physics_overlap(const de_physics_world *p, de_shape shape, const de_transform *at,
                              de_collision_filter filter, de_entity *out, u32 max);

/* Queries block (DENG-137 additions): closest point, full filter, query flags. */

/**
 * The point of any body's shape (a solid: inside counts) closest to `point`, within `max_dist`
 * (>= 0). On a hit: `point` = that closest point, `fraction` = distance / max_dist (0 when
 * max_dist is 0), `normal` = unit direction from the closest point toward `point` (a triangle's
 * front normal when `point` lies on it; the zero vector when `point` is inside a solid), and
 * `sub_shape`. Ties: lower body index, then lower sub-shape.
 */
DE_API bool de_physics_closest_point(const de_physics_world *p, de_dvec3 point, f32 max_dist,
                                     de_collision_filter filter, de_ray_hit *hit);

/** de_physics_raycast with a full filter (NULL = defaults). */
DE_API bool de_physics_raycast_ex(const de_physics_world *p, de_dvec3 origin, de_vec3 dir,
                                  f32 max_dist, const de_physics_query_filter *filter,
                                  de_ray_hit *hit);
/** de_physics_raycast_all with a full filter (NULL = defaults). */
DE_API u32 de_physics_raycast_all_ex(const de_physics_world *p, de_dvec3 origin, de_vec3 dir,
                                     f32 max_dist, const de_physics_query_filter *filter,
                                     de_ray_hit *hits, u32 max_hits);
/** de_physics_shape_cast with a full filter (NULL = defaults). */
DE_API bool de_physics_shape_cast_ex(const de_physics_world *p, de_shape shape,
                                     const de_transform *from, de_vec3 dir, f32 max_dist,
                                     const de_physics_query_filter *filter, de_ray_hit *hit);
/**
 * de_physics_overlap with a full filter (NULL = defaults). Writes body handles to `bodies` and
 * owner entities to `entities` (either may be NULL), at most `max` of each, ascending body index.
 */
DE_API u32 de_physics_overlap_ex(const de_physics_world *p, de_shape shape, const de_transform *at,
                                 const de_physics_query_filter *filter, de_body *bodies,
                                 de_entity *entities, u32 max);
/** de_physics_closest_point with a full filter (NULL = defaults). */
DE_API bool de_physics_closest_point_ex(const de_physics_world *p, de_dvec3 point, f32 max_dist,
                                        const de_physics_query_filter *filter, de_ray_hit *hit);

/* ---------- Joints ---------- */
/*
 * Joints (DENG-220, SPEC §13.1). A joint holds two bodies (or a body and the world) together.
 * Every joint is a soft-step constraint solved with the contacts (joints first, then contacts).
 *
 * Frames. `frame_a` is the joint frame on side A and `frame_b` on side B. On a body, the frame is
 * in the body's local frame: position relative to the body origin (not the center of mass),
 * rotation relative to the body. On the world side, it is in world space (f64 position). Scale is
 * ignored; a zero rotation is the identity. The joint pulls frame B onto frame A. Its axes are
 * the axes of frame A: X is the primary axis (hinge axis, slider axis, twist axis), Y and Z are
 * the two others. Axis indices 0..2 are translations along X, Y, Z; 3..5 are rotations: 3 is the
 * twist about X, 4 and 5 are the swing about Y and Z (a swing-twist split of B's rotation in A).
 *
 * Kinds:
 * - DE_JOINT_FIXED: every degree of freedom locked.
 * - DE_JOINT_HINGE: anchors together, X axes aligned; free rotation about X. With `enable_limit`
 *   the angle stays in [limits_min[0], limits_max[0]] (radians, within [-pi, pi]). Motor: axis 0.
 * - DE_JOINT_BALL: anchors together; rotation free.
 * - DE_JOINT_SLIDER: rotation locked, B's anchor on A's X axis; free translation along X. With
 *   `enable_limit` the offset stays in [limits_min[0], limits_max[0]] (m). Motor: axis 0.
 * - DE_JOINT_DISTANCE: the distance between the anchors stays in [limits_min[0], limits_max[0]]
 *   (m; min 0 is a rope). Both 0: the distance at create (a rod). `hertz` > 0 makes the range a
 *   spring. Rotation free. Motor: axis 0 (along the anchor line).
 * - DE_JOINT_CONE_TWIST: anchors together; B's X axis stays in an elliptical cone around A's X
 *   axis with half-angles limits_max[4] (swing about Y) and limits_max[5] (swing about Z), and the
 *   twist stays in [limits_min[3], limits_max[3]]. Angles in radians. Motor: axis 3 (twist).
 *   Shoulders and hips of ragdolls.
 * - DE_JOINT_6DOF: each axis is DE_JOINT_AXIS_LOCKED (at 0), _LIMITED ([limits_min[i],
 *   limits_max[i]]) or _FREE (`axis_mode[i]`; a zeroed desc locks all six, like DE_JOINT_FIXED).
 *   Motors on any axis that is not locked. Swing limits apply to each swing component separately.
 *
 * Motors (`motor_mode[i]`, de_joint_motor_mode): a velocity motor drives the joint speed along
 * axis i to motor_target[i] (m/s or rad/s); a position motor drives the joint coordinate to
 * motor_target[i] (m or rad) like a spring of `motor_hertz` and `motor_damping_ratio` (0: as stiff
 * as a locked axis). Either uses at most motor_max_force[i] (N or N m); 0 disables the motor.
 *
 * Softness: `hertz` > 0 and `damping_ratio` make the locked axes and limits a spring; 0 is a rigid
 * joint. Rigid joints are soft-step constraints of a quarter of the substep rate (60 Hz at 60 Hz
 * and 4 substeps; cvars physics.joints.hertz, physics.joints.damping_ratio), so a joint stretches
 * under load by about load / (effective mass * (2 pi hertz)^2): a hanging chain of 20 links sags
 * about 2.6 mm per joint at the top with 4 substeps and 0.7 mm with 8.
 *
 * Breaking: with `break_force` > 0 the joint breaks when the force on its anchors (linear
 * constraint impulse / substep) exceeds it in a step. A broken joint no longer constrains; it stays
 * valid until it is destroyed (de_joint_is_broken, de_physics_joint_breaks).
 *
 * `collide_connected` false (the default): the two bodies never collide while the joint exists.
 *
 * Bodies: `body_a` / `body_b` are the bodies of the joint. An invalid handle is the world, unless
 * the entity `a` / `b` is set: entity sides are resolved by the ECS systems (DENG-223), which
 * fill the body handles. Both sides the world, or the same body twice, are errors. Destroying a
 * body destroys its joints. Creating or destroying a joint wakes both bodies.
 *
 * Determinism (SPEC §5.7): joints are solved in joint index order (graph-colored with the rest).
 * Memory: the first de_joint_create allocates the joint pool (cvar physics.joints.max_joints;
 * 0 = between 64 and 4096, at most max_bodies); later creates and steps allocate nothing.
 */

/** de_joint_desc.motor_mode[i]. */
typedef enum de_joint_motor_mode {
    DE_JOINT_MOTOR_OFF = 0,  /**< no motor */
    DE_JOINT_MOTOR_VELOCITY, /**< drive the speed along the axis to motor_target */
    DE_JOINT_MOTOR_POSITION  /**< drive the coordinate along the axis to motor_target */
} de_joint_motor_mode;

/** de_joint_desc.axis_mode[i] (DE_JOINT_6DOF). */
typedef enum de_joint_axis_mode {
    DE_JOINT_AXIS_LOCKED = 0, /**< held at 0 */
    DE_JOINT_AXIS_LIMITED,    /**< kept in [limits_min[i], limits_max[i]] */
    DE_JOINT_AXIS_FREE        /**< not constrained */
} de_joint_axis_mode;

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
    /* DENG-220 (appended): see the Joints comment above. */
    de_body body_a, body_b;               /* invalid = the world (or the entity, DENG-223) */
    f32 hertz, damping_ratio;             /* softness of locked axes and limits; 0 = rigid */
    de_joint_motor_mode motor_mode[6];    /* per axis; OFF by default */
    f32 motor_hertz, motor_damping_ratio; /* position motors; 0 = stiff */
    de_joint_axis_mode axis_mode[6];      /* DE_JOINT_6DOF only; LOCKED by default */
    bool enable_limit;                    /* hinge, slider: limit axis 0 */
    bool collide_connected;               /* false: the two bodies never collide */
} de_joint_desc;
/**
 * Creates a joint (see the Joints comment). Allocates the joint pool on first use only. Wakes
 * both bodies. On failure returns an invalid handle and sets the error context:
 * DE_ERR_INVALID_ARGUMENT (unknown kind, stale body, both sides the world or the same body, an
 * entity side without a body, non-finite frame, limits min > max where they apply, negative
 * force, hertz or damping), DE_ERR_OUT_OF_MEMORY (pool full or not allocatable) or
 * DE_ERR_ALREADY_EXISTS (another joint class is registered with the solver).
 */
DE_API de_joint de_joint_create(de_physics_world *p, const de_joint_desc *desc);
/** Destroys a joint and wakes its bodies; stale handles are ignored. */
DE_API void de_joint_destroy(de_physics_world *p, de_joint j);

/* Joints block (DENG-220 additions). */

/** true if `j` is a live joint of `p` (broken joints are live until destroyed). */
DE_API bool de_joint_valid(const de_physics_world *p, de_joint j);
/** true when the joint broke (break_force exceeded); false for a stale handle. */
DE_API bool de_joint_is_broken(const de_physics_world *p, de_joint j);
/** Sets motor_target[axis] (axis 0..5); wakes the bodies when the value changes. false for a
 *  stale handle, a bad axis or a non-finite target. */
DE_API bool de_joint_set_motor_target(de_physics_world *p, de_joint j, u32 axis, f32 target);

/** A joint that broke in the last step. */
typedef struct de_joint_break_event {
    de_joint joint;
    de_body body_a, body_b; /**< invalid for the world */
    de_entity a, b;         /**< de_joint_desc.a / .b */
    f32 force;              /**< the force that broke it (N) */
} de_joint_break_event;

/**
 * The joints that broke in the last de_physics_step, in joint index order. Writes at most `max`
 * events (out may be NULL when max is 0) and returns how many joints broke.
 */
DE_API u32 de_physics_joint_breaks(const de_physics_world *p, de_joint_break_event *out, u32 max);

/* ---------- ECS components ---------- */
typedef struct DeRigidBody {
    de_body_type type;
    f32 mass; /* 0 = from shape density */
    de_vec3 linear_velocity, angular_velocity;
    f32 linear_damping, angular_damping;
    f32 gravity_scale;
    bool ccd, allow_sleep, lock_rot_x, lock_rot_y, lock_rot_z;
    de_body body; /* internal, set by the physics system */
    /* DENG-223 (appended): true = physics.interpolate leaves this body's DeGlobalTransform at the
     * simulated pose (no fixed-step interpolation). */
    bool no_interpolate;
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

/* ---------- Character controller (DENG-221) ----------
 * A kinematic capsule character controller (SPEC §13.1). Each character is a kinematic body
 * with an upright capsule shape (the "proxy"), so dynamic bodies collide with it. The controller
 * itself only uses the scene queries and the body API:
 *
 * - de_character_move() runs once per tick, before de_physics_step(). It depenetrates the
 *   capsule from static and kinematic bodies, applies the game's intent, collides and slides
 *   (shape casts, at most 4 iterations), climbs steps, snaps to the ground, follows moving
 *   platforms and pushes dynamic bodies. It then sets the proxy's velocity so that the next
 *   de_physics_step() carries the proxy exactly to the returned pose.
 * - Position: the proxy's position is the center of the capsule. Up is world +Z; the capsule
 *   never tilts, and its yaw only follows rotating platforms.
 * - DeCharacter fields: `radius` and `height` (the total height, at least 2 * radius) size the
 *   capsule at create; later changes are ignored (no resize). `step_height` (m), `max_slope_rad`
 *   (the steepest walkable slope, below pi/2) and `skin_width` (the gap kept to surfaces, m) are
 *   read on every move. A field that is 0 takes its DE_CHARACTER_DEFAULT_* value.
 * - Input: `desired_velocity` (m/s, world). Its x and y are the walking velocity, applied as
 *   given (the game owns acceleration and air control). A positive z jumps when the character
 *   is grounded: the vertical velocity becomes desired_velocity.z. While airborne, the
 *   controller integrates the world gravity's z component into the vertical velocity.
 * - Outputs: `velocity` (the character's own displacement over dt, without depenetration and
 *   platform carry; its z is also the vertical speed carried into the next airborne tick),
 *   `grounded`, `ground_normal` (zero when airborne), `ground_entity` (the owner of the ground
 *   body, DE_ENTITY_NULL when airborne or for bodies outside the ECS). `grounded` is also an
 *   input: ground snapping and stepping run only when the character was grounded the tick before.
 * - Slopes: a surface is walkable when normal.z >= cos(max_slope_rad). A capsule resting on an
 *   edge (a stair nose) counts as supported when the face just inside the edge is walkable.
 *   Steeper surfaces act as walls for walking (no climbing by projection) and the character slides
 *   down them under gravity. Projection never adds speed.
 * - Pushing: a dynamic body hit while moving gets an impulse at the contact point, at most what
 *   makes it match the character's speed into it (shared by the mass ratio with the character
 *   mass, cvar `physics.character.mass`, 80 kg) and at most push force * dt (cvar
 *   `physics.character.push_force`, 1000 N). Each body is pushed at most once per move, in hit
 *   order. Static and kinematic bodies are never pushed.
 * - Determinism (SPEC §5.7): de_dmath only, fixed iteration caps, query results in their fixed
 *   order (fraction, then body index). No allocation per move.
 */

/** Defaults of the DeCharacter parameters that are 0. */
#define DE_CHARACTER_DEFAULT_RADIUS        0.3f
#define DE_CHARACTER_DEFAULT_HEIGHT        1.8f
#define DE_CHARACTER_DEFAULT_STEP_HEIGHT   0.3f
#define DE_CHARACTER_DEFAULT_MAX_SLOPE_RAD 0.785398163f /* 45 degrees */
#define DE_CHARACTER_DEFAULT_SKIN_WIDTH    0.02f

/**
 * Creates a character: a capsule shape of `c->radius` and `c->height` and a kinematic body with
 * it, centered at `at->position` (rotation: its yaw is kept, tilt is not; scale ignored), owned by
 * `owner` (DE_ENTITY_NULL for none). The body uses the default collision filter. Create-time
 * call: it may allocate (the shape, and the controller state on the first character of a world).
 * On failure returns an invalid handle and sets the error context: DE_ERR_INVALID_ARGUMENT (NULL
 * or non-finite input, negative parameters, height < 2 * radius, max_slope_rad >= pi/2) or
 * DE_ERR_OUT_OF_MEMORY.
 */
DE_API de_body de_character_create(de_physics_world *p, const DeCharacter *c,
                                   const de_transform *at, de_entity owner);
/** Destroys a character's proxy body and its capsule shape. Stale handles are ignored. */
DE_API void de_character_destroy(de_physics_world *p, de_body character);
/**
 * Moves a character for one tick of `dt` seconds (see the block comment): reads the parameters
 * and `desired_velocity` of `io`, writes its outputs, sets the proxy's velocity for the next
 * de_physics_step() and writes the pose the proxy will have after that step to `out` (may be
 * NULL). Call it once per tick, before de_physics_step(). Allocates nothing.
 * @return DE_OK, or DE_ERR_INVALID_ARGUMENT (NULL world or io, a body that is not a character,
 *         dt not > 0 and finite, non-finite or negative parameters); nothing changes then.
 */
DE_API de_result de_character_move(de_physics_world *p, de_body character, DeCharacter *io, f32 dt,
                                   de_transform *out);

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

/* ---------- ECS integration (DENG-223) ----------
 * de_physics_attach() adds two systems to the ECS world (SPEC §13.1, §10.2):
 *
 * DE_PHYSICS_STEP_SYSTEM ("physics.step"), DE_SCHEDULE_FIXED_UPDATE, DE_SYSTEM_DETERMINISTIC.
 * It declares write access to DeRigidBody, DeCharacter, DeTransform and the DePhysicsWorld
 * resource, read access to DeCollider, and sends the two event types below. Each run (one tick of
 * the fixed dt):
 *   1. Removes the bodies of entities that were despawned or lost their components, in entity
 *      index order.
 *   2. Syncs ECS to physics, in archetype / row order of the changed components:
 *      - An entity with DeRigidBody + DeCollider gets a body of DeRigidBody.type; DeCollider
 *        without DeRigidBody gets a static body; DeCharacter (with or without the others) gets a
 *        character (de_character_create, owner = the entity). DeTransform (optional; identity when
 *        absent) is the world pose of the body origin (the character's capsule center); its
 *        scale is ignored. Entities with a parent are not supported (DeTransform is read as the
 *        world pose). A non-identity DeCollider.offset puts the shape in a one-child compound that
 *        the system owns. DeRigidBody.body is set to the body. Bodies are created the tick the
 *        components are seen, so an entity spawned before the run has its body after it.
 *      - Changing DeRigidBody.type or .mass, or any DeCollider field, re-creates the body (same
 *        pose and velocity, new handle; contacts end and begin again). Damping, gravity_scale,
 *        ccd, allow_sleep and the rotation locks are applied in place.
 *      - A DeRigidBody velocity or a DeTransform pose that a game changed (and that differs from
 *        the body's) is pushed into the body: a teleport for dynamic and static bodies and
 *        characters; for a kinematic body the pose is a target, reached exactly at the end of the
 *        tick (its velocity is set for the step, then back to DeRigidBody's velocities).
 *        Unchanged values are never written, so sleeping bodies stay asleep.
 *   3. Moves every character (de_character_move with DeCharacter, in query order).
 *   4. de_physics_step(dt).
 *   5. Writes the pose of every dynamic and kinematic body and character back to DeTransform, and
 *      the velocities of dynamic bodies to DeRigidBody, only when they changed.
 *   6. Sends the contact events of the step, then the joint break events.
 * Game systems in FixedUpdate that write these components run after physics.step unless they
 * name it in de_system_desc.before (systems that conflict run in registration order).
 *
 * DE_PHYSICS_INTERPOLATE_SYSTEM ("physics.interpolate"), DE_SCHEDULE_POST_UPDATE, after
 * DE_TRANSFORM_PROPAGATE_SYSTEM. Presentation only (not deterministic): for every dynamic and
 * kinematic body (unless DeRigidBody.no_interpolate) and character without a parent, it writes
 * DeGlobalTransform = the pose between the last two ticks, at the frame's alpha (position lerp in
 * f64, rotation nlerp; alpha >= 1, as in variable timing, gives the last tick's pose exactly),
 * and recomputes the DeGlobalTransform of its descendants. It never writes DeTransform or any
 * other component FixedUpdate reads, so the simulation is the same with or without it.
 *
 * Contact events (de_contact_event), one per body pair and transition, in body-pair order:
 *   - DE_CONTACT_BEGIN: the pair's first contact manifold (the shapes came within the
 *     speculative margin). `a` / `b` are the owners of the lower / higher body index, `normal`
 *     points from a to b, `point` is the mean of the contact points (world, f64) and `impulse`
 *     the pair's summed normal impulse of the step (N s).
 *   - DE_CONTACT_END: the pair's last manifold ended (they separated, or a body was destroyed:
 *     its entity may be dead). point, normal and impulse are zero.
 *   - DE_TRIGGER_ENTER / DE_TRIGGER_EXIT: the same for pairs with a trigger (DeCollider.is_trigger)
 *     body, which never get a collision response. `point` is the other body's origin (b's when
 *     both are triggers); normal and impulse are zero.
 * An END sorts before a BEGIN of the same pair. Bodies outside the ECS have DE_ENTITY_NULL;
 * pairs where both are DE_ENTITY_NULL send nothing.
 *
 * Joint breaks: each de_joint_break_event of de_physics_joint_breaks() after the step, with `a` /
 * `b` filled from the bodies' owners when the joint was made with body handles.
 *
 * Entity joints: de_joint_create() resolves de_joint_desc.a / .b (when body_a / body_b are
 * invalid) to the entities' bodies; an entity has a body once physics.step has run after it got
 * its components (de_physics_entity_body). Despawning an entity destroys its body and its joints.
 *
 * DeRigidBody added through the ECS is constructed with gravity_scale 1 and allow_sleep true
 * (type static, everything else 0).
 *
 * Lifetime: destroy the ECS world before the physics world (the systems point to it).
 */

/** Name of the FixedUpdate physics system (de_system_desc.before / after). */
#define DE_PHYSICS_STEP_SYSTEM "physics.step"
/** Name of the PostUpdate interpolation system. */
#define DE_PHYSICS_INTERPOLATE_SYSTEM "physics.interpolate"

/** The event type of de_contact_event in the attached ECS world (invalid before attach). */
DE_API de_event_type de_physics_contact_event_type(const de_physics_world *p);
/** The event type of de_joint_break_event in the attached ECS world (invalid before attach). */
DE_API de_event_type de_physics_joint_break_event_type(const de_physics_world *p);
/** The body physics.step made for entity `e` (rigid body, static collider or character), or an
 *  invalid handle (no body yet, or not attached). */
DE_API de_body de_physics_entity_body(const de_physics_world *p, de_entity e);

/* ---------- Ragdolls (DENG-224) ----------
 * A ragdoll is a tree of bones (SPEC §13.4; DENG-2 Q14: the one optional physics feature of v1).
 * Each bone is a dynamic body with its own capsule, box or sphere shape, and each bone except
 * the root is held to its parent by a DENG-220 joint: cone-twist (the default; shoulders, hips,
 * spine, neck) or hinge (knees, elbows). Nothing else is simulated: a ragdoll is a way to build
 * and address those bodies and joints.
 *
 * - Bones: `bones[0]` is the root (parent DE_RAGDOLL_NO_PARENT); every other bone names a parent
 *   with a lower index. `bind` is the bone's pose in model space; de_ragdoll_desc.transform places
 *   the model in the world. A bone's origin is the joint with its parent.
 * - Shapes: built by the ragdoll (owned and destroyed with it), placed in the bone's frame by
 *   `shape_offset` (zero rotation = identity). The bone's body sits at the shape, so its mass
 *   properties are the shape's. Capsule axis: the shape's local Z.
 * - Joints: at the bone origin. `joint_rotation` (bone frame; zero = identity) orients the joint:
 *   its X axis is the twist axis of a cone-twist or the axis of a hinge. Limits are relative to the
 *   bind pose and always on: cone-twist twist in [limit_min, limit_max] about X and an elliptical
 *   swing cone of half-angles swing_y (about Y) and swing_z (about Z); hinge angle in
 *   [limit_min, limit_max]. Radians, within [-pi, pi]. Joints are rigid (DENG-220 softness).
 * - Self-collision: the bones of a ragdoll never collide with each other. Unless `self_collision`
 *   is set, they share one non-zero collision group: de_ragdoll_desc.filter.group, or (0) an
 *   automatic group with DE_RAGDOLL_AUTO_GROUP_BIT set (games keep their own groups below it).
 *   With `self_collision` the group is filter.group as given (0 = none): jointed pairs (parent and
 *   child) stay excluded by their joints, and non-adjacent bones collide unless their layers and
 *   masks (de_ragdoll_bone.filter) keep them apart.
 * - Powered ragdolls: a bone with motor_max_force > 0 (N m) is driven toward a target rotation
 *   relative to its parent (de_ragdoll_set_motor_targets; the bind pose until set). A hinge uses
 *   its own position motor; a cone-twist bone gets a second joint, a 6-DOF whose angular axes are
 *   free with position motors on twist and both swings. motor_hertz / motor_damping_ratio
 *   (0 = as stiff as a rigid joint) shape the motors.
 * - Activation: every bone starts with the rigid-body velocity of de_ragdoll_desc.linear_velocity
 *   (of the root bone's center of mass) and angular_velocity.
 * - Determinism (SPEC §5.7): bodies are created in bone order, then joints in bone order (each
 *   bone's joint, then its motor joint), so equal inputs give equal body and joint indices.
 *   de_dmath only. Create and destroy allocate (shapes, the ragdoll record); the getters and
 *   setters do not, and a ragdoll adds no work to the step besides its bodies and joints.
 * - Threading: like the body API (create, destroy and setters single-threaded).
 */

DE_HANDLE(de_ragdoll);

/** Most bones of one ragdoll. */
#define DE_RAGDOLL_MAX_BONES 128u
/** de_ragdoll_bone.parent of the root bone. */
#define DE_RAGDOLL_NO_PARENT 0xFFFFFFFFu
/** Set in every automatic ragdoll collision group. */
#define DE_RAGDOLL_AUTO_GROUP_BIT 0x80000000u

typedef enum de_ragdoll_shape {
    DE_RAGDOLL_SHAPE_CAPSULE = 0, /**< radius, half_height (axis along the shape's local Z) */
    DE_RAGDOLL_SHAPE_BOX,         /**< half_extents */
    DE_RAGDOLL_SHAPE_SPHERE       /**< radius */
} de_ragdoll_shape;

typedef enum de_ragdoll_joint {
    DE_RAGDOLL_JOINT_CONE_TWIST = 0, /**< swing cone + twist range (the default) */
    DE_RAGDOLL_JOINT_HINGE           /**< one axis, angle range */
} de_ragdoll_joint;

/** One bone of a de_ragdoll_desc (see the Ragdolls comment). */
typedef struct de_ragdoll_bone {
    u32 parent;        /**< lower bone index; DE_RAGDOLL_NO_PARENT for bones[0] only */
    de_transform bind; /**< model-space bind pose (scale ignored; zero rotation = identity) */
    de_ragdoll_shape shape;
    f32 radius, half_height;   /**< capsule, sphere */
    de_vec3 half_extents;      /**< box */
    de_transform shape_offset; /**< shape frame in the bone frame (scale ignored) */
    f32 mass;                  /**< kg; 0 = from the shape and de_ragdoll_desc.material.density */
    de_ragdoll_joint joint;    /**< joint to the parent (ignored for the root) */
    de_quat joint_rotation;    /**< joint frame in the bone frame; X = twist / hinge axis */
    f32 limit_min, limit_max;  /**< cone-twist: twist range; hinge: angle range (rad) */
    f32 swing_y, swing_z;      /**< cone-twist: swing half-angles about Y and Z (rad, >= 0) */
    f32 motor_max_force;       /**< N m; 0 = not powered */
    /** A non-zero layer or mask replaces the ragdoll's layer and mask for this bone; a non-zero
     *  group replaces its group. */
    de_collision_filter filter;
} de_ragdoll_bone;

/** de_ragdoll_create() parameters (see the Ragdolls comment). */
typedef struct de_ragdoll_desc {
    const de_ragdoll_bone *bones;
    u32 bone_count;          /**< 1..DE_RAGDOLL_MAX_BONES */
    de_transform transform;  /**< model -> world (f64 position; zero rotation = identity) */
    de_vec3 linear_velocity; /**< of the root bone's center of mass at activation (m/s) */
    de_vec3 angular_velocity;
    de_material_phys material;  /**< every bone (density 0 = DE_PHYSICS_DEFAULT_DENSITY) */
    de_collision_filter filter; /**< all zero = default layer and mask, automatic group */
    bool self_collision;        /**< let non-adjacent bones collide (see above) */
    f32 linear_damping, angular_damping;
    u32 flags; /**< de_body_flags for every bone (DE_BODY_FLAG_ALLOW_SLEEP, _CCD) */
    f32 motor_hertz, motor_damping_ratio; /**< powered bones; 0 = stiff */
    de_entity owner;                      /**< owner of every bone body (DE_ENTITY_NULL: none) */
} de_ragdoll_desc;

/**
 * Builds a ragdoll: its shapes, one dynamic body per bone (gravity scale 1), then the joints, in
 * bone order. Allocates. On failure nothing is left behind and the result is invalid, with the
 * error context set: DE_ERR_INVALID_ARGUMENT (NULL or empty bones, too many, a bad parent, a
 * non-finite or negative value, limit_min > limit_max, an unknown shape or joint kind) or
 * DE_ERR_OUT_OF_MEMORY (memory, or the body or joint pool is full).
 */
DE_API de_ragdoll de_ragdoll_create(de_physics_world *p, const de_ragdoll_desc *desc);
/** Destroys the ragdoll's joints, bodies (which wakes what they touched) and shapes. Stale
 *  handles are ignored. */
DE_API void de_ragdoll_destroy(de_physics_world *p, de_ragdoll r);
/** true if `r` is a live ragdoll of `p`. */
DE_API bool de_ragdoll_valid(const de_physics_world *p, de_ragdoll r);
/** Bones of a live ragdoll; 0 for a stale handle. */
DE_API u32 de_ragdoll_bone_count(const de_physics_world *p, de_ragdoll r);
/** The body of bone `bone`; invalid for a stale handle or a bad index. */
DE_API de_body de_ragdoll_bone_body(const de_physics_world *p, de_ragdoll r, u32 bone);
/** The joint between bone `bone` and its parent; invalid for the root. */
DE_API de_joint de_ragdoll_bone_joint(const de_physics_world *p, de_ragdoll r, u32 bone);
/**
 * World pose of every bone (its origin and rotation; scale 1) into out[0..count). `count` must be
 * the bone count. false for a stale handle, a wrong count, or a bone body destroyed elsewhere.
 */
DE_API bool de_ragdoll_get_pose(const de_physics_world *p, de_ragdoll r, de_transform *out,
                                u32 count);
/**
 * Teleports every bone to the world poses bones[0..count) (count = the bone count; scale
 * ignored) and wakes them. Velocities are kept (de_ragdoll_set_velocity). false, and nothing
 * changes, for a stale handle, a wrong count or a non-finite pose.
 */
DE_API bool de_ragdoll_set_pose(de_physics_world *p, de_ragdoll r, const de_transform *bones,
                                u32 count);
/** Gives every bone the rigid-body velocity of `linear` (at the root bone's center of mass) and
 *  `angular`. false for a stale handle or a non-finite value. */
DE_API bool de_ragdoll_set_velocity(de_physics_world *p, de_ragdoll r, de_vec3 linear,
                                    de_vec3 angular);
/**
 * Motor targets of a powered ragdoll: local[i] is bone i's target rotation relative to its parent
 * bone (parent^-1 * bone, as in an animation's local pose; zero = the bind pose). local[0] and
 * bones without a motor are ignored. `count` must be the bone count. A changed target wakes the
 * bone's bodies. false for a stale handle, a wrong count or a non-finite rotation.
 */
DE_API bool de_ragdoll_set_motor_targets(de_physics_world *p, de_ragdoll r, const de_quat *local,
                                         u32 count);

/* ---------- 2D (same design, operates in the configured 2D plane) ---------- */
/*
 * 2D physics (DENG-252 and the rest of S24, SPEC §13.3). Its own world type, de_physics2d_world,
 * built into the same deterministic module as 3D. Bodies and shapes use the de_body and de_shape
 * handle types with the same packing, but a handle belongs to the world that made it: never pass
 * a 2D handle to a 3D function or the reverse.
 *
 * Plane coordinates: 2D physics works in (x, y) of its plane. Positions are f64 (large worlds,
 * like 3D); rotations, velocities and all shape data are f32. Angles are radians, counter-clockwise
 * from +x to +y. How the plane maps into the world (SPEC §12.12: side view = world X-Z, top-down =
 * world X-Y) is de_physics2d_desc.plane; the mapping and the ECS systems come with DENG-260.
 * Mass is in kg, so a density is in kg/m^2 (area density).
 *
 * Determinism and threading: as for 3D (see the 3D notes above). de_physics2d_checksum() hashes
 * the simulation state; bodies are processed in ascending body index.
 */

/** Shape kinds. Circle, capsule, box and polygon are solid (they have area and mass); segment and
 *  chain are lines (static and kinematic bodies only). */
typedef enum de_shape2d_kind {
    DE_SHAPE2D_CIRCLE,
    DE_SHAPE2D_CAPSULE,
    DE_SHAPE2D_BOX,
    DE_SHAPE2D_POLYGON,
    DE_SHAPE2D_CHAIN,
    DE_SHAPE2D_SEGMENT
} de_shape2d_kind;

/** Which world plane the 2D plane is (SPEC §12.12). Used by the plane mapping (DENG-260). */
typedef enum de_physics2d_plane {
    /** Side view: plane x = world X, plane y = world Z (the camera looks along +Y). */
    DE_PHYSICS2D_PLANE_SIDE,
    /** Top-down: plane x = world X, plane y = world Y (the camera looks along -Z). */
    DE_PHYSICS2D_PLANE_TOP_DOWN
} de_physics2d_plane;

/** A rotation in the plane: cos and sin of the angle. (1, 0) is no rotation. */
typedef struct de_rot2d {
    f32 c, s;
} de_rot2d;

/** Position (f64, plane coordinates) and rotation of a 2D body. */
typedef struct de_transform2d {
    f64 x, y;
    de_rot2d rotation;
} de_transform2d;

/** An axis-aligned box in the plane. Empty when min > max. */
typedef struct de_aabb2d {
    de_vec2 min, max;
} de_aabb2d;

/**
 * 2D mass properties: `mass` in kg, `area` in m^2, `center` (center of mass) in the shape's or
 * body's local frame, `inertia` the rotational inertia about the center of mass (kg m^2).
 */
typedef struct de_mass_props2d {
    f32 mass;
    f32 area;
    de_vec2 center;
    f32 inertia;
} de_mass_props2d;

/** Most vertices of a polygon (SPEC §13.3). */
#define DE_SHAPE2D_MAX_POLYGON_VERTICES 8u
/** Most input points of a polygon (its hull must still have at most 8 vertices). */
#define DE_SHAPE2D_MAX_POLYGON_INPUT 256u
/** Most points of a chain. */
#define DE_SHAPE2D_MAX_CHAIN_POINTS 65536u
/** Density used when de_material_phys.density is 0 for a 2D body (kg/m^2). */
#define DE_PHYSICS2D_DEFAULT_DENSITY 1.0f

/** The rotation by `radians` (de_dsincos: deterministic). */
DE_API de_rot2d de_rot2d_from_angle(f32 radians);
/** The angle of `r` in [-pi, pi] (de_datan2). */
DE_API f32 de_rot2d_angle(de_rot2d r);

/**
 * de_physics2d_create() parameters. `(de_physics2d_desc){0}` (or a NULL desc) gives the defaults.
 *
 * - `allocator`: everything the world allocates. NULL = de_heap_allocator(). Must outlive it.
 * - `tasks`: the task system the step may use (DENG-253 on); NULL = single-threaded. Results never
 *   depend on it.
 * - `gravity`: m/s^2 in plane coordinates. The zero vector means the default of the plane:
 *   (0, -9.81) for DE_PHYSICS2D_PLANE_SIDE, none for DE_PHYSICS2D_PLANE_TOP_DOWN. For zero gravity
 *   in a side-view world call de_physics2d_set_gravity() after create.
 * - `substeps`: solver substeps per step (DENG-254); 0 = 4.
 * - `max_bodies`: size of the preallocated body pool; 0 = DE_PHYSICS_DEFAULT_MAX_BODIES, at most
 *   DE_PHYSICS_MAX_BODIES_LIMIT.
 * - `deterministic`: 2D physics is always deterministic; false is treated as true.
 * - `plane`: the world plane (default side view).
 */
typedef struct de_physics2d_desc {
    const de_allocator *allocator;
    de_task_system *tasks;
    de_vec2 gravity; /* default (0, -9.81) for the side plane */
    u32 substeps;    /* default 4 */
    u32 max_bodies;
    bool deterministic; /* default true */
    de_physics2d_plane plane;
} de_physics2d_desc;

/**
 * Creates a 2D physics world and preallocates its body pool.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (`out` NULL, `max_bodies` above the limit, an unknown
 *         plane or a non-finite gravity) or DE_ERR_OUT_OF_MEMORY.
 */
DE_API de_result de_physics2d_create(const de_physics2d_desc *desc, de_physics2d_world **out);
/** Destroys every body and shape and frees all memory of the world. NULL is ignored. */
DE_API void de_physics2d_destroy(de_physics2d_world *p);
/**
 * Advances the world by `dt` seconds (> 0, finite): one tick. Collision (DENG-253): broadphase,
 * narrowphase (on `tasks` when set; the result never depends on it) and persistent contact
 * manifolds. The first step creates the collision state (allocates); later steps allocate nothing.
 * An invalid `dt` sets DE_ERR_INVALID_ARGUMENT and does nothing.
 */
DE_API void de_physics2d_step(de_physics2d_world *p, f32 dt);
/**
 * XXH3-64 of the simulation state: the tick, gravity, substeps, plane, every live body in
 * ascending index order, then (after the first step) the contact manifolds in key order and the
 * contacts that ended in the last step, field by field (never raw struct bytes). Equal states give
 * equal values on every platform. 0 for NULL.
 */
DE_API u64 de_physics2d_checksum(const de_physics2d_world *p);
/** Ticks stepped since create. */
DE_API u64 de_physics2d_tick(const de_physics2d_world *p);
/** Sets the gravity (m/s^2, plane coordinates; any finite vector, zero included). */
DE_API void de_physics2d_set_gravity(de_physics2d_world *p, de_vec2 gravity);
DE_API de_vec2 de_physics2d_get_gravity(const de_physics2d_world *p);
/** The plane the world was created with. */
DE_API de_physics2d_plane de_physics2d_get_plane(const de_physics2d_world *p);

/**
 * de_shape2d_create() parameters. Fill the fields of `kind`; the others are ignored. Shapes are in
 * their own local frame; a body places that frame in the plane.
 *
 * - DE_SHAPE2D_CIRCLE: `radius` > 0 around `center`.
 * - DE_SHAPE2D_CAPSULE: the segment `p1`..`p2` swept by `radius` > 0. p1 == p2 is a circle.
 * - DE_SHAPE2D_BOX: `half_extents` > 0, centered on `center` and rotated by `angle`; a polygon of
 *   4 vertices (rounded by `radius` >= 0).
 * - DE_SHAPE2D_POLYGON: the convex hull of `points[0..point_count)` (3 to
 *   DE_SHAPE2D_MAX_POLYGON_INPUT points), rounded by `radius` >= 0. Points closer than
 *   `hull_tolerance` to an earlier point are dropped (welded), and so are points within it of the
 *   line through their hull neighbours (collinear); 0 picks the largest of 1e-5, 1e-4 times the
 *   longest side of the points' box and 4e-7 times the largest |coordinate|. The hull is stored
 *   counter-clockwise. A hull of more than DE_SHAPE2D_MAX_POLYGON_VERTICES vertices, and points
 *   that are all on one line or at one point, are rejected.
 * - DE_SHAPE2D_SEGMENT: the two-sided segment `p1`..`p2` (p1 != p2). No area: static and kinematic
 *   bodies only.
 * - DE_SHAPE2D_CHAIN: use de_shape2d_chain_create().
 */
typedef struct de_shape2d_desc {
    de_shape2d_kind kind;
    de_vec2 center;       /* circle, box */
    de_vec2 p1, p2;       /* capsule, segment */
    de_vec2 half_extents; /* box */
    f32 angle;            /* box, radians */
    f32 radius;           /* circle, capsule (> 0); box, polygon rounding (>= 0) */
    const de_vec2 *points;
    u32 point_count;    /* polygon */
    f32 hull_tolerance; /* polygon: weld / collinearity tolerance in meters; 0 = automatic */
} de_shape2d_desc;

/**
 * Builds a shape (see de_shape2d_desc). Shapes are immutable and shared: any number of bodies may
 * use one. May allocate. On failure returns an invalid handle and sets the error context:
 * DE_ERR_INVALID_ARGUMENT (bad parameters, degenerate polygon, hull of more than 8 vertices) or
 * DE_ERR_OUT_OF_MEMORY.
 */
DE_API de_shape de_shape2d_create(de_physics2d_world *p, const de_shape2d_desc *desc);
/**
 * Builds a chain: the one-sided segments points[0]..points[1], ...,
 * points[count-2]..points[count-1] and, when `loop`, points[count-1]..points[0]. A segment collides
 * only on its right side (seen along its direction), like the edges of a counter-clockwise polygon:
 * a counter-clockwise loop is solid inside; ground running right to left (-x) collides from above.
 * Each segment keeps its neighbours' far points as ghost vertices for smooth collision (DENG-253);
 * the open ends of a chain have no neighbour and are free corners. Needs 2 (open) or 3 (loop) to
 * DE_SHAPE2D_MAX_CHAIN_POINTS finite points; consecutive points (and, for a loop, the last and the
 * first) must be farther apart than max(1e-5, 4e-7 times the largest |coordinate|) meters. The
 * points are copied. Static and kinematic bodies only. Same failures as de_shape2d_create.
 */
DE_API de_shape de_shape2d_chain_create(de_physics2d_world *p, const de_vec2 *points, u32 count,
                                        bool loop);
/**
 * Destroys a shape. A shape still used by a body is not destroyed: the call returns DE_ERR_BUSY,
 * logs a warning and changes nothing. A stale or invalid handle gives DE_ERR_INVALID_ARGUMENT.
 */
DE_API de_result de_shape2d_destroy(de_physics2d_world *p, de_shape s);
/** true if `s` is a live shape of `p`. */
DE_API bool de_shape2d_valid(const de_physics2d_world *p, de_shape s);
/** The kind of a live shape, or false for a stale handle. */
DE_API bool de_shape2d_get_kind(const de_physics2d_world *p, de_shape s, de_shape2d_kind *out);
/** The shape's bounding box in its local frame (rounding radius included), or false. */
DE_API bool de_shape2d_get_local_aabb(const de_physics2d_world *p, de_shape s, de_aabb2d *out);
/**
 * Mass properties of the shape filled with `density` (kg/m^2; 0 = DE_PHYSICS2D_DEFAULT_DENSITY).
 * Exact for every solid kind, rounded polygons included. Segments and chains: all zero.
 * @return DE_OK or DE_ERR_INVALID_ARGUMENT (stale handle, NULL `out`, negative density).
 */
DE_API de_result de_shape2d_get_mass_props(const de_physics2d_world *p, de_shape s, f32 density,
                                           de_mass_props2d *out);

/* de_body_flags that 2D uses: DE_BODY_FLAG_CCD (bullet, DENG-254), DE_BODY_FLAG_ALLOW_SLEEP,
 * DE_BODY_FLAG_TRIGGER and DE_BODY2D_FLAG_FIXED_ROTATION. The other bits are ignored. */
/** A 2D body that never rotates: its inverse inertia is 0 (de_body_flags bit LOCK_ROT_Z). */
#define DE_BODY2D_FLAG_FIXED_ROTATION DE_BODY_FLAG_LOCK_ROT_Z
/** A fast 2D body that gets swept collision (DENG-254) (de_body_flags bit CCD). */
#define DE_BODY2D_FLAG_BULLET DE_BODY_FLAG_CCD

/**
 * de_body2d_create() parameters. `(de_body2d_desc){0}` is a static body at the origin with no
 * shape.
 *
 * - `transform`: f64 position and rotation of the body origin. The rotation is normalized; (0, 0)
 *   is no rotation.
 * - `shape`: a live shape of the same world, or an invalid handle for no shape. A dynamic body
 *   needs a solid shape (not a segment or a chain).
 * - `mass`: kg; 0 = from the shape area and `material.density`. A positive mass rescales the
 *   shape's inertia to it.
 * - `material`: density in kg/m^2; 0 means DE_PHYSICS2D_DEFAULT_DENSITY.
 * - `filter`: all zero means {DE_COLLISION_FILTER_DEFAULT_LAYER, DE_COLLISION_FILTER_DEFAULT_MASK,
 *   0}.
 * - `flags`: de_body_flags (see above). `owner`: the ECS entity (DE_ENTITY_NULL for none).
 */
typedef struct de_body2d_desc {
    de_body_type type;
    de_transform2d transform;
    de_vec2 linear_velocity;
    f32 angular_velocity; /* rad/s, counter-clockwise */
    de_shape shape;
    f32 mass;
    de_material_phys material;
    de_collision_filter filter;
    f32 linear_damping, angular_damping, gravity_scale;
    u32 flags;
    de_entity owner;
} de_body2d_desc;

/**
 * Creates a body in the preallocated pool: the most recently freed index first (LIFO), else the
 * next unused one. Allocates nothing. On failure returns an invalid handle and sets the error
 * context: DE_ERR_INVALID_ARGUMENT (bad type, stale shape, non-finite transform or velocity,
 * negative mass, a dynamic body without a solid shape) or DE_ERR_OUT_OF_MEMORY (pool full).
 */
DE_API de_body de_body2d_create(de_physics2d_world *p, const de_body2d_desc *desc);
/** Destroys a body; its handle and every copy of it become stale. Stale handles are ignored. */
DE_API void de_body2d_destroy(de_physics2d_world *p, de_body b);
/** true if `b` is a live body of `p`. */
DE_API bool de_body2d_valid(const de_physics2d_world *p, de_body b);
/** Live bodies. */
DE_API u32 de_body2d_count(const de_physics2d_world *p);
/** Position and rotation of a live body; false for a stale handle. */
DE_API bool de_body2d_get_transform(const de_physics2d_world *p, de_body b, de_transform2d *out);
/** Teleports a body (rotation normalized). false for a stale handle or a non-finite transform. */
DE_API bool de_body2d_set_transform(de_physics2d_world *p, de_body b, const de_transform2d *t);
/** Linear (m/s) and angular (rad/s) velocity; either output may be NULL. false for a stale
 *  handle. */
DE_API bool de_body2d_get_velocity(const de_physics2d_world *p, de_body b, de_vec2 *linear,
                                   f32 *angular);
/** Sets both velocities. false for a stale handle or a non-finite value. Static bodies keep zero
 *  velocity. */
DE_API bool de_body2d_set_velocity(de_physics2d_world *p, de_body b, de_vec2 linear, f32 angular);
/** Mass properties of a live body in its local frame (mass 0 for static and kinematic bodies);
 *  false for a stale handle. */
DE_API bool de_body2d_get_mass_props(const de_physics2d_world *p, de_body b, de_mass_props2d *out);

typedef struct DeRigidBody2D {
    de_body_type type;
    f32 mass;
    de_vec2 linear_velocity;
    f32 angular_velocity;
    f32 linear_damping, angular_damping, gravity_scale;
    bool fixed_rotation, bullet;
    de_body body;
} DeRigidBody2D;
/* DeCollider2D.shape: a runtime handle of a shape built outside the component (chains, shared
 * shapes; de_shape2d_chain_create). When it is valid it wins over kind / half_extents / radius /
 * points (DENG-260 binds it). Snapshotted, not serialized. */
typedef struct DeCollider2D {
    de_shape shape;
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

DE_API void de_physics2d_attach(de_physics2d_world *p, de_world *w);
DE_API bool de_physics2d_raycast(const de_physics2d_world *p, de_vec2 origin, de_vec2 dir,
                                 f32 max_dist, de_collision_filter filter, de_entity *hit_entity,
                                 de_vec2 *point, de_vec2 *normal);

DE_EXTERN_C_END
#endif
