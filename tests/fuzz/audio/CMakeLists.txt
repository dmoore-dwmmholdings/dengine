if(DE_BUILD_TOOLS)
    de_add_fuzz_target(NAME sound_import SOURCES fuzz_sound_import.c
        LIBS de_cooker CORPUS corpus/sound_import)
    target_include_directories(fuzz_sound_import PRIVATE "${DE_ROOT_DIR}/src" "${DE_ROOT_DIR}/tools")
endif()
