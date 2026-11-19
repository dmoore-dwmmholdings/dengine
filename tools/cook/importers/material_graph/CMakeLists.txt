# tools/cook/importers/material_graph: material graph to Slang code generation (DENG-579,
# SPEC §12.6, DIDE-R161). Part of the cooker library (de_cooker); it compiles the generated module
# through the shader importer's Slang shim, so it needs Slang like the shader importer does.
#
#   material_graph.h/.c   graph checks, parameter layout, Slang emission, error mapping (C11)
if(NOT TARGET de_tp_slang OR NOT TARGET de_cooker)
    return()
endif()

target_sources(de_cooker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/material_graph.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/material_graph.c")

# The `material_graph` asset importer (DENG-770): cooks a graph into DMGR plus a default material.
#   mg_import.h/.c   settings, generation, per-target compile, DMGR and DMAT output
target_sources(de_cooker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/mg_import.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/mg_import.c")
# The built-in importer table (../engine_importers.c) lists `material_graph` only when compiled in.
target_compile_definitions(de_cooker PRIVATE DE_COOKER_HAS_MATERIAL_GRAPH_IMPORTER=1)
# The engine's own Slang modules the generated code imports; $DE_ENGINE_SHADER_ROOT overrides it.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/mg_import.c" TARGET_DIRECTORY de_cooker
    PROPERTIES COMPILE_DEFINITIONS "DE_COOK_ENGINE_SHADER_ROOT=\"${DE_ROOT_DIR}/shaders\"")
