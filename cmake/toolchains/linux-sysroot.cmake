# linux-sysroot.cmake: cross-compile to Linux x86-64 with Clang + lld and a Linux sysroot
# (SPEC §3.2: Windows host -> Linux). See docs/building.md, "Linux from a sysroot".
#
# The sysroot is a copy of a Linux root file system with the C runtime and the development
# headers/libraries the build needs (glibc 2.31+ to match SPEC §3.1). Give its path in the
# environment variable DE_LINUX_SYSROOT, or with -DDE_LINUX_SYSROOT=<dir> on the first configure.
#
# Toolchain files run again for every try_compile, where cache entries from the command line are
# not visible. The resolved path is therefore also exported to the environment of this CMake
# process and listed in CMAKE_TRY_COMPILE_PLATFORM_VARIABLES.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(_de_triple x86_64-linux-gnu)

if(DE_LINUX_SYSROOT)
    set(_de_sysroot "${DE_LINUX_SYSROOT}")
elseif(DEFINED ENV{DE_LINUX_SYSROOT} AND NOT "$ENV{DE_LINUX_SYSROOT}" STREQUAL "")
    set(_de_sysroot "$ENV{DE_LINUX_SYSROOT}")
else()
    message(FATAL_ERROR
        "linux-sysroot.cmake: no Linux sysroot. Set the environment variable DE_LINUX_SYSROOT "
        "(or pass -DDE_LINUX_SYSROOT=<dir>) to the root of an x86-64 Linux sysroot. "
        "See docs/building.md, section \"Linux from a sysroot\".")
endif()
file(TO_CMAKE_PATH "${_de_sysroot}" _de_sysroot)
if(NOT IS_DIRECTORY "${_de_sysroot}/usr/include")
    message(FATAL_ERROR
        "linux-sysroot.cmake: DE_LINUX_SYSROOT='${_de_sysroot}' has no usr/include directory. "
        "It must point at the root of an x86-64 Linux sysroot. "
        "See docs/building.md, section \"Linux from a sysroot\".")
endif()
set(DE_LINUX_SYSROOT "${_de_sysroot}" CACHE PATH "Root of the x86-64 Linux sysroot")
set(ENV{DE_LINUX_SYSROOT} "${_de_sysroot}")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES DE_LINUX_SYSROOT)

set(CMAKE_SYSROOT "${_de_sysroot}")

if(NOT DEFINED CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER clang)
endif()
if(NOT DEFINED CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER clang++)
endif()
# Passed as --target=<triple> to every compile and link.
set(CMAKE_C_COMPILER_TARGET ${_de_triple})
set(CMAKE_CXX_COMPILER_TARGET ${_de_triple})

# Link with lld: the host linker (link.exe, ld64) cannot produce ELF.
foreach(_de_kind EXE SHARED MODULE)
    set(CMAKE_${_de_kind}_LINKER_FLAGS_INIT "-fuse-ld=lld")
endforeach()

# Programs come from the host; headers, libraries and packages only from the sysroot.
set(CMAKE_FIND_ROOT_PATH "${_de_sysroot}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# FindPkgConfig invokes a host program whose search paths do not follow CMake's
# root-path rules. Restrict its metadata to the target sysroot as well.
set(_de_pkgconfig_dirs
    "${_de_sysroot}/usr/lib/${_de_triple}/pkgconfig"
    "${_de_sysroot}/usr/lib/pkgconfig"
    "${_de_sysroot}/usr/lib64/pkgconfig"
    "${_de_sysroot}/usr/share/pkgconfig"
    "${_de_sysroot}/lib/${_de_triple}/pkgconfig"
    "${_de_sysroot}/lib/pkgconfig"
    "${_de_sysroot}/lib64/pkgconfig"
    "${_de_sysroot}/usr/local/lib/${_de_triple}/pkgconfig"
    "${_de_sysroot}/usr/local/lib/pkgconfig"
    "${_de_sysroot}/usr/local/lib64/pkgconfig"
    "${_de_sysroot}/usr/local/share/pkgconfig")
if(CMAKE_HOST_WIN32)
    list(JOIN _de_pkgconfig_dirs ";" _de_pkgconfig_path)
else()
    list(JOIN _de_pkgconfig_dirs ":" _de_pkgconfig_path)
endif()
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${_de_sysroot}")
set(ENV{PKG_CONFIG_LIBDIR} "${_de_pkgconfig_path}")
set(ENV{PKG_CONFIG_PATH} "")
if(CMAKE_HOST_WIN32)
    # Native Windows pkg-config/pkgconf otherwise rewrites prefix=/usr from
    # the .pc location, corrupting Linux multiarch paths before sysrooting.
    # Keep Linux package prefixes intact; FindPkgConfig forwards these options.
    list(APPEND PKG_CONFIG_ARGN --dont-define-prefix)
    list(REMOVE_DUPLICATES PKG_CONFIG_ARGN)
endif()
# FindPkgConfig otherwise appends unrooted host CMAKE_PREFIX_PATH directories.
set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH FALSE)
