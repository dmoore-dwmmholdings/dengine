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
