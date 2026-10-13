# third_party/slang: the Slang shader compiler (Apache-2.0 WITH LLVM-exception), a prebuilt entry
# of third_party/vendor.toml (DENG-390, DENG-289). The headers and LICENSE are committed; the
# compiler library of the target host is extracted into <build>/prebuilt/slang/ at configure time
# by de_prebuilt_import (cmake/DePrebuilt.cmake). Nothing here is ever committed as a binary.
#
# Slang is a tools-only library (SPEC §22): only the shader importer uses it
# (tools/cook/importers/shader/, de-cook and the in-process cooker of Debug and Dev builds), never
# Shipping. So it is imported only with DE_BUILD_TOOLS.
#
# de_tp_slang (INTERFACE): the headers (SYSTEM: slang.h is C++ and not warning-clean under the
# engine flags) and, when the host has an archive, DE_SLANG_LIBRARY="<path>": the compiler library
# that the importer opens at run time with de_dylib_open. No target links Slang, so executables
# that never cook a shader do not load the 30 MB library, and Windows needs no IMPORTED_IMPLIB or
# DLL copy (the shim gives Slang a library loader that looks in the library's own directory for
# slang-glslang, then on the system search path for DXC). Without an archive
# for the target host (OPTIONAL), DE_SLANG_LIBRARY is not defined and shader cooks fail with a
# clear error.
if(NOT DE_BUILD_TOOLS)
    return()
endif()

include(DePrebuilt)
de_prebuilt_import(slang OPTIONAL)

add_library(de_tp_slang INTERFACE)
target_include_directories(de_tp_slang SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
set_target_properties(de_tp_slang PROPERTIES FOLDER "dengine/third_party")

set(DE_SLANG_LIBRARY "" CACHE INTERNAL "Slang compiler library opened by the shader importer")
if(DE_PREBUILT_SLANG_FOUND)
    if(DE_PREBUILT_SLANG_HOST MATCHES "^windows-")
        set(_de_slang_lib "${DE_PREBUILT_SLANG_DIR}/bin/slang-compiler.dll")
    elseif(DE_PREBUILT_SLANG_HOST MATCHES "^macos-")
        set(_de_slang_lib "${DE_PREBUILT_SLANG_DIR}/lib/libslang-compiler.dylib")
    else()
        set(_de_slang_lib "${DE_PREBUILT_SLANG_DIR}/lib/libslang-compiler.so")
    endif()
    set(DE_SLANG_LIBRARY "${_de_slang_lib}" CACHE INTERNAL
        "Slang compiler library opened by the shader importer")
    target_compile_definitions(de_tp_slang INTERFACE "DE_SLANG_LIBRARY=\"${_de_slang_lib}\"")
    unset(_de_slang_lib)

    # Smoke test (C, engine flags): the library of this host opens, exports the entry points the
    # importer resolves, and reports the pinned version. Not built when cross-compiling: the
    # library is the target's, the build host cannot open it.
    if(NOT CMAKE_CROSSCOMPILING)
        de_add_test(NAME de_tp_smoke_slang
            SOURCES smoke/smoke_slang.c
            LIBS de_tp_slang de_platform de_core
            LABELS unit third_party)
    endif()
endif()
