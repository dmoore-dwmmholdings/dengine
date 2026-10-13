# tests/determinism/input: the input recording test vector and its determinism scenario
# (DENG-89). input_replay.deir, input_replay_ticks.checksums and input_replay.checksums are the
# checked-in golden files (see test_det_input.c for how and when to regenerate them).
#
#   det.input_replay.live_rerecord   the scripted session recorded live equals input_replay.deir
#                                    byte for byte, and its per-tick input checksums equal
#                                    input_replay_ticks.checksums
#   det.input_replay.golden          the recording replayed into a world: per-tick world
#                                    checksums against input_replay.checksums
#   det.input_replay.workers_1_vs_n  the same scenario on 0, 1 and 4 workers
#
# input_script.c is also compiled into tests/unit/input (test_record_replay).
if(NOT TARGET de_input)
    return()
endif()
add_executable(test_det_input test_det_input.c input_script.c input_script.h)
target_link_libraries(test_det_input PRIVATE de_det_harness de_input de_test_framework
                      de_build_flags)
# src/ is an include root for the internal input headers (injection, the .deir writer).
target_include_directories(test_det_input PRIVATE "${DE_ROOT_DIR}/src")
target_compile_options(test_det_input PRIVATE ${DE_DETERMINISTIC_FLAGS})
target_compile_definitions(test_det_input PRIVATE
    "DE_DET_OUT_DIR=\"${DE_DET_OUT_DIR}\""
    "DE_DET_GOLDEN_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\"")
set_target_properties(test_det_input PROPERTIES FOLDER "dengine/tests")

de_det_add_tests(TARGET test_det_input TESTS
    det.input_replay.live_rerecord=input_replay.live_rerecord
    det.input_replay.golden=input_replay.golden
    det.input_replay.workers_1_vs_n=input_replay.workers_1_vs_n)
