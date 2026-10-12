# tests/determinism/ecs: ECS determinism scenario `ecs_spawn_move` (DENG-353) and the harness
# self tests. ecs_spawn_move.checksums is the checked-in golden file (see test_det_ecs.c for how
# and when to regenerate it).
#
#   det.ecs_spawn_move.workers_1_vs_n   no task system vs 1 and 8 workers, 3 runs each
#   det.ecs_spawn_move.golden           this build vs the golden file; writes
#                                       <build>/determinism/ecs_spawn_move.checksums
#   det.ecs_spawn_move.rollback         snapshot at tick 400, replay 200 ticks after a restore
#   det.harness_detects_divergence      injected hash-map iteration order is reported at its tick
#   det.harness_compare_and_io          compare, report and checksum-file reading / writing
add_executable(test_det_ecs test_det_ecs.c scenario_spawn_move.c scenario_spawn_move.h)
target_link_libraries(test_det_ecs PRIVATE de_det_harness de_test_framework de_build_flags)
# src/ is an include root for the internal core/de_hashmap.h (test-only injection).
target_include_directories(test_det_ecs PRIVATE "${DE_ROOT_DIR}/src")
target_compile_options(test_det_ecs PRIVATE ${DE_DETERMINISTIC_FLAGS})
target_compile_definitions(test_det_ecs PRIVATE
    "DE_DET_OUT_DIR=\"${DE_DET_OUT_DIR}\""
    "DE_DET_GOLDEN_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\"")
set_target_properties(test_det_ecs PROPERTIES FOLDER "dengine/tests")

de_det_add_tests(TARGET test_det_ecs TESTS
    det.ecs_spawn_move.workers_1_vs_n=ecs_spawn_move.workers_1_vs_n
    det.ecs_spawn_move.golden=ecs_spawn_move.golden
    det.ecs_spawn_move.rollback=ecs_spawn_move.rollback
    det.harness_detects_divergence=harness.detects_divergence
    det.harness_compare_and_io=harness.compare_and_io)
