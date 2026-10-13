# third_party/bc7enc_rdo: bc7enc_rdo (MIT or public domain), pinned in third_party/vendor.toml. The
# BC1/BC3/BC4/BC5/BC7 encoders of the texture importer (tools/cook/importers/texture/, DENG-99):
# bc7enc (BC7), rgbcx (BC1 to BC5) and ert (the optional rate-distortion pass), with bc7decomp
# (the BC7 unpacker ert needs). Tools only: only the cooker library links it, and Release and
# Shipping never link the cooker. C++ behind the importer's C shim texture_bc_enc.cpp
# (DENG-2 Q1); include <bc7enc.h>, <rgbcx.h>, <ert.h>, <bc7decomp.h> from C++ only.
#
# Configuration:
#   RGBCX_USE_SMALLER_TABLES=1  rgbcx uses its 110 KB ordering table (rgbcx_table4_small.h); the
#                               450 KB full table is not vendored. PUBLIC, because rgbcx.h sizes
#                               its declarations from it.
# Contraction of a*b+c into FMA is off (DE_DETERMINISTIC_FLAGS) and nothing uses fast math, so the
# encoders make the same float decisions on hosts whose compilers contract by default (Clang on
# arm64): a cook must be byte-identical.
add_library(de_tp_bc7enc_rdo STATIC
    bc7enc.cpp
    rgbcx.cpp
    ert.cpp
    bc7decomp.cpp)
target_include_directories(de_tp_bc7enc_rdo SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(de_tp_bc7enc_rdo PUBLIC RGBCX_USE_SMALLER_TABLES=1)
# Vendored code: no engine warning flags, and its own warnings are off (CONVENTIONS §6).
target_compile_options(de_tp_bc7enc_rdo PRIVATE
    $<IF:$<STREQUAL:${DE_COMPILER_FRONTEND},MSVC>,/w,-w>
    ${DE_DETERMINISTIC_FLAGS})
set_target_properties(de_tp_bc7enc_rdo PROPERTIES
    POSITION_INDEPENDENT_CODE ${DE_BUILD_SHARED}
    LINKER_LANGUAGE CXX
    FOLDER "dengine/third_party")

# Smoke test: the C test calls a small C++ helper (smoke/de_smoke_bc7enc_rdo.cpp, built with the
# engine's strict flags) that includes the encoder headers, encodes one block per format and
# unpacks it again. CTest entries: de_tp_smoke_bc7enc_rdo.<test>.
de_add_test(NAME de_tp_smoke_bc7enc_rdo
    SOURCES smoke/smoke_bc7enc_rdo.c smoke/de_smoke_bc7enc_rdo.cpp
    LIBS de_tp_bc7enc_rdo
    LABELS unit third_party)
