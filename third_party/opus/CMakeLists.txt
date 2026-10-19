# third_party/opus: libopus (BSD-3-Clause), pinned in third_party/vendor.toml. Built here from the
# vendored sources, not with upstream's build system.
#
# de_tp_opus: static library with the CELT, SILK (fixed point) and Opus layers. Configuration:
#   - FIXED_POINT and DISABLE_FLOAT_API: the codec is integer-only, so the encoder gives the same
#     bytes on every host and compiler (the cooked Opus streams of the sound importer, DENG-314).
#     The float API (opus_encode_float, opus_decode_float, ...) and the float-only tonality
#     analysis are not built; callers convert to and from int16.
#   - No intrinsics and no run-time CPU detection (no OPUS_X86_* / OPUS_ARM_* macros), no DNN
#     features (deep PLC, DRED, OSCE), no custom modes.
#   - USE_ALLOCA for the codec's stack buffers (upstream's choice for compilers without VLAs).
#   - ENABLE_HARDENING, upstream's default: internal consistency checks stay on in release builds.
# Users: the cooker (encoder, tools/cook/importers/sound/) and the runtime sources (decoder,
# DENG-313). Public include: `#include <opus.h>`, `<opus_multistream.h>`.
file(GLOB _de_opus_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/celt/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/silk/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/silk/fixed/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
list(SORT _de_opus_sources)
add_library(de_tp_opus STATIC ${_de_opus_sources})
target_include_directories(de_tp_opus
    SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/celt"
        "${CMAKE_CURRENT_SOURCE_DIR}/silk"
        "${CMAKE_CURRENT_SOURCE_DIR}/silk/fixed")
target_compile_definitions(de_tp_opus PRIVATE
    OPUS_BUILD
    FIXED_POINT=1
    DISABLE_FLOAT_API
    USE_ALLOCA
    ENABLE_HARDENING)
if(UNIX)
    # Strict C11 libc headers do not implicitly declare alloca (DENG-501).
    target_compile_definitions(de_tp_opus PRIVATE HAVE_ALLOCA_H=1)
endif()
# Vendored code: no engine warning flags, and its own warnings are off (CONVENTIONS §6). No FP
# contraction, like the engine's deterministic code (little float is left in a fixed-point build).
target_compile_options(de_tp_opus PRIVATE
    $<IF:$<STREQUAL:${DE_COMPILER_FRONTEND},MSVC>,/w,-w> ${DE_DETERMINISTIC_FLAGS})
set_target_properties(de_tp_opus PROPERTIES
    POSITION_INDEPENDENT_CODE ${DE_BUILD_SHARED}
    FOLDER "dengine/third_party")
if(NOT DE_COMPILER_FRONTEND STREQUAL "MSVC")
    target_link_libraries(de_tp_opus PRIVATE m)
endif()

# Smoke test, built with the engine's strict flags: proves the headers compile cleanly in engine
# code. CTest entries: de_tp_smoke_opus.<test>.
de_add_test(NAME de_tp_smoke_opus
    SOURCES smoke/smoke_opus.c
    LIBS de_tp_opus
    LABELS unit third_party)
