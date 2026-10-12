# tests/unit/host/fixtures: game libraries for the DENG-92 host tests, all from test_game.c.
#
#   test_game                   de_add_game(): a library in Debug, Dev and Release (DE_GAME_DYLIB),
#                               a static executable in Shipping. Calls the engine.
#   test_game_standalone        valid, calls nothing in the engine     } plain MODULE libraries in
#   test_game_bad_version       api_version + 1                        } <binary dir>/plain, used
#   test_game_no_callback       register_systems is NULL               } in process by
#   test_game_no_symbol         no de_game_get_api export              } test_game_module.c and
#                                                                        by the dengine runs.
# The plain libraries need nothing from the engine, so they load into any process, also the
# test executable (which does not export the engine) and the Windows static host.
de_add_game(NAME test_game SOURCES test_game.c GAME_TOML test_game.toml)
target_link_libraries(test_game PRIVATE de_build_flags)
if(TARGET test_game_static)
    target_link_libraries(test_game_static PRIVATE de_build_flags)
endif()

set(DE_HOST_PLAIN_GAME_DIR "${CMAKE_CURRENT_BINARY_DIR}/plain" PARENT_SCOPE)
foreach(_de_variant STANDALONE BAD_VERSION NO_CALLBACK NO_SYMBOL)
    string(TOLOWER "test_game_${_de_variant}" _de_fixture)
    add_library(${_de_fixture} MODULE test_game.c)
    target_compile_definitions(${_de_fixture} PRIVATE TEST_GAME_${_de_variant})
    target_link_libraries(${_de_fixture} PRIVATE de_config de_build_flags)
    # The platform file name (de_dylib_platform_name) and hidden visibility, like a game
    # library; no export list, so the no-symbol variant links.
    set_target_properties(${_de_fixture} PROPERTIES
        PREFIX "${CMAKE_SHARED_LIBRARY_PREFIX}"
        SUFFIX "${CMAKE_SHARED_LIBRARY_SUFFIX}"
        LIBRARY_OUTPUT_DIRECTORY "$<1:${CMAKE_CURRENT_BINARY_DIR}/plain>"
        C_VISIBILITY_PRESET hidden
        FOLDER "dengine/tests")
endforeach()
