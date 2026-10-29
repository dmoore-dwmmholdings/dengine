# tests/fuzz/host: fuzz targets of module `host` (DENG-90). Each runs its seed corpus under CTest
# (label `fuzz`); see cmake/DeTesting.cmake for real fuzzing with libFuzzer.
de_add_fuzz_target(NAME game_toml SOURCES fuzz_game_toml.c LIBS de_host CORPUS corpus/game_toml)
if(TARGET fuzz_game_toml)
    # The target includes the internal header "host/engine_internal.h".
    target_include_directories(fuzz_game_toml PRIVATE "${DE_ROOT_DIR}/src")
endif()

if(DE_HAS_RENDER AND TARGET de_tp_dide_protocol AND NOT DE_IS_SHIPPING)
    de_add_fuzz_target(NAME editor_settings SOURCES fuzz_editor_settings.c
        "${DE_ROOT_DIR}/src/host/editor/editor_settings_value.c"
        LIBS de_core de_tp_dide_protocol CORPUS corpus/editor_settings)
    target_include_directories(fuzz_editor_settings PRIVATE "${DE_ROOT_DIR}/src")
endif()
