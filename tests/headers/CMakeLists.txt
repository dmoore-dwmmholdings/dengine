# tests/headers: public header compile check (SPEC §5.1, §19.3, §26). See cmake/DeHeaderCheck.cmake.
#
#   de_header_check  OBJECT library: every include/dengine/*.h compiled standalone as C11 and as
#                    C++17, plus examples/minimal_game.c as a C11 object. Part of the default
#                    build, so a broken header fails `cmake --build` on every preset, including
#                    the windows-xwin cross build (clang-cl), which has no test presets.
#   header_check     ctest (label `check`) that builds de_header_check. DENG-66's CI runs it by
#                    this name and label; both are a contract.
#
# New headers are picked up automatically (configure-time glob). Do not list headers here.
include(DeHeaderCheck)

de_add_header_check(TARGET de_header_check
    EXTRA_C_SOURCES "${DE_ROOT_DIR}/examples/minimal_game.c")

add_test(NAME header_check
    COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target de_header_check
            --config "$<CONFIG>")
set_tests_properties(header_check PROPERTIES
    LABELS check
    TIMEOUT 300)
