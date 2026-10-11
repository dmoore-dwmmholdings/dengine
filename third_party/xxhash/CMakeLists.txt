# third_party/xxhash: xxHash (BSD-2-Clause), pinned in third_party/vendor.toml.
#
# de_tp_xxhash: static library that compiles the xxHash implementation once (de_tp_xxhash.c),
# so `#include <xxhash.h>` links. Code on a hot path may instead `#define XXH_INLINE_ALL` before
# the include; that renames the symbols (XXH_INLINE_*) and needs no library at all.
add_library(de_tp_xxhash STATIC de_tp_xxhash.c)
target_include_directories(de_tp_xxhash SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
# Vendored code: no engine warning flags, and its own warnings are off (CONVENTIONS §6).
target_compile_options(de_tp_xxhash PRIVATE $<IF:$<STREQUAL:${DE_COMPILER_FRONTEND},MSVC>,/w,-w>)
set_target_properties(de_tp_xxhash PROPERTIES
    POSITION_INDEPENDENT_CODE ${DE_BUILD_SHARED}
    FOLDER "dengine/third_party")

# Smoke test, built with the engine's strict flags: proves the headers compile cleanly in engine
# code. CTest entries: de_tp_smoke_xxhash.<test>.
de_add_test(NAME de_tp_smoke_xxhash
    SOURCES smoke/smoke_xxhash.c
    LIBS de_tp_xxhash
    LABELS unit third_party)
