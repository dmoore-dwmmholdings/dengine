# DeCompiler.cmake: compiler flags (SPEC §3.2, CONVENTIONS §3).
#
# `de_build_flags` is an INTERFACE target with the strict warning set. Engine modules link it
# PRIVATE (de_add_module does this); tests and tools may link it too. Vendored third-party code
# must not link it, so their warnings never fail the build.
#
#   Clang/GCC:        -Wall -Wextra -Wpedantic -Wvla [-Werror] -fno-fast-math
#   MSVC / clang-cl:  /W4 [/WX] /utf-8   (/std:c11 comes from CMAKE_C_STANDARD 11)
#
# -Wunused-parameter stays on (it is part of -Wextra). Silence a deliberately unused parameter
# with `(void)p;`, never with -Wno-unused-parameter.
#
# DE_SANITIZE ("" | address | undefined | thread) applies to every target, third-party included,
# because sanitizer runtimes expect the whole program to be instrumented (used by CI, DENG-66).
include_guard(GLOBAL)

option(DE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors (-Werror, /WX)" ON)
set(DE_SANITIZE "" CACHE STRING "Sanitizer: empty, address, undefined or thread")
set_property(CACHE DE_SANITIZE PROPERTY STRINGS "" address undefined thread)

# MSVC is also true for clang-cl, which takes the MSVC-style flags.
if(MSVC)
    set(DE_COMPILER_FRONTEND MSVC)
else()
    set(DE_COMPILER_FRONTEND GNU)
endif()

set(_de_langs "C,CXX,OBJC,OBJCXX")

if(NOT TARGET de_build_flags)
    add_library(de_build_flags INTERFACE)
    if(DE_COMPILER_FRONTEND STREQUAL "MSVC")
        target_compile_options(de_build_flags INTERFACE
            "$<$<COMPILE_LANGUAGE:${_de_langs}>:/W4;/utf-8>"
            "$<$<AND:$<COMPILE_LANGUAGE:${_de_langs}>,$<BOOL:${DE_WARNINGS_AS_ERRORS}>>:/WX>")
    else()
        target_compile_options(de_build_flags INTERFACE
            "$<$<COMPILE_LANGUAGE:${_de_langs}>:-Wall;-Wextra;-Wpedantic;-Wvla;-fno-fast-math>"
            "$<$<AND:$<COMPILE_LANGUAGE:${_de_langs}>,$<BOOL:${DE_WARNINGS_AS_ERRORS}>>:-Werror>")
    endif()
endif()

# Flags for every translation unit of a deterministic module (SPEC §5.7 rule 1).
if(DE_COMPILER_FRONTEND STREQUAL "MSVC")
    set(DE_DETERMINISTIC_FLAGS /fp:precise)
else()
    set(DE_DETERMINISTIC_FLAGS -ffp-contract=off)
endif()

# ---- Sanitizers ----
if(DE_SANITIZE)
    if(NOT DE_SANITIZE MATCHES "^(address|undefined|thread)$")
        message(FATAL_ERROR "dengine: DE_SANITIZE must be empty, address, undefined or thread "
                            "(got '${DE_SANITIZE}')")
    endif()
    if(DE_COMPILER_FRONTEND STREQUAL "MSVC")
        if(NOT DE_SANITIZE STREQUAL "address")
            message(FATAL_ERROR "dengine: MSVC/clang-cl support only DE_SANITIZE=address")
        endif()
        add_compile_options(/fsanitize=address)
    else()
        add_compile_options(-fsanitize=${DE_SANITIZE} -fno-omit-frame-pointer)
        if(DE_SANITIZE STREQUAL "undefined")
            # Make UB reports fail the test instead of printing and continuing.
            add_compile_options(-fno-sanitize-recover=undefined)
        endif()
        add_link_options(-fsanitize=${DE_SANITIZE})
    endif()
endif()
