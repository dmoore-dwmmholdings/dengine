# Runs a fuzz target on one input that must crash it.
# Usage: cmake -DEXE=<fuzz-exe> -DINPUT=<file> -P expect_crash.cmake
execute_process(COMMAND "${EXE}" "${INPUT}"
    OUTPUT_VARIABLE out ERROR_VARIABLE out RESULT_VARIABLE rc)
if(rc STREQUAL "0")
    message(FATAL_ERROR "'${EXE}' did not fail on the crash input\n${out}")
endif()
message(STATUS "crash detected as expected (${rc})")
