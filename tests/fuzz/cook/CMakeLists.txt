if(DE_BUILD_TOOLS)
    de_add_fuzz_target(NAME shader_cli SOURCES fuzz_shader_cli.c LIBS de_cook_shader_cmd
        CORPUS corpus/shader_cli)
endif()

# DENG-579: material graph to Slang code generation (generation only; Slang is not run).
if(DE_BUILD_TOOLS AND TARGET de_tp_slang)
    de_add_fuzz_target(NAME material_graph SOURCES fuzz_material_graph.c LIBS de_cooker
        CORPUS corpus/material_graph)
    if(TARGET fuzz_material_graph)
        target_include_directories(fuzz_material_graph PRIVATE "${DE_ROOT_DIR}/src"
            "${DE_ROOT_DIR}/tools")
    endif()
endif()
