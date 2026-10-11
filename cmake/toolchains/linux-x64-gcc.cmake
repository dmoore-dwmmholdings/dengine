# linux-x64-gcc.cmake: native Linux x86-64 build with GCC (SPEC §3.2: GCC must build cleanly in
# CI so the code stays portable; Clang is the primary compiler).
#
# Native build: CMAKE_SYSTEM_NAME is not set. Use -DCMAKE_C_COMPILER=gcc-13 (and the matching
# g++) to select a versioned install.

if(NOT DEFINED CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER gcc)
endif()
if(NOT DEFINED CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER g++)
endif()

cmake_host_system_information(RESULT _de_host_arch QUERY OS_PLATFORM)
# Toolchain files run again for every try_compile; the env marker keeps it to one warning.
if(NOT _de_host_arch MATCHES "^(x86_64|AMD64|amd64)$" AND NOT DEFINED ENV{DE__ARCH_WARNED})
    set(ENV{DE__ARCH_WARNED} 1)
    message(WARNING "linux-x64-gcc.cmake: host is ${_de_host_arch}, not x86-64. The build is "
                    "native ${_de_host_arch}, which is not a supported v1 target.")
endif()
