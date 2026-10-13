# tests/determinism/cook: deterministic cooking (SPEC §11.5; DENG-216). `de-cook cook` of the
# fixture project tests/data/cook_project/ (in-process, de_cook_cook_run, with the fixture types
# of tests/unit/cook/cook_project_util.h) must give byte-identical packs and `.decache` entries:
#
#   det.cook.identical_cold_warm      cold vs warm cache; writes <build>/determinism/
#                                     cook_project.linux.{dpak,decache} for the CI cross-host diff
#   det.cook.identical_jobs           --jobs 1 vs --jobs 8 (cold), and a warm --jobs 8
#   det.cook.identical_relocated      a copy of the project at another absolute path
#   det.cook.report_first_difference  the mismatch reports (asset ID and offsets) on planted
#                                     differences
#
# Cooking needs the runtime importers (DE_HAS_COOKER: Debug and Dev); the tests skip in Release,
# so the Release jobs of determinism.yml skip them and its determinism-cook job runs them in Dev.
# Scratch files go to the build tree.
if(NOT DE_BUILD_TOOLS)
    return()
endif()
add_executable(test_det_cook test_cook_identical.c)
target_link_libraries(test_det_cook PRIVATE de_cook_cmd de_cooker de_asset de_task de_platform
                      de_tp_xxhash de_test_framework de_build_flags)
# src/ for the internal pack reader, tools/ and tests/unit/cook/ for the fixture helpers.
target_include_directories(test_det_cook PRIVATE "${DE_ROOT_DIR}/src" "${DE_ROOT_DIR}/tools"
                           "${DE_ROOT_DIR}/tests/unit/cook")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_det_cook_tmp")
target_compile_definitions(test_det_cook PRIVATE
    "DE_COOK_CLI_TMP_DIR=\"${CMAKE_CURRENT_BINARY_DIR}/test_det_cook_tmp\""
    "DE_COOK_PROJECT_DIR=\"${DE_ROOT_DIR}/tests/data/cook_project\""
    "DE_DET_OUT_DIR=\"${DE_DET_OUT_DIR}\"")
set_target_properties(test_det_cook PROPERTIES FOLDER "dengine/tests")

de_det_add_tests(TARGET test_det_cook TESTS
    det.cook.identical_cold_warm=cook.identical_cold_warm
    det.cook.identical_jobs=cook.identical_jobs
    det.cook.identical_relocated=cook.identical_relocated
    det.cook.report_first_difference=cook.report_first_difference)
