# tools/cook/importers/shader: the `shader` importer (SPEC §11.6, §12.2; DENG-289). `.slang`
# sources to SPIR-V, DXIL and metallib with reflection (src/gpu/shader/shader_format.h).
#
#   shader_importer.c   settings, compiles per target and permutation, the cooked blob (C11)
#   slang_compile.cpp   the only C++ file: Slang behind a C interface (slang_compile.h;
#                       DENG-2 Q1).
#   shader_common_embed.c (generated)  shaders/common/*.slang as byte arrays (embed_common.cmake)
#
# DENG-409: the importer is part of the cooker library (de_cooker), like the texture and model
# importers, so the cooker's built-in importer table (tools/cook/importers/engine_importers.c,
# de_cooker_builtin_importer) names it: `de-cook cook` and the runtime `shader` type of Debug and
# Dev builds get it from there. Slang is still opened at run time from DE_SLANG_LIBRARY
# (third_party/slang/) on the first shader cook, so nothing links the Slang library.
#
# de_cook_shader is kept as an INTERFACE name for the targets that linked the separate library of
# DENG-289 (tests, the golden harness, examples): it brings de_cooker.
if(NOT TARGET de_tp_slang OR NOT TARGET de_cooker)
    return()
endif()

set(_de_shader_common_dir "${DE_ROOT_DIR}/shaders/common")
set(_de_shader_common bindless push_constants math color) # bindless first: preloaded
set(_de_shader_common_files "")
foreach(_m IN LISTS _de_shader_common)
    list(APPEND _de_shader_common_files "${_de_shader_common_dir}/${_m}.slang")
endforeach()
set(_de_shader_embed "${CMAKE_CURRENT_BINARY_DIR}/shader_common_embed.c")
add_custom_command(
    OUTPUT "${_de_shader_embed}"
    COMMAND "${CMAKE_COMMAND}" "-DOUT=${_de_shader_embed}" "-DDIR=${_de_shader_common_dir}"
            "-DNAMES=${_de_shader_common}" -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_common.cmake"
    DEPENDS ${_de_shader_common_files} "${CMAKE_CURRENT_SOURCE_DIR}/embed_common.cmake"
    COMMENT "Embedding shaders/common/*.slang"
    VERBATIM)
# The generated file is a source of de_cooker, which lives in another directory (tools/cook/lib):
# the custom target here runs the command, and de_cooker waits for it (CMP0118: the GENERATED
# property is visible in every directory).
add_custom_target(de_cook_shader_embed DEPENDS "${_de_shader_embed}")
set_target_properties(de_cook_shader_embed PROPERTIES FOLDER "dengine/tools")

set(_de_shader_dir "${CMAKE_CURRENT_SOURCE_DIR}")
target_sources(de_cooker PRIVATE
    "${_de_shader_dir}/shader_importer.h"
    "${_de_shader_dir}/shader_importer.c"
    "${_de_shader_dir}/slang_compile.h"
    "${_de_shader_dir}/slang_compile.cpp"
    "${_de_shader_embed}")
add_dependencies(de_cooker de_cook_shader_embed)
# The built-in importer table (../engine_importers.c) lists `shader` only when it is compiled in.
target_compile_definitions(de_cooker PRIVATE DE_COOKER_HAS_SHADER_IMPORTER=1)
# Slang's headers (SYSTEM) and DE_SLANG_LIBRARY, the library the shim opens.
target_link_libraries(de_cooker PRIVATE de_tp_slang)
# UBSan's vptr check reads RTTI the Slang library does not share with the shim: calls into Slang's
# COM objects would be reported. The rest of the undefined-behaviour checks stay on.
if(NOT DE_COMPILER_FRONTEND STREQUAL "MSVC" AND "undefined" IN_LIST DE_SANITIZERS)
    set_source_files_properties("${_de_shader_dir}/slang_compile.cpp" TARGET_DIRECTORY de_cooker
        PROPERTIES COMPILE_OPTIONS "-fno-sanitize=vptr")
endif()

add_library(de_cook_shader INTERFACE)
target_link_libraries(de_cook_shader INTERFACE de_cooker)
