# minimal_game.cmake: the examples/minimal_game.c smoke test (DENG-95). Registered by
# examples/CMakeLists.txt as smoke_minimal_game and smoke_minimal_game_static.
#
#   cmake -DCMD=<exe|arg|...> -DFRAMES=<n> [-DFORM=<regex>] [-DREJECT=<re1|re2>]
#         -P minimal_game.cmake
#
# CMD is the command, its arguments separated by '|' (a ';' would split the CTest argument); it
# must run the game headless for FRAMES frames. The run passes when:
#   - it exits 0 within 120 s;
#   - headless mode was on (the host's "headless: no window, GPU, audio or UI" line);
#   - the game was attached the expected way (FORM: "attached from library" or "attached
#     (statically linked)");
#   - `level_01 ready` was logged exactly once (the asset load hook and status polling), and
#     `level_01 failed` never;
#   - the host's exit line says FRAMES frames and at least one FixedUpdate step (the game's
#     deterministic spin system runs once per FixedUpdate step; tests/unit/host/test_headless.c
#     checks that count exactly);
#   - no REJECT regex matches, and nothing logged an error.
if(NOT DEFINED CMD OR NOT DEFINED FRAMES)
    message(FATAL_ERROR "minimal_game.cmake: CMD and FRAMES are required")
endif()
string(REPLACE "|" ";" cmd "${CMD}")
string(REPLACE "|" ";" reject "${REJECT}")

execute_process(
    COMMAND ${cmd}
    OUTPUT_VARIABLE out
    ERROR_VARIABLE out
    RESULT_VARIABLE rc
    TIMEOUT 120)

set(problems "")
if(NOT "${rc}" STREQUAL "0")
    string(APPEND problems "exit code ${rc}, expected 0\n")
endif()
if(NOT out MATCHES "headless: no window, GPU, audio or UI")
    string(APPEND problems "headless mode was not on\n")
endif()
if(DEFINED FORM AND NOT out MATCHES "game 'minimal' ${FORM}")
    string(APPEND problems "the game was not attached as expected (${FORM})\n")
endif()
string(REGEX MATCHALL "level_01 ready" ready "${out}")
list(LENGTH ready ready_count)
if(NOT ready_count EQUAL 1)
    string(APPEND problems "`level_01 ready` logged ${ready_count} times, expected once\n")
endif()
if(out MATCHES "level_01 failed")
    string(APPEND problems "the level load failed\n")
endif()
if(out MATCHES "exited normally \\(frames: ([0-9]+), FixedUpdate steps: ([0-9]+)\\)")
    set(frames "${CMAKE_MATCH_1}")
    set(steps "${CMAKE_MATCH_2}")
    if(NOT frames EQUAL FRAMES)
        string(APPEND problems "ran ${frames} frames, expected ${FRAMES}\n")
    endif()
    if(steps LESS 1)
        string(APPEND problems "no FixedUpdate step ran\n")
    endif()
else()
    string(APPEND problems "no exit line with the frame and FixedUpdate step counts\n")
endif()
foreach(re IN LISTS reject)
    if(out MATCHES "${re}")
        string(APPEND problems "output matches: ${re}\n")
    endif()
endforeach()
if(out MATCHES "\\[(error|fatal)\\]|de_host_run failed")
    string(APPEND problems "an error was logged\n")
endif()

string(REPLACE ";" " " shown "${cmd}")
if(problems)
    message(FATAL_ERROR "minimal_game smoke: ${shown}\n${problems}--- output ---\n${out}")
endif()
message(STATUS "minimal_game smoke: ${shown}: ${frames} frames, ${steps} FixedUpdate steps, "
               "level_01 ready once")
