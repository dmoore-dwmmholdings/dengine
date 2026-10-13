# tests/unit/physics: unit tests of module `physics` (de_physics, SPEC §13). Shared file
# (append-only within S22): later S22 tasks add their own de_add_test() blocks below.
#
# One executable per suite (CTest names physics_world.<test>, physics_shapes.<test>, ...). The
# tests include the internal headers ("physics/physics_internal.h", src/ is an include root) and
# use the module's deterministic flags (no FP contraction), like the code they check.
if(NOT TARGET de_physics)
    return()
endif()

# DENG-132: world, bodies, shapes, convex hulls, mass properties.
de_add_test(NAME physics_world SOURCES test_world_bodies.c LIBS de_physics)
de_add_test(NAME physics_shapes SOURCES test_shapes.c LIBS de_physics)
de_add_test(NAME physics_hull SOURCES test_convex_hull.c LIBS de_physics TIMEOUT 120)
de_add_test(NAME physics_mass SOURCES test_mass.c LIBS de_physics)
foreach(_de_phys_test test_physics_world test_physics_shapes test_physics_hull test_physics_mass)
    if(TARGET ${_de_phys_test})
        target_include_directories(${_de_phys_test} PRIVATE "${DE_ROOT_DIR}/src")
        target_compile_options(${_de_phys_test} PRIVATE ${DE_DETERMINISTIC_FLAGS})
    endif()
endforeach()
# The checked-in vector of hull outputs and mass properties (physics_hull.golden_vector). x86-64
# and arm64 must reproduce it bit for bit. Regenerate (never by hand) with
#     DE_REGEN_GOLDEN=1 build/dev/tests/unit/physics/test_physics_hull --filter=physics_hull.golden_vector
if(TARGET test_physics_hull)
    target_compile_definitions(test_physics_hull PRIVATE
        "DE_PHYSICS_SHAPES_GOLDEN=\"${CMAKE_CURRENT_SOURCE_DIR}/vectors/shapes_golden.txt\"")
endif()

# DENG-132: SPEC §5.7 static check over src/physics/ (comments stripped): no <math.h>, no libm
# sqrt (de_dsqrt / de_dsqrt64 instead), no fast-math de_math.h functions (sections 2 and 3: they
# may use FMA or SIMD), no approximate reciprocal intrinsics, no hash-map iteration. DENG-136's
# tools/check_determinism_src.py generalises this for every deterministic module.
if(DE_BUILD_TESTS)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/physics_determinism_src.cmake" [=[
file(GLOB_RECURSE _de_srcs "${DE_PHYSICS_DIR}/*.c" "${DE_PHYSICS_DIR}/*.h")
if(NOT _de_srcs)
    message(FATAL_ERROR "no sources found under ${DE_PHYSICS_DIR}")
endif()
set(_de_rules
    "#[ \t]*include[ \t]*<math\\.h>"
    "(^|[^A-Za-z0-9_])sqrtf?[ \t]*\\("
    "(^|[^A-Za-z0-9_])de_(v2|v3|v4|quat|m3|m4|affine|aabb|plane|ray|frustum)_[a-z0-9_]+[ \t]*\\("
    "_mm_(rcp|rsqrt)_(ps|ss)|vrecpe|vrsqrte"
    "de_hashmap_iter_(begin|next)|de_hashmap_iter[ \t]")
set(_de_bad "")
foreach(_de_src IN LISTS _de_srcs)
    file(READ "${_de_src}" _de_text)
    string(REGEX REPLACE "/\\*([^*]|\\*+[^*/])*\\*+/" "" _de_text "${_de_text}")
    string(REGEX REPLACE "//[^\n]*" "" _de_text "${_de_text}")
    foreach(_de_rule IN LISTS _de_rules)
        string(REGEX MATCHALL "${_de_rule}[^\n]*" _de_hits "${_de_text}")
        foreach(_de_hit IN LISTS _de_hits)
            string(APPEND _de_bad "  ${_de_src}: ${_de_hit}\n")
        endforeach()
    endforeach()
