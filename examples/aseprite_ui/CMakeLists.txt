if(NOT TARGET de_ui OR NOT TARGET de_render OR NOT TARGET de_cooker)
 return()
endif()
# An executable run target works both in DIDE and from the command line.
set(DE_GAME_DYLIB FALSE)
set(_sprite_ui_config game.toml)
if(APPLE)
 set(_sprite_ui_config game.macos.toml)
endif()
de_add_game(NAME aseprite_ui SOURCES main.c capture.c GAME_TOML "${_sprite_ui_config}")
target_link_libraries(aseprite_ui PRIVATE de_build_flags)
target_link_libraries(aseprite_ui_static PRIVATE de_build_flags)
target_compile_definitions(aseprite_ui_static PRIVATE
 "ASEPRITE_UI_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}\"")
