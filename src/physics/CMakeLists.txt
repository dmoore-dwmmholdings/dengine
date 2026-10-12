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
