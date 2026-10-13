# tests/fuzz/render: fuzz targets of module `render`. DENG-106: fuzz_cooked_render, the cooked
# format parsers of src/render/formats/ (DTEX, DTEX + ENVS, DMSH v2, DMAT, DMDL). The walkers are
# shared with the importer targets (tests/fuzz/import/fuzz_cooked_walk.c). The seed corpus
# corpus/cooked_render/ is written by tests/fuzz/import/make_cooked_corpus.c (CTest
# cooked_corpus.*, tools builds).
de_add_fuzz_target(NAME cooked_render
                   SOURCES fuzz_cooked_render.c "${DE_ROOT_DIR}/tests/fuzz/import/fuzz_cooked_walk.c"
                   LIBS de_anim de_platform
                   CORPUS corpus/cooked_render)
if(TARGET fuzz_cooked_render)
    # The formats are internal headers ("render/formats/...", "anim/formats/...").
    target_include_directories(fuzz_cooked_render PRIVATE
        "${DE_ROOT_DIR}/src" "${DE_ROOT_DIR}/tests/fuzz/import")
endif()
