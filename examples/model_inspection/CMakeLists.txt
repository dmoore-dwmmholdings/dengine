if(NOT TARGET de_cooker OR NOT DE_HAS_RENDER)
    return()
endif()
set(DE_GAME_DYLIB FALSE)
de_add_game(NAME model_inspection SOURCES main.c GAME_TOML game.toml)
target_link_libraries(model_inspection PRIVATE de_build_flags)
target_link_libraries(model_inspection_static PRIVATE de_build_flags)
target_compile_definitions(model_inspection_static PRIVATE
    "MODEL_INSPECTION_ROOT=\"${DE_ROOT_DIR}\"")
