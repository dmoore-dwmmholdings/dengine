# tests/unit/task: unit tests of module `task` (de_task, SPEC §8). Append-only within S04:
# DENG-292 and DENG-293 add their executables below.
#
# One executable per area, so the CTest names are deque.<test> and sched.<test>. Both include
# internal headers under src/ ("task/deque.h", "task/task_internal.h", "platform/de_atomic.h"),
# and carry the label `tsan`: threading tests the TSan job must keep covering.
de_add_test(NAME deque SOURCES test_deque.c LIBS de_task LABELS unit tsan TIMEOUT 120)
de_add_test(NAME sched SOURCES test_scheduler.c LIBS de_task LABELS unit tsan TIMEOUT 180)
foreach(_de_task_test test_deque test_sched)
    if(TARGET ${_de_task_test})
        target_include_directories(${_de_task_test} PRIVATE "${DE_ROOT_DIR}/src")
    endif()
endforeach()

# sched.background_isolation asserts a wall-clock bound (1000 frame tasks in < 50 ms), so it runs
# alone and also gets the label `timing` (Trama "Decision: Timing tests run serially and loosen
# their bounds on CI"; same include-file approach as tests/unit/platform/CMakeLists.txt). The
# other sched.* tests only check counts and stay parallel.
if(TARGET test_sched AND NOT (CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR))
    # In CTest script mode set_property(TEST) is not available, so the labels are read back from
    # the generated set_tests_properties line and set again with `timing` appended.
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/test_sched_serial.cmake" [=[
if(EXISTS "${_de_tests}")
    file(STRINGS "${_de_tests}" _de_sched_props
         REGEX "^set_tests_properties\\(\\[==\\[sched\\.background_isolation\\]==\\]")
    if(_de_sched_props MATCHES "LABELS \\[==\\[([^]]*)\\]==\\]")
        # file(STRINGS) escapes the ';' inside a line; unescape to get the label list back.
        string(REPLACE "\\;" ";" _de_sched_labels "${CMAKE_MATCH_1}")
        list(APPEND _de_sched_labels timing)
        set_tests_properties("sched.background_isolation" PROPERTIES
                             RUN_SERIAL TRUE LABELS "${_de_sched_labels}")
    endif()
endif()
]=])
    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
                 "${CMAKE_CURRENT_BINARY_DIR}/test_sched_serial.cmake")
endif()
