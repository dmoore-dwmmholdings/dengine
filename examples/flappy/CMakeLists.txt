if(NOT TARGET de_ui OR NOT TARGET de_render OR NOT TARGET de_cooker)
 return()
endif()
de_add_game(NAME flappy SOURCES main.c GAME_TOML game.toml)
target_link_libraries(flappy PRIVATE de_build_flags)
target_compile_definitions(flappy PRIVATE "FLAPPY_ROOT=\"${DE_ROOT_DIR}\"")
if(TARGET flappy_static)
 target_link_libraries(flappy_static PRIVATE de_build_flags)
 target_compile_definitions(flappy_static PRIVATE "FLAPPY_ROOT=\"${DE_ROOT_DIR}\"")
endif()

# flappy_pack: the assets cooked into pack/flappy.dpak, which Release builds load (they have no
# cooker). Cooking needs the importers, so only a Debug or Dev build can make it; DIDE's Publish
# builds it there and ships it beside the Release executable (Flappy.dproj, [publish.itch]).
# The cook needs Flappy's component types (flappy.scene): flappy-cook links the static game,
# elsewhere de-cook loads the game library.
if(NOT DE_MULTI_CONFIG AND CMAKE_BUILD_TYPE MATCHES "^(Debug|Dev)$" AND TARGET de_cook_cmd)
 set(_pack_dir "${CMAKE_CURRENT_BINARY_DIR}/pack")
 if(TARGET flappy_static)
  add_executable(flappy-cook cook.c)
  target_link_libraries(flappy-cook PRIVATE flappy_static de_cook_cmd de_build_flags)
  set(_cook $<TARGET_FILE:flappy-cook>)
  set(_cook_target flappy-cook)
  set(_game)
 else()
  set(_cook $<TARGET_FILE:de-cook> cook)
  set(_cook_target de-cook flappy)
  set(_game --game $<TARGET_FILE:flappy>)
 endif()
 if(WIN32)
  set(_platform windows)
 elseif(APPLE)
  set(_platform macos)
 else()
  set(_platform linux)
 endif()
 file(GLOB _art CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/art/*.png")
 add_custom_command(
  OUTPUT "${_pack_dir}/flappy.dpak"
  COMMAND ${_cook} "${CMAKE_CURRENT_SOURCE_DIR}/assets.toml" --platform ${_platform}
          --out "${_pack_dir}" --pack-name flappy --cache "${_pack_dir}/.decache" --quiet ${_game}
  DEPENDS assets.toml flappy.scene ${_art} fonts/NotoSans.ttf
          ${_cook_target}
  COMMENT "Cooking Flappy's assets into flappy.dpak"
  VERBATIM)
 add_custom_target(flappy_pack DEPENDS "${_pack_dir}/flappy.dpak")
endif()
