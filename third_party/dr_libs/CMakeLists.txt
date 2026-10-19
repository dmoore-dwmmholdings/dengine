# third_party/dr_libs: dr_wav and dr_flac (Unlicense or MIT-0), pinned in third_party/vendor.toml.
#
# de_tp_dr_libs: static library with one implementation TU (de_tp_dr_libs.c) for both headers.
# Tools only: the sound importer (tools/cook/importers/sound/, DENG-314) links it into de_cooker;
# Release and Shipping never contain it. Public include: `#include <dr_wav.h>`, `<dr_flac.h>`.
#
# The configuration macros are PUBLIC, so every TU sees the same declarations: no stdio and no
# wchar (sources come from memory), and no Ogg FLAC (`.ogg` files are Vorbis; the importer
# rejects anything else).
add_library(de_tp_dr_libs STATIC de_tp_dr_libs.c)
target_include_directories(de_tp_dr_libs SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(de_tp_dr_libs PUBLIC
    DR_WAV_NO_STDIO
    DR_WAV_NO_WCHAR
    DR_FLAC_NO_STDIO
    DR_FLAC_NO_WCHAR
    DR_FLAC_NO_OGG)
# Vendored code: no engine warning flags, and its own warnings are off (CONVENTIONS §6).
target_compile_options(de_tp_dr_libs PRIVATE $<IF:$<STREQUAL:${DE_COMPILER_FRONTEND},MSVC>,/w,-w>)
set_target_properties(de_tp_dr_libs PROPERTIES
    POSITION_INDEPENDENT_CODE ${DE_BUILD_SHARED}
    FOLDER "dengine/third_party")

# Smoke test, built with the engine's strict flags: proves the headers compile cleanly in engine
# code. CTest entries: de_tp_smoke_dr_libs.<test>.
de_add_test(NAME de_tp_smoke_dr_libs
    SOURCES smoke/smoke_dr_libs.c
    LIBS de_tp_dr_libs
    LABELS unit third_party)
