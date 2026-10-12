# tools/pack/lib: de_pack_lib, the .dpak writer (DENG-211). Tools only: the runtime reads packs
# with src/asset/dpak/ and never links this library. Callers include "dpak_writer.h".
#
# The format header src/asset/dpak/dpak_format.h is header-only and shared with the reader; it is
# reached through the private src/ include root (as "asset/dpak/dpak_format.h").
add_library(de_pack_lib STATIC
    dpak_writer.c
    dpak_writer.h
    "${DE_ROOT_DIR}/src/asset/dpak/dpak_format.h")
target_include_directories(de_pack_lib
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    PRIVATE "${DE_ROOT_DIR}/src")
target_link_libraries(de_pack_lib
    PUBLIC de_core de_platform
    PRIVATE de_tp_zstd_dict de_tp_zstd de_build_flags)
set_target_properties(de_pack_lib PROPERTIES FOLDER "dengine/tools")
