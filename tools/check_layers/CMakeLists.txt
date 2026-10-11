# tools/check_layers: include-graph layering check (SPEC §4.1, §4.2, §19.3).
#
#   layering_check     check_layers.py on the whole repository (src/, tools/cook/lib/,
#                      include/dengine/) against layers.toml.
#   layering_selftest  the checker on the fixture trees in tests/fixtures/.
#
# Both are plain Python (stdlib only, 3.10+) and carry the ctest label `check`.
# REQUIRED: the check must not disappear silently on a machine without Python.
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

add_test(NAME layering_check
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/check_layers.py"
            --root "${DE_ROOT_DIR}"
            --table "${CMAKE_CURRENT_SOURCE_DIR}/layers.toml"
            --graph "${CMAKE_CURRENT_BINARY_DIR}/layers.dot")

add_test(NAME layering_selftest
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_selftest.py")

set_tests_properties(layering_check layering_selftest PROPERTIES
    LABELS check
    TIMEOUT 60)