endforeach()
if(_de_bad)
    message(FATAL_ERROR "src/physics breaks SPEC 5.7 (deterministic math only):\n${_de_bad}")
endif()
list(LENGTH _de_srcs _de_n)
message(STATUS "src/physics: ${_de_n} files clean")
]=])
    add_test(NAME physics.determinism_src
        COMMAND "${CMAKE_COMMAND}" "-DDE_PHYSICS_DIR=${DE_ROOT_DIR}/src/physics"
                -P "${CMAKE_CURRENT_BINARY_DIR}/physics_determinism_src.cmake")
    set_tests_properties(physics.determinism_src PROPERTIES LABELS "unit;physics" TIMEOUT 60)
endif()

# DENG-133: broadphase (AABB tree, pairs, filters, queries, golden pair-list vector). The
# vector is a cross-platform contract like shapes_golden.txt; regenerate (never by hand) with
#     DE_REGEN_GOLDEN=1 build/dev/tests/unit/physics/test_physics_broadphase --filter=physics_broadphase.test_pair_vector_golden
de_add_test(NAME physics_aabb_tree SOURCES test_aabb_tree.c LIBS de_physics)
de_add_test(NAME physics_broadphase SOURCES test_broadphase.c LIBS de_physics TIMEOUT 180)
foreach(_de_phys_test test_physics_aabb_tree test_physics_broadphase)
    if(TARGET ${_de_phys_test})
        target_include_directories(${_de_phys_test} PRIVATE "${DE_ROOT_DIR}/src")
        target_compile_options(${_de_phys_test} PRIVATE ${DE_DETERMINISTIC_FLAGS})
    endif()
endforeach()
if(TARGET test_physics_broadphase)
    target_compile_definitions(test_physics_broadphase PRIVATE
        "DE_PHYSICS_BROADPHASE_GOLDEN=\"${CMAKE_CURRENT_SOURCE_DIR}/vectors/broadphase_golden.txt\"")
endif()

# DENG-134: narrowphase (specialized paths, SAT, GJK/EPA, midphase) and its golden vector.
de_add_test(NAME physics_narrow SOURCES test_narrow_specialized.c LIBS de_physics de_test_support
            TIMEOUT 120)
foreach(_de_np_test test_physics_narrow)
    if(TARGET ${_de_np_test})
        target_include_directories(${_de_np_test} PRIVATE "${DE_ROOT_DIR}/src")
        target_compile_options(${_de_np_test} PRIVATE ${DE_DETERMINISTIC_FLAGS})
    endif()
endforeach()
# The checked-in vector of contact sets (physics_narrow.golden_vector). x86-64 and arm64 must
# reproduce it bit for bit. Regenerate (never by hand) with
#     DE_REGEN_GOLDEN=1 build/dev/tests/unit/physics/test_physics_narrow --filter=physics_narrow.golden_vector
if(TARGET test_physics_narrow)
    target_compile_definitions(test_physics_narrow PRIVATE
        "DE_PHYSICS_NARROW_GOLDEN=\"${CMAKE_CURRENT_SOURCE_DIR}/vectors/narrow_golden.txt\"")
endif()
de_add_test(NAME physics_sat SOURCES test_sat.c LIBS de_physics)
de_add_test(NAME physics_gjk SOURCES test_gjk_epa.c LIBS de_physics)
de_add_test(NAME physics_midphase SOURCES test_midphase.c LIBS de_physics)
foreach(_de_np_test test_physics_sat test_physics_gjk test_physics_midphase)
    if(TARGET ${_de_np_test})
        target_include_directories(${_de_np_test} PRIVATE "${DE_ROOT_DIR}/src")
        target_compile_options(${_de_np_test} PRIVATE ${DE_DETERMINISTIC_FLAGS})
    endif()
endforeach()

