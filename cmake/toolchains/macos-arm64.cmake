# macos-arm64.cmake: native macOS build on Apple Silicon (SPEC §3.1, §3.2).
#
# Apple Clang from Xcode / the Command Line Tools is the compiler. Intel Macs are out of scope
# (DENG-2 Q3), so the only architecture is arm64. This file does not set CMAKE_SYSTEM_NAME: the
# build is native, not a cross build.
#
# Override the compiler with -DCMAKE_C_COMPILER=... if you need to (for example Homebrew clang).

set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "macOS target architecture (arm64 only)")
set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS version (SPEC §3.1)")

# Use Apple Clang even when Homebrew LLVM is first in PATH. `xcrun --find` returns the Xcode or
# CLT toolchain binary; fall back to the PATH lookup if xcrun is not available.
if(NOT DEFINED CMAKE_C_COMPILER)
    execute_process(COMMAND xcrun --find clang
        OUTPUT_VARIABLE _de_clang OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _de_rc ERROR_QUIET)
    if(_de_rc EQUAL 0 AND EXISTS "${_de_clang}")
        set(CMAKE_C_COMPILER "${_de_clang}")
        set(CMAKE_OBJC_COMPILER "${_de_clang}")
    else()
        set(CMAKE_C_COMPILER clang)
        set(CMAKE_OBJC_COMPILER clang)
    endif()
endif()
if(NOT DEFINED CMAKE_CXX_COMPILER)
    execute_process(COMMAND xcrun --find clang++
        OUTPUT_VARIABLE _de_clangxx OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _de_rc ERROR_QUIET)
    if(_de_rc EQUAL 0 AND EXISTS "${_de_clangxx}")
        set(CMAKE_CXX_COMPILER "${_de_clangxx}")
        set(CMAKE_OBJCXX_COMPILER "${_de_clangxx}")
    else()
        set(CMAKE_CXX_COMPILER clang++)
        set(CMAKE_OBJCXX_COMPILER clang++)
    endif()
endif()
