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

# DENG-403: the scene and prefab fuzz targets (fuzz_scene_harness.h). fuzz_scene_dscn: hostile
# DSCN bytes into de__scene_view_parse, then de_prefab_instantiate (every build). fuzz_scene_toml:
# hostile TOML through the `scene` and `prefab` importers in a cooker import context (tools
# builds: the cooker library). Seed corpora corpus/scene_dscn/ and corpus/scene_toml/; the DSCN
# seeds are the cooks of the TOML seeds, checked (and with DE_SCENE_CORPUS_UPDATE=1 written) by
# CTest scene_corpus.seeds.
set(_de_fz_scene_libs de_asset de_ecs de_reflect de_platform)
de_add_fuzz_target(NAME scene_dscn SOURCES fuzz_scene_dscn.c fuzz_scene_harness.c
                   fuzz_scene_harness.h LIBS ${_de_fz_scene_libs} CORPUS corpus/scene_dscn)
if(TARGET fuzz_scene_dscn)
    # Internal headers of de_asset ("asset/scene/scene_format.h", "asset/slots_internal.h").
    target_include_directories(fuzz_scene_dscn PRIVATE "${DE_ROOT_DIR}/src")
endif()
if(DE_BUILD_TOOLS)
    set(_de_fz_scene_cook_sources fuzz_scene_harness.c fuzz_scene_harness.h fuzz_scene_cook.c)
    de_add_fuzz_target(NAME scene_toml SOURCES fuzz_scene_toml.c ${_de_fz_scene_cook_sources}
                       LIBS de_cooker ${_de_fz_scene_libs} CORPUS corpus/scene_toml
                       DICT dict/scene_toml.dict)
    de_add_test(NAME scene_corpus SOURCES fuzz_scene_corpus.c ${_de_fz_scene_cook_sources}
                LIBS de_cooker ${_de_fz_scene_libs})
    foreach(_de_fz_t IN ITEMS fuzz_scene_toml test_scene_corpus)
        if(TARGET ${_de_fz_t})
            # The cooker's import context ("cook/lib/cooker_internal.h") and de_asset internals.
            target_include_directories(${_de_fz_t} PRIVATE "${DE_ROOT_DIR}/src" "${DE_ROOT_DIR}/tools")
        endif()
    endforeach()
    if(TARGET test_scene_corpus)
        target_compile_definitions(test_scene_corpus PRIVATE
            "DE_SCENE_CORPUS_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/corpus\"")
    endif()
endif()
