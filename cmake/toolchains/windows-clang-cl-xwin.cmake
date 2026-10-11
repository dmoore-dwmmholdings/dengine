# windows-clang-cl-xwin.cmake: cross-compile to Windows x86-64 from macOS or Linux with clang-cl,
# lld-link, llvm-lib, llvm-rc and llvm-mt (SPEC §3.2). See docs/building.md, section
# "Windows from macOS or Linux (xwin)".
#
# The Windows SDK and the MSVC CRT are NOT part of this repository and are never downloaded by
# the build (CONVENTIONS §6). The user runs `xwin --accept-license splat --output ~/.xwin`, which
# means the user accepts Microsoft's license, and this file only reads that directory:
#
#   DE_XWIN_DIR   env var or -D cache entry. Default: $HOME/.xwin
#   DE_LLVM_DIR   optional env var or -D cache entry: an LLVM install whose bin/ has clang-cl.
#                 Default search: PATH, Homebrew llvm / lld, /usr/lib/llvm-*.
#
# Two xwin layouts are accepted:
#   splat (default)            crt/include, crt/lib/x86_64, sdk/include/*, sdk/lib/{um,ucrt}/x86_64
#                              -> explicit -imsvc and /libpath: flags
#   --use-winsysroot-style     VC/Tools/MSVC/<ver>, Windows Kits/10/...
#                              -> /winsysroot (clang-cl) and /winsysroot: (lld-link)
#
# Toolchain files run again for every try_compile, where cache entries from the command line are
# not visible. Resolved paths are therefore exported to this CMake process's environment and
# listed in CMAKE_TRY_COMPILE_PLATFORM_VARIABLES.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(_de_doc "docs/building.md, section \"Windows from macOS or Linux (xwin)\"")
set(_de_triple x86_64-pc-windows-msvc)

# ---- Windows SDK + CRT (from xwin) ----
if(DE_XWIN_DIR)
    set(_de_xwin "${DE_XWIN_DIR}")
elseif(DEFINED ENV{DE_XWIN_DIR} AND NOT "$ENV{DE_XWIN_DIR}" STREQUAL "")
    set(_de_xwin "$ENV{DE_XWIN_DIR}")
else()
    set(_de_xwin "$ENV{HOME}/.xwin")
endif()
file(TO_CMAKE_PATH "${_de_xwin}" _de_xwin)

if(IS_DIRECTORY "${_de_xwin}/crt/include" AND IS_DIRECTORY "${_de_xwin}/sdk/include/ucrt")
    set(_de_xwin_layout splat)
elseif(IS_DIRECTORY "${_de_xwin}/VC/Tools/MSVC" AND IS_DIRECTORY "${_de_xwin}/Windows Kits/10")
    set(_de_xwin_layout winsysroot)
else()
    message(FATAL_ERROR
        "windows-clang-cl-xwin.cmake: no Windows SDK/CRT found at '${_de_xwin}'.\n"
        "Set the environment variable DE_XWIN_DIR (default: ~/.xwin) to the output directory of "
        "`xwin --accept-license splat --output <dir>`. Running xwin means you accept Microsoft's "
        "license; the build never downloads the SDK for you.\n"
        "See ${_de_doc}.")
endif()
set(DE_XWIN_DIR "${_de_xwin}" CACHE PATH "Windows SDK/CRT directory made by xwin splat")
set(ENV{DE_XWIN_DIR} "${_de_xwin}")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES DE_XWIN_DIR)

# ---- LLVM tools ----
set(_de_llvm_hints "")
if(DE_LLVM_DIR)
    list(APPEND _de_llvm_hints "${DE_LLVM_DIR}/bin")
elseif(DEFINED ENV{DE_LLVM_DIR} AND NOT "$ENV{DE_LLVM_DIR}" STREQUAL "")
    list(APPEND _de_llvm_hints "$ENV{DE_LLVM_DIR}/bin")
endif()
# Apple's toolchain has no clang-cl or lld-link: Homebrew `llvm` gives clang-cl and the llvm-*
# tools, Homebrew `lld` gives lld-link. Debian/Ubuntu install versioned trees in /usr/lib/llvm-N.
file(GLOB _de_distro_llvm LIST_DIRECTORIES true "/usr/lib/llvm-*/bin")
list(SORT _de_distro_llvm COMPARE NATURAL ORDER DESCENDING)
list(APPEND _de_llvm_hints
    /opt/homebrew/opt/llvm/bin /opt/homebrew/opt/lld/bin /opt/homebrew/bin
    /usr/local/opt/llvm/bin /usr/local/opt/lld/bin /usr/local/bin
    ${_de_distro_llvm})

