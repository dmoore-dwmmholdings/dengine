# third_party/ufbx: ufbx (MIT or the Unlicense), pinned in third_party/vendor.toml. The FBX loader
# of the model importer (tools/cook/importers/model/fbx_import.c, DENG-102). Tools only: it is
# linked into the cooker library, which Release and Shipping never link.
# Public include: `#include <ufbx.h>`.
#
# ufbx.c is upstream's one implementation TU, compiled as it is (default configuration: double
# precision `ufbx_real`, libc math, no threads unless a caller passes thread options).
add_library(de_tp_ufbx STATIC ufbx.c)
target_include_directories(de_tp_ufbx SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
# Vendored code: no engine warning flags, and its own warnings are off (CONVENTIONS §6).
target_compile_options(de_tp_ufbx PRIVATE $<IF:$<STREQUAL:${DE_COMPILER_FRONTEND},MSVC>,/w,-w>)
set_target_properties(de_tp_ufbx PROPERTIES
    POSITION_INDEPENDENT_CODE ${DE_BUILD_SHARED}
    FOLDER "dengine/third_party")
if(NOT DE_COMPILER_FRONTEND STREQUAL "MSVC")
    target_link_libraries(de_tp_ufbx PRIVATE m)
endif()

# Smoke test, built with the engine's strict flags: proves the header compiles cleanly in engine
# code. CTest entries: de_tp_smoke_ufbx.<test>.
de_add_test(NAME de_tp_smoke_ufbx
    SOURCES smoke/smoke_ufbx.c
    LIBS de_tp_ufbx
    LABELS unit third_party)
