if(NOT TARGET de_cooker)
    return()
endif()
set(DE_GAME_DYLIB FALSE)
de_add_game(NAME font_atlas_demo SOURCES main.c GAME_TOML game.toml)
target_link_libraries(font_atlas_demo PRIVATE de_build_flags)
target_link_libraries(font_atlas_demo_static PRIVATE de_build_flags)
target_compile_definitions(font_atlas_demo_static PRIVATE
    "FONT_ATLAS_DEMO_ROOT=\"${DE_ROOT_DIR}\"")
