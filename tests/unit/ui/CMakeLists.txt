if(TARGET de_ui)
 de_add_test(NAME ui_layout SOURCES test_flex.c test_grid.c test_anchor_popup.c test_ui_scale.c test_layout_tree.c test_stylesheet.c test_style_resolve.c test_tween.c test_widgets_basic.c test_pointer.c test_text_input.c test_virtual_list.c test_scroll_view.c test_dropdown_tabs_tooltip.c LIBS de_ui de_platform de_ecs de_reflect de_tp_tomlc17)
 if(TARGET test_ui_layout)
  target_include_directories(test_ui_layout PRIVATE "${DE_ROOT_DIR}/src")
 endif()
endif()

if(TARGET test_ui_layout AND DE_BUILD_TOOLS)
 target_link_libraries(test_ui_layout PRIVATE de_cooker de_pack_lib de_tp_stb de_tp_harfbuzz)
 file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/text-packed-tmp")
 target_compile_definitions(test_ui_layout PRIVATE
  "DE_TEXT_PACK_TMP=\"${CMAKE_CURRENT_BINARY_DIR}/text-packed-tmp\"")
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
if(TARGET test_ui_draw AND TARGET de_gpu_metal)
 target_link_libraries(test_ui_draw PRIVATE de_gpu_metal "-framework Metal" "-framework Foundation")
endif()
if(TARGET test_ui_draw AND DE_BUILD_TOOLS AND DE_HAS_RENDER)
 target_compile_definitions(test_ui_draw PRIVATE "DE_UI_DRAW_ASSET_ROOT=\"${DE_ROOT_DIR}\"")
endif()

# DENG-322 real world-root layout/ray routing and integer internal UI geometry.
if(TARGET de_ui)
 de_add_test(NAME ui_world_pixel SOURCES test_world_panel.c test_pixel_ui.c
  LIBS de_ui de_platform de_ecs de_reflect LABELS ui)
 target_include_directories(test_ui_world_pixel PRIVATE "${DE_ROOT_DIR}/src")
 if(TARGET de_gpu_metal)
  target_link_libraries(test_ui_world_pixel PRIVATE de_gpu_metal "-framework Metal" "-framework Foundation")
 endif()
endif()

# DENG-320 resolved actions, spatial graph and actual S21 .deir menu replay.
if(TARGET de_ui)
 de_add_test(NAME ui_focus SOURCES test_focus.c test_spatial_nav.c test_menu_replay.c LIBS de_ui de_input de_platform de_ecs de_reflect de_test_support LABELS ui)
 target_include_directories(test_ui_focus PRIVATE "${DE_ROOT_DIR}/src" "${DE_ROOT_DIR}/tests/unit/input")
 file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/menu_replay")
 target_compile_definitions(test_ui_focus PRIVATE "DE_UI_REPLAY_DIR=\"${CMAKE_CURRENT_BINARY_DIR}/menu_replay\"")
endif()

# DENG-321 reflection paths, generic CHANGED writeback and idle HUD costs.
if(TARGET de_ui)
 de_add_test(NAME ui_binding SOURCES test_binding.c LIBS de_ui de_platform de_ecs de_reflect de_test_support de_tp_tomlc17 LABELS ui)
 target_include_directories(test_ui_binding PRIVATE "${DE_ROOT_DIR}/src")
endif()

if(TARGET test_ui_focus)
 target_sources(test_ui_focus PRIVATE test_terminal.c "${DE_ROOT_DIR}/examples/terminal_demo/session.c")
 if(WIN32)
  target_compile_definitions(test_ui_focus PRIVATE _CRT_SECURE_NO_WARNINGS)
 endif()
endif()

# Cook fixture packs in test executables; runtime libraries stay cooker-free.
if(DE_BUILD_TOOLS)
 foreach(target test_ui_draw test_ui_binding)
  if(TARGET ${target})
   target_link_libraries(${target} PRIVATE de_cooker de_pack_lib)
   target_include_directories(${target} PRIVATE "${DE_ROOT_DIR}/tools")
   file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${target}-packed")
   target_compile_definitions(${target} PRIVATE DE_UI_TEST_PACKED_ASSETS=1
    "DE_TEXT_PACK_TMP=\"${CMAKE_CURRENT_BINARY_DIR}/${target}-packed\"")
  endif()
 endforeach()
endif()
