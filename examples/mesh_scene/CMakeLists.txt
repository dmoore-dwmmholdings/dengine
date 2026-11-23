if(NOT TARGET de_cooker OR NOT DE_HAS_RENDER)
    return()
endif()
set(DE_GAME_DYLIB FALSE)
de_add_game(NAME mesh_scene SOURCES main.c GAME_TOML game.toml)
target_link_libraries(mesh_scene PRIVATE de_build_flags)
target_link_libraries(mesh_scene_static PRIVATE de_build_flags)
target_compile_definitions(mesh_scene_static PRIVATE
    "MESH_SCENE_ROOT=\"${DE_ROOT_DIR}\"")
