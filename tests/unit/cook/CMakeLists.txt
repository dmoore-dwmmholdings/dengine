# tests/unit/cook: tests of the de-cook tool (tools/cook/). DENG-145: `de-cook --check`. CTest
# names cook_check.<test> (in-process, labels unit;cook) and cook_cli.<case> (the real
# executable, through run_cli.cmake). de-cook is a tool, so the tests exist only when tools are
# built. tests/ is added before tools/, so test for the option, not for the target.
if(NOT DE_BUILD_TOOLS)
    return()
endif()

de_add_test(NAME shader_cli SOURCES test_shader_cli.c LIBS de_cook_shader_cmd)
if(DE_BUILD_TESTS AND NOT (CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR))
    add_test(NAME shader_cli.outputs COMMAND "${CMAKE_COMMAND}"
        "-DDE_CLI=$<TARGET_FILE:de-cook>" "-DDE_ROOT=${DE_ROOT_DIR}"
        "-DDE_TMP=${CMAKE_CURRENT_BINARY_DIR}/shader_cli_tmp"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/shader_cli_outputs.cmake")
    set_tests_properties(shader_cli.outputs PROPERTIES LABELS "unit;cook;shader" TIMEOUT 180)
endif()

de_add_test(NAME cook_check SOURCES test_check.c LIBS de_cook_check de_platform)
if(TARGET test_cook_check)
    target_compile_definitions(test_cook_check PRIVATE
        "DE_COOK_TEST_DATA_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/data\""
        "DE_COOK_EXAMPLES_DIR=\"${DE_ROOT_DIR}/examples\"")
endif()

# The executable itself: exit codes and output of the shipped command line. Skipped when the
# build host cannot run what it builds.
if(DE_BUILD_TESTS AND NOT (CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR))
    # de__cook_cli_test(<name> <exit code> <regex the output must match> <arguments>...)
    function(de__cook_cli_test name rc regex)
        string(JOIN "|" args ${ARGN})
        add_test(NAME cook_cli.${name}
            COMMAND "${CMAKE_COMMAND}"
                "-DEXE=$<TARGET_FILE:de-cook>"
                "-DARGS=${args}"
                "-DEXPECT_RC=${rc}"
                "-DEXPECT_REGEX=${regex}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cli.cmake"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data")
        set_tests_properties(cook_cli.${name} PROPERTIES LABELS "unit;cook" TIMEOUT 60)
    endfunction()
    de__cook_cli_test(clean_example 0 "^de-cook --check: 0 errors, 0 warnings, 0 notes"
        --check "${DE_ROOT_DIR}/examples/assets.toml" --no-sources -I example_includes)
    de__cook_cli_test(four_issues 1 "^four_issues/assets.toml:8: error: .*3 errors, 1 warning"
        --check four_issues/assets.toml)
    de__cook_cli_test(no_arguments 2 "usage: de-cook <command>")
    de__cook_cli_test(unknown_command 2 "unknown command '--cook-everything'"
        --cook-everything)
    de__cook_cli_test(bad_option 2 "unknown option '--bogus'.*usage: de-cook --check"
        --check four_issues/assets.toml --bogus)
endif()

# DENG-209: the cooker library (tools/cook/lib/, de_cooker): registry, cooking, sub-assets,
# dependencies, the determinism check, the settings hash, the import context, and the runtime
# pipeline cooking in-process (cooker.runtime_source_mode, DE_HAS_COOKER builds; skipped in the
# others). Sources are written into a scratch directory of the build tree (mounted as ckt:/).
de_add_test(NAME cooker SOURCES test_cooker.c test_settings_hash.c test_import_ctx.c
            LIBS de_cooker de_asset de_task de_platform)
if(TARGET test_cooker)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_cooker_tmp")
    target_include_directories(test_cooker PRIVATE "${DE_ROOT_DIR}/src" "${DE_ROOT_DIR}/tools")
    target_compile_definitions(test_cooker PRIVATE
        "DE_COOK_TEST_TMP_DIR=\"${CMAKE_CURRENT_BINARY_DIR}/test_cooker_tmp\"")
endif()

# DENG-209: cooker_link_check: builds that do not cook in-process (Release, Shipping) link no
# cooker symbol, and Debug/Dev builds do (the positive control). nm reads the aggregate `dengine`
# library and cooker_link_probe, an executable that links `dengine` the way a game does and
# creates an asset system (which installs the load pipeline, and with it the cook stage). See
# check_cooker_link.cmake.
if(DE_BUILD_TESTS)
    add_executable(cooker_link_probe cooker_link_probe.c)
    target_link_libraries(cooker_link_probe PRIVATE dengine de_build_flags)
    set_target_properties(cooker_link_probe PROPERTIES FOLDER "dengine/tests")
    if(CMAKE_NM)
        set(_de_cook_nm "${CMAKE_NM}")
    else()
        find_program(_de_cook_nm NAMES llvm-nm nm)
    endif()
    add_test(NAME cooker_link_check
        COMMAND "${CMAKE_COMMAND}"
            "-DDE_NM=${_de_cook_nm}"
            "-DDE_EXPECT_COOKER=$<IF:$<AND:$<CONFIG:Debug,Dev>,$<TARGET_EXISTS:de_cooker>>,1,0>"
            "-DDE_FILES=$<TARGET_FILE:dengine>|$<TARGET_FILE:cooker_link_probe>"
            "-DDE_CONFIG=$<CONFIG>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_cooker_link.cmake")
    set_tests_properties(cooker_link_check PROPERTIES
        LABELS "unit;cook"
        SKIP_REGULAR_EXPRESSION "cooker_link_check: SKIPPED"
        TIMEOUT 60)
endif()

# DENG-210: the cook cache `.decache` (tools/cook/lib/decache*.c): hits skip the importer, every
# key input misses, the warm path reads no source byte, touched files, damaged entries, and two
# cookers storing the same key at once. Each test empties its own directory below ckt:/.
de_add_test(NAME decache SOURCES test_decache.c LIBS de_cooker de_asset de_task de_platform)
if(TARGET test_decache)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_decache_tmp")
    target_include_directories(test_decache PRIVATE "${DE_ROOT_DIR}/src" "${DE_ROOT_DIR}/tools")
    target_compile_definitions(test_decache PRIVATE
        "DE_COOK_TEST_TMP_DIR=\"${CMAKE_CURRENT_BINARY_DIR}/test_decache_tmp\"")
endif()

# DENG-213: `de-cook cook` (tools/cook/cook_cmd.c) in-process on the fixture project
# tests/data/cook_project/ (identical runs, validation abort, stripped names, usage, the packed
# host's game:/ packs through host/host_packs.h). cook_project_util.h is shared with
# tests/unit/pack/test_pack_cli.c. Outputs go to a scratch directory of the build tree.
de_add_test(NAME cook_cli SOURCES test_cook_cli.c
            LIBS de_cook_cmd de_cooker de_host de_asset de_task de_platform TIMEOUT 300)
if(TARGET test_cook_cli)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_cook_cli_tmp")
    target_include_directories(test_cook_cli PRIVATE "${DE_ROOT_DIR}/src" "${DE_ROOT_DIR}/tools")
    target_compile_definitions(test_cook_cli PRIVATE
        "DE_COOK_CLI_TMP_DIR=\"${CMAKE_CURRENT_BINARY_DIR}/test_cook_cli_tmp\""
        "DE_COOK_PROJECT_DIR=\"${DE_ROOT_DIR}/tests/data/cook_project\""
        "DE_COOK_TEST_DATA_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/data\"")
endif()

# DENG-213: the real executables end to end: de-cook cooks the engine-types part of the fixture
# project (engine.toml) into the build tree, then de-pack verifies, lists and extracts the pack
# (tests/unit/pack/CMakeLists.txt, fixture cook_cli_engine_pack). The cache goes to the build
# tree too, never next to the fixture. Cooking needs the runtime importers (DE_HAS_COOKER), so
# those cases exist in Debug and Dev builds only; elsewhere `de-cook cook` refuses (exit 2).
if(DE_BUILD_TESTS AND NOT (CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR))
    de__cook_cli_test(cook_usage 2 "no --platform given.*usage: de-cook cook"
        cook four_issues/assets.toml --out "${CMAKE_BINARY_DIR}/tests/cook_cli_out/abort")
    if(CMAKE_BUILD_TYPE MATCHES "^(Debug|Dev)$")
        set(_de_cook_cli_out "${CMAKE_BINARY_DIR}/tests/cook_cli_out")
        de__cook_cli_test(engine_project 0
            "de-cook cook: .*engine.dpak \\(linux\\).*assets cooked: 4 \\(\\+0 sub-assets\\), bundles: 2.*\\[4/4\\]"
            cook "${DE_ROOT_DIR}/tests/data/cook_project/engine.toml" --platform linux
            --out "${_de_cook_cli_out}" --cache "${_de_cook_cli_out}/cache" --pack-name engine
            --jobs 2)
        set_tests_properties(cook_cli.engine_project PROPERTIES
            FIXTURES_SETUP cook_cli_engine_pack)
        de__cook_cli_test(cook_validation_abort 1
            "four_issues/assets.toml:8: error: .*nothing was cooked"
            cook four_issues/assets.toml --platform macos --out "${_de_cook_cli_out}/abort"
            --no-cache)
    endif()
endif()

# DENG-394: `de-cook cook --game <lib>`. fixtures/ builds the game library cook_game (de_add_game,
# where it builds libraries), which registers the fixture project's types, and the plain
# libraries cook_game_plain and cook_game_no_symbol (every build). test_cook_game runs the
# command in-process without hooks. Where cook_game exists with a static engine, the test links
# the whole engine and exports it, as de-cook does (tools/cook/CMakeLists.txt), so the library
# binds to it; the whole archive comes before the module libraries on the link line. Elsewhere
# (Windows without DE_BUILD_SHARED, Shipping, DE_BUILD_SHARED) only the plain-library cases run.
add_subdirectory(fixtures)
set(_de_cook_game_exports FALSE)
if(DE_GAME_DYLIB AND NOT DE_BUILD_SHARED)
    set(_de_cook_game_exports TRUE)
    de_add_test(NAME cook_game SOURCES test_cook_game.c
                LIBS "$<LINK_LIBRARY:WHOLE_ARCHIVE,dengine>" de_cook_cmd de_cooker TIMEOUT 120)
else()
    de_add_test(NAME cook_game SOURCES test_cook_game.c
                LIBS de_cook_cmd de_cooker de_host de_asset de_task de_platform TIMEOUT 120)
endif()
if(TARGET test_cook_game)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_cook_game_tmp")
    target_include_directories(test_cook_game PRIVATE "${DE_ROOT_DIR}/src" "${DE_ROOT_DIR}/tools")
    target_compile_definitions(test_cook_game PRIVATE
        "DE_COOK_CLI_TMP_DIR=\"${CMAKE_CURRENT_BINARY_DIR}/test_cook_game_tmp\""
        "DE_COOK_PROJECT_DIR=\"${DE_ROOT_DIR}/tests/data/cook_project\""
        "COOK_GAME_PLAIN_LIB=\"$<TARGET_FILE:cook_game_plain>\""
        "COOK_GAME_NO_SYMBOL_LIB=\"$<TARGET_FILE:cook_game_no_symbol>\"")
    add_dependencies(test_cook_game cook_game_plain cook_game_no_symbol)
    if(_de_cook_game_exports)
        target_compile_definitions(test_cook_game PRIVATE
            "COOK_GAME_LIB=\"$<TARGET_FILE:cook_game>\"")
        add_dependencies(test_cook_game cook_game)
        set_target_properties(test_cook_game PROPERTIES ENABLE_EXPORTS ON)
        if(APPLE)
            target_link_options(test_cook_game PRIVATE "LINKER:-export_dynamic")
        endif()
    endif()
endif()

# DENG-394: the real executable with --game: the full fixture project (which needs the game's
# types) cooks with cook_game where it exists; an unloadable library exits 2 on every platform.
if(DE_BUILD_TESTS AND NOT (CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
   AND CMAKE_BUILD_TYPE MATCHES "^(Debug|Dev)$")
    set(_de_cook_game_out "${CMAKE_BINARY_DIR}/tests/cook_cli_out/game")
    if(_de_cook_game_exports)
        de__cook_cli_test(game_project 0
            "de-cook cook: .*game.dpak \\(linux\\).*assets cooked: 7 \\(\\+2 sub-assets\\), bundles: 2.*game: cook_game \\(1 asset types added\\)"
            cook "${DE_ROOT_DIR}/tests/data/cook_project/assets.toml" --platform linux
            --out "${_de_cook_game_out}" --no-cache --jobs 2 --game "$<TARGET_FILE:cook_game>")
    endif()
    de__cook_cli_test(game_no_symbol 2
        "cannot load the game library .*does not export de_game_get_api"
        cook "${DE_ROOT_DIR}/tests/data/cook_project/engine.toml" --platform linux
        --out "${_de_cook_game_out}/nosym" --no-cache
        --game "$<TARGET_FILE:cook_game_no_symbol>")
endif()

# DENG-409: the cooker's built-in importers (de_cooker_builtin_importer: texture, model, shader)
# and `de-cook cook` on the engine-types-only project data/builtin_project/ (two `.slang` shaders,
# a PNG, a glTF), in-process without hooks (test_cook_builtin.c, CTest names
# cook_builtin.<test>), and the real executable (cook_cli.builtin_project). The shader checks read
# the cooked format through de_gpu_shader (de__shader_view_init) where a GPU module is built.
de_add_test(NAME cook_builtin SOURCES test_cook_builtin.c
            LIBS de_cook_cmd de_cooker de_host de_asset de_task de_platform TIMEOUT 300)
if(TARGET test_cook_builtin)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_cook_builtin_tmp")
    target_include_directories(test_cook_builtin PRIVATE "${DE_ROOT_DIR}/src" "${DE_ROOT_DIR}/tools")
    target_compile_definitions(test_cook_builtin PRIVATE
        "DE_COOK_CLI_TMP_DIR=\"${CMAKE_CURRENT_BINARY_DIR}/test_cook_builtin_tmp\""
        "DE_COOK_PROJECT_DIR=\"${DE_ROOT_DIR}/tests/data/cook_project\""
        "DE_COOK_BUILTIN_PROJECT=\"${CMAKE_CURRENT_SOURCE_DIR}/data/builtin_project\"")
    if(TARGET de_gpu_shader)
        target_link_libraries(test_cook_builtin PRIVATE de_gpu_shader)
        target_compile_definitions(test_cook_builtin PRIVATE DE_COOK_TEST_SHADER_VIEW=1)
    endif()
endif()
if(DE_BUILD_TESTS AND NOT (CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
   AND CMAKE_BUILD_TYPE MATCHES "^(Debug|Dev)$")
    set(_de_cook_builtin_out "${CMAKE_BINARY_DIR}/tests/cook_cli_out/builtin")
    de__cook_cli_test(builtin_project 0
        "de-cook cook: .*builtin.dpak \\(linux\\).*assets cooked: 4 \\(\\+3 sub-assets\\), bundles: 1"
        cook builtin_project/assets.toml --platform linux --out "${_de_cook_builtin_out}"
        --cache "${_de_cook_builtin_out}/cache" --pack-name builtin --jobs 2)
    set_tests_properties(cook_cli.builtin_project PROPERTIES TIMEOUT 300)
endif()
