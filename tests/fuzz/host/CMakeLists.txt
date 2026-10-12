# tests/fuzz/host: fuzz targets of module `host` (DENG-90). Each runs its seed corpus under CTest
# (label `fuzz`); see cmake/DeTesting.cmake for real fuzzing with libFuzzer.
de_add_fuzz_target(NAME game_toml SOURCES fuzz_game_toml.c LIBS de_host CORPUS corpus/game_toml)
if(TARGET fuzz_game_toml)
    # The target includes the internal header "host/engine_internal.h".
    target_include_directories(fuzz_game_toml PRIVATE "${DE_ROOT_DIR}/src")
endif()
