# third_party/zstd: Zstandard (BSD-3-Clause), pinned in third_party/vendor.toml.
#
# de_tp_zstd: static library with lib/common, lib/compress and lib/decompress. Not built: legacy
# formats (ZSTD_LEGACY_SUPPORT=0), multithreaded compression (no ZSTD_MULTITHREAD), the dictionary
# builder, and the x86-64 assembly (ZSTD_DISABLE_ASM, so every target runs the same C path).
# Public include: `#include <zstd.h>`.
file(GLOB _de_zstd_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/common/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/compress/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/decompress/*.c")
list(SORT _de_zstd_sources)
add_library(de_tp_zstd STATIC ${_de_zstd_sources})
target_include_directories(de_tp_zstd
    SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/lib"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/lib/common")
target_compile_definitions(de_tp_zstd PRIVATE
    ZSTD_LEGACY_SUPPORT=0
    ZSTD_DISABLE_ASM=1
    # zstd carries its own xxHash copy; the prefix keeps it apart from de_tp_xxhash.
    XXH_NAMESPACE=ZSTD_)
# Vendored code: no engine warning flags, and its own warnings are off (CONVENTIONS §6).
target_compile_options(de_tp_zstd PRIVATE $<IF:$<STREQUAL:${DE_COMPILER_FRONTEND},MSVC>,/w,-w>)
set_target_properties(de_tp_zstd PROPERTIES
    POSITION_INDEPENDENT_CODE ${DE_BUILD_SHARED}
    FOLDER "dengine/third_party")

# Smoke test, built with the engine's strict flags: proves the headers compile cleanly in engine
# code. CTest entries: de_tp_smoke_zstd.<test>.
de_add_test(NAME de_tp_smoke_zstd
    SOURCES smoke/smoke_zstd.c
    LIBS de_tp_zstd
    LABELS unit third_party)
