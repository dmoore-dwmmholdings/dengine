# third_party/cgltf: cgltf (MIT), pinned in third_party/vendor.toml. The glTF 2.0 parser of the
# model importer (tools/cook/importers/model/, DENG-101). Tools only: it is linked into the cooker
# library, which Release and Shipping never link. Public include: `#include <cgltf.h>`.
#
# de_tp_cgltf.c is the one implementation TU (CGLTF_IMPLEMENTATION). cgltf_write.h is not vendored.
add_library(de_tp_cgltf STATIC de_tp_cgltf.c)
target_include_directories(de_tp_cgltf SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
# Vendored code: no engine warning flags, and its own warnings are off (CONVENTIONS §6).
target_compile_options(de_tp_cgltf PRIVATE $<IF:$<STREQUAL:${DE_COMPILER_FRONTEND},MSVC>,/w,-w>)
set_target_properties(de_tp_cgltf PROPERTIES
    POSITION_INDEPENDENT_CODE ${DE_BUILD_SHARED}
    FOLDER "dengine/third_party")

# Smoke test, built with the engine's strict flags: proves the header compiles cleanly in engine
# code. CTest entries: de_tp_smoke_cgltf.<test>.
de_add_test(NAME de_tp_smoke_cgltf
    SOURCES smoke/smoke_cgltf.c
    LIBS de_tp_cgltf
    LABELS unit third_party)
