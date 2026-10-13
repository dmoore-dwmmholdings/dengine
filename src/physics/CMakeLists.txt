# src/physics: module `physics` (target de_physics), layer gameplay. SPEC §13. Deterministic
# (SPEC §5.7): every source gets -ffp-contract=off (or /fp:precise).
# 2D physics (src/physics/2d/, listed in 2d/CMakeLists.txt) is part of this deterministic module,
# not its own target.
#
# Shared file (append-only within S22): later tasks add their sources in their own block below,
# guarded with if(TARGET de_physics) because the module is skipped when DE_WITH_PHYSICS is off:
#     if(TARGET de_physics)
#         target_sources(de_physics PRIVATE broadphase/aabb_tree.c ...)
#     endif()
de_add_module(
    NAME physics
    LAYER gameplay
    SOURCES
        # DENG-132: world, bodies, shapes, ECS attach, minimal step, checksum.
        physics_internal.h
        world.c
        body.c
        step.c
        checksum.c
        shapes/shape_internal.h
        shapes/shape.c
        shapes/support.c
        shapes/mass.c
        shapes/convex_hull.c
        shapes/mesh_bvh.c
        shapes/heightfield.c
        shapes/compound.c
        ecs/attach.c
    DEPS ecs
    OPTIONAL_OPTION DE_WITH_PHYSICS
    DETERMINISTIC
)

# DENG-133: broadphase (static + dynamic AABB trees, sorted pair list, traversal for queries).
if(TARGET de_physics)
    target_sources(de_physics PRIVATE
        broadphase/aabb_tree.h
        broadphase/aabb_tree.c
        broadphase/broadphase.h
        broadphase/broadphase.c)
endif()

# DENG-134: narrowphase (contact sets: specialized sphere/capsule paths, SAT, GJK/EPA, midphase).
if(TARGET de_physics)
    target_sources(de_physics PRIVATE
        narrow/narrowphase.h
        narrow/dispatch.c
        narrow/sphere_capsule.c
        narrow/sat_box.c
        narrow/sat_hull.c
        narrow/clip.c
        narrow/gjk.c
        narrow/epa.c
        narrow/midphase.c)
endif()

# DENG-135: persistent contact manifolds (feature matching, reduction, accumulation, touch state)
# and the collision pipeline wired into de_physics_step (step.c, checksum.c above).
if(TARGET de_physics)
    target_sources(de_physics PRIVATE
        contact/manifold.h
        contact/manifold.c
        contact/reduce.c
        contact/pair_cache.c)
endif()

# DENG-137: scene queries (raycast, raycast_all, shape cast, overlap, closest point).
if(TARGET de_physics)
    target_sources(de_physics PRIVATE
        query/query_internal.h
        query/raycast.c
        query/ray_shapes.c
        query/shape_cast.c
        query/overlap.c
        query/closest_point.c)
endif()

# DENG-252: 2D physics (SPEC §13.3). Its sources are listed in 2d/CMakeLists.txt (append-only for
# the S24 tasks); they compile into de_physics and so get the DETERMINISTIC flags.
if(TARGET de_physics)
    add_subdirectory(2d)
endif()

# DENG-217: sub-stepped soft-step solver (constraint interface, contacts, integration) and the
# hook stubs that DENG-218 (islands.c) and DENG-219 (ccd.c) replace; step.c runs the pipeline.
if(TARGET de_physics)
    target_sources(de_physics PRIVATE
        solver/constraint.h
        solver/solver_internal.h
        solver/solver.c
        solver/contact_solver.c
        solver/integrate.c
        islands.c
        ccd.c)
endif()

# DENG-218: islands and sleeping (islands.c replaces the DENG-217 stub), deterministic graph
# coloring and the colored parallel stage runner of the solver.
if(TARGET de_physics)
    target_sources(de_physics PRIVATE
        islands.h
        sleep.c
        solver/graph_color.h
        solver/graph_color.c
        solver/solver_parallel.c)
endif()

# DENG-220: joints (fixed, hinge, ball, slider, distance, cone-twist, 6-DOF; limits, motors,
# break force) as the solver's joint constraint class.
if(TARGET de_physics)
    target_sources(de_physics PRIVATE
        joints/joint_internal.h
        joints/joint.c
        joints/joint_fixed.c
        joints/joint_hinge.c
        joints/joint_ball.c
        joints/joint_slider.c
        joints/joint_distance.c
        joints/joint_cone_twist.c
        joints/joint_6dof.c)
endif()

# DENG-221: kinematic capsule character controller (collide and slide on the scene queries,
# slopes, steps, ground snap, moving platforms, pushing dynamic bodies).
if(TARGET de_physics)
    target_sources(de_physics PRIVATE
        character/character_internal.h
        character/character.c
        character/character_move.c)
endif()

# DENG-219: CCD. ccd.c replaces the DENG-217 stub (listed above): per-body predictions for the
# broadphase's predictive boxes and the per-pair speculative distance (contact/manifold.c), and
# the swept time of impact of bullets (DE_BODY_FLAG_CCD).
if(TARGET de_physics)
    target_sources(de_physics PRIVATE ccd_internal.h)
endif()
