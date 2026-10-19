if(TARGET de_ui)
 de_add_test(NAME ui_layout SOURCES test_flex.c test_grid.c test_anchor_popup.c test_ui_scale.c test_layout_tree.c test_stylesheet.c test_style_resolve.c test_tween.c LIBS de_ui de_platform de_ecs de_reflect de_tp_tomlc17)
 if(TARGET test_ui_layout)
  target_include_directories(test_ui_layout PRIVATE "${DE_ROOT_DIR}/src")
 endif()
endif()

if(TARGET test_ui_layout AND DE_BUILD_TOOLS)
 target_link_libraries(test_ui_layout PRIVATE de_cooker de_tp_stb de_tp_harfbuzz)
 target_include_directories(test_ui_layout PRIVATE "${DE_ROOT_DIR}/tools")
 target_compile_definitions(test_ui_layout PRIVATE DE_UI_TEST_FONTS=1 "DE_FONT_DATA=\"${DE_ROOT_DIR}/tests/data/fonts/latin\"")
endif()

if(TARGET test_ui_layout AND DE_BUILD_TOOLS AND DE_HAS_RENDER)
 target_compile_definitions(test_ui_layout PRIVATE "DE_UI_ASSET_ROOT=\"${DE_ROOT_DIR}\"")
endif()

# DENG-317 real retained commands, ownership and allocation/performance checks.
if(TARGET de_ui)
 de_add_test(NAME ui_draw SOURCES test_draw_list.c LIBS de_ui de_platform de_ecs de_reflect de_test_support LABELS ui)
 target_include_directories(test_ui_draw PRIVATE "${DE_ROOT_DIR}/src")
endif()
