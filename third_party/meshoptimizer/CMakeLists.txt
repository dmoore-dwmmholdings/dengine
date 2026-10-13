# third_party/meshoptimizer: meshoptimizer (MIT), pinned in third_party/vendor.toml. Mesh
# optimization, meshlets and LOD simplification for the model importer
# (tools/cook/importers/model/, DENG-103). Tools only: only the cooker library links it, and
# Release and Shipping never link the cooker. Public include: `#include <meshoptimizer.h>`.
#
# The library is C++ with a C API (DENG-2 Q1). The engine calls only that C API (the header's
# `extern "C"` block); C code that links de_tp_meshoptimizer is linked with the C++ linker, which
# CMake picks from the static library's link languages. Every src/*.cpp is compiled (upstream
# builds them all the same way); the importer uses the optimizers, the simplifier and the
# clusterizer.
#
# Contraction of a*b+c into FMA is off (DE_DETERMINISTIC_FLAGS), so the simplifier and the
# meshlet builder make the same float decisions on hosts whose compilers contract by default
# (Clang on arm64). A cook must be byte-identical; this also keeps cooks closer across hosts.
file(GLOB _de_meshopt_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
list(SORT _de_meshopt_sources)
add_library(de_tp_meshoptimizer STATIC ${_de_meshopt_sources})
target_include_directories(de_tp_meshoptimizer SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
# Vendored code: no engine warning flags, and its own warnings are off (CONVENTIONS §6).
target_compile_options(de_tp_meshoptimizer PRIVATE
    $<IF:$<STREQUAL:${DE_COMPILER_FRONTEND},MSVC>,/w,-w>
    ${DE_DETERMINISTIC_FLAGS})
set_target_properties(de_tp_meshoptimizer PROPERTIES
    POSITION_INDEPENDENT_CODE ${DE_BUILD_SHARED}
    LINKER_LANGUAGE CXX
    FOLDER "dengine/third_party")

# Smoke test, built with the engine's strict flags as C11: proves <meshoptimizer.h> compiles
# cleanly in engine C code, and that a C executable links the C++ library. CTest entries:
# de_tp_smoke_meshoptimizer.<test>.
de_add_test(NAME de_tp_smoke_meshoptimizer
    SOURCES smoke/smoke_meshoptimizer.c
    LIBS de_tp_meshoptimizer
    LABELS unit third_party)
