# Self tests of the test framework (tests/unit/de_test.h).
de_add_test(NAME selftest SOURCES test_selftest.c)

# The checks below run the test executables, which a cross build cannot do on the build host.
if(NOT TARGET test_selftest OR (CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR))
    return()
endif()

# Same source with DE_TEST_SELFTEST_FAIL: adds tests that really fail and skip.
add_executable(test_selftest_fail test_selftest.c)
target_compile_definitions(test_selftest_fail PRIVATE DE_TEST_SELFTEST_FAIL)
target_link_libraries(test_selftest_fail PRIVATE de_test_framework de_build_flags)
set_target_properties(test_selftest_fail PROPERTIES FOLDER "dengine/tests")

# A failing test prints [FAIL] file:line and exits non-zero; a skip exits 77.
add_test(NAME selftest.deliberate_failure
    COMMAND "${CMAKE_COMMAND}" "-DEXE=$<TARGET_FILE:test_selftest_fail>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_failure.cmake")

# --list, --filter, --repeat and --shuffle --seed (same order on every run).
add_test(NAME selftest.runner_cli
    COMMAND "${CMAKE_COMMAND}" "-DEXE=$<TARGET_FILE:test_selftest>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/runner_cli.cmake")

set_tests_properties(selftest.deliberate_failure selftest.runner_cli PROPERTIES
    LABELS "unit;selftest" TIMEOUT 60)
