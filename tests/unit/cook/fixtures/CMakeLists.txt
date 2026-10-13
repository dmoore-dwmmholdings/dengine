# tests/unit/cook/fixtures: game libraries for the `de-cook cook --game` tests (DENG-394), all
# from cook_game.c.
#
#   cook_game               de_add_game(), only where it builds libraries (DE_GAME_DYLIB: not
#                           Shipping, not Windows without DE_BUILD_SHARED). Registers the fixture
#                           project's types; calls the engine.
#   cook_game_plain         valid, registers nothing, calls nothing in the engine  } plain MODULE
#   cook_game_no_symbol     no de_game_get_api export                              } libraries in
#                                                                                    <binary>/plain
# The plain libraries need nothing from the engine, so they load into any process, also on
# Windows, where neither de-cook nor the test executable exports the engine.
if(DE_GAME_DYLIB)
    de_add_game(NAME cook_game SOURCES cook_game.c)
    target_link_libraries(cook_game PRIVATE de_build_flags)
    target_include_directories(cook_game PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
endif()

foreach(_de_variant PLAIN NO_SYMBOL)
    string(TOLOWER "cook_game_${_de_variant}" _de_fixture)
    add_library(${_de_fixture} MODULE cook_game.c)
    target_compile_definitions(${_de_fixture} PRIVATE COOK_GAME_${_de_variant})
    target_link_libraries(${_de_fixture} PRIVATE de_config de_build_flags)
    # The platform file name and hidden visibility, like a game library (see
    # tests/unit/host/fixtures/CMakeLists.txt); no export list, so the no-symbol variant links.
    set_target_properties(${_de_fixture} PROPERTIES
        PREFIX "${CMAKE_SHARED_LIBRARY_PREFIX}"
        SUFFIX "${CMAKE_SHARED_LIBRARY_SUFFIX}"
        LIBRARY_OUTPUT_DIRECTORY "$<1:${CMAKE_CURRENT_BINARY_DIR}/plain>"
        C_VISIBILITY_PRESET hidden
        FOLDER "dengine/tests")
endforeach()
