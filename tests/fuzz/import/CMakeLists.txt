# tests/fuzz/import: fuzz targets of the content importers (DENG-106, SPEC §19.1, §21.3): fuzz_gltf,
# fuzz_fbx, fuzz_image and fuzz_ktx2 run the real `model` and `texture` importers in memory
# (fuzz_import_harness.c) and check every blob they write with its runtime parser
# (fuzz_cooked_walk.c). Each replays its seed corpus corpus/<target>/ under CTest (label `fuzz`),
# with the libFuzzer dictionary dict/<target>.dict; make_corpus.py writes the corpora from the
# unit-test fixtures. Importers are cooker code: tools builds only (as tests/unit/import).
if(NOT DE_BUILD_TOOLS)
    return()
endif()

set(_de_fz_import_sources fuzz_import_harness.c fuzz_import_harness.h fuzz_cooked_walk.c
                          fuzz_cooked_walk.h)
foreach(_de_fz IN ITEMS gltf fbx image ktx2)
    de_add_fuzz_target(NAME ${_de_fz} SOURCES fuzz_${_de_fz}.c ${_de_fz_import_sources}
                       LIBS de_cooker de_asset de_anim de_platform
                       CORPUS corpus/${_de_fz} DICT dict/${_de_fz}.dict)
    if(TARGET fuzz_${_de_fz})
        # The harness uses the cooker's import context ("cook/lib/cooker_internal.h") and the
        # cooked format headers ("render/formats/...", "anim/formats/...").
        target_include_directories(fuzz_${_de_fz} PRIVATE "${DE_ROOT_DIR}/src" "${DE_ROOT_DIR}/tools")
    endif()
endforeach()

# import_fuzz_regressions: replays regressions/<target>/ (every input a fuzzer crashed on, after
# the fix) through the same harness without libFuzzer, and checks that oversize inputs fail with a
# de_result instead of allocating (CTest names import_fuzz_regressions.<suite>.<test>).
de_add_test(NAME import_fuzz_regressions
            SOURCES test_import_fuzz_regressions.c ${_de_fz_import_sources}
            LIBS de_cooker de_asset de_anim de_platform)
if(TARGET test_import_fuzz_regressions)
    target_include_directories(test_import_fuzz_regressions PRIVATE
        "${DE_ROOT_DIR}/src" "${DE_ROOT_DIR}/tools")
    target_compile_definitions(test_import_fuzz_regressions PRIVATE
        "DE_FUZZ_IMPORT_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\"")
endif()

# cooked_corpus: writes (DE_COOKED_CORPUS_UPDATE=1) and checks the seed corpora of
# fuzz_cooked_render (tests/fuzz/render/) and fuzz_cooked_anim (tests/fuzz/anim/): blobs the
# importers cook from this directory's corpora, through the same harness (make_cooked_corpus.c).
de_add_test(NAME cooked_corpus
            SOURCES make_cooked_corpus.c ${_de_fz_import_sources}
            LIBS de_cooker de_asset de_anim de_platform)
if(TARGET test_cooked_corpus)
    target_include_directories(test_cooked_corpus PRIVATE
        "${DE_ROOT_DIR}/src" "${DE_ROOT_DIR}/tools")
    target_compile_definitions(test_cooked_corpus PRIVATE
        "DE_FUZZ_IMPORT_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\"")
endif()