# DENG-135: persistent contact manifolds (reduction, carry-over, accumulation, touch state,
# triggers) and the collision pipeline of de_physics_step (worker-count invariance, overflow,
# checksum, no allocation after warm-up).
de_add_test(NAME physics_manifold SOURCES test_manifold.c LIBS de_physics)
de_add_test(NAME physics_pipeline SOURCES test_collide_pipeline.c LIBS de_physics TIMEOUT 180)
foreach(_de_ct_test test_physics_manifold test_physics_pipeline)
    if(TARGET ${_de_ct_test})
        target_include_directories(${_de_ct_test} PRIVATE "${DE_ROOT_DIR}/src")
        target_compile_options(${_de_ct_test} PRIVATE ${DE_DETERMINISTIC_FLAGS})
    endif()
endforeach()

# DENG-137: scene queries. physics_query_ray holds the checked-in vector of 10,000 rays and 1,000
# shape casts (physics_query_ray.golden_vector); x86-64 and arm64 must reproduce it bit for bit.
# Regenerate (never by hand) with
#     DE_REGEN_GOLDEN=1 build/dev/tests/unit/physics/test_physics_query_ray --filter=physics_query_ray.golden_vector
# physics_query_cast includes the 4-thread concurrency smoke test, so it carries the tsan label.
de_add_test(NAME physics_query_ray SOURCES test_query_raycast.c LIBS de_physics de_test_support
            TIMEOUT 120)
de_add_test(NAME physics_query_cast SOURCES test_query_cast_overlap.c
            LIBS de_physics de_platform de_test_support LABELS unit tsan TIMEOUT 120)
foreach(_de_q_test test_physics_query_ray test_physics_query_cast)
    if(TARGET ${_de_q_test})
        target_include_directories(${_de_q_test} PRIVATE "${DE_ROOT_DIR}/src")
        target_compile_options(${_de_q_test} PRIVATE ${DE_DETERMINISTIC_FLAGS})
    endif()
endforeach()
if(TARGET test_physics_query_ray)
    target_compile_definitions(test_physics_query_ray PRIVATE
        "DE_PHYSICS_QUERY_GOLDEN=\"${CMAKE_CURRENT_SOURCE_DIR}/vectors/query_golden.txt\"")
endif()

# DENG-217: the soft-step solver. physics_solver_basic: integration, damping, kinematic bodies,
# rotation locks, speed clamps, large worlds. physics_solver_contact: restitution, incline
# friction, anisotropic friction. physics_solver_stack: stack of 10, 1:100 mass ratio, warm
# start, the constraint interface, no allocation, worker-count invariance.
de_add_test(NAME physics_solver_basic SOURCES test_solver_basic.c LIBS de_physics)
de_add_test(NAME physics_solver_contact SOURCES test_solver_friction_restitution.c LIBS de_physics)
de_add_test(NAME physics_solver_stack SOURCES test_solver_stack.c LIBS de_physics)
foreach(_de_sv_test test_physics_solver_basic test_physics_solver_contact test_physics_solver_stack)
    if(TARGET ${_de_sv_test})
        target_include_directories(${_de_sv_test} PRIVATE "${DE_ROOT_DIR}/src")
        target_compile_options(${_de_sv_test} PRIVATE ${DE_DETERMINISTIC_FLAGS})
    endif()
endforeach()

# DENG-218: islands, sleeping, deterministic graph coloring and the colored parallel solver.
# physics_solver_parallel holds the worker-count invariance and allocation checks run under TSan
# (label tsan); its scenes stay small so the TSan Debug builds finish quickly.
de_add_test(NAME physics_islands SOURCES test_islands.c LIBS de_physics)
de_add_test(NAME physics_sleep SOURCES test_sleep.c LIBS de_physics)
de_add_test(NAME physics_graph_color SOURCES test_graph_color.c LIBS de_physics)
de_add_test(NAME physics_solver_parallel SOURCES test_solver_parallel.c
            LIBS de_physics de_platform LABELS unit tsan TIMEOUT 120)
