# src/physics/2d: 2D physics (SPEC §13.3), part of the deterministic module `physics` (target
# de_physics), not a module of its own: the sources go into de_physics, so every TU gets
# DE_DETERMINISTIC_FLAGS (-ffp-contract=off or /fp:precise) and check_determinism_src covers it
# (tools/determinism_modules.toml lists src/physics).
#
# Shared file (append-only within S24): later tasks add their own block below. Paths are relative
# to this directory (subdirectories such as joints2d/ or ecs2d/ work too):
#     target_sources(de_physics PRIVATE joints2d/revolute2d.c ...)
# src/physics/CMakeLists.txt adds this directory only when the target exists (DE_WITH_PHYSICS).

# DENG-252: 2D world, bodies, shapes (circle, capsule, box, polygon, segment, chain), mass
# properties, minimal step and checksum.
target_sources(de_physics PRIVATE
    p2d_internal.h
    world2d.c
    body2d.c
    shapes2d.c
    polygon2d.c
    chain2d.c
    mass2d.c
    checksum2d.c
    step2d.c)
