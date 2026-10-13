# src/physics: module `physics` (target de_physics), layer gameplay. SPEC §13. Deterministic
# (SPEC §5.7): every source gets -ffp-contract=off (or /fp:precise).
# 2D physics (src/physics/2d/*.c) is part of this deterministic module, not its own target.
#
# Shared file (append-only within S22): later tasks add their sources in their own block below,
# guarded with if(TARGET de_physics) because the module is skipped when DE_WITH_PHYSICS is off:
#     if(TARGET de_physics)
#         target_sources(de_physics PRIVATE broadphase/aabb_tree.c ...)
#     endif()
file(GLOB de_physics_2d_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/2d/*.c")
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
        ${de_physics_2d_sources}
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
