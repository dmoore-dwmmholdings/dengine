if(NOT TARGET de_anim)
    return()
endif()
add_executable(test_anim_determinism test_anim_determinism.c)
target_link_libraries(test_anim_determinism PRIVATE de_det_harness de_anim de_platform
    de_test_framework de_build_flags)
target_include_directories(test_anim_determinism PRIVATE "${DE_ROOT_DIR}/src"
    "${DE_ROOT_DIR}/tests/unit/anim")
target_compile_options(test_anim_determinism PRIVATE ${DE_DETERMINISTIC_FLAGS})
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/assets")
target_compile_definitions(test_anim_determinism PRIVATE DE_ANIM_HAS_COOKER=0
    "DE_ANIM_DET_ASSETS=\"${CMAKE_CURRENT_BINARY_DIR}/assets\""
    "DE_ANIM_DET_GOLDEN=\"${CMAKE_CURRENT_SOURCE_DIR}/anim_checksums.txt\""
    "DE_DET_OUT_DIR=\"${DE_DET_OUT_DIR}\"")
if(WIN32)
    target_compile_definitions(test_anim_determinism PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
de_det_add_tests(TARGET test_anim_determinism TESTS
    determinism_anim.workers_1=anim_determinism.workers_1
    determinism_anim.workers_4=anim_determinism.workers_4
    determinism_anim.workers_8=anim_determinism.workers_8
    determinism_anim.rollback_and_cadence=anim_determinism.rollback_and_cadence
    determinism_anim.detects_pose_bit_change=anim_determinism.detects_pose_bit_change)
