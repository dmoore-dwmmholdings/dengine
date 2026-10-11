# linux-x64-clang.cmake: native Linux x86-64 build with Clang, the primary compiler (SPEC §3.2).
#
# Native build: CMAKE_SYSTEM_NAME is not set. The compiler names are looked up in PATH, so a
# versioned install works with -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18.
# To build on a non-Linux host, use linux-sysroot.cmake instead.

if(NOT DEFINED CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER clang)
endif()
if(NOT DEFINED CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER clang++)
endif()

# Linux arm64 is not a v1 target (DENG-2 Q3). Warn instead of failing so that a developer on an
# arm64 machine can still build and run the portable tests.
cmake_host_system_information(RESULT _de_host_arch QUERY OS_PLATFORM)
# Toolchain files run again for every try_compile; the env marker keeps it to one warning.
if(NOT _de_host_arch MATCHES "^(x86_64|AMD64|amd64)$" AND NOT DEFINED ENV{DE__ARCH_WARNED})
    set(ENV{DE__ARCH_WARNED} 1)
    message(WARNING "linux-x64-clang.cmake: host is ${_de_host_arch}, not x86-64. The build is "
                    "native ${_de_host_arch}, which is not a supported v1 target.")
endif()
