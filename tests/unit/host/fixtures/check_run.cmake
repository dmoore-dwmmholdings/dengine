# check_run.cmake: runs a command and checks its exit code and output (DENG-92 host tests).
#
#   cmake -DCMD=<a|b|c> -DEXPECT_EXIT=<n> [-DEXPECT=<re1|re2>] [-DREJECT=<re1|re2>]
#         [-DWORKDIR=<dir>] -P check_run.cmake
#
# CMD, EXPECT and REJECT are lists separated by '|' (a ';' would split the CTest argument).
# Every EXPECT regex must match the combined stdout and stderr, no REJECT regex may match, and
# the exit code must be EXPECT_EXIT. CTest's PASS_REGULAR_EXPRESSION cannot do this: it ignores
# the exit code.
if(NOT DEFINED CMD OR NOT DEFINED EXPECT_EXIT)
    message(FATAL_ERROR "check_run.cmake: CMD and EXPECT_EXIT are required")
endif()
string(REPLACE "|" ";" cmd "${CMD}")
string(REPLACE "|" ";" expect "${EXPECT}")
string(REPLACE "|" ";" reject "${REJECT}")
if(NOT WORKDIR)
    set(WORKDIR "${CMAKE_CURRENT_BINARY_DIR}")
endif()

execute_process(
    COMMAND ${cmd}
    WORKING_DIRECTORY "${WORKDIR}"
    OUTPUT_VARIABLE out
    ERROR_VARIABLE out
    RESULT_VARIABLE rc
    TIMEOUT 120)

set(problems "")
if(NOT "${rc}" STREQUAL "${EXPECT_EXIT}")
    string(APPEND problems "exit code ${rc}, expected ${EXPECT_EXIT}\n")
endif()
foreach(re IN LISTS expect)
    if(NOT out MATCHES "${re}")
        string(APPEND problems "output does not match: ${re}\n")
    endif()
endforeach()
foreach(re IN LISTS reject)
    if(out MATCHES "${re}")
        string(APPEND problems "output matches: ${re}\n")
    endif()
endforeach()

string(REPLACE ";" " " shown "${cmd}")
if(problems)
    message(FATAL_ERROR "check_run: ${shown}\n${problems}--- output ---\n${out}")
endif()
message(STATUS "check_run: ${shown}: exit ${rc}, output as expected")
