# Retained CPU layout remains usable in headless configurations.
de_add_module(NAME ui LAYER gameplay
 SOURCES ui.c ui_reflect.c layout/layout.c layout/flex.c layout/grid.c layout/anchor.c layout/measure.c style/stylesheet.c style/style_resolve.c style/tween.c style/theme.c
 DEPS text input OPTIONAL_OPTION DE_WITH_UI)
if(TARGET de_ui)
 target_link_libraries(de_ui PRIVATE de_tp_xxhash de_tp_tomlc17)
 set_property(TARGET de_ui APPEND PROPERTY DE_MODULE_LIBS de_tp_xxhash de_tp_tomlc17)
 if(DE_HAS_RENDER)
  target_link_libraries(de_ui PRIVATE de_render)
 endif()
endif()

# DENG-317 retained draw generation remains usable without a GPU/renderer.
if(TARGET de_ui)
 target_sources(de_ui PRIVATE render/draw_list.c render/ui_extract.c render/ui_pass.c)
 target_compile_definitions(de_ui PRIVATE DE_UI_RETAINED_RENDER=1)
endif()

# DENG-318: embed the real TOML sheet, keeping source edits tracked by the build.
if(TARGET de_ui)
 set(_ui_default_style "${CMAKE_CURRENT_BINARY_DIR}/ui_default_style.h")
 add_custom_command(OUTPUT "${_ui_default_style}"
  COMMAND "${CMAKE_COMMAND}" "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/widgets/default_style.toml" "-DOUTPUT=${_ui_default_style}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_style.cmake"
  DEPENDS widgets/default_style.toml cmake/embed_style.cmake VERBATIM)
 target_sources(de_ui PRIVATE "${_ui_default_style}" widgets/text.c widgets/image.c widgets/button.c widgets/toggle.c widgets/slider.c widgets/progress.c input/pointer.c)
 target_include_directories(de_ui PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
endif()

# DENG-320: replay-aware keyboard/gamepad focus navigation.
if(TARGET de_ui)
 target_sources(de_ui PRIVATE input/focus.c input/spatial_nav.c input/ui_actions.c)
 target_compile_definitions(de_ui PRIVATE DE_UI_NAVIGATION=1)
endif()
