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
