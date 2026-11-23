# DENG-580: the terrain model gives the same bytes on every platform (SPEC section 5.7).
if(NOT TARGET de_terrain)
    return()
endif()
add_executable(test_terrain_determinism test_terrain_determinism.c)
target_link_libraries(test_terrain_determinism PRIVATE de_det_harness de_terrain de_test_framework
    de_build_flags)
target_include_directories(test_terrain_determinism PRIVATE "${DE_ROOT_DIR}/src")
target_compile_options(test_terrain_determinism PRIVATE ${DE_DETERMINISTIC_FLAGS})
target_compile_definitions(test_terrain_determinism PRIVATE
    "DE_TERRAIN_DET_GOLDEN=\"${CMAKE_CURRENT_SOURCE_DIR}/terrain_checksums.txt\""
    "DE_TERRAIN_CARVE_DET_GOLDEN=\"${CMAKE_CURRENT_SOURCE_DIR}/terrain_carve_checksums.txt\""
    "DE_DET_OUT_DIR=\"${DE_DET_OUT_DIR}\"")
if(WIN32)
    target_compile_definitions(test_terrain_determinism PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
de_det_add_tests(TARGET test_terrain_determinism TESTS
    determinism_terrain.strokes_match_golden=terrain_determinism.strokes_match_golden
    determinism_terrain.repeat_runs_match=terrain_determinism.repeat_runs_match
    determinism_terrain.carve_matches_golden=terrain_determinism.carve_matches_golden
    determinism_terrain.carve_repeat_runs_match=terrain_determinism.carve_repeat_runs_match)
