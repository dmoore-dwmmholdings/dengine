# third_party/tomlc17: tomlc17 TOML parser (MIT), pinned in third_party/vendor.toml. Used by the
# cvar system and the asset manifest (SPEC §5.8, §11.2). Public include: `#include <tomlc17.h>`.
#
# Note for wrappers: allocation hooks are process-global (toml_set_option) and take no user
# pointer, so an engine wrapper must route them to its allocator itself.
add_library(de_tp_tomlc17 STATIC src/tomlc17.c)
target_include_directories(de_tp_tomlc17 SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
# Vendored code: no engine warning flags, and its own warnings are off (CONVENTIONS §6).
target_compile_options(de_tp_tomlc17 PRIVATE $<IF:$<STREQUAL:${DE_COMPILER_FRONTEND},MSVC>,/w,-w>)
set_target_properties(de_tp_tomlc17 PROPERTIES
    POSITION_INDEPENDENT_CODE ${DE_BUILD_SHARED}
    FOLDER "dengine/third_party")
if(NOT DE_COMPILER_FRONTEND STREQUAL "MSVC")
    target_link_libraries(de_tp_tomlc17 PRIVATE m)
endif()

# Smoke test, built with the engine's strict flags: proves the headers compile cleanly in engine
# code. CTest entries: de_tp_smoke_tomlc17.<test>.
de_add_test(NAME de_tp_smoke_tomlc17
    SOURCES smoke/smoke_tomlc17.c
    LIBS de_tp_tomlc17
    LABELS unit third_party)
