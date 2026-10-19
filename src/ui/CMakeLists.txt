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
