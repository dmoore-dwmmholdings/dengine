# third_party/tinyexr: tinyexr (BSD-3-Clause), pinned in third_party/vendor.toml. The OpenEXR
# decoder of the texture importer (tools/cook/importers/texture/, DENG-98). Tools only: only the
# cooker library links it, and Release and Shipping never link the cooker.
#
# de_tp_tinyexr: de_tp_tinyexr.cc is the one implementation TU (TINYEXR_IMPLEMENTATION, C++). The
# declaration part of <tinyexr.h> is C (`extern "C"` under C++), so the importer calls it from C
# (DENG-2 Q1: C++ behind a C interface). Configuration of the implementation:
#   TINYEXR_USE_MINIZ=0, TINYEXR_USE_STB_ZLIB=1  inflate (ZIP, ZIPS) through stb_image's zlib
#                              decoder (de_tp_stb), so miniz is not vendored. Writing EXR files is
#                              not supported: the compressor is a stub that fails (see the TU).
#   TINYEXR_USE_THREAD=0, TINYEXR_USE_OPENMP=0  one thread per decode (the cooker runs imports in
#                              parallel itself, and output must not depend on thread counts).
add_library(de_tp_tinyexr STATIC de_tp_tinyexr.cc)
target_include_directories(de_tp_tinyexr SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(de_tp_tinyexr PRIVATE
    TINYEXR_USE_MINIZ=0
    TINYEXR_USE_STB_ZLIB=1
    TINYEXR_USE_THREAD=0
    TINYEXR_USE_OPENMP=0)
target_link_libraries(de_tp_tinyexr PRIVATE de_tp_stb)
# Vendored code: no engine warning flags, and its own warnings are off (CONVENTIONS §6).
target_compile_options(de_tp_tinyexr PRIVATE $<IF:$<STREQUAL:${DE_COMPILER_FRONTEND},MSVC>,/w,-w>)
set_target_properties(de_tp_tinyexr PROPERTIES
    POSITION_INDEPENDENT_CODE ${DE_BUILD_SHARED}
    FOLDER "dengine/third_party")

# Smoke test, built with the engine's strict flags: proves <tinyexr.h> compiles as C11 and decodes
# a small EXR built in memory. CTest entries: de_tp_smoke_tinyexr.<test>.
de_add_test(NAME de_tp_smoke_tinyexr
    SOURCES smoke/smoke_tinyexr.c
    LIBS de_tp_tinyexr
    LABELS unit third_party)
