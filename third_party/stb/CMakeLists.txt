# third_party/stb: stb single-file libraries (MIT or public domain), pinned in
# third_party/vendor.toml. One stb entry and one stb version for the whole engine: later sprints
# add headers (stb_image_write.h, stb_truetype.h) to the same entry and a TU here.
#
# de_tp_stb: static library with one implementation TU per stb file.
#   stb_image.h    `#include <stb_image.h>`         (impl: de_tp_stb_image.c)
#   stb_vorbis.c   `#include <de_stb_vorbis.h>`     (header-only view; impl: de_tp_stb_vorbis.c)
add_library(de_tp_stb STATIC de_tp_stb_image.c de_tp_stb_vorbis.c)
target_include_directories(de_tp_stb SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
# Vendored code: no engine warning flags, and its own warnings are off (CONVENTIONS §6).
target_compile_options(de_tp_stb PRIVATE $<IF:$<STREQUAL:${DE_COMPILER_FRONTEND},MSVC>,/w,-w>)
set_target_properties(de_tp_stb PROPERTIES
    POSITION_INDEPENDENT_CODE ${DE_BUILD_SHARED}
    FOLDER "dengine/third_party")
if(NOT DE_COMPILER_FRONTEND STREQUAL "MSVC")
    target_link_libraries(de_tp_stb PRIVATE m)
    # stb_image's integer IDCT and colour conversion overflow `int` on hostile JPEG coefficients
    # and rely on two's-complement wrapping (UBSan: "signed integer overflow" in stbi__idct_block,
    # found by fuzz_image, DENG-106). -fwrapv makes that wrapping defined; a damaged JPEG still
    # decodes to garbage pixels, never out of bounds. MSVC does not optimise on the overflow.
    set_source_files_properties(de_tp_stb_image.c PROPERTIES COMPILE_OPTIONS -fwrapv)
endif()

# Smoke test, built with the engine's strict flags: proves the headers compile cleanly in engine
# code. CTest entries: de_tp_smoke_stb.<test>.
de_add_test(NAME de_tp_smoke_stb
    SOURCES smoke/smoke_stb.c
    LIBS de_tp_stb
    LABELS unit third_party)
