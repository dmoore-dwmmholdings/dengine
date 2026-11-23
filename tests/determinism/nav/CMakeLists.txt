# DENG-77: navmesh build and deterministic-mode queries give the same bytes on every platform
# (SPEC sections 5.7 and 21.2). The golden was recorded on macOS arm64; the cross-arch check is
# the determinism CI diff of <build>/determinism/nav_queries.checksums.
if(NOT TARGET de_nav)
    return()
endif()
add_executable(test_nav_determinism test_nav_determinism.c)
target_link_libraries(test_nav_determinism PRIVATE de_det_harness de_nav de_nav_build de_ecs
    de_reflect de_platform de_core de_test_framework de_build_flags)
target_include_directories(test_nav_determinism PRIVATE "${DE_ROOT_DIR}/src")
target_compile_options(test_nav_determinism PRIVATE ${DE_DETERMINISTIC_FLAGS})
target_compile_definitions(test_nav_determinism PRIVATE
    "DE_NAV_DET_GOLDEN=\"${CMAKE_CURRENT_SOURCE_DIR}/nav_queries_checksums.txt\""
    "DE_DET_OUT_DIR=\"${DE_DET_OUT_DIR}\"")
if(WIN32)
    target_compile_definitions(test_nav_determinism PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
de_det_add_tests(TARGET test_nav_determinism TESTS
    determinism_nav.queries_match_golden=nav_determinism.queries_match_golden
    determinism_nav.repeat_runs_match=nav_determinism.repeat_runs_match)
