# third_party/dide_protocol: DIDE's editor IPC protocol, engine side (generated C, transport,
# mpack 1.1.1, MIT), pinned in third_party/vendor.toml. The host's editor service (Dev only,
# src/host/editor) is its only user. Include as `#include "de_editor_protocol.h"`.
if(DE_IS_SHIPPING)
    return()
endif()
add_library(de_tp_dide_protocol STATIC
    de_editor_protocol.c
    de_editor_transport.c
    third_party/mpack/mpack.c)
target_include_directories(de_tp_dide_protocol SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
# Vendored code: no engine warning flags, and its own warnings are off (CONVENTIONS §6).
target_compile_options(de_tp_dide_protocol
    PRIVATE $<IF:$<STREQUAL:${DE_COMPILER_FRONTEND},MSVC>,/w,-w>)
if(WIN32)
    target_compile_definitions(de_tp_dide_protocol PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(de_tp_dide_protocol PUBLIC advapi32)
endif()
set_target_properties(de_tp_dide_protocol PROPERTIES
    POSITION_INDEPENDENT_CODE ${DE_BUILD_SHARED}
    FOLDER "dengine/third_party")
