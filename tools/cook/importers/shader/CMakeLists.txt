# tools/cook/importers/shader: the `shader` importer (SPEC §11.6, §12.2; DENG-289). Target
# de_cook_shader, a tools-only static library: `.slang` sources to SPIR-V, DXIL and metallib with
# reflection (src/gpu/shader/shader_format.h). Registered from tools/cook/CMakeLists.txt.
#
#   shader_importer.c   settings, compiles per target and permutation, the cooked blob (C11)
#   slang_compile.cpp   the only C++ file: Slang behind a C interface (slang_compile.h;
#                       DENG-2 Q1). CXX is used by this target only.
#   shader_common_embed.c (generated)  shaders/common/*.slang as byte arrays (embed_common.cmake)
#
# Slang is opened at run time from DE_SLANG_LIBRARY (third_party/slang/), so nothing links it.
# The importer is not linked into de_cooker: a game or a tool that cooks shaders registers it
# (de_shader_import, shader_importer.h), as the shader tests do.
if(NOT TARGET de_tp_slang)
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

add_library(de_cook_shader STATIC
    shader_importer.h
    shader_importer.c
    slang_compile.h
    slang_compile.cpp
    "${_de_shader_embed}")
target_include_directories(de_cook_shader
    PUBLIC "${DE_ROOT_DIR}/tools" "${DE_ROOT_DIR}/src")
target_link_libraries(de_cook_shader
    PUBLIC de_cooker de_platform de_core
    PRIVATE de_tp_slang de_build_flags)
# UBSan's vptr check reads RTTI the Slang library does not share with this file: calls into
# Slang's COM objects would be reported. The rest of the undefined-behaviour checks stay on.
if(NOT DE_COMPILER_FRONTEND STREQUAL "MSVC" AND "undefined" IN_LIST DE_SANITIZERS)
    target_compile_options(de_cook_shader PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-fno-sanitize=vptr>")
endif()
set_target_properties(de_cook_shader PROPERTIES FOLDER "dengine/tools")
if(DE_BUILD_SHARED)
    set_target_properties(de_cook_shader PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
