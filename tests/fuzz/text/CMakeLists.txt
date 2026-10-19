de_add_fuzz_target(NAME bmfont SOURCES fuzz_bmfont.c
 "${DE_ROOT_DIR}/src/text/import/bmfont_import.c" LIBS de_core CORPUS corpus/bmfont)
if(TARGET fuzz_bmfont)
 target_include_directories(fuzz_bmfont PRIVATE "${DE_ROOT_DIR}/src")
endif()

if(DE_BUILD_TOOLS)
 de_add_fuzz_target(NAME text_layout SOURCES fuzz_text_layout.c LIBS de_text de_cooker de_platform CORPUS corpus/layout)
 if(TARGET fuzz_text_layout)
  target_include_directories(fuzz_text_layout PRIVATE "${DE_ROOT_DIR}/src")
  target_compile_definitions(fuzz_text_layout PRIVATE "DE_LAYOUT_FONT_ROOT=\"${DE_ROOT_DIR}/tests/data/fonts\"")
 endif()
endif()
