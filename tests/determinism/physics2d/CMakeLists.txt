# DENG-256. The workflow automatically builds de_determinism_tests and diffs every
# emitted *.checksums file; adding scenarios requires no matrix edits.
if(NOT TARGET de_physics)
    return()
endif()
add_executable(test_det_physics2d scenarios_joints2d.c "${DE_ROOT_DIR}/tests/unit/de_test.c")
target_link_libraries(test_det_physics2d PRIVATE de_det_harness de_physics de_build_flags)
target_include_directories(test_det_physics2d PRIVATE "${DE_ROOT_DIR}/tests/unit")
target_compile_options(test_det_physics2d PRIVATE ${DE_DETERMINISTIC_FLAGS})
target_compile_definitions(test_det_physics2d PRIVATE
    "DE_DET_OUT_DIR=\"${DE_DET_OUT_DIR}\""
    "DE_P2D_GOLDEN_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/golden\"")
de_det_add_tests(TARGET test_det_physics2d TESTS
    determinism_physics2d_bridge2d_0=bridge2d.workers_0
    determinism_physics2d_bridge2d_1=bridge2d.workers_1
    determinism_physics2d_bridge2d_4=bridge2d.workers_4
    determinism_physics2d_car2d_0=car2d.workers_0
    determinism_physics2d_car2d_1=car2d.workers_1
    determinism_physics2d_car2d_4=car2d.workers_4)

# DENG-260: ECS bodies and character course; checksums combine physics and ECS state.
target_sources(test_det_physics2d PRIVATE scenarios_2d.c)
target_include_directories(test_det_physics2d PRIVATE "${DE_ROOT_DIR}/src" "${DE_ROOT_DIR}/tests/unit/physics2d")
de_det_add_tests(TARGET test_det_physics2d TESTS
    determinism_physics2d_pyramid2d_side_0=pyramid2d_side.workers_0
    determinism_physics2d_pyramid2d_side_1=pyramid2d_side.workers_1
    determinism_physics2d_pyramid2d_side_4=pyramid2d_side.workers_4
    determinism_physics2d_chain_terrain_top_0=chain_terrain_top.workers_0
    determinism_physics2d_chain_terrain_top_1=chain_terrain_top.workers_1
    determinism_physics2d_chain_terrain_top_4=chain_terrain_top.workers_4
    determinism_physics2d_platformer_character2d_0=platformer_character2d.workers_0
    determinism_physics2d_platformer_character2d_1=platformer_character2d.workers_1
    determinism_physics2d_platformer_character2d_4=platformer_character2d.workers_4)
