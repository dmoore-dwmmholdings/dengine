# DENG-581: the foliage model gives the same bytes on every platform (SPEC section 5.7).
if(NOT TARGET de_foliage OR NOT TARGET de_terrain)
    return()
endif()
add_executable(test_foliage_determinism test_foliage_determinism.c)
target_link_libraries(test_foliage_determinism PRIVATE de_det_harness de_foliage de_terrain
    de_test_framework de_build_flags)
target_include_directories(test_foliage_determinism PRIVATE "${DE_ROOT_DIR}/src")
target_compile_options(test_foliage_determinism PRIVATE ${DE_DETERMINISTIC_FLAGS})
target_compile_definitions(test_foliage_determinism PRIVATE
    "DE_FOLIAGE_DET_GOLDEN=\"${CMAKE_CURRENT_SOURCE_DIR}/foliage_checksums.txt\""
    "DE_DET_OUT_DIR=\"${DE_DET_OUT_DIR}\"")
if(WIN32)
    target_compile_definitions(test_foliage_determinism PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
de_det_add_tests(TARGET test_foliage_determinism TESTS
    determinism_foliage.ops_match_golden=foliage_determinism.ops_match_golden
    determinism_foliage.repeat_runs_match=foliage_determinism.repeat_runs_match)
