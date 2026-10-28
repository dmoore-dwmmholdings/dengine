if(DE_BUILD_TOOLS)
    de_add_fuzz_target(NAME shader_cli SOURCES fuzz_shader_cli.c LIBS de_cook_shader_cmd
        CORPUS corpus/shader_cli)
endif()
