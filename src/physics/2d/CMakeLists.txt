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

# DENG-253: collision: 2D AABB trees and broadphase, narrowphase (circles, capsules, polygons,
# segments, chain segments with ghost vertices), chain segment bounds, persistent manifolds and
# the collision pipeline of the step.
target_sources(de_physics PRIVATE
    aabb_tree2d.h
    aabb_tree2d.c
    broadphase2d.h
    broadphase2d.c
    narrow2d.h
    narrow2d.c
    collide_chain2d.c
    manifold2d.h
    manifold2d.c)

# DENG-254: the soft-step solver (contacts, integration, f64 positions), the constraint interface
# of the 2D joints (constraint2d.h), islands, sleeping, deterministic graph coloring with colored
# parallel batches, and bullets (time of impact against static and kinematic bodies).
target_sources(de_physics PRIVATE
    constraint2d.h
    solver2d_internal.h
    solver2d.c
    contact_solver2d.c
    islands2d.c
    sleep2d.c
    graph_color2d.c
    bullet2d.c)

# DENG-258: 2D scene queries (rays, shape casts by GJK conservative advancement, overlaps,
# closest points) and the 2D kinematic character controller.
target_sources(de_physics PRIVATE
    query2d_internal.h
    query2d.c
    raycast2d.c
    shapecast2d.c
    character2d_internal.h
    character2d.c)

# DENG-256: scalar-row joints on the colored constraint interface.
target_sources(de_physics PRIVATE joints2d/joint2d.c joints2d/joint2d_internal.h)
