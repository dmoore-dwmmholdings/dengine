# tests/fuzz/anim: fuzz targets of module `anim`. DENG-106: fuzz_cooked_anim, the cooked skeleton
# and clip parsers of src/anim/formats/ (DSKL, DCLP and the reference sampler). The walkers are
# shared with the importer targets (tests/fuzz/import/fuzz_cooked_walk.c). The seed corpus
# corpus/cooked_anim/ is written by tests/fuzz/import/make_cooked_corpus.c (CTest
# cooked_corpus.*, tools builds).
de_add_fuzz_target(NAME cooked_anim
                   SOURCES fuzz_cooked_anim.c "${DE_ROOT_DIR}/tests/fuzz/import/fuzz_cooked_walk.c"
                   LIBS de_anim de_platform
                   CORPUS corpus/cooked_anim)
if(TARGET fuzz_cooked_anim)
    target_include_directories(fuzz_cooked_anim PRIVATE
        "${DE_ROOT_DIR}/src" "${DE_ROOT_DIR}/tests/fuzz/import")
endif()
