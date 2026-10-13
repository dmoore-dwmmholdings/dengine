# tests/unit/physics2d: unit tests of 2D physics (src/physics/2d/, target de_physics, SPEC §13.3).
# Shared file (append-only within S24): later S24 tasks add their own de_add_test() blocks below.
#
# One executable per suite (CTest names physics2d_world.<test>, physics2d_shapes.<test>, ...).
# The tests include the internal headers ("physics/2d/p2d_internal.h", src/ is an include root)
# and use the module's deterministic flags (no FP contraction), like the code they check.
if(NOT TARGET de_physics)
    return()
endif()

# DENG-252: world, bodies, shapes, polygon hulls, chains, mass properties.
de_add_test(NAME physics2d_world SOURCES test_world2d.c LIBS de_physics)
de_add_test(NAME physics2d_shapes SOURCES test_shapes2d.c LIBS de_physics)
foreach(_de_p2d_test test_physics2d_world test_physics2d_shapes)
    if(TARGET ${_de_p2d_test})
        target_include_directories(${_de_p2d_test} PRIVATE "${DE_ROOT_DIR}/src")
        target_compile_options(${_de_p2d_test} PRIVATE ${DE_DETERMINISTIC_FLAGS})
    endif()
endforeach()
# The checked-in vector of polygon hulls and mass properties (physics2d_shapes.golden_vector).
# x86-64 and arm64 must reproduce it bit for bit. Regenerate (never by hand) with
#     DE_REGEN_GOLDEN=1 build/dev/tests/unit/physics2d/test_physics2d_shapes --filter=physics2d_shapes.golden_vector
if(TARGET test_physics2d_shapes)
    target_compile_definitions(test_physics2d_shapes PRIVATE
        "DE_PHYSICS2D_SHAPES_GOLDEN=\"${CMAKE_CURRENT_SOURCE_DIR}/vectors/shapes2d_golden.txt\"")
endif()
