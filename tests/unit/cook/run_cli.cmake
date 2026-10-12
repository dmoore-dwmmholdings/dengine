# run_cli.cmake: runs the de-cook executable once and checks its exit code and output (DENG-145).
# Called by the cook_cli.* tests of tests/unit/cook/CMakeLists.txt:
#   cmake -DEXE=<de-cook> -DARGS=<a|b|c> -DEXPECT_RC=<n> -DEXPECT_REGEX=<regex> -P run_cli.cmake
# stdout and stderr are checked together. CMake's `.` also matches a newline.
string(REPLACE "|" ";" args "${ARGS}")
execute_process(
    COMMAND "${EXE}" ${args}
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
set(all "${out}${err}")
if(NOT rc STREQUAL EXPECT_RC)
    message(FATAL_ERROR "de-cook ${args}: exit code ${rc}, expected ${EXPECT_RC}\n${all}")
endif()
if(NOT all MATCHES "${EXPECT_REGEX}")
    message(FATAL_ERROR "de-cook ${args}: output does not match '${EXPECT_REGEX}':\n${all}")
endif()
message(STATUS "de-cook ${args}: exit ${rc}, output as expected")
