# third_party/basisu: the Basis Universal transcoder (Apache-2.0), pinned in
# third_party/vendor.toml. The texture importer (tools/cook/importers/texture/, DENG-98) transcodes
# KTX2 files with ETC1S (BasisLZ) or UASTC payloads at cook time. Tools only: only the cooker
# library links it, and Release and Shipping never link the cooker. C++ behind the importer's C
# interface (tools/cook/importers/texture/texture_basis.cpp, DENG-2 Q1).
#
# de_tp_basisu: transcoder/basisu_transcoder.cpp, the transcoder's one TU. Include
# <basisu_transcoder.h> from C++ only. The configuration is PUBLIC, because basisu_transcoder.h
# declares its classes from the same macros:
#   BASISD_SUPPORT_KTX2=1, BASISD_SUPPORT_KTX2_ZSTD=1  KTX2 files, zstd supercompression through
#                                  the vendored zstd (zstd/zstd.h forwards to third_party/zstd)
#   BASISD_SUPPORT_PVRTC1/PVRTC2/ATC/FXT1/ETC2_EAC_A8/ETC2_EAC_RG11=0  mobile target formats the
#                                  engine never transcodes to (SPEC §11.6: BC on every desktop
#                                  GPU). Kept: RGBA32 (DENG-98), BC1/BC3/BC4/BC5/BC7 (unused:
#                                  DENG-99 filters the RGBA32 output and encodes BC itself), ASTC
#                                  (shared with the XUASTC and UASTC HDR decoders, and with the BC6H
#                                  encoder the texture importer uses, DENG-99).
add_library(de_tp_basisu STATIC transcoder/basisu_transcoder.cpp)
target_include_directories(de_tp_basisu SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/transcoder")
target_compile_definitions(de_tp_basisu PUBLIC
    BASISD_SUPPORT_KTX2=1
    BASISD_SUPPORT_KTX2_ZSTD=1
    BASISD_SUPPORT_PVRTC1=0
    BASISD_SUPPORT_PVRTC2=0
    BASISD_SUPPORT_ATC=0
    BASISD_SUPPORT_FXT1=0
    BASISD_SUPPORT_ETC2_EAC_A8=0
    BASISD_SUPPORT_ETC2_EAC_RG11=0)
target_link_libraries(de_tp_basisu PRIVATE de_tp_zstd)
# Vendored code: no engine warning flags, and its own warnings are off (CONVENTIONS §6). Upstream
# requires -fno-strict-aliasing with GCC and Clang (its root CMakeLists.txt; MSVC and clang-cl do
# not optimize on strict aliasing). /bigobj: the TU is 1.6 MB of tables and templates.
target_compile_options(de_tp_basisu PRIVATE
    "$<IF:$<STREQUAL:${DE_COMPILER_FRONTEND},MSVC>,/w;/bigobj,-w;-fno-strict-aliasing>")
# DENG-99: the texture importer's BC6H encoder is the transcoder's (astc_6x6_hdr::fast_encode_bc6h),
# so the transcoder is built without FMA contraction too, like the other encoders and the importer:
# its float decisions do not depend on the compiler's contraction default.
target_compile_options(de_tp_basisu PRIVATE ${DE_DETERMINISTIC_FLAGS})
set_target_properties(de_tp_basisu PROPERTIES
    POSITION_INDEPENDENT_CODE ${DE_BUILD_SHARED}
    FOLDER "dengine/third_party")

# Smoke test: the C test calls a small C++ helper (smoke/de_smoke_basisu.cpp, built with the
# engine's strict flags) that includes <basisu_transcoder.h> with the configuration above, starts
# the transcoder and refuses a damaged KTX2 file. CTest entries: de_tp_smoke_basisu.<test>.
de_add_test(NAME de_tp_smoke_basisu
    SOURCES smoke/smoke_basisu.c smoke/de_smoke_basisu.cpp
    LIBS de_tp_basisu
    LABELS unit third_party)
