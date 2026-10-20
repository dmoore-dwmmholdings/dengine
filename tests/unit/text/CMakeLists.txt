if(DE_BUILD_TOOLS)
 de_add_test(NAME text_import SOURCES test_font_import.c test_bmfont.c test_pixel_font.c
   LIBS de_cooker de_text de_platform de_tp_stb)
 if(TARGET test_text_import)
  target_include_directories(test_text_import PRIVATE "${DE_ROOT_DIR}/src" "${DE_ROOT_DIR}/tools")
  target_compile_definitions(test_text_import PRIVATE "DE_FONT_DATA=\"${DE_ROOT_DIR}/tests/data/fonts/latin\"")
 endif()
endif()

if(TARGET test_text_import)
 target_sources(test_text_import PRIVATE test_font_runtime.c test_fallback.c)
endif()

if(TARGET test_text_import)
 target_link_libraries(test_text_import PRIVATE de_tp_harfbuzz)
endif()
if(TARGET test_text_import)
 target_sources(test_text_import PRIVATE test_shaper.c test_itemize.c test_shape_cache.c)
endif()
if(TARGET test_text_import)
 target_sources(test_text_import PRIVATE test_atlas_packer.c test_glyph_cache.c)
endif()
if(DE_BUILD_TOOLS AND DE_HAS_RENDER)
 de_add_test(NAME text_render SOURCES test_text_batch.c LIBS de_text de_cooker de_render de_gpu de_platform)
 if(TARGET test_text_render)
  target_include_directories(test_text_render PRIVATE "${DE_ROOT_DIR}/src" "${DE_ROOT_DIR}/tools")
  target_link_libraries(test_text_render PRIVATE de_tp_stb)
  target_compile_definitions(test_text_render PRIVATE "DE_FONT_DATA=\"${DE_ROOT_DIR}/tests/data/fonts/latin\"")
  target_compile_definitions(test_text_render PRIVATE "DE_TEXT_RENDER_CAPTURE=\"${CMAKE_CURRENT_BINARY_DIR}/msdf-coverage.png\"")
 endif()
endif()

# DENG-261: full Unicode conformance has no GPU dependency.
de_add_test(NAME text_layout SOURCES test_bidi.c test_linebreak.c LIBS de_text de_tp_sheenbidi de_tp_stb de_tp_harfbuzz)
if(TARGET test_text_layout)
 target_include_directories(test_text_layout PRIVATE "${DE_ROOT_DIR}/src")
 target_compile_definitions(test_text_layout PRIVATE "DE_UNICODE_DATA=\"${DE_ROOT_DIR}/third_party/unicode/unicodetools/data/ucd/dev\"")
 if(WIN32)
  target_compile_definitions(test_text_layout PRIVATE _CRT_SECURE_NO_WARNINGS)
 endif()
endif()

if(TARGET test_text_layout AND DE_BUILD_TOOLS)
 target_sources(test_text_layout PRIVATE test_paragraph.c test_hit_test.c)
 target_link_libraries(test_text_layout PRIVATE de_cooker de_platform)
 target_include_directories(test_text_layout PRIVATE "${DE_ROOT_DIR}/tools")
 target_compile_definitions(test_text_layout PRIVATE "DE_FONT_DATA=\"${DE_ROOT_DIR}/tests/data/fonts/latin\"")
endif()

# DENG-264: original internal localization object is independent of font/GPU initialization.
if(DE_BUILD_TOOLS)
    de_add_test(NAME text_loc SOURCES test_string_table.c test_message_format.c test_plurals.c
        test_number_format.c test_pseudo_loc.c
        "${DE_ROOT_DIR}/src/text/loc/loc.c" "${DE_ROOT_DIR}/src/text/loc/loc_api.c"
        LIBS de_loc_common de_asset de_cooker de_platform de_test_support de_tp_tomlc17 TIMEOUT 30)
    target_include_directories(test_text_loc PRIVATE "${DE_ROOT_DIR}/src" "${DE_ROOT_DIR}/tools")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/loc-tmp")
    target_compile_definitions(test_text_loc PRIVATE "DE_LOC_TMP_DIR=\"${CMAKE_CURRENT_BINARY_DIR}/loc-tmp\"")
endif()

# Runtime fixtures prepare real cooked packs when the runtime has no cooker.
# The tool libraries belong to the tests only, never to de_text/de_asset.
if(DE_BUILD_TOOLS)
 file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/packed-tmp")
 foreach(_text_fixture test_text_import test_text_layout test_text_render test_text_loc)
  if(TARGET ${_text_fixture})
   target_link_libraries(${_text_fixture} PRIVATE de_pack_lib)
   target_compile_definitions(${_text_fixture} PRIVATE
    "DE_TEXT_PACK_TMP=\"${CMAKE_CURRENT_BINARY_DIR}/packed-tmp\"")
  endif()
 endforeach()
endif()
