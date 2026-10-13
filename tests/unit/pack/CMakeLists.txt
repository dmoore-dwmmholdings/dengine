# Unit tests of the .dpak pack writer (tools/pack/lib, DENG-211). CTest names dpak_writer.<test>.
# The writer is a tool library, so the tests exist only when tools are built. tests/ is added
# before tools/, so test for the option, not for the target.
if(NOT DE_BUILD_TOOLS)
    return()
endif()

de_add_test(NAME dpak_writer SOURCES test_dpak_writer.c LIBS de_pack_lib de_platform de_tp_zstd)
if(TARGET test_dpak_writer)
    # dpak_format.h ("asset/dpak/dpak_format.h") and the VFS test hook (platform/vfs_internal.h).
    target_include_directories(test_dpak_writer PRIVATE "${DE_ROOT_DIR}/src")
    target_compile_definitions(test_dpak_writer PRIVATE
        DE_PACK_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/test_dpak_writer_tmp")
endif()

# DENG-213: the de-pack CLI (tools/pack/pack_cmds.c) in-process: verify, a damaged pack, list
# --json, extract against the cooker's own output. Each test first cooks the fixture project
# tests/data/cook_project/ with `de-cook cook` (tools/cook/cook_cmd.c) into a scratch directory.
de_add_test(NAME pack_cli SOURCES test_pack_cli.c
            LIBS de_pack_cmds de_cook_cmd de_cooker de_pack_lib de_asset de_ecs de_task de_platform
            TIMEOUT 300)
if(TARGET test_pack_cli)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_pack_cli_tmp")
    target_include_directories(test_pack_cli PRIVATE "${DE_ROOT_DIR}/src" "${DE_ROOT_DIR}/tools")
    target_compile_definitions(test_pack_cli PRIVATE
        "DE_COOK_CLI_TMP_DIR=\"${CMAKE_CURRENT_BINARY_DIR}/test_pack_cli_tmp\""
        "DE_COOK_PROJECT_DIR=\"${DE_ROOT_DIR}/tests/data/cook_project\"")
endif()

# DENG-213: the real de-pack executable on the pack the real de-cook wrote in the CTest case
# cook_cli.engine_project (tests/unit/cook/CMakeLists.txt, fixture cook_cli_engine_pack; Debug
# and Dev builds only).
if(DE_BUILD_TESTS AND NOT (CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR))
    set(_de_pack_cli_pak "${CMAKE_BINARY_DIR}/tests/cook_cli_out/engine.dpak")
    # de__pack_cli_test(<name> <exit code> <regex> <arguments>...), through the de-cook tests'
    # run_cli.cmake (it runs any executable).
    function(de__pack_cli_test name rc regex)
        string(JOIN "|" args ${ARGN})
        add_test(NAME pack_cli.${name}
            COMMAND "${CMAKE_COMMAND}"
                "-DEXE=$<TARGET_FILE:de-pack>"
                "-DARGS=${args}"
                "-DEXPECT_RC=${rc}"
                "-DEXPECT_REGEX=${regex}"
                -P "${DE_ROOT_DIR}/tests/unit/cook/run_cli.cmake")
        set_tests_properties(pack_cli.${name} PROPERTIES LABELS "unit;pack" TIMEOUT 60)
    endfunction()
    de__pack_cli_test(exe_usage 2 "usage: de-pack")
    # The pack comes from `de-cook cook`, which needs DE_HAS_COOKER (Debug and Dev).
    if(CMAKE_BUILD_TYPE MATCHES "^(Debug|Dev)$")
        de__pack_cli_test(exe_verify 0 "engine.dpak: OK \\(platform linux, 4 entries"
            verify "${_de_pack_cli_pak}")
        de__pack_cli_test(exe_list 0 "4 entries, 2 bundles.*levels/demo.*bundle core"
            list "${_de_pack_cli_pak}")
        de__pack_cli_test(exe_extract 0 "props/barrel  728 bytes"
            extract "${_de_pack_cli_pak}" "props/*" -o "${CMAKE_BINARY_DIR}/tests/cook_cli_out/x")
        set_tests_properties(pack_cli.exe_verify pack_cli.exe_list pack_cli.exe_extract
            PROPERTIES FIXTURES_REQUIRED cook_cli_engine_pack)
    endif()
endif()
