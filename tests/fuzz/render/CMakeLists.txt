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

# DENG-340: shipping .deprobe parser (version, settings hash, lengths and payload checksum).
if(TARGET de_render)
    de_add_fuzz_target(NAME probe_blob SOURCES fuzz_probe_blob.c LIBS de_render de_platform CORPUS corpus/probe_blob)
    if(TARGET fuzz_probe_blob)
        target_include_directories(fuzz_probe_blob PRIVATE "${DE_ROOT_DIR}/src")
    endif()
endif()
