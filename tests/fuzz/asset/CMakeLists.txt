# tests/fuzz/asset: fuzz targets of module `asset`. DENG-145: fuzz_manifest (assets.toml parse,
# resolve and validate). Each runs its seed corpus under CTest (label `fuzz`); see
# cmake/DeTesting.cmake for real fuzzing with libFuzzer.
de_add_fuzz_target(NAME manifest SOURCES fuzz_manifest.c LIBS de_asset de_platform
                   CORPUS corpus/manifest)
if(TARGET fuzz_manifest)
    # The target reads the internal manifest headers ("asset/manifest_internal.h", ...).
    target_include_directories(fuzz_manifest PRIVATE "${DE_ROOT_DIR}/src")
endif()
