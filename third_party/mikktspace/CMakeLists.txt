# third_party/mikktspace: MikkTSpace (zlib), pinned in third_party/vendor.toml. Tangent-space
# generation for the model importer (tools/cook/importers/model/, DENG-101). Tools only: it is
# linked into the cooker library, which Release and Shipping never link.
# Public include: `#include <mikktspace.h>`.
add_library(de_tp_mikktspace STATIC mikktspace.c)
target_include_directories(de_tp_mikktspace SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
# Vendored code: no engine warning flags, and its own warnings are off (CONVENTIONS §6).
target_compile_options(de_tp_mikktspace
    PRIVATE $<IF:$<STREQUAL:${DE_COMPILER_FRONTEND},MSVC>,/w,-w>)
set_target_properties(de_tp_mikktspace PROPERTIES
    POSITION_INDEPENDENT_CODE ${DE_BUILD_SHARED}
    FOLDER "dengine/third_party")
if(NOT DE_COMPILER_FRONTEND STREQUAL "MSVC")
    target_link_libraries(de_tp_mikktspace PRIVATE m)
endif()

# Smoke test, built with the engine's strict flags: proves the header compiles cleanly in engine
# code. CTest entries: de_tp_smoke_mikktspace.<test>.
de_add_test(NAME de_tp_smoke_mikktspace
    SOURCES smoke/smoke_mikktspace.c
    LIBS de_tp_mikktspace
    LABELS unit third_party)
