# tools/abi: the ABI check (DENG-128, SPEC §19.3, §5.1; policy in docs/versioning.md).
#
#   abi_dump.py   public headers -> JSON (functions, struct/union layouts measured on the target
#                 by a generated probe, enums, typedefs, version macros). Parses with clang's JSON
#                 AST; Python 3.10+, stdlib only.
#   abi_check.py  baseline.json current.json -> breaking/compatible report; non-zero exit on a
#                 breaking change unless the major version increased.
#
# de_abi_probe: the generated layout probe for include/dengine/dengine.h, built with this build's
# compiler and strict flags (-Wall -Wextra -Wpedantic -Werror, or /W4 /WX), so every preset,
# including the windows-xwin-dev cross build, proves the probe compiles for its target. The probe
# source is generated with host clang. CI (.github/workflows/abi-check.yml) runs the dump itself.
# The tool tests are in tests/unit/abi.
find_package(Python3 3.10 COMPONENTS Interpreter)
find_program(DE_ABI_CLANG NAMES clang clang-20 clang-19 clang-18 DOC "clang for the ABI dump")
if(NOT Python3_Interpreter_FOUND OR NOT DE_ABI_CLANG)
    message(STATUS "tools/abi: no Python 3.10+ or host clang; de_abi_probe not built")
    return()
endif()

set(_de_abi_probe "${CMAKE_CURRENT_BINARY_DIR}/de_abi_probe.c")
file(GLOB _de_abi_headers CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/include/dengine/*.h")
set(_de_abi_args)
if(CMAKE_C_COMPILER_TARGET)
    list(APPEND _de_abi_args --target "${CMAKE_C_COMPILER_TARGET}")
endif()
add_custom_command(OUTPUT "${_de_abi_probe}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/abi_dump.py"
            --include-dir "${PROJECT_SOURCE_DIR}/include" --header dengine/dengine.h
            --clang "${DE_ABI_CLANG}" ${_de_abi_args} --emit-probe "${_de_abi_probe}"
    DEPENDS ${_de_abi_headers} "${CMAKE_CURRENT_SOURCE_DIR}/abi_dump.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/layout_probe.c.in"
    COMMENT "Generating the ABI layout probe"
    VERBATIM)

add_executable(de_abi_probe "${_de_abi_probe}")
target_include_directories(de_abi_probe PRIVATE "${PROJECT_SOURCE_DIR}/include")
set_target_properties(de_abi_probe PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_link_libraries(de_abi_probe PRIVATE de_build_flags)
if(MSVC)
    target_compile_options(de_abi_probe PRIVATE /W4 /WX)
else()
    target_compile_options(de_abi_probe PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
