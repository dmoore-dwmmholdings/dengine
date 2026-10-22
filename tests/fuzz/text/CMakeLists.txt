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
  target_link_libraries(fuzz_text_layout PRIVATE de_pack_lib)
  target_include_directories(fuzz_text_layout PRIVATE "${DE_ROOT_DIR}/tools")
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/layout-packed-tmp")
  target_compile_definitions(fuzz_text_layout PRIVATE
   "DE_LAYOUT_PACK_TMP=\"${CMAKE_CURRENT_BINARY_DIR}/layout-packed-tmp\"")
 endif()
endif()

# DENG-264: the actual bounded formatter/loader, fully instrumented with DE_SANITIZE builds.
set(_loc_fuzz_sources
    "${DE_ROOT_DIR}/src/text/loc/message_format.c"
    "${DE_ROOT_DIR}/src/text/loc/string_table_format.c"
    "${DE_ROOT_DIR}/src/text/loc/plural_rules.c"
    "${DE_ROOT_DIR}/src/text/loc/plural_rules_gen.c"
    "${DE_ROOT_DIR}/src/text/loc/number_format.c"
    "${DE_ROOT_DIR}/src/text/loc/number_data_gen.c"
    "${DE_ROOT_DIR}/src/text/loc/pseudo_loc.c")
de_add_fuzz_target(NAME message_format SOURCES fuzz_message_format.c ${_loc_fuzz_sources} LIBS de_core
    CORPUS corpus/message_format)
de_add_fuzz_target(NAME string_table SOURCES fuzz_string_table.c ${_loc_fuzz_sources} LIBS de_core
    CORPUS corpus/string_table)
foreach(_loc_fuzz fuzz_message_format fuzz_string_table)
    if(TARGET ${_loc_fuzz})
        target_include_directories(${_loc_fuzz} PRIVATE "${DE_ROOT_DIR}/src")
    endif()
endforeach()
