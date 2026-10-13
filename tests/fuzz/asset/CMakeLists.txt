# tests/fuzz/asset: fuzz targets of module `asset`. DENG-145: fuzz_manifest (assets.toml parse,
# resolve and validate). Each runs its seed corpus under CTest (label `fuzz`); see
# cmake/DeTesting.cmake for real fuzzing with libFuzzer.
de_add_fuzz_target(NAME manifest SOURCES fuzz_manifest.c LIBS de_asset de_platform
                   CORPUS corpus/manifest)
if(TARGET fuzz_manifest)
    # The target reads the internal manifest headers ("asset/manifest_internal.h", ...).
    target_include_directories(fuzz_manifest PRIVATE "${DE_ROOT_DIR}/src")
endif()

# DENG-216: fuzz_dpak_reader, the .dpak pack reader (dpak_open_memory, then every accessor and
# every chunk decode; see fuzz_dpak_reader.c). The seed corpus corpus/dpak_reader/ holds packs
# written by the DENG-211 writer; make_dpak_corpus.c makes them, and CTest dpak_corpus.seeds
# checks that each still opens and has the feature it is there for (tools builds only: the writer
# is a tool library). DE_DPAK_CORPUS_UPDATE=1 ctest -R dpak_corpus.seeds writes them again.
de_add_fuzz_target(NAME dpak_reader SOURCES fuzz_dpak_reader.c LIBS de_asset de_tp_zstd
                   CORPUS corpus/dpak_reader)
if(TARGET fuzz_dpak_reader)
    # The reader is internal to de_asset ("asset/dpak/dpak_reader.h").
    target_include_directories(fuzz_dpak_reader PRIVATE "${DE_ROOT_DIR}/src")
endif()
if(DE_BUILD_TOOLS)
    de_add_test(NAME dpak_corpus SOURCES make_dpak_corpus.c
                LIBS de_pack_lib de_asset de_platform)
    if(TARGET test_dpak_corpus)
        target_include_directories(test_dpak_corpus PRIVATE "${DE_ROOT_DIR}/src")
        target_compile_definitions(test_dpak_corpus PRIVATE
            "DE_DPAK_CORPUS_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/corpus/dpak_reader\"")
    endif()
endif()
