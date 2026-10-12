# tests/unit/cook: tests of the de-cook tool (tools/cook/). DENG-145: `de-cook --check`. CTest
# names cook_check.<test> (in-process, labels unit;cook) and cook_cli.<case> (the real
# executable, through run_cli.cmake). de-cook is a tool, so the tests exist only when tools are
# built. tests/ is added before tools/, so test for the option, not for the target.
if(NOT DE_BUILD_TOOLS)
    return()
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
