# tests/fuzz/core: fuzz targets of module `core` (DENG-155). Each runs its seed corpus under
# CTest (label `fuzz`); see cmake/DeTesting.cmake for real fuzzing with libFuzzer.
de_add_fuzz_target(NAME cvar_config SOURCES fuzz_cvar_config.c LIBS de_core de_platform
                   CORPUS corpus/cvar_config)
de_add_fuzz_target(NAME str_parse SOURCES fuzz_str_parse.c LIBS de_core de_platform
                   CORPUS corpus/str_parse)
