# third_party/bcdec: bcdec (MIT or public domain), pinned in third_party/vendor.toml. A reference
# BC1 to BC7 decoder, test-only (DENG-99): the texture importer's BC tests decode the cooked
# blocks with it. Only test executables link de_tp_bcdec; de_cooker, de-cook and every runtime
# target never do. Public include: `#include <bcdec.h>` (declarations; the implementation is
# compiled once in de_tp_bcdec.c).
add_library(de_tp_bcdec STATIC de_tp_bcdec.c)
target_include_directories(de_tp_bcdec SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
# Vendored code: no engine warning flags, and its own warnings are off (CONVENTIONS §6).
target_compile_options(de_tp_bcdec PRIVATE $<IF:$<STREQUAL:${DE_COMPILER_FRONTEND},MSVC>,/w,-w>)
set_target_properties(de_tp_bcdec PROPERTIES
    POSITION_INDEPENDENT_CODE ${DE_BUILD_SHARED}
    FOLDER "dengine/third_party")

# Smoke test, built with the engine's strict flags: proves the header compiles cleanly in engine
# code and decodes known blocks. CTest entries: de_tp_smoke_bcdec.<test>.
de_add_test(NAME de_tp_smoke_bcdec
    SOURCES smoke/smoke_bcdec.c
    LIBS de_tp_bcdec
    LABELS unit third_party)
