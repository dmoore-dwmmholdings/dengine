# tools/pack: the .dpak pack writer library (lib/, DENG-211) and, from DENG-213, the de-pack CLI.
# Later tasks append their targets below.
add_subdirectory(lib)

# DENG-213: the `de-pack` CLI (list, extract, verify). The commands are a library (de_pack_cmds)
# so tests/unit/pack/ runs them in-process; main.c only forwards the arguments. It reads packs
# with the runtime reader (src/asset/dpak/dpak_reader.h, in de_asset) through the private src/
# root, and takes the engine type names from tools/cook/lib/de_cooker.h (a macro; no cooker code).
add_library(de_pack_cmds STATIC pack_cmds.c pack_cmds.h)
target_include_directories(de_pack_cmds
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    PRIVATE "${DE_ROOT_DIR}/src" "${DE_ROOT_DIR}/tools")
target_link_libraries(de_pack_cmds
    PUBLIC de_core
    PRIVATE de_asset de_platform de_tp_zstd de_tp_xxhash de_build_flags)
set_target_properties(de_pack_cmds PROPERTIES FOLDER "dengine/tools")

add_executable(de-pack main.c)
target_link_libraries(de-pack PRIVATE de_pack_cmds de_build_flags)
set_target_properties(de-pack PROPERTIES FOLDER "dengine/tools")
