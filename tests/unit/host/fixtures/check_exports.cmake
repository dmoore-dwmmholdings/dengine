# check_exports.cmake: a game library exports de_game_get_api and nothing else (DENG-92).
#
#   cmake -DNM=<nm> -DLIB=<library> -P check_exports.cmake
#
# macOS: `nm -gU` (defined external symbols, with the leading underscore). Linux: `nm -D
# --defined-only` (the dynamic symbol table). Windows is not checked here: a DLL exports only
# what is marked __declspec(dllexport), which test_game.c does for de_game_get_api alone.
if(NOT NM OR NOT LIB)
    message(FATAL_ERROR "check_exports.cmake: NM and LIB are required")
endif()
if(APPLE OR CMAKE_HOST_APPLE)
    set(args -gU)
    set(want "_de_game_get_api")
else()
    set(args -D --defined-only)
    set(want "de_game_get_api")
endif()
execute_process(COMMAND "${NM}" ${args} "${LIB}"
    OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "check_exports: ${NM} ${args} ${LIB} failed (${rc}): ${err}")
endif()

set(found FALSE)
set(extra "")
string(REPLACE "\n" ";" lines "${out}")
foreach(line IN LISTS lines)
    # "<address> <type> <name>"; the version-script node of ELF shows as an absolute symbol
    # with no address meaning (type A), which is not an export of the game's code.
    if(NOT line MATCHES "^[0-9a-fA-F ]* ([A-Za-z]) (.+)$")
        continue()
    endif()
    set(type "${CMAKE_MATCH_1}")
    set(name "${CMAKE_MATCH_2}")
    if(name STREQUAL want)
        set(found TRUE)
    elseif(NOT type STREQUAL "A")
        list(APPEND extra "${name}")
    endif()
endforeach()
if(NOT found)
    message(FATAL_ERROR "check_exports: ${LIB} does not export ${want}\n${out}")
endif()
if(extra)
    message(FATAL_ERROR "check_exports: ${LIB} exports more than ${want}: ${extra}\n${out}")
endif()
message(STATUS "check_exports: ${LIB} exports ${want} only")
