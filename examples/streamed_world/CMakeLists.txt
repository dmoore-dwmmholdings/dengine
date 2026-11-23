if(NOT TARGET de_render OR NOT TARGET de_cooker)
 return()
endif()
# DENG-836: an example game with a small streamed world, the target of DIDE's real-engine
# world-streaming e2e (DIDE-401). The game only loads the manifest; the editor session's
# world.streaming service (DENG-831) opens the world and moves the sources.
# DIDE resolves run targets to executables on every platform, so keep it static like flappy.
set(DE_GAME_DYLIB FALSE)
set(_sw_game_toml game.toml)
if(APPLE)
 set(_sw_game_toml game.macos.toml)
endif()
de_add_game(NAME streamed_world SOURCES main.c GAME_TOML "${_sw_game_toml}")
target_link_libraries(streamed_world PRIVATE de_build_flags)
target_compile_definitions(streamed_world PRIVATE "STREAMED_WORLD_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}\"")
if(TARGET streamed_world_static)
 target_link_libraries(streamed_world_static PRIVATE de_build_flags)
 target_compile_definitions(streamed_world_static PRIVATE
  "STREAMED_WORLD_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}\"")
endif()

# The committed cells must match a fresh `de-cook world` of the sources.
if(DE_BUILD_TESTS AND TARGET de-cook AND NOT DE_IS_SHIPPING)
 add_test(NAME example_streamed_world_cook
  COMMAND ${CMAKE_COMMAND} -DDE_COOK=$<TARGET_FILE:de-cook> -DPROJECT=${CMAKE_CURRENT_SOURCE_DIR}/project
          -DOUT=${CMAKE_CURRENT_BINARY_DIR}/cook_check
          -P ${CMAKE_CURRENT_SOURCE_DIR}/check_cook.cmake)
 set_tests_properties(example_streamed_world_cook PROPERTIES LABELS "example;streaming")
endif()
