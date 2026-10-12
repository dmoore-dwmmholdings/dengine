# tests/unit/host: unit tests of module `host` (de_host, SPEC §10). Shared file (S06,
# append-only): later S06 tasks add their own de_add_test() blocks below, one block per task.
#
# The tests include the internal headers "host/host_internal.h" and "host/engine_internal.h"
# (src/ is an include root). DE_HOST_TEST_DATA_DIR holds the game.toml fixtures (data/), which
# the tests mount as game:/; DE_HOST_TEST_TMP_DIR (build tree) holds user:/ and cache:/, so no
# test writes into the real user profile.

# DENG-90: engine create/destroy, de_host_run, game.toml. Every test runs headless, except
# engine_window (label `window`), which checks that a zeroed desc opens a window and skips
# without a video driver.
de_add_test(NAME engine SOURCES test_engine.c LIBS de_host TIMEOUT 300)
de_add_test(NAME game_toml SOURCES test_game_toml.c LIBS de_host)
de_add_test(NAME engine_window SOURCES test_engine_window.c LIBS de_host LABELS unit window)
foreach(_de_host_test test_engine test_game_toml test_engine_window)
    if(TARGET ${_de_host_test})
        target_include_directories(${_de_host_test} PRIVATE "${DE_ROOT_DIR}/src")
        target_compile_definitions(${_de_host_test} PRIVATE
            DE_HOST_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
            DE_HOST_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/${_de_host_test}_tmp")
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${_de_host_test}_tmp")
    endif()
endforeach()
