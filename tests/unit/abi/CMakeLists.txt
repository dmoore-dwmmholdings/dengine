# tests/unit/abi: the ABI checker (tools/abi, DENG-128) on tiny fixture headers, one per rule, and
# the dump on the real public headers. Plain Python (stdlib only, 3.10+); each test compiles and
# runs the layout probe with host clang. ctest labels `unit` and `check`.
if(NOT DE_BUILD_TESTS)
    return()
endif()
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
find_program(DE_ABI_CLANG NAMES clang clang-20 clang-19 clang-18 DOC "clang for the ABI dump")
if(NOT DE_ABI_CLANG OR CMAKE_CROSSCOMPILING)
    message(STATUS "tests/unit/abi: no host clang or cross build; ABI tool tests skipped")
    return()
endif()

set(_de_abi_tests
    test_abi_removed_function
    test_abi_param_type_change
    test_abi_struct_reorder
    test_abi_enum_value_change
    test_abi_handle_change
    test_abi_identical_ok
    test_abi_added_function_ok
    test_abi_appended_enum_ok
    test_abi_major_bump_allows
    test_abi_dump_fixture_contents
    test_abi_dump_real_headers)
foreach(_t IN LISTS _de_abi_tests)
    add_test(NAME abi.${_t}
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_abi.py"
                --clang "${DE_ABI_CLANG}" TestAbi.${_t})
    set_tests_properties(abi.${_t} PROPERTIES LABELS "unit;check" TIMEOUT 120)
endforeach()
