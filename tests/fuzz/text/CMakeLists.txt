de_add_fuzz_target(NAME bmfont SOURCES fuzz_bmfont.c
 "${DE_ROOT_DIR}/src/text/import/bmfont_import.c" LIBS de_core CORPUS corpus/bmfont)
if(TARGET fuzz_bmfont)
 target_include_directories(fuzz_bmfont PRIVATE "${DE_ROOT_DIR}/src")
endif()
