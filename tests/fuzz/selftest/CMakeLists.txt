# Self test of the fuzz harness (tests/fuzz/de_fuzz.h, de_add_fuzz_target).
de_add_fuzz_target(NAME selftest SOURCES fuzz_selftest.c CORPUS corpus)

if(NOT TARGET fuzz_selftest OR (CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR))
    return()
endif()

# The crash input must make the target fail, in both builds: proof that inputs reach it.
add_test(NAME fuzz.selftest_crash_detected
    COMMAND "${CMAKE_COMMAND}" "-DEXE=$<TARGET_FILE:fuzz_selftest>"
            "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/crash/crash_input.txt"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_crash.cmake"
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
set_tests_properties(fuzz.selftest_crash_detected PROPERTIES LABELS "fuzz;selftest" TIMEOUT 60)
