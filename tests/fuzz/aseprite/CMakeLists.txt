if(NOT DE_BUILD_TOOLS)
    return()
endif()
de_add_fuzz_target(NAME aseprite SOURCES fuzz_aseprite.c LIBS de_cooker
    CORPUS "${DE_ROOT_DIR}/tests/data/aseprite")
if(TARGET fuzz_aseprite)
    target_include_directories(fuzz_aseprite PRIVATE "${DE_ROOT_DIR}/tools")
endif()
