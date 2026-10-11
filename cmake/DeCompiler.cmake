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
# DE_SANITIZE ("" | address | undefined | thread, or a comma list such as address,undefined)
# applies to every target, third-party included, because sanitizer runtimes expect the whole
# program to be instrumented (used by CI, DENG-66). DE_SANITIZERS is the parsed list.
include_guard(GLOBAL)

option(DE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors (-Werror, /WX)" ON)
set(DE_SANITIZE "" CACHE STRING
    "Sanitizers: empty, address, undefined, thread, or a comma list (address,undefined)")
set_property(CACHE DE_SANITIZE PROPERTY STRINGS
    "" address undefined thread "address,undefined" "thread,undefined")

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
# ASan and UBSan are usually run together (one CI job, DENG-66), so DE_SANITIZE takes a comma list.
string(REPLACE "," ";" DE_SANITIZERS "${DE_SANITIZE}")
list(REMOVE_DUPLICATES DE_SANITIZERS)
if(DE_SANITIZERS)
    foreach(_de_san IN LISTS DE_SANITIZERS)
        if(NOT _de_san MATCHES "^(address|undefined|thread)$")
            message(FATAL_ERROR "dengine: DE_SANITIZE must be empty, or a comma list of address, "
                                "undefined and thread (got '${DE_SANITIZE}')")
        endif()
    endforeach()
    # The ASan and TSan runtimes cannot share a process.
    if("address" IN_LIST DE_SANITIZERS AND "thread" IN_LIST DE_SANITIZERS)
        message(FATAL_ERROR "dengine: DE_SANITIZE cannot combine address and thread")
    endif()
    if(DE_COMPILER_FRONTEND STREQUAL "MSVC")
        if(NOT DE_SANITIZERS STREQUAL "address")
            message(FATAL_ERROR "dengine: MSVC/clang-cl support only DE_SANITIZE=address")
        endif()
        add_compile_options(/fsanitize=address)
    else()
        string(JOIN "," _de_san_flag ${DE_SANITIZERS})
        add_compile_options(-fsanitize=${_de_san_flag} -fno-omit-frame-pointer)
        if("undefined" IN_LIST DE_SANITIZERS)
            # Make UB reports fail the test instead of printing and continuing.
            add_compile_options(-fno-sanitize-recover=undefined)
        endif()
        add_link_options(-fsanitize=${_de_san_flag})
    endif()
endif()
