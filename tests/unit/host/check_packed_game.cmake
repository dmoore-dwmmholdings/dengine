# check_packed_game.cmake (DENG-393): keeps the checked-in packs of tests/data/packed_game/ equal
# to what the real de-cook makes of tests/data/cook_project/engine.toml today.
#
# The packed-engine test (test_packed_engine.c) runs in every build type, Shipping included, but a
# Release or Shipping de-cook refuses `cook` (no runtime importers, DENG-213). So the test reads
# packs cooked once and checked in, one per platform (a pack is refused on another platform). This
# script cooks the same packs again with a Debug or Dev de-cook and compares them byte for byte
# (`de-cook cook` is deterministic). CTest runs it as packed_game.packs_current in Debug and Dev.
#
# Regenerate the packs after a cooker or format change (from the repo root, after a Dev build):
#   cmake -DDE_COOK=build/dev/tools/cook/de-cook -DUPDATE=ON -P tests/unit/host/check_packed_game.cmake
#
# Variables: DE_COOK (required): the de-cook executable. OUT: scratch directory (default
# packed_game_cook/ under the current directory). UPDATE: copy the new packs over the checked-in
# ones instead of failing.
cmake_minimum_required(VERSION 3.25)

if(NOT DE_COOK)
    message(FATAL_ERROR "check_packed_game: set -DDE_COOK=<path to de-cook>")
endif()
get_filename_component(_root "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(_manifest "${_root}/tests/data/cook_project/engine.toml")
set(_packs "${_root}/tests/data/packed_game")
if(NOT OUT)
    set(OUT "${CMAKE_CURRENT_BINARY_DIR}/packed_game_cook")
endif()

set(_stale "")
foreach(_platform linux macos windows)
    set(_out "${OUT}/${_platform}")
    file(REMOVE_RECURSE "${_out}")
    # The options are part of the fixture: names stripped (as a shipped game), no cook cache.
    execute_process(
        COMMAND "${DE_COOK}" cook "${_manifest}" --platform ${_platform} --out "${_out}"
                --no-cache --strip-names --jobs 2 --quiet
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _stdout
        ERROR_VARIABLE _stderr)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "check_packed_game: de-cook cook --platform ${_platform} exited "
                            "${_rc}\n${_stdout}${_stderr}")
    endif()
    set(_new "${_out}/game.dpak")
    set(_old "${_packs}/${_platform}/game.dpak")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${_new}" "${_old}"
                    RESULT_VARIABLE _same)
    if(_same EQUAL 0)
        message(STATUS "check_packed_game: ${_platform}/game.dpak is current")
    elseif(UPDATE)
        file(MAKE_DIRECTORY "${_packs}/${_platform}")
        file(COPY_FILE "${_new}" "${_old}")
        message(STATUS "check_packed_game: ${_platform}/game.dpak updated")
    else()
        list(APPEND _stale "${_platform}")
    endif()
endforeach()

if(_stale)
    message(FATAL_ERROR
        "check_packed_game: tests/data/packed_game/{${_stale}}/game.dpak differ from a fresh cook "
        "of tests/data/cook_project/engine.toml. If the cooker or the .dpak format changed on "
        "purpose, regenerate them (see the top of tests/unit/host/check_packed_game.cmake) and "
        "commit the new packs.")
endif()