set(_de_missing "")
macro(de__find_llvm_tool var tool)
    if(NOT ${var})
        find_program(${var} NAMES ${tool} HINTS ${_de_llvm_hints} NO_CACHE)
    endif()
    if(NOT ${var})
        list(APPEND _de_missing ${tool})
    endif()
endmacro()
de__find_llvm_tool(_de_clang_cl clang-cl)
de__find_llvm_tool(_de_lld_link lld-link)
de__find_llvm_tool(_de_llvm_lib llvm-lib)
de__find_llvm_tool(_de_llvm_rc llvm-rc)
de__find_llvm_tool(_de_llvm_mt llvm-mt)
if(_de_missing)
    string(REPLACE ";" ", " _de_missing "${_de_missing}")
    message(FATAL_ERROR
        "windows-clang-cl-xwin.cmake: LLVM tools not found: ${_de_missing}.\n"
        "On macOS: `brew install llvm lld` (Apple's toolchain has no clang-cl or lld-link). "
        "On Linux: install clang, lld and llvm. Or set DE_LLVM_DIR to an LLVM install.\n"
        "See ${_de_doc}.")
endif()

if(NOT DEFINED CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER "${_de_clang_cl}")
endif()
if(NOT DEFINED CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER "${_de_clang_cl}")
endif()
set(CMAKE_C_COMPILER_TARGET ${_de_triple})
set(CMAKE_CXX_COMPILER_TARGET ${_de_triple})
set(CMAKE_LINKER "${_de_lld_link}" CACHE FILEPATH "Linker (lld-link)")
set(CMAKE_AR "${_de_llvm_lib}" CACHE FILEPATH "Archiver (llvm-lib)")
set(CMAKE_RC_COMPILER "${_de_llvm_rc}" CACHE FILEPATH "Resource compiler (llvm-rc)")
set(CMAKE_MT "${_de_llvm_mt}" CACHE FILEPATH "Manifest tool (llvm-mt)")

# ---- Flags ----
# --target: Homebrew clang-cl on Apple Silicon would otherwise target aarch64-pc-windows-msvc.
# -imsvc marks the SDK/CRT headers as system headers, so /W4 /WX never fires inside them.
set(_de_cflags "--target=${_de_triple}")
set(_de_rcflags "")
set(_de_ldflags "/machine:x64")
if(_de_xwin_layout STREQUAL "splat")
    set(_de_incs
        "${_de_xwin}/crt/include"
        "${_de_xwin}/sdk/include/ucrt"
        "${_de_xwin}/sdk/include/um"
        "${_de_xwin}/sdk/include/shared"
        "${_de_xwin}/sdk/include/winrt")
    set(_de_libs
        "${_de_xwin}/crt/lib/x86_64"
        "${_de_xwin}/sdk/lib/um/x86_64"
        "${_de_xwin}/sdk/lib/ucrt/x86_64")
    foreach(_de_d IN LISTS _de_incs)
        string(APPEND _de_cflags " -imsvc \"${_de_d}\"")
        string(APPEND _de_rcflags " -I \"${_de_d}\"")
    endforeach()
    foreach(_de_d IN LISTS _de_libs)
        string(APPEND _de_ldflags " \"/libpath:${_de_d}\"")
    endforeach()
    set(_de_debug_crt "${_de_xwin}/crt/lib/x86_64/msvcrtd.lib")
else()
    string(APPEND _de_cflags " \"/winsysroot\" \"${_de_xwin}\"")
    string(APPEND _de_ldflags " \"/winsysroot:${_de_xwin}\"")
    file(GLOB _de_debug_crt "${_de_xwin}/VC/Tools/MSVC/*/lib/x64/msvcrtd.lib")
    if(_de_debug_crt)
        list(GET _de_debug_crt 0 _de_debug_crt)
    endif()
endif()

set(CMAKE_C_FLAGS_INIT "${_de_cflags}")
set(CMAKE_CXX_FLAGS_INIT "${_de_cflags}")
set(CMAKE_RC_FLAGS_INIT "${_de_rcflags}")
foreach(_de_kind EXE SHARED MODULE)
    set(CMAKE_${_de_kind}_LINKER_FLAGS_INIT "${_de_ldflags}")
endforeach()

# `xwin splat` leaves out the debug CRT (msvcrtd.lib, ucrtd.lib) unless --include-debug-libs is
# given. Without it, use the release CRT in every config, so Debug builds still link.
if(NOT _de_debug_crt OR NOT EXISTS "${_de_debug_crt}")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
endif()
# try_compile checks run in a config of CMake's choosing; Release never needs the debug CRT.
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)

# Programs come from the host; nothing else is searched outside the xwin tree and the project.
set(CMAKE_FIND_ROOT_PATH "${_de_xwin}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
