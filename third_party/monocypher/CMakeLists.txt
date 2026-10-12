# third_party/monocypher: Monocypher (BSD-2-Clause OR CC0-1.0), pinned in third_party/vendor.toml.
# Network crypto (SPEC §16.1, §22): src/net/crypto.c is its only engine user (DENG-52).
# Public include: `#include <monocypher.h>`.
#
# tests/tis-ci-vectors.h is upstream's small test-vector subset. It is not part of the library;
# the net unit tests add `tests/` to their own include path (tests/unit/net/CMakeLists.txt).
add_library(de_tp_monocypher STATIC src/monocypher.c)
target_include_directories(de_tp_monocypher SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
# Vendored code: no engine warning flags, and its own warnings are off (CONVENTIONS §6).
target_compile_options(de_tp_monocypher
    PRIVATE $<IF:$<STREQUAL:${DE_COMPILER_FRONTEND},MSVC>,/w,-w>)
set_target_properties(de_tp_monocypher PROPERTIES
    POSITION_INDEPENDENT_CODE ${DE_BUILD_SHARED}
    FOLDER "dengine/third_party")

# Smoke test, built with the engine's strict flags: proves the header compiles cleanly in engine
# code. CTest entries: de_tp_smoke_monocypher.<test>.
de_add_test(NAME de_tp_smoke_monocypher
    SOURCES smoke/smoke_monocypher.c
    LIBS de_tp_monocypher
    LABELS unit third_party)