foreach(_de_is_test test_physics_islands test_physics_sleep test_physics_graph_color
        test_physics_solver_parallel)
    if(TARGET ${_de_is_test})
        target_include_directories(${_de_is_test} PRIVATE "${DE_ROOT_DIR}/src")
        target_compile_options(${_de_is_test} PRIVATE ${DE_DETERMINISTIC_FLAGS})
    endif()
endforeach()

# DENG-220: joints. physics_joints: API, pool, each kind, pendulum period, chain of 20,
# collide_connected, 6-DOF locked = fixed, wake. physics_joint_limits: limits, motors, the cone,
# break force, worker-count invariance (label tsan; scenes stay small).
de_add_test(NAME physics_joints SOURCES test_joints.c LIBS de_physics)
de_add_test(NAME physics_joint_limits SOURCES test_joint_limits_motors.c
            LIBS de_physics de_platform LABELS unit tsan TIMEOUT 120)
foreach(_de_jt_test test_physics_joints test_physics_joint_limits)
    if(TARGET ${_de_jt_test})
        target_include_directories(${_de_jt_test} PRIVATE "${DE_ROOT_DIR}/src")
        target_compile_options(${_de_jt_test} PRIVATE ${DE_DETERMINISTIC_FLAGS})
    endif()
endforeach()

# DENG-221: the kinematic capsule character controller. physics_character: arguments, falling and
# jumping, collide and slide, depenetration, slopes, steps, stairs, thin walls, no allocation.
# physics_character_platforms: moving platforms, pushing dynamic bodies, determinism.
de_add_test(NAME physics_character SOURCES test_character.c LIBS de_physics)
de_add_test(NAME physics_character_platforms SOURCES test_character_platforms.c LIBS de_physics)
foreach(_de_ch_test test_physics_character test_physics_character_platforms)
    if(TARGET ${_de_ch_test})
        target_include_directories(${_de_ch_test} PRIVATE "${DE_ROOT_DIR}/src")
        target_compile_options(${_de_ch_test} PRIVATE ${DE_DETERMINISTIC_FLAGS})
    endif()
endforeach()

# DENG-219: CCD. physics_ccd: bullets against a thin wall (1,000 random angles), meshes,
# heightfields and compounds; documented tunnelling without the flag; fast drops on speculative
# contacts; no ghost stops; predictive broadphase boxes and the per-pair speculative distance; no
# allocation per step; worker-count invariance (label tsan: small scenes).
de_add_test(NAME physics_ccd SOURCES test_ccd.c LIBS de_physics de_platform LABELS unit tsan
            TIMEOUT 120)
if(TARGET test_physics_ccd)
    target_include_directories(test_physics_ccd PRIVATE "${DE_ROOT_DIR}/src")
    target_compile_options(test_physics_ccd PRIVATE ${DE_DETERMINISTIC_FLAGS})
endif()

# DENG-223: the ECS systems. physics_ecs: spawn / despawn sync, component changes, static
# colliders, kinematic targets, sleeping, characters, entity joints and break events, no
# allocation per tick. physics_contact_events: begin / end, triggers, order, impulses.
# physics_interpolation: alpha blend at 30 Hz / 144 Hz, variable-mode pass-through, the
# simulation unchanged by interpolation, children, opt-out.
de_add_test(NAME physics_ecs SOURCES test_physics_ecs.c LIBS de_physics)
de_add_test(NAME physics_contact_events SOURCES test_contact_events.c LIBS de_physics)
de_add_test(NAME physics_interpolation SOURCES test_interpolation.c LIBS de_physics)
foreach(_de_pe_test test_physics_ecs test_physics_contact_events test_physics_interpolation)
    if(TARGET ${_de_pe_test})
        target_include_directories(${_de_pe_test} PRIVATE "${DE_ROOT_DIR}/src")
        target_compile_options(${_de_pe_test} PRIVATE ${DE_DETERMINISTIC_FLAGS})
    endif()
endforeach()
